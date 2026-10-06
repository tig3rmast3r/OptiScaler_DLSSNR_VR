#pragma once
#include <string_view>

namespace library_search {
// SR/RR are legitimate native game dependencies even when DLSSG output is
// disabled. Keep the OptiScaler FG-bundle exclusion for unrelated libraries.
inline bool accept_streamline_candidate(std::wstring_view library, bool dlssg_output) {
    return dlssg_output || library == L"nvngx_dlss.dll" || library == L"nvngx_dlssd.dll";
}
}
