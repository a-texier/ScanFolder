#include "scan.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace sf {
static uint64_t u64(DWORD hi, DWORD lo) { return (uint64_t(hi) << 32) | lo; }
void Progress::status(const std::wstring& s, const std::wstring& p) { std::lock_guard lock(mutex); phase = s; current = p; }
std::wstring errorText(DWORD e) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring s = message ? message : L"Erreur Windows " + std::to_wstring(e);
    if (message) LocalFree(message);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s;
}
std::wstring normalize(const std::wstring& input) {
    auto s = input;
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    DWORD n = GetFullPathNameW(s.c_str(), 0, nullptr, nullptr);
    if (!n) return s;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(s.c_str(), n, full.data(), nullptr); full.resize(n);
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    return full;
}
std::wstring extended(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}
std::wstring Result::path(uint32_t i) const {
    if (i >= nodes.size()) return root;
    std::vector<uint32_t> parts;
    while (i && i != none && parts.size() <= nodes.size()) { parts.push_back(i); i = nodes[i].parent; }
    std::wstring s = root;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) { if (s.back() != L'\\') s += L'\\'; s += nodes[*it].name; }
    return s;
}
std::vector<uint32_t> Result::children(uint32_t i) const {
    std::vector<uint32_t> out;
    if (i >= nodes.size()) return out;
    for (auto c = nodes[i].firstChild; c != none; c = nodes[c].nextSibling)
        if (!nodes[c].removed) out.push_back(c);
    return out;
}
void Result::aggregate() {
    if (nodes.empty()) return;
    // Iterative postorder also works for MFT records, whose parents need not precede children.
    std::vector<uint32_t> order; order.reserve(nodes.size()); order.push_back(0);
    for (size_t p = 0; p < order.size(); ++p) {
        auto& n = nodes[order[p]]; n.total = n.removed ? 0 : n.size; n.totalAllocated = n.removed ? 0 : n.allocated;
        n.files = n.folders = 0;
        if (!n.removed) for (auto c = n.firstChild; c != none; c = nodes[c].nextSibling) order.push_back(c);
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        auto& n = nodes[*it];
        if (n.parent == none || n.removed) continue;
        auto& p = nodes[n.parent]; p.total += n.total; p.totalAllocated += n.totalAllocated;
        p.files += n.directory ? n.files : 1; p.folders += n.directory ? n.folders + 1 : 0;
    }
}
std::wstring countText(uint64_t v) {
    auto s = std::to_wstring(v); for (int p = int(s.size()) - 3; p > 0; p -= 3) s.insert(p, L" "); return s;
}
std::wstring sizeText(uint64_t v) {
    const wchar_t* units[] = {L"o", L"Kio", L"Mio", L"Gio", L"Tio", L"Pio"};
    double x = double(v); int u = 0; while (x >= 1024 && u < 5) { x /= 1024; ++u; }
    wchar_t b[64]; swprintf_s(b, L"%.*f %s", u ? (x < 10 ? 2 : 1) : 0, x, units[u]); return b;
}
static void walk(const std::wstring& root, Progress& progress, Result& result, unsigned workers) {
    struct Task { std::wstring path; uint32_t parent; };
    struct Entry { Node node; std::wstring path; };
    std::deque<Task> queue{{extended(root), 0}};
    std::mutex mutex; std::condition_variable cv;
    std::exception_ptr failure;
    size_t active = 0; bool done = false;
    uint64_t cluster = 4096; DWORD spc = 0, bps = 0, unused1 = 0, unused2 = 0;
    wchar_t volume[MAX_PATH]{};
    if (GetVolumePathNameW(root.c_str(), volume, MAX_PATH) && GetDiskFreeSpaceW(volume, &spc, &bps, &unused1, &unused2)) cluster = uint64_t(spc) * bps;
    result.method = L"Parcours parallèle · " + std::to_wstring(workers) + L" workers";
    progress.status(L"Lecture des dossiers", root);
    auto worker = [&] {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return done || progress.cancel || !queue.empty(); });
                if (done || progress.cancel) { cv.notify_all(); return; }
                task = std::move(queue.front()); queue.pop_front(); ++active;
            }
            progress.status(L"Lecture des dossiers", task.path);
            std::vector<Entry> batch; batch.reserve(128);
            WIN32_FIND_DATAW data{};
            auto mask = task.path + (task.path.back() == L'\\' ? L"*" : L"\\*");
            HANDLE find = FindFirstFileExW(mask.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) {
                if (GetLastError() != ERROR_FILE_NOT_FOUND) ++progress.errors;
            } else {
                DWORD enumerationError = ERROR_NO_MORE_FILES;
                do {
                    if (progress.cancel) break;
                    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                    Entry e; e.node.name = data.cFileName; e.node.parent = task.parent;
                    e.node.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    e.node.reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                    e.node.modified = u64(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
                    if (e.node.directory) ++progress.folders;
                    else {
                        e.node.size = u64(data.nFileSizeHigh, data.nFileSizeLow);
                        e.node.allocated = (e.node.size + cluster - 1) / cluster * cluster;
                        // Reparse files (e.g. OneDrive) and sparse/compressed files: query metadata without reading contents.
                        if (data.dwFileAttributes & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_REPARSE_POINT)) {
                            auto full = task.path + (task.path.back() == L'\\' ? L"" : L"\\") + e.node.name;
                            DWORD hi = 0; SetLastError(0); DWORD lo = GetCompressedFileSizeW(full.c_str(), &hi);
                            if (lo != INVALID_FILE_SIZE || GetLastError() == 0) e.node.allocated = u64(hi, lo);
                        }
                        ++progress.files; progress.bytes += e.node.size;
                    }
                    if (e.node.directory && !e.node.reparse) e.path = task.path + (task.path.back() == L'\\' ? L"" : L"\\") + e.node.name;
                    batch.push_back(std::move(e));
                    if (batch.size() >= 2048) {
                        std::lock_guard lock(mutex);
                        for (auto& entry : batch) {
                            uint32_t idx = uint32_t(result.nodes.size());
                            entry.node.nextSibling = result.nodes[task.parent].firstChild;
                            result.nodes[task.parent].firstChild = idx;
                            if (entry.node.directory && entry.node.reparse) ++result.skippedLinks;
                            result.nodes.push_back(std::move(entry.node));
                            if (!entry.path.empty()) queue.push_back({std::move(entry.path), idx});
                        }
                        batch.clear(); cv.notify_all();
                    }
                } while (FindNextFileW(find, &data) || (enumerationError = GetLastError(), false));
                FindClose(find);
                if (enumerationError != ERROR_NO_MORE_FILES && !progress.cancel) ++progress.errors;
            }
            {
                std::lock_guard lock(mutex);
                for (auto& e : batch) {
                    uint32_t idx = uint32_t(result.nodes.size()); e.node.nextSibling = result.nodes[task.parent].firstChild;
                    result.nodes[task.parent].firstChild = idx;
                    if (e.node.directory && e.node.reparse) ++result.skippedLinks;
                    result.nodes.push_back(std::move(e.node));
                    if (!e.path.empty()) queue.push_back({std::move(e.path), idx});
                }
                if (--active == 0 && queue.empty()) done = true;
            }
            cv.notify_all();
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < workers; ++i) pool.emplace_back([&] { try {worker();}catch(...){std::lock_guard lock(mutex);if(!failure)failure=std::current_exception();progress.cancel=true;done=true;cv.notify_all();} });
    for (auto& t : pool) t.join();
    if(failure)std::rethrow_exception(failure);
}
std::shared_ptr<Result> scan(const std::wstring& input, Progress& p, const Options& options) {
    auto start = std::chrono::steady_clock::now();
    auto r = std::make_shared<Result>(); r->root = normalize(input);
    auto attr = GetFileAttributesW(extended(r->root).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        r->errors = 1; r->note = L"Dossier inaccessible : " + errorText(attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY); return r;
    }
    bool ok = options.mft && scanMft(r->root, p, *r);
    if (!ok && !p.cancel) {
        auto reason = r->note;
        r->nodes.clear(); Node n; n.name = r->root; n.directory = true; r->nodes.push_back(std::move(n));
        p.files = 0; p.folders = 0; p.bytes = 0; p.records = 0; p.errors = 0;
        walk(r->root, p, *r, std::clamp(options.workers, 1u, 8u));
        r->note = reason;
    }
    r->cancelled = p.cancel; r->errors += p.errors;
    p.status(L"Calcul des tailles"); r->aggregate();
    r->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    p.status(r->cancelled ? L"Scan interrompu · résultats partiels" : L"Analyse terminée");
    return r;
}
static std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr); return out;
}
bool exportCsv(const Result& r, const std::wstring& path, std::wstring& error) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) { error = L"Impossible de créer le fichier CSV."; return false; }
    out << "\xEF\xBB\xBF" << "Chemin;Type;Taille_octets;Alloue_octets;Fichiers;Dossiers\r\n";
    for (uint32_t i = 0; i < r.nodes.size(); ++i) {
        const auto& n = r.nodes[i]; if (n.removed) continue;
        auto s = utf8(r.path(i)); std::string escaped;
        for (char c : s) { if (c == '"') escaped += '"'; escaped += c; }
        out << '"' << escaped << '"' << ';' << (n.directory ? "Dossier" : "Fichier") << ';' << n.total << ';' << n.totalAllocated << ';' << n.files << ';' << n.folders << "\r\n";
    }
    if (!out.good()) { error = L"Écriture CSV interrompue."; return false; } return true;
}
}
