#pragma once
#include "scan.hpp"
#include <algorithm>
#include <optional>
#include <unordered_set>

namespace sf {
inline std::atomic<uint64_t> selectionVersion{1};
struct SelectionState {
    uint64_t version=selectionVersion++;
    std::unordered_set<uint32_t> ids;
    uint32_t focus = none, anchor = none;
    void clear() { ids.clear(); focus = anchor = none;version=selectionVersion++; }
    void select(const std::vector<uint32_t>& rows, size_t position, bool ctrl, bool shift, bool context = false) {
        if (position >= rows.size()) return;
        version=selectionVersion++;
        auto id = rows[position];
        if (context && ids.contains(id)) { focus = id; return; }
        if (shift) {
            auto first = std::find(rows.begin(), rows.end(), anchor);
            size_t from = first == rows.end() ? position : size_t(first - rows.begin());
            if (!ctrl) ids.clear();
            for (size_t i = std::min(from, position); i <= std::max(from, position); ++i) ids.insert(rows[i]);
            if (anchor == none || first == rows.end()) anchor = id;
        } else {
            if (!ctrl || context) ids.clear();
            if (ctrl && !context && ids.contains(id)) ids.erase(id); else ids.insert(id);
            anchor = id;
        }
        focus = id;
    }
    void all(const std::vector<uint32_t>& rows) { version=selectionVersion++;ids.clear(); ids.insert(rows.begin(), rows.end()); if (!rows.empty()) { focus = rows.front(); anchor = focus; } else clear(); }
    void retain(const std::vector<uint32_t>& rows) {
        version=selectionVersion++;
        if(ids.empty()){focus=anchor=none;return;}
        std::unordered_set<uint32_t> visible(rows.begin(), rows.end());
        for (auto it = ids.begin(); it != ids.end();) if (!visible.contains(*it)) it = ids.erase(it); else ++it;
        if (!visible.contains(focus) || !ids.contains(focus)) focus = ids.empty() ? none : *ids.begin();
        if (!visible.contains(anchor)) anchor = focus;
    }
};
struct Location {
    uint32_t directory = 0;
    size_t scroll = 0;
    int tab = 0, sort = 1;
    bool descending = true, allocated = false;
    std::wstring query, extension;
    SelectionState selection;
};
class NavigationHistory {
    std::vector<Location> before, after;
public:
    void clear() { before.clear(); after.clear(); }
    bool canBack() const { return !before.empty(); }
    bool canForward() const { return !after.empty(); }
    void visit(Location from) { before.push_back(std::move(from)); after.clear(); }
    std::optional<Location> back(Location from) {
        if (before.empty()) return {}; after.push_back(std::move(from)); auto to = std::move(before.back()); before.pop_back(); return to;
    }
    std::optional<Location> forward(Location from) {
        if (after.empty()) return {}; before.push_back(std::move(from)); auto to = std::move(after.back()); after.pop_back(); return to;
    }
};
// A parent and a selected descendant must be acted on only once.
inline std::vector<uint32_t> selectedRoots(const Result& result, const SelectionState& selection) {
    std::vector<uint32_t> out;
    for (auto id : selection.ids) {
        if (id >= result.nodes.size() || result.nodes[id].removed) continue;
        auto parent = result.nodes[id].parent; bool covered = false;
        while (parent != none) { if (selection.ids.contains(parent)) { covered = true; break; } parent = result.nodes[parent].parent; }
        if (!covered) out.push_back(id);
    }
    std::sort(out.begin(), out.end()); return out;
}
inline bool interactionTest() {
    std::vector<uint32_t> rows{7, 3, 9, 4, 2}; SelectionState s;
    s.select(rows, 1, false, false); s.select(rows, 3, true, false);
    if (s.ids.size() != 2 || !s.ids.contains(3) || !s.ids.contains(4)) return false;
    s.select(rows, 3, false, false, true); if (s.ids.size() != 2) return false;
    s.select(rows, 1, true, false); if (s.ids.contains(3) || s.ids.size() != 1) return false;
    s.select(rows, 4, false, true); if (s.ids.size() != 4 || !s.ids.contains(3) || !s.ids.contains(2)) return false;
    s.select(rows, 0, true, true); if (s.ids.size() != 5) return false;
    s.retain({3, 4}); if (s.ids.size() != 2 || !s.ids.contains(4)) return false;
    NavigationHistory h; Location a; a.directory = 7; a.scroll = 31; a.query = L"test"; a.selection = s;
    Location b; b.directory = 9; h.visit(a); auto back = h.back(b);
    if (!back || back->directory != 7 || back->scroll != 31 || back->query != L"test" || back->selection.ids.size() != 2) return false;
    auto forward = h.forward(*back); if (!forward || forward->directory != 9) return false;
    h.visit(*forward); if (h.canForward()) return false;
    Result result; result.nodes.resize(4); result.nodes[0].parent = none; result.nodes[1].parent = 0; result.nodes[2].parent = 1; result.nodes[3].parent = 0;
    s.clear(); s.ids = {1, 2, 3}; auto roots = selectedRoots(result, s);
    return roots == std::vector<uint32_t>({1, 3});
}
}
