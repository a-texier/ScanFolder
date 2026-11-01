#include "cache.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
namespace sf {
namespace {
std::wstring lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size())); return s; }
uint64_t u64(DWORD hi,DWORD lo) { return (uint64_t(hi)<<32)|lo; }
void removeTree(Result& result,uint32_t id) {
    std::vector<uint32_t> stack{id};
    while(!stack.empty()){auto i=stack.back();stack.pop_back();auto& n=result.nodes[i];n.removed=true;
        for(auto c=n.firstChild;c!=none;c=result.nodes[c].nextSibling)stack.push_back(c);}
}
void adopt(Result& dest,uint32_t id,const Result& fresh) {
    auto parent=dest.nodes[id].parent,next=dest.nodes[id].nextSibling;auto name=dest.nodes[id].name;
    for(auto c=dest.nodes[id].firstChild;c!=none;c=dest.nodes[c].nextSibling)removeTree(dest,c);
    std::vector<uint32_t> map(fresh.nodes.size(),none);map[0]=id;
    for(size_t i=1;i<fresh.nodes.size();++i)map[i]=uint32_t(dest.nodes.size()+i-1);
    Node base=fresh.nodes[0];base.name=name;base.parent=parent;base.nextSibling=next;base.firstChild=base.firstChild==none?none:map[base.firstChild];dest.nodes[id]=std::move(base);
    for(size_t i=1;i<fresh.nodes.size();++i){Node n=fresh.nodes[i];n.parent=n.parent==none?none:map[n.parent];n.firstChild=n.firstChild==none?none:map[n.firstChild];n.nextSibling=n.nextSibling==none?none:map[n.nextSibling];dest.nodes.push_back(std::move(n));}
}
uint64_t clusterFor(const std::wstring& path){wchar_t volume[MAX_PATH]{};DWORD spc=0,bps=0,a=0,b=0;if(GetVolumePathNameW(path.c_str(),volume,MAX_PATH)&&GetDiskFreeSpaceW(volume,&spc,&bps,&a,&b))return uint64_t(spc)*bps;return 4096;}
Node entry(const WIN32_FIND_DATAW& data,uint32_t parent,uint64_t cluster,const std::wstring& path){
    Node n;n.name=data.cFileName;n.parent=parent;n.directory=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;n.reparse=(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
    n.modified=u64(data.ftLastWriteTime.dwHighDateTime,data.ftLastWriteTime.dwLowDateTime);
    if(!n.directory){n.size=u64(data.nFileSizeHigh,data.nFileSizeLow);n.allocated=(n.size+cluster-1)/cluster*cluster;
        if(data.dwFileAttributes&(FILE_ATTRIBUTE_COMPRESSED|FILE_ATTRIBUTE_SPARSE_FILE|FILE_ATTRIBUTE_REPARSE_POINT)){DWORD hi=0;SetLastError(0);DWORD lo=GetCompressedFileSizeW(extended(path).c_str(),&hi);if(lo!=INVALID_FILE_SIZE||!GetLastError())n.allocated=u64(hi,lo);}}
    return n;
}
void deepRefresh(Result& result,uint32_t id,Progress& progress,unsigned workers){
    if(progress.cancel)return;auto path=result.path(id);DWORD attributes=GetFileAttributesW(extended(path).c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)removeTree(result,id);else ++progress.errors;return;}
    if((attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){
        auto fresh=scan(path,progress,Options{false,workers});if(!fresh->nodes.empty())adopt(result,id,*fresh);else if(fresh->errors)++progress.errors;
    }else{
        WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileExW(extended(path).c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,0);
        if(find==INVALID_HANDLE_VALUE){++progress.errors;return;}FindClose(find);
        auto n=entry(data,result.nodes[id].parent,clusterFor(path),path);n.nextSibling=result.nodes[id].nextSibling;
        for(auto c=result.nodes[id].firstChild;c!=none;c=result.nodes[c].nextSibling)removeTree(result,c);result.nodes[id]=std::move(n);
    }
}
void refreshParent(Result& result,uint32_t id,Progress& progress,unsigned workers){
    if(progress.cancel||result.nodes[id].removed||result.nodes[id].reparse)return;
    auto path=result.path(id);progress.status(L"Mise à jour du dossier concerné",path);
    std::unordered_map<std::wstring,uint32_t> existing;for(auto c=result.nodes[id].firstChild;c!=none;c=result.nodes[c].nextSibling)if(!result.nodes[c].removed)existing[lower(result.nodes[c].name)]=c;
    std::unordered_set<uint32_t> seen;uint64_t cluster=clusterFor(path);
    auto pattern=extended(path)+(path.back()==L'\\'?L"*":L"\\*");WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND){for(auto [name,c]:existing)removeTree(result,c);}else ++progress.errors;return;}
    std::vector<uint32_t> added;
    do{
        if(progress.cancel)break;if(!wcscmp(data.cFileName,L".")||!wcscmp(data.cFileName,L".."))continue;
        auto full=path+(path.back()==L'\\'?L"":L"\\")+data.cFileName;
        auto n=entry(data,id,cluster,full);auto it=existing.find(lower(n.name));
        if(it!=existing.end()){
            auto c=it->second;seen.insert(c);
            if(n.directory==result.nodes[c].directory&&n.reparse==result.nodes[c].reparse){n.firstChild=result.nodes[c].firstChild;n.nextSibling=result.nodes[c].nextSibling;result.nodes[c]=std::move(n);}
            else{n.nextSibling=result.nodes[c].nextSibling;for(auto child=result.nodes[c].firstChild;child!=none;child=result.nodes[child].nextSibling)removeTree(result,child);result.nodes[c]=std::move(n);if(result.nodes[c].directory&&!result.nodes[c].reparse)added.push_back(c);}
        }else{auto c=uint32_t(result.nodes.size());n.nextSibling=result.nodes[id].firstChild;result.nodes[id].firstChild=c;result.nodes.push_back(std::move(n));if(result.nodes[c].directory&&!result.nodes[c].reparse)added.push_back(c);}
    }while(FindNextFileW(find,&data));DWORD error=GetLastError();FindClose(find);
    if(!progress.cancel&&error==ERROR_NO_MORE_FILES){for(auto& [name,c]:existing)if(!seen.contains(c))removeTree(result,c);}else if(!progress.cancel)++progress.errors;
    for(auto c:added)deepRefresh(result,c,progress,workers);
}
}
uint32_t findCached(const Result& result,const std::wstring& input){
    if(result.nodes.empty())return none;auto root=normalize(result.root),path=normalize(input);
    if(path.size()<root.size()||_wcsnicmp(root.c_str(),path.c_str(),root.size())!=0)return none;
    if(path.size()==root.size())return result.nodes[0].removed?none:0;
    size_t pos=root.size();if(root.back()!=L'\\'){if(path[pos]!=L'\\')return none;++pos;}
    uint32_t current=0;
    while(pos<path.size()){auto end=path.find(L'\\',pos);if(end==std::wstring::npos)end=path.size();auto name=path.substr(pos,end-pos);uint32_t found=none;
        for(auto c=result.nodes[current].firstChild;c!=none;c=result.nodes[c].nextSibling)if(!result.nodes[c].removed&&_wcsicmp(result.nodes[c].name.c_str(),name.c_str())==0){found=c;break;}
        if(found==none)return none;current=found;pos=end+1;}
    return current;
}
std::shared_ptr<Result> refreshCache(const std::shared_ptr<const Result>& source,const CacheChanges& changes,Progress& progress,unsigned workers){
    if(!source||source->nodes.empty())return {};auto begin=std::chrono::steady_clock::now();progress.status(L"Actualisation locale du cache");
    auto result=std::make_shared<Result>(*source);
    std::unordered_set<std::wstring> changed,parents;
    for(auto& path:changes.changed)if(changed.insert(lower(normalize(path))).second){auto id=findCached(*result,path);if(id!=none)deepRefresh(*result,id,progress,workers);}
    for(auto& path:changes.parents)if(parents.insert(lower(normalize(path))).second){auto id=findCached(*result,path);if(id!=none)refreshParent(*result,id,progress,workers);}
    result->aggregate();result->method=L"Cache · actualisation locale";result->note=L"Seuls les éléments modifiés ont été actualisés. Allocation estimée pour les entrées actualisées. F5 pour rescanner.";
    result->cancelled=progress.cancel;if(result->cancelled)result->note=L"Actualisation du cache interrompue : F5 pour mettre à jour les résultats.";result->errors+=progress.errors;result->seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();return result;
}
}
