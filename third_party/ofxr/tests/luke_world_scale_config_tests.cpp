#include "xrfg/luke_world_scale_config.hpp"
#include <fstream>
#include <iostream>
int main() {
    const auto ini = std::filesystem::temp_directory_path() /
        (L"ofxr-world-scale-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    const auto write = [&](const char* contents) {
        std::ofstream file(ini); file << contents; file.close();
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    };
    write("[DOOMTDA]\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F,
        "DOOM reads its active Luke scale");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::right_down_forward,
        "DOOM selects right/down/forward decoding for both Up and Forward");
    write("[doomtda]\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F,
        "Luke section matching is case insensitive");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::right_down_forward,
        "DOOM axis selection is case insensitive");
    write("[INDY]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F,
        "Indiana reads its validated scale");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::view,
        "Indiana retains its accepted producer view basis");
    write("[DOOMTDA]\nUnused=1\n[INDY]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F,
        "Unused DOOM section must not override the active Indiana section");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::view,
        "An inactive DOOM section must not select its camera convention");
    write("[GOT]\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F,
        "Ghost-only real configuration must initialize scale");
    write("[HFW]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F,
        "Horizon keeps its existing scale");
    write("[GENERAL]\nEnabled=1\n[UnknownLukeGame]\nWorldScale=0.8\n[RVR]\nQuality=2\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.8F,
        "Any Luke game section is supported without a whitelist");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::view,
        "Unknown Luke titles retain the accepted view convention");
    write("[DOOMTDA]\nWorldScale=1.0\n[INDY]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
        "Two active title sections reject ambiguous scale");
    write("[GOT]\nWorldScale=0.8\n[HFW]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
        "Multiple WorldScale keys reject ambiguous configuration");
    write("[GENERAL]\nEnabled=1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
        "Missing WorldScale rejects instead of assuming a default");
    for (const char* invalid : {"", "nan", "inf", "0.19", "5.01", "1.0garbage"}) {
        write((std::string("[GOT]\nWorldScale=") + invalid + "\n").c_str());
        check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
            "Malformed/out-of-range scale must reject");
    }
    std::filesystem::remove(ini);
    return failures ? 1 : 0;
}
