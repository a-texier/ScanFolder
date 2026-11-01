#include "store.hpp"
#include <compressapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#pragma comment(lib,"cabinet.lib")
namespace sf {
namespace {
std::string utf8(const std::wstring& s){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);if(!n&&!s.empty())throw std::runtime_error("unicode");std::string out(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;}
std::wstring wide(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);if(!n&&!s.empty())throw std::runtime_error("unicode");std::wstring out(n,0);if(n)MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),out.data(),n);return out;}
uint32_t crc(const uint8_t* p,size_t size){static const auto table=[] {std::array<uint32_t,256> t{};for(unsigned i=0;i<256;++i){uint32_t c=i;for(int b=0;b<8;++b)c=(c>>1)^(0xedb88320u&uint32_t(-int(c&1)));t[i]=c;}return t;}();uint32_t c=UINT32_MAX;for(size_t i=0;i<size;++i)c=table[(c^p[i])&255]^(c>>8);return ~c;}
template<class T> void put(std::ostream& out,T v){out.write(reinterpret_cast<const char*>(&v),sizeof v);if(!out)throw std::runtime_error("write");}
template<class T> T get(std::istream& in){T v{};in.read(reinterpret_cast<char*>(&v),sizeof v);if(!in)throw std::runtime_error("read");return v;}
struct Writer {
    std::ofstream out;COMPRESSOR_HANDLE compressor=nullptr;std::vector<uint8_t> block;
    explicit Writer(const std::wstring& path):out(std::filesystem::path(path),std::ios::binary){if(!out||!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&compressor))throw std::runtime_error("compressor");block.reserve(1024*1024);}
    ~Writer(){if(compressor)CloseCompressor(compressor);}
    void flush(){if(block.empty())return;SIZE_T needed=0;Compress(compressor,block.data(),block.size(),nullptr,0,&needed);if(!needed)throw std::runtime_error("compression");std::vector<uint8_t> zipped(needed);if(!Compress(compressor,block.data(),block.size(),zipped.data(),zipped.size(),&needed))throw std::runtime_error("compression");put(out,uint32_t(block.size()));put(out,uint32_t(needed));put(out,crc(block.data(),block.size()));put(out,crc(zipped.data(),needed));out.write(reinterpret_cast<const char*>(zipped.data()),needed);if(!out)throw std::runtime_error("write");block.clear();}
    void byte(uint8_t b){block.push_back(b);if(block.size()==1024*1024)flush();}
    void number(uint64_t v){do{uint8_t b=uint8_t(v&127);v>>=7;byte(b|(v?128:0));}while(v);}
    void string(const std::wstring& s){auto b=utf8(s);number(b.size());for(unsigned char c:b)byte(c);}
};
struct Reader {
    std::ifstream in;DECOMPRESSOR_HANDLE decompressor=nullptr;std::vector<uint8_t> block;size_t pos=0;uint64_t total=0;
    explicit Reader(const std::wstring& path):in(std::filesystem::path(path),std::ios::binary){if(!in||!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&decompressor))throw std::runtime_error("decompressor");}
    ~Reader(){if(decompressor)CloseDecompressor(decompressor);}
    uint8_t byte(){if(pos==block.size()){uint32_t size=get<uint32_t>(in),compressed=get<uint32_t>(in),checksum=get<uint32_t>(in),compressedCrc=get<uint32_t>(in);if(!size||size>1024*1024||!compressed||compressed>2*1024*1024||(total+=size)>uint64_t(2)*1024*1024*1024)throw std::runtime_error("limits");std::vector<uint8_t> input(compressed);in.read(reinterpret_cast<char*>(input.data()),compressed);if(!in||crc(input.data(),input.size())!=compressedCrc)throw std::runtime_error("read");block.resize(size);SIZE_T actual=0;if(!Decompress(decompressor,input.data(),input.size(),block.data(),block.size(),&actual)||actual!=size||crc(block.data(),size)!=checksum)throw std::runtime_error("corrupt");pos=0;}return block[pos++];}
    uint64_t number(){uint64_t n=0;for(unsigned shift=0;shift<64;shift+=7){auto b=byte();if(shift==63&&(b&0xfe))throw std::runtime_error("varint");n|=uint64_t(b&127)<<shift;if(!(b&128))return n;}throw std::runtime_error("varint");}
    uint32_t index(){auto n=number();if(n>UINT32_MAX)throw std::runtime_error("index");return uint32_t(n);}
    std::wstring string(){auto n=number();if(n>131072)throw std::runtime_error("string");std::string s(size_t(n),'\0');for(char& c:s)c=char(byte());return wide(s);}
};
void validate(const Result& r){if(r.nodes.empty()||r.nodes[0].parent!=none||!r.nodes[0].directory)throw std::runtime_error("root");std::vector<uint8_t> visited(r.nodes.size(),0);std::vector<uint32_t> queue{0};visited[0]=1;
    for(size_t p=0;p<queue.size();++p){auto i=queue[p];if(!r.nodes[i].directory&&r.nodes[i].firstChild!=none)throw std::runtime_error("file children");for(auto c=r.nodes[i].firstChild;c!=none;c=r.nodes[c].nextSibling){if(c>=r.nodes.size()||visited[c]||r.nodes[c].parent!=i)throw std::runtime_error("graph");visited[c]=1;queue.push_back(c);}}
    if(queue.size()!=r.nodes.size())throw std::runtime_error("orphan");}
}
std::wstring databasePath(){wchar_t custom[32768]{};if(GetEnvironmentVariableW(L"SCANFOLDER_CACHE_DIR",custom,32768))return normalize(custom)+L"\\cache.db";PWSTR local=nullptr;std::wstring base;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){base=local;CoTaskMemFree(local);}else{wchar_t temp[32768]{};GetTempPathW(32768,temp);base=temp;}return base+L"\\ScanFolder\\cache.db";}
namespace {
std::wstring cacheKey(const std::wstring& input){
    auto path=normalize(input);wchar_t mount[32768]{},volume[128]{};std::wstring identity=path;
    // A volume GUID prevents another USB disk using the same letter from inheriting its cache.
    if(GetVolumePathNameW(path.c_str(),mount,32768)&&GetVolumeNameForVolumeMountPointW(mount,volume,128))identity=std::wstring(volume)+path.substr(wcslen(mount));
    if(!identity.empty())CharUpperBuffW(identity.data(),DWORD(identity.size()));
    uint64_t hash=14695981039346656037ull;for(wchar_t c:identity){hash^=uint8_t(c);hash*=1099511628211ull;hash^=uint8_t(c>>8);hash*=1099511628211ull;}
    wchar_t key[32]{};swprintf_s(key,L"index-%016llx",hash);return key;
}
std::filesystem::path cacheDirectory(){return std::filesystem::path(databasePath()).parent_path();}
std::wstring sessionPath(const std::wstring& root){return (cacheDirectory()/(root.empty()?L"session.dat":cacheKey(root)+L".session")).wstring();}
bool ownedCacheFile(const std::wstring& name){if(name==L"cache.db"||name==L"session.dat")return true;if(name.rfind(L"index-",0)!=0)return false;auto end=name.find(L'.');if(end!=22)return false;for(size_t i=6;i<end;++i)if(!iswxdigit(name[i]))return false;return name.substr(end)==L".db"||name.substr(end)==L".session";}
template<class F> void eachCacheFile(F action){auto pattern=(cacheDirectory()/L"*").wstring();WIN32_FIND_DATAW d{};HANDLE h=FindFirstFileW(pattern.c_str(),&d);if(h==INVALID_HANDLE_VALUE)return;do{if(!(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&ownedCacheFile(d.cFileName))action((cacheDirectory()/d.cFileName).wstring(),d);}while(FindNextFileW(h,&d));FindClose(h);}
bool writeSession(const SessionState& s,const std::wstring& path){try{std::filesystem::create_directories(cacheDirectory());auto temp=path+L".tmp-"+std::to_wstring(GetCurrentProcessId());Writer w(temp);put(w.out,uint32_t(0x53534653));w.string(s.root);w.string(s.directory);w.string(s.query);w.string(s.extension);w.number(s.scroll);w.number(s.tab);w.number(s.sort);w.byte(s.descending);w.byte(s.allocated);w.flush();w.out.close();return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE;}catch(...){return false;}}
}
std::wstring databasePath(const std::wstring& root){return (cacheDirectory()/(cacheKey(root)+L".db")).wstring();}
uint64_t cacheBytes(){uint64_t bytes=0;eachCacheFile([&](const std::wstring&,const WIN32_FIND_DATAW& d){bytes+=(uint64_t(d.nFileSizeHigh)<<32)|d.nFileSizeLow;});return bytes;}
std::shared_ptr<Result> loadCachedIndex(const std::wstring& target,std::wstring& error){
    if(target.empty())return loadDatabase(databasePath(),error);
    auto path=std::filesystem::path(normalize(target));
    for(;;){auto candidate=path.wstring();auto r=loadDatabase(databasePath(candidate),error);if(r){r->root=candidate;return r;}auto parent=path.parent_path();if(parent.empty()||parent==path)break;path=std::move(parent);}
    // Read the previous single-index format once as a fallback; never scan to populate it.
    auto legacy=loadDatabase(databasePath(),error);if(legacy){auto root=normalize(legacy->root),desired=normalize(target);if(!_wcsicmp(root.c_str(),desired.c_str())||(desired.size()>root.size()&&!_wcsnicmp(root.c_str(),desired.c_str(),root.size())&&(root.back()==L'\\'||desired[root.size()]==L'\\')))return legacy;}
    return {};
}
bool saveDatabase(const Result& result,const std::wstring& path,std::wstring& error){auto temp=path+L".tmp-"+std::to_wstring(GetCurrentProcessId());try{
    if(result.nodes.empty()||result.nodes[0].removed)return false;
    std::vector<uint32_t> order{0},map(result.nodes.size(),none);map[0]=0;
    for(size_t p=0;p<order.size();++p)for(auto c=result.nodes[order[p]].firstChild;c!=none;c=result.nodes[c].nextSibling)if(!result.nodes[c].removed){map[c]=uint32_t(order.size());order.push_back(c);}
    Writer w(temp);uint32_t header[]={0x42444653,2,uint32_t(order.size())};for(auto value:header)put(w.out,value);put(w.out,crc(reinterpret_cast<const uint8_t*>(header),sizeof header));w.string(result.root);w.number(result.errors);w.number(result.skippedLinks);
    for(auto id:order){auto& n=result.nodes[id];w.number(n.parent==none?none:map[n.parent]);uint32_t first=n.firstChild;while(first!=none&&result.nodes[first].removed)first=result.nodes[first].nextSibling;uint32_t next=n.nextSibling;while(next!=none&&result.nodes[next].removed)next=result.nodes[next].nextSibling;
        w.number(first==none?none:map[first]);w.number(next==none?none:map[next]);w.number(n.size);w.number(n.allocated);w.number(n.modified);w.byte(uint8_t(n.directory)|(uint8_t(n.reparse)<<1));w.string(n.name);}
    w.flush();w.out.flush();if(!w.out)throw std::runtime_error("flush");w.out.close();
    if(!MoveFileExW(extended(temp).c_str(),extended(path).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("publish");return true;
}catch(const std::exception&){DeleteFileW(extended(temp).c_str());error=L"Le cache disque n’a pas pu être enregistré. L’index reste disponible en mémoire.";return false;}}
std::shared_ptr<Result> loadDatabase(const std::wstring& path,std::wstring& error){if(GetFileAttributesW(extended(path).c_str())==INVALID_FILE_ATTRIBUTES)return {};try{
    auto start=std::chrono::steady_clock::now();Reader r(path);uint32_t header[]={get<uint32_t>(r.in),get<uint32_t>(r.in),get<uint32_t>(r.in)};auto headerCrc=get<uint32_t>(r.in);if(header[0]!=0x42444653||header[1]!=2||headerCrc!=crc(reinterpret_cast<const uint8_t*>(header),sizeof header))throw std::runtime_error("version");auto count=header[2];if(!count||count>16000000)throw std::runtime_error("count");
    auto result=std::make_shared<Result>();result->root=r.string();result->errors=r.number();result->skippedLinks=r.number();result->nodes.resize(count);
    for(size_t i=0;i<count;++i){auto& n=result->nodes[i];n.parent=r.index();n.firstChild=r.index();n.nextSibling=r.index();n.size=r.number();n.allocated=r.number();n.modified=r.number();auto flags=r.byte();if(flags>3)throw std::runtime_error("flags");n.directory=flags&1;n.reparse=flags&2;n.name=r.string();if(n.parent!=none&&n.parent>=count)throw std::runtime_error("parent");if(i&&(n.name.empty()||n.name==L"."||n.name==L".."||n.name.find_first_of(L"\\/:")!=std::wstring::npos||n.name.find(L'\0')!=std::wstring::npos))throw std::runtime_error("name");}
    if(r.pos!=r.block.size()||r.in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("trailing data");validate(*result);result->aggregate();result->method=L"Cache disque · aucun parcours des fichiers";result->note=L"Index mémorisé du dernier scan. F5 pour actualiser les fichiers qui ont changé hors de l’application.";result->seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();return result;
}catch(const std::exception&){error=L"Cache absent, incomplet ou incompatible : lancez une nouvelle analyse.";return {};}}
bool saveSession(const SessionState& s){if(s.root.empty())return false;bool saved=writeSession(s,sessionPath(s.root));return writeSession(s,sessionPath(L""))&&saved;}
std::optional<SessionState> loadSession(const std::wstring& root){try{Reader r(sessionPath(root));if(get<uint32_t>(r.in)!=0x53534653)return {};SessionState s;s.root=r.string();s.directory=r.string();s.query=r.string();s.extension=r.string();s.scroll=r.number();s.tab=int(r.number());s.sort=int(r.number());s.descending=r.byte()!=0;s.allocated=r.byte()!=0;if(s.tab<0||s.tab>2||s.sort<0||s.sort>3)return {};
    if(!root.empty()&&_wcsicmp(root.c_str(),s.root.c_str())){if(s.directory.size()>=s.root.size()&&!_wcsnicmp(s.directory.c_str(),s.root.c_str(),s.root.size()))s.directory=root+s.directory.substr(s.root.size());else s.directory=root;s.root=root;}return s;}catch(...){return {};}}
DiskWriter::~DiskWriter(){shutdown();}
void DiskWriter::shutdown(){{std::lock_guard lock(mutex);stopping=true;}cv.notify_all();if(thread.joinable())thread.join();}
void DiskWriter::save(std::shared_ptr<const Result> r,std::function<void()> callback){auto path=databasePath(r->root);{std::lock_guard lock(mutex);Work work{std::move(r),false,path,std::move(callback)};if(!queue.empty()&&!queue.back().clear&&queue.back().path==path)queue.back()=std::move(work);else queue.push_back(std::move(work));if(!thread.joinable())thread=std::thread([this]{loop();});}cv.notify_all();}
void DiskWriter::clear(std::function<void()> callback){{std::lock_guard lock(mutex);queue.clear();queue.push_back({{},true,L"",std::move(callback)});if(!thread.joinable())thread=std::thread([this]{loop();});}cv.notify_all();}
std::wstring DiskWriter::lastError(){std::lock_guard lock(mutex);return error;}
void DiskWriter::waitIdle(){std::unique_lock lock(mutex);cv.wait(lock,[&]{return queue.empty()&&!writing;});}
void DiskWriter::loop(){for(;;){Work work;{std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping||!queue.empty();});if(queue.empty()&&stopping)return;work=std::move(queue.front());queue.pop_front();writing=true;}
    std::wstring message;try{std::filesystem::create_directories(cacheDirectory());if(work.clear){eachCacheFile([&](const std::wstring& path,const WIN32_FIND_DATAW&){if(!DeleteFileW(extended(path).c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)message=L"Le cache disque n’a pas pu être entièrement vidé.";});}
        else if(work.result)saveDatabase(*work.result,work.path,message);
        bytes=cacheBytes();
    }catch(...){message=L"Erreur d’accès au cache disque.";}
    {std::lock_guard lock(mutex);error=std::move(message);}if(work.notify)work.notify();{std::lock_guard lock(mutex);writing=false;}cv.notify_all();}}
}
