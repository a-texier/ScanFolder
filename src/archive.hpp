#pragma once
#include "delete.hpp"
namespace sf {
struct ArchiveResult { bool success = false, cancelled = false; uint64_t files = 0, skipped = 0; double seconds = 0; std::wstring message, output; };
ArchiveResult createZip(const std::wstring& source, const std::wstring& output, bool replace, DeleteProgress&, unsigned workers = 4, unsigned level = 1);
bool archivePolicyTest();
}
