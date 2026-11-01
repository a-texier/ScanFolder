#include "scan.hpp"
#include "delete.hpp"
#include "view.hpp"
#include "operations.hpp"
#include "archive.hpp"
#include "interaction.hpp"
#include "cache.hpp"
#include "store.hpp"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <windowsx.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib,"windowscodecs.lib")

using namespace sf;
namespace {
constexpr UINT scanDone = WM_APP + 1, deleteDone = WM_APP + 2, viewDone = WM_APP + 3, transferDone = WM_APP + 4, zipDone = WM_APP + 5, cacheLoaded = WM_APP + 6, cacheWritten = WM_APP + 7;
template<class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
struct Rect { float x,y,w,h; bool contains(float a, float b) const { return a >= x && b >= y && a < x+w && b < y+h; } D2D1_RECT_F d2d() const { return {x,y,x+w,y+h}; } };
D2D1_COLOR_F color(unsigned c, float alpha = 1) { return D2D1::ColorF(c, alpha); }
struct Palette { unsigned bg, panel, elevated, field, line, text, muted, accent, accentDim, danger; };
Palette dark{0x0d111b,0x141b29,0x1a2536,0x101725,0x29354a,0xf0f4fc,0x94a6bf,0x63dbc1,0x173c3a,0xff8c97};
Palette light{0xf3f6fa,0xffffff,0xeaf0f7,0xf3f6fa,0xdce3ed,0x17253b,0x65748a,0x067963,0xd9f3eb,0xb4233c};
std::wstring getText(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n+1, 0); GetWindowTextW(h,s.data(),n+1); s.resize(n); return s; }
bool isAdmin() { SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY; PSID sid = nullptr; BOOL yes = FALSE; if (AllocateAndInitializeSid(&auth,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)) { CheckTokenMembership(nullptr,sid,&yes); FreeSid(sid); } return yes != FALSE; }
std::wstring dateText(uint64_t raw) {
    if (!raw) return L"—"; FILETIME f{DWORD(raw),DWORD(raw>>32)}, local{}; SYSTEMTIME s{};
    FileTimeToLocalFileTime(&f,&local); FileTimeToSystemTime(&local,&s); wchar_t b[32]; swprintf_s(b,L"%02u/%02u/%04u",s.wDay,s.wMonth,s.wYear); return b;
}
std::wstring lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size())); return s; }
struct App {
    HWND hwnd = nullptr, pathEdit = nullptr, searchEdit = nullptr; HBRUSH editBrush = nullptr; HFONT editFont = nullptr;
    ID2D1Factory* factory = nullptr; IDWriteFactory* write = nullptr; ID2D1HwndRenderTarget* rt = nullptr; ID2D1SolidColorBrush* brush = nullptr;
    ID2D1Bitmap* logoBitmap=nullptr;
    std::map<int,IDWriteTextFormat*> formats;
    float dpi = 1, width = 1280, height = 820; bool lightMode = false;
    Palette palette = dark;
    std::vector<std::pair<int,Rect>> hits;
    struct Drive { std::wstring path; uint64_t used, total; }; std::vector<Drive> drives;
    std::wstring initial, message, query, extFilter;
    std::shared_ptr<Result> result, completed;
    std::shared_ptr<Progress> progress;
    std::shared_ptr<DeleteProgress> deleting;
    DeleteResult deleteResult;
    ArchiveResult archiveResult;
    std::thread worker;
    bool busy = false, deleteBusy = false, transferBusy = false, zipBusy = false, permanentDelete = false, turbo = true, allocated = false, descending = true;
    std::wstring operationLabel;
    DWORD pendingCutSequence = 0;
    unsigned workers = 4; uint32_t current = 0; SelectionState selections; uint32_t& selection=selections.focus;
    int tab = 0, sort = 1, hover = -1; size_t scroll = 0;
    std::vector<uint32_t> rows; NavigationHistory history;
    DiskWriter disk;bool loadingCache=false,explicitRoot=false,suppressViewChange=false;
    std::wstring cacheLoadError,cacheTarget;std::optional<SessionState> rescanView;
    mutable uint64_t summaryVersion=0,selectedBytes=0,selectedFiles=0;mutable bool summaryAllocated=false;mutable const Result* summaryResult=nullptr;mutable std::vector<uint32_t> cachedRoots;
    std::vector<FileType> types;
    ViewWorker viewer; uint64_t viewGeneration = 0;
    std::chrono::steady_clock::time_point start;
    bool indexing = false;
    Location location()const{Location s;s.directory=current;s.scroll=scroll;s.tab=tab;s.sort=sort;s.descending=descending;s.allocated=allocated;s.query=getText(searchEdit);s.extension=extFilter;s.selection=selections;return s;}
    SessionState session()const{SessionState s;if(result&&!result->nodes.empty()){s.root=result->root;s.directory=result->path(current);}s.scroll=scroll;s.tab=tab;s.sort=sort;s.descending=descending;s.allocated=allocated;s.query=getText(searchEdit);s.extension=extFilter;return s;}
    void remember(){if(result&&!result->nodes.empty())saveSession(session());}
    void changedView(){SetTimer(hwnd,3,450,nullptr);}
    void persist(){if(result&&!result->nodes.empty()&&!result->cancelled){disk.save(result,[this]{PostMessageW(hwnd,cacheWritten,0,0);});remember();}}
    void restoreSession(const SessionState& s){if(!result||result->nodes.empty())return;auto found=findCached(*result,s.directory);current=found==none?0:found;scroll=size_t(s.scroll);tab=s.tab;sort=s.sort;descending=s.descending;allocated=s.allocated;extFilter=s.extension;suppressViewChange=true;SetWindowTextW(searchEdit,s.query.c_str());suppressViewChange=false;selections.clear();}
    void applyCache(){if(completed){result=std::move(completed);while(current!=none&&current&&result->nodes[current].removed)current=result->nodes[current].parent;if(current==none)current=0;if(result->cancelled)message+=L" · "+result->note;}else message+=L" · F5 pour actualiser les résultats";buildRows();persist();}
    void openLocation(const std::wstring& input){
        if(busy||input.empty())return;auto desired=normalize(input);
        if(result&&!result->nodes.empty()){
            if(!_wcsicmp(desired.c_str(),normalize(result->root).c_str())){SetWindowTextW(pathEdit,result->root.c_str());message=L"Index déjà chargé · aucune nouvelle analyse";invalidate();return;}
            auto id=findCached(*result,desired);if(id!=none&&result->nodes[id].directory){navigate(id);SetWindowTextW(pathEdit,result->root.c_str());message=L"Navigation dans l’index mémorisé · aucune nouvelle analyse";return;}
        }
        remember();KillTimer(hwnd,3);KillTimer(hwnd,2);if(worker.joinable())worker.join();viewer.cancel();++viewGeneration;indexing=false;
        result.reset();completed.reset();progress.reset();rows.clear();types.clear();selections.clear();history.clear();current=0;scroll=0;extFilter.clear();tab=0;
        suppressViewChange=true;SetWindowTextW(searchEdit,L"");suppressViewChange=false;
        cacheTarget=desired;cacheLoadError.clear();SetWindowTextW(pathEdit,desired.c_str());busy=loadingCache=true;message=L"Chargement de l’index mémorisé…";
        worker=std::thread([this,desired]{disk.waitIdle();completed=loadCachedIndex(desired,cacheLoadError);PostMessageW(hwnd,cacheLoaded,0,0);});invalidate();
    }
    void restoreCache(){if(busy)return;auto s=loadSession();if(explicitRoot)openLocation(initial);else if(s&&!s->root.empty())openLocation(s->root);else{
        if(worker.joinable())worker.join();busy=loadingCache=true;cacheTarget.clear();cacheLoadError.clear();worker=std::thread([this]{disk.waitIdle();completed=loadCachedIndex(L"",cacheLoadError);PostMessageW(hwnd,cacheLoaded,0,0);});invalidate();}}
    void finishLoad(){if(worker.joinable())worker.join();busy=loadingCache=false;result=std::move(completed);
        if(result&&!result->nodes.empty()){
            auto s=loadSession(result->root);if(!s){auto last=loadSession();if(last&&!_wcsicmp(last->root.c_str(),result->root.c_str()))s=last;}if(s)restoreSession(*s);
            if(!cacheTarget.empty()&&_wcsicmp(cacheTarget.c_str(),result->root.c_str())){auto id=findCached(*result,cacheTarget);if(id==none||!result->nodes[id].directory){result.reset();current=0;}else{current=id;scroll=0;extFilter.clear();suppressViewChange=true;SetWindowTextW(searchEdit,L"");suppressViewChange=false;}}
            if(result){SetWindowTextW(pathEdit,result->root.c_str());message=L"Index rechargé depuis le cache disque · aucune analyse";buildRows();remember();}
        }
        if(!result||result->nodes.empty()){result.reset();current=0;message=cacheLoadError.empty()?L"Aucune analyse mémorisée pour cet emplacement. Cliquez sur Analyser.":cacheLoadError;buildRows();}
        disk.bytes=cacheBytes();invalidate();}
    const std::vector<uint32_t>& roots()const{if(summaryVersion!=selections.version||summaryResult!=result.get()||summaryAllocated!=allocated){summaryVersion=selections.version;summaryAllocated=allocated;summaryResult=result.get();cachedRoots=result?selectedRoots(*result,selections):std::vector<uint32_t>{};selectedBytes=selectedFiles=0;if(result)for(auto id:cachedRoots){auto& n=result->nodes[id];selectedBytes+=value(n);selectedFiles+=n.directory?n.files:1;}}return cachedRoots;}
    std::vector<std::wstring> selectedPaths()const{std::vector<std::wstring> out;if(result)for(auto id:roots())out.push_back(result->path(id));return out;}
    float sidebar() const { return 204; }
    float inspector() const { return width >= 1250 ? 246.f : 0.f; }
    float contentX() const { return sidebar()+28; }
    float contentW() const { return width-contentX()-28-inspector(); }
    float listY() const { return 354; }
    float listBottom() const { return height-70; }
    float rowHeight() const { return 52.f; }
    size_t page() const { return size_t(std::max(1.f,std::floor((listBottom()-listY())/rowHeight()))); }
    uint64_t value(const Node& n) const { return allocated ? n.totalAllocated : n.total; }
    void invalidate() { InvalidateRect(hwnd,nullptr,FALSE); }
    void font(int key, float size, bool bold=false) {
        IDWriteTextFormat* f = nullptr;
        write->CreateTextFormat(L"Segoe UI",nullptr,bold?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"fr-fr",&f);
        if (f) { f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
            IDWriteInlineObject* sign = nullptr; write->CreateEllipsisTrimmingSign(f,&sign); f->SetTrimming(&trim,sign); release(sign); formats[key]=f; }
    }
    void fill(Rect r,unsigned c,float radius=0,float alpha=1) { brush->SetColor(color(c,alpha)); if(radius) rt->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(),radius,radius),brush); else rt->FillRectangle(r.d2d(),brush); }
    void stroke(Rect r,unsigned c,float radius=0) { brush->SetColor(color(c)); if(radius) rt->DrawRoundedRectangle(D2D1::RoundedRect(r.d2d(),radius,radius),brush,1); else rt->DrawRectangle(r.d2d(),brush,1); }
    void text(const std::wstring& s,Rect r,int key,unsigned c,DWRITE_TEXT_ALIGNMENT align=DWRITE_TEXT_ALIGNMENT_LEADING) {
        auto f = formats[key]; if (!f) return; f->SetTextAlignment(align); brush->SetColor(color(c)); rt->DrawTextW(s.data(),UINT32(s.size()),f,r.d2d(),brush,D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void line(float x,float y,float x2,float y2,unsigned c,float thick=1) { brush->SetColor(color(c)); rt->DrawLine({x,y},{x2,y2},brush,thick); }
    void folder(float x,float y,unsigned c,bool dir=true) {
        if(dir) { fill({x,y+4,18,13},c,3); fill({x+1,y+1,8,6},c,2); }
        else { stroke({x+2,y,13,18},c,2); line(x+5,y+7,x+12,y+7,c); line(x+5,y+11,x+12,y+11,c); }
    }
    void logo(Rect r){if(logoBitmap)rt->DrawBitmap(logoBitmap,r.d2d(),1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);else{fill(r,palette.accentDim,9);folder(r.x+8,r.y+8,palette.accent);}}
    void button(int id,const std::wstring& s,Rect r,bool primary=false,bool danger=false,bool enabled=true) {
        auto fg = danger?palette.danger:primary?(lightMode?0xffffff:0x0e2925):palette.text;
        fill(r,primary?palette.accent:palette.elevated,8,enabled?1:.45f);
        if(hover==id && enabled) stroke(r,danger?palette.danger:palette.accent,8);
        text(s,{r.x+8,r.y+(r.h-20)/2,r.w-16,23},1,enabled?fg:palette.muted,DWRITE_TEXT_ALIGNMENT_CENTER);
        if(enabled) hits.push_back({id,r});
    }
    void setup(HWND h) {
        hwnd=h; dpi=GetDpiForWindow(hwnd)/96.f;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&write));
        font(0,12); font(1,13,true); font(2,14); font(3,18,true); font(4,28,true); font(5,11,true); font(6,13); font(7,38,true);
        pathEdit=CreateWindowExW(0,L"EDIT",initial.c_str(),WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(100),nullptr,nullptr);
        searchEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(101),nullptr,nullptr);
        SendMessageW(pathEdit,EM_SETLIMITTEXT,32700,0); SendMessageW(searchEdit,EM_SETLIMITTEXT,256,0);
        SendMessageW(searchEdit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Rechercher un nom…"));
        // Subclass edit controls for Enter/Escape without replacing their native text behavior.
        for(HWND e : {pathEdit,searchEdit}) { SetPropW(e,L"app",this); SetPropW(e,L"oldproc",reinterpret_cast<HANDLE>(SetWindowLongPtrW(e,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(editProc)))); }
        theme(); layout(); refreshDrives(); DragAcceptFiles(hwnd,TRUE); SetTimer(hwnd,1,120,nullptr);
    }
    static LRESULT CALLBACK editProc(HWND h,UINT m,WPARAM w,LPARAM l) {
        auto a=reinterpret_cast<App*>(GetPropW(h,L"app"));
        if(m==WM_XBUTTONUP){a->travel(GET_XBUTTON_WPARAM(w)==XBUTTON2);return TRUE;}
        if(m==WM_APPCOMMAND){auto command=GET_APPCOMMAND_LPARAM(l);if(command==APPCOMMAND_BROWSER_BACKWARD||command==APPCOMMAND_BROWSER_FORWARD){a->travel(command==APPCOMMAND_BROWSER_FORWARD);return TRUE;}}
        if(m==WM_SYSKEYDOWN&&(w==VK_LEFT||w==VK_RIGHT||w==VK_UP)){if(w==VK_UP)a->parent();else a->travel(w==VK_RIGHT);return 0;}
        if(m==WM_KEYDOWN && w==VK_RETURN && h==a->pathEdit) { a->openLocation(getText(h)); return 0; }
        if(m==WM_KEYDOWN && w==VK_ESCAPE) { SetFocus(a->hwnd); return 0; }
        return CallWindowProcW(reinterpret_cast<WNDPROC>(GetPropW(h,L"oldproc")),h,m,w,l);
    }
    void theme() {
        palette=lightMode?light:dark; BOOL darkTitle=!lightMode; DwmSetWindowAttribute(hwnd,20,&darkTitle,sizeof darkTitle);
        if(editBrush) DeleteObject(editBrush); editBrush=CreateSolidBrush(RGB((palette.field>>16)&255,(palette.field>>8)&255,palette.field&255));
        if(editFont) DeleteObject(editFont); editFont=CreateFontW(-int(14*dpi),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(HWND e:{pathEdit,searchEdit}) { SendMessageW(e,WM_SETFONT,reinterpret_cast<WPARAM>(editFont),TRUE); InvalidateRect(e,nullptr,TRUE); }
        invalidate();
    }
    void layout() {
        RECT r{}; GetClientRect(hwnd,&r); width=r.right/dpi; height=r.bottom/dpi;
        float x=contentX(),w=contentW();
        MoveWindow(pathEdit,int((x+14)*dpi),int(97*dpi),int((w-232)*dpi),int(24*dpi),TRUE);
        MoveWindow(searchEdit,int((x+w-238)*dpi),int(291*dpi),int(222*dpi),int(23*dpi),TRUE);
        if(rt) rt->Resize(D2D1::SizeU(r.right,r.bottom));
        clampScroll(); invalidate();
    }
    void refreshDrives() {
        drives.clear(); DWORD mask=GetLogicalDrives();
        for(int i=0;i<26;++i) if(mask&(1u<<i)) { std::wstring p{wchar_t(L'A'+i),L':',L'\\'};
            if(GetDriveTypeW(p.c_str())==DRIVE_REMOTE || GetDriveTypeW(p.c_str())==DRIVE_CDROM) continue;
            ULARGE_INTEGER free{},total{},allFree{}; if(GetDiskFreeSpaceExW(p.c_str(),&free,&total,&allFree)) drives.push_back({p,total.QuadPart-allFree.QuadPart,total.QuadPart}); }
    }
    void ensureTarget() {
        if(rt) return; RECT r{}; GetClientRect(hwnd,&r);
        factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),D2D1::HwndRenderTargetProperties(hwnd,D2D1::SizeU(r.right,r.bottom)),&rt);
        if(rt) { rt->SetDpi(96*dpi,96*dpi); rt->CreateSolidColorBrush(color(palette.text),&brush);
            IWICImagingFactory* imaging=nullptr;IWICStream* stream=nullptr;IWICBitmapDecoder* decoder=nullptr;IWICBitmapFrameDecode* frame=nullptr;IWICFormatConverter* converter=nullptr;
            auto module=GetModuleHandleW(nullptr);auto resource=FindResourceW(module,MAKEINTRESOURCEW(104),RT_RCDATA);auto data=resource?LoadResource(module,resource):nullptr;
            if(data&&SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))&&SUCCEEDED(imaging->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(data)),SizeofResource(module,resource)))&&SUCCEEDED(imaging->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder))&&SUCCEEDED(decoder->GetFrame(0,&frame))&&SUCCEEDED(imaging->CreateFormatConverter(&converter))&&SUCCEEDED(converter->Initialize(frame,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))rt->CreateBitmapFromWicBitmap(converter,nullptr,&logoBitmap);
            release(converter);release(frame);release(decoder);release(stream);release(imaging);
        }
    }
    void paint() {
        PAINTSTRUCT ps; BeginPaint(hwnd,&ps); ensureTarget(); if(!rt || !brush) { EndPaint(hwnd,&ps); return; }
        rt->BeginDraw(); rt->Clear(color(palette.bg)); hits.clear();
        float sx=sidebar(),x=contentX(),cw=contentW(),right=x+cw;
        fill({0,0,sx,height},palette.panel); line(sx,0,sx,height,palette.line);
        logo({19,24,42,42});
        text(L"ScanFolder",{66,28,128,27},3,palette.text); text(L"L’ESPACE, EN CLAIR.",{23,77,170,20},5,palette.muted);
        text(L"EMPLACEMENTS",{23,133,170,20},5,palette.muted);
        float dy=169;
        for(size_t i=0;i<drives.size() && dy<height-300;++i,dy+=90) {
            auto& d=drives[i]; Rect area{14,dy,176,78};auto shown=getText(pathEdit);bool active=shown.size()>1&&towupper(shown[0])==towupper(d.path[0]);
            if(active || hover==200+int(i)) fill(area,active?palette.elevated:palette.field,10);
            folder(26,dy+10,palette.muted); text(L"Disque "+d.path.substr(0,2),{54,dy+8,126,22},1,palette.text);
            text(sizeText(d.used)+L" / "+sizeText(d.total),{26,dy+34,155,18},0,palette.muted);
            fill({26,dy+60,148,4},palette.line,2); fill({26,dy+60,float(148.*d.used/std::max<uint64_t>(1,d.total)),4},palette.accent,2);
            hits.push_back({200+int(i),area});
        }
        text(L"MOTEUR DE SCAN",{23,height-243,170,20},5,palette.muted);
        button(27,L"Vider cache · "+sizeText(disk.bytes.load()),{17,height-294,170,34},false,false,!busy&&disk.bytes>0);
        button(20,turbo?L"✓  Turbo NTFS":L"○  Parcours standard",{17,height-213,170,36});
        button(21,std::to_wstring(workers)+L" workers   +",{17,height-169,170,34});
        if(!isAdmin()) button(22,L"Mode administrateur ↗",{17,height-126,170,34});
        text(L"●  Local · sans télémétrie",{23,height-70,173,20},0,palette.accent);
        text(L"C++ natif  /  v1.2  ·  À propos",{23,height-46,174,20},0,palette.muted); hits.push_back({26,{17,height-49,179,23}});
        text(L"Votre stockage, sans détour.",{x,27,cw-170,34},4,palette.text);
        button(23,lightMode?L"☾ Sombre":L"☼ Clair",{right-114,28,114,32});
        text(L"Repérez les dossiers lourds. Retrouvez de la place.",{x,65,cw,23},2,palette.muted);
        fill({x,91,cw-217,38},palette.field,8); stroke({x,91,cw-217,38},palette.line,8);
        button(1,L"Parcourir",{right-205,91,97,38},false,false,!busy);
        button(2,busy?L"Arrêter":L"Analyser",{right-98,91,98,38},!busy,busy);
        uint64_t bytes=0,files=0,folders=0;
        if(busy && !deleteBusy && !transferBusy && !zipBusy && progress) { bytes=progress->bytes; files=progress->files; folders=progress->folders; }
        else if(result && !result->nodes.empty()) { auto& n=result->nodes[current]; bytes=value(n); files=n.files; folders=n.folders; }
        float cardW=(cw-24)/3;
        bool allocationEstimate=result && result->method.find(L"MFT")==std::wstring::npos;
        const std::wstring labels[]={allocated?(allocationEstimate?L"ALLOCATION ESTIMÉE":L"ESPACE ALLOUÉ"):L"TAILLE DES FICHIERS",L"FICHIERS",L"DOSSIERS"};
        const std::wstring values[]={sizeText(bytes),countText(files),countText(folders)};
        for(int i=0;i<3;++i) { float cx=x+i*(cardW+12); fill({cx,151,cardW,99},palette.panel,12); stroke({cx,151,cardW,99},palette.line,12);
            text(labels[i],{cx+17,167,cardW-30,18},5,palette.muted); text(values[i],{cx+17,194,cardW-30,38},4,i?palette.text:palette.accent); }
        const std::wstring tabs[]={L"Dossiers",L"Gros fichiers",L"Types"}; float tx=x;
        for(int i=0;i<3;++i) { float tw=i==1?118.f:91.f; Rect rr{tx,282,tw,40}; if(tab==i) fill(rr,palette.elevated,8);
            text(tabs[i],{tx+10,291,tw-20,23},1,tab==i?palette.accent:palette.muted); hits.push_back({10+i,rr}); tx+=tw+5; }
        fill({right-250,284,250,35},palette.field,8); stroke({right-250,284,250,35},palette.line,8);
        button(4,L"←",{x,261,30,20},false,false,!busy&&history.canBack());
        button(5,L"→",{x+34,261,30,20},false,false,!busy&&history.canForward());
        button(3,L"↑",{x+68,261,30,20},false,false,result && current!=0 && !busy);
        if(result && !busy) button(46,L"Coller ici",{right-100,258,100,24});
        std::wstring location=result && !result->nodes.empty()?result->path(current):L"Choisissez un dossier ou un disque";
        text(location,{x+108,260,cw-220,22},0,palette.muted);
        line(x,327,right,327,palette.line);
        float nameW=std::max(160.f,cw-350);
        const std::wstring heads[]={tab==2?L"EXTENSION":L"NOM",allocated?L"ALLOUÉ":L"TAILLE",L"FICHIERS",L"MODIFIÉ"};
        float hx[]={x+12,x+nameW+10,right-180,right-95}; float hw[]={nameW-18,135,78,90};
        for(int i=0;i<4;++i) { if(tab==2 && i==3) continue; text(heads[i]+(sort==i?(descending?L" ↓":L" ↑"):L""),{hx[i],332,hw[i],20},5,palette.muted); hits.push_back({30+i,{hx[i],328,hw[i],26}}); }
        rt->PushAxisAlignedClip({x,listY(),right,listBottom()},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if(busy || indexing) {
            text(indexing&&!busy?L"Préparation de la vue…":loadingCache?L"Chargement du cache disque…":deleteBusy?L"Suppression en cours…":(transferBusy||zipBusy)?operationLabel+L"…":L"Analyse en cours…",{x+24,listY()+35,cw-48,36},3,palette.text);
            text(L"Le travail continue en arrière-plan. Vous pouvez l’arrêter.",{x+24,listY()+75,cw-48,25},2,palette.muted);
            float t=float(GetTickCount64()%1800)/1800;
            fill({x+24,listY()+126,cw-48,4},palette.elevated,2); fill({x+24+(cw-160)*t,listY()+126,90,4},palette.accent,2);
            if((deleteBusy||transferBusy||zipBusy) && deleting) { text(zipBusy?std::to_wstring(deleting->percent.load())+L" % · "+countText(deleting->files)+L" fichiers":countText(deleting->files)+L" fichiers · "+countText(deleting->folders)+L" dossiers traités",{x+24,listY()+152,cw-48,25},2,palette.accent);
                text(countText(deleting->errors)+L" erreurs",{x+24,listY()+185,cw-48,25},2,palette.muted); }
        } else if(!result || result->nodes.empty()) {
            logo({x+cw/2-40,listY()+40,80,80});
            text(L"Cet emplacement attend son analyse.",{x+20,listY()+140,cw-40,36},3,palette.text,DWRITE_TEXT_ALIGNMENT_CENTER);
            text(L"Cliquez sur Analyser pour créer ou renouveler son index.",{x+20,listY()+183,cw-40,28},2,palette.muted,DWRITE_TEXT_ALIGNMENT_CENTER);
            text(L"Un EXE portable. Aucune donnée envoyée.",{x+20,listY()+218,cw-40,25},0,palette.muted,DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            size_t count=tab==2?types.size():rows.size();
            for(size_t k=scroll;k<std::min(count,scroll+page()+1);++k) {
                float y=listY()+float(k-scroll)*rowHeight(); Rect rr{x,y,cw-9,rowHeight()-3};
                bool selected=tab!=2 && selections.ids.contains(rows[k]);
                if(selected) fill(rr,palette.accentDim,8); else if(hover==1000+int(k-scroll)) fill(rr,palette.panel,8);
                uint64_t sz; std::wstring name,sub,date; uint64_t fc; bool dir=false,link=false;
                if(tab==2) { auto& t=types[k]; sz=t.bytes; name=t.name; fc=t.count; }
                else { auto& n=result->nodes[rows[k]]; sz=value(n); name=n.name; dir=n.directory; link=n.reparse; fc=n.directory?n.files:0; date=dateText(n.modified);
                    if(tab==1) { auto full=result->path(rows[k]); auto pos=full.find_last_of(L'\\'); if(pos!=std::wstring::npos) sub=full.substr(0,pos); }
                    else if(link) sub=L"Lien · cible non parcourue";
                    else if(dir) sub=countText(n.folders)+L" sous-dossiers"; }
                folder(x+12,y+15,dir?palette.accent:palette.muted,dir);
                text(name,{x+43,y+7,nameW-54,23},2,palette.text);
                if(!sub.empty()) text(sub,{x+43,y+29,nameW-54,17},0,palette.muted);
                float maxBar=std::max(30.f,hw[1]-17),fraction=0;
                auto denom=value(result->nodes[current]); if(denom) fraction=float(double(sz)/double(denom));
                text(sizeText(sz),{hx[1],y+8,hw[1]-6,23},1,palette.text);
                fill({hx[1],y+35,maxBar,3},palette.elevated,1); fill({hx[1],y+35,maxBar*std::clamp(fraction,0.f,1.f),3},palette.accent,1);
                text((tab==2||dir)?countText(fc):L"—",{hx[2],y+14,hw[2],22},6,palette.muted);
                if(tab!=2) text(date,{hx[3],y+14,hw[3],22},0,palette.muted);
                hits.push_back({1000+int(k-scroll),rr});
            }
            if(!count) text(L"Aucun élément dans cette vue.",{x+20,listY()+42,cw-40,30},2,palette.muted,DWRITE_TEXT_ALIGNMENT_CENTER);
            if(count>page()) { float h=listBottom()-listY(),thumb=std::max(28.f,h*page()/float(count)); fill({right-4,listY(),3,h},palette.line,1); fill({right-4,listY()+(h-thumb)*scroll/float(count-page()),3,thumb},palette.muted,1); }
        }
        rt->PopAxisAlignedClip();
        if(inspector()) {
            float ix=right+24,iw=inspector()-24;
            fill({ix,91,iw,height-161},palette.panel,12); text(L"SÉLECTION",{ix+18,110,iw-36,20},5,palette.muted);
            bool has=result && !selections.ids.empty() && selection!=none && selection<result->nodes.size() && !busy;
            if(has) {
                auto& n=result->nodes[selection];bool multi=selections.ids.size()>1;roots();
                folder(ix+18,148,palette.accent,n.directory);
                text(multi?countText(selections.ids.size())+L" éléments":n.name,{ix+18,181,iw-36,30},3,palette.text);text(sizeText(selectedBytes),{ix+18,224,iw-36,40},4,palette.accent);
                text(countText(selectedFiles)+L" fichiers",{ix+18,274,iw-36,23},2,palette.muted);
                text(multi?L"Ctrl+clic · Maj+clic":n.reparse?L"Lien / jonction":dateText(n.modified),{ix+18,306,iw-36,23},0,palette.muted);
                button(40,L"Ouvrir dans l’Explorateur",{ix+14,342,iw-28,36});
                button(44,L"Copier",{ix+14,389,(iw-36)/2,35});
                button(45,L"Couper",{ix+22+(iw-36)/2,389,(iw-36)/2,35});
                button(47,L"Copier vers…",{ix+14,434,(iw-36)/2,35});
                button(48,L"Déplacer…",{ix+22+(iw-36)/2,434,(iw-36)/2,35});
                button(49,L"Compresser en ZIP…",{ix+14,481,iw-28,37},false,false,!multi);
                line(ix+18,538,ix+iw-18,538,palette.line);
                button(42,L"Mettre à la corbeille",{ix+14,556,iw-28,36});
                button(43,L"Supprimer définitivement",{ix+14,604,iw-28,39},false,true);
                text(L"Confirmation obligatoire",{ix+18,661,iw-36,20},0,palette.muted);
            } else {
                text(L"Chaque octet compte.",{ix+18,155,iw-36,30},3,palette.text);
                text(L"Sélectionnez une ligne",{ix+18,205,iw-36,23},2,palette.muted);
                text(L"pour voir ses détails",{ix+18,232,iw-36,23},2,palette.muted);
                text(L"et les actions disponibles.",{ix+18,259,iw-36,23},2,palette.muted);
                line(ix+18,322,ix+iw-18,322,palette.line);
                text(L"RACCOURCIS",{ix+18,345,iw-36,20},5,palette.muted);
                const std::wstring shortcuts[]={L"Souris 4/5    Préc. / Suiv.",L"Alt + ← / →    Historique",L"Ctrl / Maj + clic    Sélection",L"Ctrl + A    Tout sélectionner",L"Ctrl + C / X    Copier / Couper",L"Ctrl + V    Coller ici",L"F5    Rescanner",L"Suppr / Maj+Suppr    Supprimer"};
                for(int i=0;i<8;++i) text(shortcuts[i],{ix+18,380.f+32*i,iw-36,22},0,palette.muted);
            }
        }
        // Compact layouts retain all actions in the footer.
        if(!inspector() && result && !selections.ids.empty() && selection!=none && !busy) { button(40,L"Explorer",{x,listBottom()+8,86,29}); button(42,L"Corbeille",{x+94,listBottom()+8,88,29}); button(43,L"Supprimer définitivement",{x+190,listBottom()+8,196,29},false,true); }
        else if(result && !busy) {
            text(countText(tab==2?types.size():rows.size())+L" affichés · "+countText(selections.ids.size())+L" sélectionnés",{x,listBottom()+12,cw-300,22},0,palette.muted);
        }
        button(24,allocated?L"✓ Alloué":L"Taille logique",{right-198,listBottom()+8,110,29},false,false,!busy);
        button(25,L"CSV ↗",{right-78,listBottom()+8,78,29},false,false,result && !busy);
        fill({sx,height-30,width-sx,30},palette.panel);
        std::wstring status=message;
        if(busy) {
            if(deleteBusy||transferBusy||zipBusy) { status=operationLabel; if(deleting) { std::lock_guard lock(deleting->mutex); status+=L"  ·  "+deleting->current; } }
            else if(progress) { std::lock_guard lock(progress->mutex); status=progress->phase+L"  ·  "+(progress->records?countText(progress->records)+L" enregistrements  ·  ":L"")+progress->current; }
        } else if(status.empty()) status=result?result->method:L"Prêt · Déposez un dossier pour commencer";
        text(status,{x,height-24,width-x-220,20},0,palette.muted);
        if(result && !busy) { wchar_t b[80]; swprintf_s(b,L"%.2f s  ·  %llu erreurs",result->seconds,result->errors); text(b,{width-220,height-24,200,20},0,result->errors?palette.danger:palette.muted,DWRITE_TEXT_ALIGNMENT_TRAILING); }
        auto hr=rt->EndDraw(); if(hr==D2DERR_RECREATE_TARGET) { release(logoBitmap);release(brush); release(rt); }
        EndPaint(hwnd,&ps);
    }
    void clampScroll() { size_t n=tab==2?types.size():rows.size(); scroll=std::min(scroll,n>page()?n-page():0); }
    void buildRows() {
        rows.clear(); types.clear(); if(!result || result->nodes.empty()) { indexing=false;invalidate();return; }
        ViewRequest q; q.result=result;q.current=current;q.tab=tab;q.sort=sort;q.descending=descending;q.allocated=allocated;q.query=getText(searchEdit);q.extension=extFilter;
        indexing=true;viewGeneration=viewer.submit(std::move(q),[this]{PostMessageW(hwnd,viewDone,0,0);});invalidate();
    }
    void finishView() {auto ready=viewer.take();if(!ready||ready->generation!=viewGeneration)return;rows=std::move(ready->rows);types=std::move(ready->types);indexing=false;selections.retain(rows);if(!ready->error.empty())message=ready->error;clampScroll();changedView();invalidate();}
    void browse() {
        IFileOpenDialog* d=nullptr; if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&d)))) {
            DWORD flags;d->GetOptions(&flags);d->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
            d->SetTitle(L"Choisir le dossier à analyser");
            if(SUCCEEDED(d->Show(hwnd))) { IShellItem* item=nullptr; if(SUCCEEDED(d->GetResult(&item))) { PWSTR path=nullptr;
                if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) { openLocation(path);CoTaskMemFree(path); } item->Release(); } }
            d->Release(); }
    }
    void beginScan() {
        if(busy){if((deleteBusy||transferBusy||zipBusy)&&deleting)deleting->cancel=true;if(progress)progress->cancel=true;return;}
        auto path=getText(pathEdit); if(path.empty()) { browse();return; }
        if(worker.joinable()) worker.join();
        remember();KillTimer(hwnd,3);rescanView=result&&_wcsicmp(normalize(path).c_str(),normalize(result->root).c_str())==0?std::optional<SessionState>(session()):loadSession(normalize(path));
        viewer.cancel();indexing=false;progress=std::make_shared<Progress>();busy=true;deleteBusy=false;transferBusy=false;zipBusy=false;message.clear();scroll=0;selections.clear();
        start=std::chrono::steady_clock::now(); auto p=progress; Options options{turbo,workers};
        worker=std::thread([this,path,p,options] { try { completed=sf::scan(path,*p,options); }
            catch(const std::exception&) { completed=std::make_shared<Result>();completed->root=path;completed->errors=1;completed->note=L"L’analyse a échoué (mémoire ou erreur système)."; }
            PostMessageW(hwnd,scanDone,0,0); }); invalidate();
    }
    void finishScan() {
        if(worker.joinable()) worker.join();result=std::move(completed);busy=false;current=0;history.clear();selections.clear();extFilter.clear();
        if(result) { SetWindowTextW(pathEdit,result->root.c_str()); if(result->nodes.empty()) message=result->note;
            else if(result->cancelled) message=L"Scan interrompu · résultats partiels";
            else message=result->method; }
        if(rescanView&&result&&!result->nodes.empty())restoreSession(*rescanView);rescanView.reset();buildRows();persist();refreshDrives();invalidate();
    }
    void navigate(uint32_t i) {
        if(busy||!result||i==current||i>=result->nodes.size()||result->nodes[i].removed||!result->nodes[i].directory||result->nodes[i].reparse)return;
        history.visit(location());current=i;selections.clear();scroll=0;extFilter.clear();suppressViewChange=true;SetWindowTextW(searchEdit,L"");suppressViewChange=false;buildRows();
    }
    void travel(bool forward){if(busy||!result)return;auto from=location();for(;;){auto target=forward?history.forward(from):history.back(from);if(!target)return;if(target->directory>=result->nodes.size()||result->nodes[target->directory].removed)continue;
        current=target->directory;scroll=target->scroll;tab=target->tab;sort=target->sort;descending=target->descending;allocated=target->allocated;extFilter=target->extension;selections=std::move(target->selection);suppressViewChange=true;SetWindowTextW(searchEdit,target->query.c_str());suppressViewChange=false;buildRows();return;}}
    void parent() { if(result && current && !busy) navigate(result->nodes[current].parent); }
    void copyPath() { if(!result||selection==none) return;auto s=result->path(selection);
        if(OpenClipboard(hwnd)) { HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,(s.size()+1)*2);if(mem) { void* p=GlobalLock(mem);if(p) { memcpy(p,s.c_str(),(s.size()+1)*2);GlobalUnlock(mem);EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,mem)) GlobalFree(mem); } else GlobalFree(mem); }CloseClipboard(); } }
    void explorer() { if(result&&selection!=none) { auto args=L"/select,\""+result->path(selection)+L"\""; ShellExecuteW(hwnd,L"open",L"explorer.exe",args.c_str(),nullptr,SW_SHOWNORMAL); } }
    bool confirm(const std::vector<uint32_t>& ids,bool permanent);
    void remove(bool permanent) {
        if(busy||indexing||!result||selections.ids.empty())return;
        auto ids=roots();auto paths=selectedPaths();if(paths.empty())return;std::wstring error;
        for(auto& path:paths)if(!validateDelete(path,error)){MessageBoxW(hwnd,(path+L"\n\n"+error).c_str(),L"Suppression bloquée",MB_OK|MB_ICONWARNING);return;}
        if(!confirm(ids,permanent))return;
        if(worker.joinable()) worker.join();
        viewer.cancel();indexing=false;completed.reset();busy=true;deleteBusy=true;permanentDelete=permanent;operationLabel=permanent?L"Suppression définitive · "+std::to_wstring(workers)+L" workers":L"Mise à la corbeille";deleting=std::make_shared<DeleteProgress>();auto p=deleting;auto count=workers;auto source=result;progress=std::make_shared<Progress>();auto cacheProgress=progress;
        worker=std::thread([this,paths,permanent,p,count,source,cacheProgress] {
            try {
                if(permanent)deleteResult=deleteBatch(paths,*p,count);
                else deleteResult=shellOperation(hwnd,paths,L"",false,true,*p);
            } catch(const std::exception&) {deleteResult.errors=1;deleteResult.message=L"Erreur système pendant la suppression.";}
            try{CacheChanges changes;changes.changed=paths;for(auto& path:paths)changes.parents.push_back(std::filesystem::path(path).parent_path().wstring());completed=refreshCache(source,changes,*cacheProgress,count);}catch(...){deleteResult.message+=L"\nLe cache n’a pas pu être actualisé. F5 pour relancer une analyse.";}
            PostMessageW(hwnd,deleteDone,0,0);
        });invalidate();
    }
    void finishDelete() {
        if(worker.joinable()) worker.join();busy=false;deleteBusy=false;
        std::wstring s=deleteResult.cancelled?L"Suppression interrompue.":L"Opération terminée.";
        s+=L"\n\n"+countText(deleteResult.files)+L" fichiers et "+countText(deleteResult.folders)+(permanentDelete?L" dossiers supprimés définitivement.":L" dossiers traités pour la corbeille.");
        if(!deleteResult.message.empty()) s+=L"\n\n"+deleteResult.message;
        if(deleteResult.errors) s+=L"\n\n"+countText(deleteResult.errors)+L" erreurs : des éléments sont restés en place.";
        if(deleteResult.errors)MessageBoxW(hwnd,s.c_str(),L"ScanFolder · suppression",MB_OK|MB_ICONWARNING);
        message=deleteResult.cancelled?L"Suppression interrompue · position conservée":L"Sélection supprimée · position conservée";
        applyCache();
    }
    void exportData() {
        if(!result) return;wchar_t file[32768]=L"ScanFolder.csv";OPENFILENAMEW o{sizeof o};o.hwndOwner=hwnd;o.lpstrFile=file;o.nMaxFile=32768;
        o.lpstrFilter=L"Fichier CSV\0*.csv\0";o.lpstrDefExt=L"csv";o.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
        if(GetSaveFileNameW(&o)) {std::wstring error;if(!exportCsv(*result,file,error)) MessageBoxW(hwnd,error.c_str(),L"Export",MB_OK|MB_ICONERROR);else {message=L"Export CSV enregistré";invalidate();}}
    }
    void context(int px,int py) {
        if(!result||selection==none||selections.ids.empty()||busy)return;HMENU m=CreatePopupMenu();AppendMenuW(m,MF_STRING,40,L"Ouvrir dans l’Explorateur");AppendMenuW(m,MF_STRING,41,L"Copier le chemin");AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        AppendMenuW(m,MF_STRING,44,L"Copier\tCtrl+C");AppendMenuW(m,MF_STRING,45,L"Couper\tCtrl+X");AppendMenuW(m,MF_STRING,46,L"Coller ici\tCtrl+V");
        AppendMenuW(m,MF_STRING,47,L"Copier vers…");AppendMenuW(m,MF_STRING,48,L"Déplacer vers…");AppendMenuW(m,MF_STRING|(selections.ids.size()>1?MF_GRAYED:0),49,L"Compresser en ZIP…");AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        AppendMenuW(m,MF_STRING,42,L"Mettre à la corbeille…");AppendMenuW(m,MF_STRING,43,L"Supprimer définitivement…");
        POINT pt{px,py};ClientToScreen(hwnd,&pt);int id=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,nullptr);DestroyMenu(m);action(id,false);
    }
    void action(int id,bool twice) {
        if(id>=1000) { if(busy||indexing)return;size_t index=scroll+size_t(id-1000); if(tab==2) {if(twice&&index<types.size()) {extFilter=types[index].name==L"Sans extension"?L"<none>":types[index].name;tab=1;scroll=0;buildRows();}return;}
            if(index<rows.size()){selections.select(rows,index,false,false);if(twice&&result->nodes[selection].directory)navigate(selection);invalidate();}return; }
        if(id>=200&&id<200+int(drives.size())) {openLocation(drives[id-200].path);return;}
        if(id>=30&&id<=33) {if(sort==id-30)descending=!descending;else{sort=id-30;descending=sort!=0;}buildRows();return;}
        if(id>=10&&id<=12) {if(!busy){tab=id-10;scroll=0;extFilter.clear();selections.clear();buildRows();}return;}
        switch(id) {
        case 1:browse();break;case 2:beginScan();break;case 3:parent();break;case 4:travel(false);break;case 5:travel(true);break;
        case 20:if(!busy){turbo=!turbo;invalidate();}break;
        case 21:if(!busy){workers=workers==8?1:workers+1;invalidate();}break;
        case 22:{if(busy)break;wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);auto args=L"--open \""+getText(pathEdit)+L"\"";auto code=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"runas",exe,args.c_str(),nullptr,SW_SHOWNORMAL));if(code>32)PostMessageW(hwnd,WM_CLOSE,0,0);break;}
        case 23:lightMode=!lightMode;theme();break;case 24:allocated=!allocated;buildRows();break;case 25:exportData();break;
        case 27:if(!busy&&MessageBoxW(hwnd,L"Vider les index mémorisés de tous les disques et dossiers ?\nVos fichiers ne seront pas supprimés. L’index déjà chargé restera en mémoire jusqu’à la fermeture.",L"Vider le cache",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)==IDYES){disk.clear([this]{PostMessageW(hwnd,cacheWritten,0,0);});message=L"Cache disque vidé · index en mémoire conservé";invalidate();}break;
        case 26:MessageBoxW(hwnd,L"ScanFolder 1.2 · C++ natif · Licence MIT\nAnalyse locale, sans compte ni télémétrie.\n\nCache séparé par disque et dossier analysé. Changer d’emplacement recharge l’index sans scan. Analyser (ou F5) renouvelle seulement l’index choisi.\n\nTurbo NTFS : accès administrateur requis. Une MFT non prise en charge entraîne un parcours standard.\nParcours standard : allocation estimée ; les liens physiques peuvent être comptés plusieurs fois.\nJonctions non parcourues. Flux alternatifs exclus.\n\nZIP : moteur 7-Zip 26.04 embarqué, Copyright Igor Pavlov, GNU LGPL 2.1 ou ultérieure.\nSources : https://www.7-zip.org/ et third_party/7zip.\n7za.exe peut être remplacé via la variable SCANFOLDER_7ZA.\n\nLe scan est un instantané : les fichiers peuvent évoluer pendant l’analyse.",L"À propos de ScanFolder",MB_OK|MB_ICONINFORMATION);break;
        case 40:explorer();break;case 41:copyPath();break;case 42:remove(false);break;case 43:remove(true);break;
        case 44:case 45:if(!busy&&result&&!selections.ids.empty()){if(putFilesOnClipboard(hwnd,selectedPaths(),id==45)){message=id==45?L"Sélection coupée · Ctrl+V dans le dossier de destination":L"Sélection copiée · Ctrl+V pour coller";invalidate();}}break;
        case 46:paste();break;case 47:transferTo(false);break;case 48:transferTo(true);break;case 49:archive();break;
        }
    }
    void click(float x,float y,bool twice,bool right=false) {
        SetFocus(hwnd);for(auto it=hits.rbegin();it!=hits.rend();++it)if(it->second.contains(x,y)){
            if(it->first>=1000&&tab!=2&&!busy&&!indexing){size_t position=scroll+size_t(it->first-1000);selections.select(rows,position,GetKeyState(VK_CONTROL)<0,GetKeyState(VK_SHIFT)<0,right);if(twice&&selection!=none&&result->nodes[selection].directory)navigate(selection);invalidate();}
            else action(it->first,twice);if(right)context(int(x*dpi),int(y*dpi));return;}
        if(!busy&&x>=contentX()&&x<contentX()+contentW()&&y>=listY()&&y<listBottom()){selections.clear();invalidate();}
    }
    void key(WPARAM k) {
        bool ctrl=GetKeyState(VK_CONTROL)<0,shift=GetKeyState(VK_SHIFT)<0;
        if(ctrl&&k=='F') {SetFocus(searchEdit);SendMessageW(searchEdit,EM_SETSEL,0,-1);return;}
        if(ctrl&&k=='L') {SetFocus(pathEdit);SendMessageW(pathEdit,EM_SETSEL,0,-1);return;}
        if(k==VK_F5)beginScan();else if(k==VK_ESCAPE&&busy)beginScan();else if(!busy) {
            if(ctrl&&k=='A'&&tab!=2&&!indexing){selections.all(rows);invalidate();}else if(k==VK_BACK)parent();else if(k==VK_DELETE)remove(shift);else if(ctrl&&k=='C')action(44,false);else if(ctrl&&k=='X')action(45,false);else if(ctrl&&k=='V')paste();
            else if(k==VK_RETURN&&selection!=none)navigate(selection);
            else if((k==VK_UP||k==VK_DOWN||k==VK_PRIOR||k==VK_NEXT||k==VK_HOME||k==VK_END)&&tab!=2&&!rows.empty()) {
                auto found=std::find(rows.begin(),rows.end(),selection);int pos=found==rows.end()?-1:int(found-rows.begin());
                if(k==VK_DOWN)++pos;if(k==VK_UP)--pos;if(k==VK_NEXT)pos+=int(page());if(k==VK_PRIOR)pos-=int(page());if(k==VK_HOME)pos=0;if(k==VK_END)pos=int(rows.size())-1;
                pos=std::clamp(pos,0,int(rows.size())-1);if(ctrl&&!shift)selection=rows[pos];else selections.select(rows,size_t(pos),ctrl,shift);if(size_t(pos)<scroll)scroll=pos;else if(size_t(pos)>=scroll+page())scroll=pos-page()+1;changedView();invalidate();
            }
        }
    }
    void paste(); void transferTo(bool move); void transfer(const std::vector<std::wstring>& sources,const std::wstring& dest,bool move); void archive(); void finishTransfer(); void finishZip();
    ~App() { viewer.shutdown();if(progress)progress->cancel=true;if(deleting)deleting->cancel=true;if(worker.joinable())worker.join();disk.shutdown();for(auto [k,f]:formats)f->Release();release(logoBitmap);release(brush);release(rt);release(factory);release(write);if(editBrush)DeleteObject(editBrush);if(editFont)DeleteObject(editFont); }
};

