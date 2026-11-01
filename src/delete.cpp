#include "delete.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <shlobj.h>
#include <thread>

namespace sf {
namespace {
struct Handle { HANDLE h = INVALID_HANDLE_VALUE; ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
std::wstring finalPath(HANDLE h) {
    DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n) return {};
    std::wstring path(n, 0); n = GetFinalPathNameByHandleW(h, path.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= path.size()) return {}; path.resize(n); while (path.size() > 7 && path.back() == L'\\') path.pop_back(); return path;
}
bool inside(const std::wstring& path, const std::wstring& root) {
    return path.size() >= root.size() && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 &&
        (path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\');
}
void failure(DeleteProgress& p, const std::wstring& path, DWORD code) {
    ++p.errors; std::lock_guard lock(p.mutex);
    if (p.firstError.empty()) p.firstError = path + L"\n" + errorText(code);
}
bool erase(const std::wstring& path, const std::wstring& boundary, DeleteProgress& p, bool directory) {
    if (p.cancel) return false;
    Handle h{CreateFileW(extended(path).c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (h.h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError(); if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) failure(p, path, e); return false;
    }
    auto resolved = finalPath(h.h);
    if (resolved.empty() || !inside(resolved, boundary)) { failure(p, path, ERROR_ACCESS_DENIED); return false; }
    // Delete the object held by this handle, not an independently resolved path. Reparse targets are never opened.
    FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    bool ok = SetFileInformationByHandle(h.h, FileDispositionInfoEx, &disposition, sizeof disposition) != FALSE;
    if (!ok) {
        DWORD code = GetLastError();
        if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION) {
            FILE_BASIC_INFO basic{}; bool changed = false;
            if (GetFileInformationByHandleEx(h.h, FileBasicInfo, &basic, sizeof basic) && (basic.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
                auto writable = basic; writable.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
                if (!writable.FileAttributes) writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                changed = SetFileInformationByHandle(h.h, FileBasicInfo, &writable, sizeof writable) != FALSE;
            }
            FILE_DISPOSITION_INFO legacy{TRUE}; ok = SetFileInformationByHandle(h.h, FileDispositionInfo, &legacy, sizeof legacy) != FALSE;
            code = GetLastError();
            if (!ok && changed) SetFileInformationByHandle(h.h, FileBasicInfo, &basic, sizeof basic);
        }
        if (!ok) failure(p, path, code);
    }
    if (ok) { if (directory) ++p.folders; else ++p.files; }
    return ok;
}
}
bool validateDelete(const std::wstring& input, std::wstring& error) {
    auto path = normalize(input);
    Handle h{CreateFileW(extended(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (h.h == INVALID_HANDLE_VALUE) { error = errorText(GetLastError()); return false; }
    auto resolved = finalPath(h.h);
    if (resolved.empty()) { error = L"Le chemin réel ne peut pas être vérifié."; return false; }
    auto plain = resolved.rfind(L"\\\\?\\UNC\\", 0) == 0 ? L"\\\\" + resolved.substr(8) : resolved.substr(4);
    wchar_t volume[MAX_PATH]{};
    if (GetVolumePathNameW(plain.c_str(), volume, MAX_PATH) && _wcsicmp(normalize(volume).c_str(), plain.c_str()) == 0) {
        error = L"La suppression d'une racine de disque est bloquée."; return false;
    }
    std::vector<std::wstring> protectedPaths;
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH); protectedPaths.push_back(windows);
    const KNOWNFOLDERID* folders[] = {&FOLDERID_Profile, &FOLDERID_UserProfiles, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData};
    for (auto id : folders) { PWSTR p = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &p))) { protectedPaths.push_back(p); CoTaskMemFree(p); } }
    for (auto& p : protectedPaths) {
        auto candidate = normalize(p);
        // Also block system descendants, but permit a selected file/folder within a user's profile.
        if (_wcsicmp(plain.c_str(), candidate.c_str()) == 0 ||
            (_wcsicmp(candidate.c_str(), windows) == 0 && inside(plain, candidate))) {
            error = L"Ce dossier système ou racine de profil est protégé."; return false;
        }
    }
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    if (inside(normalize(exe), plain)) { error = L"Ce dossier contient l'application en cours d'exécution."; return false; }
    return true;
}
DeleteResult deletePermanently(const std::wstring& input, DeleteProgress& p, unsigned workers) {
    auto start = std::chrono::steady_clock::now(); DeleteResult result;
    std::wstring error;
    if (!validateDelete(input, error)) { result.errors = 1; result.message = error; return result; }
    auto target = normalize(input);
    Handle rootHandle{CreateFileW(extended(target).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    auto boundary = finalPath(rootHandle.h);
    if (boundary.empty()) { result.errors = 1; result.message = L"Chemin réel inaccessible."; return result; }
    BY_HANDLE_FILE_INFORMATION rootInfo{};
    if (!GetFileInformationByHandle(rootHandle.h, &rootInfo)) { result.errors = 1; result.message = errorText(GetLastError()); return result; }
    CloseHandle(rootHandle.h); rootHandle.h = INVALID_HANDLE_VALUE;
    if (!(rootInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (rootInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        erase(target, boundary, p, (rootInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } else {
        struct Job { std::wstring path; bool directory; };
        std::deque<Job> queue; std::mutex mutex; std::condition_variable cv;
        bool finished = false;
        std::exception_ptr fatal;
        unsigned nWorkers = std::clamp(workers, 1u, 8u);
        auto worker = [&] {
            for (;;) {
                Job job;
                { std::unique_lock lock(mutex); cv.wait(lock, [&] { return finished || p.cancel || !queue.empty(); });
                    if (p.cancel || (finished && queue.empty())) return;
                    job = std::move(queue.front()); queue.pop_front(); cv.notify_all(); }
                erase(job.path, boundary, p, job.directory);
            }
        };
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < nWorkers; ++i) pool.emplace_back([&]{try{worker();}catch(...){std::lock_guard lock(mutex);if(!fatal)fatal=std::current_exception();p.cancel=true;cv.notify_all();}});
        auto enqueue = [&](Job job) {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return queue.size() < 8192 || p.cancel; });
            if (!p.cancel) queue.push_back(std::move(job)); cv.notify_all();
        };
        struct Dir { std::wstring path; unsigned depth; };
        std::vector<Dir> pending{{target, 0}}, directories;
        try { while (!pending.empty() && !p.cancel) {
            Dir dir = std::move(pending.back()); pending.pop_back();
            // Recheck every directory handle and canonical boundary before enumeration.
            Handle h{CreateFileW(extended(dir.path).c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            BY_HANDLE_FILE_INFORMATION info{};
            if (h.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h.h, &info)) { failure(p, dir.path, GetLastError()); continue; }
            if (!inside(finalPath(h.h), boundary)) { failure(p, dir.path, ERROR_ACCESS_DENIED); continue; }
            if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { CloseHandle(h.h); h.h = INVALID_HANDLE_VALUE; enqueue({dir.path, true}); continue; }
            directories.push_back(dir);
            { std::lock_guard lock(p.mutex); p.current = dir.path; }
            auto mask = extended(dir.path) + (dir.path.back() == L'\\' ? L"*" : L"\\*");
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileExW(mask.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) { if (GetLastError() != ERROR_FILE_NOT_FOUND) failure(p, dir.path, GetLastError()); continue; }
            do {
                if (p.cancel) break;
                if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
                auto path = dir.path + (dir.path.back() == L'\\' ? L"" : L"\\") + data.cFileName;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back({std::move(path), dir.depth + 1});
                else enqueue({std::move(path), (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
            } while (FindNextFileW(find, &data));
            DWORD e = GetLastError(); FindClose(find);
            if (e != ERROR_NO_MORE_FILES && !p.cancel) failure(p, dir.path, e);
        } } catch(...) { std::lock_guard lock(mutex);if(!fatal)fatal=std::current_exception();p.cancel=true; }
        { std::lock_guard lock(mutex); finished = true; } cv.notify_all();
        for (auto& t : pool) t.join();
        if(fatal)std::rethrow_exception(fatal);
        // Children before parents; directories on the same depth are independent and removed in parallel.
        std::sort(directories.begin(), directories.end(), [](auto& a, auto& b) { return a.depth > b.depth; });
        for (size_t begin = 0; begin < directories.size() && !p.cancel; ) {
            size_t end = begin + 1; while (end < directories.size() && directories[end].depth == directories[begin].depth) ++end;
            std::atomic<size_t> next{begin}; pool.clear();
            for (unsigned k = 0; k < nWorkers; ++k) pool.emplace_back([&] {
                try{for (;;) { auto i = next.fetch_add(1); if (i >= end || p.cancel) break; erase(directories[i].path, boundary, p, true); }}catch(...){std::lock_guard lock(mutex);if(!fatal)fatal=std::current_exception();p.cancel=true;}
            });
            for (auto& t : pool) t.join(); begin = end;
            if(fatal)std::rethrow_exception(fatal);
        }
    }
    result.files = p.files; result.folders = p.folders; result.errors = p.errors; result.cancelled = p.cancel;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    { std::lock_guard lock(p.mutex); result.message = p.firstError; }
    return result;
}
DeleteResult deleteBatch(const std::vector<std::wstring>& paths,DeleteProgress& progress,unsigned workers){
    auto start=std::chrono::steady_clock::now();DeleteResult result;
    // Validate the entire batch before touching the first element.
    for(auto& path:paths){std::wstring error;if(!validateDelete(path,error)){result.errors=1;result.message=path+L"\n"+error;return result;}}
    for(auto& path:paths){if(progress.cancel)break;auto item=deletePermanently(path,progress,workers);if(item.errors>progress.errors.load())progress.errors=item.errors;if(!item.message.empty())result.message=item.message;}
    result.files=progress.files;result.folders=progress.folders;result.errors=progress.errors;result.cancelled=progress.cancel;result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();return result;
}
}
