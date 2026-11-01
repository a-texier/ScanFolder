#pragma once
#include "scan.hpp"
namespace sf {
struct DeleteProgress {
    std::atomic<uint64_t> files{0}, folders{0}, errors{0};
    std::atomic<unsigned> percent{0};
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::wstring current, firstError;
};
struct DeleteResult { uint64_t files = 0, folders = 0, errors = 0; bool cancelled = false; double seconds = 0; std::wstring message; };
bool validateDelete(const std::wstring&, std::wstring& error);
DeleteResult deletePermanently(const std::wstring&, DeleteProgress&, unsigned workers = 4);
DeleteResult deleteBatch(const std::vector<std::wstring>&,DeleteProgress&,unsigned workers=4);
}
