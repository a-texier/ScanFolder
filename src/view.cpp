#include "view.hpp"
#include <algorithm>
#include <unordered_map>
namespace sf {
namespace {
std::wstring lowercase(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size())); return s; }
struct Cancelled {};
ViewOutput build(const ViewRequest& q, const std::atomic<uint64_t>& generation) {
    ViewOutput out; out.generation = q.generation;
    if (!q.result || q.result->nodes.empty()) return out;
    auto& result = *q.result; auto query = lowercase(q.query);
    auto value = [&](const Node& n) { return q.allocated ? n.totalAllocated : n.total; };
    auto check = [&] { if (generation != q.generation) throw Cancelled{}; };
    std::vector<uint32_t> stack;
    if (q.tab == 0) stack = result.children(q.current); else stack.push_back(q.current);
    std::unordered_map<std::wstring, FileType> types;
    uint64_t visited = 0;
    while (!stack.empty()) {
        auto i = stack.back(); stack.pop_back(); if ((++visited & 1023) == 0) check();
        auto& n = result.nodes[i]; if (n.removed) continue;
        if (q.tab && n.directory) { for (auto c = n.firstChild; c != none; c = result.nodes[c].nextSibling) stack.push_back(c); continue; }
        if (q.tab == 2) {
            auto dot = n.name.find_last_of(L'.'); auto ext = dot == std::wstring::npos ? L"Sans extension" : lowercase(n.name.substr(dot));
            auto& t = types[ext]; t.name = ext; t.bytes += value(n); ++t.count;
        } else {
            bool match = true;
            if (!query.empty() || !q.extension.empty()) {
                auto name = lowercase(n.name); if (!query.empty()) match = name.find(query) != std::wstring::npos;
                if (q.extension == L"<none>") match &= name.find_last_of(L'.') == std::wstring::npos;
                else if (!q.extension.empty()) match &= name.size() >= q.extension.size() && name.substr(name.size() - q.extension.size()) == q.extension;
            }
            if (match) out.rows.push_back(i);
        }
    }
    if (q.tab == 2) {
        for (auto& [name, type] : types) if (query.empty() || lowercase(name).find(query) != std::wstring::npos) out.types.push_back(std::move(type));
        std::sort(out.types.begin(), out.types.end(), [&](auto& a, auto& b) {
            if ((++visited & 16383) == 0) check();
            if (q.sort == 0) return q.descending ? a.name > b.name : a.name < b.name;
            uint64_t x = q.sort == 2 ? a.count : a.bytes, y = q.sort == 2 ? b.count : b.bytes;
            if (x == y) return a.name < b.name; return q.descending ? x > y : x < y;
        });
    } else std::sort(out.rows.begin(), out.rows.end(), [&](uint32_t a, uint32_t b) {
        if ((++visited & 16383) == 0) check();
        auto& x = result.nodes[a]; auto& y = result.nodes[b]; int cmp = 0;
        if (!q.sort) cmp = _wcsicmp(x.name.c_str(), y.name.c_str());
        else { auto vx = q.sort == 1 ? value(x) : q.sort == 2 ? x.files : x.modified, vy = q.sort == 1 ? value(y) : q.sort == 2 ? y.files : y.modified; cmp = vx < vy ? -1 : vx > vy ? 1 : 0; }
        if (!cmp) return a < b; return q.descending ? cmp > 0 : cmp < 0;
    });
    check(); return out;
}
}
ViewWorker::~ViewWorker() { shutdown(); }
void ViewWorker::shutdown() { { std::lock_guard lock(mutex); stopping = true; ++generation; } cv.notify_all(); if (thread.joinable()) thread.join(); }
void ViewWorker::cancel() { ++generation; std::lock_guard lock(mutex); pending.reset(); ready.reset(); }
uint64_t ViewWorker::submit(ViewRequest request, std::function<void()> callback) {
    std::lock_guard lock(mutex); request.generation = ++generation; auto id = request.generation;
    pending = std::move(request); ready.reset(); notify = std::move(callback);
    if (!thread.joinable()) thread = std::thread([this] { loop(); }); cv.notify_all(); return id;
}
std::optional<ViewOutput> ViewWorker::take() { std::lock_guard lock(mutex); auto out = std::move(ready); ready.reset(); return out; }
void ViewWorker::loop() {
    for (;;) {
        ViewRequest request; std::function<void()> callback;
        { std::unique_lock lock(mutex); cv.wait(lock, [&] { return stopping || pending.has_value(); }); if (stopping) return; request = std::move(*pending); pending.reset(); callback = notify; }
        try {
            auto out = build(request, generation);
            { std::lock_guard lock(mutex); if (generation != request.generation || stopping) continue; ready = std::move(out); }
            callback();
        } catch (const Cancelled&) {} catch (const std::exception&) {
            { std::lock_guard lock(mutex); if (generation != request.generation || stopping) continue; ready = ViewOutput{{},{},request.generation,L"Mémoire insuffisante pour cette vue."}; }
            callback();
        }
    }
}
bool viewTest() {
    auto result = std::make_shared<Result>(); result->nodes.resize(5);
    result->nodes[0].directory = true; result->nodes[0].firstChild = 1;
    for (uint32_t i = 1; i <= 4; ++i) { auto& n = result->nodes[i]; n.name = i == 4 ? L"noextension" : L"f-" + std::to_wstring(i) + L".bin"; n.parent = 0; n.nextSibling = i < 4 ? i + 1 : none; n.size = i * 10; }
    result->aggregate(); ViewRequest q; q.result = result; q.generation = 1; q.tab = 1;
    std::atomic<uint64_t> generation{1}; auto out = build(q, generation);
    if (out.rows.size() != 4 || out.rows[0] != 4) return false;
    q.query = L"F-2"; out = build(q, generation); if (out.rows.size() != 1 || out.rows[0] != 2) return false;
    q.query.clear(); q.extension = L"<none>"; out = build(q, generation); if (out.rows.size() != 1 || out.rows[0] != 4) return false;
    q.extension.clear(); q.tab = 2; out = build(q, generation); return out.types.size() == 2 && out.types[0].name == L".bin" && out.types[0].bytes == 60;
}
}