// A dedicated modal forces a fresh, typed confirmation for every permanent deletion.
struct ConfirmDialog {
    HWND owner=nullptr,hwnd=nullptr,edit=nullptr,ok=nullptr;bool accepted=false,done=false,permanent=false;HFONT font=nullptr;HBRUSH bg=nullptr;
    std::wstring path,summary;
    static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
        auto d=reinterpret_cast<ConfirmDialog*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE){d=reinterpret_cast<ConfirmDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(d));d->hwnd=h;}
        if(!d)return DefWindowProcW(h,m,w,l);
        switch(m) {
        case WM_CREATE:{
            UINT dpi=GetDpiForWindow(h);float scale=dpi/96.f;
            d->font=CreateFontW(-int(15*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            d->bg=CreateSolidBrush(RGB(247,249,252));
            auto ctl=[&](const wchar_t* type,const std::wstring& text,DWORD style,int x,int y,int width,int height,int id) {
                HWND c=CreateWindowExW(wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,text.c_str(),WS_CHILD|WS_VISIBLE|style,int(x*scale),int(y*scale),int(width*scale),int(height*scale),h,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
                SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(d->font),TRUE);return c;};
            ctl(L"STATIC",d->permanent?L"Supprimer définitivement les éléments sélectionnés ?":L"Envoyer la sélection à la corbeille ?",0,26,23,565,30,0);
            ctl(L"EDIT",d->path,ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,26,69,560,90,0);
            ctl(L"STATIC",d->summary,0,26,177,560,55,0);
            ctl(L"STATIC",d->permanent?L"Cette action ne passe pas par la corbeille. Elle est irréversible.\nSaisissez SUPPRIMER pour confirmer cette suppression.":L"Windows doit disposer d’une corbeille sur ce volume.\nAucun repli vers une suppression définitive n’est demandé.",0,26,241,560,55,0);
            if(d->permanent)d->edit=ctl(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,26,307,560,33,103);
            ctl(L"BUTTON",L"Annuler",BS_PUSHBUTTON|WS_TABSTOP,322,365,122,37,IDCANCEL);
            d->ok=ctl(L"BUTTON",d->permanent?L"Supprimer":L"Confirmer",BS_PUSHBUTTON|WS_TABSTOP,457,365,129,37,IDOK);
            if(d->permanent)EnableWindow(d->ok,FALSE);SetFocus(d->permanent?d->edit:GetDlgItem(h,IDCANCEL));return 0;
        }
        case WM_COMMAND:
            if(LOWORD(w)==103&&HIWORD(w)==EN_CHANGE){EnableWindow(d->ok,getText(d->edit)==L"SUPPRIMER");return 0;}
            if(LOWORD(w)==IDOK){if(!d->permanent||getText(d->edit)==L"SUPPRIMER"){d->accepted=true;DestroyWindow(h);}return 0;}
            if(LOWORD(w)==IDCANCEL){DestroyWindow(h);return 0;}break;
        case WM_CTLCOLORSTATIC:{auto dc=reinterpret_cast<HDC>(w);SetTextColor(dc,RGB(30,42,61));SetBkColor(dc,RGB(247,249,252));return reinterpret_cast<LRESULT>(d->bg);}
        case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect(reinterpret_cast<HDC>(w),&r,d->bg);return 1;}
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:d->done=true;return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }
    bool show(HWND parent) {
        owner=parent;WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"ScanFolderConfirm";c.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&c);
        RECT pr;GetWindowRect(parent,&pr);float scale=GetDpiForWindow(parent)/96.f;RECT r{0,0,int(614*scale),int(425*scale)};AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME);
        hwnd=CreateWindowExW(WS_EX_DLGMODALFRAME,c.lpszClassName,L"ScanFolder · confirmation obligatoire",WS_CAPTION|WS_SYSMENU,(pr.left+pr.right-(r.right-r.left))/2,(pr.top+pr.bottom-(r.bottom-r.top))/2,r.right-r.left,r.bottom-r.top,parent,nullptr,c.hInstance,this);
        if(!hwnd)return false;EnableWindow(parent,FALSE);ShowWindow(hwnd,SW_SHOW);if(edit)SetFocus(edit);
        MSG msg;while(!done&&GetMessageW(&msg,nullptr,0,0)>0){if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE){DestroyWindow(hwnd);continue;}if(!IsDialogMessageW(hwnd,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        EnableWindow(parent,TRUE);SetForegroundWindow(parent);if(font)DeleteObject(font);if(bg)DeleteObject(bg);return accepted;
    }
};
bool App::confirm(const std::vector<uint32_t>& ids,bool permanent) {
    ConfirmDialog d;d.permanent=permanent;uint64_t bytes=0,files=0,folders=0;
    for(size_t i=0;i<ids.size();++i){auto id=ids[i];auto& n=result->nodes[id];bytes+=n.total;files+=n.directory?n.files:1;folders+=n.directory?n.folders+1:0;if(i<4096){if(!d.path.empty())d.path+=L"\r\n";d.path+=result->path(id);}}
    if(ids.size()>4096)d.path+=L"\r\n… et "+countText(ids.size()-4096)+L" autres éléments sélectionnés.";
    d.summary=countText(ids.size())+L" éléments · "+sizeText(bytes)+L" · "+countText(files)+L" fichiers · "+countText(folders)+L" dossiers (dernier scan)";
    if(permanent)d.summary+=L"\nSuppression parallèle : "+std::to_wstring(workers)+L" workers.";
    return d.show(hwnd);
}
std::wstring selectFolder(HWND owner,const wchar_t* title) {
    std::wstring path;IFileOpenDialog* dialog=nullptr;
    if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
        DWORD flags=0;dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);dialog->SetTitle(title);
        if(SUCCEEDED(dialog->Show(owner))) {IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))) {PWSTR p=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p))){path=p;CoTaskMemFree(p);}item->Release();}}
        dialog->Release();
    }return path;
}
void App::transfer(const std::vector<std::wstring>& sources,const std::wstring& destination,bool move) {
    if(busy||sources.empty())return;
    std::wstring detail=(move?L"Déplacer ":L"Copier ")+countText(sources.size())+L" élément(s) vers :\n\n"+destination+L"\n\n";
    for(size_t i=0;i<std::min<size_t>(sources.size(),4);++i)detail+=sources[i]+L"\n";
    detail+=L"\nLes noms en conflit seront renommés pour préserver les éléments existants.";
    if(move)detail+=L"\nEntre deux disques, les sources ne sont retirées qu’après la copie correspondante.";
    if(MessageBoxW(hwnd,detail.c_str(),move?L"Confirmer le déplacement":L"Confirmer la copie",MB_OKCANCEL|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDOK)return;
    if(worker.joinable())worker.join();
    viewer.cancel();indexing=false;completed.reset();busy=true;transferBusy=true;zipBusy=false;deleteBusy=false;pendingCutSequence=0;operationLabel=move?L"Déplacement Windows":L"Copie Windows";deleting=std::make_shared<DeleteProgress>();auto p=deleting;auto index=result;auto count=workers;progress=std::make_shared<Progress>();auto cacheProgress=progress;
    worker=std::thread([this,sources,destination,move,p,index,count,cacheProgress]{try{deleteResult=shellOperation(hwnd,sources,destination,move,false,*p);}catch(const std::exception&){deleteResult={};deleteResult.errors=1;deleteResult.message=L"L’opération de fichiers a échoué.";}
        try{CacheChanges changes;changes.parents.push_back(destination);if(move){changes.changed=sources;for(auto& path:sources)changes.parents.push_back(std::filesystem::path(path).parent_path().wstring());}completed=refreshCache(index,changes,*cacheProgress,count);}catch(...){deleteResult.message+=L"\nF5 pour actualiser l’index.";}PostMessageW(hwnd,transferDone,0,0);});invalidate();
}
void App::paste(){if(busy||!result||result->nodes.empty())return;bool cut=false;auto sources=clipboardFiles(hwnd,cut);if(sources.empty()){message=L"Aucun fichier dans le presse-papiers.";invalidate();return;}auto sequence=GetClipboardSequenceNumber();transfer(sources,result->path(current),cut);if(cut&&busy&&transferBusy)pendingCutSequence=sequence;}
void App::transferTo(bool move){if(busy||!result||selections.ids.empty())return;auto dest=selectFolder(hwnd,move?L"Déplacer vers ce dossier":L"Copier vers ce dossier");if(!dest.empty())transfer(selectedPaths(),dest,move);}
void App::finishTransfer(){if(worker.joinable())worker.join();busy=false;transferBusy=false;
    if(pendingCutSequence&&!deleteResult.errors&&!deleteResult.cancelled&&OpenClipboard(hwnd)){if(GetClipboardSequenceNumber()==pendingCutSequence)EmptyClipboard();CloseClipboard();}pendingCutSequence=0;
    std::wstring s=deleteResult.cancelled?L"Opération interrompue. Les éléments déjà copiés ou déplacés restent en place.":deleteResult.errors?L"L’opération n’a pas entièrement abouti.":L"Opération terminée.";
    if(!deleteResult.message.empty())s+=L"\n\n"+deleteResult.message;if(deleteResult.errors)MessageBoxW(hwnd,s.c_str(),L"ScanFolder · fichiers",MB_OK|MB_ICONWARNING);message=s;applyCache();}
void App::archive(){
    if(busy||!result||selection==none||selections.ids.size()!=1)return;auto source=result->path(selection);auto sourcePath=std::filesystem::path(source);auto proposed=sourcePath.parent_path()/(sourcePath.filename().wstring()+L".zip");
    wchar_t file[32768]{};wcsncpy_s(file,proposed.c_str(),_TRUNCATE);OPENFILENAMEW ofn{sizeof ofn};ofn.hwndOwner=hwnd;ofn.lpstrFile=file;ofn.nMaxFile=32768;ofn.lpstrFilter=L"Archive ZIP\0*.zip\0";ofn.lpstrDefExt=L"zip";ofn.lpstrTitle=L"Créer un ZIP rapide · source conservée";ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&ofn))return;std::wstring output=file;bool replace=GetFileAttributesW(extended(output).c_str())!=INVALID_FILE_ATTRIBUTES;
    if(worker.joinable())worker.join();viewer.cancel();indexing=false;completed.reset();busy=true;zipBusy=true;transferBusy=false;deleteBusy=false;operationLabel=L"ZIP rapide · compression et vérification";deleting=std::make_shared<DeleteProgress>();auto p=deleting;auto n=workers;auto index=result;progress=std::make_shared<Progress>();auto cacheProgress=progress;
    worker=std::thread([this,source,output,replace,p,n,index,cacheProgress]{try{archiveResult=createZip(source,output,replace,*p,n,1);}catch(const std::exception&){archiveResult={};archiveResult.message=L"La compression n’a pas abouti.";}
        try{CacheChanges changes;changes.parents.push_back(std::filesystem::path(output).parent_path().wstring());completed=refreshCache(index,changes,*cacheProgress,n);}catch(...){archiveResult.message+=L"\nF5 pour actualiser l’index.";}PostMessageW(hwnd,zipDone,0,0);});invalidate();
}
void App::finishZip(){if(worker.joinable())worker.join();busy=false;zipBusy=false;
    auto text=archiveResult.message;if(archiveResult.success)text+=L"\n\n"+archiveResult.output;
    if(!archiveResult.success||archiveResult.skipped)MessageBoxW(hwnd,text.c_str(),L"ScanFolder · ZIP",MB_OK|MB_ICONWARNING);message=text;applyCache();}
