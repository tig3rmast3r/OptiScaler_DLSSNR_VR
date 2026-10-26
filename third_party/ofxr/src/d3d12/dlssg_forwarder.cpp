// Private DLSS-G handles never pass through the game's NGX core or SR hooks.
// The signed snippet requires calls from an NGX-named module, as does NR.
#define NGX_SNIPPET_BUILD
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <filesystem>
#include <mutex>

namespace {
using Init = decltype(&NVSDK_NGX_D3D12_Init_Ext);
using Create = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using Evaluate = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using Release = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
using Requirements = decltype(&NVSDK_NGX_D3D12_GetFeatureRequirements);
using Populate = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using DevicePopulate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*, NVSDK_NGX_Parameter*);
HMODULE provider{};
Init init{}; Create create{}; Evaluate evaluate{}; Release release{};
Requirements requirements{}; Populate populate{}; DevicePopulate device_populate{};
std::once_flag once;
bool load() {
    std::call_once(once, [] {
        HMODULE self{}; wchar_t path[32768]{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&load), &self) ||
            !GetModuleFileNameW(self, path, 32768)) return;
        const auto file = std::filesystem::path(path).parent_path() / L"nvngx_dlssg.dll";
        provider = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!provider) return;
#define ENTRY(field, name) field = reinterpret_cast<decltype(field)>(GetProcAddress(provider, name))
        ENTRY(init, "NVSDK_NGX_D3D12_Init_Ext");
        ENTRY(create, "NVSDK_NGX_D3D12_CreateFeature");
        ENTRY(evaluate, "NVSDK_NGX_D3D12_EvaluateFeature");
        ENTRY(release, "NVSDK_NGX_D3D12_ReleaseFeature");
        ENTRY(requirements, "NVSDK_NGX_D3D12_GetFeatureRequirements");
        ENTRY(populate, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
        ENTRY(device_populate, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl");
#undef ENTRY
        // Pin for the process lifetime. Never shut down the shared provider;
        // the game or another swapchain may own a concurrent feature.
    });
    return init && create && evaluate && release && requirements && populate && device_populate;
}
NVSDK_NGX_Result NVSDK_CONV direct_create(ID3D12GraphicsCommandList* list,
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle) {
    volatile auto result = create(list, feature, params, handle);
    return result;
}
NVSDK_NGX_Result NVSDK_CONV direct_evaluate(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
    PFN_NVSDK_NGX_ProgressCallback callback) {
    volatile auto result = evaluate(list, handle, params, callback);
    return result;
}
NVSDK_NGX_Result NVSDK_CONV direct_evaluate_c(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
    PFN_NVSDK_NGX_ProgressCallback_C callback) {
    // The FG helper passes no progress callback. The snippet exports only
    // the canonical C++ callback ABI; never reinterpret a C callback as it.
    if (callback) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    volatile auto result = evaluate(list, handle, params, nullptr);
    return result;
}
void set_float(NVSDK_NGX_Parameter* params, const char* name, float value) { params->Set(name,value); }
void set_uint(NVSDK_NGX_Parameter* params, const char* name, unsigned value) { params->Set(name,value); }
void set_resource(NVSDK_NGX_Parameter* params, const char* name, ID3D12Resource* value) { params->Set(name,value); }
void set_pointer(NVSDK_NGX_Parameter* params, const char* name, void* value) { params->Set(name,value); }
}
// Use the SDK's parameter setters and complete FG helpers, with only their
// final dispatch redirected to the owned snippet. No guessed parameter vtable.
#define NVSDK_NGX_D3D12_CreateFeature direct_create
#define NVSDK_NGX_D3D12_EvaluateFeature direct_evaluate
#define NVSDK_NGX_D3D12_EvaluateFeature_C direct_evaluate_c
#define NVSDK_NGX_Parameter_SetF set_float
#define NVSDK_NGX_Parameter_SetUI set_uint
#define NVSDK_NGX_Parameter_SetD3d12Resource set_resource
#define NVSDK_NGX_Parameter_SetVoidPointer set_pointer
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#undef NVSDK_NGX_D3D12_CreateFeature
#undef NVSDK_NGX_D3D12_EvaluateFeature
#undef NVSDK_NGX_D3D12_EvaluateFeature_C
#undef NVSDK_NGX_Parameter_SetF
#undef NVSDK_NGX_Parameter_SetUI
#undef NVSDK_NGX_Parameter_SetD3d12Resource
#undef NVSDK_NGX_Parameter_SetVoidPointer

extern "C" {
__declspec(dllexport) unsigned ofxr_dlssg_abi() { return 45601; }
__declspec(dllexport) NVSDK_NGX_Result ofxr_dlssg_requirements(IDXGIAdapter* adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* info, NVSDK_NGX_FeatureRequirement* out) {
    // Keep the snippet's return address inside this module even in /O2.
    // A tail call would make its caller validation see the game/layer instead.
    volatile auto result = load() ? requirements(adapter, info, out) : NVSDK_NGX_Result_FAIL_FeatureNotFound;
    return result;
}
__declspec(dllexport) NVSDK_NGX_Result ofxr_dlssg_init(ID3D12Device* device,
    const wchar_t* logs, NVSDK_NGX_Parameter* params) {
    if (!load()) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    // Stable custom identifier for OFXR-BRIDGE; not a different game's ID.
    auto result = init(0x4f4658524155544full, logs, device, NVSDK_NGX_Version_API, params);
    if (NVSDK_NGX_FAILED(result)) return result;
    result = populate(params);
    volatile auto populated = NVSDK_NGX_SUCCEED(result) ? device_populate(device, params) : result;
    return populated;
}
__declspec(dllexport) NVSDK_NGX_Result ofxr_dlssg_create(ID3D12GraphicsCommandList* list,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_DLSSG_Create_Params* args, NVSDK_NGX_Handle** handle) {
    if (!load()) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    auto result = populate(params);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (NVSDK_NGX_SUCCEED(result)) result = device_populate(device.Get(), params);
    return NVSDK_NGX_SUCCEED(result) ? NGX_D3D12_CREATE_DLSSG(list, 1, 1, handle, params, args) : result;
}
__declspec(dllexport) NVSDK_NGX_Result ofxr_dlssg_evaluate(ID3D12GraphicsCommandList* list,
    NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params,
    NVSDK_NGX_D3D12_DLSSG_Eval_Params* args, NVSDK_NGX_DLSSG_Opt_Eval_Params* options) {
    return load() ? NGX_D3D12_EVALUATE_DLSSG(list, handle, params, args, options) :
        NVSDK_NGX_Result_FAIL_FeatureNotFound;
}
__declspec(dllexport) NVSDK_NGX_Result ofxr_dlssg_release(NVSDK_NGX_Handle* handle) {
    volatile auto result = load() ? release(handle) : NVSDK_NGX_Result_FAIL_FeatureNotFound;
    return result;
}
}
