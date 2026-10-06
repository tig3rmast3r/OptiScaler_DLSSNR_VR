#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: xrfg_layer_smoke <layer-dll> [delegated-layer-dll]\n";
        return EXIT_FAILURE;
    }

    const bool expect_excluded = argc == 3 && std::strcmp(argv[2], "--expect-excluded") == 0;
    HMODULE delegated_module = nullptr;
    if (argc == 3 && !expect_excluded) {
        delegated_module = LoadLibraryA(argv[2]);
        if (delegated_module == nullptr) {
            std::cerr << "delegated LoadLibrary failed: " << GetLastError() << '\n';
            return EXIT_FAILURE;
        }
    }

    const HMODULE module = LoadLibraryA(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }

    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
        GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
    if (negotiate == nullptr) {
        std::cerr << "negotiation export is missing\n";
        FreeLibrary(module);
        if (delegated_module != nullptr) FreeLibrary(delegated_module);
        return EXIT_FAILURE;
    }

    XrNegotiateLoaderInfo loader_info{};
    loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader_info.structSize = sizeof(loader_info);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_CURRENT_API_VERSION;

    XrNegotiateApiLayerRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);

    const XrResult result = negotiate(
        &loader_info,
        "XR_APILAYER_XRFrameBridge_diagnostic",
        &request);
    if (expect_excluded) {
        const bool refused = result == XR_ERROR_INITIALIZATION_FAILED &&
            request.getInstanceProcAddr == nullptr && request.createApiLayerInstance == nullptr;
        FreeLibrary(module);
        std::cout << (refused ? "Excluded process negotiation refused correctly" : "Unexpected exclusion result") << '\n';
        return refused ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (XR_FAILED(result) || request.getInstanceProcAddr == nullptr ||
        request.createApiLayerInstance == nullptr || request.layerInterfaceVersion != 1 ||
        XR_VERSION_MAJOR(request.layerApiVersion) != 1) {
        std::cerr << "layer negotiation failed: " << result << '\n';
        FreeLibrary(module);
        if (delegated_module != nullptr) FreeLibrary(delegated_module);
        return EXIT_FAILURE;
    }

    const HMODULE embedded_owner = delegated_module ? delegated_module : module;
    const auto embedded_active = reinterpret_cast<int (*)() noexcept>(
        GetProcAddress(embedded_owner, "OFXR_GetEmbeddedLayerActiveV1"));
    if (embedded_active && embedded_active() != 1) {
        std::cerr << "successful embedded negotiation did not establish ownership\n";
        return EXIT_FAILURE;
    }

    if (delegated_module != nullptr) {
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(request.createApiLayerInstance),
                &owner) ||
            owner != delegated_module) {
            std::cerr << "layer negotiation did not delegate to the requested module\n";
            FreeLibrary(module);
            FreeLibrary(delegated_module);
            return EXIT_FAILURE;
        }
    }

    FreeLibrary(module);
    if (delegated_module != nullptr) FreeLibrary(delegated_module);
    std::cout << "OpenXR layer export and negotiation smoke test passed\n";
    return EXIT_SUCCESS;
}
