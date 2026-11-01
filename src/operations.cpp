#include "operations.hpp"
#include <shlobj.h>
#include <shellapi.h>
#include <chrono>
namespace sf {
namespace {
bool allowDelete(bool recycleOnly, DWORD flags, bool cancelled) { return !cancelled && (!recycleOnly || (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)); }
class Sink final : public IFileOperationProgressSink {
    LONG refs = 1;
public:
    DeleteProgress& p; bool recycleOnly; HRESULT outcome = S_OK;
    Sink(DeleteProgress& progress, bool recycle) : p(progress), recycleOnly(recycle) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id == IID_IUnknown || id == IID_IFileOperationProgressSink) { *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK; } return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    HRESULT state(IShellItem* item = nullptr) {
        if (p.cancel) return E_ABORT;
        if (item) { PWSTR path = nullptr; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { std::lock_guard lock(p.mutex); p.current = path; CoTaskMemFree(path); } }
        return S_OK;
    }
    HRESULT after(IShellItem* item, HRESULT hr) {
        if (FAILED(hr)) { ++p.errors; std::lock_guard lock(p.mutex); if (p.firstError.empty()) p.firstError = L"Échec d’une opération de fichiers : " + errorText(HRESULT_CODE(hr)); }
        else { SFGAOF attributes = 0; if (item && SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)) && (attributes & SFGAO_FOLDER)) ++p.folders; else ++p.files; }
        return state();
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return state(); }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT hr) override { outcome = hr; return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem* i, LPCWSTR) override { return state(i); }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem* i, LPCWSTR, HRESULT h, IShellItem*) override { return after(i, h); }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem* i, IShellItem*, LPCWSTR) override { return state(i); }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem* i, IShellItem*, LPCWSTR, HRESULT h, IShellItem*) override { return after(i, h); }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem* i, IShellItem*, LPCWSTR) override { return state(i); }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem* i, IShellItem*, LPCWSTR, HRESULT h, IShellItem*) override { return after(i, h); }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem* item) override {
        if (!allowDelete(recycleOnly, flags, p.cancel)) { if (!p.cancel) { ++p.errors; std::lock_guard lock(p.mutex); p.firstError = L"Cet élément ne peut pas être mis à la corbeille. Aucune suppression définitive n’a été autorisée."; } return E_ABORT; }
        return state(item);
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem* i, HRESULT h, IShellItem*) override { return after(i, h); }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return state(); }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT h, IShellItem* i) override { return after(i, h); }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return state(); }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};
}
DeleteResult shellOperation(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destination, bool move, bool recycleOnly, DeleteProgress& p) {
    auto start = std::chrono::steady_clock::now(); DeleteResult result;
    auto co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if(move&&!recycleOnly){auto dest=normalize(destination);for(auto& source:sources){auto src=normalize(source);auto split=src.find_last_of(L'\\');if(split!=std::wstring::npos&&_wcsicmp(src.substr(0,split).c_str(),dest.c_str())==0){result.message=L"Cet élément se trouve déjà dans le dossier de destination.";result.errors=1;if(SUCCEEDED(co))CoUninitialize();return result;}}}
    IFileOperation* op = nullptr; IShellItem* dest = nullptr; auto sink = new Sink(p, recycleOnly);
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&op));
    if (SUCCEEDED(hr)) {
        op->SetOwnerWindow(owner);
        DWORD flags = FOFX_EARLYFAILURE | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR;
        if (recycleOnly) flags |= FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD | FOF_NOCONFIRMATION;
        else flags |= FOF_RENAMEONCOLLISION; // Never silently overwrite an existing destination.
        hr = op->SetOperationFlags(flags);
        if (SUCCEEDED(hr) && !recycleOnly) hr = SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&dest));
        for (auto& path : sources) {
            if (FAILED(hr) || p.cancel) break;
            if (move || recycleOnly) { std::wstring error; if (!validateDelete(path, error)) { hr = E_ACCESSDENIED; std::lock_guard lock(p.mutex);p.firstError = error; break; } }
            IShellItem* item = nullptr; hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
            if (SUCCEEDED(hr)) {
                hr = recycleOnly ? op->DeleteItem(item, sink) : move ? op->MoveItem(item, dest, nullptr, sink) : op->CopyItem(item, dest, nullptr, sink);
                item->Release();
            }
        }
        if (SUCCEEDED(hr) && !p.cancel) hr = op->PerformOperations();
        BOOL aborted = FALSE; op->GetAnyOperationsAborted(&aborted); if (aborted && SUCCEEDED(hr)) hr = E_ABORT;
        if (FAILED(sink->outcome) && SUCCEEDED(hr)) hr = sink->outcome;
        op->Release();
    }
    if (dest) dest->Release(); sink->Release();
    result.cancelled = p.cancel; result.errors = p.errors; result.files = p.files; result.folders = p.folders;
    { std::lock_guard lock(p.mutex); result.message = p.firstError; }
    if (FAILED(hr)) { if (!result.errors) ++result.errors; if (result.message.empty()) result.message = L"L’opération Windows n’a pas abouti : " + errorText(HRESULT_CODE(hr)); }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (SUCCEEDED(co)) CoUninitialize(); return result;
}
bool putFilesOnClipboard(HWND owner, const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty() || !OpenClipboard(owner)) return false;
    size_t chars = 1; for (auto& p : paths) chars += p.size() + 1;
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!data || !effect) { if (data) GlobalFree(data); if (effect) GlobalFree(effect); CloseClipboard(); return false; }
    auto drop = static_cast<DROPFILES*>(GlobalLock(data)); auto fx = static_cast<DWORD*>(GlobalLock(effect));
    if (!drop || !fx) { if (drop) GlobalUnlock(data); if (fx) GlobalUnlock(effect); GlobalFree(data); GlobalFree(effect); CloseClipboard(); return false; }
    drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE; auto text = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
    for (auto& p : paths) { memcpy(text, p.c_str(), (p.size() + 1) * sizeof(wchar_t)); text += p.size() + 1; }
    *fx = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY; GlobalUnlock(data); GlobalUnlock(effect);
    EmptyClipboard(); bool ok = SetClipboardData(CF_HDROP, data) != nullptr;
    if (!ok) GlobalFree(data);
    if (!SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect)) GlobalFree(effect);
    CloseClipboard(); return ok;
}
std::vector<std::wstring> clipboardFiles(HWND owner, bool& cut) {
    std::vector<std::wstring> paths; cut = false;
    if (!OpenClipboard(owner)) return paths;
    auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (drop) {
        UINT n = DragQueryFileW(drop, UINT_MAX, nullptr, 0);
        for (UINT i = 0; i < n && i < 100000; ++i) { UINT size = DragQueryFileW(drop, i, nullptr, 0); std::wstring p(size + 1, 0); DragQueryFileW(drop, i, p.data(), size + 1); p.resize(size); paths.push_back(std::move(p)); }
    }
    auto effect = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    if (effect && GlobalSize(effect)>=sizeof(DWORD)) { auto value = static_cast<const DWORD*>(GlobalLock(effect)); if (value) { cut = (*value & DROPEFFECT_MOVE) != 0; GlobalUnlock(effect); } }
    CloseClipboard(); return paths;
}
bool operationPolicyTest() {
    return !allowDelete(true, 0, false) && allowDelete(true, TSF_DELETE_RECYCLE_IF_POSSIBLE, false) &&
        !allowDelete(true, TSF_DELETE_RECYCLE_IF_POSSIBLE, true) && allowDelete(false, 0, false);
}
}
