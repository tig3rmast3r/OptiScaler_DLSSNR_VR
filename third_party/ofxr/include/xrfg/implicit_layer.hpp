#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xrfg::implicit_layer {
inline constexpr wchar_t kDefaultExcludedProcesses[] = L"PimaxHome-Win64-Shipping.exe";
[[nodiscard]] std::vector<std::wstring> read_excluded_processes(const std::filesystem::path&) noexcept;
[[nodiscard]] bool executable_is_excluded(std::wstring_view, const std::vector<std::wstring>&) noexcept;
[[nodiscard]] std::wstring current_executable_name() noexcept;


// One manual-reset kernel event per arm. Old readers retain their own event;
// re-arming cannot accidentally reactivate a session that was already stopped.
[[nodiscard]] std::wstring arm_signal_name(const std::filesystem::path& manifest);
[[nodiscard]] void* create_arm_signal(const std::filesystem::path& manifest,
                                      std::wstring* error = nullptr) noexcept;
[[nodiscard]] bool signal_arm_stop(const std::filesystem::path& manifest,
                                    std::wstring* error = nullptr) noexcept;
class ManualArmControl {
public:
    explicit ManualArmControl(const std::filesystem::path& module_directory) noexcept;
    ~ManualArmControl();
    ManualArmControl(const ManualArmControl&) = delete;
    ManualArmControl& operator=(const ManualArmControl&) = delete;
    [[nodiscard]] bool stop_requested() const noexcept;
private:
    void* event_{};
    bool managed_{};
};

inline constexpr wchar_t kRegistrySubkey[] =
    L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
inline constexpr wchar_t kManifestPrefix[] =
    L"XR_APILAYER_XRFrameBridge_manual-";
inline constexpr wchar_t kManifestSuffix[] = L".json";

enum class RegistryScope {
    current_user,
    local_machine,
};

[[nodiscard]] RegistryScope registry_scope_for_integrity_rid(
    std::uint32_t integrity_rid) noexcept;
[[nodiscard]] RegistryScope preferred_registry_scope() noexcept;
[[nodiscard]] std::wstring_view registry_scope_name(RegistryScope scope) noexcept;

enum class ConfiguredFlowBackend {
    fidelity_fx,
    nvidia,
};

enum class ConfiguredNvidiaPerformancePreset {
    slow,
    medium,
    fast,
};

enum class ConfiguredNvidiaInputScale {
    full,
    three_quarter,
    half,
    quarter,
};

struct ConfiguredNvidiaOptions {
    ConfiguredNvidiaPerformancePreset preset{
        ConfiguredNvidiaPerformancePreset::medium};
    ConfiguredNvidiaInputScale input_scale{
        ConfiguredNvidiaInputScale::half};
    bool bidirectional{};
};

[[nodiscard]] ConfiguredFlowBackend read_flow_backend(
    const std::filesystem::path& module_directory) noexcept;

[[nodiscard]] ConfiguredNvidiaOptions read_nvidia_options(
    const std::filesystem::path& module_directory) noexcept;

[[nodiscard]] bool register_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

[[nodiscard]] bool unregister_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

[[nodiscard]] bool manifest_registered(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

[[nodiscard]] bool owned_manifest_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& runtime_directory) noexcept;

// Recognizes only tray-owned manual manifests in RuntimeLayer (legacy) or
// RuntimeLayer/vNNN. Does not require the manifest or DLL to still exist.
[[nodiscard]] bool owned_registration_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& local_directory) noexcept;

// Disable before deleting; read back registry state, and retain the JSON when
// deregistration fails. The caller must report a false result, never claim Off.
[[nodiscard]] bool retire_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

// Registry-first recovery across ALL versions, including missing JSON files.
// Removes only owned registration values and owned JSONs, never DLLs/settings.
[[nodiscard]] bool cleanup_owned_registrations(
    const std::filesystem::path& local_directory,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

} // namespace xrfg::implicit_layer
