#pragma once
#include <Windows.h>
#include <cmath>
#include <cwchar>
#include <filesystem>

namespace xrfg {
// Luke stores game settings under a game-specific section in RealVR.ini.
// Read the unique WorldScale key, without a game/executable whitelist.
inline float luke_world_scale_from_ini(const std::filesystem::path& ini) noexcept {
    wchar_t sections[32768]{};
    const DWORD length = GetPrivateProfileSectionNamesW(sections, 32768, ini.c_str());
    if (!length || length >= 32766) return 0.0F;
    float result = 0.0F;
    bool found = false;
    for (const wchar_t* section = sections; *section;
         section += std::wcslen(section) + 1) {
        wchar_t value[64]{};
        const DWORD count = GetPrivateProfileStringW(section, L"WorldScale", L"",
            value, 64, ini.c_str());
        if (!count) continue;
        if (found || count >= 63) return 0.0F;
        wchar_t* end{};
        const float scale = std::wcstof(value, &end);
        if (end == value || *end != L'\0' || !std::isfinite(scale) ||
            scale < 0.2F || scale > 5.0F) return 0.0F;
        result = scale;
        found = true;
    }
    return result;
}
} // namespace xrfg