LRESULT CALLBACK windowProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    auto a=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE){a=reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));a->hwnd=h;}
    if(!a)return DefWindowProcW(h,m,w,l);
    switch(m) {
    case WM_CREATE:a->setup(h);return 0;
    case WM_SIZE:if(a->pathEdit)a->layout();return 0;
    case WM_DPICHANGED:{a->dpi=HIWORD(w)/96.f;RECT* r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);if(a->rt)a->rt->SetDpi(96*a->dpi,96*a->dpi);a->theme();a->layout();return 0;}
    case WM_GETMINMAXINFO:{auto p=reinterpret_cast<MINMAXINFO*>(l);p->ptMinTrackSize={LONG(1050*a->dpi),LONG(740*a->dpi)};return 0;}
    case WM_PAINT:a->paint();return 0;case WM_ERASEBKGND:return 1;
    case WM_MOUSEMOVE:{float x=GET_X_LPARAM(l)/a->dpi,y=GET_Y_LPARAM(l)/a->dpi;int hover=-1;for(auto& [id,r]:a->hits)if(r.contains(x,y))hover=id;if(hover!=a->hover){a->hover=hover;a->invalidate();}return 0;}
    case WM_LBUTTONDOWN:a->click(GET_X_LPARAM(l)/a->dpi,GET_Y_LPARAM(l)/a->dpi,false);return 0;
    case WM_LBUTTONDBLCLK:a->click(GET_X_LPARAM(l)/a->dpi,GET_Y_LPARAM(l)/a->dpi,true);return 0;
    case WM_RBUTTONUP:a->click(GET_X_LPARAM(l)/a->dpi,GET_Y_LPARAM(l)/a->dpi,false,true);return 0;
    case WM_XBUTTONDOWN:return TRUE;
    case WM_XBUTTONUP:a->travel(GET_XBUTTON_WPARAM(w)==XBUTTON2);return TRUE;
    case WM_APPCOMMAND:{auto command=GET_APPCOMMAND_LPARAM(l);if(command==APPCOMMAND_BROWSER_BACKWARD||command==APPCOMMAND_BROWSER_FORWARD){a->travel(command==APPCOMMAND_BROWSER_FORWARD);return TRUE;}break;}
    case WM_SYSKEYDOWN:if(w==VK_LEFT||w==VK_RIGHT||w==VK_UP){if(w==VK_UP)a->parent();else a->travel(w==VK_RIGHT);return 0;}break;
    case WM_MOUSEWHEEL:{int steps=GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA;long long s=long long(a->scroll)-steps*3;a->scroll=size_t(std::max(0LL,s));a->clampScroll();a->changedView();a->invalidate();return 0;}
    case WM_KEYDOWN:a->key(w);return 0;
    case WM_COMMAND:if(LOWORD(w)==101&&HIWORD(w)==EN_CHANGE){if(!a->suppressViewChange)SetTimer(h,2,240,nullptr);return 0;}break;
    case WM_TIMER:if(w==1){if(a->busy||a->indexing)a->invalidate();}else if(w==2){KillTimer(h,2);if(!a->busy)a->buildRows();}else if(w==3){KillTimer(h,3);if(!a->busy)a->remember();}return 0;
    case WM_DROPFILES:{if(!a->busy){wchar_t path[32768]{};DragQueryFileW(reinterpret_cast<HDROP>(w),0,path,32768);DWORD attributes=GetFileAttributesW(path);if(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY))a->openLocation(std::filesystem::path(path).parent_path().wstring());else a->openLocation(path);}DragFinish(reinterpret_cast<HDROP>(w));return 0;}
    case WM_CTLCOLOREDIT:{auto dc=reinterpret_cast<HDC>(w);auto p=a->palette;SetTextColor(dc,RGB((p.text>>16)&255,(p.text>>8)&255,p.text&255));SetBkColor(dc,RGB((p.field>>16)&255,(p.field>>8)&255,p.field&255));return reinterpret_cast<LRESULT>(a->editBrush);}
    case scanDone:a->finishScan();return 0;case deleteDone:a->finishDelete();return 0;case viewDone:a->finishView();return 0;case transferDone:a->finishTransfer();return 0;case zipDone:a->finishZip();return 0;
    case cacheLoaded:a->finishLoad();return 0;
    case cacheWritten:{auto error=a->disk.lastError();if(!error.empty())a->message=error;a->invalidate();return 0;}
    case WM_CLOSE:
        if(a->busy){if(MessageBoxW(h,L"Une opération est en cours. L’arrêter et fermer l’application ?\nLes éléments déjà supprimés définitivement restent supprimés.",L"ScanFolder",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES)return 0;if(a->progress)a->progress->cancel=true;if(a->deleting)a->deleting->cancel=true;}
        a->remember();DestroyWindow(h);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc>=4&&!wcscmp(argv[1],L"--cache-benchmark")){
        Progress p;auto r=scan(argv[2],p,Options{false,4});std::wstring error;auto begin=std::chrono::steady_clock::now();bool saved=saveDatabase(*r,argv[3],error);auto saveSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();auto loaded=loadDatabase(argv[3],error);bool valid=loaded&&!r->nodes.empty()&&loaded->nodes[0].total==r->nodes[0].total&&loaded->nodes[0].files==r->nodes[0].files;
        std::ofstream report(std::filesystem::path(std::wstring(argv[3])+L".txt"));report<<"scan_seconds="<<r->seconds<<"\nfiles="<<(r->nodes.empty()?0:r->nodes[0].files)<<"\nfolders="<<(r->nodes.empty()?0:r->nodes[0].folders)<<"\ndb_save_seconds="<<saveSeconds<<"\ndb_load_seconds="<<(loaded?loaded->seconds:0)<<"\ndb_bytes="<<(saved?std::filesystem::file_size(argv[3]):0)<<"\nverified="<<valid<<"\n";LocalFree(argv);return saved&&valid?0:1;
    }
    if(argc>=3&&wcscmp(argv[1],L"--third-party")==0){
        auto dir=sf::normalize(argv[2]);CreateDirectoryW(sf::extended(dir).c_str(),nullptr);bool ok=true;
        const wchar_t* names[]={L"7za.exe",L"7zip-LICENSE.txt",L"7z2604-src.tar.xz"};
        for(int i=0;i<3;++i){auto module=GetModuleHandleW(nullptr);auto resource=FindResourceW(module,MAKEINTRESOURCEW(101+i),RT_RCDATA);auto data=resource?LoadResource(module,resource):nullptr;DWORD size=resource?SizeofResource(module,resource):0;auto ptr=data?LockResource(data):nullptr;
            auto path=sf::extended(dir+L"\\"+names[i]);HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);DWORD got=0;
            if(f==INVALID_HANDLE_VALUE){ok=false;continue;}ok=ok&&ptr&&WriteFile(f,ptr,size,&got,nullptr)&&got==size;CloseHandle(f);
        }LocalFree(argv);return ok?0:1;
    }
    if(argc>=2&&wcscmp(argv[1],L"--self-test")==0){auto code=selfTest(argc>=3?argv[2]:L"self-test.txt");LocalFree(argv);return code;}
    if(argc>=3&&wcscmp(argv[1],L"--scan")==0){Progress p;Options o;for(int i=3;i<argc;++i)if(!wcscmp(argv[i],L"--standard"))o.mft=false;
        auto r=sf::scan(argv[2],p,o);std::wstring error;auto csv=argc>=4&&argv[3][0]!=L'-'?argv[3]:L"scan.csv";bool ok=exportCsv(*r,csv,error);
        std::ofstream log(std::filesystem::path(std::wstring(csv)+L".txt"));log<<"method="<<(r->method.find(L"MFT")!=std::wstring::npos?"MFT":"parallel")<<"\nseconds="<<r->seconds<<"\nerrors="<<r->errors<<"\nnodes="<<r->nodes.size()<<"\nfiles="<<(r->nodes.empty()?0:r->nodes[0].files)<<"\nfolders="<<(r->nodes.empty()?0:r->nodes[0].folders)<<"\nlogical_bytes="<<(r->nodes.empty()?0:r->nodes[0].total)<<"\n";
        LocalFree(argv);return !ok||r->nodes.empty()?1:0;}
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    App app;
    wchar_t user[32768]{};GetEnvironmentVariableW(L"USERPROFILE",user,32768);app.initial=user;
    if(argc>=3&&(!wcscmp(argv[1],L"--open")||!wcscmp(argv[1],L"--start")))app.initial=argv[2];
    bool autoStart=argc>=3&&!wcscmp(argv[1],L"--start");app.explicitRoot=argc>=3;LocalFree(argv);
    WNDCLASSEXW c{sizeof c};c.style=CS_DBLCLKS;c.lpfnWndProc=windowProc;c.hInstance=instance;c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    c.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));c.hIconSm=c.hIcon;c.lpszClassName=L"ScanFolderApp";RegisterClassExW(&c);
    UINT dpi=GetDpiForSystem();int width=int(1390*dpi/96.f),height=int(860*dpi/96.f);
    HWND h=CreateWindowExW(WS_EX_ACCEPTFILES,c.lpszClassName,L"ScanFolder · Analyse d’espace disque",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,width,height,nullptr,nullptr,instance,&app);
    if(!h){CoUninitialize();return 1;}ShowWindow(h,show);UpdateWindow(h);if(autoStart)app.beginScan();else app.restoreCache();
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}CoUninitialize();return int(msg.wParam);
}
