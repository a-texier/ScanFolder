#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sf {
constexpr uint32_t none = UINT32_MAX;
struct Node {
    std::wstring name;
    uint64_t size = 0, allocated = 0, modified = 0;
    uint64_t total = 0, totalAllocated = 0;
    uint32_t parent = none, firstChild = none, nextSibling = none;
    uint32_t files = 0, folders = 0;
    bool directory = false, reparse = false, removed = false;
};
struct Result {
    std::wstring root, method, note;
    std::vector<Node> nodes;
    uint64_t errors = 0, skippedLinks = 0;
    double seconds = 0;
    bool cancelled = false;
    std::wstring path(uint32_t index) const;
    void aggregate();
    std::vector<uint32_t> children(uint32_t index) const;
};
struct Progress {
    std::atomic<uint64_t> files{0}, folders{0}, bytes{0}, errors{0}, records{0};
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::wstring phase = L"Préparation…", current;
    void status(const std::wstring& text, const std::wstring& path = L"");
};
struct Options { bool mft = true; unsigned workers = 4; };
std::shared_ptr<Result> scan(const std::wstring&, Progress&, const Options& = {});
bool scanMft(const std::wstring&, Progress&, Result&);
std::wstring normalize(const std::wstring&);
std::wstring extended(const std::wstring&);
std::wstring sizeText(uint64_t);
std::wstring countText(uint64_t);
std::wstring errorText(DWORD);
bool exportCsv(const Result&, const std::wstring&, std::wstring&);
int selfTest(const std::wstring& output);
bool mftParserTest();
}
