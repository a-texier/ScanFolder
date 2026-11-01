#pragma once
#include "delete.hpp"
namespace sf {
// The shell progress sink refuses permanent fallback when recycleOnly is true.
DeleteResult shellOperation(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destination,
    bool move, bool recycleOnly, DeleteProgress& progress);
bool putFilesOnClipboard(HWND, const std::vector<std::wstring>&, bool cut);
std::vector<std::wstring> clipboardFiles(HWND, bool& cut);
bool operationPolicyTest();
}
