#include "archive.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <objbase.h>

namespace sf {
namespace {
std::wstring quote(const std::wstring& s) {
    std::wstring out=L"\"";unsigned slashes=0;
    for(wchar_t c:s) {if(c==L'\\'){++slashes;continue;}if(c==L'"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
    out.append(slashes*2,L'\\');out+=L'"';return out;
}
bool within(const std::wstring& path,const std::wstring& root){return path.size()>=root.size()&&_wcsnicmp(path.c_str(),root.c_str(),root.size())==0&&(path.size()==root.size()||root.back()==L'\\'||path[root.size()]==L'\\');}
std::wstring resolved(const std::wstring& path) {
    HANDLE h=CreateFileW(extended(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(h==INVALID_HANDLE_VALUE)return {};DWORD n=GetFinalPathNameByHandleW(h,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);std::wstring s(n+1,0);
    if(n)n=GetFinalPathNameByHandleW(h,s.data(),DWORD(s.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);CloseHandle(h);s.resize(n);return s;
}
std::string utf8(const std::wstring& s){int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string b(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),b.data(),n,nullptr,nullptr);return b;}
std::wstring wide(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),w.data(),n);return w;}
struct Temp {
    std::wstring dir,engine,list;
    ~Temp(){if(!engine.empty())DeleteFileW(engine.c_str());if(!list.empty())DeleteFileW(list.c_str());if(!dir.empty())RemoveDirectoryW(dir.c_str());}
};
bool saveResource(WORD id,const std::wstring& path){auto module=GetModuleHandleW(nullptr);auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);if(!resource)return false;auto data=LoadResource(module,resource);auto size=SizeofResource(module,resource);auto ptr=LockResource(data);if(!ptr)return false;
    HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD written=0;bool ok=WriteFile(h,ptr,size,&written,nullptr)&&written==size;CloseHandle(h);return ok;}
bool run7zip(const std::wstring& engine,const std::wstring& args,const std::wstring& cwd,DeleteProgress& p,std::string& log,DWORD& exitCode){
    SECURITY_ATTRIBUTES security{sizeof security,nullptr,TRUE};HANDLE reader=nullptr,writer=nullptr;
    if(!CreatePipe(&reader,&writer,&security,0))return false;SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)){if(job)CloseHandle(job);CloseHandle(reader);CloseHandle(writer);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);return false;}
    STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=writer;startup.hStdError=writer;startup.hStdInput=input;
    PROCESS_INFORMATION process{};auto cmd=quote(engine)+L" "+args;std::vector<wchar_t> command(cmd.begin(),cmd.end());command.push_back(0);
    bool ok=CreateProcessW(engine.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,cwd.c_str(),&startup,&process)!=FALSE;
    CloseHandle(writer);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    if(!ok){CloseHandle(reader);CloseHandle(job);return false;}
    if(!AssignProcessToJobObject(job,process.hProcess)){TerminateProcess(process.hProcess,2);CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(reader);CloseHandle(job);return false;}
    ResumeThread(process.hThread);CloseHandle(process.hThread);
    for(;;){
        DWORD available=0;
        while(PeekNamedPipe(reader,nullptr,0,nullptr,&available,nullptr)&&available){char buffer[4096];DWORD got=0;if(!ReadFile(reader,buffer,std::min<DWORD>(sizeof buffer,available),&got,nullptr)||!got)break;log.append(buffer,got);if(log.size()>16384)log.erase(0,log.size()-16384);
            auto percent=log.find_last_of('%');if(percent!=std::string::npos){size_t first=percent;while(first&&log[first-1]>='0'&&log[first-1]<='9')--first;if(first<percent){unsigned value=0;for(size_t i=first;i<percent;++i)value=value*10+unsigned(log[i]-'0');p.percent=std::min(100u,value);}} }
        if(p.cancel){TerminateJobObject(job,255);WaitForSingleObject(process.hProcess,5000);break;}
        if(WaitForSingleObject(process.hProcess,80)==WAIT_OBJECT_0){
            DWORD available2=0;while(PeekNamedPipe(reader,nullptr,0,nullptr,&available2,nullptr)&&available2){char b[4096];DWORD got=0;if(!ReadFile(reader,b,std::min<DWORD>(sizeof b,available2),&got,nullptr)||!got)break;log.append(b,got);}break;
        }
    }
    GetExitCodeProcess(process.hProcess,&exitCode);CloseHandle(process.hProcess);CloseHandle(reader);CloseHandle(job);return true;
}
}
ArchiveResult createZip(const std::wstring& input,const std::wstring& destination,bool replace,DeleteProgress& p,unsigned workers,unsigned level){
    ArchiveResult result;auto start=std::chrono::steady_clock::now();auto source=normalize(input),output=normalize(destination);result.output=output;
    DWORD attributes=GetFileAttributesW(extended(source).c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){result.message=L"Source inaccessible ou lien : choisissez un dossier ou fichier réel.";return result;}
    auto srcPath=std::filesystem::path(source);auto parent=srcPath.parent_path().wstring(),base=srcPath.filename().wstring();
    if(base.empty()){result.message=L"Sélectionnez un dossier, pas une racine de disque.";return result;}
    auto actualSource=resolved(source),actualDestParent=resolved(std::filesystem::path(output).parent_path().wstring());
    if(actualSource.empty()||actualDestParent.empty()){result.message=L"Les chemins réels ne peuvent pas être vérifiés.";return result;}
    auto actualOutput=actualDestParent+L"\\"+std::filesystem::path(output).filename().wstring();
    if(within(actualOutput,actualSource)||_wcsicmp(source.c_str(),output.c_str())==0){result.message=L"Enregistrez le ZIP en dehors du dossier source.";return result;}
    if(!replace&&GetFileAttributesW(extended(output).c_str())!=INVALID_FILE_ATTRIBUTES){result.message=L"L’archive existe déjà.";return result;}
    GUID guid{};CoCreateGuid(&guid);wchar_t id[40]{};StringFromGUID2(guid,id,40);wchar_t tempPath[32768]{};GetTempPathW(32768,tempPath);
    Temp temp;temp.dir=std::wstring(tempPath)+L"ScanFolderZip-"+id;
    if(!CreateDirectoryW(temp.dir.c_str(),nullptr)){result.message=errorText(GetLastError());return result;}
    temp.engine=temp.dir+L"\\7za.exe";temp.list=temp.dir+L"\\files.txt";
    if(!saveResource(101,temp.engine)){result.message=L"Le moteur 7-Zip embarqué ne peut pas être préparé.";return result;}
    auto engine=temp.engine;wchar_t replacement[32768]{};if(GetEnvironmentVariableW(L"SCANFOLDER_7ZA",replacement,32768)&&GetFileAttributesW(replacement)!=INVALID_FILE_ATTRIBUTES)engine=replacement;
    std::ofstream list(std::filesystem::path(temp.list),std::ios::binary);if(!list){result.message=L"Impossible de préparer la liste des fichiers.";return result;}
    uint64_t listed=0;auto add=[&](const std::wstring& relative){list<<utf8(quote(relative))<<"\n";++listed;};
    {std::lock_guard lock(p.mutex);p.current=L"Préparation de la liste · liens et fichiers cloud exclus";}
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)){add(base);++result.files;}
    else{
        std::vector<std::wstring> stack{L""};
        while(!stack.empty()&&!p.cancel){auto relative=std::move(stack.back());stack.pop_back();auto path=source+(relative.empty()?L"":L"\\"+relative);
            // Guard aliases and reparse changes before every enumeration.
            auto attr=GetFileAttributesW(extended(path).c_str());auto final=resolved(path);
            if(attr==INVALID_FILE_ATTRIBUTES||final.empty()||!within(final,actualSource)){result.message=L"Un dossier est devenu inaccessible pendant la préparation.";return result;}
            if(attr&FILE_ATTRIBUTE_REPARSE_POINT){++result.skipped;continue;}
            WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileExW((extended(path)+L"\\*").c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
            if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND){add(base+(relative.empty()?L"":L"\\"+relative)+L"\\");continue;}result.message=errorText(GetLastError());return result;}
            bool any=false;do{
                if(p.cancel)break;if(!wcscmp(data.cFileName,L".")||!wcscmp(data.cFileName,L".."))continue;
                any=true;auto rel=relative.empty()?std::wstring(data.cFileName):relative+L"\\"+data.cFileName;
                if(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){++result.skipped;continue;}
                if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)stack.push_back(std::move(rel));else{add(base+L"\\"+rel);++result.files;p.files=result.files;}
            }while(FindNextFileW(find,&data));DWORD error=GetLastError();FindClose(find);
            if(!p.cancel&&error!=ERROR_NO_MORE_FILES){result.message=errorText(error);return result;}
            if(!any)add(base+(relative.empty()?L"":L"\\"+relative)+L"\\");
        }
    }
    list.close();if(!list.good()){result.message=L"La liste de fichiers ne peut pas être écrite.";return result;}
    if(p.cancel){result.cancelled=true;result.message=L"Compression annulée.";return result;}
    if(!listed){result.message=L"Aucun fichier ordinaire à archiver. Les liens et éléments cloud ne sont pas suivis.";return result;}
    if(!result.files&&!(attributes&FILE_ATTRIBUTE_DIRECTORY)){result.message=L"Aucun fichier à compresser.";return result;}
    auto temporary=output+L".ScanFolder-"+id+L".partial.zip";
    auto args=L"a -tzip -mx="+std::to_wstring(std::min(level,9u))+L" -mmt="+std::to_wstring(std::clamp(workers,1u,8u))+L" -scsUTF-8 -sccUTF-8 -bb0 -bsp1 -y "+quote(temporary)+L" "+quote(L"@"+temp.list);
    {std::lock_guard lock(p.mutex);p.current=L"Compression ZIP · 7-Zip 26.04";}
    std::string log;DWORD code=2;bool ran=run7zip(engine,args,parent,p,log,code);
    if(!ran||code!=0||p.cancel){DeleteFileW(extended(temporary).c_str());result.cancelled=p.cancel;result.message=p.cancel?L"Compression interrompue. Le dossier source est intact.":L"7-Zip n’a pas produit une archive complète (code "+std::to_wstring(code)+L").\n"+wide(log);return result;}
    // Verify CRCs before publishing the archive. The destination is untouched until verification succeeds.
    p.percent=0;{std::lock_guard lock(p.mutex);p.current=L"Vérification de l’intégrité du ZIP";}
    log.clear();ran=run7zip(engine,L"t -bsp1 -bb0 -sccUTF-8 "+quote(temporary),parent,p,log,code);
    if(!ran||code!=0||p.cancel){DeleteFileW(extended(temporary).c_str());result.cancelled=p.cancel;result.message=L"Le ZIP n’a pas été publié : vérification interrompue ou en échec.\n"+wide(log);return result;}
    DWORD flags=MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0);
    if(!MoveFileExW(extended(temporary).c_str(),extended(output).c_str(),flags)){result.message=L"Archive vérifiée mais publication impossible : "+errorText(GetLastError())+L"\nArchive conservée : "+temporary;return result;}
    result.success=true;p.percent=100;result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    result.message=L"ZIP créé et vérifié (CRC).";if(result.skipped)result.message+=L"\nAttention : "+countText(result.skipped)+L" liens ou éléments cloud ont été exclus.";
    return result;
}
bool archivePolicyTest(){return quote(L"a b\\")==L"\"a b\\\\\""&&within(L"C:\\source\\a",L"C:\\source")&&!within(L"C:\\source-other\\a",L"C:\\source");}
}
