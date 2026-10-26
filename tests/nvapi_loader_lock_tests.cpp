// Compiles the unchanged shipping LoadNvApi and QueryInterface bodies against
// controlled dependencies. loader_fixture.dll invokes these from real DllMain
// while a second thread owns the GPU discovery mutex, matching Assetto's cycle.
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <cstdio>

#define LOG_FUNC(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define GET_ID(name) name
namespace VendorId { enum Value { Invalid=0, Nvidia=0x10de, AMD=0x1002 }; }
struct GpuInformation { VendorId::Value vendorId; };
std::mutex gpuMutex;
struct IdentifyGpu {
#include "observed_vendor.inc"
    static GpuInformation getPrimaryGpu() {
        std::scoped_lock lock(gpuMutex);
        return {primaryVendor.load()};
    }
};
struct Option {
    std::optional<bool> value;
    bool value_or_default() const { return value.value_or(true); }
    bool value_or(bool fallback) const { return value.value_or(fallback); }
};
struct Config {
    Option UseFakenvapi, DisableFlipMetering;
    static Config* Instance() { static Config config; return &config; }
};
enum class FGOutput { Other, XeFG };
struct State {
    FGOutput activeFgOutput=FGOutput::Other;
    static State& Instance() { static State state; return state; }
};
HMODULE dllModule=reinterpret_cast<HMODULE>(0x1234);
bool usingFake=false, nativeMissing=false;
int hooks=0;
namespace fakenvapi {
void setUsingAsMainNvapi(bool value) { usingFake=value; }
void* queryInterface(unsigned id) { return reinterpret_cast<void*>(size_t{0x10000}+id); }
}
struct NtdllProxy {
    static HMODULE LoadLibraryExW_Ldr(const wchar_t*, HANDLE, DWORD) {
        if (nativeMissing) return nullptr;
        // Real system DLL load from inside real DllMain, without an NVIDIA
        // driver requirement in this deterministic dependency fixture.
        return LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
};
using Query=void*(__stdcall*)(unsigned);
using PFN_NvApi_QueryInterface=Query;
void* __stdcall nativeQuery(unsigned id) { return reinterpret_cast<void*>(size_t{0x20000}+id); }
Query o_NvAPI_QueryInterface=nativeQuery;
enum : unsigned {
    NvAPI_D3D12_SetFlipConfig=1, NvAPI_D3D_SetSleepMode, NvAPI_D3D_Sleep,
    NvAPI_D3D_GetLatency, NvAPI_D3D_SetLatencyMarker, NvAPI_D3D12_SetAsyncFrameMarker,
    NvAPI_Vulkan_GetLatency, NvAPI_Vulkan_SetLatencyMarker, NvAPI_Vulkan_SetSleepMode,
    NvAPI_GPU_GetArchInfo=20, NvAPI_DRS_GetSetting
};
void archMethod() {}
void drsMethod() {}
// Shipping bodies use these names as function symbols as well as IDs.
#undef GET_ID
#define GET_ID(name) id_##name
constexpr unsigned id_NvAPI_D3D12_SetFlipConfig=NvAPI_D3D12_SetFlipConfig;
constexpr unsigned id_NvAPI_D3D_SetSleepMode=NvAPI_D3D_SetSleepMode;
constexpr unsigned id_NvAPI_D3D_Sleep=NvAPI_D3D_Sleep;
constexpr unsigned id_NvAPI_D3D_GetLatency=NvAPI_D3D_GetLatency;
constexpr unsigned id_NvAPI_D3D_SetLatencyMarker=NvAPI_D3D_SetLatencyMarker;
constexpr unsigned id_NvAPI_D3D12_SetAsyncFrameMarker=NvAPI_D3D12_SetAsyncFrameMarker;
constexpr unsigned id_NvAPI_Vulkan_GetLatency=NvAPI_Vulkan_GetLatency;
constexpr unsigned id_NvAPI_Vulkan_SetLatencyMarker=NvAPI_Vulkan_SetLatencyMarker;
constexpr unsigned id_NvAPI_Vulkan_SetSleepMode=NvAPI_Vulkan_SetSleepMode;
constexpr unsigned id_NvAPI_GPU_GetArchInfo=NvAPI_GPU_GetArchInfo;
constexpr unsigned id_NvAPI_DRS_GetSetting=NvAPI_DRS_GetSetting;
#define NvAPI_GPU_GetArchInfo archMethod
#define NvAPI_DRS_GetSetting drsMethod
decltype(&archMethod) o_NvAPI_GPU_GetArchInfo=nullptr;
decltype(&drsMethod) o_NvAPI_DRS_GetSetting=nullptr;
struct ReflexHooks {
    static void hookReflex(Query) {}
    static void* getHookedReflex(unsigned id) { return nativeQuery(id); }
};
struct NvApiHooks {
    static void Hook(HMODULE) { ++hooks; }
    static void* __stdcall hkNvAPI_QueryInterface(unsigned);
    static void hkNvAPI_GPU_GetArchInfo() {}
    static void hkNvAPI_DRS_GetSetting() {}
};
struct LibraryLoadHooks { static HMODULE LoadNvApi(); };
#include "shipping_nvapi.inc"

HMODULE loaded=nullptr;
void* queried=nullptr;
extern "C" __declspec(dllexport) void RunNvapiLoaderTest() {
    loaded=LibraryLoadHooks::LoadNvApi();
    queried=NvApiHooks::hkNvAPI_QueryInterface(id_NvAPI_D3D12_SetFlipConfig);
}
HANDLE ready, releaseWorker;
DWORD WINAPI discover(void*) {
    std::scoped_lock lock(gpuMutex);
    SetEvent(ready);
    return WaitForSingleObject(releaseWorker, 10000)==WAIT_OBJECT_0 ? 0 : 1;
}
int main() {
    ready=CreateEventW(nullptr, TRUE, FALSE, nullptr);
    releaseWorker=CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto worker=CreateThread(nullptr, 0, discover, nullptr, 0, nullptr);
    if (WaitForSingleObject(ready, 2000)!=WAIT_OBJECT_0) return 2;
    int failures=0, checks=0;
    for (auto vendor : {VendorId::Invalid, VendorId::Nvidia, VendorId::AMD}) {
        for (bool fake : {false,true}) {
            for (bool missing : {false,true}) {
                IdentifyGpu::primaryVendor.store(vendor);
                Config::Instance()->UseFakenvapi.value=fake;
                nativeMissing=missing;
                usingFake=false; hooks=0; loaded=nullptr;
                HMODULE fixture=LoadLibraryW(L"loader_fixture.dll");
                if (!fixture) return 3;
                FreeLibrary(fixture);
                const bool expectFake=fake && (vendor==VendorId::AMD || missing);
                failures += (usingFake!=expectFake || (loaded==dllModule)!=expectFake);
                failures += (!expectFake && ((loaded==nullptr)!=missing));
                const bool expectFlipNull=vendor==VendorId::AMD;
                failures += (queried==nullptr)!=expectFlipNull;
                ++checks;
                if (loaded && loaded!=dllModule) FreeLibrary(loaded);
            }
        }
    }
    SetEvent(releaseWorker);
    if (WaitForSingleObject(worker,2000)!=WAIT_OBJECT_0) return 4;
    CloseHandle(worker); CloseHandle(ready); CloseHandle(releaseWorker);
    std::printf("%d loader-lock routing cases, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
