#pragma once
#include "scan.hpp"
namespace sf {
struct CacheChanges {
    std::vector<std::wstring> changed;
    std::vector<std::wstring> parents;
};
uint32_t findCached(const Result&, const std::wstring&);
std::shared_ptr<Result> refreshCache(const std::shared_ptr<const Result>&, const CacheChanges&, Progress&, unsigned workers = 4);
}
