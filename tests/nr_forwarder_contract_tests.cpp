#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    // Inspect the new checkpoint artifact's PE exports without running DllMain,
    // loading the NVIDIA model, or initializing a game/driver session.
    const auto module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) return 2;
    unsigned found = 0;
    for (const auto name : {"dlssnr_call_create", "dlssnr_call_evaluate",
        "dlssnr_call_release", "dlssnr_call_set_extras", "dlssnr_call_set_contract",
        "dlssnr_call_set_float_slot", "dlssnr_call_probe_float",
        "dlssnr_call_last_init", "dlssnr_call_last_create"}) {
        if (!GetProcAddress(module, name)) std::cerr << "Missing artifact export: " << name << '\n';
        else ++found;
    }
    FreeLibrary(module);
    if (found != 9) return 3;
    std::cout << "Matched NR forwarder artifact: all 9 host exports present: PASS\n";
}
