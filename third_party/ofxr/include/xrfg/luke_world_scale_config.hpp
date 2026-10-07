#pragma once
#include "xrfg/luke_pose_camera.hpp"
#include <Windows.h>
#include <cmath>
#include <cwchar>
#include <filesystem>

namespace xrfg {
struct LukeGameCameraConfig {
    float world_scale{};
    LukeProducerCameraAxes axes{LukeProducerCameraAxes::view};
};
// The unique WorldScale-bearing section identifies the active Luke title.
// DOOM supplies right/down/forward rows; its entire input camera basis needs
// conversion to view right/up/back. Other titles retain their accepted rows.
inline LukeGameCameraConfig luke_game_camera_config_from_ini(
    const std::filesystem::path& ini) noexcept {
    wchar_t sections[32768]{};
    const DWORD length = GetPrivateProfileSectionNamesW(sections, 32768, ini.c_str());
    if (!length || length >= 32766) return {};
    LukeGameCameraConfig result{};
    bool found = false;
    for (const wchar_t* section = sections; *section;
         section += std::wcslen(section) + 1) {
        wchar_t value[64]{};
        const DWORD count = GetPrivateProfileStringW(section, L"WorldScale", L"",
            value, 64, ini.c_str());
        if (!count) continue;
        if (found || count >= 63) return {};
        wchar_t* end{};
        const float scale = std::wcstof(value, &end);
        if (end == value || *end != L'\0' || !std::isfinite(scale) ||
            scale < 0.2F || scale > 5.0F) return {};
        result.world_scale = scale;
        result.axes = _wcsicmp(section, L"DOOMTDA") == 0
            ? LukeProducerCameraAxes::right_down_forward
            : LukeProducerCameraAxes::view;
        found = true;
    }
    return result;
}
inline float luke_world_scale_from_ini(const std::filesystem::path& ini) noexcept {
    return luke_game_camera_config_from_ini(ini).world_scale;
}
} // namespace xrfg
