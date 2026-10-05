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
    write("[GOT]\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F,
        "Ghost-only real configuration must initialize scale");
    write("[HFW]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F,
        "Horizon keeps its existing scale");
    write("[GENERAL]\nEnabled=1\n[UnknownLukeGame]\nWorldScale=0.8\n[RVR]\nQuality=2\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.8F,
        "Any Luke game section is supported without a whitelist");
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
