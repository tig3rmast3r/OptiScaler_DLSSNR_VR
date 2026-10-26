// Run as acs.exe in an isolated test directory. Loads the actual host and actual
// SteamVR DLL, without launching the game or creating an OpenXR session.
#include <Windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstdio>
#include "assetto_startup_addresses.h"
int wmain(int argc, wchar_t** argv) {
    if (argc!=2) return 2;
    HMODULE host=LoadLibraryW(L"version.dll");
    if (!host) return 3;
    auto* vendor=reinterpret_cast<std::atomic<unsigned>*>(reinterpret_cast<char*>(host)+vendorRva);
    Sleep(500);
#ifdef EXPECT_EARLY_DISCOVERY
    if (vendor->load()==0) return 4;
    std::puts("PASS: V33035 performs GPU discovery before any D3D11 device (negative control)");
    return 0;
#else
    auto* pending=reinterpret_cast<std::atomic<bool>*>(reinterpret_cast<char*>(host)+deferredRva);
    if (!pending->load() || vendor->load()!=0) return 5;
    std::puts("PASS: discovery is pending; no GPU capability probe before the device");
    std::fflush(stdout);
    HMODULE steamvr=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!steamvr) { std::printf("SteamVR DLL load failed: %lu\n",GetLastError()); return 6; }
    std::puts("PASS: actual SteamVR DLL initialization returned before GPU discovery");
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    for (int index=0;index<2;++index) {
        ID3D11Device* device=nullptr;
        ID3D11DeviceContext* context=nullptr;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
            levels,2,D3D11_SDK_VERSION,&device,&level,&context);
        if (FAILED(hr) || !device || !context) return 7;
        if (pending->load() || vendor->load()!=0x10de) return 8;
        context->Release(); device->Release();
    }
    std::puts("PASS: real NVIDIA D3D11 devices complete deferred discovery and preserve later creation");
    // Keep loaded modules alive until process exit, as host discovery hooks do.
    return 0;
#endif
}
