#define XR_USE_GRAPHICS_API_VULKAN
#include "xrfg/vulkan_session_bridge.hpp"
#include "xrfg/d3d11_bridge.hpp"
#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d12_native_binding.hpp"
#include "djules_output_buffer.hpp"
#include "djules_triple_release.hpp"
#include "xrfg/two_output_schedule.hpp"
#include "xrfg/midpoint_schedule.hpp"
#include "xrfg/frame_capture.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/luke_guide_eye.hpp"
#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/d3d11_color_identity.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/fsr_cadence_probe.hpp"
#include "xrfg/luke_render_identity.hpp"
#include "xrfg/generation_backpressure.hpp"
#include "xrfg/implicit_layer.hpp"
#include "xrfg/luke_afw.hpp"
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_world_scale.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/presentation_transform.h"
#include "xrfg/fsr_frame_generation.hpp"
#include "xrfg/provider_api.hpp"
#include "xrfg/two_frame_capture.hpp"
#include <atomic>
#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/steamvr_delivery.hpp"

#include <windows.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>


// This library exists only as part of OptiScaler. A successful negotiation
// establishes ownership here even when the loader reaches this export directly,
// rather than through a tray bootstrap that calls the activation export first.
namespace {

[[nodiscard]] std::filesystem::path current_layer_directory() noexcept;

  // namespace optiscaler_bootstrap

constexpr char kLayerName[] = "XR_APILAYER_XRFrameBridge_diagnostic";
constexpr XrVersion kLayerApiVersion = XR_MAKE_VERSION(1, 0, 0);
constexpr XrDuration kGenerationCooldownDuration = 1'000'000'000;
// Consecutive submissions one scanout apart before any phase correction is
// allowed to run. Phase means nothing until the rate is right: a grid that is
// skipping slots has no stable phase to correct towards, and correcting one
// anyway drives a feedback loop - see the comments at both correction sites.
// Eight is a quarter of a second at 90 Hz.
constexpr std::uint32_t kPhaseCorrectionGridStreak = 8;
constexpr auto kStructuralQuarantineDuration = std::chrono::seconds(1);
// The runtime's own xrWaitFrame pacing happens first. Only a bridge transaction
// still pending after that natural idle window may hold the application here.
// This prevents the next game/NGX frame from being queued behind unfinished
// synthesis while keeping a genuinely unhealthy GPU wait bounded.
constexpr std::uint32_t kFrameStartSynthesisWaitMilliseconds = 1000;

template <typename Handle>
[[nodiscard]] std::uint64_t handle_value(Handle handle) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

[[nodiscard]] std::filesystem::path current_layer_directory() noexcept {
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&current_layer_directory),
                &module)) {
            return {};
        }
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(
            module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return {};
        }
        return std::filesystem::path(path.data()).parent_path();
    } catch (...) {
        return {};
    }
}

[[nodiscard]] std::uint64_t optical_flow_configuration_code(
    xrfg::D3D12OpticalFlowBackend backend,
    const xrfg::D3D12NvidiaOpticalFlowOptions& options) noexcept {
    if (backend != xrfg::D3D12OpticalFlowBackend::nvidia) {
        return 0;
    }
    std::uint64_t code = 2;
    if (options.preset == xrfg::D3D12NvidiaPerformancePreset::slow) {
        code = 1;
    } else if (options.preset == xrfg::D3D12NvidiaPerformancePreset::fast) {
        code = 3;
    }
    std::uint64_t scale_code = 0;
    if (options.input_scale ==
        xrfg::D3D12NvidiaInputScale::three_quarter) {
        scale_code = 0x10000U;
    } else if (options.input_scale == xrfg::D3D12NvidiaInputScale::half) {
        scale_code = 0x20000U;
    }
    return code | (options.bidirectional ? 0x100U : 0U) | scale_code;
}

// The runtime's Vulkan entry points, by the names the layer gives them. The
// enable2 variants share the signatures of the originals.
using PFN_GetVulkanExtensions = PFN_xrGetVulkanInstanceExtensionsKHR;
using PFN_CreateVulkanInstance = PFN_xrCreateVulkanInstanceKHR;
using PFN_CreateVulkanDevice = PFN_xrCreateVulkanDeviceKHR;
using PFN_GetVulkanGraphicsDevice = PFN_xrGetVulkanGraphicsDeviceKHR;
using PFN_GetVulkanGraphicsDevice2 = PFN_xrGetVulkanGraphicsDevice2KHR;
using PFN_GetVulkanGraphicsRequirements = PFN_xrGetVulkanGraphicsRequirementsKHR;

struct VulkanNegotiationDispatch {
    PFN_GetVulkanExtensions get_instance_extensions{};
    PFN_GetVulkanExtensions get_device_extensions{};
    PFN_CreateVulkanInstance create_instance{};
    PFN_CreateVulkanDevice create_device{};
    PFN_GetVulkanGraphicsDevice get_graphics_device{};
    PFN_GetVulkanGraphicsDevice2 get_graphics_device2{};
    PFN_GetVulkanGraphicsRequirements get_graphics_requirements{};
    PFN_GetVulkanGraphicsRequirements get_graphics_requirements2{};
};

// The Vulkan extensions an interop with D3D12 would need, as bits; the
// numbering is the one BridgeFlightOperation::vulkan_negotiation documents.
[[nodiscard]] std::uint64_t vulkan_interop_extension_bit(
    std::string_view name) noexcept {
    constexpr std::array<std::string_view, 11> kNames{
        "VK_KHR_external_memory_win32",
        "VK_KHR_external_semaphore_win32",
        "VK_KHR_external_memory",
        "VK_KHR_external_semaphore",
        "VK_KHR_timeline_semaphore",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_win32_keyed_mutex",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_KHR_get_physical_device_properties2",
    };
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        if (name == kNames[index]) {
            return 1ULL << index;
        }
    }
    return 0;
}

struct VulkanExtensionSummary {
    std::uint64_t count{};
    std::uint64_t bits{};
};

// The runtime's answer to xrGetVulkan*ExtensionsKHR: one space-separated
// string.
[[nodiscard]] VulkanExtensionSummary summarize_vulkan_extension_string(
    std::string_view list) noexcept {
    VulkanExtensionSummary summary{};
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t end = list.find(' ', start);
        const std::string_view name = list.substr(
            start,
            end == std::string_view::npos ? list.size() - start : end - start);
        if (!name.empty()) {
            ++summary.count;
            summary.bits |= vulkan_interop_extension_bit(name);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return summary;
}

[[nodiscard]] VulkanExtensionSummary summarize_vulkan_extension_array(
    const char* const* names,
    std::uint32_t count) noexcept {
    VulkanExtensionSummary summary{};
    if (names == nullptr) {
        return summary;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        if (names[index] != nullptr) {
            ++summary.count;
            summary.bits |= vulkan_interop_extension_bit(names[index]);
        }
    }
    return summary;
}

[[nodiscard]] std::uint64_t pack_negotiation_results(
    XrResult xr_result,
    VkResult vk_result) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(xr_result))
            << 32) |
        static_cast<std::uint32_t>(vk_result);
}

void log_vulkan_negotiation(
    std::int64_t selector,
    std::uint64_t a,
    std::uint64_t b = 0,
    std::uint64_t c = 0) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::vulkan_negotiation, selector, a, b, c);
}

// Whether the application's Vulkan device can take part in a D3D12 interop,
// asked of the device itself: with XR_KHR_vulkan_enable the application
// creates the device, so the list it was handed says what it was asked to
// enable, not what it did. A device returns no entry point for a command of
// an extension it did not enable.
//   11  a= commands the device returned: bit 0 vkGetMemoryWin32HandleKHR,
//       1 vkGetSemaphoreWin32HandleKHR, 2 vkImportSemaphoreWin32HandleKHR,
//       3 vkGetSemaphoreCounterValueKHR, 4 vkGetSemaphoreCounterValue
//   12  a= the same interop extension bits, for what the physical device
//       supports at all; b= how many device extensions it lists
void probe_vulkan_interop_support(
    const XrGraphicsBindingVulkanKHR& binding) noexcept {
    try {
        const HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
        if (vulkan == nullptr || binding.device == VK_NULL_HANDLE) {
            log_vulkan_negotiation(11, 0, 0, 1);
            return;
        }
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
        const auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
        if (get_device_proc != nullptr) {
            constexpr std::array<const char*, 5> kCommands{
                "vkGetMemoryWin32HandleKHR",
                "vkGetSemaphoreWin32HandleKHR",
                "vkImportSemaphoreWin32HandleKHR",
                "vkGetSemaphoreCounterValueKHR",
                "vkGetSemaphoreCounterValue",
            };
            std::uint64_t live = 0;
            for (std::size_t index = 0; index < kCommands.size(); ++index) {
                if (get_device_proc(binding.device, kCommands[index]) != nullptr) {
                    live |= 1ULL << index;
                }
            }
            log_vulkan_negotiation(11, live);
        }
        if (get_instance_proc != nullptr && binding.instance != VK_NULL_HANDLE &&
            binding.physicalDevice != VK_NULL_HANDLE) {
            const auto enumerate =
                reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    get_instance_proc(
                        binding.instance, "vkEnumerateDeviceExtensionProperties"));
            std::uint32_t extension_count = 0;
            if (enumerate != nullptr &&
                enumerate(binding.physicalDevice, nullptr, &extension_count,
                          nullptr) == VK_SUCCESS &&
                extension_count > 0) {
                std::vector<VkExtensionProperties> extensions(extension_count);
                if (enumerate(binding.physicalDevice, nullptr, &extension_count,
                              extensions.data()) >= VK_SUCCESS) {
                    std::uint64_t supported = 0;
                    for (std::uint32_t index = 0; index < extension_count; ++index) {
                        supported |= vulkan_interop_extension_bit(
                            extensions[index].extensionName);
                    }
                    log_vulkan_negotiation(12, supported, extension_count);
                }
            }
        }
    } catch (...) {
    }
}

struct Dispatch {
    bool vulkan_bridge{};
    VulkanNegotiationDispatch vulkan{};
    PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats{};
    // The D3D11 bridge (SessionState::d3d11_bridge): set when the ini asks
    // for it, the application enabled XR_KHR_D3D11_enable, and the layer
    // could add XR_KHR_D3D12_enable to the instance. The runtime's D3D12
    // requirements call has to be made before a D3D12 session is created.
    bool d3d11_bridge{};
    PFN_xrGetD3D12GraphicsRequirementsKHR get_d3d12_graphics_requirements{};
    XrInstance instance{XR_NULL_HANDLE};
    PFN_xrGetInstanceProcAddr get_instance_proc_addr{};
    PFN_xrConvertTimeToWin32PerformanceCounterKHR convert_time_to_qpc{};
    PFN_xrLocateViews locate_views{};
    PFN_xrCreateReferenceSpace create_reference_space{};
    PFN_xrCreateActionSpace create_action_space{};
    PFN_xrLocateSpace locate_space{};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrBeginSession begin_session{};
    PFN_xrEndSession end_session{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images{};
    PFN_xrAcquireSwapchainImage acquire_swapchain_image{};
    PFN_xrWaitSwapchainImage wait_swapchain_image{};
    PFN_xrReleaseSwapchainImage release_swapchain_image{};
    // Best-effort: a runtime that does not expose it simply never has a space
    // destroyed underneath a queued submission through this layer.
    PFN_xrDestroySpace destroy_space{};
    // Best-effort, like destroy_space above. The layer does not act on events
    // and never consumes one; it records session state transitions so a
    // capture can say whether the runtime stopped asking for frames, and why.
    PFN_xrPollEvent poll_event{};
    bool steamvr_runtime{};
    bool virtual_desktop_runtime{};
    bool inline_unless_pipelined{};
    XrVersion runtime_version{};
    std::string runtime_name;
};

[[nodiscard]] std::uint64_t runtime_name_hash(
    std::string_view name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : name) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::uint64_t dlss_guide_camera_hash(
    const xrfg::DlssMotionVectorFrame& guide) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto append = [&hash](const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    };
    append(guide.camera_position.data(),
           guide.camera_position.size() * sizeof(guide.camera_position[0]));
    append(guide.camera_right.data(),
           guide.camera_right.size() * sizeof(guide.camera_right[0]));
    append(guide.camera_forward.data(),
           guide.camera_forward.size() * sizeof(guide.camera_forward[0]));
    append(&guide.camera_fov_vertical, sizeof(guide.camera_fov_vertical));
    append(&guide.camera_data_valid, sizeof(guide.camera_data_valid));
    append(guide.camera_view_to_clip.data(),
           guide.camera_view_to_clip.size() *
               sizeof(guide.camera_view_to_clip[0]));
    append(&guide.camera_matrices_valid,
           sizeof(guide.camera_matrices_valid));
    return hash;
}

[[nodiscard]] std::uint64_t projection_pose_hash(
    const XrCompositionLayerProjectionView& view) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const float fields[] = {
        view.pose.position.x, view.pose.position.y, view.pose.position.z,
        view.pose.orientation.x, view.pose.orientation.y,
        view.pose.orientation.z, view.pose.orientation.w,
        view.fov.angleLeft, view.fov.angleRight,
        view.fov.angleUp, view.fov.angleDown,
    };
    for (const float value : fields) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (bits >> shift) & 0xffU;
            hash *= prime;
        }
    }
    return hash;
}

void log_luke_projection_poses(
    const XrFrameEndInfo* frame,
    std::uint64_t serial,
    std::int64_t stage) noexcept {
    if (frame == nullptr || frame->layers == nullptr || serial == 0) return;
    for (std::uint32_t layer_index = 0;
         layer_index < frame->layerCount && layer_index < 8;
         ++layer_index) {
        const auto* header = frame->layers[layer_index];
        if (header == nullptr ||
            header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* projection =
            reinterpret_cast<const XrCompositionLayerProjection*>(header);
        if (projection->views == nullptr) continue;
        for (std::uint32_t view_index = 0;
             view_index < projection->viewCount && view_index < 8;
             ++view_index) {
            // result = stage*100 + layer*10 + view. Stages: 1=application,
            // 2=synthetic, 3=real. The hash covers pose and FOV only.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_pose_contract,
                stage * 100 + layer_index * 10 + view_index,
                serial,
                static_cast<std::uint64_t>(frame->displayTime),
                projection_pose_hash(projection->views[view_index]));
            xrfg::two_frame_capture_timeline(
                "pose", stage * 100 + layer_index * 10 + view_index,
                serial, static_cast<std::uint64_t>(frame->displayTime),
                projection_pose_hash(projection->views[view_index]));
        }
    }
}

void log_luke_view_bits(std::uint64_t serial, std::int64_t code,
                        const XrCompositionLayerProjectionView& view) noexcept {
    const auto pair = [](float x, float y) noexcept {
        std::uint32_t a{}, b{};
        std::memcpy(&a, &x, sizeof(a));
        std::memcpy(&b, &y, sizeof(b));
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    const float values[] = {
        view.pose.position.x, view.pose.position.y, view.pose.position.z,
        view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
        view.pose.orientation.w, view.fov.angleLeft, view.fov.angleRight,
        view.fov.angleUp, view.fov.angleDown, 0.0f};
    for (int index = 0; index < 3; ++index) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_inline_pose_bits,
            code * 10 + index, serial,
            pair(values[index * 4], values[index * 4 + 1]),
            pair(values[index * 4 + 2], values[index * 4 + 3]));
    }
}

struct GeneratedFrameEndInfo;
void log_luke_guide_basis(const xrfg::DlssMotionVectorFrame& guide) noexcept {
    if (!xrfg::bridge_flight_logger().ram_enabled()) return;
    const auto pair = [](float x, float y) noexcept {
        std::uint32_t a{}, b{};
        std::memcpy(&a, &x, sizeof(a)); std::memcpy(&b, &y, sizeof(b));
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    const std::array<const std::array<float, 3>*, 4> rows{
        &guide.camera_position, &guide.camera_right, &guide.camera_up, &guide.camera_forward};
    for (std::size_t row = 0; row < rows.size(); ++row)
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_render_camera,
            200 + row, guide.publication, pair((*rows[row])[0], (*rows[row])[1]),
            pair((*rows[row])[2], guide.camera_data_valid ? 1.0f : 0.0f));
}
struct ThreeSlotFrame;
struct SynthesisJob;

struct PresenterSubmission {
    std::uint64_t sequence{};
    std::uint64_t pair_sequence{};
    std::shared_ptr<GeneratedFrameEndInfo> owned_frame;
    const XrFrameEndInfo* borrowed_frame{};
    std::uint64_t app_release_value{};
    XrResult result{XR_SUCCESS};
    bool completed{};
};

struct ProjectionLayerSnapshot {
    std::uint32_t layer_index{};
    XrCompositionLayerFlags layer_flags{};
    XrSpace space{XR_NULL_HANDLE};
    std::vector<XrCompositionLayerProjectionView> views;
};

struct ProjectionSnapshot {
    XrTime display_time{};
    XrEnvironmentBlendMode environment_blend_mode{};
    std::vector<ProjectionLayerSnapshot> layers;
    std::optional<xrfg::LukeRenderIdentity> luke_identity;
    std::uint64_t app_release_value{};
};

// Immutable metadata belongs to the two released AFW images, not the next
// application's projection. No borrowed extension pointers are retained.
struct HeldAfwResource {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    XrSwapchain output_swapchain{XR_NULL_HANDLE};
    std::uint32_t logical_eye{};
};

struct HeldAfwPair {
    ProjectionSnapshot projection;
    std::array<HeldAfwResource, 2> resources;
    std::uint64_t revision{};
    std::uint64_t production_serial{};
};

struct ProjectionViewReference {
    std::size_t projection_index{};
    std::size_t view_index{};
};

struct ProjectionResourceMapping {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    // Canonical logical views for this resource. Ordinarily there is one per
    // array slice; UEVR Native Stereo may instead submit two non-overlapping
    // eye viewports in one physical slice.
    std::vector<ProjectionViewReference> views;
};

enum class ProjectionMappingReason : std::int64_t {
    ready = 0,
    no_projection_views = 1,
    unknown_swapchain = 2,
    unsupported_array_size = 3,
    zero_resource_extent = 4,
    array_slice_out_of_range = 5,
    negative_subimage_offset = 6,
    nonpositive_subimage_extent = 7,
    subimage_out_of_bounds = 8,
    missing_array_slice = 9,
    exception = 10,
    unsupported_view_layout = 11,
};

struct ProjectionMappingResult {
    std::vector<ProjectionResourceMapping> mappings;
    ProjectionMappingReason reason{ProjectionMappingReason::exception};
    std::uint64_t detail{};

    [[nodiscard]] bool ready() const noexcept {
        return reason == ProjectionMappingReason::ready && !mappings.empty();
    }
};

enum class SessionGraphicsBinding : std::int64_t {
    none = 0,
    d3d11 = 1,
    d3d12 = 2,
    vulkan = 3,
    opengl = 4,
};

struct PendingApplicationFrame {
    XrTime display_time{};
    XrDuration display_period{};
    std::uint64_t serial{};
};

enum class GenerationQuarantineReason : std::int64_t {
    swapchain_created = 1,
    swapchain_destroyed = 2,
    d3d11_images_changed = 3,
    d3d12_images_changed = 4,
    projection_changed = 6,
    projection_mapping_failed = 7,
    generation_prepare_failed = 8,
    generated_end_info_failed = 9,
    presenter_composition_failed = 10,
    downstream_end_failed = 11,
};

// Windows 10 1803 and later. Declared here so the layer still builds against
// an SDK that predates it; the create call degrades to a coarse timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct SessionState {
    // D25 comparison policy: SteamVR's runtime gets its own binding queue.
    // The canonical standard flavor never substitutes this queue.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> binding_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> app_release_fence;
    Microsoft::WRL::ComPtr<ID3D12Fence> reacquire_fence;
    std::mutex binding_join_mutex;
    std::uint64_t app_release_counter{};
    std::uint64_t reacquire_counter{};
    std::uint64_t app_end_frame_release_value{};

    struct VulkanBridge { xrfg::VulkanSessionBinding binding{}; };
    std::unique_ptr<VulkanBridge> vulkan_bridge;
    xrfg::VulkanSessionBinding vulkan_binding{};

    std::mutex logged_view_rects_mutex;
    std::unordered_map<XrSwapchain, std::array<std::optional<std::array<std::uint64_t, 3>>, 4>> logged_view_rects;
    std::unique_ptr<xrfg::SteamVrDelivery> steamvr_delivery;
    std::chrono::steady_clock::time_point compositor_diagnostic_next_poll{};
    std::uint32_t application_frames_submitted{};
    // The D3D11 bridge. The application bound D3D11; the runtime was handed
    // a D3D12 session on the layer's own device, so from here on the session
    // is a D3D12 one (graphics_binding, d3d12_device, d3d12_queue) and
    // d3d11_device stays null: no interop, no runtime-entry gate, no D3D11
    // work by the runtime on any thread. The application's device lives
    // here, for the per-swapchain shared textures it renders into
    // (D3D11BridgeSwapchain). Why: the NVIDIA D3D11 driver faults when the
    // runtime works the game's device from the presenter thread, and the
    // inline alternative is throttled by SteamVR; a runtime that only ever
    // sees D3D12 has neither problem. Capability bit 128.
    struct D3D11Bridge {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    };
    std::unique_ptr<D3D11Bridge> d3d11_bridge;
    std::atomic<bool> has_private_depth_swapchain{false};
    explicit SessionState(std::shared_ptr<Dispatch> next_dispatch)
        : dispatch(std::move(next_dispatch)), manual_control(current_layer_directory()),
          fsr_cadence(current_layer_directory()) {}

    ~SessionState() {
        xrfg::embedded::detach(control_id);
        if (presenter_pace_timer != nullptr) {
            CloseHandle(presenter_pace_timer);
        }
    }

    SessionState(const SessionState&) = delete;
    SessionState& operator=(const SessionState&) = delete;

    std::shared_ptr<Dispatch> dispatch;
    xrfg::implicit_layer::ManualArmControl manual_control;
    xrfg::FsrCadenceProbe fsr_cadence;
    std::uint64_t control_id{xrfg::embedded::attach()};
    std::uint64_t control_revision{}; // frame_call_mutex
    bool control_reconfigure_required{};
    std::atomic<bool> menu_enabled{true};
    std::atomic<bool> generation_steady_state_established{false};
    bool manual_stop_applied{}; // frame_call_mutex; terminal for this XrSession.
    std::unique_ptr<xrfg::OpenXrFpsOverlay> fps_overlay;
    std::unique_ptr<xrfg::FrameCapture> frame_capture;
    std::uint64_t diagnostic_app_frame{};
    bool diagnostic_key_down{};
    // Serializes each generated synthetic/real frame pair atomically with
    // respect to application frame calls. A successful application wait owns
    // the next admission until its matching begin has been attempted, so a
    // pipelined second wait cannot overtake that begin and deadlock the runtime.
    std::mutex frame_call_mutex;
    std::condition_variable frame_call_condition;
    // Serializes virtual waits and their matching BeginFrame admissions.
    // No application refresh timer is attached to this mutex.
    std::mutex adaptive_wait_mutex;
    // frame_call_mutex: scoped to the recognized Luke admission route. The
    // clock runs on elapsed wall time, not on how often Luke submits repeats.
    bool luke_clock_active{};
    bool luke_duplicate_cycle{};
    XrTime luke_clock_origin{};
    std::chrono::steady_clock::time_point luke_clock_wall_origin{};
    std::chrono::steady_clock::time_point application_wait_returned_at{};
    bool application_wait_pending_begin{};
    // Some applications run the next wait on a dedicated thread before the
    // current render thread ends its frame. Two consecutive overlaps select a
    // virtual application loop; the runtime-facing presenter then owns all
    // later physical frame cycles.
    bool application_frame_in_progress{};
    bool application_frame_has_overlapping_wait{};
    std::uint32_t pipelined_wait_streak{};
    bool pipelined_presenter_mode{};
    bool pipelined_presenter_start_requested{};
    bool presenter_start_requested{};
    bool runtime_frame_waited_unbegun{}; // frame_call_mutex
    bool application_begin_needs_wait{}; // frame_call_mutex
    std::optional<XrFrameState> presenter_adopted_frame; // presenter_mutex
    bool single_threaded_d3d11{}; // D08: runtime calls cannot move to another thread.
    bool split_frame_loop{}; // D09: targeted direct-D3D11 driver workaround.
    DWORD application_wait_thread_id{};
    DWORD application_end_thread_id{};
    XrFrameState last_inline_frame_state{XR_TYPE_FRAME_STATE};
    bool last_inline_frame_state_valid{};
    std::mutex mutex;
    std::mutex gpu_mutex;
    std::deque<PendingApplicationFrame> pending_frames;
    std::uint64_t next_application_frame_serial{1}; // mutex
    std::optional<ProjectionSnapshot> previous_projection;
    XrTime generation_resume_display_time{};
    std::chrono::steady_clock::time_point generation_resume_wall_time{};
    XrDuration minimum_runtime_display_period{};
    // How long the runtime's own xrWaitFrame blocked, and how many consecutive
    // waits came back too quickly to have been pacing anything. This is what
    // the presenter's existence actually turns on; see
    // runtime_wait_lacks_pacing.
    std::chrono::steady_clock::duration last_application_wait_elapsed{};
    std::uint32_t unpaced_wait_streak{};
    // Consecutive pairs whose two frames left the layer close enough together
    // to land in one scanout window - see inline_pair_lands_in_one_scanout.
    std::uint32_t bunched_pair_streak{};
    std::uint32_t steamvr_throttled_wait_streak{};
    // Consecutive application frames the layer could not generate from. A
    // single one says nothing - the frame passes through and the next one
    // usually pairs - so the presenter is only demoted once they run together.
    std::uint32_t generation_failure_streak{};
    xrfg::D3D12OpticalFlowBackend optical_flow_backend{
        xrfg::D3D12OpticalFlowBackend::fidelity_fx};
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options{};
    // D35: bounded opt-in allocation/adapter records; never a pacing input.
    std::mutex video_memory_mutex;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> video_memory_adapter;
    bool video_memory_unavailable{};
    std::chrono::steady_clock::time_point video_memory_poll_at{};
    std::chrono::steady_clock::time_point video_memory_first_poll{};

    bool nvidia_new_route{};
    std::atomic_bool djules_deep_pipeline{};
    std::atomic_bool djules_triple_requested{};
    std::atomic_uint64_t djules_configuration_epoch{};
    xrfg::DjulesTripleRelease djules_triple_release;
    bool djules_pipeline_switchable{};
    std::chrono::steady_clock::time_point djules_pipeline_poll{};
    std::optional<std::pair<bool, bool>> djules_pipeline_candidate;

    bool dlss_motion_vectors{};
    xrfg::D3D12FrameGenerationMode frame_generation_mode{};
    // Luke AFW capability is independent of the currently selected AFW mode.
    // The latter may be Off while ordinary OFXR generation remains enabled.
    bool luke_afw_enabled{};
    // Physical separation from ONE runtime pair, not cross-time AER poses.
    std::optional<float> luke_xr_eye_baseline; // mutex
    std::optional<bool> luke_afw_previous_selected; // frame_call_mutex
    bool luke_afw_odd_serial_is_left{true};
    bool luke_afw_f6_down{}; // frame_call_mutex; high-bit edge detector
    bool luke_f11_down{}; // frame_call_mutex; lightweight timing marker
    std::shared_ptr<xrfg::LukeAfwSynthesizer> luke_afw;
    // FSR keeps one immutable two-eye guide selection across D3D11 eye
    // releases. AFW records the real released eye here and resolves its
    // guide at xrEndFrame using the original color's decoded production token.
    std::mutex luke_guide_mutex;
    std::optional<std::uint32_t> luke_afw_released_eye;
    std::array<std::uint64_t, 2> luke_afw_last_guide_publication{};
    // PureDark has one temporal state across alternating real-eye calls.
    // Per-eye monotonicity alone permits a newer right-eye evaluation to be
    // followed by an older left-eye one when XR releases arrive out of order.
    std::uint64_t luke_afw_last_evaluated_publication{};
    // Last fully published AFW stereo pair. OpenXR permits re-submission of
    // the last released swapchain image on an XR frame with no game render.
    std::shared_ptr<const HeldAfwPair> luke_afw_last_pair;
    bool luke_frame_guide_resolved{};
    std::uint64_t luke_last_consumed_guide_publication{};
    std::shared_ptr<const xrfg::DlssMotionVectorSet>
        luke_frame_motion_vectors;
    xrfg::D3D12PresentationTransform presentation_transform{};
    SessionGraphicsBinding graphics_binding{SessionGraphicsBinding::none};
    std::uint64_t graphics_binding_capabilities{};
    // Set once the runtime has refused a private swapchain. A runtime caps how
    // many swapchains one session may hold at all -- SteamVR hands out 16 --
    // and the application is usually still creating its own when the layer
    // reaches that ceiling, so the private swapchains already taken are what
    // makes the application's next creation fail. Hand the whole budget back
    // and pass frames through for the rest of the session: an application that
    // cannot finish creating its swapchains has no way to recover, while one
    // that merely loses generation carries on.
    std::atomic<bool> generation_budget_exhausted{false};
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context;
    // Serialises this layer's entries into the runtime for as long as the
    // application's binding is D3D11. See RuntimeEntry below for why the gate
    // is here rather than on the device. Recursive because the frame path
    // already nests these calls inside one another on one thread.
    std::recursive_mutex runtime_entry_mutex;
    std::uint64_t runtime_entry_count{};       // runtime_entry_mutex
    std::uint64_t runtime_entry_contended{};   // runtime_entry_mutex
    std::uint64_t runtime_entry_wait_ns{};     // runtime_entry_mutex
    std::uint64_t runtime_entry_wait_max_ns{}; // runtime_entry_mutex
    std::uint64_t runtime_entry_hold_max_ns{}; // runtime_entry_mutex
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_queue;
    // Synthesis runs here rather than on the application's queue. The work
    // itself is short - the pack and composite command lists measure 249 us
    // per swapchain against MSFS 2024, and the 1610 us of optical flow runs
    // on the OFA engine rather than on any D3D12 queue - but the queue waits
    // on the OFA fence between them, and submitting that on the application's
    // queue blocks everything the application has already queued behind it,
    // which is its next frame's rendering. On a queue of its own the wait
    // holds only this. Null when the application binds D3D11, where
    // d3d12_queue is already the layer's own bridge queue.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_synthesis_queue;
    // Some SteamVR configurations throttle the inline second wait/begin/end
    // cycle to the application's half-rate interval. After that behavior is
    // measured, the dedicated presenter becomes the sole owner of downstream
    // calls while the application observes a virtual half-rate loop.
    std::mutex presenter_mutex;
    std::condition_variable presenter_condition;
    std::mutex presenter_content_mutex;
    std::deque<std::shared_ptr<PresenterSubmission>> presenter_submissions;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_last_frame;
    // A synthetic has reached xrEndFrame, but its paired current has not.
    // Protected by presenter_mutex so admission cannot discard that current
    // in the gap between the two presenter cycles.
    std::uint64_t presenter_pending_current_pair_sequence{};
    std::shared_ptr<PresenterSubmission> presenter_inflight_submission;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_inflight_frame;
    std::thread presenter_thread;
    // presenter_mutex: descriptors only, never image storage.
    xrfg::DjulesOutputBuffer<std::shared_ptr<ThreeSlotFrame>> three_slots;
    std::shared_ptr<SynthesisJob> synthesis_job;
    std::thread synthesis_thread;
    bool three_slot_director{};
    bool fsr_two_slot_presenter{}; // presenter_mutex; immutable while thread runs
    bool queued_nvidia_presenter{}; // Legacy synthesis, fresh S/B consumer; immutable while running.
    bool synthesis_busy{};
    bool director_draining{};
    std::uint64_t next_source_sequence{1};
    // When the presenter last handed a frame to the runtime, and the period it
    // was told to expect. The presenter paces itself against these because
    // xrWaitFrame cannot be relied on to do it: see pace_presenter_submission.
    // Guarded by presenter_mutex.
    // When the next submission is due. Advanced by exactly one period per
    // submission rather than measured from the previous one, so the loop's own
    // cost lands as a constant offset instead of accumulating into the
    // cadence. Chaining each wait off the last submission added about 4.6 ms
    // of per-cycle overhead to a 10 ms pace and held the runtime to 64/s.
    std::chrono::steady_clock::time_point presenter_next_submit{};
    // The best phase this presenter has managed lately: predictedDisplayTime
    // minus the moment the frame aimed at it was actually submitted, reduced
    // modulo the display period. The two clocks have different epochs, so the
    // value is meaningless on its own and exact as a comparison - which is
    // all the phase needs.
    std::int64_t presenter_lead_reference{};
    bool presenter_lead_valid{};
    // When the previous submission actually went out, so the phase controller
    // can tell whether a lead was achieved on the grid or by overshooting it.
    std::chrono::steady_clock::time_point presenter_last_submitted_at{};
    // What the schedule expected the last interval to be. The pair is biased,
    // so "on grid" is not always one period - see the comment at the advance.
    std::chrono::nanoseconds presenter_expected_interval{};
    // Consecutive submissions that landed one scanout apart. Phase only means
    // anything once the rate is right, so the correction waits for a run of
    // them - see the comment at the controller.
    std::uint32_t presenter_on_grid_streak{};
    // The adaptive pair bias and the two smoothed costs it is derived from.
    // The synthetic's downstream xrEndFrame costs more than the real frame's
    // exactly when the runtime is blocking on pixels that are not finished,
    // so the difference is the block and the bias buys it out. The real
    // frame is the baseline rather than a constant because it absorbs
    // whatever the runtime charges per submission regardless - 0.65 to
    // 0.81 ms across every capture here, and different elsewhere.
    std::chrono::nanoseconds presenter_synthetic_call_mean{};
    std::chrono::nanoseconds presenter_real_call_mean{};
    std::chrono::nanoseconds presenter_pair_bias{};
    std::uint32_t presenter_bias_tick{};
    // The other constraint on the bias, and the one the controller was blind
    // to: the interval between the two hand-overs of a pair. Too small and
    // they share a scanout and the compositor keeps one - 11.90 ms measured
    // 0.0% sharing, 8.50 ms measured 76.4%.
    std::chrono::steady_clock::time_point presenter_synthetic_returned_at{};
    std::chrono::nanoseconds presenter_pair_gap_mean{};
    bool presenter_schedule_valid{};
    // The display time the runtime reported for the previous internal
    // xrWaitFrame, and how many slots the pace has been asked to give back.
    //
    // The schedule above is a steady_clock grid. The compositor scans out on
    // the display's clock, and nothing related the two: the phase between
    // them was whatever it happened to be when the presenter thread started,
    // and no path corrected it. A grid sitting just after the compositor's
    // deadline makes every submission miss the scanout it was built for and
    // land in the next, so each scanout sees either nothing new or two
    // frames - continuously, not occasionally, and latched for the life of
    // the session, which is why leaving and re-entering changed everything.
    //
    // predictedDisplayTime is the runtime naming the scanout each frame is
    // for, so consecutive waits advancing by exactly one period is the lock
    // condition, and the delta is the error. Guarded by presenter_mutex.
    XrTime presenter_last_predicted_display{};
    bool presenter_last_predicted_valid{};
    // A condition variable waits on the system tick, which is 15.6 ms by
    // default on Windows. Every pace wait rounded up to that, so an 11.11 ms
    // schedule produced 15.5 ms submissions and exactly 64/s no matter what
    // the schedule asked for. A high-resolution timer sleeps to well under a
    // millisecond without changing the process-wide timer period, which a
    // layer has no business doing to its host.
    HANDLE presenter_pace_timer{};
    // The smallest display period the runtime has reported, which is the one
    // the hardware actually scans at. Not the latest reported value: SteamVR
    // returns a multiple of the true period when it considers the caller
    // behind - 11.1, then 55.6, then 22.2 ms within a few frames - so pacing
    // against the latest value lets a slow frame widen the pace, which makes
    // the next frame later still. That spiral throttled the presenter to
    // 3.7 Hz and froze the session.
    XrDuration presenter_display_period{};
    XrFrameState presenter_frame_state{XR_TYPE_FRAME_STATE};
    XrTime last_virtual_display_time{};
    XrResult presenter_failure{XR_SUCCESS};
    std::uint64_t next_presenter_sequence{1};
    std::size_t outstanding_presenter_submissions{};
    bool presenter_frame_state_valid{};
    // The presenter's frame counter, and the count the application was last
    // released at. The application is handed a doubled period, so it has to be
    // released once per pair, and the validity flag above cannot do that: it is
    // a latch set on the presenter's first wait and cleared only on start and
    // stop, so waiting on it returned in microseconds on every frame after the
    // first. An application slow enough to be the limit never noticed, because
    // its own rendering paced it. One fast enough to keep up ran at a frame per
    // display period against a layer that can consume one per pair, and the
    // surplus frame lost the history ring's capture slot and was rendered and
    // thrown away -- one application frame in six on UEVR, at a steady beat,
    // never two in a row.
    std::uint64_t presenter_frame_serial{};
    std::uint64_t application_served_serial{};
    bool presenter_stop_requested{};
    bool presenter_active{};
    XrSession handle{XR_NULL_HANDLE};
};

// Inside xrEndFrame, and inside each swapchain image call, the runtime drives
// the application's single D3D11 immediate context. ID3D11Multithread makes
// each of the runtime's own D3D11 calls atomic and no more, so the sequence
// one of those OpenXR calls issues interleaves with the sequence another
// issues on the other thread, and the driver's dependency tracking walks a
// chain that has moved. That is what kills nvwgf2umx when a presenter thread
// runs beside the application thread on a D3D11 session.
//
// Holding the layer's own device section could not cover it and recorded no
// contention at all: that guards the layer's D3D11 work, while the work that
// collides is the runtime's, inside the runtime's own calls. The gate has to
// wrap our entry into the runtime, because that is the only place from which
// the runtime's sequence is reachable.
//
// One OpenXR call's worth of D3D11 work is the unit that has to be atomic, so
// this wraps single calls and never a longer span. xrWaitFrame stays outside
// it deliberately: it blocks for most of a display period and issues no D3D11
// work, and holding it here would hand the application thread exactly the
// stall the presenter exists to remove.
thread_local int runtime_entry_depth = 0;

struct RuntimeEntry {
    explicit RuntimeEntry(SessionState* session) noexcept
        : session_(session != nullptr && session->d3d11_device != nullptr
                       ? session
                       : nullptr) {
        if (session_ == nullptr) {
            return;
        }
        outermost_ = runtime_entry_depth == 0;
        const auto before = std::chrono::steady_clock::now();
        session_->runtime_entry_mutex.lock();
        entered_ = std::chrono::steady_clock::now();
        ++runtime_entry_depth;
        if (!outermost_) {
            return;
        }
        const auto waited = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                entered_ - before)
                .count());
        ++session_->runtime_entry_count;
        session_->runtime_entry_wait_ns += waited;
        if (waited > session_->runtime_entry_wait_max_ns) {
            session_->runtime_entry_wait_max_ns = waited;
        }
        // A lock handed over uncontended still costs a few hundred
        // nanoseconds, so only a wait long enough to be a real hand-off
        // counts as one thread having found the other inside the runtime.
        if (waited >= 20'000) {
            ++session_->runtime_entry_contended;
        }
    }

    ~RuntimeEntry() {
        if (session_ == nullptr) {
            return;
        }
        --runtime_entry_depth;
        if (outermost_) {
            const auto held = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - entered_)
                    .count());
            if (held > session_->runtime_entry_hold_max_ns) {
                session_->runtime_entry_hold_max_ns = held;
            }
            // Reported in windows rather than per entry: one record per frame
            // would bury the log, and the question this answers - whether the
            // two threads ever meet here - is a rate, not an event.
            // Eight application frames' worth, so a short capture still
            // reports and a long one costs a handful of records a second.
            if (session_->runtime_entry_count >= 64) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::runtime_entry_section,
                    static_cast<std::int64_t>(session_->runtime_entry_contended),
                    session_->runtime_entry_wait_ns /
                        session_->runtime_entry_count,
                    session_->runtime_entry_wait_max_ns,
                    session_->runtime_entry_hold_max_ns);
                session_->runtime_entry_count = 0;
                session_->runtime_entry_contended = 0;
                session_->runtime_entry_wait_ns = 0;
                session_->runtime_entry_wait_max_ns = 0;
                session_->runtime_entry_hold_max_ns = 0;
            }
        }
        session_->runtime_entry_mutex.unlock();
    }

    RuntimeEntry(const RuntimeEntry&) = delete;
    RuntimeEntry& operator=(const RuntimeEntry&) = delete;

private:
    SessionState* session_{};
    bool outermost_{};
    std::chrono::steady_clock::time_point entered_{};
};

[[nodiscard]] ID3D12CommandQueue* runtime_queue(const SessionState& state) noexcept {
    return state.binding_queue ? state.binding_queue.Get()
                               : state.d3d12_queue.Get();
}

// The application has released one of its images: mark where its queue
// stands, for the join before a downstream xrEndFrame that names it. See
// SessionState::binding_queue. Nothing waits here.
void mark_application_release(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->app_release_fence ||
        !state->d3d12_queue) {
        return;
    }
    try {
        std::scoped_lock lock(state->binding_join_mutex);
        const std::uint64_t value = state->app_release_counter + 1;
        if (SUCCEEDED(state->d3d12_queue->Signal(
                state->app_release_fence.Get(), value))) {
            state->app_release_counter = value;
        }
    } catch (...) {
    }
}

// The runtime has made one of the application's images safe to write, on the
// binding queue. Carry that to the queue the application will write it on.
void carry_reacquire_to_application(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->reacquire_fence ||
        !state->d3d12_queue) {
        return;
    }
    try {
        std::scoped_lock lock(state->binding_join_mutex);
        const std::uint64_t value = state->reacquire_counter + 1;
        if (SUCCEEDED(state->binding_queue->Signal(
                state->reacquire_fence.Get(), value))) {
            state->reacquire_counter = value;
            static_cast<void>(state->d3d12_queue->Wait(
                state->reacquire_fence.Get(), value));
        }
    } catch (...) {
    }
}

// Before a downstream xrEndFrame: the binding queue waits until the
// application's queue has run past the release of every image of the
// application's that the frame names. `value` is app_release_counter as it
// stood at the application's own xrEndFrame for that frame. A repeat waits
// the same value again, which costs nothing.
void join_runtime_queue_to_application(
    SessionState* state,
    std::uint64_t value) noexcept {
    if (state == nullptr || !state->binding_queue || !state->app_release_fence ||
        value == 0) {
        return;
    }
    static_cast<void>(
        state->binding_queue->Wait(state->app_release_fence.Get(), value));
}

// Lets the binding queue finish what it holds before the session it serves
// goes away. Bounded: a wait it holds for the application's queue could
// otherwise keep this here for as long as the application's queue is stuck.
void drain_binding_queue(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->reacquire_fence) {
        return;
    }
    try {
        std::uint64_t value = 0;
        {
            std::scoped_lock lock(state->binding_join_mutex);
            value = state->reacquire_counter + 1;
            if (FAILED(state->binding_queue->Signal(
                    state->reacquire_fence.Get(), value))) {
                return;
            }
            state->reacquire_counter = value;
        }
        if (state->reacquire_fence->GetCompletedValue() >= value) {
            return;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            return;
        }
        if (SUCCEEDED(state->reacquire_fence->SetEventOnCompletion(value, event))) {
            WaitForSingleObject(event, 2000);
        }
        CloseHandle(event);
    } catch (...) {
    }
}


// Final upstream phase acquisition: absolute remaining-time error, one
// quarter per real frame, 250us dead band, half-period maximum correction.
// Folding into one period would hide the compositor's extra-interval failure.
[[nodiscard]] std::chrono::nanoseconds calculate_djules_phase_step(
    std::optional<std::chrono::nanoseconds> left,
    std::chrono::nanoseconds period) noexcept {
    if (!left || period <= std::chrono::nanoseconds::zero()) return {};
    constexpr auto target = std::chrono::nanoseconds(3'000'000);
    constexpr auto dead_band = std::chrono::nanoseconds(250'000);
    const auto error = *left - target;
    if (error >= -dead_band && error <= dead_band) return {};
    return std::clamp(error / 4, -period / 2, period / 2);
}

// Non-SteamVR/non-VD runtimes pace through xrWaitFrame. Impose only a
// half-period floor after the actual previous EndFrame return, not a second
// absolute clock. VD deliberately keeps the steady grid without a release offset.
[[nodiscard]] std::chrono::steady_clock::time_point djules_handover_floor(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_returned,
    std::chrono::nanoseconds period) noexcept {
    if (last_returned == std::chrono::steady_clock::time_point{} ||
        period <= std::chrono::nanoseconds::zero()) return now;
    return (std::max)(now, last_returned + period / 2);
}

// D26's absolute display grid: loop cost never compounds into the period.
// After a slow handover, skip whole slots to retain a quarter-period lead,
// rather than submitting a catch-up burst (final upstream V427 policy).
class DjulesAbsoluteSchedule {
public:
    using Clock = std::chrono::steady_clock;
    Clock::time_point claim(Clock::time_point now, std::chrono::nanoseconds period) noexcept {
        if (!valid_ || period != period_) next_ = now;
        period_ = period;
        valid_ = period > std::chrono::nanoseconds::zero();
        return next_;
    }
    void submitted(Clock::time_point slot, std::chrono::nanoseconds period,
        Clock::time_point returned, std::chrono::nanoseconds phase_step = {}) noexcept {
        valid_ = period > std::chrono::nanoseconds::zero();
        period_ = period;
        if (!valid_) { next_ = returned; return; }
        next_ = slot + period + phase_step;
        const auto earliest = returned + period / 4;
        if (next_ < earliest) {
            const auto behind = std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - next_).count();
            const auto slots = (behind + period.count() - 1) / period.count();
            next_ += period * slots;
        }
    }
private:
    Clock::time_point next_{};
    std::chrono::nanoseconds period_{};
    bool valid_{};
};

[[nodiscard]] bool djules_native_scheduler(const SessionState& state) noexcept {
    return state.nvidia_new_route && !state.luke_afw_enabled &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr &&
        state.optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state.graphics_binding == SessionGraphicsBinding::d3d12;
}

[[nodiscard]] bool djules_triple_active(const SessionState& state) noexcept {
    return state.djules_triple_requested && state.menu_enabled && djules_native_scheduler(state);
}
[[nodiscard]] bool djules_triple_director(const SessionState& state) noexcept {
    return state.three_slot_director && djules_triple_active(state);
}
[[nodiscard]] bool djules_deep_active(const SessionState& state) noexcept {
    return state.djules_deep_pipeline && djules_native_scheduler(state) && !djules_triple_director(state);
}

// D26 clock-source step only: a runtime prediction may already describe a
// throttled application cadence. SteamVR's actual HMD period owns the display
// cadence once available. Absolute phase/deadline policy follows separately.
[[nodiscard]] bool adopt_djules_display_period(SessionState& state,
    std::optional<std::chrono::nanoseconds> measured) noexcept {
    if (!djules_native_scheduler(state) || !state.dispatch->steamvr_runtime ||
        !measured || measured->count() < 2'000'000 || measured->count() > 50'000'000) return false;
    std::scoped_lock lock(state.presenter_mutex);
    const auto prior = state.presenter_display_period;
    if (prior == measured->count()) return false;
    state.presenter_display_period = measured->count();
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_clock_anchor,
        26, static_cast<std::uint64_t>(prior), static_cast<std::uint64_t>(measured->count()), 1);
    return true;
}
void refresh_djules_display_period(const std::shared_ptr<SessionState>& state) noexcept {
    if (!djules_native_scheduler(*state) || !state->dispatch->steamvr_runtime || !state->steamvr_delivery) return;
    static_cast<void>(adopt_djules_display_period(*state, state->steamvr_delivery->display_period()));
}

[[nodiscard]] std::chrono::nanoseconds sample_djules_phase_step(
    const std::shared_ptr<SessionState>& state, bool fresh_real,
    std::uint64_t sequence) noexcept {
    if (!fresh_real || !state->menu_enabled || !djules_native_scheduler(*state) ||
        !state->dispatch->steamvr_runtime || !state->steamvr_delivery) return {};
    const auto left = state->steamvr_delivery->frame_time_remaining();
    const auto period = std::chrono::nanoseconds(state->presenter_display_period);
    const auto step = calculate_djules_phase_step(left, period);
    if (left) xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_clock_anchor,
        27, sequence, static_cast<std::uint64_t>(left->count() - 3'000'000),
        static_cast<std::uint64_t>(step.count()));
    return step;
}

// Observer only: compositor records never feed this variant's pacing policy.
// Poll on the application frame thread, at most four times a second, and only
// while diagnostics are enabled. Session establishment remains the D33 gate.
void log_compositor_delivery(SessionState& state) noexcept {
    if (!xrfg::bridge_flight_logger().enabled() || !state.steamvr_delivery) return;
    try {
        const auto now = std::chrono::steady_clock::now();
        if (now < state.compositor_diagnostic_next_poll) return;
        state.compositor_diagnostic_next_poll = now + std::chrono::milliseconds(250);
        const auto pack = [](float ms) -> std::uint64_t {
            return static_cast<std::uint64_t>(std::clamp(
                static_cast<double>(ms) * 100.0 + 32768.0, 0.0, 65535.0));
        };
        auto& logger = xrfg::bridge_flight_logger();
        if (const auto vsync = state.steamvr_delivery->vsync_anchor()) {
            logger.event(xrfg::BridgeFlightOperation::compositor_vsync, 0,
                vsync->frame_counter,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    vsync->at.time_since_epoch()).count()),
                static_cast<std::uint64_t>(vsync->cost.count()));
        }
        for (const auto& frame : state.steamvr_delivery->settled_frames()) {
            logger.event(xrfg::BridgeFlightOperation::compositor_presentation,
                frame.mispresented, frame.frame_index,
                static_cast<std::uint64_t>(frame.reprojection_flags) |
                    (static_cast<std::uint64_t>(frame.skipped) << 32),
                static_cast<std::uint64_t>(frame.presents) |
                    (static_cast<std::uint64_t>(frame.dropped) << 32));
            logger.event(xrfg::BridgeFlightOperation::compositor_timeline, 0,
                frame.frame_index,
                static_cast<std::uint64_t>(std::max(0.0, frame.system_time_seconds) * 1e6),
                pack(frame.wait_get_poses_called_ms) |
                    (pack(frame.new_frame_ready_ms) << 16) |
                    (pack(frame.compositor_render_start_ms) << 32));
            logger.event(xrfg::BridgeFlightOperation::compositor_timeline, 1,
                frame.frame_index,
                static_cast<std::uint64_t>(frame.total_render_gpu_us) |
                    (static_cast<std::uint64_t>(frame.compositor_render_gpu_us) << 32),
                static_cast<std::uint64_t>(frame.ready_vsyncs) |
                    (static_cast<std::uint64_t>(frame.vsyncs_to_first_view) << 32));
        }
    } catch (...) {} // An optional observer cannot fail an application frame.
}

// The gate covers exactly one call into the runtime, so a call is what it
// takes. Several frame submissions reach the runtime through the overlay
// rather than through the dispatch pointer, and those are the ones a live
// session actually uses, so the whole submitting expression is handed over
// rather than the dispatch call inside it.
template <typename Call>
[[nodiscard]] auto with_runtime_entry(SessionState* session, Call&& call)
    -> decltype(call()) {
    const auto token = session && session->d3d11_device
        ? xrfg::bridge_flight_logger().begin(xrfg::BridgeFlightOperation::timing_runtime_gate)
        : xrfg::BridgeFlightToken{};
    const RuntimeEntry gate(session);
    xrfg::bridge_flight_logger().end(token,
        xrfg::BridgeFlightOperation::timing_runtime_gate, 0);
    return call();
}

// The frame paths hold the session by shared_ptr and the swapchain paths by
// raw pointer; the gate does not care which.
template <typename Call>
[[nodiscard]] auto with_runtime_entry(
    const std::shared_ptr<SessionState>& session, Call&& call)
    -> decltype(call()) {
    return with_runtime_entry(session.get(), std::forward<Call>(call));
}

enum class PrivateOwnershipPhase {
    idle,
    acquired,
    waited,
    release_pending,
};

struct PrivateSwapchainState {
    XrSwapchain handle{XR_NULL_HANDLE};
    PrivateOwnershipPhase phase{PrivateOwnershipPhase::idle};
    std::uint32_t acquired_index{};
    bool first_use_logged{};
};

// A buffered pair owns one current and one synthetic private swapchain. An
// OpenXR composition layer names a swapchain rather than an image index, and
// the runtime binds whichever image was released last when xrEndFrame runs.
// Reusing either handle while an older pair is queued would therefore repoint
// that queued output at newer content. Two complete slots preserve FIFO order
// while the application produces independently of the OpenXR consumer.
constexpr std::size_t kCurrentSlotCount = 2;
constexpr std::size_t kSyntheticSlotCount = 2;
constexpr std::size_t kAfwScratchSlot = kCurrentSlotCount;

struct CompositionCopy12 {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    std::uint64_t ready_value{};
};

struct DjulesOutputStaging {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::atomic<std::uint64_t> last_handover_copy{};
};

struct FrameGenerationSwapchainState {
    bool staged_outputs{};
    std::array<std::shared_ptr<DjulesOutputStaging>,2> current_staging;
    std::array<std::shared_ptr<DjulesOutputStaging>,4> synthetic_staging;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> current_runtime_images, synthetic_runtime_images;
    Microsoft::WRL::ComPtr<ID3D12Fence> handover_fence;
    std::array<CompositionCopy12,8> handover_copies;
    std::uint64_t handover_serial{};
    std::size_t handover_copy_slot{};
    // AFW scratch has its own handle. It cannot repoint either queued real.
    std::array<PrivateSwapchainState, kCurrentSlotCount + 1> current{};
    // Destination images are addressed by a flat index across every slot, so
    // slot s image i is s * current_images_per_slot + i.
    std::uint32_t current_images_per_slot{};
    std::size_t current_slot{};
    std::array<PrivateSwapchainState, kSyntheticSlotCount> synthetic{};
    std::uint32_t synthetic_images_per_slot{};
    std::size_t synthetic_slot{};
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> d3d11_interop;
    // Luke AFW writes the final eye into the current interop mirror. Its
    // optical-flow history must be captured there, never from the AER source.
    std::shared_ptr<xrfg::D3D12SwapchainHistory> afw_final_history;
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> afw_temporal_synthesizer;
};


struct CompositionImageSlot {
    PrivateSwapchainState image;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures12;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
    CompositionCopy12 copy12;
    std::uint64_t ready_value{};
};

struct CompositionCapture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture12;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
    std::array<CompositionCopy12, 3> copies12;
    std::size_t next_copy12{};
    std::uint64_t fence_value{};
    bool valid{};
    // Two retained endpoint colors plus the producer's completed, not-yet-
    // admitted AFW packet. S/B share their endpoint lease. This is storage,
    // not another director slot: admission and physical cadence are unchanged.
    std::array<std::shared_ptr<CompositionImageSlot>, 3> outputs;
};

template<class Fence>
[[nodiscard]] HRESULT wait_composition_fence(Fence* fence, std::uint64_t value) {
    if (!fence || !value) return S_OK;
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (completed >= value) return S_OK;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT result = fence->SetEventOnCompletion(value, event);
    if (SUCCEEDED(result) && WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        result = E_FAIL;
    CloseHandle(event);
    return result;
}

// These copies execute on the application/runtime queue, not the optical-flow
// queue. A bounded allocator ring can wait only for its own previous copy.
[[nodiscard]] HRESULT copy_composition12(
    ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence,
    std::uint64_t& serial, CompositionCopy12& work,
    ID3D12Resource* destination, D3D12_RESOURCE_STATES destination_state,
    ID3D12Resource* source, D3D12_RESOURCE_STATES source_state) {
    HRESULT result = wait_composition_fence(fence, work.ready_value);
    if (FAILED(result)) return result;
    if (!work.allocator) {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&work.allocator));
        if (SUCCEEDED(result)) result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            work.allocator.Get(), nullptr, IID_PPV_ARGS(&work.list));
        if (FAILED(result)) return result;
        work.list->SetName(L"OFXR Owned Composition Copy");
    } else {
        result = work.allocator->Reset();
        if (SUCCEEDED(result)) result = work.list->Reset(work.allocator.Get(), nullptr);
        if (FAILED(result)) return result;
    }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, destination_state, D3D12_RESOURCE_STATE_COPY_DEST};
    work.list->ResourceBarrier(2, barriers);
    work.list->CopyResource(destination, source);
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    work.list->ResourceBarrier(2, barriers);
    result = work.list->Close();
    if (FAILED(result)) return result;
    ID3D12CommandList* lists[] = {work.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    result = queue->Signal(fence, serial + 1);
    if (SUCCEEDED(result)) work.ready_value = ++serial;
    return result;
}

void log_completed_nvidia_gpu_timings(
    const std::shared_ptr<xrfg::D3D12FrameSynthesizer>& synthesizer) noexcept {
    if (!synthesizer || !xrfg::bridge_flight_logger().gpu_instrumentation_enabled()) {
        return;
    }
    for (;;) {
        xrfg::D3D12NvidiaGpuTiming timing{};
        const HRESULT result =
            synthesizer->consume_nvidia_gpu_timing(&timing);
        if (result == S_FALSE) {
            return;
        }
        if (FAILED(result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::nvidia_gpu_total,
                result);
            return;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::nvidia_gpu_stages,
            timing.eye_count,
            timing.pack_microseconds,
            timing.eye0_microseconds,
            timing.eye1_microseconds);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::nvidia_gpu_total,
            0,
            timing.composition_microseconds,
            timing.total_microseconds,
            timing.current_serial);
        // When the GPU actually began and ended this pair's synthesis, on
        // the log's own timeline, so it can be compared directly with the
        // internal_end_frame that handed the synthetic to the runtime.
        if (timing.gpu_begin_qpc != 0 && timing.gpu_end_qpc != 0) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::synthesis_gpu_span,
                static_cast<std::int64_t>(timing.total_microseconds),
                static_cast<std::uint64_t>(
                    xrfg::bridge_flight_logger().microseconds_for_counter(
                        static_cast<std::int64_t>(timing.gpu_begin_qpc))),
                static_cast<std::uint64_t>(
                    xrfg::bridge_flight_logger().microseconds_for_counter(
                        static_cast<std::int64_t>(timing.gpu_end_qpc))),
                timing.current_serial);
        }
    }
}

struct SwapchainState;

enum class VideoMemoryStage : std::int64_t {
    session = 1,
    application_swapchain = 2,
    current_ring = 3,
    synthetic_ring = 4,
    interop = 5,
    history = 6,
    synthesizer = 7,
    swapchain_destroyed = 8,
    periodic = 9,
    // The first acquire of a private swapchain: a runtime that allocates
    // on first use rather than at creation shows up here, c= the swapchain.
    private_first_use = 10,
    // The staging textures of single-swapchain rings.
    staging = 11,
};

void log_video_memory(
    SessionState& session,
    VideoMemoryStage stage,
    std::uint64_t subject) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().enabled()) {
            return;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
        {
            std::scoped_lock lock(session.video_memory_mutex);
            if (!session.video_memory_adapter && !session.video_memory_unavailable) {
                LUID luid{};
                bool found = false;
                if (session.d3d12_device) {
                    luid = session.d3d12_device->GetAdapterLuid();
                    found = true;
                } else if (session.d3d11_device) {
                    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
                    Microsoft::WRL::ComPtr<IDXGIAdapter> device_adapter;
                    DXGI_ADAPTER_DESC description{};
                    if (SUCCEEDED(session.d3d11_device.As(&dxgi_device)) &&
                        SUCCEEDED(dxgi_device->GetAdapter(&device_adapter)) &&
                        SUCCEEDED(device_adapter->GetDesc(&description))) {
                        luid = description.AdapterLuid;
                        found = true;
                    }
                }
                // No device yet: a later record finds one.
                if (!found) {
                    return;
                }
                Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                    FAILED(factory->EnumAdapterByLuid(
                        luid, IID_PPV_ARGS(&session.video_memory_adapter)))) {
                    session.video_memory_adapter.Reset();
                    session.video_memory_unavailable = true;
                    return;
                }
            }
            adapter = session.video_memory_adapter;
        }
        if (!adapter) {
            return;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (FAILED(adapter->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
            return;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::vram_usage,
            static_cast<std::int64_t>(stage),
            local.CurrentUsage,
            local.Budget,
            subject);
    } catch (...) {
    }
}

// swapchain_image for a D3D12 image: what the runtime actually allocated
// against what was asked for. result= DXGI format, a= swapchain, b= width
// <<32 | height, c= mip levels<<48 | array size<<32 | resource flags.
void log_d3d12_image_description(XrSwapchain swapchain, ID3D12Resource* image) noexcept {
    try {
        if (image == nullptr ||
            !xrfg::bridge_flight_logger().enabled()) {
            return;
        }
        const D3D12_RESOURCE_DESC description = image->GetDesc();
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::swapchain_image,
            static_cast<std::int64_t>(description.Format),
            handle_value(swapchain),
            (static_cast<std::uint64_t>(description.Width) << 32) |
                description.Height,
            (static_cast<std::uint64_t>(description.MipLevels) << 48) |
                (static_cast<std::uint64_t>(description.DepthOrArraySize) << 32) |
                static_cast<std::uint32_t>(description.Flags));
    } catch (...) {
    }
}

enum class SwapchainEligibilityReason : std::int64_t {
    ready = 0,
    no_d3d12_binding = 1,
    incomplete_enumeration = 2,
    invalid_d3d12_image = 3,
    ambiguous_attachment_usage = 4,
    protected_content = 5,
    history_initialize_failed = 6,
    depth_only = 7,
    static_image = 8,
    unsupported_face_count = 9,
    missing_dispatch_or_history = 10,
    current_private_swapchain_failed = 11,
    synthetic_private_swapchain_failed = 12,
    synthesis_initialize_failed = 13,
    exception = 14,
    d3d11_interop_initialize_failed = 15,
    invalid_d3d11_image = 16,
    awaiting_projection_use = 17,
    budget_exhausted = 18,
};

void log_swapchain_eligibility(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason reason,
    std::uint64_t detail = 0,
    std::uint64_t auxiliary = 0) noexcept;

struct SwapchainState {
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    // On a bridged D3D11 session (SessionState::d3d11_bridge): the shared
    // textures the application renders into and the copies into the
    // runtime's images. enumerated_d3d12_images are then the shared
    // textures, which is what the D3D12 history captures from.
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    bool private_depth{};
    SwapchainState(
        std::shared_ptr<SessionState> owner,
        const XrSwapchainCreateInfo& input_create_info)
        : session(std::move(owner)),
          create_info(input_create_info) {
        create_info.next = nullptr;
    }

    std::shared_ptr<SessionState> session;
    XrSwapchain handle{XR_NULL_HANDLE};
    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // OpenXR permits acquire/wait/release calls from different threads. Keep the
    // downstream call and the matching ownership bookkeeping in one total order.
    std::mutex call_mutex;
    std::mutex mutex;
    std::deque<std::uint32_t> acquired_indices;
    // Djules 690e1ee9 / V335: waited images remain owned across session
    // begin/end, and several acquire/wait calls may precede the first release.
    std::size_t waited_count{};
    bool ownership_tracking_valid{true};
    std::optional<std::uint32_t> last_released_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> d3d12_history;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> enumerated_d3d11_images;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> enumerated_d3d12_images;
    std::optional<xrfg::D3D12HistoryCaptureTicket> last_released_capture;
    // Latched before producer backpressure, never sampled from EndFrame.
    std::optional<xrfg::LukeRenderIdentity> luke_released_identity;
    std::optional<xrfg::LukeRenderIdentity> luke_admitted_identity;
    std::uint64_t luke_admitted_capture_serial{};
    bool luke_released_duplicate{};
    std::uint64_t last_diagnostic_capture{};
    std::shared_ptr<const xrfg::DlssMotionVectorSet> last_released_motion_vectors;
    std::uint32_t dlss_logical_eye_count{1};
    std::optional<std::uint32_t> dlss_logical_eye_index;
    bool presentation_transform_known{};
    bool presentation_transform_active{};
    std::shared_ptr<FrameGenerationSwapchainState> frame_generation;
    // Release-time private snapshot, never a read of an already released app
    // image. Output leases are held by A and by B/S(A,B) until presentation.
    // call_mutex guards capture/pool mutations; gpu_mutex guards D3D11 calls.
    std::unique_ptr<CompositionCapture> composition_capture;
    bool composition_referenced{};
    // Generation costs three runtime swapchains, and a swapchain that never
    // reaches a projection layer never spends them: an application's UI quads
    // and its stereo views are indistinguishable at enumeration time, so the
    // decision waits until xrEndFrame names this swapchain as a projection
    // view. Guarded by call_mutex.
    bool generation_eligible_pending{};
    std::uint32_t enumerated_image_count{};
    // An attempt that failed for its own reasons -- synthesis, interop -- is
    // not retried every frame. Cleared whenever the history is rebuilt, which
    // is the point at which the images themselves changed.
    bool generation_declined{};
};

using QueryLukeRossD3D12Context = int (*)(
    ID3D12Device**, ID3D12CommandQueue**) noexcept;

[[nodiscard]] bool query_luke_ross_d3d12_context(
    ID3D12Device** device,
    ID3D12CommandQueue** queue) noexcept {
    if (device != nullptr) *device = nullptr;
    if (queue != nullptr) *queue = nullptr;
    if (device == nullptr || queue == nullptr) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&query_luke_ross_d3d12_context),
            &module)) {
        return false;
    }
    const auto query = reinterpret_cast<QueryLukeRossD3D12Context>(
        GetProcAddress(module, "OFXR_GetLukeRossD3D12ContextV1"));
    return query != nullptr && query(device, queue) != 0 &&
        *device != nullptr && *queue != nullptr;
}

void log_swapchain_eligibility(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason reason,
    std::uint64_t detail,
    std::uint64_t auxiliary) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_eligibility,
        static_cast<std::int64_t>(reason),
        state ? handle_value(state->handle) : 0,
        detail,
        auxiliary);
}

std::mutex g_state_mutex;
std::unordered_map<XrInstance, std::shared_ptr<Dispatch>> g_instances;
std::unordered_map<XrSession, std::shared_ptr<SessionState>> g_sessions;
std::unordered_map<XrSwapchain, std::shared_ptr<SwapchainState>> g_swapchains;
struct ObservedSpace {
    std::shared_ptr<Dispatch> dispatch;
    std::weak_ptr<SessionState> session;
};
std::unordered_map<XrSpace, ObservedSpace> g_observed_spaces; // g_state_mutex
std::atomic<std::uint64_t> g_space_sample_id{1};

template <typename Function>
[[nodiscard]] XrResult guard_c_api_boundary(Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] std::shared_ptr<Dispatch> find_dispatch(XrInstance instance) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_instances.find(instance);
    return iterator == g_instances.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::shared_ptr<SessionState> find_session(XrSession session) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_sessions.find(session);
    return iterator == g_sessions.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::shared_ptr<SwapchainState> find_swapchain(XrSwapchain swapchain) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_swapchains.find(swapchain);
    return iterator == g_swapchains.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::vector<std::shared_ptr<SwapchainState>> find_swapchains(
    const std::shared_ptr<SessionState>& session) {
    std::vector<std::shared_ptr<SwapchainState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_swapchains) {
        (void)handle;
        if (state->session == session) {
            matches.push_back(state);
        }
    }
    return matches;
}

[[nodiscard]] std::vector<std::shared_ptr<SwapchainState>> find_swapchains(
    const std::shared_ptr<Dispatch>& dispatch) {
    std::vector<std::shared_ptr<SwapchainState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_swapchains) {
        (void)handle;
        if (state->session->dispatch == dispatch) {
            matches.push_back(state);
        }
    }
    return matches;
}

// Submit a deferred real-frame copy without waiting for its fence. Moving the
// synthesis wait to capture must not move this copy with it: the game render
// interval gives the copy time to complete before its runtime hand-off.
[[nodiscard]] HRESULT flush_session_pending_copies(
    const std::shared_ptr<SessionState>& session) noexcept {
    try {
        HRESULT aggregate = S_OK;
        for (const auto& swapchain : find_swapchains(session)) {
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (!generation || !generation->synthesizer) {
                continue;
            }
            const HRESULT result =
                generation->synthesizer->flush_current_copy(nullptr);
            if (FAILED(result)) {
                aggregate = result;
            }
        }
        return aggregate;
    } catch (...) {
        return E_FAIL;
    }
}

[[nodiscard]] HRESULT wait_for_previous_session_synthesis(
    const std::shared_ptr<SessionState>& session) noexcept {
    try {
        HRESULT aggregate = S_OK;
        for (const auto& swapchain : find_swapchains(session)) {
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (!generation) continue;
            // Promotion must drain both producers of ordinary director
            // destinations. In Luke, the inline OFXR endpoint belongs to
            // afw_temporal_synthesizer, not the AER-to-AFW copy synthesizer.
            // A completed AFW input does not imply that endpoint is idle.
            for (const auto& synthesizer : {
                     generation->synthesizer, generation->afw_temporal_synthesizer}) {
                if (!synthesizer) continue;
                const HRESULT result = synthesizer->wait_for_previous_submission(
                    kFrameStartSynthesisWaitMilliseconds);
                if (FAILED(result)) {
                    aggregate = result;
                    if (result != HRESULT_FROM_WIN32(ERROR_BUSY)) return result;
                }
            }
        }
        return aggregate;
    } catch (...) {
        return E_FAIL;
    }
}

[[nodiscard]] std::vector<std::shared_ptr<SessionState>> find_sessions(
    const std::shared_ptr<Dispatch>& dispatch) {
    std::vector<std::shared_ptr<SessionState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_sessions) {
        (void)handle;
        if (state->dispatch == dispatch) {
            matches.push_back(state);
        }
    }
    return matches;
}

void three_slot_director_main(const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool fsr_two_slot_session(const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool queued_nvidia_session(const std::shared_ptr<SessionState>& state) noexcept;
void synthesis_worker_main(const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] XrResult wait_for_synthesis_handoff(
    const std::shared_ptr<SessionState>& state) noexcept;

// A promoted frame owner inherits an already-forwarded, unbegun runtime
// wait exactly once. Issuing another wait first would deadlock pipelined games.
XrResult wait_presenter_runtime_frame(const std::shared_ptr<SessionState>& state,
    const XrFrameWaitInfo* wait, XrFrameState* frame) {
    {
        std::scoped_lock lock(state->presenter_mutex);
        if (state->presenter_adopted_frame) {
            *frame = *state->presenter_adopted_frame;
            state->presenter_adopted_frame.reset();
            frame->next = nullptr;
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                500, frame->predictedDisplayTime, frame->predictedDisplayPeriod, frame->shouldRender);
            return XR_SUCCESS;
        }
    }
    return state->dispatch->wait_frame(state->handle, wait, frame);
}

[[nodiscard]] XrDuration doubled_display_period(XrDuration period) noexcept;
[[nodiscard]] XrDuration djules_application_period(const SessionState& state,XrDuration period) noexcept {
    if(!djules_triple_active(state)) return doubled_display_period(period);
    if(period>std::numeric_limits<XrDuration>::max()/3) return std::numeric_limits<XrDuration>::max();
    return period>0?period*3:period;
}

[[nodiscard]] bool presenter_forbidden(const SessionState& state) noexcept {
    return state.graphics_binding == SessionGraphicsBinding::d3d11 &&
        (state.single_threaded_d3d11 || state.split_frame_loop);
}

[[nodiscard]] bool djules_inline_runtime(const SessionState& state) noexcept {
    return state.nvidia_new_route && !state.luke_afw_enabled &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr &&
        state.dispatch->inline_unless_pipelined && !state.pipelined_presenter_mode;
}

// frame_call_mutex protects the application thread shape. Observe the first
// EndFrame too, so a split loop cannot promote before its next application Wait.
void observe_application_frame_thread(SessionState& state, bool ending) noexcept {
    auto& id = ending ? state.application_end_thread_id : state.application_wait_thread_id;
    id = GetCurrentThreadId();
    if (!state.split_frame_loop && state.application_wait_thread_id && state.application_end_thread_id &&
        state.application_wait_thread_id != state.application_end_thread_id) {
        state.split_frame_loop = true;
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
            600, state.application_wait_thread_id, state.application_end_thread_id,
            static_cast<std::uint64_t>(state.graphics_binding));
    }
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame = nullptr,
    bool preserve_virtual_timeline = false) noexcept;
void stop_continuous_presenter(
    const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool continuous_presenter_active(
    const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool three_slot_generation_session(
    const std::shared_ptr<SessionState>& state) noexcept;
// Prevent new enqueues, finish queued/in-flight submissions, then retire any
// autonomous repeat before invalidating an application-owned handle.
struct PresenterResourceLifetimeGuard {
    explicit PresenterResourceLifetimeGuard(const std::shared_ptr<SessionState>& state);
    std::unique_lock<std::mutex> frame_lock;
    std::unique_lock<std::mutex> content_lock;
};
void schedule_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail = 0) noexcept;
void clear_generation_continuity(
    const std::shared_ptr<SessionState>& state) noexcept;
void enter_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail = 0) noexcept;

[[nodiscard]] XrTime add_display_duration(
    XrTime time,
    XrDuration duration) noexcept;

void drain_swapchain_gpu(const std::shared_ptr<SwapchainState>& state) noexcept {
    const auto flight_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::gpu_drain,
        state && state->session ? handle_value(state->session->handle) : 0);
    try {
        std::scoped_lock gpu_lock(state->session->gpu_mutex);
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        {
            std::scoped_lock lock(state->mutex);
            history = state->d3d12_history;
            generation = state->frame_generation;
        }
        HRESULT result = S_OK;
        if (generation && generation->afw_temporal_synthesizer) {
            result = generation->afw_temporal_synthesizer->wait_for_idle();
        }
        if (generation && generation->synthesizer) {
            const HRESULT source_result =
                generation->synthesizer->wait_for_idle();
            if (SUCCEEDED(result)) result = source_result;
        }
        if (generation && generation->afw_final_history) {
            const HRESULT final_result =
                generation->afw_final_history->wait_for_idle();
            if (SUCCEEDED(result)) result = final_result;
        }
        if (history) {
            const HRESULT history_result = history->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = history_result;
            }
        }
        if (generation && generation->d3d11_interop) {
            const HRESULT interop_result =
                generation->d3d11_interop->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = interop_result;
            }
        }
        std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
        {
            std::scoped_lock lock(state->mutex);
            bridge = state->bridge;
        }
        if (bridge) {
            const HRESULT bridge_result = bridge->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = bridge_result;
            }
        }
        std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
        { std::scoped_lock lock(state->mutex); vulkan_bridge = state->vulkan_bridge; }
        if (vulkan_bridge) {
            const HRESULT drained = vulkan_bridge->wait_for_idle();
            if (SUCCEEDED(result)) result = drained;
        }
        xrfg::bridge_flight_logger().end(
            flight_token,
            xrfg::BridgeFlightOperation::gpu_drain,
            result);
    } catch (...) {
        xrfg::bridge_flight_logger().end(
            flight_token,
            xrfg::BridgeFlightOperation::gpu_drain,
            E_FAIL);
    }
}

[[nodiscard]] bool release_private_image(
    SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image) noexcept {
    try {
        if (image.handle == XR_NULL_HANDLE || image.phase == PrivateOwnershipPhase::idle) {
            return true;
        }
        if (!dispatch || dispatch->wait_swapchain_image == nullptr ||
            dispatch->release_swapchain_image == nullptr) {
            return false;
        }
        if (image.phase == PrivateOwnershipPhase::acquired) {
            XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait_info.timeout = XR_INFINITE_DURATION;
            const auto wait_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::private_swapchain_wait,
                handle_value(image.handle),
                image.acquired_index,
                static_cast<std::uint64_t>(image.phase));
            const XrResult wait_result = with_runtime_entry(session, [&] {
                return dispatch->wait_swapchain_image(image.handle, &wait_info);
            });
            xrfg::bridge_flight_logger().end(
                wait_token,
                xrfg::BridgeFlightOperation::private_swapchain_wait,
                wait_result,
                handle_value(image.handle),
                image.acquired_index,
                static_cast<std::uint64_t>(image.phase));
            if (wait_result != XR_SUCCESS && wait_result != XR_SESSION_LOSS_PENDING) {
                return false;
            }
            image.phase = PrivateOwnershipPhase::waited;
        }
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto release_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_release,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        const XrResult release_result = with_runtime_entry(session, [&] {
            const auto released=dispatch->release_swapchain_image(image.handle, &release_info);
            // Record inside the same runtime gate as actual xrEndFrame, so
            // cross-thread order reflects the runtime's last-released image.
            if(xrfg::bridge_flight_logger().ram_enabled())
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    released,68,handle_value(image.handle),image.acquired_index);
            return released;
        });
        xrfg::bridge_flight_logger().end(
            release_token,
            xrfg::BridgeFlightOperation::private_swapchain_release,
            release_result,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (XR_FAILED(release_result)) {
            image.phase = PrivateOwnershipPhase::release_pending;
            return false;
        }
        image.phase = PrivateOwnershipPhase::idle;
        return true;
    } catch (...) {
        return false;
    }
}

void destroy_composition_capture(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        if (auto capture = std::move(state->composition_capture)) {
            static_cast<void>(wait_composition_fence(capture->fence.Get(), capture->fence_value));
            static_cast<void>(wait_composition_fence(capture->fence12.Get(), capture->fence_value));
            for (auto& slot : capture->outputs) {
                if (!slot) continue;
                static_cast<void>(release_private_image(state->session.get(), state->session->dispatch, slot->image));
                if (slot->image.handle && state->session->dispatch->destroy_swapchain) {
                    static_cast<void>(with_runtime_entry(state->session.get(), [&] {
                        return state->session->dispatch->destroy_swapchain(slot->image.handle);
                    }));
                    slot->image.handle = XR_NULL_HANDLE;
                }
            }
        }
    } catch (...) {
    }
}

void destroy_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        {
            std::scoped_lock lock(state->mutex);
            generation = std::move(state->frame_generation);
        }
        if (!generation || !state->session || !state->session->dispatch) {
            return;
        }

        const auto& dispatch = state->session->dispatch;
        if(generation->staged_outputs)
            static_cast<void>(wait_composition_fence(generation->handover_fence.Get(),generation->handover_serial));
        for (PrivateSwapchainState& image : generation->synthetic) {
            static_cast<void>(
                release_private_image(state->session.get(), dispatch, image));
        }
        for (PrivateSwapchainState& image : generation->current) {
            static_cast<void>(
                release_private_image(state->session.get(), dispatch, image));
        }
        if (dispatch->destroy_swapchain == nullptr) {
            return;
        }
        std::array<PrivateSwapchainState*,
                   kCurrentSlotCount + 1 + kSyntheticSlotCount> owned{};
        for (std::size_t slot = 0; slot < kSyntheticSlotCount; ++slot) {
            owned[slot] = &generation->synthetic[slot];
        }
        for (std::size_t slot = 0; slot < generation->current.size(); ++slot) {
            owned[kSyntheticSlotCount + slot] = &generation->current[slot];
        }
        for (PrivateSwapchainState* image : owned) {
            if (image->handle == XR_NULL_HANDLE) {
                continue;
            }
            static_cast<void>(dispatch->destroy_swapchain(image->handle));
            image->handle = XR_NULL_HANDLE;
        }
    } catch (...) {
    }
}

[[nodiscard]] std::optional<D3D12_RESOURCE_STATES> required_release_state(
    const SwapchainState& state) noexcept {
    const bool is_color =
        (state.create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    const bool is_depth =
        (state.create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    if (is_color == is_depth) {
        return std::nullopt;
    }
    return is_color ? D3D12_RESOURCE_STATE_RENDER_TARGET
                    : D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

struct CreatedPrivateSwapchain {
    PrivateSwapchainState state;
    std::vector<ID3D12Resource*> d3d12_resources;
    std::vector<ID3D11Texture2D*> d3d11_resources;
};

[[nodiscard]] bool create_private_swapchain(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainCreateInfo& create_info,
    CreatedPrivateSwapchain* output,
    XrResult* refusal = nullptr) {
    if (refusal != nullptr) {
        *refusal = XR_SUCCESS;
    }
    if (!state || !state->session || !state->session->dispatch || output == nullptr) {
        return false;
    }
    const auto& dispatch = state->session->dispatch;
    XrSwapchain handle = XR_NULL_HANDLE;
    XrResult result = dispatch->create_swapchain(
        state->session->handle,
        &create_info,
        &handle);
    if (XR_FAILED(result) || handle == XR_NULL_HANDLE) {
        // Which error the runtime gave separates a budget it will not exceed
        // from a create info it rejects outright, and only the first is worth
        // handing the whole session's private swapchains back for.
        if (refusal != nullptr) {
            *refusal = XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE;
        }
        return false;
    }

    std::uint32_t image_count = 0;
    result = dispatch->enumerate_swapchain_images(handle, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        dispatch->destroy_swapchain(handle);
        return false;
    }

    if (state->session->graphics_binding == SessionGraphicsBinding::d3d11) {
        std::vector<XrSwapchainImageD3D11KHR> images(image_count);
        for (auto& image : images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            image.next = nullptr;
        }
        result = dispatch->enumerate_swapchain_images(
            handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || image_count != images.size()) {
            dispatch->destroy_swapchain(handle);
            return false;
        }
        output->d3d11_resources.resize(image_count);
        for (std::uint32_t index = 0; index < image_count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR ||
                images[index].texture == nullptr) {
                dispatch->destroy_swapchain(handle);
                output->d3d11_resources.clear();
                return false;
            }
            output->d3d11_resources[index] = images[index].texture;
        }
    } else {
        std::vector<XrSwapchainImageD3D12KHR> images(image_count);
        for (auto& image : images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            image.next = nullptr;
        }
        result = dispatch->enumerate_swapchain_images(
            handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || image_count != images.size()) {
            dispatch->destroy_swapchain(handle);
            return false;
        }
        output->d3d12_resources.resize(image_count);
        for (std::uint32_t index = 0; index < image_count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR ||
                images[index].texture == nullptr) {
                dispatch->destroy_swapchain(handle);
                output->d3d12_resources.clear();
                return false;
            }
            output->d3d12_resources[index] = images[index].texture;
        }
    }
    output->state.handle = handle;
    return true;
}

struct CreatedCurrentRing {
    std::array<CreatedPrivateSwapchain, kCurrentSlotCount + 1> slots{};
    // Every slot's destination images end to end, in slot order, which is the
    // flat addressing both the synthesizer and the D3D11 interop expect.
    std::vector<ID3D12Resource*> d3d12_resources;
    std::vector<ID3D11Texture2D*> d3d11_resources;
    std::uint32_t images_per_slot{};
};

void destroy_current_ring(
    const std::shared_ptr<Dispatch>& dispatch,
    CreatedCurrentRing* ring) noexcept {
    if (ring == nullptr || !dispatch || dispatch->destroy_swapchain == nullptr) {
        return;
    }
    for (CreatedPrivateSwapchain& created : ring->slots) {
        if (created.state.handle == XR_NULL_HANDLE) {
            continue;
        }
        static_cast<void>(dispatch->destroy_swapchain(created.state.handle));
        created.state.handle = XR_NULL_HANDLE;
    }
}

[[nodiscard]] bool create_current_ring(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainCreateInfo& create_info,
    CreatedCurrentRing* output,
    XrResult* refusal = nullptr,
    std::size_t slot_count = kCurrentSlotCount) {
    if (refusal != nullptr) {
        *refusal = XR_SUCCESS;
    }
    if (output == nullptr) {
        return false;
    }
    if (slot_count == 0 || slot_count > output->slots.size()) return false;
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        if (!create_private_swapchain(
                state, create_info, &output->slots[slot], refusal)) {
            return false;
        }
        const CreatedPrivateSwapchain& created = output->slots[slot];
        const std::size_t count = created.d3d12_resources.empty()
            ? created.d3d11_resources.size()
            : created.d3d12_resources.size();
        // A flat destination index assumes one stride for every slot, so a
        // runtime that hands out different image counts is not usable here.
        if (count == 0 ||
            (slot != 0 && count != output->images_per_slot)) {
            return false;
        }
        output->images_per_slot = static_cast<std::uint32_t>(count);
        output->d3d12_resources.insert(
            output->d3d12_resources.end(),
            created.d3d12_resources.begin(),
            created.d3d12_resources.end());
        output->d3d11_resources.insert(
            output->d3d11_resources.end(),
            created.d3d11_resources.begin(),
            created.d3d11_resources.end());
    }
    return true;
}

// Publishes a created ring into the shared generation state.
void adopt_current_ring(
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    CreatedCurrentRing& ring) noexcept {
    for (std::size_t slot = 0; slot < generation->current.size(); ++slot) {
        generation->current[slot] = ring.slots[slot].state;
        ring.slots[slot].state.handle = XR_NULL_HANDLE;
    }
    generation->current_images_per_slot = ring.images_per_slot;
    generation->current_slot = 0;
}

void adopt_synthetic_ring(
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    CreatedCurrentRing& ring) noexcept {
    static_assert(kSyntheticSlotCount == kCurrentSlotCount);
    for (std::size_t slot = 0; slot < kSyntheticSlotCount; ++slot) {
        generation->synthetic[slot] = ring.slots[slot].state;
        ring.slots[slot].state.handle = XR_NULL_HANDLE;
    }
    generation->synthetic_images_per_slot = ring.images_per_slot;
    generation->synthetic_slot = 0;
}

[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_d3d12_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedCurrentRing current;
    CreatedCurrentRing synthetic;
    if (failure_reason != nullptr) {
        *failure_reason = SwapchainEligibilityReason::exception;
    }
    if (failure_detail != nullptr) {
        *failure_detail = 0;
    }
    try {
        const auto& dispatch = state->session->dispatch;
        const bool is_color =
            (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool is_depth =
            (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const bool protected_content =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        const bool static_image =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (!is_color || is_depth || protected_content || static_image ||
            state->create_info.faceCount != 1 ||
            state->session->handle == XR_NULL_HANDLE ||
            dispatch->create_swapchain == nullptr ||
            dispatch->destroy_swapchain == nullptr ||
            dispatch->enumerate_swapchain_images == nullptr ||
            state->session->d3d12_device == nullptr ||
            state->session->d3d12_queue == nullptr ||
            !state->d3d12_history) {
            if (failure_reason != nullptr) {
                *failure_reason = protected_content
                    ? SwapchainEligibilityReason::protected_content
                    : static_image
                        ? SwapchainEligibilityReason::static_image
                        : state->create_info.faceCount != 1
                            ? SwapchainEligibilityReason::unsupported_face_count
                            : SwapchainEligibilityReason::missing_dispatch_or_history;
            }
            return nullptr;
        }

        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        generation->staged_outputs = djules_native_scheduler(*state->session);
        const auto output_slots = generation->staged_outputs ? 1U : kCurrentSlotCount;
        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |=
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrResult refusal = XR_SUCCESS;
        if (!create_current_ring(state, private_info, &current, &refusal,
                state->session->luke_afw_enabled ? kCurrentSlotCount + 1 : output_slots)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_current_ring(dispatch, &current);
            return nullptr;
        }
        log_video_memory(*state->session, VideoMemoryStage::current_ring, handle_value(state->handle));
        if (!create_current_ring(state, private_info, &synthetic, &refusal, output_slots)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_current_ring(dispatch, &current);
            destroy_current_ring(dispatch, &synthetic);
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthetic_ring, handle_value(state->handle));
        if (generation->staged_outputs) {
            for(auto* image:current.d3d12_resources) generation->current_runtime_images.emplace_back(image);
            for(auto* image:synthetic.d3d12_resources) generation->synthetic_runtime_images.emplace_back(image);
            auto description = current.d3d12_resources.front()->GetDesc();
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            HRESULT created = state->session->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&generation->handover_fence));
            current.d3d12_resources.clear(); synthetic.d3d12_resources.clear();
            auto make_stage=[&](auto& stage,auto& destinations) {
                if(FAILED(created)) return;
                stage=std::make_shared<DjulesOutputStaging>();
                created=state->session->d3d12_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,
                    &description,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&stage->resource));
                if(SUCCEEDED(created)) destinations.push_back(stage->resource.Get());
            };
            for(auto& stage:generation->current_staging) make_stage(stage,current.d3d12_resources);
            for(auto& stage:generation->synthetic_staging) make_stage(stage,synthetic.d3d12_resources);
            if(FAILED(created)) {
                if(failure_detail) *failure_detail=static_cast<std::uint64_t>(created);
                destroy_current_ring(dispatch,&synthetic); destroy_current_ring(dispatch,&current);
                return nullptr;
            }
        }
        auto synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        const xrfg::D3D12OpticalFlowBackend backend =
            state->session->optical_flow_backend;
        const auto initialize_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::synthesis_initialize,
            handle_value(state->session->handle),
            (static_cast<std::uint64_t>(state->create_info.width) << 32) |
                state->create_info.height,
            optical_flow_configuration_code(
                backend, state->session->nvidia_options));
        const HRESULT gpu_result = synthesizer->initialize(
            state->session->d3d12_device.Get(),
            state->session->d3d12_synthesis_queue
                ? state->session->d3d12_synthesis_queue.Get()
                : state->session->d3d12_queue.Get(),
            state->d3d12_history,
            std::span<ID3D12Resource* const>(
                current.d3d12_resources.data(),
                current.d3d12_resources.size()),
            std::span<ID3D12Resource* const>(
                synthetic.d3d12_resources.data(),
                synthetic.d3d12_resources.size()),
            static_cast<DXGI_FORMAT>(state->create_info.format),
            // These are the layer's own private swapchains, so the layer
            // picks the state they rest in. On a private synthesis queue
            // that has to be COMMON: D3D12 requires a non-simultaneous-
            // access texture to be in COMMON at the point queue ownership
            // transfers, and these transfer twice a pair - written by the
            // synthesis queue, read by the runtime against the queue the
            // application supplied. synchronize_consumer_queue orders the
            // two; it does not transfer ownership, and a fence alone leaves
            // what the reader sees undefined. Observed as synthetic frames
            // that were generated, paced and complete on time, and still
            // did not reach the headset: The Callisto Protocol under UEVR
            // held 45 with 1257 pairs, 99.6% of submissions on grid and the
            // synthetic marker flashing a few times a second rather than
            // forty-five.
            //
            // Resting in COMMON costs nothing. Every transition in the
            // synthesis lists already starts and ends at this state, and a
            // resource in COMMON is implicitly promoted on first use, so a
            // runtime that wants it as a render target still gets one.
            (generation->staged_outputs || state->session->d3d12_synthesis_queue)
                ? D3D12_RESOURCE_STATE_COMMON
                : D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            state->session->nvidia_options,
            xrfg::bridge_flight_logger().gpu_instrumentation_enabled(),
            state->session->frame_generation_mode,
            (state->session->graphics_binding_capabilities & 8ULL) != 0);
        xrfg::bridge_flight_logger().end(
            initialize_token,
            xrfg::BridgeFlightOperation::synthesis_initialize,
            gpu_result,
            current.d3d12_resources.size(),
            synthetic.d3d12_resources.size(),
            state->create_info.arraySize);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthesis_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(gpu_result);
            }
            destroy_current_ring(dispatch, &synthetic);
            destroy_current_ring(dispatch, &current);
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthesizer, handle_value(state->handle));
        adopt_current_ring(generation, current);
        adopt_synthetic_ring(generation, synthetic);
        if(generation->staged_outputs) {
            generation->current_images_per_slot=1;
            generation->synthetic_images_per_slot=2;
        }
        generation->synthesizer = std::move(synthesizer);
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch &&
            state->session->dispatch->destroy_swapchain != nullptr) {
            destroy_current_ring(state->session->dispatch, &synthetic);
            destroy_current_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_d3d11_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    std::span<ID3D11Texture2D* const> application_images,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedCurrentRing current;
    CreatedCurrentRing synthetic;
    if (failure_reason != nullptr) {
        *failure_reason = SwapchainEligibilityReason::exception;
    }
    if (failure_detail != nullptr) {
        *failure_detail = 0;
    }
    try {
        const auto& session = state->session;
        const auto& dispatch = session->dispatch;
        const bool is_color =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool is_depth =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const bool protected_content =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        const bool static_image =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (!is_color || is_depth || protected_content || static_image ||
            state->create_info.faceCount != 1 || application_images.empty() ||
            session->handle == XR_NULL_HANDLE ||
            dispatch->create_swapchain == nullptr ||
            dispatch->destroy_swapchain == nullptr ||
            dispatch->enumerate_swapchain_images == nullptr ||
            session->d3d11_device == nullptr ||
            session->d3d11_context == nullptr ||
            session->d3d12_device == nullptr ||
            session->d3d12_queue == nullptr) {
            if (failure_reason != nullptr) {
                *failure_reason = protected_content
                    ? SwapchainEligibilityReason::protected_content
                    : static_image
                        ? SwapchainEligibilityReason::static_image
                        : state->create_info.faceCount != 1
                            ? SwapchainEligibilityReason::unsupported_face_count
                            : SwapchainEligibilityReason::missing_dispatch_or_history;
            }
            return nullptr;
        }

        const auto destroy_private = [&]() noexcept {
            destroy_current_ring(dispatch, &synthetic);
            destroy_current_ring(dispatch, &current);
        };

        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrResult refusal = XR_SUCCESS;
        if (!create_current_ring(state, private_info, &current, &refusal,
                state->session->luke_afw_enabled ? kCurrentSlotCount + 1 : kCurrentSlotCount)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }
        log_video_memory(*state->session, VideoMemoryStage::current_ring, handle_value(state->handle));
        if (!create_current_ring(state, private_info, &synthetic, &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthetic_ring, handle_value(state->handle));
        auto interop =
            std::make_shared<xrfg::D3D11D3D12SwapchainInterop>();
        xrfg::D3D11InteropInitializationStage interop_failure_stage =
            xrfg::D3D11InteropInitializationStage::complete;
        HRESULT gpu_result = interop->initialize(
            session->d3d11_device.Get(),
            session->d3d11_context.Get(),
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            application_images,
            std::span<ID3D11Texture2D* const>(
                current.d3d11_resources.data(),
                current.d3d11_resources.size()),
            std::span<ID3D11Texture2D* const>(
                synthetic.d3d11_resources.data(),
                synthetic.d3d11_resources.size()),
            &interop_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::d3d11_interop_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(interop_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::interop, handle_value(state->handle));
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        xrfg::D3D12HistoryInitializationStage history_failure_stage =
            xrfg::D3D12HistoryInitializationStage::complete;
        gpu_result = history->initialize(
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            interop->source_images(),
            D3D12_RESOURCE_STATE_COMMON,
            &history_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::history_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(history_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::history, handle_value(state->handle));
        auto synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        const xrfg::D3D12OpticalFlowBackend backend =
            session->optical_flow_backend;
        const auto initialize_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::synthesis_initialize,
            handle_value(session->handle),
            (static_cast<std::uint64_t>(state->create_info.width) << 32) |
                state->create_info.height,
            optical_flow_configuration_code(
                backend, session->nvidia_options));
        gpu_result = synthesizer->initialize(
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            history,
            interop->current_destination_images(),
            interop->synthetic_destination_images(),
            static_cast<DXGI_FORMAT>(state->create_info.format),
            D3D12_RESOURCE_STATE_COMMON,
            backend,
            session->nvidia_options,
            xrfg::bridge_flight_logger().gpu_instrumentation_enabled(),
            session->luke_afw_enabled
                ? xrfg::D3D12FrameGenerationMode::ofxr
                : session->frame_generation_mode,
            (session->graphics_binding_capabilities & 8ULL) != 0);
        xrfg::bridge_flight_logger().end(
            initialize_token,
            xrfg::BridgeFlightOperation::synthesis_initialize,
            gpu_result,
            interop->current_destination_images().size(),
            interop->synthetic_destination_images().size(),
            state->create_info.arraySize);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthesis_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthesizer, handle_value(state->handle));
        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        adopt_current_ring(generation, current);
        adopt_synthetic_ring(generation, synthetic);
        generation->synthesizer = std::move(synthesizer);
        generation->d3d11_interop = std::move(interop);
        if (session->luke_afw_enabled) {
            auto final_history =
                std::make_shared<xrfg::D3D12SwapchainHistory>();
            auto temporal =
                std::make_shared<xrfg::D3D12FrameSynthesizer>();
            const HRESULT history_result = final_history->initialize(
                session->d3d12_device.Get(), session->d3d12_queue.Get(),
                generation->d3d11_interop->current_destination_images(),
                D3D12_RESOURCE_STATE_COMMON);
            const HRESULT temporal_result = SUCCEEDED(history_result)
                ? temporal->initialize(
                    session->d3d12_device.Get(), session->d3d12_queue.Get(),
                    final_history,
                    generation->d3d11_interop->current_destination_images(),
                    generation->d3d11_interop->synthetic_destination_images(),
                    static_cast<DXGI_FORMAT>(state->create_info.format),
                    D3D12_RESOURCE_STATE_COMMON, backend,
                    session->nvidia_options,
                    xrfg::bridge_flight_logger().gpu_instrumentation_enabled(),
                    xrfg::D3D12FrameGenerationMode::ofxr, true)
                : history_result;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                temporal_result, 50,
                handle_value(state->handle), 0);
            if (SUCCEEDED(temporal_result)) {
                generation->afw_final_history = std::move(final_history);
                generation->afw_temporal_synthesizer = std::move(temporal);
            }
        }
        {
            std::scoped_lock lock(state->mutex);
            state->d3d12_history = std::move(history);
        }
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch &&
            state->session->dispatch->destroy_swapchain != nullptr) {
            destroy_current_ring(state->session->dispatch, &synthetic);
            destroy_current_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

// A refused private swapchain is the only failure that says anything about the
// rest of the session: whatever ceiling the runtime reached, the application's
// own creations are competing for it. Synthesis, interop and an unusable image
// are local to the one swapchain that hit them.
[[nodiscard]] bool budget_refusal(SwapchainEligibilityReason reason) noexcept {
    return reason ==
               SwapchainEligibilityReason::current_private_swapchain_failed ||
           reason ==
               SwapchainEligibilityReason::synthetic_private_swapchain_failed;
}

// Hands every private swapchain in the session back to the runtime.
//
// The caller must own the frame call mutex and, when a presenter is running,
// have drained it and taken the content lock first: a queued submission names
// these swapchains, and destroying one the runtime still has queued latches a
// presenter failure for the rest of the session.
void release_session_generation_budget(
    const std::shared_ptr<SessionState>& session) noexcept {
    try {
        if (!session) {
            return;
        }
        {
            // The retained repeat names private swapchains of its own.
            std::scoped_lock lock(session->presenter_mutex);
            session->presenter_last_frame.reset();
        }
        for (const auto& swapchain : find_swapchains(session)) {
            std::scoped_lock call_lock(swapchain->call_mutex);
            swapchain->generation_eligible_pending = false;
            swapchain->generation_declined = true;
            drain_swapchain_gpu(swapchain);
            destroy_frame_generation_swapchains(swapchain);
        }
    } catch (...) {
    }
}

// Creates the generation resources a swapchain deferred at enumeration time.
// Returns false only when the runtime refused a private swapchain, which is
// the caller's signal to hand the session's whole budget back.
[[nodiscard]] bool ensure_frame_generation(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        if (!state || !state->session) {
            return true;
        }
        std::scoped_lock call_lock(state->call_mutex);
        if (!state->generation_eligible_pending || state->generation_declined) {
            return true;
        }
        {
            std::scoped_lock lock(state->mutex);
            if (state->frame_generation) {
                state->generation_eligible_pending = false;
                return true;
            }
        }
        const std::uint64_t auxiliary =
            (static_cast<std::uint64_t>(state->enumerated_image_count) << 32) |
            state->create_info.arraySize;
        SwapchainEligibilityReason reason =
            SwapchainEligibilityReason::exception;
        std::uint64_t detail = 0;
        // Enumeration ran before the application had submitted anything, so
        // nothing else was touching the queue. This runs on the frame path,
        // where a release on another thread may be capturing into history and
        // synthesizer initialisation submits work of its own.
        std::shared_ptr<FrameGenerationSwapchainState> candidate;
        {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            if (state->session->graphics_binding ==
                SessionGraphicsBinding::d3d11) {
                // Ask the runtime for the images again rather than holding a
                // reference to each of them from enumeration until whenever
                // the application first composites with this swapchain. Those
                // are the application's textures: keeping them alive here
                // outlives what the layer is entitled to hold, and an
                // application that exits without destroying its swapchains
                // then releases them during teardown, which hung the D3D11
                // call chain tests at process exit. xrEnumerateSwapchainImages
                // may be called as often as we like.
                std::vector<XrSwapchainImageD3D11KHR> enumerated(
                    state->enumerated_image_count,
                    XrSwapchainImageD3D11KHR{
                        XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, nullptr, nullptr});
                std::uint32_t count = 0;
                const XrResult enumerate_result =
                    state->session->dispatch->enumerate_swapchain_images(
                        state->handle,
                        state->enumerated_image_count,
                        &count,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(
                            enumerated.data()));
                std::vector<ID3D11Texture2D*> images;
                if (XR_SUCCEEDED(enumerate_result) &&
                    count == state->enumerated_image_count) {
                    images.reserve(count);
                    for (const XrSwapchainImageD3D11KHR& image : enumerated) {
                        images.push_back(image.texture);
                    }
                }
                if (images.empty()) {
                    reason = SwapchainEligibilityReason::invalid_d3d11_image;
                    detail = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(enumerate_result));
                } else {
                    candidate = create_d3d11_frame_generation_swapchains(
                        state,
                        std::span<ID3D11Texture2D* const>(
                            images.data(), images.size()),
                        &reason,
                        &detail);
                }
            } else {
                candidate = create_d3d12_frame_generation_swapchains(
                    state, &reason, &detail);
            }
        }
        if (!candidate) {
            state->generation_declined = true;
            log_swapchain_eligibility(state, reason, detail, auxiliary);
            return !budget_refusal(reason);
        }
        {
            std::scoped_lock lock(state->mutex);
            state->frame_generation = std::move(candidate);
        }
        state->generation_eligible_pending = false;
        log_swapchain_eligibility(
            state,
            SwapchainEligibilityReason::ready,
            state->enumerated_image_count,
            auxiliary);
        return true;
    } catch (...) {
        return true;
    }
}

// Spends the generation budget on the swapchains this frame actually submits
// as projection views. Returns false once the runtime has refused one, meaning
// the caller must release the session's budget and stop generating.
[[nodiscard]] bool ensure_projection_frame_generation(
    const std::shared_ptr<SessionState>& session,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    if (!session ||
        session->generation_budget_exhausted.load(std::memory_order_acquire)) {
        return true;
    }
    for (const ProjectionResourceMapping& mapping : mappings) {
        const auto swapchain = find_swapchain(mapping.application_swapchain);
        if (!swapchain || ensure_frame_generation(swapchain)) {
            continue;
        }
        session->generation_budget_exhausted.store(
            true, std::memory_order_release);
        log_swapchain_eligibility(
            swapchain,
            SwapchainEligibilityReason::budget_exhausted,
            0,
            handle_value(session->handle));
        return false;
    }
    return true;
}

// Whether any swapchain this frame submits as a projection view still owes the
// work ensure_projection_frame_generation would do. Cheap: two locks per
// mapping and no allocation, and it answers true at most once per swapchain
// for the life of a session.
[[nodiscard]] bool projection_frame_generation_pending(
    const std::shared_ptr<SessionState>& session,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    try {
        if (!session || session->generation_budget_exhausted.load(
                            std::memory_order_acquire)) {
            return false;
        }
        for (const ProjectionResourceMapping& mapping : mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) {
                continue;
            }
            std::scoped_lock call_lock(swapchain->call_mutex);
            if (swapchain->generation_eligible_pending &&
                !swapchain->generation_declined) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

template <typename Function>
[[nodiscard]] bool load_function(
    PFN_xrGetInstanceProcAddr get_instance_proc_addr,
    XrInstance instance,
    const char* name,
    Function& output) {
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = get_instance_proc_addr(instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        output = nullptr;
        return false;
    }
    output = reinterpret_cast<Function>(function);
    return true;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_instance(XrInstance instance);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_session(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_session(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL layer_begin_session(
    XrSession session,
    const XrSessionBeginInfo* begin_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_end_session(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL layer_wait_frame(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state);
XRAPI_ATTR XrResult XRAPI_CALL layer_begin_frame(
    XrSession session,
    const XrFrameBeginInfo* begin_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_end_frame(
    XrSession session,
    const XrFrameEndInfo* end_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_swapchain(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_swapchain(XrSwapchain swapchain);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_space(XrSpace space);
XRAPI_ATTR XrResult XRAPI_CALL layer_poll_event(
    XrInstance instance,
    XrEventDataBuffer* event_data);
XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images);
XRAPI_ATTR XrResult XRAPI_CALL layer_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index);
XRAPI_ATTR XrResult XRAPI_CALL layer_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info);

template <typename Function>
XrResult expose_intercept(
    const std::shared_ptr<Dispatch>& dispatch,
    Function next_function,
    Function layer_function,
    PFN_xrVoidFunction* output) {
    if (!dispatch || next_function == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    *output = reinterpret_cast<PFN_xrVoidFunction>(layer_function);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_set_presentation_transform_ofxr(
    XrSession session,
    const XrfgPresentationTransformV1* transform) {
    if (transform == nullptr ||
        transform->version != XRFG_PRESENTATION_TRANSFORM_VERSION_1 ||
        transform->view_count > 2U ||
        (transform->active != 0U && transform->view_count == 0U)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto state = find_session(session);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    std::scoped_lock lock(state->frame_call_mutex);
    state->presentation_transform = {};
    state->presentation_transform.active = transform->active != 0U;
    state->presentation_transform.view_count = transform->view_count;
    for (std::uint32_t view = 0; view < transform->view_count; ++view) {
        std::copy_n(
            transform->clip_positions[view],
            state->presentation_transform.clip_positions[view].size(),
            state->presentation_transform.clip_positions[view].begin());
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_locate_views(
    XrSession session, const XrViewLocateInfo* info, XrViewState* view_state,
    std::uint32_t capacity, std::uint32_t* count, XrView* views) {
    try {
        const auto state = find_session(session);
        if (!state || !state->dispatch->locate_views) return XR_ERROR_HANDLE_INVALID;
        const auto result = state->dispatch->locate_views(
            session, info, view_state, capacity, count, views);
        if (XR_SUCCEEDED(result) && info &&
            info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
            view_state && count && *count == 2 &&
            capacity >= 2 && views &&
            (state->graphics_binding_capabilities & 8ULL) != 0 &&
            (view_state->viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
            float baseline{};
            if (xrfg::luke_xr_eye_baseline(
                    {views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z},
                    {views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z},
                    baseline)) {
                std::scoped_lock lock(state->mutex);
                state->luke_xr_eye_baseline = baseline;
            }
        }
        if (info && (state->graphics_binding_capabilities & 8ULL) != 0 &&
            xrfg::bridge_flight_logger().ram_enabled()) {
            std::uint64_t serial{};
            {
                std::scoped_lock lock(state->mutex);
                serial = state->next_application_frame_serial - 1;
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_locate_context,
                result, serial, static_cast<std::uint64_t>(info->displayTime),
                handle_value(info->space));
            if (XR_SUCCEEDED(result) && count && views) {
                for (std::uint32_t index = 0; index < std::min({*count, capacity, 8u}); ++index) {
                    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    view.pose = views[index].pose;
                    view.fov = views[index].fov;
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_locate_pose,
                        index, serial, static_cast<std::uint64_t>(info->displayTime),
                        projection_pose_hash(view));
                    log_luke_view_bits(serial, 900 + index, view);
                }
            }
        }
        return result;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

void observe_created_space(const std::shared_ptr<SessionState>& state,
    XrSpace space, std::int64_t type, const XrPosef& offset) {
    { std::scoped_lock lock(g_state_mutex);
      g_observed_spaces[space] = {state->dispatch, state}; }
    if ((state->graphics_binding_capabilities & 8ULL) != 0 &&
        xrfg::bridge_flight_logger().ram_enabled()) {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_space_created,
            type, handle_value(space), handle_value(state->handle), 0);
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.pose = offset;
        log_luke_view_bits(handle_value(space), 960, view);
    }
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_reference_space(XrSession session,
    const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || !state->dispatch->create_reference_space) return XR_ERROR_HANDLE_INVALID;
        const auto result = state->dispatch->create_reference_space(session, info, space);
        if (XR_SUCCEEDED(result) && info && space) {
            // Observation failure must not change a successful runtime call.
            try { observe_created_space(state, *space, info->referenceSpaceType,
                info->poseInReferenceSpace); } catch (...) {}
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_action_space(XrSession session,
    const XrActionSpaceCreateInfo* info, XrSpace* space) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || !state->dispatch->create_action_space) return XR_ERROR_HANDLE_INVALID;
        const auto result = state->dispatch->create_action_space(session, info, space);
        if (XR_SUCCEEDED(result) && info && space) {
            try { observe_created_space(state, *space, -1, info->poseInActionSpace); } catch (...) {}
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_locate_space(XrSpace space, XrSpace base,
    XrTime time, XrSpaceLocation* location) {
    return guard_c_api_boundary([&]() -> XrResult {
        ObservedSpace owner;
        {
            std::scoped_lock lock(g_state_mutex);
            if (const auto it = g_observed_spaces.find(space); it != g_observed_spaces.end())
                owner = it->second;
            // Preserve passthrough for unobserved/invalid handles in a sole
            // runtime, without guessing an instance in multi-instance use.
            else if (g_instances.size() == 1) owner.dispatch = g_instances.begin()->second;
        }
        if (!owner.dispatch || !owner.dispatch->locate_space) return XR_ERROR_HANDLE_INVALID;
        const auto result = owner.dispatch->locate_space(space, base, time, location);
        const auto state = owner.session.lock();
        if (state && (state->graphics_binding_capabilities & 8ULL) != 0 &&
            xrfg::bridge_flight_logger().ram_enabled()) {
            const auto id = g_space_sample_id.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t latest_wait{};
            { std::scoped_lock lock(state->mutex);
              latest_wait = state->next_application_frame_serial - 1; }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_space_locate,
                result, id, handle_value(space), handle_value(base));
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_space_time,
                XR_SUCCEEDED(result) && location ? location->locationFlags : 0,
                id, static_cast<std::uint64_t>(time), latest_wait);
            if (XR_SUCCEEDED(result) && location) {
                XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose = location->pose;
                log_luke_view_bits(id, 950, view);
            }
        }
        return result;
    });
}

// Djules 3096ddcf (V334), adapted to the embedded DLL: only loader discovery
// is needed here; do not bring in standalone's OptiScaler delegation/tray.
[[nodiscard]] FARPROC loader_export(HMODULE module, const char* name) noexcept {
    __try {
        return GetProcAddress(module, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

[[nodiscard]] bool enumerate_loader_modules(
    HMODULE* modules, DWORD capacity, DWORD* bytes) noexcept {
    __try {
        return EnumProcessModules(GetCurrentProcess(), modules, capacity, bytes) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] PFN_xrGetInstanceProcAddr find_loader_get_instance_proc_addr() {
    const auto loader_entry = [](HMODULE module) noexcept -> PFN_xrGetInstanceProcAddr {
        if (module == nullptr ||
            loader_export(module, "xrNegotiateLoaderApiLayerInterface") != nullptr ||
            loader_export(module, "xrNegotiateLoaderRuntimeInterface") != nullptr ||
            loader_export(module, "xrEnumerateApiLayerProperties") == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<PFN_xrGetInstanceProcAddr>(
            loader_export(module, "xrGetInstanceProcAddr"));
    };
    if (const auto entry = loader_entry(GetModuleHandleW(L"openxr_loader.dll"))) {
        return entry;
    }
    std::vector<HMODULE> modules(256);
    DWORD bytes = 0;
    if (!enumerate_loader_modules(
            modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) {
        return nullptr;
    }
    if (bytes > modules.size() * sizeof(HMODULE)) {
        modules.resize(bytes / sizeof(HMODULE));
        if (!enumerate_loader_modules(
                modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) {
            return nullptr;
        }
    }
    const std::size_t count = std::min(modules.size(), static_cast<std::size_t>(bytes / sizeof(HMODULE)));
    for (std::size_t index = 0; index < count; ++index) {
        if (const auto entry = loader_entry(modules[index])) {
            return entry;
        }
    }
    return nullptr;
}

// Before an instance exists there is no next-layer dispatch. Upper layers
// (e.g. Cheeky Foveated DLSS) still need the loader's global extension query.
// Never expose instance functions or mistake this embedded DLL for a loader.
XrResult resolve_global_function(const char* name, PFN_xrVoidFunction* function) {
    if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") != 0 &&
        std::strcmp(name, "xrEnumerateApiLayerProperties") != 0) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const PFN_xrGetInstanceProcAddr loader = find_loader_get_instance_proc_addr();
    if (loader == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = loader(XR_NULL_HANDLE, name, function);
    if (XR_FAILED(result) || *function == nullptr) {
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

[[nodiscard]] std::string augment_vulkan_extension_list(
    std::string_view list,
    bool device_list,
    std::uint64_t* appended_bits) {
    std::string augmented(list);
    const VulkanExtensionSummary present =
        summarize_vulkan_extension_string(list);
    const auto append = [&](std::string_view name) {
        const std::uint64_t bit = vulkan_interop_extension_bit(name);
        if ((present.bits & bit) != 0) {
            return;
        }
        if (!augmented.empty()) {
            augmented.push_back(' ');
        }
        augmented.append(name);
        *appended_bits |= bit;
    };
    if (device_list) {
        append("VK_KHR_external_semaphore_win32");
    } else {
        append("VK_KHR_external_semaphore_capabilities");
    }
    return augmented;
}

XrResult get_vulkan_extensions_recorded(
    PFN_GetVulkanExtensions next,
    std::int64_t selector,
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    if (next == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (count == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Read the runtime's whole list whatever the application asked for, so
    // the size query and the fill call both answer for the augmented list.
    std::uint32_t runtime_count = 0;
    XrResult result = next(instance, system_id, 0, &runtime_count, nullptr);
    if (XR_FAILED(result) || runtime_count == 0) {
        return next(instance, system_id, capacity, count, buffer);
    }
    std::string runtime_list(runtime_count, '\0');
    result = next(
        instance, system_id, runtime_count, &runtime_count, runtime_list.data());
    if (XR_FAILED(result)) {
        return next(instance, system_id, capacity, count, buffer);
    }
    runtime_list.resize(runtime_count > 0 ? runtime_count - 1 : 0);
    std::uint64_t appended_bits = 0;
    const bool enabled = GetPrivateProfileIntW(L"ofxr", L"vulkan_bridge", 1,
        (current_layer_directory() / L"ofxr_bridge.ini").c_str()) == 1;
    const std::string augmented = enabled
        ? augment_vulkan_extension_list(runtime_list, selector == 3, &appended_bits)
        : runtime_list;
    *count = static_cast<std::uint32_t>(augmented.size() + 1);
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (buffer == nullptr || capacity < *count) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::memcpy(buffer, augmented.c_str(), augmented.size() + 1);
    // Only the call that fills the buffer is recorded; the size query before
    // it would record the same list twice.
    const VulkanExtensionSummary summary =
        summarize_vulkan_extension_string(runtime_list);
    log_vulkan_negotiation(
        selector, summary.count, summary.bits, runtime_list.size());
    log_vulkan_negotiation(10, static_cast<std::uint64_t>(selector), appended_bits);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_instance_extensions(
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_extensions_recorded(
            dispatch ? dispatch->vulkan.get_instance_extensions : nullptr,
            2, instance, system_id, capacity, count, buffer);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_device_extensions(
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_extensions_recorded(
            dispatch ? dispatch->vulkan.get_device_extensions : nullptr,
            3, instance, system_id, capacity, count, buffer);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_vulkan_instance(
    XrInstance instance,
    const XrVulkanInstanceCreateInfoKHR* create_info,
    VkInstance* vulkan_instance,
    VkResult* vulkan_result) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.create_instance == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->vulkan.create_instance(
            instance, create_info, vulkan_instance, vulkan_result);
        VulkanExtensionSummary summary{};
        if (create_info != nullptr && create_info->vulkanCreateInfo != nullptr) {
            summary = summarize_vulkan_extension_array(
                create_info->vulkanCreateInfo->ppEnabledExtensionNames,
                create_info->vulkanCreateInfo->enabledExtensionCount);
        }
        log_vulkan_negotiation(
            4, summary.count, summary.bits,
            pack_negotiation_results(
                result, vulkan_result != nullptr ? *vulkan_result : VK_SUCCESS));
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_vulkan_device(
    XrInstance instance,
    const XrVulkanDeviceCreateInfoKHR* create_info,
    VkDevice* vulkan_device,
    VkResult* vulkan_result) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.create_device == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->vulkan.create_device(
            instance, create_info, vulkan_device, vulkan_result);
        const VkDeviceCreateInfo* device_info =
            create_info != nullptr ? create_info->vulkanCreateInfo : nullptr;
        VulkanExtensionSummary summary{};
        if (device_info != nullptr) {
            summary = summarize_vulkan_extension_array(
                device_info->ppEnabledExtensionNames,
                device_info->enabledExtensionCount);
        }
        log_vulkan_negotiation(
            5, summary.count, summary.bits,
            pack_negotiation_results(
                result, vulkan_result != nullptr ? *vulkan_result : VK_SUCCESS));
        if (device_info != nullptr && device_info->pQueueCreateInfos != nullptr) {
            for (std::uint32_t index = 0;
                 index < device_info->queueCreateInfoCount;
                 ++index) {
                const auto& queue = device_info->pQueueCreateInfos[index];
                log_vulkan_negotiation(
                    6, queue.queueFamilyIndex, queue.queueCount);
            }
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_device(
    XrInstance instance,
    XrSystemId system_id,
    VkInstance vulkan_instance,
    VkPhysicalDevice* physical_device) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.get_graphics_device == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        log_vulkan_negotiation(7, 1);
        return dispatch->vulkan.get_graphics_device(
            instance, system_id, vulkan_instance, physical_device);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_device2(
    XrInstance instance,
    const XrVulkanGraphicsDeviceGetInfoKHR* get_info,
    VkPhysicalDevice* physical_device) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.get_graphics_device2 == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        log_vulkan_negotiation(7, 2);
        return dispatch->vulkan.get_graphics_device2(
            instance, get_info, physical_device);
    });
}

XrResult get_vulkan_requirements_recorded(
    PFN_GetVulkanGraphicsRequirements next,
    std::uint64_t which,
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    if (next == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = next(instance, system_id, requirements);
    if (XR_SUCCEEDED(result) && requirements != nullptr) {
        log_vulkan_negotiation(
            8, which, requirements->minApiVersionSupported,
            requirements->maxApiVersionSupported);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_requirements(
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_requirements_recorded(
            dispatch ? dispatch->vulkan.get_graphics_requirements : nullptr,
            1, instance, system_id, requirements);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_requirements2(
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_requirements_recorded(
            dispatch ? dispatch->vulkan.get_graphics_requirements2 : nullptr,
            2, instance, system_id, requirements);
    });
}


// A bridged Vulkan session's runtime is D3D12 and lists DXGI formats; the
// application asked a Vulkan session and reads VkFormats. Each DXGI format
// with a Vulkan equivalent is shown once, in the runtime's order.
XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_formats(
    XrSession session,
    std::uint32_t format_capacity_input,
    std::uint32_t* format_count_output,
    std::int64_t* formats) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || state->dispatch->enumerate_swapchain_formats == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!state->vulkan_bridge) {
            return state->dispatch->enumerate_swapchain_formats(
                session, format_capacity_input, format_count_output, formats);
        }
        if (format_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::uint32_t runtime_count = 0;
        XrResult result = state->dispatch->enumerate_swapchain_formats(
            session, 0, &runtime_count, nullptr);
        if (XR_FAILED(result)) {
            return result;
        }
        std::vector<std::int64_t> runtime_formats(runtime_count);
        if (runtime_count > 0) {
            result = state->dispatch->enumerate_swapchain_formats(
                session, runtime_count, &runtime_count, runtime_formats.data());
            if (XR_FAILED(result)) {
                return result;
            }
            runtime_formats.resize(runtime_count);
        }
        std::vector<std::int64_t> translated;
        for (const std::int64_t format : runtime_formats) {
            const VkFormat vulkan_format =
                xrfg::vulkan_format_for_dxgi(static_cast<DXGI_FORMAT>(format));
            if (vulkan_format == VK_FORMAT_UNDEFINED ||
                std::find(translated.begin(), translated.end(),
                          static_cast<std::int64_t>(vulkan_format)) != translated.end()) {
                continue;
            }
            translated.push_back(static_cast<std::int64_t>(vulkan_format));
        }
        *format_count_output = static_cast<std::uint32_t>(translated.size());
        if (format_capacity_input == 0) {
            return XR_SUCCESS;
        }
        if (formats == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (format_capacity_input < translated.size()) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        std::copy(translated.begin(), translated.end(), formats);
        return XR_SUCCESS;
    });
}


XrResult layer_get_instance_proc_addr_impl(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    if (name == nullptr || function == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *function = nullptr;

    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(layer_get_instance_proc_addr);
        return XR_SUCCESS;
    }

    if (instance == XR_NULL_HANDLE) {
        return resolve_global_function(name, function);
    }
    const auto dispatch = find_dispatch(instance);
    if (!dispatch) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (std::strcmp(
            name, XRFG_SET_PRESENTATION_TRANSFORM_FUNCTION_NAME) == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(
            layer_set_presentation_transform_ofxr);
        return XR_SUCCESS;
    }

    if (std::strcmp(name, "xrEnumerateSwapchainFormats") == 0) {
        return expose_intercept(dispatch, dispatch->enumerate_swapchain_formats,
            layer_enumerate_swapchain_formats, function);
    }
    if (std::strcmp(name, "xrGetVulkanInstanceExtensionsKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_instance_extensions, layer_get_vulkan_instance_extensions, function);
    }
    if (std::strcmp(name, "xrGetVulkanDeviceExtensionsKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_device_extensions, layer_get_vulkan_device_extensions, function);
    }
    if (std::strcmp(name, "xrCreateVulkanInstanceKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.create_instance, layer_create_vulkan_instance, function);
    }
    if (std::strcmp(name, "xrCreateVulkanDeviceKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.create_device, layer_create_vulkan_device, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsDeviceKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_device, layer_get_vulkan_graphics_device, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsDevice2KHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_device2, layer_get_vulkan_graphics_device2, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsRequirementsKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_requirements, layer_get_vulkan_graphics_requirements, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsRequirements2KHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_requirements2, layer_get_vulkan_graphics_requirements2, function);
    }
    if (std::strcmp(name, "xrDestroyInstance") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_instance, layer_destroy_instance, function);
    }
    if (std::strcmp(name, "xrCreateSession") == 0) {
        return expose_intercept(dispatch, dispatch->create_session, layer_create_session, function);
    }
    if (std::strcmp(name, "xrDestroySession") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_session, layer_destroy_session, function);
    }
    if (std::strcmp(name, "xrBeginSession") == 0) {
        return expose_intercept(dispatch, dispatch->begin_session, layer_begin_session, function);
    }
    if (std::strcmp(name, "xrEndSession") == 0) {
        return expose_intercept(dispatch, dispatch->end_session, layer_end_session, function);
    }
    if (std::strcmp(name, "xrWaitFrame") == 0) {
        return expose_intercept(dispatch, dispatch->wait_frame, layer_wait_frame, function);
    }
    if (std::strcmp(name, "xrBeginFrame") == 0) {
        return expose_intercept(dispatch, dispatch->begin_frame, layer_begin_frame, function);
    }
    if (std::strcmp(name, "xrEndFrame") == 0) {
        return expose_intercept(dispatch, dispatch->end_frame, layer_end_frame, function);
    }
    if (std::strcmp(name, "xrLocateViews") == 0) {
        return expose_intercept(dispatch, dispatch->locate_views, layer_locate_views, function);
    }
    if (std::strcmp(name, "xrCreateReferenceSpace") == 0) {
        return expose_intercept(dispatch, dispatch->create_reference_space, layer_create_reference_space, function);
    }
    if (std::strcmp(name, "xrCreateActionSpace") == 0) {
        return expose_intercept(dispatch, dispatch->create_action_space, layer_create_action_space, function);
    }
    if (std::strcmp(name, "xrLocateSpace") == 0) {
        return expose_intercept(dispatch, dispatch->locate_space, layer_locate_space, function);
    }
    if (std::strcmp(name, "xrCreateSwapchain") == 0) {
        return expose_intercept(dispatch, dispatch->create_swapchain, layer_create_swapchain, function);
    }
    if (std::strcmp(name, "xrDestroySwapchain") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_swapchain, layer_destroy_swapchain, function);
    }
    if (std::strcmp(name, "xrDestroySpace") == 0) {
        return expose_intercept(
            dispatch, dispatch->destroy_space, layer_destroy_space, function);
    }
    if (std::strcmp(name, "xrPollEvent") == 0) {
        return expose_intercept(
            dispatch, dispatch->poll_event, layer_poll_event, function);
    }
    if (std::strcmp(name, "xrEnumerateSwapchainImages") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->enumerate_swapchain_images,
            layer_enumerate_swapchain_images,
            function);
    }
    if (std::strcmp(name, "xrAcquireSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->acquire_swapchain_image,
            layer_acquire_swapchain_image,
            function);
    }
    if (std::strcmp(name, "xrWaitSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->wait_swapchain_image,
            layer_wait_swapchain_image,
            function);
    }
    if (std::strcmp(name, "xrReleaseSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->release_swapchain_image,
            layer_release_swapchain_image,
            function);
    }

    return dispatch->get_instance_proc_addr(instance, name, function);
}

// view_configuration, once per view at session creation.
void log_view_configuration(
    const Dispatch& dispatch, XrInstance instance, XrSystemId system_id) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().enabled() || system_id == XR_NULL_SYSTEM_ID) {
            return;
        }
        PFN_xrEnumerateViewConfigurationViews enumerate = nullptr;
        if (!load_function(
                dispatch.get_instance_proc_addr, instance,
                "xrEnumerateViewConfigurationViews", enumerate)) {
            return;
        }
        std::array<XrViewConfigurationView, 4> views{};
        for (auto& view : views) {
            view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        }
        std::uint32_t count = 0;
        if (XR_FAILED(enumerate(
                instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                static_cast<std::uint32_t>(views.size()), &count, views.data()))) {
            return;
        }
        for (std::uint32_t index = 0; index < std::min<std::uint32_t>(count, 4); ++index) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::view_configuration,
                index,
                (static_cast<std::uint64_t>(views[index].recommendedImageRectWidth) << 32) |
                    views[index].recommendedImageRectHeight,
                (static_cast<std::uint64_t>(views[index].maxImageRectWidth) << 32) |
                    views[index].maxImageRectHeight,
                views[index].recommendedSwapchainSampleCount);
        }
    } catch (...) {
    }
}


XrResult layer_create_api_layer_instance_impl(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
    if (create_info == nullptr || layer_info == nullptr || instance == nullptr ||
        layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layer_info->structVersion < XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layer_info->structSize < sizeof(XrApiLayerCreateInfo) ||
        layer_info->nextInfo == nullptr ||
        layer_info->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        layer_info->nextInfo->structVersion < XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        layer_info->nextInfo->structSize < sizeof(XrApiLayerNextInfo) ||
        std::strcmp(layer_info->nextInfo->layerName, kLayerName) != 0 ||
        layer_info->nextInfo->nextGetInstanceProcAddr == nullptr ||
        layer_info->nextInfo->nextCreateApiLayerInstance == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrApiLayerCreateInfo next_layer_info = *layer_info;
    next_layer_info.nextInfo = layer_info->nextInfo->next;

    const PFN_xrGetInstanceProcAddr next_get_instance_proc_addr =
        layer_info->nextInfo->nextGetInstanceProcAddr;
    const PFN_xrCreateApiLayerInstance next_create_api_layer_instance =
        layer_info->nextInfo->nextCreateApiLayerInstance;

    auto dispatch = std::make_shared<Dispatch>();
    dispatch->get_instance_proc_addr = next_get_instance_proc_addr;
    // Diagnostic-only capability negotiation. Enable the advertised clock
    // extension before creation; never guess the origin of XrTime or call an
    // extension which wasn't enabled. Unsupported runtimes are logged as such.
    XrInstanceCreateInfo diagnostic_create = *create_info;
    std::vector<const char*> diagnostic_extensions;
    bool bridge_enabled_clock = false;
    if (xrfg::bridge_flight_logger().ram_enabled()) {
        bool requested = false;
        for (std::uint32_t index = 0; index < create_info->enabledExtensionCount; ++index)
            requested |= std::strcmp(create_info->enabledExtensionNames[index],
                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0;
        PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
        if (!requested && load_function(next_get_instance_proc_addr, XR_NULL_HANDLE,
                "xrEnumerateInstanceExtensionProperties", enumerate)) {
            std::uint32_t count{};
            if (XR_SUCCEEDED(enumerate(nullptr, 0, &count, nullptr))) {
                std::vector<XrExtensionProperties> extensions(
                    count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
                if (XR_SUCCEEDED(enumerate(nullptr, count, &count, extensions.data()))) {
                    for (const auto& extension : extensions) {
                        if (std::strcmp(extension.extensionName,
                                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0) {
                            for (std::uint32_t index = 0; index < create_info->enabledExtensionCount; ++index)
                                diagnostic_extensions.push_back(create_info->enabledExtensionNames[index]);
                            diagnostic_extensions.push_back(
                                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
                            diagnostic_create.enabledExtensionCount =
                                static_cast<std::uint32_t>(diagnostic_extensions.size());
                            diagnostic_create.enabledExtensionNames = diagnostic_extensions.data();
                            bridge_enabled_clock = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // The D3D11 bridge needs XR_KHR_D3D12_enable on the instance, which the
    // application did not ask for. Added here, only when the ini asks for
    // the bridge, the application enabled D3D11 and the runtime lists D3D12;
    // anything short of that leaves the instance as the application made it.
    XrInstanceCreateInfo bridged_create_info{};
    std::vector<const char*> bridged_extensions;
    const XrInstanceCreateInfo* forwarded_create_info = bridge_enabled_clock ? &diagnostic_create : create_info;
    const XrInstanceCreateInfo* const negotiated_info = forwarded_create_info;
    bool bridge_extension_added = false;
    bool bridge_requested_by_ini = false;
    bool vulkan_bridge_requested_by_ini = false;
    bool asked_vulkan = false;
    bool asked_d3d11 = false;
    bool asked_d3d12 = false;
    try {
        for (std::uint32_t index = 0; index < create_info->enabledExtensionCount; ++index) {
            const char* const extension = create_info->enabledExtensionNames[index];
            if (extension == nullptr) continue;
            if (std::string_view(extension) == "XR_KHR_vulkan_enable" ||
                std::string_view(extension) == "XR_KHR_vulkan_enable2") asked_vulkan = true;
            if (std::string_view(extension) == "XR_KHR_D3D11_enable") asked_d3d11 = true;
            if (std::string_view(extension) == "XR_KHR_D3D12_enable") asked_d3d12 = true;
        }
        // A game that enabled XR_KHR_D3D12_enable itself - Assetto Corsa
        // and SkyrimVR enable every graphics extension the runtime lists -
        // needs nothing added; the bridge then decides at xrCreateSession
        // on the binding it sees.
        bridge_requested_by_ini =
            GetPrivateProfileIntW(L"ofxr", L"d3d11_bridge", 1, (current_layer_directory() / L"ofxr_bridge.ini").c_str()) == 1;
        vulkan_bridge_requested_by_ini = GetPrivateProfileIntW(L"ofxr", L"vulkan_bridge", 1,
            (current_layer_directory() / L"ofxr_bridge.ini").c_str()) == 1;
        if (!asked_d3d12 && ((asked_d3d11 && bridge_requested_by_ini) ||
                            (asked_vulkan && vulkan_bridge_requested_by_ini))) {
            PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions = nullptr;
            bool runtime_has_d3d12 = false;
            if (XR_SUCCEEDED(next_get_instance_proc_addr(
                    XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                    reinterpret_cast<PFN_xrVoidFunction*>(&enumerate_extensions))) &&
                enumerate_extensions != nullptr) {
                std::uint32_t count = 0;
                if (XR_SUCCEEDED(enumerate_extensions(nullptr, 0, &count, nullptr)) && count > 0) {
                    std::vector<XrExtensionProperties> properties(
                        count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
                    if (XR_SUCCEEDED(enumerate_extensions(
                            nullptr, count, &count, properties.data()))) {
                        for (std::uint32_t index = 0; index < count; ++index) {
                            if (std::string_view(properties[index].extensionName) ==
                                "XR_KHR_D3D12_enable") {
                                runtime_has_d3d12 = true;
                            }
                        }
                    }
                }
            }
            if (runtime_has_d3d12) {
                bridged_extensions.assign(
                    negotiated_info->enabledExtensionNames,
                    negotiated_info->enabledExtensionNames + negotiated_info->enabledExtensionCount);
                bridged_extensions.push_back("XR_KHR_D3D12_enable");
                bridged_create_info = *negotiated_info;
                bridged_create_info.enabledExtensionNames = bridged_extensions.data();
                bridged_create_info.enabledExtensionCount =
                    static_cast<std::uint32_t>(bridged_extensions.size());
                forwarded_create_info = &bridged_create_info;
                bridge_extension_added = true;
            }
        }
    } catch (...) {
        forwarded_create_info = negotiated_info;
        bridge_extension_added = false;
    }

    XrInstance created_instance = XR_NULL_HANDLE;
    const XrResult result = next_create_api_layer_instance(
        forwarded_create_info,
        &next_layer_info,
        &created_instance);
    if (XR_FAILED(result)) {
        return result;
    }

    const bool loaded =
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroyInstance", dispatch->destroy_instance) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrCreateSession", dispatch->create_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroySession", dispatch->destroy_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrBeginSession", dispatch->begin_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrEndSession", dispatch->end_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrWaitFrame", dispatch->wait_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrBeginFrame", dispatch->begin_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrEndFrame", dispatch->end_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrCreateSwapchain", dispatch->create_swapchain) &&
        (static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
             "xrCreateReferenceSpace", dispatch->create_reference_space)), true) &&
        (static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
             "xrCreateActionSpace", dispatch->create_action_space)), true) &&
        (static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
             "xrLocateSpace", dispatch->locate_space)), true) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroySwapchain", dispatch->destroy_swapchain) &&
        // Best effort, deliberately not a load requirement: a runtime without
        // it keeps working, it just never has a space settled before destroy.
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrDestroySpace",
             dispatch->destroy_space)),
         true) &&
        // Best effort, deliberately not a load requirement: a runtime without
        // it keeps working, the recorder just never sees a state transition.
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrPollEvent",
             dispatch->poll_event)),
         true) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrEnumerateSwapchainImages",
            dispatch->enumerate_swapchain_images) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrAcquireSwapchainImage",
            dispatch->acquire_swapchain_image) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrWaitSwapchainImage",
            dispatch->wait_swapchain_image) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrReleaseSwapchainImage",
            dispatch->release_swapchain_image);
    if (!loaded) {
        if (dispatch->destroy_instance != nullptr) {
            dispatch->destroy_instance(created_instance);
        }
        return XR_ERROR_INITIALIZATION_FAILED;
    }

        // The bridge decision, on every instance: a the graphics extensions
        // the application enabled (4 D3D11, 8 D3D12), b whether the ini asks
        // for the bridge, c whether the extension was added. result 0 with
        // c=1 is the bridge armed; -1 is an added extension whose
        // requirements call the runtime then failed to hand out.
        const bool d3d12_on_instance = bridge_extension_added || asked_d3d12;
        if (bridge_requested_by_ini && asked_d3d11 && d3d12_on_instance) {
            dispatch->d3d11_bridge = load_function(
                next_get_instance_proc_addr, created_instance,
                "xrGetD3D12GraphicsRequirementsKHR",
                dispatch->get_d3d12_graphics_requirements);
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::d3d11_bridge,
            bridge_requested_by_ini && asked_d3d11 && d3d12_on_instance &&
                    !dispatch->d3d11_bridge
                ? -1
                : 0,
            (asked_d3d11 ? 4ULL : 0ULL) | (asked_d3d12 ? 8ULL : 0ULL),
            bridge_requested_by_ini ? 1 : 0,
            (bridge_extension_added ? 1ULL : 0ULL) |
                (dispatch->d3d11_bridge ? 2ULL : 0ULL));

    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrEnumerateSwapchainFormats", dispatch->enumerate_swapchain_formats));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanInstanceExtensionsKHR", dispatch->vulkan.get_instance_extensions));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanDeviceExtensionsKHR", dispatch->vulkan.get_device_extensions));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrCreateVulkanInstanceKHR", dispatch->vulkan.create_instance));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrCreateVulkanDeviceKHR", dispatch->vulkan.create_device));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsDeviceKHR", dispatch->vulkan.get_graphics_device));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsDevice2KHR", dispatch->vulkan.get_graphics_device2));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsRequirementsKHR", dispatch->vulkan.get_graphics_requirements));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsRequirements2KHR", dispatch->vulkan.get_graphics_requirements2));
    if (vulkan_bridge_requested_by_ini && asked_vulkan && d3d12_on_instance) {
        dispatch->vulkan_bridge = dispatch->get_d3d12_graphics_requirements != nullptr ||
            load_function(next_get_instance_proc_addr, created_instance,
                "xrGetD3D12GraphicsRequirementsKHR", dispatch->get_d3d12_graphics_requirements);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::vulkan_bridge,
            dispatch->vulkan_bridge ? 0 : -1, asked_vulkan, bridge_extension_added, 0);
    }

    dispatch->instance = created_instance;
    bool application_enabled_clock_conversion = false;
    for (std::uint32_t index = 0;
         index < create_info->enabledExtensionCount; ++index) {
        const char* extension = create_info->enabledExtensionNames[index];
        if (extension != nullptr && std::strcmp(
                extension,
                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0) {
            application_enabled_clock_conversion = true;
            break;
        }
    }
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrLocateViews", dispatch->locate_views));
    if (application_enabled_clock_conversion || bridge_enabled_clock) {
        static_cast<void>(load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrConvertTimeToWin32PerformanceCounterKHR",
            dispatch->convert_time_to_qpc));
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_clock_capability,
        dispatch->convert_time_to_qpc ? 1 : 0,
        application_enabled_clock_conversion ? 1u : 0u,
        dispatch->convert_time_to_qpc ? 1u : 0u,
        bridge_enabled_clock ? 1u : 0u);

    PFN_xrGetInstanceProperties get_instance_properties = nullptr;
    if (load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrGetInstanceProperties",
            get_instance_properties)) {
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(get_instance_properties(created_instance, &properties))) {
            dispatch->runtime_version = properties.runtimeVersion;
            dispatch->runtime_name = properties.runtimeName;
            dispatch->virtual_desktop_runtime = dispatch->runtime_name.find("VirtualDesktopXR") != std::string::npos;
            dispatch->inline_unless_pipelined = dispatch->virtual_desktop_runtime ||
                dispatch->runtime_name.find("Pimax OpenXR") != std::string::npos;
            dispatch->steamvr_runtime =
                std::string_view(dispatch->runtime_name).find("SteamVR") !=
                std::string_view::npos;
        }
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::runtime_identity,
        dispatch->steamvr_runtime ? 1 : 0,
        dispatch->runtime_version,
        dispatch->runtime_name.size(),
        runtime_name_hash(dispatch->runtime_name));

    try {
        std::scoped_lock lock(g_state_mutex);
        g_instances[created_instance] = dispatch;
    } catch (const std::bad_alloc&) {
        dispatch->destroy_instance(created_instance);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        dispatch->destroy_instance(created_instance);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *instance = created_instance;
    return result;
}

XrResult layer_destroy_instance_impl(XrInstance instance) {
    const auto dispatch = find_dispatch(instance);
    if (!dispatch || dispatch->destroy_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    for (const auto& session_state : find_sessions(dispatch)) {
        stop_continuous_presenter(session_state);
        if (session_state->frame_capture) session_state->frame_capture->shutdown();
        session_state->fps_overlay.reset();
    }
    for (const auto& swapchain_state : find_swapchains(dispatch)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_composition_capture(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }

    bool last_instance = false;
    const XrResult result = dispatch->destroy_instance(instance);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        for (auto iterator = g_sessions.begin(); iterator != g_sessions.end();) {
            if (iterator->second->dispatch == dispatch) {
                iterator = g_sessions.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = g_swapchains.begin(); iterator != g_swapchains.end();) {
            if (iterator->second->session->dispatch == dispatch) {
                iterator = g_swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        g_instances.erase(instance);
        last_instance = g_instances.empty();
        std::erase_if(g_observed_spaces, [&](const auto& entry) {
            return entry.second.dispatch && entry.second.dispatch->instance == instance;
        });
    }
    if (last_instance) xrfg::SteamVrDelivery::release_process_connection();
    return result;
}

XrResult layer_create_session_impl(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session) {
    const auto dispatch = find_dispatch(instance);
    if (!dispatch || dispatch->create_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (session == nullptr) {
        return dispatch->create_session(instance, create_info, session);
    }

    auto state = std::make_shared<SessionState>(dispatch);
    const auto initial_control = xrfg::embedded::snapshot();
    state->optical_flow_backend = initial_control.desired.backend == 0
        ? xrfg::D3D12OpticalFlowBackend::fidelity_fx : xrfg::D3D12OpticalFlowBackend::nvidia;
    state->nvidia_new_route = initial_control.desired.backend == 2;
    state->djules_pipeline_switchable=true;
    state->djules_triple_requested=GetPrivateProfileIntW(L"ofxr",L"triple_frame_gen",0,
        (current_layer_directory()/L"ofxr_bridge.ini").c_str())==1;
    state->djules_deep_pipeline = state->djules_triple_requested || GetPrivateProfileIntW(L"ofxr",L"deep_pipeline",1,
        (current_layer_directory()/L"ofxr_bridge.ini").c_str())==1;
    xrfg::embedded::register_djules_pipeline(state->control_id, state->djules_deep_pipeline);
    state->nvidia_options = {
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(initial_control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(initial_control.desired.scale),
        initial_control.desired.backward,
        state->nvidia_new_route, xrfg::flow_encoding_for_route(state->nvidia_new_route), state->nvidia_new_route};
    state->menu_enabled = initial_control.desired.enabled;
    state->frame_generation_mode = static_cast<xrfg::D3D12FrameGenerationMode>(initial_control.desired.frame_generation);
    state->dlss_motion_vectors = initial_control.desired.motion_vectors == 1 ||
        state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3;
    const auto experimental_ini = current_layer_directory() / L"ofxr_bridge.ini";
    state->luke_afw_enabled = GetPrivateProfileIntW(
        L"experimental", L"luke_afw", 0, experimental_ini.c_str()) != 0;
    state->luke_afw_odd_serial_is_left = GetPrivateProfileIntW(
        L"experimental", L"luke_afw_odd_serial_is_left", 1,
        experimental_ini.c_str()) != 0;
    if (state->luke_afw_enabled) state->dlss_motion_vectors = true;
    state->control_revision = initial_control.revision;
    xrfg::embedded::applied(state->control_id, state->control_revision, state->menu_enabled, 0);
    XrStructureType binding_structure_type = XR_TYPE_UNKNOWN;
    if (create_info != nullptr) {
        auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
        while (next != nullptr) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                const auto* binding =
                    reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
                state->graphics_binding = SessionGraphicsBinding::d3d11;
                binding_structure_type = next->type;
                state->d3d11_device = binding->device;
                if (state->d3d11_device) {
                    if ((state->d3d11_device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0) {
                        state->single_threaded_d3d11 = true;
                        state->graphics_binding_capabilities |= 32ULL;
                    }
                    state->d3d11_device->GetImmediateContext(
                        state->d3d11_context.ReleaseAndGetAddressOf());
                    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
                    if (SUCCEEDED(state->d3d11_device.As(&device5))) {
                        state->graphics_binding_capabilities |= 1ULL;
                    }
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
                    if (state->d3d11_context &&
                        SUCCEEDED(state->d3d11_context.As(&context4))) {
                        state->graphics_binding_capabilities |= 2ULL;
                    }
                }
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
                state->graphics_binding = SessionGraphicsBinding::d3d12;
                binding_structure_type = next->type;
                if (FAILED(xrfg::d3d_native::canonical_binding(
                        binding->device, binding->queue,
                        state->d3d12_device, state->d3d12_queue))) {
                    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
                }
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                const auto* binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next);
                state->vulkan_binding = {binding->instance, binding->physicalDevice,
                    binding->device, binding->queueFamilyIndex, binding->queueIndex};
                probe_vulkan_interop_support(*binding);
                state->graphics_binding = SessionGraphicsBinding::vulkan;
                binding_structure_type = next->type;
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
                state->graphics_binding = SessionGraphicsBinding::opengl;
                binding_structure_type = next->type;
                break;
            }
            next = next->next;
        }
    }

    XrSessionCreateInfo substituted_info{};
    const XrSessionCreateInfo* forwarded_info = create_info;
    // The D3D11 bridge: see SessionState::d3d11_bridge. From here the
    // session is a D3D12 one for every path below, and the runtime gets a
    // D3D12 binding on the layer's device. Anything that fails leaves the
    // D3D11 binding as the application made it.
    XrGraphicsBindingD3D12KHR bridge_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    const void* d3d11_binding_next = nullptr;
    if (create_info != nullptr) {
        for (auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
             next != nullptr; next = next->next) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                d3d11_binding_next = next->next;
            }
        }
    }
    Microsoft::WRL::ComPtr<ID3D12Device> luke_producer_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> luke_producer_queue;
    const bool luke_producer = state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        query_luke_ross_d3d12_context(luke_producer_device.GetAddressOf(), luke_producer_queue.GetAddressOf());
    if (!luke_producer && !state->luke_afw_enabled && dispatch->d3d11_bridge &&
        dispatch->get_d3d12_graphics_requirements != nullptr &&
        state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        create_info != nullptr && state->d3d11_device && state->d3d11_context) {
        XrGraphicsRequirementsD3D12KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult requirements_result =
            dispatch->get_d3d12_graphics_requirements(
                instance, create_info->systemId, &requirements);
        Microsoft::WRL::ComPtr<ID3D12Device> bridge_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> bridge_queue;
        HRESULT device_result = E_FAIL;
        if (XR_SUCCEEDED(requirements_result)) {
            device_result = xrfg::create_d3d12_device_for_d3d11(
                state->d3d11_device.Get(),
                bridge_device.ReleaseAndGetAddressOf(),
                bridge_queue.ReleaseAndGetAddressOf());
        }
        if (XR_SUCCEEDED(requirements_result) && SUCCEEDED(device_result) &&
            bridge_device && bridge_queue) {
            auto bridge = std::make_unique<SessionState::D3D11Bridge>();
            bridge->device = std::move(state->d3d11_device);
            bridge->context = std::move(state->d3d11_context);
            state->d3d11_device.Reset();
            state->d3d11_context.Reset();
            state->d3d11_bridge = std::move(bridge);
            state->d3d12_device = std::move(bridge_device);
            state->d3d12_queue = std::move(bridge_queue);
            state->graphics_binding = SessionGraphicsBinding::d3d12;
            state->single_threaded_d3d11 = false;
            state->graphics_binding_capabilities |= 128ULL;
            bridge_binding.device = state->d3d12_device.Get();
            bridge_binding.queue = state->d3d12_queue.Get();
            // Whatever the application chained after its binding stays
            // chained, after ours.
            bridge_binding.next = d3d11_binding_next;
            substituted_info = *create_info;
            substituted_info.next = &bridge_binding;
            forwarded_info = &substituted_info;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                1,
                0,
                0,
                static_cast<std::uint64_t>(requirements.adapterLuid.LowPart));
        } else {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                XR_FAILED(requirements_result)
                    ? static_cast<std::int64_t>(requirements_result)
                    : static_cast<std::int64_t>(device_result),
                0,
                XR_FAILED(requirements_result) ? 1 : 2,
                0);
        }
    }

    // The Vulkan bridge: see SessionState::vulkan_bridge. The same
    // substitution as the D3D11 bridge's, on a D3D12 device of the layer's
    // for the adapter the Vulkan physical device reports. Anything that
    // fails leaves the Vulkan binding as the application made it, on its native Vulkan runtime; no obsolete mirror fallback.
    const void* vulkan_binding_next = nullptr;
    if (create_info != nullptr) {
        for (auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
             next != nullptr; next = next->next) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                vulkan_binding_next = next->next;
            }
        }
    }
    if (dispatch->vulkan_bridge && GetPrivateProfileIntW(L"ofxr", L"vulkan_bridge", 1,
            (current_layer_directory() / L"ofxr_bridge.ini").c_str()) == 1 &&
        xrfg::vulkan_queue_serialized(state->vulkan_binding) &&
        dispatch->get_d3d12_graphics_requirements != nullptr &&
        state->graphics_binding == SessionGraphicsBinding::vulkan &&
        create_info != nullptr &&
        state->vulkan_binding.device != VK_NULL_HANDLE) {
        XrGraphicsRequirementsD3D12KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult requirements_result =
            dispatch->get_d3d12_graphics_requirements(
                instance, create_info->systemId, &requirements);
        Microsoft::WRL::ComPtr<ID3D12Device> bridge_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> bridge_queue;
        HRESULT device_result = E_FAIL;
        if (XR_SUCCEEDED(requirements_result)) {
            device_result = xrfg::create_d3d12_device_for_vulkan(
                state->vulkan_binding,
                bridge_device.ReleaseAndGetAddressOf(),
                bridge_queue.ReleaseAndGetAddressOf());
        }
        if (XR_SUCCEEDED(requirements_result) && SUCCEEDED(device_result) &&
            bridge_device && bridge_queue) {
            auto bridge = std::make_unique<SessionState::VulkanBridge>();
            bridge->binding = state->vulkan_binding;
            state->vulkan_bridge = std::move(bridge);
            state->d3d12_device = std::move(bridge_device);
            state->d3d12_queue = std::move(bridge_queue);
            state->graphics_binding = SessionGraphicsBinding::d3d12;
            state->graphics_binding_capabilities |= 256ULL;
            bridge_binding.device = state->d3d12_device.Get();
            bridge_binding.queue = state->d3d12_queue.Get();
            bridge_binding.next = vulkan_binding_next;
            substituted_info = *create_info;
            substituted_info.next = &bridge_binding;
            forwarded_info = &substituted_info;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                1,
                0,
                0,
                static_cast<std::uint64_t>(requirements.adapterLuid.LowPart));
        } else {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                XR_FAILED(requirements_result)
                    ? static_cast<std::int64_t>(requirements_result)
                    : static_cast<std::int64_t>(device_result),
                0,
                XR_FAILED(requirements_result) ? 1 : 2,
                0);
        }
    }

    XrGraphicsBindingD3D12KHR substituted_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    if (state->nvidia_new_route && dispatch->steamvr_runtime &&
        state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        create_info != nullptr && state->d3d12_device && state->d3d12_queue) {
        const auto* first = state->d3d11_bridge || state->vulkan_bridge
            ? reinterpret_cast<const XrBaseInStructure*>(&bridge_binding)
            : static_cast<const XrBaseInStructure*>(create_info->next);
        if (first != nullptr && first->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            D3D12_COMMAND_QUEUE_DESC binding_queue_description{};
            binding_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            binding_queue_description.NodeMask =
                state->d3d12_queue->GetDesc().NodeMask;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> binding_queue;
            Microsoft::WRL::ComPtr<ID3D12Fence> release_fence;
            Microsoft::WRL::ComPtr<ID3D12Fence> reacquire_fence;
            if (SUCCEEDED(state->d3d12_device->CreateCommandQueue(
                    &binding_queue_description,
                    IID_PPV_ARGS(binding_queue.GetAddressOf()))) &&
                SUCCEEDED(state->d3d12_device->CreateFence(
                    0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(release_fence.GetAddressOf()))) &&
                SUCCEEDED(state->d3d12_device->CreateFence(
                    0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(reacquire_fence.GetAddressOf())))) {
                substituted_binding =
                    *reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(first);
                substituted_binding.queue = binding_queue.Get();
                substituted_info = *create_info;
                substituted_info.next = &substituted_binding;
                forwarded_info = &substituted_info;
                state->binding_queue = std::move(binding_queue);
                state->app_release_fence = std::move(release_fence);
                state->reacquire_fence = std::move(reacquire_fence);
                state->graphics_binding_capabilities |= 16ULL;
            }
        }
    }


    XrSession created_session = XR_NULL_HANDLE;
    const XrResult result = dispatch->create_session(instance, forwarded_info, &created_session);
    if (XR_FAILED(result)) {
        return result;
    }
    state->handle = created_session;
    state->steamvr_delivery = std::make_unique<xrfg::SteamVrDelivery>(dispatch->steamvr_runtime);
    log_video_memory(*state, VideoMemoryStage::session, 0);
    log_view_configuration(*dispatch, instance, create_info ? create_info->systemId : XR_NULL_SYSTEM_ID);

    // Optional instrumentation cannot fail an otherwise valid session.
    try {
        state->fps_overlay = std::make_unique<xrfg::OpenXrFpsOverlay>(
            instance, created_session, create_info ? create_info->systemId : 0,
            dispatch->get_instance_proc_addr, dispatch->end_frame,
            state->d3d12_device.Get(), runtime_queue(*state),
            state->d3d11_device.Get(), current_layer_directory() / L"ofxr_bridge.ini");
        state->fps_overlay->set_delivery_source(state->steamvr_delivery.get());
    } catch (...) {}
    if (state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        (state->graphics_binding_capabilities & 3ULL) == 3ULL) {
        Microsoft::WRL::ComPtr<ID3D12Device> luke_ross_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> luke_ross_queue;
        if (query_luke_ross_d3d12_context(
                luke_ross_device.GetAddressOf(),
                luke_ross_queue.GetAddressOf())) {
            state->d3d12_device = std::move(luke_ross_device);
            state->d3d12_queue = std::move(luke_ross_queue);
            state->graphics_binding_capabilities |= 8ULL;
            if (state->luke_afw_enabled) {
                auto afw = std::make_shared<xrfg::LukeAfwSynthesizer>();
                const HRESULT afw_result = afw->initialize(
                    state->d3d12_device.Get(), state->d3d12_queue.Get(),
                    current_layer_directory() / L"PDAFWPlugin.dll");
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::synthesis_initialize,
                    afw_result, 0xAF023245ULL,
                    state->luke_afw_odd_serial_is_left ? 1ULL : 0ULL, 0);
                if (SUCCEEDED(afw_result)) state->luke_afw = std::move(afw);
            }
        } else {
            const HRESULT bridge_device_result =
                xrfg::create_d3d12_device_for_d3d11(
                    state->d3d11_device.Get(),
                    state->d3d12_device.ReleaseAndGetAddressOf(),
                    state->d3d12_queue.ReleaseAndGetAddressOf());
            if (SUCCEEDED(bridge_device_result)) {
                state->graphics_binding_capabilities |= 4ULL;
            } else {
                state->d3d12_device.Reset();
                state->d3d12_queue.Reset();
            }
        }
    }
    if (state->graphics_binding == SessionGraphicsBinding::d3d11 && state->d3d11_device && state->d3d11_context) {
        try { state->frame_capture = std::make_unique<xrfg::FrameCapture>(state->d3d11_device.Get(),
            state->d3d11_context.Get(), current_layer_directory() / L"captures"); } catch (...) {}
    }
    // A queue of the layer's own for synthesis, so the GPU-side wait on the
    // optical flow fence does not sit in the middle of the application's
    // queue. Only for a native D3D12 application: a D3D11 one already runs
    // synthesis on the bridge queue created just above.
    if (state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        state->d3d12_device && state->d3d12_queue) {
        D3D12_COMMAND_QUEUE_DESC synthesis_queue_description{};
        synthesis_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // Synthesis has a hard deadline the application's rendering does
        // not: it must finish inside one display period or its frame is
        // reprojected away.
        synthesis_queue_description.Priority =
            D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        synthesis_queue_description.NodeMask =
            state->d3d12_queue->GetDesc().NodeMask;
        if (FAILED(state->d3d12_device->CreateCommandQueue(
                &synthesis_queue_description,
                IID_PPV_ARGS(
                    state->d3d12_synthesis_queue.ReleaseAndGetAddressOf())))) {
            // Fail open: synthesis stays on the application's queue, which
            // is what every build before this one did.
            state->d3d12_synthesis_queue.Reset();
        } else {
            state->graphics_binding_capabilities |= 8ULL;
        }
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::session_binding,
        static_cast<std::int64_t>(state->graphics_binding),
        static_cast<std::uint64_t>(binding_structure_type) |
            (state->graphics_binding_capabilities << 32),
        state->d3d11_device
            ? handle_value(state->d3d11_device.Get())
            : handle_value(state->d3d12_device.Get()),
        state->d3d11_context
            ? handle_value(state->d3d11_context.Get())
            : handle_value(state->d3d12_queue.Get()));

    try {
        {
            std::scoped_lock lock(g_state_mutex);
            g_sessions[created_session] = state;
        }
    } catch (const std::bad_alloc&) {
        dispatch->destroy_session(created_session);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        dispatch->destroy_session(created_session);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *session = created_session;
    return result;
}

XrResult layer_destroy_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->destroy_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    stop_continuous_presenter(state);
    drain_binding_queue(state.get());
    state->fps_overlay.reset();
    if (state->frame_capture) state->frame_capture->shutdown();
    for (const auto& swapchain_state : find_swapchains(state)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_composition_capture(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }

    const XrResult result = state->dispatch->destroy_session(session);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        for (auto iterator = g_swapchains.begin(); iterator != g_swapchains.end();) {
            if (iterator->second->session == state) {
                iterator = g_swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        g_sessions.erase(session);
        std::erase_if(g_observed_spaces, [&](const auto& entry) {
            const auto owner = entry.second.session.lock();
            return !owner || owner->handle == session;
        });
    }
    return result;
}

void reset_frame_bookkeeping(const std::shared_ptr<SessionState>& state) {
    if (state->fps_overlay) state->fps_overlay->reset_metrics();
    {
        std::scoped_lock frame_call_lock(state->frame_call_mutex);
        state->application_wait_pending_begin = false;
        state->application_frame_in_progress = false;
        state->application_frame_has_overlapping_wait = false;
        state->pipelined_wait_streak = 0;
        state->pipelined_presenter_mode = false;
        state->pipelined_presenter_start_requested = false;
        state->presenter_start_requested = false;
        state->split_frame_loop = false;
        state->application_wait_thread_id = state->application_end_thread_id = 0;
        state->runtime_frame_waited_unbegun = state->application_begin_needs_wait = false;
        state->last_inline_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        state->last_inline_frame_state_valid = false;
        state->generation_steady_state_established = false;
        state->luke_clock_active = false;
        state->luke_duplicate_cycle = false;
        state->application_wait_returned_at = {};
    }
    state->frame_call_condition.notify_all();
    {
        std::scoped_lock presenter_lock(state->presenter_mutex);
        state->last_virtual_display_time = 0;
    }
    {
        std::scoped_lock lock(state->mutex);
        state->pending_frames.clear();
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        state->generation_resume_wall_time = {};
        state->minimum_runtime_display_period = 0;
        state->steamvr_throttled_wait_streak = 0;
        state->unpaced_wait_streak = 0;
    }
}

void reset_swapchain_bookkeeping(const std::shared_ptr<SessionState>& session) noexcept {
    try {
        for (const auto& state : find_swapchains(session)) {
            std::scoped_lock call_lock(state->call_mutex);
            std::scoped_lock gpu_lock(session->gpu_mutex);
            std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(state->mutex);
                // Neither session call releases the application's images.
                // Keep the runtime's outstanding acquire/wait mirror; reset
                // only history and generation state, not image ownership.
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                history = state->d3d12_history;
                generation = state->frame_generation;
            }
            if (generation && generation->synthesizer) {
                static_cast<void>(generation->synthesizer->wait_for_idle());
            }
            if (generation && generation->afw_temporal_synthesizer) {
                static_cast<void>(
                    generation->afw_temporal_synthesizer->wait_for_idle());
            }
            if (generation && generation->afw_final_history) {
                static_cast<void>(generation->afw_final_history->invalidate());
            }
            if (history) {
                static_cast<void>(history->invalidate());
            }
            if (generation && generation->d3d11_interop) {
                static_cast<void>(generation->d3d11_interop->wait_for_idle());
            }
        }
    } catch (...) {
    }
}

XrResult layer_begin_session_impl(
    XrSession session,
    const XrSessionBeginInfo* begin_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->begin_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    const XrResult result = state->dispatch->begin_session(session, begin_info);
    if (XR_SUCCEEDED(result)) {
        reset_frame_bookkeeping(state);
        reset_swapchain_bookkeeping(state);
    }
    return result;
}

XrResult layer_end_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->end_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    stop_continuous_presenter(state);
    const XrResult result = state->dispatch->end_session(session);
    if (XR_SUCCEEDED(result)) {
        reset_frame_bookkeeping(state);
        reset_swapchain_bookkeeping(state);
    }
    return result;
}

// frameWaitInfo and frameBeginInfo are optional="true" in the OpenXR registry,
// so a null pointer is valid input that a runtime must accept, and only a
// non-null one carries a type worth checking. This matters wherever the layer
// answers a frame call itself instead of forwarding it: the pass-through path
// hands the pointer to the runtime, which accepts null, while the virtual wait
// and begin used in presenter and pipelined modes have to accept it too.
// Rejecting null there fails applications that pass it and only on the runtimes
// that promote a presenter -- Luke Ross's mods call xrWaitFrame(session, NULL,
// &state) and drop back to 2D on the XR_ERROR_VALIDATION_FAILURE.
template <typename Info>
[[nodiscard]] bool valid_optional_frame_info(
    const Info* info,
    XrStructureType expected) noexcept {
    return info == nullptr || info->type == expected;
}

void update_luke_application_clock(SessionState& state, bool recognized, bool duplicate) {
    // Called with frame_call_mutex. In particular, do not sample a newer Luke
    // counter here: admission still uses the identity latched at Release.
    if (!recognized) {
        state.luke_clock_active = false;
        state.luke_duplicate_cycle = false;
        return;
    }
    if (!state.luke_clock_active) {
        if (state.last_virtual_display_time <= 0 ||
            state.application_wait_returned_at == std::chrono::steady_clock::time_point{}) return;
        state.luke_clock_origin = state.last_virtual_display_time;
        state.luke_clock_wall_origin = state.application_wait_returned_at;
        state.luke_clock_active = true;
    }
    state.luke_duplicate_cycle = duplicate;
}

XrResult layer_wait_frame_impl(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state) {
    const auto state = find_session(session);
    if (!state || state->dispatch->wait_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::unique_lock adaptive_wait_lock(state->adaptive_wait_mutex);
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    state->frame_call_condition.wait(frame_call_lock, [&] {
        return !state->application_wait_pending_begin;
    });
    observe_application_frame_thread(*state, false);
    XrResult result = XR_SUCCESS;
    const bool use_continuous_presenter = continuous_presenter_active(state);
    const bool adaptive_nvidia_presenter = use_continuous_presenter &&
        three_slot_generation_session(state);
    if (!adaptive_nvidia_presenter || !state->menu_enabled || state->manual_control.stop_requested() ||
        state->last_virtual_display_time == 0)
        update_luke_application_clock(*state, false, false);
    bool use_provisional_pipelined_wait = false;
    bool forwarded_application_wait = false;
    if (!use_continuous_presenter && state->application_frame_in_progress) {
        if (!state->application_frame_has_overlapping_wait) {
            state->application_frame_has_overlapping_wait = true;
            ++state->pipelined_wait_streak;
        }
        if (!presenter_forbidden(*state) && !state->luke_afw_enabled && state->pipelined_wait_streak >= 2 &&
            state->last_inline_frame_state_valid) {
            // The current runtime frame is still owned by the render thread,
            // so the transition cannot start its presenter until xrEndFrame
            // closes that boundary. Return the first virtual wait now and
            // preserve its timeline when the presenter starts at that end.
            state->pipelined_presenter_mode = true;
            state->pipelined_presenter_start_requested = true;
            use_provisional_pipelined_wait = true;
        }
    }
    if (use_continuous_presenter) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::unique_lock presenter_lock(state->presenter_mutex);
        // This waits only for the initial valid prediction. D3D11's director
        // seeds that prediction at promotion. Non-Luke virtual waits neither
        // sleep nor wait for a new director tick; filtered Luke repeats use
        // the independent bounded clock below. Two display periods describe
        // the intended real/synthetic timeline, not a producer rate limiter
        // or the display period advertised to the three-slot application's AER.
        state->presenter_condition.wait(presenter_lock, [&] {
            return state->presenter_frame_state_valid ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        if (!state->presenter_frame_state_valid ||
            state->presenter_stop_requested) {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        if (state->luke_clock_active && state->three_slot_director) {
            const auto period = std::clamp<XrDuration>(
                state->presenter_frame_state.predictedDisplayPeriod, 2'000'000, 50'000'000);
            if (state->luke_duplicate_cycle) {
                const auto token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::luke_cycle_wait, period);
                // Absolute deadline from the previous Wait, not an extra
                // period after End. No director tick, worker fence, or render
                // counter must advance to release this wait. The condition
                // wait releases presenter_mutex while asleep.
                state->presenter_condition.wait_until(presenter_lock,
                    state->application_wait_returned_at + std::chrono::nanoseconds(period), [&] {
                        return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) ||
                            !state->menu_enabled || state->manual_control.stop_requested();
                    });
                const auto wait_result = XR_FAILED(state->presenter_failure) ? state->presenter_failure
                    : state->presenter_stop_requested ? XR_ERROR_SESSION_NOT_RUNNING : XR_SUCCESS;
                xrfg::bridge_flight_logger().end(token,
                    xrfg::BridgeFlightOperation::luke_cycle_wait, wait_result);
                if (XR_FAILED(wait_result)) return wait_result;
                state->luke_duplicate_cycle = false;
            }
            // Keep the established XR epoch and prediction lead, but advance
            // it with real elapsed time even when the director has no output.
            // Unlike the non-Luke ceiling below, an idle director cannot turn
            // repeated app cycles into a nanosecond clock or stop progress.
            const auto now = std::chrono::steady_clock::now();
            const XrTime prediction = state->luke_clock_origin +
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - state->luke_clock_wall_origin).count();
            frame_state->predictedDisplayTime = std::max(prediction, state->last_virtual_display_time + 1);
            state->last_virtual_display_time = frame_state->predictedDisplayTime;
            frame_state->predictedDisplayPeriod = state->presenter_frame_state.predictedDisplayPeriod;
            frame_state->shouldRender = state->presenter_frame_state.shouldRender;
        } else {
        const XrDuration virtual_period =
                    state->manual_control.stop_requested() || !state->menu_enabled
                ? state->presenter_frame_state.predictedDisplayPeriod
                : djules_application_period(*state,
                      state->presenter_frame_state.predictedDisplayPeriod);
        // The application's timeline is anchored to the runtime's own
        // prediction, one virtual period ahead of the frame the presenter is
        // about to submit.
        const XrTime anchor = add_display_duration(
            state->presenter_frame_state.predictedDisplayTime,
            virtual_period);
        // A second wait inside one presenter frame has to come back later than
        // the first, so the guard below steps off the last time served. That
        // step invents time the runtime never advanced, and the ceiling is
        // what stops it becoming a clock of its own: every period handed out
        // beyond the anchor is a period the application's prediction runs
        // ahead of the runtime's, and nothing ever gives it back.
        //
        // It is not a corner case. Whenever the layer fails open - no
        // projection layers in the submission, which is what a menu or a
        // loading screen looks like - the application is paced one frame per
        // presenter frame instead of one per pair, while still being handed a
        // doubled period on every one of them. Unbounded, that drifts a full
        // second per second: a captured session reached 637 s of lead,
        // predicting poses ten minutes into the future, and never generated
        // again once it got there, because the ratchet only turns one way.
        // With the ceiling the clock simply ticks at the rate the application
        // is actually being paced at, and re-anchors as soon as the pairing
        // comes back.
        const XrTime ceiling = add_display_duration(anchor, virtual_period);
        XrTime virtual_time = anchor;
        if (state->last_virtual_display_time != 0 &&
            virtual_time <= state->last_virtual_display_time) {
            virtual_time = add_display_duration(
                state->last_virtual_display_time,
                virtual_period);
        }
        if (virtual_time > ceiling) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::virtual_clock_clamp,
                0,
                static_cast<std::uint64_t>(virtual_time - ceiling),
                static_cast<std::uint64_t>(ceiling),
                static_cast<std::uint64_t>(
                    state->presenter_frame_state.predictedDisplayTime));
            virtual_time = ceiling;
        }
        // The ceiling can land at or below the time already served, because
        // the anchor tracks a runtime prediction that does not advance by a
        // whole period every time it is read. Never hand the application a
        // display time that does not move: it is not something a caller has
        // to tolerate, and the runtime safeguards added in v0.2.1 read a
        // non-advancing time as a projection resource layout change and stop
        // generation for a second, which the fail-open pacing that follows
        // then reproduces on the next frame.
        //
        // A nanosecond is enough. The drift the ceiling exists to prevent
        // accumulates in whole periods - it reached 637 s of lead in a
        // captured session - so a floor measured in nanoseconds keeps the
        // sequence strictly increasing without giving the ratchet anything
        // to turn on.
        if (state->last_virtual_display_time != 0 &&
            virtual_time <= state->last_virtual_display_time) {
            virtual_time = state->last_virtual_display_time + 1;
        }
        state->last_virtual_display_time = virtual_time;
        frame_state->predictedDisplayTime = virtual_time;
        // The three-slot director owns the two-output cadence. Report the
        // runtime display period to the app, as the inline route does, rather
        // than feeding that doubled cadence into upstream half-rate policies.
        // Keep the prediction anchor/ceiling and all buffer pacing unchanged.
        frame_state->predictedDisplayPeriod = djules_triple_active(*state)?virtual_period:
            (state->three_slot_director || state->fsr_two_slot_presenter)
            ? state->presenter_frame_state.predictedDisplayPeriod : virtual_period;
        frame_state->shouldRender = state->presenter_frame_state.shouldRender;
        }
    } else if (use_provisional_pipelined_wait) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        const XrDuration virtual_period = (state->manual_control.stop_requested() || !state->menu_enabled)
            ? state->last_inline_frame_state.predictedDisplayPeriod
            : djules_application_period(*state,state->last_inline_frame_state.predictedDisplayPeriod);
        const XrTime anchor = add_display_duration(
            state->last_inline_frame_state.predictedDisplayTime,
            virtual_period);
        // Same ceiling as the presenter path above, for the same reason: this
        // branch repeats for as long as the promotion takes, and each repeat
        // would otherwise push the application's timeline a period further
        // from the one the runtime is predicting on.
        const XrTime ceiling = add_display_duration(anchor, virtual_period);
        XrTime virtual_time = anchor;
        {
            std::scoped_lock presenter_lock(state->presenter_mutex);
            if (state->last_virtual_display_time != 0 &&
                virtual_time <= state->last_virtual_display_time) {
                virtual_time = add_display_duration(
                    state->last_virtual_display_time,
                    virtual_period);
            }
            if (virtual_time > ceiling) {
                virtual_time = ceiling;
            }
            // Same floor as the presenter path, for the same reason: a
            // display time that does not advance is not something a caller
            // has to tolerate.
            if (state->last_virtual_display_time != 0 &&
                virtual_time <= state->last_virtual_display_time) {
                virtual_time = state->last_virtual_display_time + 1;
            }
            state->last_virtual_display_time = virtual_time;
        }
        frame_state->predictedDisplayTime = virtual_time;
        frame_state->predictedDisplayPeriod = virtual_period;
        frame_state->shouldRender =
            state->last_inline_frame_state.shouldRender;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            100,
            state->pipelined_wait_streak,
            static_cast<std::uint64_t>(virtual_time),
            static_cast<std::uint64_t>(virtual_period));
    } else {
        const auto application_wait_started = std::chrono::steady_clock::now();
        result = state->dispatch->wait_frame(session, wait_info, frame_state);
        const auto application_wait_elapsed =
            std::chrono::steady_clock::now() - application_wait_started;
        if (XR_SUCCEEDED(result) && frame_state != nullptr) {
            state->last_inline_frame_state = *frame_state;
            state->last_inline_frame_state.next = nullptr;
            state->last_inline_frame_state_valid = true;
            state->last_application_wait_elapsed = application_wait_elapsed;
            forwarded_application_wait = true;
            state->runtime_frame_waited_unbegun = true;
        }
    }
    if (XR_SUCCEEDED(result) && frame_state != nullptr) {
        state->application_wait_returned_at = std::chrono::steady_clock::now();
        // Do not even enter a synthesizer mutex from the virtual D3D11 wait:
        // the worker can hold it while waiting for GPU completion. Producer
        // ownership is resolved only at the release/EndFrame handoffs.
        if (!adaptive_nvidia_presenter) {
            if (!state->d3d12_synthesis_queue)
                static_cast<void>(wait_for_previous_session_synthesis(state));
            else
                static_cast<void>(flush_session_pending_copies(state));
        }
        state->application_wait_pending_begin = true;
        std::scoped_lock lock(state->mutex);
        // Every runtime, not only SteamVR: the synthetic interpolation fraction
        // needs a display period wherever generation runs, and the promotion
        // that also reads this guards on the runtime itself. A runtime that
        // never reports one keeps the fixed midpoint.
        if (!use_continuous_presenter &&
            frame_state->predictedDisplayPeriod > 0 &&
            (state->minimum_runtime_display_period == 0 ||
             frame_state->predictedDisplayPeriod <
                 state->minimum_runtime_display_period)) {
            state->minimum_runtime_display_period =
                frame_state->predictedDisplayPeriod;
        }
        // A runtime that is pacing this application blocks the wait for most
        // of a display period. SteamVR returns in about 1.55 ms against
        // 11.11 ms, which is not pacing anything. Count the waits that came
        // back too quickly, on the same half-period line the throttle detector
        // uses in the other direction.
        if (forwarded_application_wait &&
            state->minimum_runtime_display_period > 0) {
            const auto elapsed_nanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    state->last_application_wait_elapsed).count();
            state->unpaced_wait_streak =
                elapsed_nanoseconds * 2 < state->minimum_runtime_display_period
                    ? state->unpaced_wait_streak + 1
                    : 0;
        }
        if(forwarded_application_wait && djules_triple_active(*state) && !state->manual_control.stop_requested())
            frame_state->predictedDisplayPeriod=djules_application_period(*state,frame_state->predictedDisplayPeriod);
        const std::uint64_t frame_serial = state->next_application_frame_serial++;
        state->pending_frames.push_back({
            frame_state->predictedDisplayTime,
            frame_state->predictedDisplayPeriod,
            frame_serial,
        });
        constexpr std::size_t kMaximumPendingFrameRecords = 32;
        while (state->pending_frames.size() > kMaximumPendingFrameRecords) {
            state->pending_frames.pop_front();
        }
        if (state->luke_afw_enabled) {
            // result 1: application wait ticket. a=serial, b=time, c=period.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_frame_contract,
                1, frame_serial,
                static_cast<std::uint64_t>(frame_state->predictedDisplayTime),
                static_cast<std::uint64_t>(frame_state->predictedDisplayPeriod));
            xrfg::two_frame_capture_timeline(
                "frame", 1, frame_serial,
                static_cast<std::uint64_t>(frame_state->predictedDisplayTime),
                static_cast<std::uint64_t>(frame_state->predictedDisplayPeriod));
        }
    }
    return result;
}

XrResult layer_begin_frame_impl(
    XrSession session,
    const XrFrameBeginInfo* begin_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->begin_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        if (continuous_presenter_active(state)) {
            result = valid_optional_frame_info(
                    begin_info, XR_TYPE_FRAME_BEGIN_INFO)
                ? XR_SUCCESS
                : XR_ERROR_VALIDATION_FAILURE;
        } else if (state->pipelined_presenter_mode) {
            result = XR_ERROR_RUNTIME_FAILURE;
        } else {
            XrResult replacement_result = XR_SUCCESS;
            if (state->application_begin_needs_wait) {
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState replacement{XR_TYPE_FRAME_STATE};
                replacement_result = state->dispatch->wait_frame(session, &wait, &replacement);
                if (XR_SUCCEEDED(replacement_result)) {
                    state->application_begin_needs_wait = false;
                    state->last_inline_frame_state = replacement;
                    state->last_inline_frame_state.next = nullptr;
                    state->last_inline_frame_state_valid = true;
                    state->runtime_frame_waited_unbegun = true;
                }
            }
            result = XR_FAILED(replacement_result) ? replacement_result : with_runtime_entry(state, [&] {
                return state->dispatch->begin_frame(session, begin_info);
            });
            if (XR_SUCCEEDED(result)) state->runtime_frame_waited_unbegun = false;
        }
    } catch (...) {
        if (state->application_wait_pending_begin) {
            state->application_wait_pending_begin = false;
            frame_call_lock.unlock();
            state->frame_call_condition.notify_all();
        }
        throw;
    }
    if (XR_SUCCEEDED(result)) {
        state->application_frame_in_progress = true;
        state->application_frame_has_overlapping_wait = false;
    }
    if (state->application_wait_pending_begin) {
        state->application_wait_pending_begin = false;
        frame_call_lock.unlock();
        state->frame_call_condition.notify_all();
    }
    return result;
}

XrResult layer_create_swapchain_impl(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    const auto state = find_session(session);
    if (!state || state->dispatch->create_swapchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (create_info == nullptr || swapchain == nullptr) {
        return state->dispatch->create_swapchain(session, create_info, swapchain);
    }

    PresenterResourceLifetimeGuard presenter_guard(state);
    const bool active_color_reconfiguration =
        state->generation_steady_state_established &&
        (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
        (create_info->usageFlags &
         XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
    // Bridged and multisampled: the runtime gets a single-sample swapchain
    // and the application a multisampled texture of its own, resolved at
    // release (D3D11BridgePath::resolve). Every path below sees the
    // single-sample create info, which is what the runtime's images are.
    XrSwapchainCreateInfo bridged_create_info{};
    const XrSwapchainCreateInfo* runtime_create_info = create_info;
    if ((state->d3d11_bridge || state->vulkan_bridge) &&
        (create_info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))
        return XR_ERROR_FEATURE_UNSUPPORTED;
    if (state->d3d11_bridge && create_info->sampleCount > 1) {
        bridged_create_info = *create_info;
        bridged_create_info.sampleCount = 1;
        runtime_create_info = &bridged_create_info;
    }
    // Bridged Vulkan: the application's format is a VkFormat and the
    // runtime's session is D3D12, so the create info the runtime sees
    // carries the DXGI equivalent, and so does everything below, which is
    // what the runtime's images are. A format with no equivalent is refused
    // the way a runtime refuses one it does not list; so is multisampled
    // colour, which the bridge does not resolve.
    if (state->vulkan_bridge) {
        const auto vulkan_format = static_cast<VkFormat>(create_info->format);
        const bool depth =
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const DXGI_FORMAT dxgi_format = depth
            ? xrfg::dxgi_depth_format_for_vulkan(vulkan_format)
            : xrfg::dxgi_format_for_vulkan(vulkan_format);
        if (dxgi_format == DXGI_FORMAT_UNKNOWN ||
            (!depth && create_info->sampleCount > 1)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                4,
                static_cast<std::uint64_t>(create_info->format),
                create_info->usageFlags,
                create_info->sampleCount);
            return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }
        bridged_create_info = *create_info;
        bridged_create_info.format = static_cast<std::int64_t>(dxgi_format);
        // A Vulkan application need not render into the swapchain image:
        // OpenComposite copies the game's own texture in, so No Man's Sky's
        // swapchains carry only TRANSFER_DST. The runtime's image and the
        // shared texture are colour attachments whatever was asked, which
        // is what makes the swapchain a colour one to every path below.
        if (!depth) {
            bridged_create_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        }
        // The runtime's depth images are never written (depth_private), so
        // they are the cheapest shape that creates.
        if (depth) {
            bridged_create_info.sampleCount = 1;
        }
        runtime_create_info = &bridged_create_info;
    }
    auto swapchain_state = std::make_shared<SwapchainState>(state, *runtime_create_info);
    XrSwapchain created_swapchain = XR_NULL_HANDLE;
    const XrResult result = state->dispatch->create_swapchain(
        session,
        runtime_create_info,
        &created_swapchain);
    if (XR_FAILED(result)) {
        // The application losing a swapchain of its own is the shape a layer
        // that overspends the runtime's budget takes from the outside, and
        // without this record the log ends at the last creation that worked.
        // A negative result distinguishes it from the create-info records
        // below, which carry a field index there.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::swapchain_create,
            result,
            0,
            (static_cast<std::uint64_t>(create_info->width) << 32) |
                create_info->height,
            static_cast<std::uint64_t>(create_info->usageFlags));
        return result;
    }
    swapchain_state->handle = created_swapchain;
    log_video_memory(*state, VideoMemoryStage::application_swapchain, handle_value(created_swapchain));

    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        0,
        handle_value(created_swapchain),
        static_cast<std::uint64_t>(create_info->createFlags),
        static_cast<std::uint64_t>(create_info->usageFlags));
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        1,
        handle_value(created_swapchain),
        (static_cast<std::uint64_t>(create_info->width) << 32) |
            create_info->height,
        (static_cast<std::uint64_t>(create_info->arraySize) << 32) |
            create_info->sampleCount);
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        2,
        handle_value(created_swapchain),
        static_cast<std::uint64_t>(create_info->format),
        (static_cast<std::uint64_t>(create_info->faceCount) << 32) |
            create_info->mipCount);

    try {
        {
            std::scoped_lock lock(g_state_mutex);
            g_swapchains[created_swapchain] = swapchain_state;
        }
    } catch (const std::bad_alloc&) {
        state->dispatch->destroy_swapchain(created_swapchain);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        state->dispatch->destroy_swapchain(created_swapchain);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (state->d3d11_bridge) {
        // Bridge it now, while the runtime's images can be enumerated
        // without the application seeing D3D12 ones: the application's
        // enumeration is answered from the bridge. A shape the bridge cannot
        // share fails the creation, which the application sees as a
        // swapchain it could not create.
        std::uint32_t runtime_count = 0;
        std::vector<XrSwapchainImageD3D12KHR> runtime_images;
        XrResult bridge_result = state->dispatch->enumerate_swapchain_images(
            created_swapchain, 0, &runtime_count, nullptr);
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            runtime_images.assign(
                runtime_count,
                XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            bridge_result = state->dispatch->enumerate_swapchain_images(
                created_swapchain, runtime_count, &runtime_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(runtime_images.data()));
        }
        HRESULT shared_result = E_FAIL;
        std::uint32_t failure_stage = 0;
        auto bridge = std::make_shared<xrfg::D3D11BridgeSwapchain>();
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            std::vector<ID3D12Resource*> resources;
            for (const XrSwapchainImageD3D12KHR& image : runtime_images) {
                resources.push_back(image.texture);
            }
            xrfg::D3D11BridgeSwapchainDescription requested{};
            requested.requested_format =
                static_cast<DXGI_FORMAT>(create_info->format);
            requested.requested_sample_count = create_info->sampleCount;
            requested.unordered_access =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0;
            requested.depth_stencil =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            shared_result = bridge->initialize(
                state->d3d11_bridge->device.Get(),
                state->d3d11_bridge->context.Get(),
                state->d3d12_device.Get(),
                state->d3d12_queue.Get(),
                resources,
                requested,
                &failure_stage);
        }
        if (XR_FAILED(bridge_result) || FAILED(shared_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                XR_FAILED(bridge_result)
                    ? static_cast<std::int64_t>(bridge_result)
                    : static_cast<std::int64_t>(shared_result),
                handle_value(created_swapchain),
                failure_stage,
                static_cast<std::uint64_t>(create_info->format));
            {
                std::scoped_lock lock(g_state_mutex);
                g_swapchains.erase(created_swapchain);
            }
            state->dispatch->destroy_swapchain(created_swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        // 2: bridged. b the image count and, in the high half, the path
        // (D3D11BridgePath); c the requested format and, in the high half,
        // the format the shared textures took from the runtime's images.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::d3d11_bridge,
            2,
            handle_value(created_swapchain),
            (static_cast<std::uint64_t>(bridge->path()) << 32) | runtime_count,
            (static_cast<std::uint64_t>(bridge->shared_format()) << 32) |
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(create_info->format)));
        if (bridge->path() == xrfg::D3D11BridgePath::depth_private) {
            state->has_private_depth_swapchain.store(true, std::memory_order_release);
            swapchain_state->private_depth = true;
        }
        std::scoped_lock lock(swapchain_state->mutex);
        swapchain_state->bridge = std::move(bridge);
    } else if (state->vulkan_bridge) {
        // Bridge it now, as for D3D11: the application's enumeration is
        // answered from the bridge's imported images.
        std::uint32_t runtime_count = 0;
        std::vector<XrSwapchainImageD3D12KHR> runtime_images;
        XrResult bridge_result = state->dispatch->enumerate_swapchain_images(
            created_swapchain, 0, &runtime_count, nullptr);
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            runtime_images.assign(
                runtime_count,
                XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            bridge_result = state->dispatch->enumerate_swapchain_images(
                created_swapchain, runtime_count, &runtime_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(runtime_images.data()));
        }
        HRESULT shared_result = E_FAIL;
        std::uint32_t failure_stage = 0;
        auto bridge = std::make_shared<xrfg::VulkanBridgeSwapchain>();
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            std::vector<ID3D12Resource*> resources;
            for (const XrSwapchainImageD3D12KHR& image : runtime_images) {
                resources.push_back(image.texture);
            }
            xrfg::VulkanBridgeSwapchainDescription requested{};
            requested.requested_format = static_cast<VkFormat>(create_info->format);
            requested.width = create_info->width;
            requested.height = create_info->height;
            requested.array_size = create_info->arraySize;
            requested.mip_levels = create_info->mipCount;
            requested.sample_count = create_info->sampleCount;
            requested.unordered_access =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0;
            requested.depth_stencil =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            shared_result = bridge->initialize(
                state->vulkan_bridge->binding,
                state->d3d12_device.Get(),
                state->d3d12_queue.Get(),
                resources,
                requested,
                &failure_stage);
        }
        if (XR_FAILED(bridge_result) || FAILED(shared_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                XR_FAILED(bridge_result)
                    ? static_cast<std::int64_t>(bridge_result)
                    : static_cast<std::int64_t>(shared_result),
                handle_value(created_swapchain),
                failure_stage,
                static_cast<std::uint64_t>(create_info->format));
            {
                std::scoped_lock lock(g_state_mutex);
                g_swapchains.erase(created_swapchain);
            }
            state->dispatch->destroy_swapchain(created_swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::vulkan_bridge,
            2,
            handle_value(created_swapchain),
            (static_cast<std::uint64_t>(bridge->path()) << 32) | runtime_count,
            (static_cast<std::uint64_t>(bridge->shared_format()) << 32) |
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(create_info->format)));
        if (bridge->path() == xrfg::VulkanBridgePath::depth_private) {
            state->has_private_depth_swapchain.store(true, std::memory_order_release);
            swapchain_state->private_depth = true;
        }
        std::scoped_lock lock(swapchain_state->mutex);
        swapchain_state->vulkan_bridge = std::move(bridge);
    }
    *swapchain = created_swapchain;
    if (active_color_reconfiguration) {
        schedule_generation_quarantine(
            state,
            GenerationQuarantineReason::swapchain_created,
            handle_value(created_swapchain));
    }
    return result;
}

XrResult layer_destroy_swapchain_impl(XrSwapchain swapchain) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->destroy_swapchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    PresenterResourceLifetimeGuard presenter_guard(state->session);
    const bool active_color_reconfiguration =
        state->session->generation_steady_state_established &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
    if (active_color_reconfiguration) {
        // Drop the pair and the retained repeat - the repeat's composition
        // names this swapchain - but do not hold generation off for the
        // structural quarantine's full second.
        //
        // The wall-clock deadline was covering a race that lazy arming has
        // since removed. The lines below drain this swapchain's GPU work and
        // destroy its private swapchains synchronously, under the call lock
        // and with the presenter held by the guard above, so nothing queued
        // names anything being torn down by the time this returns. What the
        // deadline additionally prevented was the layer arming the
        // replacement swapchains the moment they were enumerated, mid
        // reconfiguration; generation resources are now taken when a
        // projection layer first names a swapchain, so the application
        // decides when the replacement is ready and the layer cannot run
        // ahead of it.
        //
        // It is not free to keep. MSFS 2024 recreates its swapchains on world
        // and settings transitions - four times in 96 s in one capture - and
        // each one cost almost exactly a second of generation: the
        // replacements were enumerated within 40 ms and armed 1.07 s later,
        // the delay being the deadline and nothing else. Every other
        // quarantine reason is unchanged.
        clear_generation_continuity(state->session);
        {
            std::scoped_lock presenter_lock(state->session->presenter_mutex);
            state->session->presenter_last_frame.reset();
        }
        state->session->generation_steady_state_established = false;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::continuity_reset,
            static_cast<std::int64_t>(
                GenerationQuarantineReason::swapchain_destroyed),
            handle_value(state->session->handle),
            handle_value(swapchain),
            0);
    }
    std::scoped_lock call_lock(state->call_mutex);
    drain_swapchain_gpu(state);
    destroy_composition_capture(state);
    destroy_frame_generation_swapchains(state);
    const XrResult result = state->session->dispatch->destroy_swapchain(swapchain);
    if (XR_SUCCEEDED(result)) {
        { std::scoped_lock lock(state->mutex); state->bridge.reset(); state->vulkan_bridge.reset(); }
        { std::scoped_lock lock(state->session->logged_view_rects_mutex); state->session->logged_view_rects.erase(swapchain); }
        std::scoped_lock lock(g_state_mutex);
        g_swapchains.erase(swapchain);
    }
    log_video_memory(*state->session, VideoMemoryStage::swapchain_destroyed, handle_value(swapchain));
    return result;
}

XrResult layer_enumerate_swapchain_images_impl(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->enumerate_swapchain_images == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    // Resource/context creation must observe one applied menu configuration,
    // and no presenter-owned composition may retain the resources while an
    // application replaces them during a resize/reconfigure transaction.
    PresenterResourceLifetimeGuard presenter_guard(state->session);
    std::scoped_lock call_lock(state->call_mutex);
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    {
        std::scoped_lock lock(state->mutex);
        bridge = state->bridge;
        vulkan_bridge = state->vulkan_bridge;
    }
    std::vector<ID3D12Resource*> bridged_resources;
    XrResult result = XR_SUCCESS;
    if (vulkan_bridge) {
        // The application gets the bridge's imported Vulkan images, never
        // the runtime's D3D12 ones; the D3D12 bookkeeping below sees the
        // shared textures, which is what history captures from.
        const auto vulkan_views = vulkan_bridge->vulkan_images();
        const auto count = static_cast<std::uint32_t>(vulkan_views.size());
        if (image_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *image_count_output = count;
        if (image_capacity_input == 0 || images == nullptr) {
            return XR_SUCCESS;
        }
        if (image_capacity_input < count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        auto* vulkan_images = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (vulkan_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            vulkan_images[index].image = vulkan_views[index];
        }
        const auto shared = vulkan_bridge->shared_images();
        bridged_resources.assign(shared.begin(), shared.end());
    } else if (bridge) {
        // The application gets the bridge's D3D11 textures, never the
        // runtime's D3D12 images; the D3D12 bookkeeping below sees the
        // shared textures, which is what history captures from.
        const auto d3d11_views = bridge->d3d11_images();
        const auto count = static_cast<std::uint32_t>(d3d11_views.size());
        if (image_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *image_count_output = count;
        if (image_capacity_input == 0 || images == nullptr) {
            return XR_SUCCESS;
        }
        if (image_capacity_input < count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        auto* d3d11_images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (d3d11_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d11_images[index].texture = d3d11_views[index];
        }
        const auto shared = bridge->shared_images();
        bridged_resources.assign(shared.begin(), shared.end());
    } else {
        result = state->session->dispatch->enumerate_swapchain_images(
            swapchain,
            image_capacity_input,
            image_count_output,
            images);
        if (XR_FAILED(result) || image_count_output == nullptr || images == nullptr || image_capacity_input == 0) {
            return result;
        }
    }

    try {
        if (state->session->graphics_binding == SessionGraphicsBinding::d3d11) {
            if (*image_count_output == 0 ||
                image_capacity_input < *image_count_output) {
                log_swapchain_eligibility(
                    state,
                    SwapchainEligibilityReason::incomplete_enumeration,
                    image_capacity_input,
                    *image_count_output);
                return result;
            }
            const std::uint32_t count = *image_count_output;
            std::vector<ID3D11Texture2D*> resources(count);
            auto* d3d11_images =
                reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            for (std::uint32_t index = 0; index < count; ++index) {
                if (d3d11_images[index].type !=
                        XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR ||
                    d3d11_images[index].texture == nullptr) {
                    log_swapchain_eligibility(
                        state,
                        SwapchainEligibilityReason::invalid_d3d11_image,
                        index,
                        static_cast<std::uint64_t>(d3d11_images[index].type));
                    return result;
                }
                resources[index] = d3d11_images[index].texture;
                D3D11_TEXTURE2D_DESC description{};
                d3d11_images[index].texture->GetDesc(&description);
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::swapchain_image,
                    static_cast<std::int64_t>(description.Format),
                    handle_value(state->handle),
                    handle_value(d3d11_images[index].texture),
                    (static_cast<std::uint64_t>(description.BindFlags) << 32) |
                        description.MiscFlags);
            }

            bool has_generation = false;
            bool resources_changed = false;
            {
                std::scoped_lock lock(state->mutex);
                resources_changed = !state->enumerated_d3d11_images.empty() &&
                    (state->enumerated_d3d11_images.size() != resources.size() ||
                     !std::equal(
                         state->enumerated_d3d11_images.begin(),
                         state->enumerated_d3d11_images.end(),
                         resources.begin(),
                         [](const auto& stored, ID3D11Texture2D* current) {
                             return stored.Get() == current;
                         }));
            }
            if (resources_changed) {
                schedule_generation_quarantine(
                    state->session,
                    GenerationQuarantineReason::d3d11_images_changed,
                    handle_value(state->handle));
                drain_swapchain_gpu(state);
                destroy_composition_capture(state);
                destroy_frame_generation_swapchains(state);
                std::shared_ptr<xrfg::D3D12SwapchainHistory> retired_history;
                {
                    std::scoped_lock lock(state->mutex);
                    retired_history = std::move(state->d3d12_history);
                    state->last_released_capture.reset();
                    state->last_released_motion_vectors.reset();
                }
                retired_history.reset();
            }
            {
                std::scoped_lock lock(state->mutex);
                state->enumerated_d3d11_images.clear();
                for (auto* resource : resources) state->enumerated_d3d11_images.emplace_back(resource);
                has_generation = static_cast<bool>(state->frame_generation);
            }
            SwapchainEligibilityReason eligibility_reason =
                SwapchainEligibilityReason::ready;
            std::uint64_t eligibility_detail = count;
            // Same deferral as the D3D12 path below, and for the same reason:
            // an interop swapchain costs the session three runtime swapchains
            // and an application's UI surfaces are indistinguishable from its
            // stereo views here. The D3D12 path can rebuild its images from
            // the history ring when it arms; this one has to keep them.
            const bool static_image =
                (state->create_info.createFlags &
                 XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
            if (!has_generation && !state->generation_declined &&
                !state->session->generation_budget_exhausted.load(
                    std::memory_order_acquire)) {
                if (static_image) {
                    eligibility_reason =
                        SwapchainEligibilityReason::static_image;
                    eligibility_detail = state->create_info.createFlags;
                } else if (state->create_info.faceCount != 1) {
                    eligibility_reason =
                        SwapchainEligibilityReason::unsupported_face_count;
                    eligibility_detail = state->create_info.faceCount;
                } else {
                    state->enumerated_image_count = count;
                    state->generation_eligible_pending = true;
                    eligibility_reason =
                        SwapchainEligibilityReason::awaiting_projection_use;
                }
            }
            log_swapchain_eligibility(
                state,
                has_generation ? SwapchainEligibilityReason::ready
                               : eligibility_reason,
                eligibility_detail,
                (static_cast<std::uint64_t>(count) << 32) |
                    state->create_info.arraySize);
            return result;
        }
        if (state->session->d3d12_device.Get() == nullptr ||
            state->session->d3d12_queue.Get() == nullptr) {
            log_swapchain_eligibility(
                state, SwapchainEligibilityReason::no_d3d12_binding);
            return result;
        }
        if (*image_count_output == 0 || image_capacity_input < *image_count_output) {
            log_swapchain_eligibility(
                state,
                SwapchainEligibilityReason::incomplete_enumeration,
                image_capacity_input,
                *image_count_output);
            return result;
        }
        const std::uint32_t count = *image_count_output;

        std::vector<ID3D12Resource*> resources(count);
        auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        if (bridge || vulkan_bridge) {
            resources = bridged_resources;
        }
        for (std::uint32_t index = 0; index < count && !bridge && !vulkan_bridge; ++index) {
            if (d3d12_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR ||
                d3d12_images[index].texture == nullptr) {
                log_swapchain_eligibility(
                    state,
                    SwapchainEligibilityReason::invalid_d3d12_image,
                    index,
                    static_cast<std::uint64_t>(d3d12_images[index].type));
                return result;
            }
            resources[index] = d3d12_images[index].texture;
        }
        log_d3d12_image_description(state->handle, resources.front());
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        bool resources_changed = false;
        {
            std::scoped_lock lock(state->mutex);
            history = state->d3d12_history;
            resources_changed = !state->enumerated_d3d12_images.empty() &&
                (state->enumerated_d3d12_images.size() != resources.size() ||
                 !std::equal(
                     state->enumerated_d3d12_images.begin(),
                     state->enumerated_d3d12_images.end(),
                     resources.begin(),
                     [](const auto& stored, ID3D12Resource* current) {
                         return stored.Get() == current;
                     }));
            state->enumerated_d3d12_images.clear();
            for (auto* resource : resources) state->enumerated_d3d12_images.emplace_back(resource);
        }
        if (resources_changed) {
            destroy_composition_capture(state);
            schedule_generation_quarantine(
                state->session,
                GenerationQuarantineReason::d3d12_images_changed,
                handle_value(state->handle));
        }
        const auto release_state = required_release_state(*state);
        const bool depth_only = release_state == D3D12_RESOURCE_STATE_DEPTH_WRITE;
        // D24 (djules V405-V406): projection synthesis never reads a depth
        // swapchain's color-history ring. Keep the reference allocation intact.
        // Actual OptiScaler depth guides and runtime images remain independent.
        const bool omit_depth_history = depth_only && state->session->nvidia_new_route;
        const bool reused_history =
            !omit_depth_history && !resources_changed && history && history->initialized();
        if (!reused_history) {
            history.reset();
        }
        const bool protected_content =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        HRESULT history_result = S_OK;
        bool history_attempted = false;
        if (!history && release_state && !omit_depth_history && !protected_content) {
            history_attempted = true;
            auto candidate = std::make_shared<xrfg::D3D12SwapchainHistory>();
            history_result = candidate->initialize(
                state->session->d3d12_device.Get(),
                state->session->d3d12_queue.Get(),
                std::span<ID3D12Resource* const>(resources.data(), resources.size()),
                *release_state);
            if (SUCCEEDED(history_result)) {
                history = std::move(candidate);
                log_video_memory(*state->session, VideoMemoryStage::history, handle_value(state->handle));
            }
        }
        if (!reused_history) {
            drain_swapchain_gpu(state);
            destroy_frame_generation_swapchains(state);
            std::shared_ptr<xrfg::D3D12SwapchainHistory> retired_history;
            {
                std::scoped_lock lock(state->mutex);
                retired_history = std::move(state->d3d12_history);
                state->d3d12_history = history;
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
            }
            retired_history.reset();
            // The images themselves changed, so an earlier refusal says
            // nothing about this set.
            state->generation_declined = false;
        }
        state->enumerated_image_count = count;

        bool has_generation = false;
        {
            std::scoped_lock lock(state->mutex);
            has_generation = static_cast<bool>(state->frame_generation);
        }
        SwapchainEligibilityReason eligibility_reason =
            SwapchainEligibilityReason::ready;
        std::uint64_t eligibility_detail = count;
        if (!release_state) {
            eligibility_reason =
                SwapchainEligibilityReason::ambiguous_attachment_usage;
            eligibility_detail = state->create_info.usageFlags;
        } else if (protected_content) {
            eligibility_reason = SwapchainEligibilityReason::protected_content;
            eligibility_detail = state->create_info.createFlags;
        } else if (depth_only && omit_depth_history) {
            eligibility_reason = SwapchainEligibilityReason::depth_only;
            eligibility_detail = state->create_info.usageFlags;
        } else if (!history) {
            eligibility_reason = SwapchainEligibilityReason::history_initialize_failed;
            eligibility_detail = history_attempted
                ? static_cast<std::uint64_t>(history_result)
                : 0;
        } else if (*release_state == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            eligibility_reason = SwapchainEligibilityReason::depth_only;
            eligibility_detail = state->create_info.usageFlags;
        }
        // Generation costs three runtime swapchains here and a runtime caps how
        // many one session may hold at all. Nothing about a colour swapchain
        // says whether the application will submit it as a projection view or
        // as a UI quad it composites once, so spending the budget now spends it
        // on both -- and the application, still creating its own swapchains,
        // is the one that finds the ceiling. Record the swapchain as a
        // candidate and let the first xrEndFrame that names it decide.
        //
        // What the create info alone settles is still settled here. A static
        // image and a face count above one can never carry generation, so
        // deferring them would put a candidacy in the log that is never
        // resolved in place of the true reason, and leave an arming attempt to
        // discover at the first frame what was knowable at creation.
        const bool static_image =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (history && release_state == D3D12_RESOURCE_STATE_RENDER_TARGET &&
            !has_generation && !state->generation_declined &&
            !state->session->generation_budget_exhausted.load(
                std::memory_order_acquire)) {
            if (static_image) {
                eligibility_reason = SwapchainEligibilityReason::static_image;
                eligibility_detail = state->create_info.createFlags;
            } else if (state->create_info.faceCount != 1) {
                eligibility_reason =
                    SwapchainEligibilityReason::unsupported_face_count;
                eligibility_detail = state->create_info.faceCount;
            } else {
                state->generation_eligible_pending = true;
                eligibility_reason =
                    SwapchainEligibilityReason::awaiting_projection_use;
            }
        }

        log_swapchain_eligibility(
            state,
            has_generation ? SwapchainEligibilityReason::ready
                           : eligibility_reason,
            eligibility_detail,
            (static_cast<std::uint64_t>(count) << 32) |
                state->create_info.arraySize);

    } catch (...) {
        log_swapchain_eligibility(
            state, SwapchainEligibilityReason::exception);
    }
    return result;
}

XrResult layer_acquire_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->acquire_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    const XrResult result = with_runtime_entry(state->session.get(), [&] {
        return state->session->dispatch->acquire_swapchain_image(
            swapchain, acquire_info, index);
    });
    if (XR_SUCCEEDED(result) && index != nullptr) {
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid) {
            try {
                state->acquired_indices.push_back(*index);
            } catch (...) {
                state->acquired_indices.clear();
                state->waited_count = 0;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->ownership_tracking_valid = false;
            }
        }
    }
    return result;
}

XrResult layer_wait_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->wait_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    const XrResult result = with_runtime_entry(state->session.get(), [&] {
        return state->session->dispatch->wait_swapchain_image(
            swapchain, wait_info);
    });
    if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING) {
        carry_reacquire_to_application(state->session.get());
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid &&
            state->waited_count < state->acquired_indices.size()) {
            const auto image = state->acquired_indices[state->waited_count++];
            if (state->bridge && FAILED(state->bridge->before_write(image)))
                return XR_ERROR_RUNTIME_FAILURE;
            if (state->vulkan_bridge && FAILED(state->vulkan_bridge->before_write(image)))
                return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    return result;
}

// Called at the intercepted release, before forwarding ownership to OpenXR.
// Eagerly snapshot not-yet-classified color chains so a static HUD released
// once at startup is also available when EndFrame first identifies it.
[[nodiscard]] HRESULT capture_composition_image(
    const std::shared_ptr<SwapchainState>& state, std::uint32_t index) {
    if (state->frame_generation && !state->composition_referenced && state->composition_capture)
        state->composition_capture->valid = false;
    if ((state->session->graphics_binding != SessionGraphicsBinding::d3d11 &&
         state->session->graphics_binding != SessionGraphicsBinding::d3d12) ||
        (state->frame_generation && !state->composition_referenced) ||
        !(state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ||
        (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return S_FALSE;
    if (state->session->graphics_binding == SessionGraphicsBinding::d3d12) {
        if (index >= state->enumerated_d3d12_images.size()) return E_INVALIDARG;
        if (!state->composition_capture) {
            auto capture = std::make_unique<CompositionCapture>();
            auto desc = state->enumerated_d3d12_images[index]->GetDesc();
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            HRESULT result = state->session->d3d12_device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&capture->texture12));
            if (SUCCEEDED(result)) result = state->session->d3d12_device->CreateFence(0,
                D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&capture->fence12));
            if (FAILED(result)) return result;
            state->composition_capture = std::move(capture);
        }
        auto& capture = *state->composition_capture;
        capture.valid = false;
        auto& copy = capture.copies12[capture.next_copy12++ % capture.copies12.size()];
        return copy_composition12(state->session->d3d12_device.Get(), state->session->d3d12_queue.Get(),
            capture.fence12.Get(), capture.fence_value, copy,
            capture.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
            state->enumerated_d3d12_images[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (index >= state->enumerated_d3d11_images.size()) return E_INVALIDARG;
    if (!state->composition_capture) {
        auto capture = std::make_unique<CompositionCapture>();
        Microsoft::WRL::ComPtr<ID3D11Device5> device;
        HRESULT result = state->session->d3d11_device.As(&device);
        if (SUCCEEDED(result)) result = state->session->d3d11_context.As(&capture->context);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        result = capture->context.As(&multithread);
        if (FAILED(result)) return result;
        multithread->SetMultithreadProtected(TRUE);
        D3D11_TEXTURE2D_DESC desc{};
        state->enumerated_d3d11_images[index]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
        desc.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        result = device->CreateTexture2D(&desc, nullptr, &capture->texture);
        if (SUCCEEDED(result)) result = device->CreateFence(0, D3D11_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&capture->fence));
        if (FAILED(result)) return result;
        state->composition_capture = std::move(capture);
    }
    auto& capture = *state->composition_capture;
    capture.valid = false;
    capture.context->CopyResource(capture.texture.Get(), state->enumerated_d3d11_images[index].Get());
    const HRESULT result = capture.context->Signal(capture.fence.Get(), capture.fence_value + 1);
    if (SUCCEEDED(result)) ++capture.fence_value;
    capture.context->Flush();
    return result;
}

[[nodiscard]] constexpr bool should_skip_luke_fsr_capture(
    bool luke_split_eye,
    xrfg::D3D12FrameGenerationMode mode,
    bool has_fresh_guides) noexcept {
    return luke_split_eye && mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
        !has_fresh_guides;
}

[[nodiscard]] constexpr bool should_latch_luke_stereo_guides(
    std::uint64_t newest_publication,
    std::uint64_t previous_newest_publication) noexcept {
    return newest_publication > previous_newest_publication &&
        (previous_newest_publication == 0 ||
         newest_publication - previous_newest_publication >= 2);
}

[[nodiscard]] constexpr bool should_evaluate_luke_afw_publication(
    std::uint64_t candidate, std::uint64_t last_evaluated) noexcept {
    return candidate != 0 && candidate > last_evaluated;
}

XrResult release_swapchain_with_identity(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainImageReleaseInfo* release_info,
    std::optional<xrfg::LukeRenderIdentity> luke_identity) {
    if (!state || state->session->dispatch->release_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    bool worker_source = false;
    bool luke_duplicate = false;
    const bool afw_producer = state->session->luke_afw_enabled &&
        xrfg::embedded::afw_snapshot().desired.mode != 0;
    {
        std::scoped_lock lock(state->mutex);
        worker_source = state->frame_generation != nullptr;
        luke_duplicate = worker_source && luke_identity &&
            state->luke_admitted_identity == luke_identity;
    }
    bool director_active = false;
    { std::scoped_lock lock(state->session->presenter_mutex);
      director_active = state->session->three_slot_director && !state->session->presenter_stop_requested;
      if (worker_source && director_active && XR_FAILED(state->session->presenter_failure))
          return state->session->presenter_failure; }
    if (afw_producer || !worker_source || !director_active || !state->session->menu_enabled ||
        state->session->manual_control.stop_requested()) {
        luke_identity.reset();
        luke_duplicate = false;
    }
    if (luke_duplicate) {
        // No history/composition copy, gpu_mutex, worker or slot wait. A may
        // still be read by the worker: retain its private capture and vectors.
        std::scoped_lock call_lock(state->call_mutex);
        const XrResult result = with_runtime_entry(state->session.get(), [&] {
            return state->session->dispatch->release_swapchain_image(state->handle, release_info);
        });
        if (XR_SUCCEEDED(result)) {
            std::scoped_lock lock(state->mutex);
            if (state->ownership_tracking_valid && state->waited_count > 0 && !state->acquired_indices.empty()) {
                state->last_released_index = state->acquired_indices.front();
                state->acquired_indices.pop_front();
                --state->waited_count;
                state->luke_released_identity = luke_identity;
                state->luke_released_duplicate = true;
            } else {
                state->ownership_tracking_valid = false;
                state->acquired_indices.clear(); state->waited_count = 0;
                state->luke_released_identity.reset();
                state->luke_released_duplicate = true; // never hand retained A off as new B
            }
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_release,
            XR_SUCCEEDED(result) ? 1 : -1, handle_value(state->handle), luke_identity->render, 0);
        return result;
    }
    if (worker_source && !afw_producer) {
        const XrResult worker_ready = wait_for_synthesis_handoff(state->session);
        if (XR_FAILED(worker_ready)) return worker_ready;
    }
    std::scoped_lock call_lock(state->call_mutex);
    std::optional<std::uint32_t> candidate_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> d3d11_interop;
    {
        std::scoped_lock lock(state->mutex);
        bridge = state->bridge;
        vulkan_bridge = state->vulkan_bridge;
        if (state->ownership_tracking_valid && state->waited_count > 0 &&
            !state->acquired_indices.empty()) {
            candidate_index = state->acquired_indices.front();
            history = state->d3d12_history;
            if (state->frame_generation) {
                d3d11_interop = state->frame_generation->d3d11_interop;
            }
        }
    }

    // Bridged: the application's rendering goes to the runtime's image now,
    // whatever generation does with it afterwards. A failed copy is logged
    // and the release still goes down, so the runtime shows its last content
    // rather than the session failing.
    if (bridge && candidate_index) {
        const HRESULT copy_result = bridge->release(*candidate_index);
        if (FAILED(copy_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                3,
                handle_value(state->handle),
                *candidate_index,
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(copy_result)));
        }
    }

    if (vulkan_bridge && candidate_index) {
        const HRESULT copied = vulkan_bridge->release(*candidate_index);
        if (FAILED(copied)) {
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::vulkan_bridge,
                copied, handle_value(state->handle), *candidate_index, 3);
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

    // Worker handoff precedes call_mutex on both APIs. The synchronous/pipelined
    // routes still need their own ownership gate, but never enter it from the director.
    if (candidate_index && history && state->session->d3d12_synthesis_queue &&
        !state->session->three_slot_director) {
        static_cast<void>(wait_for_previous_session_synthesis(state->session));
    }
    std::unique_lock<std::mutex> gpu_lock;
    if (state->session->manual_control.stop_requested() ||
        (!state->session->menu_enabled &&
         !state->session->luke_afw_enabled)) {
        // Preserve the application's acquire/wait/release bookkeeping, but
        // stop recording new bridge history after the manual arm is revoked.
        history.reset();
        d3d11_interop.reset();
    }
    if (candidate_index && (history || state->session->graphics_binding == SessionGraphicsBinding::d3d11 ||
                           state->session->graphics_binding == SessionGraphicsBinding::d3d12)) {
        gpu_lock = std::unique_lock<std::mutex>(state->session->gpu_mutex);
    }

    const HRESULT composition_result = candidate_index
        ? capture_composition_image(state, *candidate_index) : S_FALSE;

    std::optional<xrfg::D3D12HistoryCaptureTicket> pending_capture;
    std::shared_ptr<const xrfg::DlssMotionVectorSet> pending_motion_vectors;
    if (candidate_index && history) {
        const bool luke_split_eye = d3d11_interop &&
            (state->session->graphics_binding_capabilities & 8ULL) != 0 &&
            state->dlss_logical_eye_count == 2 &&
            state->dlss_logical_eye_index.has_value();
        const bool luke_private_stereo_guides = luke_split_eye &&
            state->session->frame_generation_mode ==
                xrfg::D3D12FrameGenerationMode::fsr3;
        // AFW resolves the guide at xrEndFrame against the eye actually
        // released on this application frame. A release-time pair may trail
        // the real colour by one or more NGX publications.
        if (state->session->dlss_motion_vectors &&
            !(luke_split_eye && state->session->luke_afw_enabled)) {
            std::scoped_lock lock(state->mutex);
            if (*candidate_index < state->enumerated_d3d12_images.size()) {
                pending_motion_vectors = xrfg::resolve_dlss_motion_vectors(
                    state->enumerated_d3d12_images[*candidate_index].Get(),
                    state->session->d3d12_queue.Get());
            } else if (d3d11_interop) {
                std::shared_ptr<const xrfg::DlssMotionVectorSet> resolved;
                if (luke_split_eye) {
                    std::scoped_lock guide_lock(
                        state->session->luke_guide_mutex);
                    if (!state->session->luke_frame_guide_resolved) {
                        state->session->luke_frame_guide_resolved = true;
                        auto candidate = luke_private_stereo_guides
                            ? xrfg::resolve_luke_alternating_guides(
                                  state->session->d3d12_queue.Get())
                            : xrfg::resolve_dlss_motion_vectors(
                                  state->dlss_logical_eye_count,
                                  state->session->d3d12_queue.Get());
                        std::uint64_t newest_publication = 0;
                        if (candidate) {
                            for (std::uint32_t eye = 0;
                                 eye < candidate->eye_count; ++eye) {
                                if (candidate->eyes[eye]) {
                                    newest_publication = std::max(
                                        newest_publication,
                                        candidate->eyes[eye]->publication);
                                }
                            }
                        }
                        if (candidate &&
                            (luke_private_stereo_guides
                                ? should_latch_luke_stereo_guides(
                                      newest_publication,
                                      state->session->
                                          luke_last_consumed_guide_publication)
                                : newest_publication > state->session->
                                      luke_last_consumed_guide_publication)) {
                            state->session->luke_last_consumed_guide_publication =
                                newest_publication;
                            state->session->luke_frame_motion_vectors =
                                std::move(candidate);
                        }
                    }
                    resolved = state->session->luke_frame_motion_vectors;
                } else {
                    resolved = xrfg::resolve_dlss_motion_vectors(
                        state->dlss_logical_eye_count,
                        state->session->d3d12_queue.Get());
                }
                if (resolved && state->dlss_logical_eye_index) {
                    // The phase is a title-specific calibration, not a
                    // global NGX contract.
                    const bool newest_is_left = luke_private_stereo_guides &&
                        resolved->eye_count == 2 && resolved->eyes[1] &&
                        (((resolved->eyes[1]->serial & 1U) != 0) ==
                         state->session->luke_afw_odd_serial_is_left);
                    const auto eye = luke_private_stereo_guides
                        ? (*state->dlss_logical_eye_index ==
                                (newest_is_left ? 0U : 1U) ? 1U : 0U)
                        : *state->dlss_logical_eye_index;
                    if (eye < resolved->eye_count && resolved->eyes[eye]) {
                        auto selected =
                            std::make_shared<xrfg::DlssMotionVectorSet>();
                        selected->eye_count = 1;
                        selected->eyes[0] = resolved->eyes[eye];
                        pending_motion_vectors = std::move(selected);
                    }
                } else {
                    pending_motion_vectors = std::move(resolved);
                }
            }
        }
        const bool skip_capture =
            should_skip_luke_fsr_capture(
                luke_split_eye,
                state->session->frame_generation_mode,
                static_cast<bool>(pending_motion_vectors));
        if (skip_capture) {
            if (xrfg::two_frame_capture_active()) {
                xrfg::TwoFrameImageInfo missing{};
                missing.stage = "xr_final_eye_skipped_no_guide";
                missing.swapchain = handle_value(state->handle);
                missing.eye = state->dlss_logical_eye_index
                    ? static_cast<std::int32_t>(
                        *state->dlss_logical_eye_index)
                    : -1;
                missing.width = state->create_info.width;
                missing.height = state->create_info.height;
                xrfg::two_frame_capture_note(missing,
                    HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_capture,
                S_FALSE,
                handle_value(state->handle),
                *candidate_index,
                2);
        } else {
            xrfg::D3D12HistoryCaptureTicket ticket{};
            HRESULT capture_result = S_OK;
            if (d3d11_interop) {
                const auto interop_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_capture,
                    handle_value(state->handle),
                    *candidate_index,
                    0);
                capture_result =
                    d3d11_interop->prepare_capture(*candidate_index);
                xrfg::bridge_flight_logger().end(
                    interop_token,
                    xrfg::BridgeFlightOperation::d3d11_capture,
                    capture_result,
                    handle_value(state->handle),
                    *candidate_index,
                    0);
            }
            if (SUCCEEDED(capture_result)) {
                xrfg::TwoFrameImageInfo diagnostic_image{};
                const bool capture_image = xrfg::two_frame_capture_active();
                if (capture_image) {
                    diagnostic_image.stage = "xr_final_eye";
                    diagnostic_image.swapchain = handle_value(state->handle);
                    diagnostic_image.eye = state->dlss_logical_eye_index
                        ? static_cast<std::int32_t>(
                            *state->dlss_logical_eye_index)
                        : -1;
                    diagnostic_image.width = state->create_info.width;
                    diagnostic_image.height = state->create_info.height;
                }
                capture_result = history->capture(*candidate_index, &ticket,
                    capture_image ? &diagnostic_image : nullptr);
            }
            if (SUCCEEDED(capture_result) && d3d11_interop) {
                const HRESULT finish_result = d3d11_interop->finish_capture();
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::d3d11_capture,
                    finish_result,
                    handle_value(state->handle),
                    *candidate_index,
                    1);
                capture_result = finish_result;
            }
            if (SUCCEEDED(capture_result)) {
                pending_capture = ticket;
            } else if (ticket.serial != 0) {
                history->discard(ticket);
            }
        }
    }

    std::uint64_t diagnostic_capture = 0;
    if (candidate_index && pending_capture && d3d11_interop && state->session->frame_capture &&
        state->session->frame_capture->active() && *candidate_index < state->enumerated_d3d11_images.size()) {
        diagnostic_capture = state->session->frame_capture->source(handle_value(state->handle),
            state->enumerated_d3d11_images[*candidate_index].Get(), *candidate_index,
            pending_capture->serial, pending_capture->fence_value);
    }
    if (bridge && candidate_index) {
        // Every read of the shared texture this frame - the copy and the
        // capture - is queued; the next acquire of this image waits for it.
        static_cast<void>(bridge->mark_read(*candidate_index));
    }

    if (vulkan_bridge && candidate_index && FAILED(vulkan_bridge->mark_read(*candidate_index)))
        return XR_ERROR_RUNTIME_FAILURE;

    mark_application_release(state->session.get());
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        result = with_runtime_entry(state->session.get(), [&] {
            return state->session->dispatch->release_swapchain_image(
                state->handle, release_info);
        });
    } catch (...) {
        if (pending_capture && history) {
            history->discard(*pending_capture);
        }
        throw;
    }

    if (state->composition_capture && composition_result != S_FALSE)
        state->composition_capture->valid = XR_SUCCEEDED(result) && composition_result == S_OK;
    if (XR_SUCCEEDED(result)) {
        bool commit_capture = false;
        {
            std::scoped_lock lock(state->mutex);
            if (state->ownership_tracking_valid && state->waited_count > 0 &&
                !state->acquired_indices.empty()) {
                const std::uint32_t released_index = state->acquired_indices.front();
                state->acquired_indices.pop_front();
                --state->waited_count;
                state->last_released_index = released_index;
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                state->luke_released_identity.reset();
                state->luke_released_duplicate = false;
                commit_capture = pending_capture &&
                                 pending_capture->source_index == released_index;
            } else if (state->ownership_tracking_valid) {
                state->acquired_indices.clear();
                state->waited_count = 0;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                state->ownership_tracking_valid = false;
                state->luke_released_identity.reset();
                state->luke_released_duplicate = false;
            }
        }

        if (pending_capture && history) {
            const bool committed =
                commit_capture && SUCCEEDED(history->commit(*pending_capture));
            if (committed) {
                std::scoped_lock lock(state->mutex);
                state->last_released_capture = pending_capture;
                state->luke_released_identity = luke_identity;
                state->last_diagnostic_capture = diagnostic_capture;
                state->last_released_motion_vectors = pending_motion_vectors;
                if (luke_identity)
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_release,
                        0, handle_value(state->handle), luke_identity->render, pending_capture->serial);
                const bool luke_split_eye =
                    (state->session->graphics_binding_capabilities & 8ULL) != 0 &&
                    state->dlss_logical_eye_count == 2 &&
                    state->dlss_logical_eye_index.has_value();
                if (luke_split_eye && state->session->luke_afw_enabled) {
                    // result=logical eye, a=stage 32, b=XR colour capture
                    // serial, c=application swapchain.
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_afw_stage,
                        static_cast<std::int64_t>(
                            *state->dlss_logical_eye_index),
                        32, pending_capture->serial,
                        handle_value(state->handle));
                }
                if (luke_split_eye && pending_motion_vectors &&
                    pending_motion_vectors->eye_count != 0 &&
                    pending_motion_vectors->eyes[0]) {
                    const auto& guide = *pending_motion_vectors->eyes[0];
                    if (state->session->luke_afw_enabled) log_luke_guide_basis(guide);
                    const std::int64_t logical_eye = static_cast<std::int64_t>(
                        *state->dlss_logical_eye_index);
                    // result=logical eye; a=application swapchain;
                    // b=capture serial; c=DLSS publication.
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_guide_contract,
                        logical_eye,
                        handle_value(state->handle),
                        pending_capture->serial,
                        guide.publication);
                    // result=100+logical eye; a=DLSS stream; b=DLSS serial;
                    // c=hash of the camera position/right/forward/FOV data.
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_guide_contract,
                        100 + logical_eye,
                        guide.stream,
                        guide.serial,
                        dlss_guide_camera_hash(guide));
                }
                if (luke_split_eye && state->session->luke_afw_enabled) {
                    std::scoped_lock guide_lock(
                        state->session->luke_guide_mutex);
                    state->session->luke_afw_released_eye =
                        *state->dlss_logical_eye_index;
                }
            } else {
                history->discard(*pending_capture);
                std::scoped_lock lock(state->mutex);
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
            }
        }
    } else if (pending_capture && history) {
        history->discard(*pending_capture);
    }
    return result;
}

XrResult layer_release_swapchain_image_impl(XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info) {
    const auto state = find_swapchain(swapchain);
    // Snapshot before any worker wait. The reader never touches application pixels.
    return release_swapchain_with_identity(state, release_info, xrfg::query_luke_render_identity());
}

struct ProjectionLayerCopy {
    std::uint32_t layer_index{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> views;
};

using OwnedCompositionLayer = std::variant<
    XrCompositionLayerQuad,
    XrCompositionLayerCubeKHR,
    XrCompositionLayerCylinderKHR,
    XrCompositionLayerEquirectKHR,
    XrCompositionLayerEquirect2KHR,
    XrCompositionLayerPassthroughFB,
    XrCompositionLayerPassthroughHTC,
    XrCompositionLayerPassthroughANDROID>;

[[nodiscard]] bool acquire_and_wait_private_image(SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch, PrivateSwapchainState& image) noexcept;

[[nodiscard]] bool hold_djules_synthetic(std::chrono::nanoseconds age,
    std::chrono::nanoseconds period, bool pixels_ready, bool prior_owned_frame,
    bool pipelined) noexcept {
    return prior_owned_frame && !pipelined && period>std::chrono::nanoseconds::zero() &&
        (age<period || (!pixels_ready && age<period*3));
}

struct DjulesPendingOutput {
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    std::shared_ptr<DjulesOutputStaging> staging;
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    xrfg::D3D12FrameSynthesisTicket ticket;
    bool synthetic{};
};

[[nodiscard]] bool handover_djules_outputs(SessionState* session,
    std::vector<DjulesPendingOutput>& outputs) noexcept {
    try {
        while(!outputs.empty()) {
            auto& pending=outputs.front(); auto& generation=*pending.generation;
            auto& image=pending.synthetic?generation.synthetic[0]:generation.current[0];
            auto& images=pending.synthetic?generation.synthetic_runtime_images:generation.current_runtime_images;
            if(FAILED(pending.synthesizer->synchronize_consumer_queue(runtime_queue(*session),pending.ticket)) ||
               !acquire_and_wait_private_image(session,session->dispatch,image) || image.acquired_index>=images.size())
                return false;
            const auto copy_slot=generation.handover_copy_slot++ % generation.handover_copies.size();
            const HRESULT copied=copy_composition12(session->d3d12_device.Get(),runtime_queue(*session),
                generation.handover_fence.Get(),generation.handover_serial,generation.handover_copies[copy_slot],
                images[image.acquired_index].Get(),D3D12_RESOURCE_STATE_COMMON,
                pending.staging->resource.Get(),D3D12_RESOURCE_STATE_COMMON);
            if(FAILED(copied)) return false;
            pending.staging->last_handover_copy.store(generation.handover_serial,std::memory_order_release);
            if(!release_private_image(session,session->dispatch,image)) return false;
            outputs.erase(outputs.begin());
        }
        return true;
    } catch(...) { return false; }
}

struct GeneratedFrameEndInfo {
    std::vector<DjulesPendingOutput> pending_outputs;
    std::uint64_t app_release_value{};
    bool synthetic{};
    bool djules_leader{};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<ProjectionLayerCopy> projections;
    std::vector<OwnedCompositionLayer> composition_layers;
    std::vector<const XrCompositionLayerBaseHeader*> layer_pointers;
    std::vector<std::shared_ptr<CompositionImageSlot>> composition_images;
    // Synthesizers whose current copy is recorded but not yet submitted.
    // Only the synthetic half of a pair carries these: the copy must reach
    // the queue after this frame has been handed over and before the
    // current frame follows a display period later.
    std::vector<std::shared_ptr<xrfg::D3D12FrameSynthesizer>>
        pending_current_copies;
};

// A buffer position owns only this descriptor. The pixels stay in one
// bridge-owned swapchain image until its director submission has completed.
struct ThreeSlotFrame {
    std::chrono::steady_clock::time_point queued_at{};
    std::uint64_t source_sequence{};
    std::uint64_t previous_sequence{};
    std::shared_ptr<GeneratedFrameEndInfo> frame;
};

struct PostAfwFrameInput {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    XrSwapchain completed_swapchain{XR_NULL_HANDLE};
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    xrfg::D3D12HistoryCaptureTicket capture{};
    std::vector<xrfg::D3D12ReprojectionView> views;
    xrfg::ColorIdentityTag color_identity;
};

[[nodiscard]] xrfg::ColorIdentityTag make_color_identity(
    std::uint64_t serial, std::uint64_t capture, XrSwapchain swapchain,
    std::uint32_t image, std::uint32_t eye, std::uint32_t role,
    const xrfg::D3D12ReprojectionView& source, std::uint32_t xr_index=0) noexcept {
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.position={source.pose.position.x,source.pose.position.y,source.pose.position.z};
    view.pose.orientation={source.pose.orientation.x,source.pose.orientation.y,
        source.pose.orientation.z,source.pose.orientation.w};
    view.fov={source.fov.angle_left,source.fov.angle_right,source.fov.angle_up,source.fov.angle_down};
    return {serial,capture,handle_value(swapchain),projection_pose_hash(view),image,eye,role,xr_index};
}

// The very same completed AFW submission is used by direct XR and OFXR.
// History tickets own the finished pixels; the frozen submission owns their
// original camera/FOV/space/rect metadata. No consumer samples newer app state.
struct CompletedAfwFrame {
    std::shared_ptr<GeneratedFrameEndInfo> submission;
    ProjectionSnapshot snapshot;
    std::vector<PostAfwFrameInput> inputs;
    std::uint64_t application_serial{};
};

struct SynthesisJob {
    xrfg::D3D12PresentationTransform presentation_transform{};
    ProjectionSnapshot snapshot;
    std::vector<ProjectionResourceMapping> mappings;
    std::shared_ptr<GeneratedFrameEndInfo> metadata;
    std::shared_ptr<ThreeSlotFrame> current;
    std::shared_ptr<const CompletedAfwFrame> post_afw_frame;
    bool pair{};
    bool triple{};
    float fraction{0.5F},extra_fraction{2.0F/3.0F};
};

[[nodiscard]] XrResult wait_for_synthesis_handoff(
    const std::shared_ptr<SessionState>& state) noexcept {
    std::unique_lock lock(state->presenter_mutex);
    if (!state->three_slot_director) return XR_SUCCESS;
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::three_slot_worker_wait, handle_value(state->handle));
    state->presenter_condition.wait(lock, [&] {
        return !state->synthesis_busy || state->presenter_stop_requested ||
               XR_FAILED(state->presenter_failure);
    });
    const XrResult result = XR_FAILED(state->presenter_failure)
        ? state->presenter_failure : XR_SUCCESS;
    lock.unlock();
    xrfg::bridge_flight_logger().end(token,
        xrfg::BridgeFlightOperation::three_slot_worker_wait, result);
    return result;
}


[[nodiscard]] std::optional<OwnedCompositionLayer>
copy_composition_layer_for_presenter(
    const XrCompositionLayerBaseHeader* source) {
    if (source == nullptr) {
        return std::nullopt;
    }

#define XRFG_COPY_COMPOSITION_LAYER(structure_type, structure_name)            \
    case structure_type: {                                                    \
        structure_name copy =                                                 \
            *reinterpret_cast<const structure_name*>(source);                 \
        copy.next = nullptr;                                                  \
        return OwnedCompositionLayer{copy};                                   \
    }
    switch (source->type) {
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_QUAD,
            XrCompositionLayerQuad)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_CUBE_KHR,
            XrCompositionLayerCubeKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR,
            XrCompositionLayerCylinderKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR,
            XrCompositionLayerEquirectKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR,
            XrCompositionLayerEquirect2KHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
            XrCompositionLayerPassthroughFB)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC,
            XrCompositionLayerPassthroughHTC)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_ANDROID,
            XrCompositionLayerPassthroughANDROID)
        default:
            return std::nullopt;
    }
#undef XRFG_COPY_COMPOSITION_LAYER
}

[[nodiscard]] const XrCompositionLayerBaseHeader* composition_layer_header(
    OwnedCompositionLayer& layer) noexcept {
    return std::visit(
        [](auto& value) {
            return reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &value);
        },
        layer);
}

[[nodiscard]] std::shared_ptr<GeneratedFrameEndInfo>
make_presenter_owned_frame(GeneratedFrameEndInfo&& source) {
    if (source.info.next != nullptr || source.projections.empty() ||
        source.info.layerCount != source.layer_pointers.size()) {
        return nullptr;
    }

    auto output = std::make_shared<GeneratedFrameEndInfo>(std::move(source));
    std::vector<bool> projection_indices(output->layer_pointers.size(), false);
    for (ProjectionLayerCopy& projection : output->projections) {
        if (projection.layer_index >= output->layer_pointers.size() ||
            projection.views.empty() ||
            projection_indices[projection.layer_index]) {
            return nullptr;
        }
        projection_indices[projection.layer_index] = true;
        projection.layer.next = nullptr;
        for (XrCompositionLayerProjectionView& view : projection.views) {
            view.next = nullptr;
        }
        projection.layer.views = projection.views.data();
        output->layer_pointers[projection.layer_index] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &projection.layer);
    }

    output->composition_layers.reserve(
        output->layer_pointers.size() - output->projections.size());
    for (std::size_t index = 0; index < output->layer_pointers.size(); ++index) {
        if (projection_indices[index]) {
            continue;
        }
        auto layer = copy_composition_layer_for_presenter(
            output->layer_pointers[index]);
        if (!layer) {
            return nullptr;
        }
        output->composition_layers.push_back(std::move(*layer));
        output->layer_pointers[index] = composition_layer_header(
            output->composition_layers.back());
    }

    output->info.next = nullptr;
    output->info.layers = output->layer_pointers.data();
    return output;
}

[[nodiscard]] XrDuration doubled_display_period(XrDuration period) noexcept {
    if (period <= 0) {
        return period;
    }
    constexpr XrDuration maximum = std::numeric_limits<XrDuration>::max();
    return period > maximum / 2 ? maximum : period * 2;
}

[[nodiscard]] XrTime add_display_duration(
    XrTime time,
    XrDuration duration) noexcept {
    if (duration <= 0) {
        return time;
    }
    constexpr XrTime maximum = std::numeric_limits<XrTime>::max();
    return time > maximum - duration ? maximum : time + duration;
}

// Djules df029629 / V417: these errors reject this frame's contents, not
// the session. Drop it and return its error; preserve all fatal-error handling.
[[nodiscard]] constexpr bool frame_content_refused(XrResult result) noexcept {
    return result == XR_ERROR_LAYER_INVALID ||
           result == XR_ERROR_LAYER_LIMIT_EXCEEDED ||
           result == XR_ERROR_SWAPCHAIN_RECT_INVALID ||
           result == XR_ERROR_POSE_INVALID;
}

void fail_pending_presenter_submissions_locked(
    SessionState& state,
    XrResult failure) noexcept {
    if (XR_FAILED(failure) && XR_SUCCEEDED(state.presenter_failure)) {
        state.presenter_failure = failure;
    }
    for (const auto& request : state.presenter_submissions) {
        request->result = failure;
        request->completed = true;
    }
    state.presenter_submissions.clear();
    state.outstanding_presenter_submissions = 0;
}

// Holds the presenter to one submission per display period.
//
// The layer submits two frames for every application frame and relies on
// xrWaitFrame to space them, one per scanout. That holds only while the
// runtime actually throttles the wait. Measured against MSFS 2024: VDXR
// blocks it for a metronomic 9.95-9.97 ms and the pair goes out 11.11 ms
// apart, exactly as intended; SteamVR with the Pimax driver returns it in
// 1.55 ms, so the presenter free-runs and submits the pair 1.12 ms apart -
// both inside one 11.11 ms window - followed by a 21.1 ms gap.
//
// A compositor holding one submitted frame at a time then never scans out the
// first of each pair: the second replaces it. Half the generated frames are
// discarded before they are ever displayed, which is why the layer's own
// overlay reported a steady 90 while the compositor reported 10% reprojection
// and about 80 FPS, and why neither pipeline depth nor synthesis readiness
// moved the result - the frames were being dropped for their timing, never
// for their contents.
//
// Pacing here rather than trusting the wait costs nothing where the wait
// already paces: the elapsed check passes immediately and the runtime's own
// blocking still sets the cadence. The wait is interruptible, and it happens
// inside the begun frame, so the runtime measures a frame that contains the
// work actually being done rather than an empty window. It must not run
// under presenter_content_mutex - waiting for presenter progress while
// holding that lock has deadlocked this layer twice.
void pace_presenter_submission(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        refresh_djules_display_period(state);
        const auto entered = std::chrono::steady_clock::now();
        std::int64_t behind_us = 0;
        std::chrono::nanoseconds remaining{0};
        // -1 pulled earlier off the ceiling, +1 pushed later off the floor.
        int pace_band_correction = 0;
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (!state->presenter_schedule_valid ||
                state->presenter_display_period <= 0 ||
                state->presenter_stop_requested) {
                return;
            }
            const auto period = std::chrono::nanoseconds(
                static_cast<std::int64_t>(state->presenter_display_period));
            auto ready_at = state->presenter_next_submit;
            behind_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    entered - ready_at)
                    .count();
            if (ready_at > entered) {
                // Never hold longer than one period, whatever the schedule
                // says. Pacing exists to stop submissions bunching up; it must
                // never be able to hold the presenter back instead.
                ready_at = std::min(ready_at, entered + period);
                remaining = ready_at - entered;
            }
            {
                // The one restoring force on the grid's phase. Every other
                // path here moves the deadline later - the catch-up adds whole
                // periods, the step-over adds one - and the phase reference
                // below cannot pull it back, because it tracks whatever phase
                // the presenter is already holding and so has no opinion about
                // a grid that is self-consistent and parked at the worst place
                // in the period. Left alone the schedule ratchets to the end
                // of the period and stays there.
                //
                // Measured in Callisto Protocol on SteamVR: V155 held 10.30 ms
                // of an 11.11 ms period, dead flat from the seventh second to
                // the end of the run, with 45.0 in, 90.0 out, an 11.111 ms
                // grid and no skips or bunching at all. Every submission
                // landed as late in its period as it could, the compositor
                // kept the real frame and dropped the synthetic, and the
                // headset showed 45. The builds that worked held 1.7 to 9.6 ms
                // and moved about.
                //
                // `entered` is the moment the runtime's own xrWaitFrame
                // released this presenter, so the hold is measured against the
                // runtime's timeline rather than the layer's own, and the
                // threshold means something absolute. Three quarters leaves
                // the working range untouched and only acts on a schedule that
                // has walked to the end.
                //
                // There is a floor as well as a ceiling, and it is not
                // symmetry for its own sake. This hold *is* the window the
                // runtime measures its client across: the pace runs inside the
                // begun frame precisely so that begin-to-end spans the real
                // work rather than an empty gap (47d8590). Let the hold reach
                // zero and begin and end go back to back again, the runtime is
                // handed a frame that costs a millisecond, and it schedules
                // against that - asking for the frame late, assuming it will
                // be ready, and reprojecting when it is not.
                //
                // That regressed here. The phase correction walks the grid
                // earlier and nothing bounded it from below, so in The
                // Witcher 3 the measured window fell from 8.0 ms at 35 s to
                // 0.06 ms at 50 s, with 97.3% of frames reporting under a
                // millisecond, and recovered only when the catch-up and
                // step-over had pushed the schedule back later. Twenty-five
                // seconds of the runtime scheduling against an empty client,
                // with the layer's own view a flawless 45 in, 90 out, 11.111
                // ms grid, no skips and no bunching throughout.
                //
                // So drive the hold into a band rather than off one edge. A
                // quarter to three quarters of a period brackets the 5.7 to
                // 8.5 ms the healthy stretches of that same run held, and
                // since every other path moves the deadline later the
                // schedule settles near the top of the band, which is also
                // where the measured window is longest.
                //
                // The floor applies when the presenter is already past its
                // deadline too - `remaining` is zero there, and that is the
                // case that empties the window completely.
                //
                // Both ends wait for a run of on-grid submissions, for the
                // same reason the phase reference does: a grid that is
                // skipping slots has no stable phase to correct towards, and
                // correcting one anyway closes a loop. Measured in Atomic
                // Heart on V157, where the application hitched to 16-33/s for
                // four seconds: the starvation skips pushed the grid later
                // through the catch-up, that lifted the hold over the ceiling,
                // the pull then fired on 44-83% of frames and held the grid at
                // 10.55-10.78 ms against 11.111, and running fast tripped the
                // step-over into the next skip. It sustained itself for nine
                // seconds after the application was back at 43-45/s, throwing
                // away five to seven slots a second where two would have done,
                // and stopped only when the hold drifted back under the
                // ceiling on its own.
                //
                // The gate costs nothing in the cases the band exists for.
                // Both were clean grids: Callisto parked at the ceiling for
                // 47 s with no skips at all, and The Witcher 3 walked to the
                // floor with 0.0-0.4 skips a second.
                // The ceiling carries the bias. The pair is deliberately unevenly
                // spaced, so the hold before a synthetic is longer by exactly
                // that much; against a bare 3/4 of a period the band reads it
                // as a grid that has walked to the end and pulls against the
                // bias. Measured while the bias was being walked by hand: mean
                // hold 4.46 -> 6.88 -> 8.29 ms against an 8.33 ms ceiling, with
                // the pull rate going 2.6% -> 7.2% -> 8.5%. Offset, never
                // removed - section 14 is what happens without a ceiling at all.
                const auto band_ceiling =
                    period * 3 / 4 + state->presenter_pair_bias;
                if (state->presenter_on_grid_streak <
                    kPhaseCorrectionGridStreak) {
                    // Rate is wrong; leave the schedule alone.
                } else if (remaining > band_ceiling) {
                    state->presenter_next_submit -= period / 16;
                    pace_band_correction = -1;
                } else if (remaining < period / 4) {
                    state->presenter_next_submit += period / 16;
                    pace_band_correction = 1;
                }
            }
        }
        // Slept without the lock: the application thread enqueues against this
        // mutex, and a paced presenter holding it would stall the very frame
        // it is waiting for.
        if (remaining > std::chrono::nanoseconds::zero()) {
            if (state->presenter_pace_timer == nullptr) {
                state->presenter_pace_timer = CreateWaitableTimerExW(
                    nullptr,
                    nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                    TIMER_ALL_ACCESS);
                if (state->presenter_pace_timer == nullptr) {
                    // Pre-1803, or the flag was refused. A coarse timer still
                    // beats the condition variable's tick rounding.
                    state->presenter_pace_timer = CreateWaitableTimerExW(
                        nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                }
            }
            if (state->presenter_pace_timer != nullptr) {
                LARGE_INTEGER due{};
                // Negative is relative, in 100 ns units.
                due.QuadPart = -(remaining.count() / 100);
                if (SetWaitableTimer(
                        state->presenter_pace_timer,
                        &due,
                        0,
                        nullptr,
                        nullptr,
                        FALSE)) {
                    static_cast<void>(WaitForSingleObject(
                        state->presenter_pace_timer, INFINITE));
                }
            }
        }
        // result=1 marks a frame where the hold had reached the ceiling and
        // the grid was pulled back off it, result=2 one pushed up off the
        // floor, so a capture shows which edge the schedule is being held away
        // from and whether the band is working or fighting.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_pace,
            pace_band_correction < 0 ? 1 : (pace_band_correction > 0 ? 2 : 0),
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - entered)
                    .count()),
            static_cast<std::uint64_t>(behind_us < 0 ? -behind_us : behind_us),
            behind_us > 0 ? 1u : 0u);
    } catch (...) {
    }
}

void continuous_presenter_main(
    const std::shared_ptr<SessionState>& state) noexcept {
    std::uint64_t clock_anchor_tick = 0;
    LARGE_INTEGER qpc_frequency{};
    static_cast<void>(QueryPerformanceFrequency(&qpc_frequency));
    for (;;) {
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (state->presenter_stop_requested) {
                break;
            }
        }


        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        const auto wait_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::internal_wait_frame,
            handle_value(state->handle));
        const XrResult wait_result = wait_presenter_runtime_frame(state, &wait_info, &frame_state);
        xrfg::bridge_flight_logger().end(
            wait_token,
            xrfg::BridgeFlightOperation::internal_wait_frame,
            wait_result,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
            frame_state.shouldRender);
        if (XR_FAILED(wait_result)) {
            std::scoped_lock lock(state->presenter_mutex);
            fail_pending_presenter_submissions_locked(*state, wait_result);
            state->presenter_condition.notify_all();
            break;
        }

        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_frame_state = frame_state;
            state->presenter_frame_state.next = nullptr;
            state->presenter_frame_state_valid = true;
            // One tick per presenter frame. The application's wait counts these
            // so that it is released once per pair rather than once per frame.
            ++state->presenter_frame_serial;
            // Lock the grid to the display the runtime is actually scanning
            // out on. One frame per period means consecutive waits advance
            // predictedDisplayTime by exactly one period; a repeat says two
            // submissions were aimed at one scanout and the grid is ahead, a
            // skip says a scanout went unfilled and it is behind. Correct by
            // a bounded fraction so a single odd prediction cannot jerk the
            // cadence, and only once the grid exists to be corrected.
            const XrDuration locked_period =
                state->presenter_display_period;
            if (state->presenter_last_predicted_valid &&
                state->presenter_schedule_valid && locked_period > 0) {
                const XrTime previous =
                    state->presenter_last_predicted_display;
                const XrTime current = frame_state.predictedDisplayTime;
                const XrDuration advance = current > previous
                    ? static_cast<XrDuration>(current - previous)
                    : 0;
                // A predicted time that did not move, or moved absurdly,
                // is the runtime not describing a new scanout - a session
                // transition, a frame it does not want rendered, a stall.
                // It is not evidence about this grid's phase, and treating
                // a repeat as proof the grid was early is what let the
                // deadline run away into the future.
                const bool usable_signal =
                    advance > 0 && advance < locked_period * 8;
                const std::int64_t slots = usable_signal
                    ? (advance + locked_period / 2) / locked_period
                    : 1;
                if (slots != 1) {
                    const auto period_ns =
                        std::chrono::nanoseconds(locked_period);
                    // A repeated slot means the grid is early and must be
                    // pushed later; a skipped one means it is late. Step by a
                    // sixteenth of a period, which still walks out a whole slot
                    // in about a fifth of a second and cannot bunch a pair
                    // inside one scanout window on its own.
                    const auto step = period_ns / 16;
                    if (slots < 1) {
                        state->presenter_next_submit += step;
                        // Never let a correction put the deadline further
                        // out than one period. Every other path moves it
                        // later too, so without a ceiling the schedule can
                        // only walk forwards, and a presenter that keeps
                        // sleeping longer stops draining the queue the
                        // application is admitted against.
                        const auto ceiling =
                            std::chrono::steady_clock::now() + period_ns;
                        if (state->presenter_next_submit > ceiling) {
                            state->presenter_next_submit = ceiling;
                        }
                    } else {
                        state->presenter_next_submit -= step;
                    }
                }
            }
            state->presenter_last_predicted_display =
                frame_state.predictedDisplayTime;
            state->presenter_last_predicted_valid =
                frame_state.predictedDisplayTime > 0;
            // Keep the smallest plausible period seen, so a runtime that
            // inflates the value under load cannot inflate the pace with it.
            constexpr XrDuration kShortestCredibleDisplayPeriod = 2'000'000;
            constexpr XrDuration kLongestCredibleDisplayPeriod = 50'000'000;
            if (frame_state.predictedDisplayPeriod >=
                    kShortestCredibleDisplayPeriod &&
                frame_state.predictedDisplayPeriod <=
                    kLongestCredibleDisplayPeriod &&
                (state->presenter_display_period == 0 ||
                 frame_state.predictedDisplayPeriod <
                     state->presenter_display_period)) {
                state->presenter_display_period =
                    frame_state.predictedDisplayPeriod;
            }
        }
        state->presenter_condition.notify_all();

        // A successful runtime wait supplies the virtual application timing,
        // but do not begin a frame until there is valid composition to submit.
        // This prevents an unsupported first application layer list from being
        // alternated with empty presenter frames while still allowing the
        // virtualized application to advance and enqueue that list.
        {
            std::unique_lock lock(state->presenter_mutex);
            state->presenter_condition.wait(lock, [&] {
                return state->presenter_stop_requested ||
                       XR_FAILED(state->presenter_failure) ||
                       state->presenter_last_frame != nullptr ||
                       !state->presenter_submissions.empty();
            });
        }

        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        const auto begin_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::internal_begin_frame,
            handle_value(state->handle));
        const XrResult begin_result = with_runtime_entry(state, [&] {
            return state->dispatch->begin_frame(state->handle, &begin_info);
        });
        xrfg::bridge_flight_logger().end(
            begin_token,
            xrfg::BridgeFlightOperation::internal_begin_frame,
            begin_result,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
        if (XR_FAILED(begin_result)) {
            std::scoped_lock lock(state->presenter_mutex);
            fail_pending_presenter_submissions_locked(*state, begin_result);
            state->presenter_condition.notify_all();
            break;
        }

        // The pace runs here, inside the begun frame, rather than before the
        // cycle. A runtime measures an application frame between xrBeginFrame
        // and xrEndFrame; calling them back to back, as this loop did, gives
        // it a frame containing nothing - measured as 0.73 ms of CPU and
        // under a millisecond of GPU, while the real work costs 12 ms of
        // application rendering and 3.59 ms of synthesis outside the window.
        //
        // A scheduler told its client costs a millisecond has every reason to
        // ask for the frame late and to assume it will be ready. Holding the
        // frame open across the pace is what every ordinary application does
        // - begin, render, end - and lets the queue timestamps span the work
        // actually being done.
        pace_presenter_submission(state);

        std::shared_ptr<PresenterSubmission> request;
        std::shared_ptr<GeneratedFrameEndInfo> repeated_frame;
        XrResult end_result = XR_ERROR_RUNTIME_FAILURE;
        std::uint32_t submitted_layer_count = 0;
        XrTime source_display_time = 0;
        // Which half of the pair the presenter submitted, kept at this scope so
        // the flight record below can carry it.
        bool fresh_synthetic = false;
        // When the downstream xrEndFrame was entered, so the bias controller
        // below can see how long the runtime held this submission.
        auto downstream_end_started = std::chrono::steady_clock::now();
        {
            // Do not let the application release a newer private output while
            // the runtime is resolving the previous handle's last image.
            std::scoped_lock content_lock(state->presenter_content_mutex);
            {
                std::scoped_lock lock(state->presenter_mutex);
                if (!state->presenter_stop_requested &&
                    !state->presenter_submissions.empty()) {
                    request = state->presenter_submissions.front();
                    state->presenter_submissions.pop_front();
                } else if (!state->presenter_stop_requested) {
                    repeated_frame = state->presenter_last_frame;
                }
                state->presenter_inflight_submission = request;
                state->presenter_inflight_frame = request
                    ? request->owned_frame : repeated_frame;
            }

            const XrFrameEndInfo* source = request
                ? request->owned_frame
                    ? &request->owned_frame->info
                    : request->borrowed_frame
                : repeated_frame
                    ? &repeated_frame->info
                    : nullptr;
            source_display_time = source ? source->displayTime : 0;
            XrFrameEndInfo submitted{XR_TYPE_FRAME_END_INFO};
            if (source != nullptr) {
                submitted = *source;
                submitted.next = nullptr;
                submitted.displayTime = frame_state.predictedDisplayTime;
            } else {
                submitted.displayTime = frame_state.predictedDisplayTime;
                submitted.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            }
            if (frame_state.shouldRender == XR_FALSE) {
                submitted.layerCount = 0;
                submitted.layers = nullptr;
            }
            submitted_layer_count = submitted.layerCount;
            const auto end_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_end_frame,
                handle_value(state->handle),
                static_cast<std::uint64_t>(submitted.displayTime),
                submitted.layerCount);
            fresh_synthetic = request && request->owned_frame &&
                request->owned_frame->synthetic;
            if (state->fps_overlay && state->manual_control.stop_requested())
                state->fps_overlay->suspend();
            downstream_end_started = std::chrono::steady_clock::now();
            join_runtime_queue_to_application(state.get(), request
                ? request->owned_frame ? request->owned_frame->app_release_value : request->app_release_value
                : repeated_frame ? repeated_frame->app_release_value : 0);
            end_result = with_runtime_entry(state, [&] {
                auto* owned=request?request->owned_frame.get():repeated_frame.get();
                if(owned && djules_native_scheduler(*state)) {
                        for(const auto& image:owned->composition_images)
                            if(image->fence12 && FAILED(runtime_queue(*state)->Wait(image->fence12.Get(),image->ready_value)))
                                return XR_ERROR_RUNTIME_FAILURE;
                    }
                    if(owned && !handover_djules_outputs(state.get(),owned->pending_outputs)) return XR_ERROR_RUNTIME_FAILURE;
                return state->fps_overlay
                    ? state->fps_overlay->end_frame(&submitted, fresh_synthetic, request != nullptr)
                    : state->dispatch->end_frame(state->handle, &submitted);
            });
            // The synthetic has reached the runtime, so its current copy can
            // go to the queue now rather than ahead of it. It has a display
            // period before the current frame that reads it is submitted.
            if (request && request->owned_frame) {
                for (const auto& synthesizer :
                     request->owned_frame->pending_current_copies) {
                    if (synthesizer) {
                        static_cast<void>(
                            synthesizer->flush_current_copy(
                                state->d3d12_synthesis_queue
                                    ? runtime_queue(*state)
                                    : nullptr));
                    }
                }
            }
            // Where in the period this submission landed, measured against the
            // scanout the runtime aimed it at. The epochs differ, so only
            // comparisons between these values mean anything - which is what
            // the phase correction below uses them for.
            const std::int64_t submitted_lead =
                static_cast<std::int64_t>(frame_state.predictedDisplayTime) -
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            {
                // Advance the schedule by exactly one period so this loop's
                // own cost does not compound into the cadence, and resync
                // rather than chase if a stall has put the schedule in the
                // past - catching up would submit a burst, which is the very
                // thing the pace exists to prevent.
                std::scoped_lock lock(state->presenter_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto period = std::chrono::nanoseconds(
                    static_cast<std::int64_t>(state->presenter_display_period));
                if (!state->presenter_schedule_valid ||
                    state->presenter_display_period <= 0) {
                    state->presenter_next_submit = now + period;
                    state->presenter_schedule_valid =
                        state->presenter_display_period > 0;
                } else {
                    // The two frames of a pair do not cost the runtime the
                    // same, so spacing them evenly gives them unequal margin.
                    // Measured in Atomic Heart across 1479 pairs, with the
                    // submission record finally carrying which half of the
                    // pair it was: the synthetic's xrEndFrame takes 2.12 ms
                    // against the current's 0.68, and the synthetic lands
                    // 2.36 ms closer to its own scanout. (V163 asserted the
                    // opposite from a capture anchored on the enqueue, which
                    // labels the previous pair's current as this pair's
                    // synthetic. It was wrong by exactly that swap.)
                    //
                    // That 1.44 ms of extra call time falls *between* the two
                    // submissions, so an even deadline spacing produces an
                    // uneven arrival spacing:
                    //
                    //   synthetic -> current   11.11 + 0.68 - 2.12 =  9.67 ms
                    //   current -> synthetic   11.11 + 2.12 - 0.68 = 12.55 ms
                    //
                    // Two arrivals 9.67 ms apart can land in one scanout
                    // window, and the compositor keeps one of them. Every
                    // metric the layer owns still reads 45 in, 90 out, two
                    // submissions per pair - which is why ten builds of pace
                    // work moved this around without fixing it. Moving the
                    // grid carries the asymmetry with it.
                    //
                    // So bias the pair, not the grid, and give the room to the
                    // frame whose call is long: an eighth of a period after
                    // the synthetic, the same back after the current. The two
                    // sum to exactly two periods, so the schedule does not
                    // drift, and both frames arrive one period apart. A repeat
                    // is not part of a pair and advances plainly.
                    // Adaptive pair bias. Shortening the interval after the
                    // synthetic lengthens the one before the *next* one, which
                    // gives that synthetic more age before its slot arrives -
                    // so the runtime blocks less, the gap to the real frame
                    // stops collapsing, and the real frame stops being
                    // discarded. See LOW_HEADROOM_PLAN.md section 16.
                    //
                    // Walked rather than computed: bias and excess are coupled,
                    // because raising the bias lowers the excess it is derived
                    // from. Predicting the result from the current distribution
                    // is the mistake section 9 forbids, so nudge and re-measure.
                    //
                    // Zero restores even spacing exactly, which is what makes
                    // this safe where there is no block to buy out - another
                    // runtime, a lower resolution, a heavier game, any refresh
                    // rate. It self-gates; the SteamVR test is belt and braces
                    // until there is a capture from each runtime rather than
                    // one smooth Quest 3 run.
                    if (request) {
                        const auto call = now - downstream_end_started;
                        auto& mean = fresh_synthetic
                            ? state->presenter_synthetic_call_mean
                            : state->presenter_real_call_mean;
                        mean = mean.count() == 0
                            ? std::chrono::duration_cast<
                                  std::chrono::nanoseconds>(call)
                            : mean + (std::chrono::duration_cast<
                                          std::chrono::nanoseconds>(call) -
                                      mean) / 16;
                        // The arrival gap, measured where it matters: between
                        // the two hand-overs, so it already carries whatever
                        // the runtime spent inside the synthetic's call.
                        if (fresh_synthetic) {
                            state->presenter_synthetic_returned_at = now;
                        } else if (state->presenter_synthetic_returned_at !=
                                   std::chrono::steady_clock::time_point{}) {
                            const auto gap = std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                now - state->presenter_synthetic_returned_at);
                            if (gap > std::chrono::nanoseconds::zero() &&
                                gap < period * 4) {
                                state->presenter_pair_gap_mean =
                                    state->presenter_pair_gap_mean.count() == 0
                                        ? gap
                                        : state->presenter_pair_gap_mean +
                                              (gap -
                                               state->presenter_pair_gap_mean) /
                                                  16;
                            }
                            state->presenter_synthetic_returned_at = {};
                        }
                    }
                    // Six times a second, not once. The block grows and shrinks
                    // with scene load - it is larger while the view is moving
                    // and smaller when it is still - so a controller that takes
                    // fifteen seconds to cover a change loses frames for the
                    // first several of them and gets them back only once the
                    // scene settles. Reported exactly that way: losing it at
                    // times, recovering after a second or two when standing
                    // still.
                    constexpr std::uint32_t kBiasUpdateFrames = 15;
                    if (++state->presenter_bias_tick >= kBiasUpdateFrames) {
                        state->presenter_bias_tick = 0;
                        // A dead band, and the rates are deliberately not
                        // symmetric. A low excess is the *success* condition -
                        // the block is suppressed because the bias is holding
                        // it down - so treating it as a reason to back off
                        // makes the controller remove its own correction and
                        // hunt. Measured doing exactly that: bias oscillating
                        // between 1.39 and 2.43 ms, with the excess swinging
                        // 0.11 to 4.24 in step with it, where the value found
                        // by hand was 2.78 and stable.
                        //
                        // So climb while the block is present, hold across the
                        // band, and decay only when the excess is clearly low,
                        // slowly enough to keep the level it found. Decay
                        // still has to exist: a genuine change - lower
                        // resolution, a heavier game, a different runtime -
                        // must bring it back to zero, and at this rate a full
                        // bias unwinds in about two minutes.
                        constexpr auto kBlockThreshold =
                            std::chrono::microseconds(500);
                        constexpr auto kSuppressedThreshold =
                            std::chrono::microseconds(200);
                        const auto excess =
                            state->presenter_synthetic_call_mean -
                            state->presenter_real_call_mean;
                        // Climb in proportion to the block, so a large one is
                        // covered in a step or two rather than walked up to.
                        // Bounded at both ends: a floor so it always makes
                        // progress, a cap so a single bad frame cannot throw
                        // the spacing. Overshoot is cheap here because the
                        // ceiling bounds it and the decay unwinds it.
                        auto climb = excess / 4;
                        const auto climb_floor = period / 128;
                        const auto climb_cap = period / 32;
                        if (climb < climb_floor) climb = climb_floor;
                        if (climb > climb_cap) climb = climb_cap;
                        const auto decay = period / 2048;
                        // period/4 was where the hand-walk stopped, not a value
                        // anything justified, and the controller saturates
                        // against it: measured pinned at 2.78 ms through a
                        // heavy stretch while the excess reached 3.53 ms, with
                        // the real frames lost for the duration.
                        //
                        // The trade is explicit. Every millisecond of bias is a
                        // millisecond less between the synthetic and the real
                        // frame, and below about 9 ms the two risk the same
                        // scanout - which is the failure this whole mechanism
                        // exists to prevent. period/3 leaves 7.4 ms and is
                        // about as far as that can go before the two
                        // constraints meet.
                        const auto ceiling = period / 4;
                        const bool steamvr = state->dispatch &&
                            state->dispatch->steamvr_runtime;
                        // Both failures are now observable, so the bias is bounded
                        // by measurement at both ends instead of by a constant.
                        // Too little and the synthetic goes over unfinished,
                        // the runtime blocks and the real frame is lost; too
                        // much and the real frame follows too soon and the
                        // synthetic is lost. Walking on the excess alone only
                        // ever saw the first, so it pushed until the second
                        // stopped it - period/6 short, period/4 close,
                        // period/3 past it, and none of those numbers survive a
                        // different headset.
                        //
                        // Order matters. While the block is present the gap is
                        // compressed *by the block*, not by the bias, so the
                        // block is dealt with first; the gap only governs once
                        // it is suppressed. Without that the controller cannot
                        // climb out of a bad state, because the state it is in
                        // makes the gap look too tight to act.
                        const auto gap_floor = period * 9 / 10;
                        const bool gap_tight =
                            state->presenter_pair_gap_mean.count() != 0 &&
                            state->presenter_pair_gap_mean < gap_floor;
                        if (steamvr && excess > kBlockThreshold) {
                            state->presenter_pair_bias =
                                state->presenter_pair_bias + climb > ceiling
                                    ? ceiling
                                    : state->presenter_pair_bias + climb;
                        } else if (gap_tight || !steamvr ||
                                   excess < kSuppressedThreshold) {
                            state->presenter_pair_bias =
                                state->presenter_pair_bias > decay
                                    ? state->presenter_pair_bias - decay
                                    : std::chrono::nanoseconds::zero();
                        }
                        xrfg::bridge_flight_logger().event(
                            xrfg::BridgeFlightOperation::presenter_transition,
                            300,
                            static_cast<std::uint64_t>(
                                state->presenter_pair_bias.count()),
                            static_cast<std::uint64_t>(
                                state->presenter_synthetic_call_mean.count()),
                            static_cast<std::uint64_t>(
                                state->presenter_pair_gap_mean.count()));
                    }
                    const auto pair_bias = state->presenter_pair_bias;
                    const auto advance = request
                        ? (fresh_synthetic ? period - pair_bias
                                           : period + pair_bias)
                        : period;
                    state->presenter_next_submit += advance;
                    // Pull the grid back towards the best phase this
                    // presenter has managed. Everything else here corrects
                    // the grid's *rate* - a repeated scanout or a skipped
                    // one - and so only runs when the rate is wrong. Once one
                    // submission lands per scanout the rate is right at every
                    // phase, including phases that put the submission on top
                    // of the runtime's deadline, so those corrections switch
                    // off and whatever offset the last disturbance left is
                    // frozen. Captured: a hard scene walked the offset 7.7 ms
                    // later in two steps and it never came back, while the
                    // layer's own view stayed a flawless 45 in, 90 out, 100%
                    // on grid - the grid was self-consistent and simply in
                    // the wrong place.
                    //
                    // predictedDisplayTime is the runtime's own scanout time,
                    // so the interval from it back to the submission says
                    // where in the period this presenter sits. Only *where in
                    // the period* though - the lead itself is not comparable
                    // across a skipped slot. The runtime advances
                    // predictedDisplayTime by exactly one period per wait
                    // whether or not the presenter filled the slot, so missing
                    // one costs a whole period of lead while changing nothing
                    // about the phase. Measured on MSFS 2024 at a flat 45 in,
                    // 90 out with nothing missing: the controller read every
                    // skip as an 11 ms deficit, pulled the grid earlier by
                    // period/16 each frame chasing a period it cannot recover,
                    // and the walk tripped the step-over into the next skip.
                    // A 5 Hz limit cycle - submissions 10.645 ms apart against
                    // an 11.111 ms period across 3117 samples, ~4 slots a
                    // second discarded, and the pace sleep sawtoothing from
                    // 11 ms down to 2 and back, which is what the application
                    // sees as ratcheting CPU frame time.
                    //
                    // So reduce both sides modulo the period before comparing.
                    // A skipped slot then reads as no phase error at all,
                    // which is the truth.
                    //
                    // Reducing is not enough on its own, and the first attempt
                    // at it (V154) made a worse failure than the one it fixed.
                    // It took the difference the short way round the period,
                    // into (-period/2, period/2], so a loss of more than half
                    // a period came back with the wrong sign. Measured in
                    // Callisto Protocol: SteamVR's own xrEndFrame ran 3.8 to
                    // 8.0 ms on alternating frames for 80 ms, the presenter
                    // overran and recovered onto a grid point through one
                    // 19.4 ms gap, and the margin to the runtime's scanout
                    // dropped 8.3 ms in one step. The controller read that as
                    // being 2.8 ms *early*, corrected nothing, and the grid
                    // stayed 8.3 ms closer to the deadline for the remaining
                    // 26 seconds - a flawless 45 in, 90 out, 99.92% on grid,
                    // all of it landing too late to be shown.
                    //
                    // The two readings describe the same timeline and the lead
                    // cannot separate them. So do not try: take the deficit
                    // into [0, period) and always read it as being behind,
                    // because pulling earlier is the direction that gains
                    // margin and the step-over below is what stops it bunching.
                    // Two dead bands keep that from firing on noise - nothing
                    // under a millisecond, and nothing within an eighth of a
                    // period of a whole one, which is the reference sitting
                    // just under a steady phase after a decay step.
                    const std::int64_t period_ns = period.count();
                    const std::int64_t submitted_phase =
                        ((submitted_lead % period_ns) + period_ns) % period_ns;
                    // Distance from b forward to a, in [0, period).
                    const auto phase_deficit =
                        [period_ns](std::int64_t difference) -> std::int64_t {
                        return ((difference % period_ns) + period_ns) %
                            period_ns;
                    };
                    const std::int64_t kAheadBand = period_ns / 8;

                    // Whether the *rate* was right for this submission. Phase
                    // is only meaningful once it is - a grid that is skipping
                    // slots has no stable phase to correct towards - so the
                    // correction waits for a run of these.
                    const auto since_previous =
                        state->presenter_last_submitted_at ==
                            std::chrono::steady_clock::time_point{}
                        ? period
                        : (now - state->presenter_last_submitted_at);
                    // Against what the schedule asked for, not against one
                    // period: the pair is deliberately biased above, so an
                    // even period is the wrong expectation for both halves of
                    // it and would read every submission as off grid.
                    const auto expected =
                        state->presenter_expected_interval >
                            std::chrono::nanoseconds::zero()
                        ? state->presenter_expected_interval
                        : period;
                    const bool landed_on_grid =
                        since_previous > expected - period / 10 &&
                        since_previous < expected + period / 10;
                    state->presenter_on_grid_streak =
                        landed_on_grid ? state->presenter_on_grid_streak + 1 : 0;
                    state->presenter_last_submitted_at = now;
                    state->presenter_expected_interval = advance;

                    // The reference follows the best phase achieved, and bleeds
                    // down about a period every four seconds so a runtime that
                    // genuinely changes its timing is tracked rather than
                    // chased forever against a stale best.
                    //
                    // Only a submission that landed on the grid may raise the
                    // reference, and only by a small step. Without the grid
                    // test the controller feeds itself: pulling the deadline
                    // earlier lengthens the lead, the longer lead becomes the
                    // new best, and the next frame is pulled earlier again,
                    // creeping forward until the step-over shoves the deadline
                    // a whole period and starts over. Measured on a 90 Hz
                    // Pimax with the application flat at 45/s: the phase
                    // walked 6 ms earlier across a minute, the mean gap stayed
                    // a perfect 11.11 ms, and only 78% of submissions landed in
                    // their slot. Without the step limit the Callisto case
                    // above would latch its own 8.3 ms loss as the new best,
                    // because that loss is also readable as a small gain.
                    //
                    // This runs before the correction so that a submission
                    // which really is ahead sets the mark rather than being
                    // corrected towards a mark it has already passed.
                    const std::int64_t ahead_of_reference = phase_deficit(
                        submitted_phase - state->presenter_lead_reference);
                    if (!state->presenter_lead_valid ||
                        (landed_on_grid && ahead_of_reference > 0 &&
                         ahead_of_reference <= kAheadBand)) {
                        state->presenter_lead_reference = submitted_phase;
                        state->presenter_lead_valid = true;
                    } else {
                        constexpr std::int64_t kLeadReferenceDecay = 31'000;
                        state->presenter_lead_reference = phase_deficit(
                            state->presenter_lead_reference -
                            kLeadReferenceDecay);
                    }

                    // Eight consecutive on-grid submissions is a quarter of a
                    // second at 90 Hz. It is what separates the two captures
                    // above: Callisto ran clean for 26 s carrying its 8.3 ms
                    // loss, so the correction gets to run, while the MSFS
                    // capture was a deliberately hard scene skipping slots
                    // about twelve times a second, where it stays switched off
                    // and cannot ratchet.
                    if (state->presenter_on_grid_streak >=
                        kPhaseCorrectionGridStreak) {
                        const std::int64_t deficit = phase_deficit(
                            state->presenter_lead_reference - submitted_phase);
                        constexpr std::int64_t kLeadTolerance = 1'000'000;
                        if (deficit > kLeadTolerance &&
                            deficit < period_ns - kAheadBand) {
                            const auto correction =
                                std::min<std::chrono::nanoseconds>(
                                    period / 16,
                                    std::chrono::nanoseconds(deficit));
                            state->presenter_next_submit -= correction;
                        }
                    }
                    // fresh full period for being late. Resetting to now+period
                    // instead made every cycle cost a period plus whatever the
                    // loop took, which is how a 11.11 ms pace produced 15.5 ms
                    // submissions and held the runtime to 64/s.
                    if (state->presenter_next_submit <= now) {
                        const auto behind = now - state->presenter_next_submit;
                        state->presenter_next_submit +=
                            (behind / period + 1) * period;
                    }
                    // Then step off any deadline that falls too soon after the
                    // frame just handed over. The schedule is an absolute grid
                    // and knows nothing about how long this submission took;
                    // when the runtime's own xrEndFrame ran long - 3.7 ms
                    // typical against 17 ms worst on SteamVR - the next grid
                    // point can be a couple of milliseconds away, so the pair
                    // lands inside one scanout window and the compositor keeps
                    // only the later one. That is the bunching the pace exists
                    // to prevent, reappearing after an overrun instead of at
                    // free-run: measured as a repeating on-grid, long, short
                    // cadence with 28.8% of gaps under 9 ms against an 11.11 ms
                    // period.
                    //
                    // The threshold has to clear the bunching without eating
                    // slots the presenter could still have filled. Stepping
                    // over costs a whole scanout - the slot shows a repeat -
                    // so the band is not free, and at half a period it fires
                    // on roughly half the deadlines left behind an overrun,
                    // which is exactly the case where content is already
                    // scarce. A quarter still clears the 3.7 ms typical
                    // xrEndFrame that produces the bunching, and keeps the
                    // deadlines between a quarter and a half of a period out
                    // - about 2.8 ms of every 11.11 - that half a period
                    // discarded.
                    //
                    // Steady state is unaffected either way: the deadline
                    // there already falls about 7 ms after the handover, so
                    // neither threshold fires. Chaining every deadline off
                    // the handover instead would add the loop's own cost to
                    // each cycle, which is what held an earlier build to
                    // 64/s.
                    //
                    // If this is too narrow it will show as submission gaps
                    // bunching under about 9 ms against the 11.11 ms period,
                    // which is the compositor discarding one of a pair.
                    const auto earliest = now + period / 4;
                    while (state->presenter_next_submit < earliest) {
                        state->presenter_next_submit += period;
                    }
                }
            }
            xrfg::bridge_flight_logger().end(
                end_token,
                xrfg::BridgeFlightOperation::internal_end_frame,
                end_result,
                static_cast<std::uint64_t>(submitted.displayTime),
                submitted.layerCount,
                frame_state.shouldRender);
        }
        // c carries which half of the pair this was: 2 synthetic, 1 current,
        // 0 a repeat. Without it the two are indistinguishable in a capture -
        // anchoring on the enqueue does not work, because the submission that
        // follows it is the previous pair's current, not this pair's
        // synthetic. That mislabelling is what made the V162 margin
        // comparison name the wrong frame.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_submission,
            end_result,
            request ? request->sequence : 0,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            request ? (fresh_synthetic ? 2u : 1u) : 0u);
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_inflight_submission.reset();
            state->presenter_inflight_frame.reset();
            if (request) {
                request->result = end_result;
                request->completed = true;
                if (state->outstanding_presenter_submissions != 0) {
                    --state->outstanding_presenter_submissions;
                }
                if (XR_SUCCEEDED(end_result) && request->owned_frame &&
                    !state->pipelined_presenter_mode) {
                    state->presenter_last_frame = request->owned_frame;
                    if (request->owned_frame->synthetic) {
                        state->presenter_pending_current_pair_sequence =
                            request->pair_sequence;
                    } else if (request->pair_sequence != 0 &&
                               request->pair_sequence ==
                                   state->presenter_pending_current_pair_sequence) {
                        state->presenter_pending_current_pair_sequence = 0;
                    }
                } else if (state->pipelined_presenter_mode) {
                    // A persistent pipelined application may destroy or replace
                    // XrSpace and non-projection swapchain handles as soon as
                    // its xrEndFrame returns. Never repeat retained application
                    // handles beyond that call boundary; let the runtime hold
                    // the last submitted image until the next owned pair.
                    state->presenter_last_frame.reset();
                    state->presenter_pending_current_pair_sequence = 0;
                }
            }
            if (frame_content_refused(end_result)) {
                state->presenter_last_frame.reset();
                state->presenter_pending_current_pair_sequence = 0;
            } else if (XR_FAILED(end_result)) {
                fail_pending_presenter_submissions_locked(*state, end_result);
            }
        }
        state->presenter_condition.notify_all();
        // Instrumentation runs after the submission's ownership and queue
        // state are settled, so an optional runtime conversion cannot delay
        // the next application admission for this pair.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_source,
            0,
            request ? request->sequence : 0,
            static_cast<std::uint64_t>(source_display_time),
            request ? request->pair_sequence : 0);
        if (state->dispatch->convert_time_to_qpc != nullptr &&
            (clock_anchor_tick++ % 64) == 0) {
            LARGE_INTEGER predicted_counter{};
            const XrResult conversion_result =
                state->dispatch->convert_time_to_qpc(
                    state->dispatch->instance,
                    frame_state.predictedDisplayTime,
                    &predicted_counter);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::presenter_clock_anchor,
                conversion_result,
                static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
                XR_SUCCEEDED(conversion_result)
                    ? static_cast<std::uint64_t>(
                          xrfg::bridge_flight_logger().microseconds_for_counter(
                              predicted_counter.QuadPart))
                    : 0,
                static_cast<std::uint64_t>(qpc_frequency.QuadPart));
        }
        if (XR_FAILED(end_result) && !frame_content_refused(end_result)) {
            break;
        }
    }

    {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_submissions.empty()) {
            const XrResult failure = XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
            fail_pending_presenter_submissions_locked(*state, failure);
        }
    }
    state->presenter_condition.notify_all();
}

// Fresh-output consumer for the NVIDIA three-slot director, legacy NVIDIA
// S/B queue, and SteamVR FSR S/B queue. It never submits a retained real twice
// and never emits empty frames just to keep a counter moving.
void three_slot_director_main(const std::shared_ptr<SessionState>& state) noexcept {
    try {
        // Both consumers use a fixed grid rather than anchoring each deadline
        // to a late wake-up. Keep FSR's long-End recovery distinct from NVIDIA.
        xrfg::TwoOutputSchedule fsr_schedule;
        xrfg::TwoOutputSchedule director_schedule;
        xrfg::MidpointSchedule midpoint_schedule;
        DjulesAbsoluteSchedule djules_schedule;
        std::chrono::steady_clock::time_point djules_last_returned{};
        std::shared_ptr<GeneratedFrameEndInfo> djules_last_owned;
        auto configuration_epoch=state->djules_configuration_epoch.load();
        for (;;) {
            std::shared_ptr<ThreeSlotFrame> selected;
            std::shared_ptr<PresenterSubmission> borrowed;
            const auto ready_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_director_wait, handle_value(state->handle));
            {
                std::unique_lock lock(state->presenter_mutex);
                state->presenter_condition.wait(lock, [&] {
                    return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) ||
                        !state->presenter_submissions.empty() ||
                        (state->three_slots.releasable(state->director_draining) &&
                         state->three_slots.slot(0)->value->frame);
                });
                if (state->presenter_stop_requested || XR_FAILED(state->presenter_failure)) break;
                if (state->three_slots.releasable(state->director_draining) &&
                    state->three_slots.slot(0)->value->frame) {
                    selected = state->three_slots.slot(0)->value;
                    state->presenter_inflight_frame = selected->frame;
                } else {
                    borrowed = state->presenter_submissions.front();
                    state->presenter_submissions.pop_front();
                    state->presenter_inflight_submission = borrowed;
                    state->presenter_inflight_frame = borrowed->owned_frame;
                }
            }
            const auto fail = [&](XrResult result) {
                std::scoped_lock lock(state->presenter_mutex);
                if (borrowed) { borrowed->result = result; borrowed->completed = true; }
                fail_pending_presenter_submissions_locked(*state, result);
                state->presenter_inflight_frame.reset();
                state->presenter_inflight_submission.reset();
                state->presenter_condition.notify_all();
            };
            xrfg::bridge_flight_logger().end(ready_token,
                xrfg::BridgeFlightOperation::timing_director_wait, 0,
                selected ? selected->source_sequence : 0,
                selected ? selected->previous_sequence : 0);
            // FSR projection publication already orders the synthetic fence on
            // the OpenXR binding queue. Owned auxiliary copies are submitted on
            // that same queue before image release. XR may take its next frame
            // cycle while those commands are pending; a CPU fence wait here
            // serializes the consumer behind all preceding application GPU work.
            // Keep the images leased through EndFrame and retain allocator-reuse
            // waits in copy_composition12; neither dependency needs this stall.
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState prediction{XR_TYPE_FRAME_STATE};
            auto token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_wait_frame, handle_value(state->handle));
            XrResult result = wait_presenter_runtime_frame(state, &wait, &prediction);
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::internal_wait_frame,
                result, prediction.predictedDisplayTime, prediction.predictedDisplayPeriod,
                prediction.shouldRender);
            if (XR_FAILED(result)) { fail(result); break; }
            {
                std::scoped_lock lock(state->presenter_mutex);
                state->presenter_frame_state = prediction;
                state->presenter_frame_state.next = nullptr;
                state->presenter_frame_state_valid = true;
                ++state->presenter_frame_serial;
                if (prediction.predictedDisplayPeriod >= 2'000'000 &&
                    prediction.predictedDisplayPeriod <= 50'000'000 &&
                    (!state->presenter_display_period ||
                     prediction.predictedDisplayPeriod < state->presenter_display_period))
                    state->presenter_display_period = prediction.predictedDisplayPeriod;
            }
            refresh_djules_display_period(state);
            state->presenter_condition.notify_all();
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_begin_frame, handle_value(state->handle));
            result = with_runtime_entry(state, [&] {
                return state->dispatch->begin_frame(state->handle, &begin);
            });
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::internal_begin_frame,
                result, prediction.predictedDisplayTime);
            if (XR_FAILED(result)) { fail(result); break; }
            // Only the consumer is limited to headset cadence. The game's
            // WaitFrame contains no sleep and no dependency on director ticks.
            bool synthetic = selected ? selected->frame->synthetic
                : borrowed->owned_frame && borrowed->owned_frame->synthetic;
            const auto pace_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::presenter_pace);
            std::chrono::steady_clock::time_point submission_slot{};
            {
                const auto period = std::chrono::nanoseconds(state->presenter_display_period);
                const auto now = std::chrono::steady_clock::now();
                if (djules_native_scheduler(*state) && (state->three_slot_director || state->queued_nvidia_presenter)) {
                    if (state->dispatch->steamvr_runtime || state->dispatch->virtual_desktop_runtime)
                        submission_slot = djules_schedule.claim(now, period);
                    else
                        submission_slot = djules_handover_floor(now, djules_last_returned, period);
                }
                else if (state->three_slot_director)
                    submission_slot = midpoint_schedule.claim(now, period, synthetic);
                else {
                    auto& schedule = state->fsr_two_slot_presenter ? fsr_schedule : director_schedule;
                    submission_slot = schedule.claim(now, period);
                }
                const auto* probe=selected?selected->frame.get():borrowed->owned_frame.get();
                if(djules_triple_active(*state) && probe && probe->djules_leader) {
                    const auto deadline=submission_slot-std::chrono::milliseconds(4);
                    if(deadline>std::chrono::steady_clock::now()) std::this_thread::sleep_until(deadline);
                    bool ready=!probe->pending_outputs.empty();
                    for(const auto& output:probe->pending_outputs)
                        ready &= output.synthesizer->ticket_available(output.ticket)==S_OK;
                    for(const auto& image:probe->composition_images)
                        ready &= !image->fence12 || image->fence12->GetCompletedValue()>=image->ready_value;
                    std::int64_t delay_us=-1,ceiling_us=0;unsigned late=0;
                    { std::scoped_lock lock(state->presenter_mutex);
                      if(state->djules_triple_release.observe(ready,period)) {
                        delay_us=state->djules_triple_release.delay.count()/1000;
                        ceiling_us=state->djules_triple_release.ceiling.count()/1000;
                        late=state->djules_triple_release.last_late;
                      } }
                    if(delay_us>=0) xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_transition,801,delay_us,late,ceiling_us);
                }
                const auto remaining = submission_slot - std::chrono::steady_clock::now();
                if (remaining > std::chrono::steady_clock::duration::zero()) {
                    if (!state->presenter_pace_timer) {
                        state->presenter_pace_timer = CreateWaitableTimerExW(nullptr, nullptr,
                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                        if (!state->presenter_pace_timer)
                            state->presenter_pace_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                    }
                    LARGE_INTEGER due{};
                    due.QuadPart = -(std::max)(1LL,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
                    if (state->presenter_pace_timer && SetWaitableTimer(state->presenter_pace_timer,
                            &due, 0, nullptr, nullptr, FALSE))
                        static_cast<void>(WaitForSingleObject(state->presenter_pace_timer, INFINITE));
                    else
                        std::this_thread::sleep_until(submission_slot);
                }
            }
            xrfg::bridge_flight_logger().end(pace_token,
                xrfg::BridgeFlightOperation::presenter_pace, 0);
            const auto epoch=state->djules_configuration_epoch.load();
            if(epoch!=configuration_epoch) {
                djules_last_owned.reset(); djules_schedule={}; djules_last_returned={};
                configuration_epoch=epoch;
            }
            bool repeat_held=false;
            bool drain_requested=false;
            { std::scoped_lock lock(state->presenter_mutex); drain_requested=state->director_draining; }
            if(selected && synthetic && djules_deep_active(*state) &&
                !drain_requested) {
                bool ready=true, output_failed=false;
                for(const auto& output:selected->frame->pending_outputs) {
                    const HRESULT available=output.synthesizer->ticket_available(output.ticket);
                    if(FAILED(available)) { fail(XR_ERROR_RUNTIME_FAILURE); output_failed=true; break; }
                    ready &= available==S_OK;
                }
                if(output_failed) break;
                repeat_held=hold_djules_synthetic(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-selected->queued_at),
                    std::chrono::nanoseconds(state->presenter_display_period),ready,
                    djules_last_owned!=nullptr,state->pipelined_presenter_mode);
                if(repeat_held) {
                    synthetic=false;
                    std::scoped_lock lock(state->presenter_mutex);
                    state->presenter_inflight_frame=djules_last_owned;
                }
            }
            const XrFrameEndInfo* source = repeat_held?&djules_last_owned->info : selected ? &selected->frame->info
                : borrowed->owned_frame ? &borrowed->owned_frame->info : borrowed->borrowed_frame;
            XrFrameEndInfo end = *source;
            end.next = nullptr;
            end.displayTime = prediction.predictedDisplayTime;
            if (!prediction.shouldRender) { end.layerCount = 0; end.layers = nullptr; }
            token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_end_frame, handle_value(state->handle),
                end.displayTime, end.layerCount);
            std::chrono::nanoseconds phase_step{};
            {
                std::scoped_lock content_lock(state->presenter_content_mutex);
                result = with_runtime_entry(state, [&] {
                    if(selected && xrfg::bridge_flight_logger().ram_enabled()) {
                        const auto stage=synthetic ? 12 : 13;
                        log_luke_projection_poses(&end,selected->source_sequence,stage);
                        for(std::uint32_t layer=0;end.layers && layer<end.layerCount && layer<8;++layer) {
                            const auto* header=end.layers[layer];
                            if(!header || header->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                            const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(header);
                            for(std::uint32_t view=0;projection->views && view<projection->viewCount && view<8;++view)
                                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_inline_view,
                                    stage*100+layer*10+view,selected->source_sequence,
                                    handle_value(projection->space),handle_value(projection->views[view].subImage.swapchain));
                        }
                    }
                    auto* owned=repeat_held?djules_last_owned.get():selected?selected->frame.get():borrowed->owned_frame.get();
                    join_runtime_queue_to_application(state.get(),owned?owned->app_release_value:borrowed->app_release_value);
                    if(owned && djules_native_scheduler(*state)) {
                        for(const auto& image:owned->composition_images)
                            if(image->fence12 && FAILED(runtime_queue(*state)->Wait(image->fence12.Get(),image->ready_value)))
                                return XR_ERROR_RUNTIME_FAILURE;
                    }
                    if(owned && !handover_djules_outputs(state.get(),owned->pending_outputs)) return XR_ERROR_RUNTIME_FAILURE;
                    phase_step = sample_djules_phase_step(state, !synthetic && !repeat_held && owned,
                        selected ? selected->source_sequence : borrowed->pair_sequence);
                    return state->fps_overlay ? state->fps_overlay->end_frame(&end, synthetic,!repeat_held)
                        : state->dispatch->end_frame(state->handle, &end);
                });
            }
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::internal_end_frame,
                result, end.displayTime, end.layerCount, synthetic ? 1 : 0);
            // Selected frames are retired below; borrowed callers receive this
            // frame's error. The director keeps its existing cadence and never
            // repeats the refused frame. Only genuine session/runtime failure
            // aborts all following requests.
            if (XR_FAILED(result) && !frame_content_refused(result)) { fail(result); break; }
            if (state->fsr_two_slot_presenter && borrowed && borrowed->owned_frame) {
                // Preserve the inline contract: B's deferred copy is submitted
                // only after S has reached XR, before B gets its own slot.
                for (const auto& synthesis : borrowed->owned_frame->pending_current_copies) {
                    const HRESULT copied = synthesis ? synthesis->flush_current_copy(
                        state->d3d12_synthesis_queue ? runtime_queue(*state) : nullptr) : S_OK;
                    if (FAILED(copied)) { fail(XR_ERROR_RUNTIME_FAILURE); return; }
                }
            }
            if (state->fsr_two_slot_presenter)
                fsr_schedule.submitted(submission_slot, std::chrono::nanoseconds(state->presenter_display_period),
                    std::chrono::steady_clock::now());
            else if (djules_native_scheduler(*state) && (state->three_slot_director || state->queued_nvidia_presenter)) {
                djules_last_returned = std::chrono::steady_clock::now();
                djules_schedule.submitted(submission_slot,
                    std::chrono::nanoseconds(state->presenter_display_period), djules_last_returned,
                    XR_SUCCEEDED(result) ? phase_step : std::chrono::nanoseconds{});
            }
            else if (state->three_slot_director)
                midpoint_schedule.submitted(submission_slot,
                    std::chrono::nanoseconds(state->presenter_display_period));
            else
                // Runtime blocking already consumed wall time. Advance the
                // intended grid, not the late actual-submit time, and let
                // claim() reanchor after a complete missed interval.
                director_schedule.submitted(submission_slot,
                    std::chrono::nanoseconds(state->presenter_display_period));
            {
                std::scoped_lock lock(state->presenter_mutex);
                if (selected) {
                    if(!repeat_held) {
                        if(XR_SUCCEEDED(result) && djules_deep_active(*state))
                            djules_last_owned=selected->frame;
                        static_cast<void>(state->three_slots.release_front(state->director_draining));
                    }
                } else {
                    borrowed->result = result;
                    borrowed->completed = true;
                    if (state->outstanding_presenter_submissions)
                        --state->outstanding_presenter_submissions;
                }
                state->presenter_inflight_frame.reset();
                state->presenter_inflight_submission.reset();
                state->presenter_schedule_valid = state->presenter_display_period > 0;
            }
            if (state->three_slot_director)
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::three_slot_present,
                repeat_held ? 2 : synthetic ? 1 : 0, selected ? selected->source_sequence : 0,
                selected ? selected->previous_sequence : 0, end.displayTime);
            if (state->frame_capture && state->frame_capture->active()) {
                try {
                state->frame_capture->record("{\"kind\":\"present\",\"qpc\":"+std::to_string(xrfg::FrameCapture::qpc())+
                    ",\"source_id\":"+std::to_string(selected ? selected->source_sequence : 0)+
                    ",\"previous_id\":"+std::to_string(selected ? selected->previous_sequence : 0)+
                    ",\"synthetic\":"+(synthetic ? "true" : "false")+",\"display_time\":"+std::to_string(end.displayTime)+"}");
                } catch (...) {}
            }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_submission,
                result, selected ? selected->source_sequence : borrowed->sequence,
                end.displayTime, synthetic ? 2 : 1);
            state->presenter_condition.notify_all();
        }
    } catch (...) {
        std::scoped_lock lock(state->presenter_mutex);
        fail_pending_presenter_submissions_locked(*state, XR_ERROR_RUNTIME_FAILURE);
    }
    state->presenter_condition.notify_all();
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame,
    bool preserve_virtual_timeline) noexcept {
    try {
        if (presenter_forbidden(*state) || djules_inline_runtime(*state)) return false;
        std::scoped_lock lock(state->presenter_mutex);
        if (state->presenter_active) {
            return true;
        }
        state->presenter_adopted_frame.reset();
        if (state->runtime_frame_waited_unbegun && state->last_inline_frame_state_valid) {
            state->presenter_adopted_frame = state->last_inline_frame_state;
            state->presenter_adopted_frame->next = nullptr;
            state->runtime_frame_waited_unbegun = false;
        }
        state->presenter_submissions.clear();
        state->presenter_last_frame.reset();
        state->three_slot_director = three_slot_generation_session(state);
        state->fsr_two_slot_presenter = fsr_two_slot_session(state);
        state->queued_nvidia_presenter = queued_nvidia_session(state);
        state->three_slots.configure(djules_triple_director(*state)?3:2);
        state->synthesis_job.reset();
        state->synthesis_busy = false;
        state->director_draining = false;
        state->next_source_sequence = 1;
        state->presenter_pending_current_pair_sequence = 0;
        state->presenter_inflight_submission.reset();
        state->presenter_inflight_frame.reset();
        state->presenter_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        if (!preserve_virtual_timeline) {
            state->last_virtual_display_time = 0;
        }
        state->presenter_failure = XR_SUCCESS;
        state->next_presenter_sequence = 1;
        state->outstanding_presenter_submissions = 0;
        if (state->three_slot_director || state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
            // Seed was already presented by the inline path. Do not repeat it.
            seed_frame.reset();
        } else if (state->pipelined_presenter_mode && seed_frame) {
            auto request = std::make_shared<PresenterSubmission>();
            request->sequence = state->next_presenter_sequence++;
            request->owned_frame = std::move(seed_frame);
            state->presenter_submissions.push_back(std::move(request));
            state->outstanding_presenter_submissions = 1;
        } else {
            state->presenter_last_frame = std::move(seed_frame);
        }
        state->presenter_frame_state_valid = false;
        if ((state->three_slot_director || state->fsr_two_slot_presenter || state->queued_nvidia_presenter) && state->last_inline_frame_state_valid) {
            // Bootstrap the virtual producer without empty/repeated runtime
            // submissions. The director waits for an eligible buffer entry.
            state->presenter_frame_state = state->last_inline_frame_state;
            state->presenter_frame_state.next = nullptr;
            state->presenter_frame_state_valid = true;
        }
        // A serial from a previous presenter would either release the first
        // wait of this one straight away or never.
        state->presenter_frame_serial = 0;
        state->application_served_serial = 0;
        // A schedule left over from a previous presenter would stall the first
        // submission of this one.
        // A display time from a previous presenter says nothing about this
        // grid, and its schedule is about to be rebuilt.
        state->presenter_last_predicted_valid = false;
        state->presenter_schedule_valid = false;
        state->presenter_display_period = 0;
        state->presenter_stop_requested = false;
        state->presenter_active = true;
        if (state->three_slot_director) {
            state->synthesis_thread = std::thread(synthesis_worker_main, state);
            state->presenter_thread = std::thread(three_slot_director_main, state);
        } else if (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
            state->presenter_thread = std::thread(three_slot_director_main, state);
        } else {
            state->presenter_thread = std::thread(continuous_presenter_main, state);
        }
        return true;
    } catch (...) {
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_stop_requested = true;
        }
        state->presenter_condition.notify_all();
        if (state->synthesis_thread.joinable()) state->synthesis_thread.join();
        if (state->presenter_thread.joinable()) state->presenter_thread.join();
        state->presenter_active = false;
        return false;
    }
}

void stop_continuous_presenter(
    const std::shared_ptr<SessionState>& state) noexcept {
    if (!state) {
        return;
    }
    try {
        {
            std::unique_lock lock(state->presenter_mutex);
            if (!state->presenter_active) {
                return;
            }
            // Let a submission the application already handed over reach the
            // runtime instead of dropping it on the floor. The application no
            // longer waits for this queue to empty, so the last frame of a
            // session is normally still sitting in it. Bounded, because
            // teardown must not depend on the presenter being healthy.
            static_cast<void>(state->presenter_condition.wait_for(
                lock, std::chrono::milliseconds(100), [&] {
                    return state->outstanding_presenter_submissions == 0 ||
                           XR_FAILED(state->presenter_failure);
                }));
            state->presenter_stop_requested = true;
        }
        state->presenter_condition.notify_all();
        if (state->synthesis_thread.joinable()) state->synthesis_thread.join();
        if (state->presenter_thread.joinable()) {
            state->presenter_thread.join();
        }
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_active = false;
            state->synthesis_busy = false;
            state->synthesis_job.reset();
            state->three_slots.clear();
            state->three_slot_director = false;
            state->fsr_two_slot_presenter = false;
            state->queued_nvidia_presenter = false;
            state->presenter_frame_state_valid = false;
            state->presenter_last_frame.reset();
            state->presenter_pending_current_pair_sequence = 0;
        }
        state->presenter_condition.notify_all();
    } catch (...) {
    }
}

[[nodiscard]] bool continuous_presenter_active(
    const std::shared_ptr<SessionState>& state) noexcept {
    std::scoped_lock lock(state->presenter_mutex);
    return state->presenter_active && !state->presenter_stop_requested;
}

[[nodiscard]] bool three_slot_generation_session(
    const std::shared_ptr<SessionState>& state) noexcept {
    return !presenter_forbidden(*state) && !djules_inline_runtime(*state) && state->menu_enabled && state->frame_generation_mode !=
               xrfg::D3D12FrameGenerationMode::fsr3 &&
           state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
           state->nvidia_new_route &&
           (state->graphics_binding == SessionGraphicsBinding::d3d11 ||
            state->graphics_binding == SessionGraphicsBinding::d3d12) &&
           !state->pipelined_presenter_mode;
}

[[nodiscard]] bool fsr_two_slot_session(const std::shared_ptr<SessionState>& state) noexcept {
    return !state->luke_afw_enabled && state->dispatch->steamvr_runtime && state->menu_enabled &&
        state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3;
}

[[nodiscard]] bool queued_nvidia_session(const std::shared_ptr<SessionState>& state) noexcept {
    return !presenter_forbidden(*state) && !djules_inline_runtime(*state) && !state->luke_afw_enabled && state->menu_enabled &&
        state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr &&
        state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        !three_slot_generation_session(state) &&
        (state->graphics_binding == SessionGraphicsBinding::d3d11 ||
         state->graphics_binding == SessionGraphicsBinding::d3d12);
}

void delay_djules_application_release(const std::shared_ptr<SessionState>& state) {
    std::chrono::nanoseconds delay{};
    { std::scoped_lock lock(state->presenter_mutex);
      if(djules_triple_active(*state)) delay=state->djules_triple_release.delay; }
    if(delay>std::chrono::nanoseconds::zero()) {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait_until(lock,std::chrono::steady_clock::now()+delay,[&] {
            return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) || !state->menu_enabled;
        });
    }
}

// Holds the application until the presenter has run a whole pair since it was
// last released. Deliberately returns nothing: it is called after the frame has
// already been handed over, so a presenter that stops or fails while this waits
// must not turn a submitted frame into an error - it just stops waiting.
void wait_for_presenter_pair(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        const std::uint64_t kPresenterFramesPerPair = djules_triple_active(*state)?3:2;
        const auto entered = std::chrono::steady_clock::now();
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return state->presenter_frame_serial >=
                       state->application_served_serial +
                           kPresenterFramesPerPair ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure) ||
            state->presenter_stop_requested) {
            return;
        }
        // Diagnostic only, and the number this hold is judged by. Assigning
        // the presenter's serial rather than advancing by the pair means an
        // application that arrives late catches up in one step: the surplus
        // below is what that step discards, and while it is non-zero this
        // hold is not throttling anything - the application is gated only by
        // whatever it blocks on next, which is the frame-start synthesis
        // fence, and that one meters nothing.
        const std::uint64_t served = state->application_served_serial;
        const std::uint64_t reached = state->presenter_frame_serial;
        const std::uint64_t surplus =
            reached > served + kPresenterFramesPerPair
            ? reached - served - kPresenterFramesPerPair
            : 0;
        state->application_served_serial = reached;
        // Never record under presenter_mutex: the presenter thread takes it
        // every frame, and this path has deadlocked the layer twice.
        lock.unlock();
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_pair_release,
            static_cast<std::int64_t>(surplus),
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - entered)
                    .count()),
            reached,
            served);
    } catch (...) {
    }
}

[[nodiscard]] XrResult wait_for_presenter_capacity(
    const std::shared_ptr<SessionState>& state,
    std::size_t maximum_outstanding) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return state->outstanding_presenter_submissions <=
                       maximum_outstanding ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        return state->presenter_stop_requested
            ? XR_ERROR_SESSION_NOT_RUNNING
            : XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] XrResult wait_for_presenter_idle(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        if (state->three_slot_director) {
            state->director_draining = true;
            state->presenter_condition.notify_all();
            state->presenter_condition.wait(lock, [&] {
                return (!state->synthesis_busy && state->three_slots.empty() &&
                        !state->presenter_inflight_frame) ||
                       XR_FAILED(state->presenter_failure) || state->presenter_stop_requested;
            });
            state->director_draining = false;
        }
        state->presenter_condition.wait(lock, [&] {
            return state->outstanding_presenter_submissions == 0 ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        return state->presenter_stop_requested
            ? XR_ERROR_SESSION_NOT_RUNNING
            : XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

PresenterResourceLifetimeGuard::PresenterResourceLifetimeGuard(
    const std::shared_ptr<SessionState>& state)
    : frame_lock(state->frame_call_mutex) {
    if (continuous_presenter_active(state)) {
        // The content lock must be acquired AFTER the drain: the presenter
        // needs it to complete the very submissions we are waiting for.
        static_cast<void>(wait_for_presenter_idle(state));
        content_lock = std::unique_lock<std::mutex>(state->presenter_content_mutex);
        std::scoped_lock lock(state->presenter_mutex);
        state->presenter_last_frame.reset();
    }
}

[[nodiscard]] std::shared_ptr<PresenterSubmission>
enqueue_presenter_submission(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> owned_frame,
    const XrFrameEndInfo* borrowed_frame) {
    auto request = std::make_shared<PresenterSubmission>();
    {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_active || state->presenter_stop_requested ||
            XR_FAILED(state->presenter_failure)) {
            request->result = XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
            request->completed = true;
            return request;
        }
        request->sequence = state->next_presenter_sequence++;
        request->owned_frame = std::move(owned_frame);
        request->borrowed_frame = borrowed_frame;
        request->app_release_value = state->app_end_frame_release_value;
        state->presenter_submissions.push_back(request);
        ++state->outstanding_presenter_submissions;
    }
    state->presenter_condition.notify_all();
    return request;
}

[[nodiscard]] XrResult wait_for_presenter_submission(
    const std::shared_ptr<SessionState>& state,
    const std::shared_ptr<PresenterSubmission>& request) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return request->completed || XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        return request->completed
            ? request->result
            : XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] XrResult enqueue_presenter_pair(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> synthetic,
    std::shared_ptr<GeneratedFrameEndInfo> current,
    std::shared_ptr<GeneratedFrameEndInfo> middle=nullptr) noexcept {
    try {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_active || state->presenter_stop_requested ||
            XR_FAILED(state->presenter_failure)) {
            return XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
        }
        auto first = std::make_shared<PresenterSubmission>();
        // One in-flight/queued pair is the queue-only output budget. Admission
        // is checked before private output acquisition and never drops S/B.
        if ((state->fsr_two_slot_presenter || state->queued_nvidia_presenter) && state->outstanding_presenter_submissions != 0)
            return XR_ERROR_RUNTIME_FAILURE;
        first->sequence = state->next_presenter_sequence++;
        first->pair_sequence = first->sequence;
        first->owned_frame = std::move(synthetic);
        std::shared_ptr<PresenterSubmission> middle_request;
        if(middle) {
            middle_request=std::make_shared<PresenterSubmission>();
            middle_request->sequence=state->next_presenter_sequence++;
            middle_request->pair_sequence=first->pair_sequence;
            middle_request->owned_frame=std::move(middle);
        }
        auto second = std::make_shared<PresenterSubmission>();
        second->sequence = state->next_presenter_sequence++;
        second->pair_sequence = first->pair_sequence;
        second->owned_frame = std::move(current);
        const unsigned output_count=middle_request?3:2;
        state->presenter_submissions.push_back(std::move(first));
        if(middle_request) state->presenter_submissions.push_back(std::move(middle_request));
        state->presenter_submissions.push_back(std::move(second));
        state->outstanding_presenter_submissions += output_count;
        state->presenter_condition.notify_all();
        return XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
}

[[nodiscard]] bool capture_projection_snapshot(
    const XrFrameEndInfo* source,
    ProjectionSnapshot* output) {
    if (source == nullptr || output == nullptr || source->layerCount == 0 ||
        source->layers == nullptr) {
        return false;
    }

    ProjectionSnapshot snapshot{};
    snapshot.display_time = source->displayTime;
    snapshot.environment_blend_mode = source->environmentBlendMode;
    for (std::uint32_t layer_index = 0; layer_index < source->layerCount; ++layer_index) {
        const XrCompositionLayerBaseHeader* layer = source->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->viewCount == 0 || projection->views == nullptr ||
            projection->space == XR_NULL_HANDLE) {
            return false;
        }

        ProjectionLayerSnapshot stored{};
        stored.layer_index = layer_index;
        stored.layer_flags = projection->layerFlags;
        stored.space = projection->space;
        stored.views.assign(
            projection->views,
            projection->views + projection->viewCount);
        for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
            const XrSwapchain application_swapchain =
                projection->views[view_index].subImage.swapchain;
            if (application_swapchain == XR_NULL_HANDLE) {
                return false;
            }
            stored.views[view_index].next = nullptr;
        }
        snapshot.layers.push_back(std::move(stored));
    }
    if (snapshot.layers.empty()) {
        return false;
    }
    *output = std::move(snapshot);
    return true;
}

[[nodiscard]] bool matching_sub_image(
    const XrSwapchainSubImage& left,
    const XrSwapchainSubImage& right) noexcept {
    return left.swapchain == right.swapchain &&
           left.imageArrayIndex == right.imageArrayIndex &&
           left.imageRect.offset.x == right.imageRect.offset.x &&
           left.imageRect.offset.y == right.imageRect.offset.y &&
           left.imageRect.extent.width == right.imageRect.extent.width &&
           left.imageRect.extent.height == right.imageRect.extent.height;
}

[[nodiscard]] bool overlapping_sub_images(
    const XrSwapchainSubImage& left,
    const XrSwapchainSubImage& right) noexcept {
    if (left.imageArrayIndex != right.imageArrayIndex) {
        return false;
    }
    const std::int64_t left_right =
        static_cast<std::int64_t>(left.imageRect.offset.x) +
        left.imageRect.extent.width;
    const std::int64_t left_bottom =
        static_cast<std::int64_t>(left.imageRect.offset.y) +
        left.imageRect.extent.height;
    const std::int64_t right_right =
        static_cast<std::int64_t>(right.imageRect.offset.x) +
        right.imageRect.extent.width;
    const std::int64_t right_bottom =
        static_cast<std::int64_t>(right.imageRect.offset.y) +
        right.imageRect.extent.height;
    return left.imageRect.offset.x < right_right &&
           right.imageRect.offset.x < left_right &&
           left.imageRect.offset.y < right_bottom &&
           right.imageRect.offset.y < left_bottom;
}

[[nodiscard]] ProjectionMappingResult
build_projection_resource_mappings(const ProjectionSnapshot& snapshot) noexcept {
    ProjectionMappingResult output{};
    try {
        if (snapshot.layers.empty() || snapshot.layers.front().views.empty()) {
            output.reason = ProjectionMappingReason::no_projection_views;
            return output;
        }
        for (std::size_t projection_index = 0;
             projection_index < snapshot.layers.size();
             ++projection_index) {
            const ProjectionLayerSnapshot& layer =
                snapshot.layers[projection_index];
            for (std::size_t view_index = 0;
                 view_index < layer.views.size();
                 ++view_index) {
                const XrSwapchainSubImage& sub_image =
                    layer.views[view_index].subImage;
                const auto swapchain = find_swapchain(sub_image.swapchain);
                if (!swapchain) {
                    output.reason = ProjectionMappingReason::unknown_swapchain;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                const XrSwapchainCreateInfo& create_info =
                    swapchain->create_info;
                if (create_info.arraySize == 0 || create_info.arraySize > 2) {
                    output.reason = ProjectionMappingReason::unsupported_array_size;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                if (create_info.width == 0 || create_info.height == 0) {
                    output.reason = ProjectionMappingReason::zero_resource_extent;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                if (sub_image.imageArrayIndex >= create_info.arraySize) {
                    output.reason = ProjectionMappingReason::array_slice_out_of_range;
                    output.detail =
                        (handle_value(sub_image.swapchain) << 8) |
                        sub_image.imageArrayIndex;
                    return output;
                }
                if (sub_image.imageRect.offset.x < 0 ||
                    sub_image.imageRect.offset.y < 0) {
                    output.reason = ProjectionMappingReason::negative_subimage_offset;
                    output.detail =
                        (static_cast<std::uint64_t>(
                             static_cast<std::uint32_t>(
                                 sub_image.imageRect.offset.x)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.offset.y);
                    return output;
                }
                if (sub_image.imageRect.extent.width <= 0 ||
                    sub_image.imageRect.extent.height <= 0) {
                    output.reason =
                        ProjectionMappingReason::nonpositive_subimage_extent;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                const std::uint64_t subimage_right =
                    static_cast<std::uint64_t>(sub_image.imageRect.offset.x) +
                    static_cast<std::uint32_t>(
                        sub_image.imageRect.extent.width);
                const std::uint64_t subimage_bottom =
                    static_cast<std::uint64_t>(sub_image.imageRect.offset.y) +
                    static_cast<std::uint32_t>(
                        sub_image.imageRect.extent.height);
                if (subimage_right > create_info.width ||
                    subimage_bottom > create_info.height) {
                    output.reason = ProjectionMappingReason::subimage_out_of_bounds;
                    output.detail =
                        (static_cast<std::uint64_t>(
                             static_cast<std::uint32_t>(
                                 sub_image.imageRect.extent.width)) << 32) |
                        static_cast<std::uint32_t>(
                            sub_image.imageRect.extent.height);
                    return output;
                }

                auto mapping = std::find_if(
                    output.mappings.begin(),
                    output.mappings.end(),
                    [&](const ProjectionResourceMapping& candidate) {
                        return candidate.application_swapchain ==
                               sub_image.swapchain;
                    });
                if (mapping == output.mappings.end()) {
                    ProjectionResourceMapping created{};
                    created.application_swapchain = sub_image.swapchain;
                    output.mappings.push_back(std::move(created));
                    mapping = std::prev(output.mappings.end());
                }
                const auto existing_view = std::find_if(
                    mapping->views.begin(),
                    mapping->views.end(),
                    [&](const ProjectionViewReference& reference) {
                        return matching_sub_image(
                            snapshot.layers[reference.projection_index]
                                .views[reference.view_index]
                                .subImage,
                            sub_image);
                    });
                if (existing_view == mapping->views.end()) {
                    mapping->views.push_back({projection_index, view_index});
                }
            }
        }
        for (const ProjectionResourceMapping& mapping : output.mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain || mapping.views.empty() || mapping.views.size() > 2) {
                output.reason = ProjectionMappingReason::unsupported_view_layout;
                output.detail = handle_value(mapping.application_swapchain);
                return output;
            }
            std::array<bool, 2> covered_slices{};
            for (std::size_t view_index = 0;
                 view_index < mapping.views.size();
                 ++view_index) {
                const ProjectionViewReference& reference = mapping.views[view_index];
                const XrSwapchainSubImage& sub_image =
                    snapshot.layers[reference.projection_index]
                        .views[reference.view_index]
                        .subImage;
                covered_slices[sub_image.imageArrayIndex] = true;
                for (std::size_t preceding = 0;
                     preceding < view_index;
                     ++preceding) {
                    const ProjectionViewReference& preceding_reference =
                        mapping.views[preceding];
                    const XrSwapchainSubImage& preceding_sub_image =
                        snapshot.layers[preceding_reference.projection_index]
                            .views[preceding_reference.view_index]
                            .subImage;
                    if (overlapping_sub_images(sub_image, preceding_sub_image)) {
                        output.reason =
                            ProjectionMappingReason::unsupported_view_layout;
                        output.detail = handle_value(mapping.application_swapchain);
                        return output;
                    }
                }
            }
            for (std::uint32_t slice = 0;
                 slice < swapchain->create_info.arraySize;
                 ++slice) {
                if (!covered_slices[slice]) {
                    output.reason = ProjectionMappingReason::missing_array_slice;
                    output.detail = handle_value(mapping.application_swapchain);
                    return output;
                }
            }
        }
        const std::uint32_t logical_eye_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                2,
                std::accumulate(
                    output.mappings.begin(), output.mappings.end(),
                    std::size_t{0},
                    [](std::size_t count,
                       const ProjectionResourceMapping& mapping) {
                        return count + mapping.views.size();
                    })));
        for (const ProjectionResourceMapping& mapping : output.mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) continue;
            std::optional<std::uint32_t> logical_eye_index;
            if (logical_eye_count == 2 && mapping.views.size() == 1) {
                const ProjectionViewReference& reference = mapping.views.front();
                std::size_t ordinal = reference.view_index;
                for (std::size_t projection = 0;
                     projection < reference.projection_index;
                     ++projection) {
                    ordinal += snapshot.layers[projection].views.size();
                }
                if (ordinal < logical_eye_count) {
                    logical_eye_index = static_cast<std::uint32_t>(ordinal);
                }
            }
            std::scoped_lock lock(swapchain->mutex);
            swapchain->dlss_logical_eye_count =
                std::max<std::uint32_t>(logical_eye_count, 1U);
            swapchain->dlss_logical_eye_index = logical_eye_index;
        }
        output.reason = ProjectionMappingReason::ready;
        return output;
    } catch (...) {
        output.mappings.clear();
        output.reason = ProjectionMappingReason::exception;
        return output;
    }
}

[[nodiscard]] bool matching_projection_camera(
    const XrPosef& left_pose,
    const XrFovf& left_fov,
    const XrPosef& right_pose,
    const XrFovf& right_fov) noexcept {
    return left_pose.orientation.x == right_pose.orientation.x &&
           left_pose.orientation.y == right_pose.orientation.y &&
           left_pose.orientation.z == right_pose.orientation.z &&
           left_pose.orientation.w == right_pose.orientation.w &&
           left_pose.position.x == right_pose.position.x &&
           left_pose.position.y == right_pose.position.y &&
           left_pose.position.z == right_pose.position.z &&
           left_fov.angleLeft == right_fov.angleLeft &&
           left_fov.angleRight == right_fov.angleRight &&
           left_fov.angleUp == right_fov.angleUp &&
           left_fov.angleDown == right_fov.angleDown;
}

[[nodiscard]] std::optional<std::vector<xrfg::D3D12ReprojectionView>>
build_reprojection_views(
    const ProjectionSnapshot& snapshot,
    const ProjectionResourceMapping& mapping) {
    if (snapshot.layers.empty() || mapping.views.empty()) {
        return std::nullopt;
    }
    // The application-submitted projection pose/FOV is the authoritative
    // render-camera contract for these pixels. An application may legitimately
    // transform or replace its xrLocateViews result before submission, so the
    // raw locate sample is neither intercepted nor an eligibility condition.
    for (const ProjectionLayerSnapshot& layer : snapshot.layers) {
        for (const XrCompositionLayerProjectionView& view : layer.views) {
            if (view.subImage.swapchain != mapping.application_swapchain) {
                continue;
            }
            const auto reference = std::find_if(
                mapping.views.begin(),
                mapping.views.end(),
                [&](const ProjectionViewReference& candidate) {
                    if (candidate.projection_index >= snapshot.layers.size() ||
                        candidate.view_index >=
                            snapshot.layers[candidate.projection_index].views.size()) {
                        return false;
                    }
                    return matching_sub_image(
                        view.subImage,
                        snapshot.layers[candidate.projection_index]
                            .views[candidate.view_index]
                            .subImage);
                });
            if (reference == mapping.views.end()) {
                return std::nullopt;
            }
            if (reference->projection_index >= snapshot.layers.size() ||
                reference->view_index >=
                    snapshot.layers[reference->projection_index].views.size()) {
                return std::nullopt;
            }
            const XrCompositionLayerProjectionView& canonical =
                snapshot.layers[reference->projection_index]
                    .views[reference->view_index];
            if (!matching_projection_camera(
                    view.pose,
                    view.fov,
                    canonical.pose,
                    canonical.fov)) {
                return std::nullopt;
            }
        }
    }

    std::vector<xrfg::D3D12ReprojectionView> output(
        mapping.views.size());
    for (std::size_t view_index = 0;
         view_index < mapping.views.size();
         ++view_index) {
        const ProjectionViewReference& reference =
            mapping.views[view_index];
        if (reference.projection_index >= snapshot.layers.size() ||
            reference.view_index >=
                snapshot.layers[reference.projection_index].views.size()) {
            return std::nullopt;
        }
        const XrCompositionLayerProjectionView& source =
            snapshot.layers[reference.projection_index]
                .views[reference.view_index];
        xrfg::D3D12ReprojectionView& destination = output[view_index];
        destination.display_time = snapshot.display_time;
        destination.pose.orientation = {
            source.pose.orientation.x,
            source.pose.orientation.y,
            source.pose.orientation.z,
            source.pose.orientation.w,
        };
        destination.pose.position = {
            source.pose.position.x,
            source.pose.position.y,
            source.pose.position.z,
        };
            destination.fov = {
            source.fov.angleLeft,
            source.fov.angleRight,
            source.fov.angleUp,
            source.fov.angleDown,
        };
        destination.image_rect = {
            static_cast<std::uint32_t>(source.subImage.imageRect.offset.x),
            static_cast<std::uint32_t>(source.subImage.imageRect.offset.y),
            static_cast<std::uint32_t>(source.subImage.imageRect.extent.width),
            static_cast<std::uint32_t>(source.subImage.imageRect.extent.height),
        };
        destination.array_slice = source.subImage.imageArrayIndex;
    }
    return output;
}

[[nodiscard]] bool projection_snapshots_compatible(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
        current.display_time <= previous.display_time ||
        previous.layers.size() != current.layers.size()) {
        return false;
    }
    for (std::size_t layer_index = 0; layer_index < current.layers.size(); ++layer_index) {
        const ProjectionLayerSnapshot& left = previous.layers[layer_index];
        const ProjectionLayerSnapshot& right = current.layers[layer_index];
        if (left.layer_index != right.layer_index || left.space != right.space ||
            left.layer_flags != right.layer_flags || left.views.size() != right.views.size()) {
            return false;
        }
        for (std::size_t view_index = 0; view_index < right.views.size(); ++view_index) {
            if (!matching_sub_image(
                    left.views[view_index].subImage,
                    right.views[view_index].subImage)) {
                return false;
            }
        }
    }
    return true;
}

// A space/flag transition only invalidates the interpolation pair and can be
// re-primed on the current application frame. A layout transition changes the
// resources or regions consumed by generation and must cross the quarantine.
//
// A display time that does not advance is deliberately not tested here. It is
// not a layout change - it names no different swapchain, sub-image, view or
// blend mode - and it is not the layer's own timeline either. MSFS 2024
// derives the time it submits from the predicted time it was given, and it
// derives it non-monotonically: with the virtual clock made strictly
// increasing and verified so over 1281 consecutive waits, 287 of the 1280
// frames that application submitted still went backwards, by whole multiples
// of the display period. A pipelined application that reorders two frames in
// flight will do this, and no clock the layer hands it prevents it.
//
// Quarantining for it is also self-sustaining: the outage puts the
// application on the fail-open path, which produces the next non-advancing
// time, which starts the next outage. Captured MSFS 2024 sessions spent 3768
// of 3895 and 1176 of 1280 frames in structural quarantine that way,
// re-entering it faster than the one-second duration expires. Clearing
// continuity and priming again on this frame handles it for the cost of one
// prime.
[[nodiscard]] bool projection_resource_layout_compatible(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
        previous.layers.size() != current.layers.size()) {
        return false;
    }
    for (std::size_t layer_index = 0; layer_index < current.layers.size();
         ++layer_index) {
        const ProjectionLayerSnapshot& left = previous.layers[layer_index];
        const ProjectionLayerSnapshot& right = current.layers[layer_index];
        if (left.layer_index != right.layer_index ||
            left.views.size() != right.views.size()) {
            return false;
        }
        for (std::size_t view_index = 0; view_index < right.views.size();
             ++view_index) {
            if (!matching_sub_image(
                    left.views[view_index].subImage,
                    right.views[view_index].subImage)) {
                return false;
            }
        }
    }
    return true;
}

struct ProjectionResourceDestination {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    XrSwapchain destination_swapchain{XR_NULL_HANDLE};
};

[[nodiscard]] bool build_generated_frame_end_info(
    const XrFrameEndInfo* source,
    const ProjectionSnapshot& current_snapshot,
    bool synthetic,
    std::span<const ProjectionResourceDestination> destinations,
    GeneratedFrameEndInfo* output) {
    if (source == nullptr || output == nullptr || destinations.empty() ||
        source->layerCount == 0 || source->layers == nullptr ||
        current_snapshot.layers.empty()) {
        return false;
    }

    output->app_release_value = current_snapshot.app_release_value;
    output->info = *source;
    output->layer_pointers.assign(
        source->layers,
        source->layers + source->layerCount);
    output->projections.reserve(current_snapshot.layers.size());

    for (std::size_t projection_index = 0;
         projection_index < current_snapshot.layers.size();
         ++projection_index) {
        const ProjectionLayerSnapshot& metadata = current_snapshot.layers[projection_index];
        if (metadata.layer_index >= source->layerCount) {
            return false;
        }
        const XrCompositionLayerBaseHeader* layer = source->layers[metadata.layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            return false;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->viewCount != metadata.views.size() || projection->views == nullptr) {
            return false;
        }
        ProjectionLayerCopy copy{};
        copy.layer_index = metadata.layer_index;
        copy.layer = *projection;
        copy.views.assign(
            projection->views,
            projection->views + projection->viewCount);
        for (std::size_t view_index = 0; view_index < copy.views.size(); ++view_index) {
            XrCompositionLayerProjectionView& view = copy.views[view_index];
            const auto destination = std::find_if(
                destinations.begin(),
                destinations.end(),
                [&](const ProjectionResourceDestination& candidate) {
                    return candidate.application_swapchain ==
                           view.subImage.swapchain;
                });
            if (destination == destinations.end() ||
                destination->destination_swapchain == XR_NULL_HANDLE) {
                return false;
            }
            view.subImage.swapchain = destination->destination_swapchain;
            if (synthetic) {
                // The V008 shader generates the synthetic image in B's camera
                // reference. Its projection metadata must therefore remain
                // B's render pose/FOV.
                view.next = nullptr;
            }
        }
        output->projections.push_back(std::move(copy));
        ProjectionLayerCopy& stored = output->projections.back();
        if (synthetic) {
            stored.layer.next = nullptr;
        }
        stored.layer.views = stored.views.data();
        const auto* stored_header =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stored.layer);
        output->layer_pointers[metadata.layer_index] = stored_header;
    }
    output->info.layerCount =
        static_cast<std::uint32_t>(output->layer_pointers.size());
    output->info.layers = output->layer_pointers.data();
    return true;
}

[[nodiscard]] bool acquire_and_wait_private_image(
    SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image) noexcept {
    try {
        if (!dispatch || image.handle == XR_NULL_HANDLE ||
            dispatch->acquire_swapchain_image == nullptr ||
            dispatch->wait_swapchain_image == nullptr ||
            dispatch->release_swapchain_image == nullptr) {
            return false;
        }
        if (image.phase != PrivateOwnershipPhase::idle &&
            !release_private_image(session, dispatch, image)) {
            return false;
        }

        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        std::uint32_t acquired_index = 0;
        const auto acquire_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_acquire,
            handle_value(image.handle),
            static_cast<std::uint64_t>(image.phase));
        const XrResult acquire_result = with_runtime_entry(session, [&] {
            return dispatch->acquire_swapchain_image(
                image.handle, &acquire_info, &acquired_index);
        });
        xrfg::bridge_flight_logger().end(
            acquire_token,
            xrfg::BridgeFlightOperation::private_swapchain_acquire,
            acquire_result,
            handle_value(image.handle),
            acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (XR_FAILED(acquire_result)) {
            return false;
        }
        image.acquired_index = acquired_index;
        image.phase = PrivateOwnershipPhase::acquired;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        const auto wait_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_wait,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        const XrResult wait_result = with_runtime_entry(session, [&] {
            return dispatch->wait_swapchain_image(image.handle, &wait_info);
        });
        xrfg::bridge_flight_logger().end(
            wait_token,
            xrfg::BridgeFlightOperation::private_swapchain_wait,
            wait_result,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (wait_result != XR_SUCCESS && wait_result != XR_SESSION_LOSS_PENDING) {
            return false;
        }
        image.phase = PrivateOwnershipPhase::waited;
        if (!image.first_use_logged && session != nullptr) {
            image.first_use_logged = true;
            log_video_memory(*session, VideoMemoryStage::private_first_use, handle_value(image.handle));
        }
        return true;
    } catch (...) {
        return false;
    }
}

enum class PreparedGenerationKind {
    none,
    prime,
    pair,
};

enum class GenerationPrepareReason : std::int64_t {
    ready = 0,
    empty_mappings = 1,
    reprojection_views_failed = 2,
    unknown_swapchain = 3,
    missing_generation = 4,
    missing_capture = 5,
    invalid_private_swapchain = 6,
    retire_previous_failed = 7,
    current_private_acquire_failed = 8,
    synthetic_private_acquire_failed = 9,
    synthesis_failed = 10,
    private_release_failed = 11,
    mixed_resource_transaction = 12,
    generated_end_info_failed = 13,
    exception = 14,
    cooldown_active = 15,
    presenter_unsafe_composition = 16,
    manual_disarmed = 17,
    structural_quarantine_active = 18,
    synthesis_busy = 19,
    presenter_route_changed = 23,
    synthesis_stale_guides = 24,
};

[[nodiscard]] constexpr GenerationPrepareReason classify_synthesis_failure(
    HRESULT result) noexcept {
    if (result == xrfg::kD3D12FrameSynthesisStaleGuides) {
        return GenerationPrepareReason::synthesis_stale_guides;
    }
    return result == HRESULT_FROM_WIN32(ERROR_BUSY) ||
            result == DXGI_ERROR_WAS_STILL_DRAWING
        ? GenerationPrepareReason::synthesis_busy
        : GenerationPrepareReason::synthesis_failed;
}

static_assert(classify_synthesis_failure(DXGI_ERROR_WAS_STILL_DRAWING) ==
    GenerationPrepareReason::synthesis_busy);
static_assert(classify_synthesis_failure(
                  xrfg::kD3D12FrameSynthesisStaleGuides) ==
    GenerationPrepareReason::synthesis_stale_guides);

[[nodiscard]] constexpr bool should_preserve_generation_anchor(
    HRESULT result,
    bool current_released,
    bool synthetic_released) noexcept {
    return result == xrfg::kD3D12FrameSynthesisStaleGuides &&
        current_released && synthetic_released;
}

static_assert(should_preserve_generation_anchor(
    xrfg::kD3D12FrameSynthesisStaleGuides, true, true));
static_assert(!should_preserve_generation_anchor(
    DXGI_ERROR_WAS_STILL_DRAWING, true, true));
static_assert(!should_preserve_generation_anchor(
    xrfg::kD3D12FrameSynthesisStaleGuides, false, true));

[[nodiscard]] constexpr bool should_preserve_missing_capture_anchor(
    bool request_pair,
    xrfg::D3D12FrameGenerationMode mode,
    bool luke_combined_bridge) noexcept {
    return request_pair &&
        mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
        luke_combined_bridge;
}

static_assert(should_preserve_missing_capture_anchor(
    true, xrfg::D3D12FrameGenerationMode::fsr3, true));
static_assert(!should_preserve_missing_capture_anchor(
    false, xrfg::D3D12FrameGenerationMode::fsr3, true));
static_assert(!should_preserve_missing_capture_anchor(
    true, xrfg::D3D12FrameGenerationMode::ofxr, true));
static_assert(!should_preserve_missing_capture_anchor(
    true, xrfg::D3D12FrameGenerationMode::fsr3, false));

struct PreparedGeneration {
    std::shared_ptr<FrameGenerationSwapchainState> staging_generation;
    xrfg::D3D12FrameSynthesisTicket staging_ticket{};
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain current_handle{XR_NULL_HANDLE};
    XrSwapchain synthetic_handle{XR_NULL_HANDLE};
    // The synthesizer holding this pair's deferred current copy, so the
    // presenter can submit it once the synthetic frame has gone.
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    bool anchor_is_current{};
    std::shared_ptr<FrameGenerationSwapchainState> worker_generation;
    xrfg::D3D11PublicationReceipt worker_publication;
    std::uint32_t current_destination{};
    std::uint32_t synthetic_destination{};
    std::size_t current_slot{};
    std::size_t synthetic_slot{};
    bool preserve_previous{};
    bool publication_deferred{};
    std::uint32_t current_destination_index{
        std::numeric_limits<std::uint32_t>::max()};
};

struct PreparedProjectionResource {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    PreparedGeneration generation;
};

struct PreparedProjectionFrame {
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain failed_swapchain{XR_NULL_HANDLE};
    std::vector<PreparedProjectionResource> resources;
    bool anchor_is_current{};
    bool preserve_previous{};
    std::shared_ptr<const HeldAfwPair> held_afw_pair;
};

void attach_djules_outputs(PreparedProjectionFrame& prepared,
    GeneratedFrameEndInfo& real, GeneratedFrameEndInfo* synthetic=nullptr,
    GeneratedFrameEndInfo* extra=nullptr) {
    for(auto& resource:prepared.resources) {
        auto& output=resource.generation;
        if(!output.staging_generation) continue;
        auto generation=output.staging_generation;
        real.pending_outputs.push_back({generation,generation->current_staging[output.current_slot],
            output.synthesizer,output.staging_ticket,false});
        if(synthetic) synthetic->pending_outputs.push_back({generation,generation->synthetic_staging[output.synthetic_slot*2],
            output.synthesizer,output.staging_ticket,true});
        if(extra) extra->pending_outputs.push_back({generation,generation->synthetic_staging[output.synthetic_slot*2+1],
            output.synthesizer,output.staging_ticket,true});
    }
}

void log_luke_publication_boundary(
    std::uint64_t serial, std::int64_t boundary, std::size_t resource,
    XrSwapchain application, XrSwapchain current, XrSwapchain synthetic,
    const std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop>& interop) noexcept {
    if (!serial || !interop || !xrfg::bridge_flight_logger().ram_enabled()) return;
    const auto snapshot = interop->publication_snapshot();
    const auto code = boundary * 100 + static_cast<std::int64_t>(resource) * 10;
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::luke_publication_images, code, serial,
        handle_value(application),
        (static_cast<std::uint64_t>(snapshot.current_index) << 32) | snapshot.synthetic_index);
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::luke_publication_slots, code, serial,
        handle_value(current), handle_value(synthetic));
    const std::uint64_t values[]{snapshot.ready_value, snapshot.copy_complete_value,
                                 snapshot.completed_value};
    for (int field = 0; field < 3; ++field)
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_publication_fence, code + field,
            serial, snapshot.fence_identity, values[field]);
}

void log_luke_physical_boundary(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* frame,
    std::uint64_t serial, std::int64_t boundary, const PreparedProjectionFrame* prepared,
    XrResult result = XR_SUCCESS) noexcept {
    if (!state || !frame || !serial || (state->graphics_binding_capabilities & 8ULL) == 0 ||
        !xrfg::bridge_flight_logger().ram_enabled()) return;
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::luke_physical_submit, boundary * 100, serial,
        static_cast<std::uint64_t>(frame->displayTime),
        (static_cast<std::uint64_t>(frame->layerCount) << 32) | static_cast<std::uint32_t>(result));
    // Boundaries 4/6 are immediately before first/second physical xrEndFrame,
    // 5/7 immediately after. No additional locate or wait is performed here.
    if (boundary == 4 || boundary == 6) {
        LARGE_INTEGER counter{};
        const auto converted = state->dispatch->convert_time_to_qpc
            ? state->dispatch->convert_time_to_qpc(state->dispatch->instance, frame->displayTime, &counter)
            : XR_ERROR_FUNCTION_UNSUPPORTED;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_inline_clock, converted, serial,
            static_cast<std::uint64_t>(frame->displayTime),
            XR_SUCCEEDED(converted) ? static_cast<std::uint64_t>(counter.QuadPart) : 0);
        log_luke_projection_poses(frame, serial, boundary);
        if (frame->layers) {
            for (std::uint32_t layer = 0; layer < std::min(frame->layerCount, 8u); ++layer) {
                const auto* header = frame->layers[layer];
                if (!header || header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
                if (!projection->views) continue;
                for (std::uint32_t view = 0; view < std::min(projection->viewCount, 8u); ++view) {
                    const auto code = boundary * 100 + layer * 10 + view;
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_inline_view, code, serial,
                        handle_value(projection->space), handle_value(projection->views[view].subImage.swapchain));
                    log_luke_view_bits(serial, code, projection->views[view]);
                }
            }
        }
    }
    if (prepared) {
        for (std::size_t index = 0; index < prepared->resources.size(); ++index) {
            const auto& resource = prepared->resources[index];
            const auto swapchain = find_swapchain(resource.application_swapchain);
            if (!swapchain) continue;
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (generation)
                log_luke_publication_boundary(serial, boundary, index,
                    resource.application_swapchain, resource.generation.current_handle,
                    resource.generation.synthetic_handle, generation->d3d11_interop);
        }
    }
}

struct AfwSourceCapture {
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    xrfg::D3D12HistoryCaptureTicket capture;
};

[[nodiscard]] PreparedGeneration prepare_frame_generation(
    XrSwapchain application_swapchain,
    bool request_pair,
    std::span<const xrfg::D3D12ReprojectionView> current_source_views,
    float interpolation_fraction,
    bool worker_submission = false,
    const xrfg::D3D12PresentationTransform* captured_transform = nullptr,
    bool defer_d3d11_publication = false,
    const PostAfwFrameInput* post_afw_input = nullptr,
    const AfwSourceCapture* afw_source = nullptr,
    float extra_fraction=2.0F/3.0F) noexcept {
    PreparedGeneration output{};
    try {
        const auto state = find_swapchain(application_swapchain);
        if (!state) {
            output.reason = GenerationPrepareReason::unknown_swapchain;
            return output;
        }

        std::scoped_lock call_lock(state->call_mutex);
        const auto presentation_transform = captured_transform ? *captured_transform
            : state->session->presentation_transform;
        if (!worker_submission && ((!state->presentation_transform_known && presentation_transform.active) ||
            (state->presentation_transform_known && state->presentation_transform_active != presentation_transform.active)))
            request_pair = false;
        state->presentation_transform_known = true;
        state->presentation_transform_active = presentation_transform.active;
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        std::optional<xrfg::D3D12HistoryCaptureTicket> capture;
        std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_vectors;
        {
            std::scoped_lock lock(state->mutex);
            generation = state->frame_generation;
            if (afw_source) {
                // The producer selected its color BEFORE private-image/GPU
                // waits, alongside the immutable guide. Never reselect latest
                // release here after those waits have changed the phase.
                if (generation != afw_source->generation || post_afw_input) {
                    output.reason = GenerationPrepareReason::missing_generation;
                    return output;
                }
                capture = afw_source->capture;
            } else {
                capture = state->last_released_capture;
                motion_vectors = state->last_released_motion_vectors;
            }
        }
        const auto synthesizer = post_afw_input && generation && generation == post_afw_input->generation
            ? generation->afw_temporal_synthesizer
            : generation ? generation->synthesizer : nullptr;
        if (post_afw_input) {
            if (generation != post_afw_input->generation || !synthesizer) {
                output.reason = GenerationPrepareReason::missing_generation;
                return output;
            }
            capture = post_afw_input->capture;
            motion_vectors.reset(); // post-AFW OFA consumes owned final color, not AER guides
        }
        if (!generation || !synthesizer) {
            output.reason = GenerationPrepareReason::missing_generation;
            return output;
        }
        if (!capture) {
            output.reason = GenerationPrepareReason::missing_capture;
            // Luke's D3D11-to-D3D12 bridge can present an application frame
            // between two actual post-ReShade captures.  That frame has
            // nothing new to synthesize, but it must not discard the last
            // valid FSR projection anchor: the following fresh capture and
            // fresh guide pair still belong to that continuity chain.  Do not
            // extend this contract to NVIDIA or non-Luke sessions, and do not
            // cache or reuse guide resources here.
            output.preserve_previous = should_preserve_missing_capture_anchor(
                request_pair,
                state->session->frame_generation_mode,
                (state->session->graphics_binding_capabilities & 8ULL) != 0
            );
            return output;
        }
        // AFW scratch is disjoint from both ordinary director output handles.
        const std::size_t current_slot = defer_d3d11_publication
            ? kAfwScratchSlot : generation->current_slot;
        const std::size_t synthetic_slot = defer_d3d11_publication
            ? 0 : generation->synthetic_slot;
        PrivateSwapchainState& current_image =
            generation->current[generation->staged_outputs ? 0 : current_slot];
        PrivateSwapchainState& synthetic_image =
            generation->synthetic[generation->staged_outputs ? 0 : synthetic_slot];
        if (current_image.handle == XR_NULL_HANDLE ||
            synthetic_image.handle == XR_NULL_HANDLE ||
            generation->current_images_per_slot == 0 ||
            generation->synthetic_images_per_slot == 0) {
            output.reason = GenerationPrepareReason::invalid_private_swapchain;
            return output;
        }

        if (!request_pair) {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            const HRESULT retire_result = synthesizer->retire_previous();
            if (FAILED(retire_result)) {
                output.reason = GenerationPrepareReason::retire_previous_failed;
                return output;
            }
        }

        const bool request_extra=request_pair && generation->staged_outputs &&
            djules_triple_active(*state->session);
        if (generation->staged_outputs) {
            std::unique_lock lock(state->session->presenter_mutex);
            state->session->presenter_condition.wait(lock,[&] {
                return (generation->current_staging[current_slot].use_count()==1 &&
                    (!request_pair || generation->synthetic_staging[synthetic_slot*2].use_count()==1) &&
                    (!request_extra || generation->synthetic_staging[synthetic_slot*2+1].use_count()==1)) ||
                    state->session->presenter_stop_requested || XR_FAILED(state->session->presenter_failure);
            });
            if(state->session->presenter_stop_requested || XR_FAILED(state->session->presenter_failure)) {
                output.reason=GenerationPrepareReason::synthesis_busy; return output;
            }
            lock.unlock();
            auto* queue=state->session->d3d12_synthesis_queue ? state->session->d3d12_synthesis_queue.Get()
                : state->session->d3d12_queue.Get();
            for(const auto& stage:{generation->current_staging[current_slot],
                    request_pair ? generation->synthetic_staging[synthetic_slot*2] : nullptr,
                    request_extra ? generation->synthetic_staging[synthetic_slot*2+1] : nullptr}) {
                if(stage && FAILED(queue->Wait(generation->handover_fence.Get(),
                        stage->last_handover_copy.load(std::memory_order_acquire)))) {
                    output.reason=GenerationPrepareReason::synthesis_failed; return output;
                }
            }
        }
        if (!generation->staged_outputs && !acquire_and_wait_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image)) {
            output.reason = GenerationPrepareReason::current_private_acquire_failed;
            if (request_pair) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(synthesizer->retire_previous());
            }
            return output;
        }

        if (request_pair && !generation->staged_outputs &&
            !acquire_and_wait_private_image(
                state->session.get(),
                state->session->dispatch,
                synthetic_image)) {
            output.reason =
                GenerationPrepareReason::synthetic_private_acquire_failed;
            static_cast<void>(release_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image));
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(synthesizer->retire_previous());
            return output;
        }

        // Both private images are acquired. The runtime made them safe to
        // write on the queue the application supplied, which is not the one
        // about to write them, so carry that guarantee across before any
        // synthesis is queued.
        if (!generation->staged_outputs && state->session->d3d12_synthesis_queue) {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(
                synthesizer->synchronize_producer_queue(
                    runtime_queue(*state->session)));
        }

        const std::uint32_t current_destination_index =
            static_cast<std::uint32_t>(current_slot) *
                generation->current_images_per_slot +
            (generation->staged_outputs ? 0 : current_image.acquired_index);
        const std::uint32_t synthetic_destination_index =
            static_cast<std::uint32_t>(synthetic_slot) *
                generation->synthetic_images_per_slot +
            (generation->staged_outputs ? 0 : synthetic_image.acquired_index);

        xrfg::D3D12FrameSynthesisTicket ticket{};
        const auto debug_marker = request_pair && state->session->fsr_cadence.marker_enabled() &&
                state->session->fps_overlay
            ? state->session->fps_overlay->marker_placement()
            : std::nullopt;
        HRESULT submit_result = E_UNEXPECTED;
        xrfg::D3D11PublicationReceipt inline_publication;
        const xrfg::BridgeFlightOperation synthesis_operation = request_pair
            ? xrfg::BridgeFlightOperation::synthesis_pair
            : xrfg::BridgeFlightOperation::synthesis_prime;
        const auto synthesis_token = xrfg::bridge_flight_logger().begin(
            synthesis_operation,
            handle_value(application_swapchain),
            capture->serial,
            (static_cast<std::uint64_t>(
                 synthetic_destination_index) << 32) |
                current_destination_index);
        if (state->session->frame_capture && state->session->frame_capture->active() && state->d3d12_history) {
            try {
            state->session->frame_capture->record("{\"kind\":\"consume\",\"qpc\":"+std::to_string(xrfg::FrameCapture::qpc())+
                ",\"swapchain\":\""+std::to_string(handle_value(application_swapchain))+"\",\"serial\":"+std::to_string(capture->serial)+
                ",\"history_fence_required\":"+std::to_string(capture->fence_value)+",\"history_fence_observed\":"+
                std::to_string(state->d3d12_history->diagnostic_completed_value())+",\"gpu_wait_required_by_pipeline\":true}");
            } catch (...) {}
        }
        {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            log_completed_nvidia_gpu_timings(synthesizer);
            if (generation->d3d11_interop) {
                submit_result =
                    generation->d3d11_interop->prepare_synthesis();
            }
            // Deferring the current copy keeps a full-resolution copy the
            // synthetic never reads off its critical path, but it only works
            // where the copy's destination is read after flush_current_copy.
            // The interop's publish below is read before it: it moves both
            // results back across to the application's D3D11 images while the
            // copy is still an unsubmitted command list, so it publishes the
            // previous pair's B as this pair's current frame. The headset then
            // runs forward to the midpoint and back a whole pair, every pair,
            // which reads as doubling that scales with motion and disappears
            // wherever the scene is still.
            // The legacy queue can hand S to the runtime while the producer
            // advances. Submit B now as well, so its queue dependency precedes
            // that next application frame rather than being inserted after S.
            const bool defer_current_copy = !generation->staged_outputs && !worker_submission &&
                generation->d3d11_interop == nullptr &&
                !state->session->queued_nvidia_presenter;
            if (!generation->d3d11_interop || SUCCEEDED(submit_result)) {
                submit_result = request_pair ? synthesizer->submit_pair(
                                          *capture,
                                          current_source_views,
                                          current_source_views,
                                          synthetic_destination_index,
                                          current_destination_index,
                                          &ticket,
                                          debug_marker,
                                          motion_vectors,
                                          defer_current_copy,
                                          interpolation_fraction, presentation_transform,
                                          request_extra ? std::optional<xrfg::D3D12ExtraSynthetic>{
                                              xrfg::D3D12ExtraSynthetic{synthetic_destination_index+1,extra_fraction}} : std::nullopt)
                                    : synthesizer->submit_prime(
                                          *capture,
                                          current_source_views,
                                          current_destination_index,
                                          &ticket,
                                          motion_vectors, presentation_transform);
                if (request_pair && post_afw_input) {
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                        submit_result, 101, post_afw_input->color_identity.application_serial,
                        ticket.current_serial);
                }
            }
            if (SUCCEEDED(submit_result) && generation->d3d11_interop &&
                !worker_submission && !defer_d3d11_publication) {
                const auto publish_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? synthetic_destination_index
                                 : std::numeric_limits<std::uint32_t>::max());
                const HRESULT publish_result =
                    generation->d3d11_interop->publish(
                        current_destination_index,
                        request_pair
                            ? std::optional<std::uint32_t>(
                                  synthetic_destination_index)
                            : std::nullopt,
                        post_afw_input ? &inline_publication : nullptr);
                xrfg::bridge_flight_logger().end(
                    publish_token,
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    publish_result,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? synthetic_destination_index
                                 : std::numeric_limits<std::uint32_t>::max());
                if (FAILED(publish_result)) {
                    static_cast<void>(synthesizer->retire_previous());
                    submit_result = publish_result;
                }
            }
        }
        xrfg::bridge_flight_logger().end(
            synthesis_token,
            synthesis_operation,
            submit_result,
            ticket.previous_serial,
            ticket.current_serial,
            ticket.fence_value);

        // A successful publish only queues D3D12->D3D11 copies. The legacy
        // post-AFW consumer must not release S/B to XR until THIS receipt is
        // complete, just as the director's worker does. Wait outside gpu_mutex
        // and the interop mutex; never follow the mutable next-frame cursor.
        if (SUCCEEDED(submit_result) && inline_publication.fence) {
            const auto publication_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_publication_complete,
                post_afw_input->color_identity.application_serial,
                handle_value(application_swapchain),
                inline_publication.copy_complete_value);
            const HRESULT published = xrfg::wait_for_d3d11_publication(inline_publication);
            xrfg::bridge_flight_logger().end(publication_token,
                xrfg::BridgeFlightOperation::timing_publication_complete, published,
                post_afw_input->color_identity.application_serial,
                handle_value(application_swapchain),
                inline_publication.copy_complete_value);
            if (FAILED(published)) {
                // Retain acquired images on failure, never expose unfinished
                // output to XR. Normal session retirement/teardown drains it.
                output.reason = GenerationPrepareReason::synthesis_failed;
                return output;
            }
        }

        // The runtime orders its use of these swapchain images against the
        // queue the application supplied, so when synthesis ran elsewhere
        // that queue has to wait for it before the release below hands the
        // images over. GPU-side, so it costs the application thread nothing.
        if (!worker_submission && state->session->d3d12_synthesis_queue && SUCCEEDED(submit_result)) {
            static_cast<void>(
                synthesizer->synchronize_consumer_queue(
                    runtime_queue(*state->session),
                    ticket));
        }

        if (generation->staged_outputs && SUCCEEDED(submit_result)) {
            output.staging_generation=generation; output.staging_ticket=ticket;
            output.synthesizer=synthesizer;
            output.current_slot=current_slot; output.synthetic_slot=synthetic_slot;
        }
        if (worker_submission && SUCCEEDED(submit_result)) {
            // Submit both eyes before waiting. No unfinished synthesis fence
            // is placed on the application's D3D11 context here.
            output.worker_generation = generation;
            output.current_destination = current_destination_index;
            output.synthetic_destination = synthetic_destination_index;
            output.current_slot = current_slot;
            output.synthetic_slot = synthetic_slot;
            output.current_handle = current_image.handle;
            output.synthetic_handle = synthetic_image.handle;
            output.synthesizer = synthesizer;
            output.anchor_is_current = true;
            output.kind = request_pair ? PreparedGenerationKind::pair
                                       : PreparedGenerationKind::prime;
            output.reason = GenerationPrepareReason::ready;
            generation->current_slot = (current_slot + 1) % kCurrentSlotCount;
            if (request_pair)
                generation->synthetic_slot = (synthetic_slot + 1) % kSyntheticSlotCount;
            return output;
        }
        bool synthetic_released = true;
        if (request_pair && !generation->staged_outputs) {
            synthetic_released = release_private_image(
                state->session.get(),
                state->session->dispatch,
                synthetic_image);
        }
        const bool current_released = generation->staged_outputs || (defer_d3d11_publication && SUCCEEDED(submit_result))
            ? true
            : release_private_image(state->session.get(),
                state->session->dispatch, current_image);

        if (FAILED(submit_result)) {
            // A busy bridge slot or an FSR context waiting for its previous GPU
            // users is ordinary backpressure. A repeated AFW guide is different:
            // pass this application frame through while retaining the last real
            // anchor, so the next advanced guide can still form an FSR pair.
            const bool stale_guides = request_pair &&
                should_preserve_generation_anchor(
                    submit_result, current_released, synthetic_released);
            output.reason = stale_guides
                ? GenerationPrepareReason::synthesis_stale_guides
                : classify_synthesis_failure(submit_result);
            output.preserve_previous = stale_guides;
            if (request_pair && !stale_guides) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(synthesizer->retire_previous());
            }
            return output;
        }

        output.anchor_is_current = true;
        output.current_handle = current_image.handle;
        output.synthetic_handle = synthetic_image.handle;
        output.publication_deferred = defer_d3d11_publication;
        output.current_destination_index = current_destination_index;
        if (current_released && synthetic_released) {
            // Hand the next frame the other slot, so it can release its output
            // while this one is still queued behind the presenter.
            if (!defer_d3d11_publication)
                generation->current_slot =
                    (current_slot + 1) % kCurrentSlotCount;
            // Only a pair defers its current copy; a prime submits it inline.
            if (request_pair) {
                generation->synthetic_slot =
                    (synthetic_slot + 1) % kSyntheticSlotCount;
                output.synthesizer = synthesizer;
            }
            output.kind = request_pair ? PreparedGenerationKind::pair
                                       : PreparedGenerationKind::prime;
            output.reason = GenerationPrepareReason::ready;
        } else {
            output.reason = GenerationPrepareReason::private_release_failed;
        }

        return output;
    } catch (...) {
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}

[[nodiscard]] float synthetic_interpolation_fraction(
    const std::shared_ptr<SessionState>& state,
    const std::optional<ProjectionSnapshot>& previous_snapshot,
    const ProjectionSnapshot& current_snapshot, unsigned index=0) noexcept {
    const unsigned frames=state && djules_triple_active(*state)?3:2;
    const float kFixedMidpoint=float(index+1)/float(frames);
    // Match the validated new NVIDIA image contract. Predicted XR-time gaps
    // must not move legacy's synthesized content toward B from pair to pair.
    // Other backends retain their independently tested contracts.
    if (state && state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3 &&
        state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        !djules_native_scheduler(*state))
        return kFixedMidpoint;
    if (!state || !previous_snapshot) {
        return kFixedMidpoint;
    }
    XrDuration period = 0;
    {
        std::scoped_lock lock(state->mutex);
        period = state->minimum_runtime_display_period;
    }
    const XrTime interval =
        current_snapshot.display_time - previous_snapshot->display_time;
    // One period or less cannot hold a synthetic at all, and an interval wider
    // than four says the pairing has already lost its cadence.
    if (period <= 0 || interval <= period * (frames-1) || interval > period * frames * 2) {
        return kFixedMidpoint;
    }
    return 1.0F - static_cast<float>(period * (frames-1-index)) / static_cast<float>(interval);
}

[[nodiscard]] PreparedProjectionFrame prepare_projection_frame(
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    bool request_pair,
    float interpolation_fraction,
    bool worker_submission = false,
    const xrfg::D3D12PresentationTransform* captured_transform = nullptr,
    std::span<const PostAfwFrameInput> post_afw_inputs = {},
    float extra_fraction=2.0F/3.0F) noexcept {
    PreparedProjectionFrame output{};
    try {
        if (mappings.empty()) {
            output.reason = GenerationPrepareReason::empty_mappings;
            return output;
        }
        const PreparedGenerationKind expected_kind = request_pair
            ? PreparedGenerationKind::pair
            : PreparedGenerationKind::prime;
        bool all_expected_kind = true;
        bool all_anchor_current = true;
        bool all_preserve_previous = request_pair;
        output.resources.reserve(mappings.size());
        for (const ProjectionResourceMapping& mapping : mappings) {
            const PostAfwFrameInput* post_afw_input = nullptr;
            if (!post_afw_inputs.empty()) {
                const auto found = std::find_if(post_afw_inputs.begin(), post_afw_inputs.end(),
                    [&](const auto& input) { return input.application_swapchain == mapping.application_swapchain; });
                if (found == post_afw_inputs.end()) {
                    output.reason = GenerationPrepareReason::missing_capture;
                    return output;
                }
                post_afw_input = &*found;
            }
            const auto reprojection_views = post_afw_input
                ? std::optional<std::vector<xrfg::D3D12ReprojectionView>>{post_afw_input->views}
                : build_reprojection_views(snapshot, mapping);
            if (!reprojection_views) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                return output;
            }
            PreparedGeneration generation = prepare_frame_generation(
                mapping.application_swapchain,
                request_pair,
                std::span<const xrfg::D3D12ReprojectionView>(
                    reprojection_views->data(),
                    reprojection_views->size()),
                interpolation_fraction, worker_submission, captured_transform, false, post_afw_input, nullptr, extra_fraction);
            all_expected_kind =
                all_expected_kind && generation.kind == expected_kind;
            all_anchor_current =
                all_anchor_current && generation.anchor_is_current;
            all_preserve_previous =
                all_preserve_previous && generation.preserve_previous;
            if (generation.reason != GenerationPrepareReason::ready &&
                output.failed_swapchain == XR_NULL_HANDLE) {
                output.reason = generation.reason;
                output.failed_swapchain = mapping.application_swapchain;
            }
            output.resources.push_back({
                mapping.application_swapchain,
                generation,
            });
        }
        output.kind = all_expected_kind
            ? expected_kind
            : PreparedGenerationKind::none;
        output.anchor_is_current = all_anchor_current;
        output.preserve_previous = all_preserve_previous;
        if (all_expected_kind) {
            output.reason = GenerationPrepareReason::ready;
        } else if (output.failed_swapchain == XR_NULL_HANDLE) {
            output.reason = GenerationPrepareReason::mixed_resource_transaction;
        }
        return output;
    } catch (...) {
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}

// Complete a stereo transaction on thread 2. No session GPU/call lock is
// held across a GPU fence wait; the producer may render its next frame.
[[nodiscard]] bool finish_worker_outputs(
    const std::shared_ptr<SessionState>& state, PreparedProjectionFrame& prepared,
    std::uint64_t source_sequence) noexcept {
    bool success = prepared.kind != PreparedGenerationKind::none;
    for (auto& resource : prepared.resources) {
        auto& g = resource.generation;
        if (!g.worker_generation) continue;
        if(g.staging_generation && (djules_deep_active(*state)||djules_triple_active(*state))) continue;
        const auto completion_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::timing_synthesis_complete,
            source_sequence, handle_value(resource.application_swapchain));
        const HRESULT ready = g.synthesizer->wait_for_previous_submission(
            kFrameStartSynthesisWaitMilliseconds);
        xrfg::bridge_flight_logger().end(completion_token,
            xrfg::BridgeFlightOperation::timing_synthesis_complete, ready,
            source_sequence, handle_value(resource.application_swapchain));
        // On failure keep the images acquired; session teardown drains GPU
        // work before releasing them. Never return unfinished images to XR.
        if (FAILED(ready)) return false;
    }
    for (auto& resource : prepared.resources) {
        auto& g = resource.generation;
        if (!g.worker_generation) continue;
        if (success && g.worker_generation->d3d11_interop) {
            const auto token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::d3d11_publish, handle_value(resource.application_swapchain));
            const HRESULT result = g.worker_generation->d3d11_interop->publish(
                g.current_destination, g.kind == PreparedGenerationKind::pair
                    ? std::optional<std::uint32_t>{g.synthetic_destination} : std::nullopt,
                &g.worker_publication);
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::d3d11_publish,
                result, g.current_destination, g.synthetic_destination);
            success = SUCCEEDED(result) && success;
        }
    }
    for (auto& resource : prepared.resources) {
        auto& g = resource.generation;
        if (!g.worker_generation) continue;
        // Wait only for this publication, outside the interop mutex. A global
        // idle wait blocks AFW capture/publish on that same bridge and can
        // also inherit the next producer's completion cursor.
        if (g.worker_generation->d3d11_interop) {
            const auto publication_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_publication_complete,
                source_sequence, handle_value(resource.application_swapchain),
                g.worker_publication.copy_complete_value);
            const HRESULT published = xrfg::wait_for_d3d11_publication(g.worker_publication);
            xrfg::bridge_flight_logger().end(publication_token,
                xrfg::BridgeFlightOperation::timing_publication_complete, published,
                source_sequence, handle_value(resource.application_swapchain),
                g.worker_publication.copy_complete_value);
            success = SUCCEEDED(published) && success;
        }
        if (g.staging_generation) continue;
        if (g.kind == PreparedGenerationKind::pair)
            success = release_private_image(state.get(), state->dispatch,
                g.worker_generation->synthetic[g.synthetic_slot]) && success;
        success = release_private_image(state.get(), state->dispatch,
            g.worker_generation->current[g.current_slot]) && success;
    }
    return success;
}

void synthesis_worker_main(const std::shared_ptr<SessionState>& state) noexcept {
    try {
        for (;;) {
            std::shared_ptr<SynthesisJob> job;
            {
                std::unique_lock lock(state->presenter_mutex);
                state->presenter_condition.wait(lock, [&] {
                    return state->synthesis_job || state->presenter_stop_requested ||
                           XR_FAILED(state->presenter_failure);
                });
                if (state->presenter_stop_requested || XR_FAILED(state->presenter_failure)) break;
                job = std::move(state->synthesis_job);
            }
            const auto worker_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_worker_run,
                job->current->source_sequence, job->current->previous_sequence);
            const auto prepare_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_worker_prepare,
                job->current->source_sequence, job->current->previous_sequence);
            PreparedProjectionFrame prepared = prepare_projection_frame(
                job->snapshot, job->mappings, job->pair, job->fraction, true,
                &job->presentation_transform, job->post_afw_frame
                    ? std::span<const PostAfwFrameInput>{job->post_afw_frame->inputs}
                    : std::span<const PostAfwFrameInput>{},job->extra_fraction);
            xrfg::bridge_flight_logger().end(prepare_token,
                xrfg::BridgeFlightOperation::timing_worker_prepare,
                prepared.kind == PreparedGenerationKind::none ? -1 : 0,
                job->current->source_sequence, job->current->previous_sequence,
                static_cast<std::uint64_t>(prepared.kind));
            bool success = finish_worker_outputs(state, prepared, job->current->source_sequence);
            const auto auxiliary_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_aux_complete,
                job->current->source_sequence, job->current->previous_sequence);
            if (success && job->metadata &&
                !(djules_deep_active(*state)||djules_triple_active(*state))) {
                for (const auto& image : job->metadata->composition_images) {
                    success = SUCCEEDED(wait_composition_fence(image->fence.Get(), image->ready_value)) && success;
                    success = SUCCEEDED(wait_composition_fence(image->fence12.Get(), image->ready_value)) && success;
                }
            }
            std::shared_ptr<GeneratedFrameEndInfo> real;
            std::shared_ptr<GeneratedFrameEndInfo> synthetic, extra_synthetic;
            xrfg::bridge_flight_logger().end(auxiliary_token,
                xrfg::BridgeFlightOperation::timing_aux_complete, success ? 0 : -1,
                job->current->source_sequence, job->current->previous_sequence);
            const auto build_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_worker_build,
                job->current->source_sequence, job->current->previous_sequence);
            if (success) {
                std::vector<ProjectionResourceDestination> real_destinations, synthetic_destinations;
                for (const auto& resource : prepared.resources) {
                    XrSwapchain source = resource.application_swapchain;
                    if (job->post_afw_frame) {
                        const auto& inputs = job->post_afw_frame->inputs;
                        const auto found = std::find_if(inputs.begin(), inputs.end(),
                            [&](const auto& input) { return input.application_swapchain == source; });
                        if (found == inputs.end()) { success = false; break; }
                        source = found->completed_swapchain;
                    }
                    real_destinations.push_back({source,
                                                resource.generation.current_handle});
                    synthetic_destinations.push_back({source,
                                                     resource.generation.synthetic_handle});
                }
                GeneratedFrameEndInfo real_info, synthetic_info;
                success = success && build_generated_frame_end_info(&job->metadata->info, job->snapshot,
                    false, real_destinations, &real_info);
                GeneratedFrameEndInfo pending_synthetic,pending_extra;
                attach_djules_outputs(prepared,real_info,job->pair?&pending_synthetic:nullptr,
                    job->pair&&job->triple?&pending_extra:nullptr);
                real_info.composition_images = job->metadata->composition_images;
                if (success) real = make_presenter_owned_frame(std::move(real_info));
                success = success && real != nullptr;
                if (success && job->pair) {
                    success = build_generated_frame_end_info(&job->metadata->info, job->snapshot,
                        true, synthetic_destinations, &synthetic_info);
                    synthetic_info.pending_outputs=std::move(pending_synthetic.pending_outputs);
                    synthetic_info.synthetic = true;
                    synthetic_info.composition_images = job->metadata->composition_images;
                    synthetic_info.djules_leader=job->triple;
                    if (success) synthetic = make_presenter_owned_frame(std::move(synthetic_info));
                    success = success && synthetic != nullptr;
                    if(success && job->triple) {
                        GeneratedFrameEndInfo extra_info;
                        success=build_generated_frame_end_info(&job->metadata->info,job->snapshot,true,
                            synthetic_destinations,&extra_info);
                        extra_info.pending_outputs=std::move(pending_extra.pending_outputs);
                        extra_info.synthetic=true;
                        extra_info.composition_images=job->metadata->composition_images;
                        if(success) extra_synthetic=make_presenter_owned_frame(std::move(extra_info));
                        success=success && extra_synthetic!=nullptr;
                    }
                }
            }
            xrfg::bridge_flight_logger().end(build_token,
                xrfg::BridgeFlightOperation::timing_worker_build, success ? 0 : -1,
                job->current->source_sequence, job->current->previous_sequence);
            xrfg::bridge_flight_logger().end(worker_token,
                xrfg::BridgeFlightOperation::timing_worker_run, success ? 0 : -1,
                job->current->source_sequence, job->current->previous_sequence);
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::generation_prepare,
                success ? 0 : static_cast<std::int64_t>(prepared.reason),
                handle_value(prepared.failed_swapchain), static_cast<std::uint64_t>(prepared.kind),
                job->current->source_sequence);
            if (!success) {
                std::scoped_lock lock(state->presenter_mutex);
                state->synthesis_busy = false;
                fail_pending_presenter_submissions_locked(*state, XR_ERROR_RUNTIME_FAILURE);
                state->presenter_condition.notify_all();
                break;
            }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::three_slot_gpu_ready,
                (djules_deep_active(*state)||djules_triple_active(*state)) ? 2 : job->pair ? 1 : 0,
                job->current->source_sequence, job->current->previous_sequence);
            {
                std::unique_lock lock(state->presenter_mutex);
                job->current->frame = std::move(real);
                if (job->pair) {
                    // A may still be inside downstream EndFrame. Never replace
                    // its descriptor until the director has acknowledged it.
                    const auto slot_token = xrfg::bridge_flight_logger().begin(
                        xrfg::BridgeFlightOperation::timing_slot_wait,
                        job->current->source_sequence, job->current->previous_sequence);
                    state->presenter_condition.wait(lock, [&] {
                        return state->three_slots.can_insert_synthetic() ||
                            state->presenter_stop_requested || XR_FAILED(state->presenter_failure);
                    });
                    if (state->presenter_stop_requested || XR_FAILED(state->presenter_failure)) break;
                    xrfg::bridge_flight_logger().end(slot_token,
                        xrfg::BridgeFlightOperation::timing_slot_wait, 0,
                        job->current->source_sequence, job->current->previous_sequence);
                    auto midpoint = std::make_shared<ThreeSlotFrame>();
                    midpoint->queued_at=job->current->queued_at;
                    midpoint->source_sequence = job->current->source_sequence;
                    midpoint->previous_sequence = job->current->previous_sequence;
                    midpoint->frame = std::move(synthetic);
                    if(job->triple) {
                        auto extra=std::make_shared<ThreeSlotFrame>();
                        extra->queued_at=job->current->queued_at; extra->source_sequence=midpoint->source_sequence;
                        extra->previous_sequence=midpoint->previous_sequence; extra->frame=std::move(extra_synthetic);
                        static_cast<void>(state->three_slots.push_synthetic_pair(std::move(midpoint),std::move(extra)));
                    } else static_cast<void>(state->three_slots.push_synthetic(std::move(midpoint)));
                    state->generation_steady_state_established = true;
                } else {
                    // Two O(1) index changes, no empty OpenXR submissions and
                    // no image copies. A is now held in logical slot 1.
                    for(std::size_t i=1;i<state->three_slots.capacity();++i)
                        static_cast<void>(state->three_slots.shift_empty());
                }
                state->synthesis_busy = false;
            }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::timing_worker_publish,
                0, job->current->source_sequence, job->current->previous_sequence);
            state->presenter_condition.notify_all();
        }
    } catch (...) {
        std::scoped_lock lock(state->presenter_mutex);
        state->synthesis_busy = false;
        fail_pending_presenter_submissions_locked(*state, XR_ERROR_RUNTIME_FAILURE);
    }
    state->presenter_condition.notify_all();
}

// Only texture-backed layers can be retained without additional extension-
// specific lifetime hooks. Preserve order, eye visibility, geometry and alpha.
[[nodiscard]] XrSwapchain* composition_swapchain(OwnedCompositionLayer& layer) {
    return std::visit([](auto& value) -> XrSwapchain* {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, XrCompositionLayerCubeKHR>) return &value.swapchain;
        else if constexpr (std::is_same_v<T, XrCompositionLayerQuad> ||
                           std::is_same_v<T, XrCompositionLayerCylinderKHR> ||
                           std::is_same_v<T, XrCompositionLayerEquirectKHR> ||
                           std::is_same_v<T, XrCompositionLayerEquirect2KHR>) return &value.subImage.swapchain;
        else return nullptr;
    }, layer);
}

[[nodiscard]] bool composition_can_be_owned(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* source) {
    if (!source || source->next || (source->layerCount && !source->layers)) return false;
    bool supported = true;
    for (std::uint32_t i = 0; i < source->layerCount; ++i) {
        const auto* header = source->layers[i];
        if (!header) return false;
        if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        auto copy = copy_composition_layer_for_presenter(header);
        XrSwapchain* handle = copy ? composition_swapchain(*copy) : nullptr;
        std::uint64_t reason = (!handle || header->next) ? 1 : 0;
        if (!reason) {
            const auto chain = find_swapchain(*handle);
            if (!chain || chain->session != state) reason = 1;
            else {
                std::scoped_lock lock(chain->call_mutex);
                chain->composition_referenced = true;
                if (!chain->composition_capture || !chain->composition_capture->valid) reason = 2;
            }
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::composition_layer,
            header->type, i, handle ? handle_value(*handle) : 0, reason);
        supported = !reason && supported;
    }
    return supported;
}

[[nodiscard]] XrResult own_composition_images(
    const std::shared_ptr<SessionState>& session, GeneratedFrameEndInfo& metadata) {
    std::unordered_map<XrSwapchain, std::shared_ptr<CompositionImageSlot>> leased;
    for (auto& layer : metadata.composition_layers) {
        auto* handle = composition_swapchain(layer);
        if (!handle) return XR_ERROR_LAYER_INVALID;
        if (const auto found = leased.find(*handle); found != leased.end()) {
            *handle = found->second->image.handle;
            continue;
        }
        const auto chain = find_swapchain(*handle);
        if (!chain || chain->session != session) return XR_ERROR_LAYER_INVALID;
        std::scoped_lock call_lock(chain->call_mutex);
        if (!chain->composition_capture || !chain->composition_capture->valid) return XR_ERROR_LAYER_INVALID;
        auto& capture = *chain->composition_capture;
        std::shared_ptr<CompositionImageSlot> slot;
        for (auto& candidate : capture.outputs) {
            if (candidate && candidate.use_count() != 1) continue;
            if (!candidate) {
                // Publish into a distinct XR handle per retained real. Reusing
                // a single handle would change an older queued frame's image.
                candidate = std::make_shared<CompositionImageSlot>();
                auto info = chain->create_info;
                info.next = nullptr;
                info.createFlags &= ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
                info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
                CreatedPrivateSwapchain created;
                const XrResult created_result = with_runtime_entry(session.get(), [&] {
                    return create_private_swapchain(chain, info, &created)
                        ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
                });
                if (XR_FAILED(created_result)) { candidate.reset(); return created_result; }
                candidate->image = created.state;
                for (auto* texture : created.d3d11_resources) candidate->textures.emplace_back(texture);
                for (auto* texture : created.d3d12_resources) candidate->textures12.emplace_back(texture);
                candidate->fence = capture.fence;
                candidate->fence12 = capture.fence12;
            }
            slot = candidate;
            break;
        }
        if (!slot) return XR_ERROR_RUNTIME_FAILURE; // capacity must have been rendezvoused
        if (!acquire_and_wait_private_image(session.get(), session->dispatch, slot->image))
            return XR_ERROR_RUNTIME_FAILURE;
        HRESULT result;
        {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            if (capture.texture12) {
                if (slot->image.acquired_index >= slot->textures12.size()) return XR_ERROR_RUNTIME_FAILURE;
                result = copy_composition12(session->d3d12_device.Get(), session->d3d12_queue.Get(),
                    capture.fence12.Get(), capture.fence_value, slot->copy12,
                    slot->textures12[slot->image.acquired_index].Get(), D3D12_RESOURCE_STATE_COMMON,
                    capture.texture12.Get(), D3D12_RESOURCE_STATE_COMMON);
                if (SUCCEEDED(result)) slot->ready_value = slot->copy12.ready_value;
            } else {
                if (slot->image.acquired_index >= slot->textures.size()) return XR_ERROR_RUNTIME_FAILURE;
                capture.context->CopyResource(slot->textures[slot->image.acquired_index].Get(), capture.texture.Get());
                result = capture.context->Signal(capture.fence.Get(), capture.fence_value + 1);
                if (SUCCEEDED(result)) slot->ready_value = ++capture.fence_value;
                capture.context->Flush();
            }
        }
        // The worker waits for this fence, not the producer. No descriptor
        // becomes director-eligible before all projection + auxiliary pixels.
        const bool released = release_private_image(session.get(), session->dispatch, slot->image);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::composition_publish,
            result, handle_value(*handle), handle_value(slot->image.handle), slot->ready_value);
        if (FAILED(result) || !released) return XR_ERROR_RUNTIME_FAILURE;
        leased.emplace(*handle, slot);
        metadata.composition_images.push_back(slot);
        *handle = slot->image.handle;
    }
    return XR_SUCCESS;
}

// Match ownership by application resource and logical eye, even if mapping
// enumeration order changes. A space/layout/configuration transition cannot
// reuse images whose camera packet belongs to the preceding configuration.
[[nodiscard]] bool prepare_held_luke_afw_frame(
    const std::shared_ptr<const HeldAfwPair>& pair,
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    const std::array<XrSwapchain, 2>& handles,
    const std::array<std::uint32_t, 2>& logical_eyes,
    std::uint64_t revision, PreparedProjectionFrame& output) {
    if (!pair || pair->revision != revision || mappings.size() != 2 ||
        !projection_resource_layout_compatible(pair->projection, snapshot)) return false;
    for (std::size_t layer = 0; layer < snapshot.layers.size(); ++layer) {
        if (pair->projection.layers[layer].space != snapshot.layers[layer].space ||
            pair->projection.layers[layer].layer_flags != snapshot.layers[layer].layer_flags)
            return false;
    }
    if (logical_eyes[0] > 1 || logical_eyes[1] > 1 || logical_eyes[0] == logical_eyes[1])
        return false;
    std::vector<PreparedProjectionResource> resources;
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        const auto found = std::find_if(pair->resources.begin(), pair->resources.end(),
            [&](const auto& resource) {
                return resource.application_swapchain == mappings[index].application_swapchain &&
                    resource.output_swapchain == handles[index] && handles[index] != XR_NULL_HANDLE &&
                    resource.logical_eye == logical_eyes[index];
            });
        if (found == pair->resources.end()) return false;
        PreparedGeneration held{};
        held.kind = PreparedGenerationKind::prime;
        held.reason = GenerationPrepareReason::ready;
        held.current_handle = found->output_swapchain;
        held.anchor_is_current = true;
        resources.push_back({found->application_swapchain, held});
    }
    output.kind = PreparedGenerationKind::prime;
    output.reason = GenerationPrepareReason::ready;
    output.anchor_is_current = true;
    output.resources = std::move(resources);
    output.held_afw_pair = pair;
    return true;
}

[[nodiscard]] bool build_afw_terminal_submission(const XrFrameEndInfo* source,
    const ProjectionSnapshot& snapshot, const PreparedProjectionFrame& produced,
    GeneratedFrameEndInfo& terminal, ProjectionSnapshot& terminal_snapshot) {
    std::vector<ProjectionResourceDestination> destinations;
    for (const auto& resource : produced.resources)
        destinations.push_back({resource.application_swapchain, resource.generation.current_handle});
    if (produced.kind == PreparedGenerationKind::none ||
        !build_generated_frame_end_info(source, snapshot, false, destinations, &terminal))
        return false;
    if (produced.held_afw_pair) {
        const auto& owned = produced.held_afw_pair->projection;
        if (!projection_resource_layout_compatible(owned, snapshot)) return false;
        for (std::size_t layer = 0; layer < terminal.projections.size(); ++layer) {
            auto& copy = terminal.projections[layer];
            const auto& saved = owned.layers[layer];
            if (copy.layer.space != saved.space || copy.layer.layerFlags != saved.layer_flags)
                return false;
            for (std::size_t view = 0; view < copy.views.size(); ++view) {
                copy.views[view].pose = saved.views[view].pose;
                copy.views[view].fov = saved.views[view].fov;
            }
        }
        // displayTime names this physical presentation interval; the retained
        // render pose/FOV still describe the pixels generated on the old one.
    }
    return capture_projection_snapshot(&terminal.info, &terminal_snapshot);
}

[[nodiscard]] std::shared_ptr<const CompletedAfwFrame> freeze_completed_afw_frame(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* source,
    const ProjectionSnapshot& snapshot, std::vector<PostAfwFrameInput> inputs,
    std::uint64_t application_serial) {
    const auto failed = [&](std::int64_t reason, std::uint64_t detail = 0)
        -> std::shared_ptr<const CompletedAfwFrame> {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            reason, 53, application_serial, detail);
        return {};
    };
    if (inputs.size() != 2) return failed(1, inputs.size());
    // The exact-output probe must not silently strip an extension chain while
    // owning metadata. Such a packet is unsupported until that chain can be
    // deep-copied; normal AFW-only delivery remains unchanged.
    if (!source || source->next || (source->layerCount && !source->layers)) return failed(8);
    for (std::uint32_t i = 0; i < source->layerCount; ++i) {
        const auto* header = source->layers[i];
        if (!header || header->next) return failed(8, i);
        if (header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
        if (!projection->views) return failed(8, i);
        for (std::uint32_t v = 0; v < projection->viewCount; ++v)
            if (projection->views[v].next) return failed(8, (static_cast<std::uint64_t>(i) << 32) | v);
    }
    std::vector<ProjectionResourceDestination> destinations;
    for (const auto& input : inputs) {
        if (!input.generation || !input.capture.fence_value || input.views.empty() ||
            input.completed_swapchain == XR_NULL_HANDLE) return failed(2, input.capture.fence_value);
        // Source is already the ONE direct-XR terminal submission. Never
        // reconstruct it from the caller's original AER projection here.
        const bool names_terminal_image = std::any_of(snapshot.layers.begin(), snapshot.layers.end(),
            [&](const auto& layer) { return std::any_of(layer.views.begin(), layer.views.end(),
                [&](const auto& view) { return view.subImage.swapchain == input.completed_swapchain; }); });
        if (!names_terminal_image) return failed(4, handle_value(input.completed_swapchain));
        destinations.push_back({input.completed_swapchain, input.completed_swapchain});
    }
    // Classify auxiliary chains BEFORE requiring their retained release image.
    // Enumeration may provision generation on a color chain later used as a
    // quad. Its release capture is skipped until composition_referenced is set.
    // Checking only after a successful AFW freeze creates a permanent cold-start
    // loop: freeze needs the capture, but no future release knows to produce it.
    // This preflight is common to inline and director consumers; AFW-only never
    // enters this OFXR ownership path.
    if (!composition_can_be_owned(state, source)) return failed(3);
    GeneratedFrameEndInfo submission;
    if (!build_generated_frame_end_info(source, snapshot, false, destinations, &submission)) return failed(4);
    auto packet = std::make_shared<CompletedAfwFrame>();
    packet->submission = make_presenter_owned_frame(std::move(submission));
    if (!packet->submission) return failed(5);
    if (!capture_projection_snapshot(&packet->submission->info, &packet->snapshot)) return failed(6);
    // Snapshot auxiliary colors before any final backpressure, too. The worker
    // waits their readiness; no caller-owned HUD/quad survives the handoff.
    const XrResult owned = own_composition_images(state, *packet->submission);
    if (XR_FAILED(owned)) return failed(7, static_cast<std::uint64_t>(owned));
    packet->inputs = std::move(inputs);
    packet->application_serial = application_serial;
    log_luke_projection_poses(&packet->submission->info, application_serial, 10);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
        0, 52, application_serial, packet->inputs.front().capture.serial);
    return packet;
}

[[nodiscard]] XrResult handoff_three_slot_frame(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* source,
    const ProjectionSnapshot& snapshot, std::span<const ProjectionResourceMapping> mappings,
    bool metadata_pairable,
    std::shared_ptr<const CompletedAfwFrame> post_afw_frame = {}) {
    if (!metadata_pairable) {
        const XrResult drain = wait_for_presenter_idle(state);
        if (XR_FAILED(drain)) return drain;
    }
    // Copy metadata now; auxiliary image ownership is acquired after capacity
    // admission, before making this frame visible to either consumer.
    std::vector<ProjectionResourceDestination> identities;
    for (const auto& mapping : mappings)
        identities.push_back({mapping.application_swapchain, mapping.application_swapchain});
    GeneratedFrameEndInfo metadata;
    if (!post_afw_frame && !build_generated_frame_end_info(source, snapshot, false, identities, &metadata))
        return XR_ERROR_LAYER_INVALID;
    auto job = std::make_shared<SynthesisJob>();
    // AFW color is already the final direct-XR output. Do not apply the
    // game's presentation transform a second time in the OFXR consumer.
    job->presentation_transform = post_afw_frame
        ? xrfg::D3D12PresentationTransform{} : state->presentation_transform;
    job->metadata = post_afw_frame ? post_afw_frame->submission
        : make_presenter_owned_frame(std::move(metadata));
    if (!job->metadata) return XR_ERROR_LAYER_INVALID;
    job->snapshot = post_afw_frame ? post_afw_frame->snapshot : snapshot;
    std::optional<ProjectionSnapshot> previous;
    { std::scoped_lock lock(state->mutex); previous=state->previous_projection; }
    job->fraction=synthetic_interpolation_fraction(state,previous,job->snapshot);
    job->extra_fraction=synthetic_interpolation_fraction(state,previous,job->snapshot,1);
    job->mappings.assign(mappings.begin(), mappings.end());
    job->post_afw_frame = std::move(post_afw_frame);
    job->current = std::make_shared<ThreeSlotFrame>();
    job->current->queued_at=std::chrono::steady_clock::now();
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::three_slot_buffer_wait, handle_value(state->handle));
    {
        std::unique_lock lock(state->presenter_mutex);
        // The only output-capacity gate: the previous synthetic has gone and
        // only the retained real remains. No input is silently discarded.
        state->presenter_condition.wait(lock, [&] {
            return (!state->synthesis_busy && state->three_slots.can_accept_real()) ||
                   state->presenter_stop_requested || XR_FAILED(state->presenter_failure);
        });
        if (XR_FAILED(state->presenter_failure)) return state->presenter_failure;
        if (state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING;
        lock.unlock();
        // Capacity admission above also proves the prior worker is finished.
        // Preflight ALL resources before submitting either eye. Extra Releases
        // between EndFrames may advance only one history by more than one:
        // drain the old real once and prime the whole current stereo frame.
        // Keep submit_pair's strict ticket/lease checks and genuine error path.
        if (metadata_pairable) {
            bool capture_gap = false;
            for (const auto& mapping : mappings) {
                const auto chain = find_swapchain(mapping.application_swapchain);
                if (!chain) continue; // normal preparation reports invalid state
                std::shared_ptr<FrameGenerationSwapchainState> generation;
                std::optional<xrfg::D3D12HistoryCaptureTicket> capture;
                {
                    std::scoped_lock source_lock(chain->mutex);
                    generation = chain->frame_generation;
                    capture = chain->last_released_capture;
                }
                capture_gap |= (!chain->presentation_transform_known && job->presentation_transform.active) ||
                    (chain->presentation_transform_known && chain->presentation_transform_active != job->presentation_transform.active);
                if (!job->post_afw_frame) {
                    capture_gap |= generation && generation->synthesizer && capture &&
                        generation->synthesizer->has_capture_gap(*capture);
                } else {
                    const auto& inputs = job->post_afw_frame->inputs;
                    const auto found = std::find_if(inputs.begin(), inputs.end(),
                        [&](const auto& input) { return input.application_swapchain == mapping.application_swapchain; });
                    capture_gap |= found == inputs.end() || !generation ||
                        !generation->afw_temporal_synthesizer ||
                        generation->afw_temporal_synthesizer->has_capture_gap(found->capture);
                }
                if (state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
                    generation && generation->synthesizer &&
                    !generation->synthesizer->can_pair_fsr(chain->last_released_motion_vectors)) {
                    capture_gap = true;
                    xrfg::report_fsr_frame_generation_status(xrfg::FsrFrameGenerationStatus::waiting_for_guides);
                }
            }
            if (capture_gap) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::continuity_reset, 2, handle_value(state->handle));
                const XrResult drain = wait_for_presenter_idle(state);
                if (XR_FAILED(drain)) return drain;
                // An empty director buffer makes job->pair false below;
                // prepare_frame_generation retires/prime-copies BOTH eyes.
            }
        }
        const XrResult composition = job->post_afw_frame ? XR_SUCCESS
            : own_composition_images(state, *job->metadata);
        if (XR_FAILED(composition)) return composition;
        lock.lock();
        if (XR_FAILED(state->presenter_failure)) return state->presenter_failure;
        if (state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING;
        job->pair = !state->three_slots.empty();
        job->triple=djules_triple_director(*state);
        job->current->source_sequence = state->next_source_sequence++;
        job->current->previous_sequence = job->pair
            ? state->three_slots.slot(0)->value->source_sequence : 0;
        // One atomic handoff starts generation AND makes A eligible for the
        // director. Worker GPU latency does not delay the release of A.
        static_cast<void>(state->three_slots.push_real(job->current));
        state->synthesis_busy = true;
        state->synthesis_job = job;
    }
    xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::three_slot_buffer_wait,
        XR_SUCCESS, job->current->source_sequence, job->current->previous_sequence);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::three_slot_handoff,
        job->pair ? 1 : 0, job->current->source_sequence, job->current->previous_sequence,
        job->post_afw_frame ? job->post_afw_frame->application_serial : 0);
    if (state->frame_capture && state->frame_capture->active()) { try {
        state->frame_capture->record("{\"kind\":\"handoff\",\"app_frame\":"+std::to_string(state->diagnostic_app_frame)+
            ",\"qpc\":"+std::to_string(xrfg::FrameCapture::qpc())+",\"source_id\":"+std::to_string(job->current->source_sequence)+
            ",\"previous_id\":"+std::to_string(job->current->previous_sequence)+"}");
    } catch (...) {} }
    state->presenter_condition.notify_all();
    return XR_SUCCESS;
}


// V23189 deliberately owns the whole two-eye transaction. Both private
// current images are populated first, AFW replaces only the peer, and only then
// are the D3D11 OpenXR images published/released.  This prevents the first eye
// from escaping before the cross-swapchain AFW write has been queued.
[[nodiscard]] PreparedProjectionFrame prepare_luke_afw_frame(
    const std::shared_ptr<SessionState>& session,
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    std::optional<std::uint32_t> released_eye,
    std::uint64_t frame_serial,
    bool capture_temporal_input,
    std::vector<PostAfwFrameInput>& completed_inputs) noexcept {
    PreparedProjectionFrame output{};
    struct Entry {
        std::shared_ptr<SwapchainState> swapchain;
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        std::vector<xrfg::D3D12ReprojectionView> views;
        PreparedGeneration prepared;
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> guide;
        xrfg::D3D12HistoryCaptureTicket afw_final_capture{};
        xrfg::D3D11PublicationReceipt publication;
        std::optional<AfwSourceCapture> source_capture;
        std::uint32_t logical_eye{std::numeric_limits<std::uint32_t>::max()};
    };
    std::array<Entry, 2> entries{};
    // Diagnostic-only stages 28/29: missing-guide subreason and the age of
    // the selected eye's NGX publication relative to the newest publication.
    const auto report_missing = [](std::int64_t reason,
        std::uint64_t detail) noexcept {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_afw_stage,
            reason, 28, detail, 0);
    };
    const auto release_deferred = [&]() noexcept {
        for (Entry& entry : entries) {
            if (!entry.prepared.publication_deferred || !entry.generation)
                continue;
            static_cast<void>(release_private_image(
                session.get(), session->dispatch, entry.generation->current[kAfwScratchSlot]));
            entry.prepared.publication_deferred = false;
        }
    };
    try {
        if (!session || !session->luke_afw_enabled || !session->luke_afw ||
            (session->graphics_binding_capabilities & 8ULL) == 0 ||
            mappings.size() != 2) {
            output.reason = GenerationPrepareReason::missing_generation;
            return output;
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const ProjectionResourceMapping& mapping = mappings[index];
            if (mapping.views.size() != 1) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                release_deferred();
                return output;
            }
            Entry& entry = entries[index];
            entry.swapchain = find_swapchain(mapping.application_swapchain);
            const auto views = build_reprojection_views(snapshot, mapping);
            if (!entry.swapchain || !views || views->size() != 1) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                release_deferred();
                return output;
            }
            entry.views = *views;
            {
                std::scoped_lock lock(entry.swapchain->mutex);
                entry.generation = entry.swapchain->frame_generation;
                if (entry.swapchain->last_released_capture)
                    entry.source_capture = AfwSourceCapture{
                        entry.generation, *entry.swapchain->last_released_capture};
                if (entry.swapchain->dlss_logical_eye_index)
                    entry.logical_eye = *entry.swapchain->dlss_logical_eye_index;
            }
            if (!entry.generation || !entry.generation->d3d11_interop ||
                entry.logical_eye > 1) {
                report_missing(1, index);
                output.reason = GenerationPrepareReason::missing_capture;
                output.failed_swapchain = mapping.application_swapchain;
                release_deferred();
                return output;
            }
        }

        const auto afw_status = xrfg::embedded::afw_snapshot();
        const auto hold_last_pair = [&]() -> bool {
            std::shared_ptr<const HeldAfwPair> pair;
            {
                std::scoped_lock guide_lock(session->luke_guide_mutex);
                pair = session->luke_afw_last_pair;
            }
            if (!prepare_held_luke_afw_frame(pair, snapshot, mappings,
                    {entries[0].generation->current[kAfwScratchSlot].handle,
                     entries[1].generation->current[kAfwScratchSlot].handle},
                    {entries[0].logical_eye, entries[1].logical_eye},
                    afw_status.revision, output)) return false;
            // No new AFW color was produced. Keep both image ownership and
            // camera metadata, while leaving the post-AFW temporal anchor intact.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                1, 33, afw_status.revision, pair->production_serial);
            return true;
        };
        if (!released_eye) {
            if (hold_last_pair()) return output;
        }

        if (!released_eye || *released_eye > 1 ||
            entries[0].logical_eye == entries[1].logical_eye) {
            report_missing(2,
                (static_cast<std::uint64_t>(released_eye.value_or(255)) << 32) |
                (static_cast<std::uint64_t>(entries[0].logical_eye) << 16) |
                entries[1].logical_eye);
            output.reason = GenerationPrepareReason::missing_capture;
            release_deferred();
            return output;
        }
        const std::size_t source_index =
            entries[0].logical_eye == *released_eye ? 0U : 1U;
        const std::size_t destination_index = 1U - source_index;

        Entry& source = entries[source_index];
        Entry& destination = entries[destination_index];
        std::optional<float> xr_eye_baseline;
        {
            std::scoped_lock lock(session->mutex);
            xr_eye_baseline = session->luke_xr_eye_baseline;
        }
        if (!xr_eye_baseline) {
            report_missing(7, 0); // never measure IPD from cross-time poses
            output.reason = GenerationPrepareReason::missing_capture;
            return output;
        }
        if (!source.source_capture) {
            output.reason = GenerationPrepareReason::missing_capture;
            return output;
        }
        // Prepare precisely this frozen color before resolving its original
        // production. No latest-guide or eye-parity join survives here.
        {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            session->luke_afw_last_pair.reset();
        }

        // The peer is an AFW output, not a second temporal-synthesis input.
        // Retire any older use of its private colour, acquire it, and make the
        // D3D12 queue wait for its last D3D11 publication.  Do not consume a
        // peer history capture or submit a redundant FSR/NVIDIA prime that AFW
        // would immediately overwrite.
        {
            std::scoped_lock call_lock(destination.swapchain->call_mutex);
            {
                std::scoped_lock gpu_lock(session->gpu_mutex);
                const HRESULT retire_result =
                    destination.generation->synthesizer->retire_previous();
                if (FAILED(retire_result)) {
                    output.reason = GenerationPrepareReason::retire_previous_failed;
                    output.failed_swapchain =
                        mappings[destination_index].application_swapchain;
                    return output;
                }
            }
            if (!acquire_and_wait_private_image(
                    session.get(), session->dispatch, destination.generation->current[kAfwScratchSlot])) {
                output.reason =
                    GenerationPrepareReason::current_private_acquire_failed;
                output.failed_swapchain =
                    mappings[destination_index].application_swapchain;
                return output;
            }
            HRESULT peer_result = E_UNEXPECTED;
            {
                std::scoped_lock gpu_lock(session->gpu_mutex);
                peer_result =
                    destination.generation->d3d11_interop->prepare_synthesis();
            }
            if (FAILED(peer_result)) {
                static_cast<void>(release_private_image(
                    session.get(), session->dispatch, destination.generation->current[kAfwScratchSlot]));
                output.reason = GenerationPrepareReason::synthesis_failed;
                output.failed_swapchain =
                    mappings[destination_index].application_swapchain;
                return output;
            }
            destination.prepared.kind = PreparedGenerationKind::prime;
            destination.prepared.reason = GenerationPrepareReason::ready;
            destination.prepared.current_handle =
                destination.generation->current[kAfwScratchSlot].handle;
            destination.prepared.publication_deferred = true;
            destination.prepared.current_destination_index =
                static_cast<std::uint32_t>(kAfwScratchSlot) *
                    destination.generation->current_images_per_slot +
                destination.generation->current[kAfwScratchSlot].acquired_index;
        }

        source.prepared = prepare_frame_generation(
            mappings[source_index].application_swapchain, false,
            std::span<const xrfg::D3D12ReprojectionView>(
                source.views.data(), source.views.size()), 0.5f,
            false, nullptr, true, nullptr, &*source.source_capture);
        if (source.prepared.kind != PreparedGenerationKind::prime ||
            !source.prepared.publication_deferred) {
            output.reason = source.prepared.reason;
            output.failed_swapchain =
                mappings[source_index].application_swapchain;
            release_deferred();
            return output;
        }
        const auto source_images =
            source.generation->d3d11_interop->current_destination_images();
        const auto destination_images =
            destination.generation->d3d11_interop->current_destination_images();
        if (source.prepared.current_destination_index >= source_images.size() ||
            destination.prepared.current_destination_index >=
                destination_images.size()) {
            output.reason = GenerationPrepareReason::invalid_private_swapchain;
            release_deferred();
            return output;
        }

        std::uint32_t origin_token{};
        HRESULT origin_result = E_UNEXPECTED;
        {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            origin_result = xrfg::read_luke_color_origin(
                session->d3d12_queue.Get(),
                source_images[source.prepared.current_destination_index],
                &origin_token);
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            origin_result, 89, frame_serial, origin_token);
        if (FAILED(origin_result) || !origin_token) {
            output.reason = GenerationPrepareReason::missing_capture;
            release_deferred();
            return output;
        }
        source.guide = xrfg::resolve_luke_guide_token(
            origin_token, session->d3d12_queue.Get(), session->d3d12_device.Get());
        std::uint32_t camera_rejection = !source.guide ? 1u : 0u;
        if (source.guide) {
            if (!source.guide->camera_data_valid) camera_rejection |= 2u;
            if (!source.guide->previous_opposite_guide) camera_rejection |= 4u;
            else if (!source.guide->previous_opposite_guide->camera_data_valid)
                camera_rejection |= 8u;
        }
        if (camera_rejection != 0) {
            xrfg::embedded::report_afw_guide_rejection(camera_rejection);
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                camera_rejection, 132, frame_serial, origin_token);
            report_missing(8, origin_token);
            output.reason = GenerationPrepareReason::missing_capture;
            release_deferred();
            return output;
        }
        std::uint64_t last_evaluated{};
        {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            last_evaluated = session->luke_afw_last_evaluated_publication;
        }
        const auto ingress = [&](std::uint64_t stage, std::uint64_t value) {
            const auto flags = static_cast<std::int64_t>(source.logical_eye |
                (session->menu_enabled ? 0x100 : 0) |
                (session->nvidia_new_route ? 0x200 : 0) |
                (static_cast<unsigned>(afw_status.desired.mode) << 12));
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                flags, stage, frame_serial, value);
        };
        ingress(71, source.guide->publication);
        ingress(72, source.source_capture->capture.serial);
        ingress(73, source.guide->stream);
        ingress(74, source.guide->epoch);
        ingress(75, source.guide->camera_frame_token_valid
            ? source.guide->camera_frame_token : UINT64_MAX);
        ingress(76, source.guide->previous_opposite_guide
            ? source.guide->previous_opposite_guide->publication : 0);
        ingress(77, source.guide->producer_fence_value);
        ingress(78, make_color_identity(frame_serial, source.source_capture->capture.serial,
            source.swapchain->handle, 0, source.logical_eye, 3, source.views.front()).pose_hash);
        ingress(90, source.guide->publication);
        ingress(79, make_color_identity(frame_serial, 0,
            destination.swapchain->handle, 0, destination.logical_eye, 3, destination.views.front()).pose_hash);
        if (!should_evaluate_luke_afw_publication(
                source.guide->publication, last_evaluated)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                static_cast<std::int64_t>(source.guide->publication),
                34, last_evaluated, source.guide->publication);
            release_deferred();
            output.reason = GenerationPrepareReason::synthesis_stale_guides;
            return output;
        }
        // Never evaluate one original color production twice for this eye.
        {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            auto& last = session->luke_afw_last_guide_publication[*released_eye];
            if (source.guide->publication <= last) {
                report_missing(5,
                    (static_cast<std::uint64_t>(*released_eye) << 32) | last);
                output.reason = GenerationPrepareReason::missing_capture;
                release_deferred();
                return output;
            }
            last = source.guide->publication;
        }

        HRESULT afw_result = E_UNEXPECTED;
        bool afw_evaluated = false;
        const auto& afw_settings = afw_status.desired;
        {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            afw_result = session->luke_afw->synthesize(
                source_images[source.prepared.current_destination_index],
                destination_images[destination.prepared.current_destination_index],
                *source.guide, source.views.front(), destination.views.front(),
                source.logical_eye, *xr_eye_baseline, afw_settings);
            afw_evaluated = SUCCEEDED(afw_result);
        }
        if (afw_evaluated) {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            for (Entry& entry : entries) {
                const auto publish_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    handle_value(entry.swapchain->handle), frame_serial, source.guide->publication);
                afw_result = entry.generation->d3d11_interop->publish(
                    entry.prepared.current_destination_index,
                    std::nullopt, &entry.publication);
                xrfg::bridge_flight_logger().end(publish_token,
                    xrfg::BridgeFlightOperation::d3d11_publish, afw_result,
                    handle_value(entry.swapchain->handle), frame_serial, source.guide->publication);
                log_luke_publication_boundary(frame_serial, 1,
                    static_cast<std::size_t>(&entry - entries.data()), entry.swapchain->handle,
                    entry.generation->current[kAfwScratchSlot].handle, entry.generation->synthetic[0].handle,
                    entry.generation->d3d11_interop);
                if (FAILED(afw_result)) break;
            }
        }
        if (afw_evaluated && SUCCEEDED(afw_result) && capture_temporal_input) {
            // This is the terminal D3D11 XR output, not the intermediate
            // D3D12 AFW render target. Both eyes have been published. Snapshot
            // the exact acquired images before either XR release; the receipt
            // binds image index and completion independently of mutable cursors.
            std::scoped_lock gpu_lock(session->gpu_mutex);
            for (Entry& entry : entries) {
                if (!entry.generation->afw_final_history) { afw_result = E_UNEXPECTED; break; }
                afw_result = entry.generation->d3d11_interop->capture_published_current(entry.publication);
                if (SUCCEEDED(afw_result))
                    afw_result = entry.generation->afw_final_history->capture(
                        entry.publication.current_destination_index, &entry.afw_final_capture);
                if (SUCCEEDED(afw_result))
                    afw_result = entry.generation->d3d11_interop->finish_capture();
                if (SUCCEEDED(afw_result))
                    afw_result = entry.generation->afw_final_history->commit(entry.afw_final_capture);
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    afw_result, 54, frame_serial,
                    (static_cast<std::uint64_t>(entry.publication.current_destination_index) << 32) |
                    entry.logical_eye);
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    entry.logical_eye, 55, frame_serial, entry.publication.copy_complete_value);
                if (FAILED(afw_result)) break;
                completed_inputs.push_back({entry.swapchain->handle,
                    entry.generation->current[kAfwScratchSlot].handle, entry.generation,
                    entry.afw_final_capture, entry.views,
                    make_color_identity(frame_serial,entry.afw_final_capture.serial,
                        entry.generation->current[kAfwScratchSlot].handle,
                        entry.publication.current_destination_index,entry.logical_eye,0,entry.views.front(),
                        entry.generation->current[kAfwScratchSlot].acquired_index)});
            }
            afw_evaluated = SUCCEEDED(afw_result);
            if (!afw_evaluated) completed_inputs.clear();
        }
        if (afw_evaluated) {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            session->luke_afw_last_evaluated_publication =
                source.guide->publication;
        }
        float motion_scale_x = source.guide->scale_x *
            static_cast<float>(source.guide->output_width) /
            static_cast<float>(source.guide->motion_width);
        float motion_scale_y = source.guide->scale_y *
            static_cast<float>(source.guide->output_height) /
            static_cast<float>(source.guide->motion_height);
        xrfg::embedded::report_afw_evaluation(
            source.guide->publication, source.logical_eye,
            source.guide->motion_width, source.guide->motion_height,
            source.guide->output_width, source.guide->output_height,
            motion_scale_x, motion_scale_y, afw_result);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::synthesis_pair,
            afw_result, 0xAF023291ULL, source.guide->publication,
            (static_cast<std::uint64_t>(source.logical_eye) << 32) |
                destination.logical_eye);
        bool released = true;
        for (Entry& entry : entries) {
            const auto index = static_cast<std::size_t>(&entry - entries.data());
            log_luke_publication_boundary(frame_serial, 2, index, entry.swapchain->handle,
                entry.generation->current[kAfwScratchSlot].handle, entry.generation->synthetic[0].handle,
                entry.generation->d3d11_interop);
            released = release_private_image(
                session.get(), session->dispatch, entry.generation->current[kAfwScratchSlot]) && released;
            entry.prepared.publication_deferred = false;
            log_luke_publication_boundary(frame_serial, 3, index, entry.swapchain->handle,
                entry.generation->current[kAfwScratchSlot].handle, entry.generation->synthetic[0].handle,
                entry.generation->d3d11_interop);
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_input_guide,
            source.logical_eye, frame_serial, source.guide->publication, source.guide->epoch);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_input_camera,
            source.logical_eye, frame_serial, source.guide->publication, dlss_guide_camera_hash(*source.guide));
        if (FAILED(afw_result) || !released) {
            output.reason = FAILED(afw_result)
                ? GenerationPrepareReason::synthesis_failed
                : GenerationPrepareReason::private_release_failed;
            return output;
        }
        auto pair = std::make_shared<HeldAfwPair>();
        pair->projection = snapshot;
        pair->revision = afw_status.revision;
        pair->production_serial = frame_serial;
        for (std::size_t index = 0; index < entries.size(); ++index)
            pair->resources[index] = {mappings[index].application_swapchain,
                entries[index].generation->current[kAfwScratchSlot].handle,
                entries[index].logical_eye};
        {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            session->luke_afw_last_pair = std::move(pair);
        }
        xrfg::two_frame_capture_mark_afw_evaluation();
        xrfg::report_dlss_motion_vector_use();
        output.kind = PreparedGenerationKind::prime;
        output.reason = GenerationPrepareReason::ready;
        output.anchor_is_current = true;
        output.resources.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            output.resources.push_back({
                mappings[index].application_swapchain,
                entries[index].prepared});
        }
        return output;
    } catch (...) {
        release_deferred();
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}

[[nodiscard]] std::optional<PendingApplicationFrame> latest_pending_application_frame(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order,
    bool match_latest_by_submission) noexcept {
    try {
        std::scoped_lock lock(state->mutex);
        if (consume_in_submission_order) {
            return state->pending_frames.empty()
                ? std::nullopt
                : std::optional<PendingApplicationFrame>{
                      state->pending_frames.front()};
        }
        const auto frame = std::find_if(
            state->pending_frames.begin(),
            state->pending_frames.end(),
            [display_time](const PendingApplicationFrame& candidate) {
                return candidate.display_time == display_time;
            });
        if (frame != state->pending_frames.end()) {
            return *frame;
        }
        // Native Stereo Fix may rewrite displayTime. A begun frame with no
        // overlapping wait identifies the newest pending record by call order.
        return match_latest_by_submission && !state->pending_frames.empty()
            ? std::optional<PendingApplicationFrame>{
                  state->pending_frames.back()}
            : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<XrDuration> latest_pending_application_period(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order,
    bool match_latest_by_submission) noexcept {
    const auto frame = latest_pending_application_frame(state, display_time,
        consume_in_submission_order, match_latest_by_submission);
    return frame ? std::optional<XrDuration>{frame->display_period} : std::nullopt;
}

[[nodiscard]] XrTime generation_resume_time(XrTime display_time) noexcept {
    constexpr XrTime maximum_time = std::numeric_limits<XrTime>::max();
    if (display_time > maximum_time - kGenerationCooldownDuration) {
        return maximum_time;
    }
    return display_time + kGenerationCooldownDuration;
}

void schedule_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail) noexcept {
    if (!state) {
        return;
    }
    try {
        const auto deadline =
            std::chrono::steady_clock::now() + kStructuralQuarantineDuration;
        {
            std::scoped_lock lock(state->mutex);
            if (deadline > state->generation_resume_wall_time) {
                state->generation_resume_wall_time = deadline;
            }
            state->previous_projection.reset();
        }
        state->generation_steady_state_established = false;
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_last_frame.reset();
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::continuity_reset,
            static_cast<std::int64_t>(reason),
            handle_value(state->handle),
            detail,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kStructuralQuarantineDuration)
                    .count()));
    } catch (...) {
    }
}

void clear_generation_continuity(
    const std::shared_ptr<SessionState>& state) noexcept {
    if (continuous_presenter_active(state))
        static_cast<void>(wait_for_presenter_idle(state));
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::continuity_reset,
        0,
        state ? handle_value(state->handle) : 0);
    try {
        {
            std::scoped_lock lock(state->mutex);
            state->previous_projection.reset();
        }
        for (const auto& swapchain : find_swapchains(state)) {
            std::scoped_lock call_lock(swapchain->call_mutex);
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
                swapchain->luke_admitted_identity.reset();
                swapchain->luke_admitted_capture_serial = 0;
            }
            if (!generation || !generation->synthesizer) {
                continue;
            }
            std::scoped_lock gpu_lock(state->gpu_mutex);
            static_cast<void>(generation->synthesizer->retire_previous());
            if (generation->afw_temporal_synthesizer) {
                static_cast<void>(
                    generation->afw_temporal_synthesizer->retire_previous());
            }
        }
    } catch (...) {
    }
}

void enter_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail) noexcept {
    schedule_generation_quarantine(state, reason, detail);
    clear_generation_continuity(state);
}

[[nodiscard]] bool should_quarantine_generation_failure(
    GenerationPrepareReason reason) noexcept {
    switch (reason) {
        case GenerationPrepareReason::reprojection_views_failed:
        case GenerationPrepareReason::unknown_swapchain:
        case GenerationPrepareReason::invalid_private_swapchain:
        case GenerationPrepareReason::retire_previous_failed:
        case GenerationPrepareReason::synthesis_failed:
        case GenerationPrepareReason::mixed_resource_transaction:
        case GenerationPrepareReason::generated_end_info_failed:
        case GenerationPrepareReason::exception:
        case GenerationPrepareReason::presenter_unsafe_composition:
            return true;
        default:
            return false;
    }
}

struct InternalCycleResult {
    XrResult result{XR_SUCCESS};
    std::chrono::steady_clock::duration wait_elapsed{};
    XrDuration predicted_display_period{};
    XrTime display_time{};
    bool completed{};
};

[[nodiscard]] InternalCycleResult submit_current_cycle(
    const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo& current_end_info,
    std::uint64_t frame_serial = 0,
    const PreparedProjectionFrame* prepared = nullptr,
    std::optional<XrFrameState> adopted_frame = std::nullopt,
    GeneratedFrameEndInfo* owned_output=nullptr, bool synthetic_output=false) {
    InternalCycleResult output{};
    if (state->dispatch->wait_frame == nullptr ||
        state->dispatch->begin_frame == nullptr ||
        state->dispatch->end_frame == nullptr) {
        return output;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    const auto wait_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_wait_frame,
        handle_value(state->handle));
    const auto wait_started = std::chrono::steady_clock::now();
    const XrResult wait_result = adopted_frame
        ? (frame_state = *adopted_frame, frame_state.next = nullptr, XR_SUCCESS)
        : state->dispatch->wait_frame(state->handle, &wait_info, &frame_state);
    output.wait_elapsed = std::chrono::steady_clock::now() - wait_started;
    output.predicted_display_period = frame_state.predictedDisplayPeriod;
    output.display_time = frame_state.predictedDisplayTime;
    xrfg::bridge_flight_logger().end(
        wait_token,
        xrfg::BridgeFlightOperation::internal_wait_frame,
        wait_result,
        static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
        static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
        frame_state.shouldRender);
    if (state->luke_afw_enabled && frame_serial != 0) {
        // result 4: requested real time versus physical runtime slot.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            4, frame_serial,
            static_cast<std::uint64_t>(current_end_info.displayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
        xrfg::two_frame_capture_timeline(
            "frame", 4, frame_serial,
            static_cast<std::uint64_t>(current_end_info.displayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
    }
    if (XR_FAILED(wait_result)) {
        return output;
    }

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const auto begin_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_begin_frame,
        handle_value(state->handle));
    const XrResult begin_result = with_runtime_entry(state, [&] {
        return state->dispatch->begin_frame(state->handle, &begin_info);
    });
    xrfg::bridge_flight_logger().end(
        begin_token,
        xrfg::BridgeFlightOperation::internal_begin_frame,
        begin_result,
        static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
    if (XR_FAILED(begin_result)) {
        output.result = begin_result;
        return output;
    }

    XrFrameEndInfo submitted = current_end_info;
    submitted.next = nullptr;
    submitted.displayTime = frame_state.predictedDisplayTime;
    if (frame_state.shouldRender == XR_FALSE) {
        submitted.layerCount = 0;
        submitted.layers = nullptr;
    }

    const auto end_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_end_frame,
        handle_value(state->handle),
        static_cast<std::uint64_t>(submitted.displayTime),
        submitted.layerCount);
    join_runtime_queue_to_application(state.get(), state->app_end_frame_release_value);
    const XrResult end_result = with_runtime_entry(state, [&] {
        log_luke_physical_boundary(state, &submitted, frame_serial, 6, prepared);
        GeneratedFrameEndInfo pending;
        if(prepared && !owned_output) {
            // Only this real's exact staging; no extra current copy or latest lookup.
            for(const auto& resource:prepared->resources) {
                const auto& output=resource.generation;
                if(output.staging_generation) pending.pending_outputs.push_back({output.staging_generation,
                    output.staging_generation->current_staging[output.current_slot],output.synthesizer,output.staging_ticket,false});
            }
        }
        if(!handover_djules_outputs(state.get(),owned_output?owned_output->pending_outputs:pending.pending_outputs))
            return XR_ERROR_RUNTIME_FAILURE;
        return state->fps_overlay
            ? state->fps_overlay->end_frame(&submitted, synthetic_output)
            : state->dispatch->end_frame(state->handle, &submitted);
    });
    log_luke_physical_boundary(state, &submitted, frame_serial, 7, prepared, end_result);
    xrfg::bridge_flight_logger().end(
        end_token,
        xrfg::BridgeFlightOperation::internal_end_frame,
        end_result,
        static_cast<std::uint64_t>(submitted.displayTime),
        submitted.layerCount,
        frame_state.shouldRender);
    if (state->luke_afw_enabled && frame_serial != 0) {
        // result 5: real output submitted (or runtime rejection).
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            5, frame_serial,
            static_cast<std::uint64_t>(submitted.displayTime),
            (static_cast<std::uint64_t>(submitted.layerCount) << 32) |
                static_cast<std::uint32_t>(end_result));
        xrfg::two_frame_capture_timeline(
            "frame", 5, frame_serial,
            static_cast<std::uint64_t>(submitted.displayTime),
            (static_cast<std::uint64_t>(submitted.layerCount) << 32) |
                static_cast<std::uint32_t>(end_result));
    }
    if (XR_FAILED(end_result)) {
        output.result = end_result;
        return output;
    }

    output.completed = true;
    return output;
}


// The presenter exists because some runtimes do not pace xrWaitFrame. The
// layer submits two frames per application frame and they must land one
// display period apart; a runtime that returns the wait immediately leaves
// both in the same scanout window and the compositor discards one, so the
// headset shows half rate while the GPU does all of the synthesis work.
//
// Inferring that from the application's threading - whether its second wait
// lands inside the previous frame, twice in a row - reads a property of the
// application under load rather than of the runtime. It moves with scene
// cost, and because the promotion latches the first time the race resolves,
// a session can run un-paced indefinitely and then engage the moment the
// view happens to get cheap. Measuring the runtime does not have that
// problem: how long its wait blocks is the same whether the headset is
// pointed at the floor or at a city.
//
// Scoped to SteamVR, like the throttle detector it sits beside. The two are
// the same runtime quirk seen from opposite ends - one throttles the inline
// second cycle, the other does not pace the application at all - and VDXR,
// which blocks a full period and gets this right for free, is deliberately
// left alone rather than being told apart by a measurement that a merely
// late application can also produce.
[[nodiscard]] bool runtime_wait_lacks_pacing(
    const std::shared_ptr<SessionState>& state) noexcept {
    // Enough waits to rule out an application that was simply late for one.
    constexpr std::uint32_t kUnpacedWaitPromotionStreak = 3;
    if (!state->dispatch->steamvr_runtime) {
        return false;
    }
    std::scoped_lock lock(state->mutex);
    return state->minimum_runtime_display_period > 0 &&
           state->unpaced_wait_streak >= kUnpacedWaitPromotionStreak;
}
// The inline path hands the synthetic to the runtime and then, immediately
// after, the real frame. Nothing in the layer decides how far apart those two
// land: the gap is whatever the runtime imposes when it blocks the second
// wait. When that gap is much shorter than a display period both frames
// arrive inside one scanout window, the compositor keeps the later one - the
// real frame - and drops the synthetic. The next window has nothing new and
// repeats. Half the layer's work is discarded, the overlay still counts it,
// and the application is separately halved because each of its frames now
// costs two runtime cycles. A game that held the full rate without the layer
// drops to half with it.
//
// The presenter fixes this by construction: it sleeps to its own
// scanout-locked grid between hand-overs, so the two frames are a period
// apart whatever the runtime does with a wait.
//
// Measured here rather than inferred. The two existing detectors ask whether
// the situation looks like one that needs a presenter - how many threads the
// application uses, how long a wait took - and on Atomic Heart both answer
// no while the output is plainly wrong. Captured on that title across two
// runtimes and two refresh rates, the gap sits at 0.42, 0.45 and 0.54 of a
// period against about 1.0 when the spacing is right, so three quarters
// separates them with margin either way.
//
// A long streak because promoting is a large switch - it hands every
// downstream frame call to another thread, and an over-eager version of the
// SteamVR detector once hung the GPU four milliseconds after firing. Thirty
// pairs is a quarter of a second at 120 Hz and cannot be reached by a hitch,
// while the real thing holds for thousands of consecutive frames.
[[nodiscard]] bool inline_pair_lands_in_one_scanout(
    const std::shared_ptr<SessionState>& state,
    std::chrono::steady_clock::duration gap) noexcept {
    if (djules_inline_runtime(*state)) {
        std::scoped_lock lock(state->mutex);
        state->bunched_pair_streak = 0;
        return false;
    }
    constexpr std::uint32_t kBunchedPairPromotionStreak = 30;
    constexpr std::int64_t kBunchedNumerator = 3;
    constexpr std::int64_t kBunchedDenominator = 4;
    const auto gap_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(gap).count();
    std::scoped_lock lock(state->mutex);
    const XrDuration period = state->minimum_runtime_display_period;
    if (period <= 0 || gap_nanoseconds < 0) {
        state->bunched_pair_streak = 0;
        return false;
    }
    const bool bunched =
        gap_nanoseconds * kBunchedDenominator < period * kBunchedNumerator;
    state->bunched_pair_streak =
        bunched ? state->bunched_pair_streak + 1 : 0;
    if (!bunched ||
        state->bunched_pair_streak < kBunchedPairPromotionStreak) {
        return false;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_transition,
        static_cast<std::int64_t>(state->bunched_pair_streak),
        static_cast<std::uint64_t>(gap_nanoseconds),
        static_cast<std::uint64_t>(period),
        200);
    return true;
}

[[nodiscard]] bool steamvr_wait_requires_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    const InternalCycleResult& cycle) noexcept {
    // FSR owns an inline two-eye pair whose guides and history are tied to the
    // application's submission timeline. Moving it to the asynchronous
    // director corrupts that ownership. A genuine overlapping application
    // xrWaitFrame can still request the established pipelined path, but a
    // runtime timing heuristic must never do so.
    if (state->frame_generation_mode ==
        xrfg::D3D12FrameGenerationMode::fsr3) {
        std::scoped_lock lock(state->mutex);
        state->steamvr_throttled_wait_streak = 0;
        return false;
    }
    if (!state->dispatch->steamvr_runtime || !cycle.completed) {
        return false;
    }
    const auto elapsed_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            cycle.wait_elapsed).count();
    std::uint32_t streak = 0;
    XrDuration baseline = 0;
    bool throttled = false;
    {
        std::scoped_lock lock(state->mutex);
        baseline = state->minimum_runtime_display_period;
        if (baseline > 0) {
            // Only a wait that actually blocked is evidence that the runtime is
            // throttling the inline cycle. An inflated predictedDisplayPeriod
            // says something different: SteamVR widens it when it considers the
            // *caller* late, so counting it closed a feedback loop: a warm-up
            // hitch widened the period, the widened period promoted the
            // presenter, and the promotion hitched harder. UEVR reached the
            // promotion three frames after its first synthesis on two 20 us
            // waits, with SteamVR reporting 33.3 ms and then 22.2 ms against an
            // 11.1 ms baseline, and the GPU hung 4 ms later.
            const XrDuration half_period = baseline / 2;
            throttled = elapsed_nanoseconds >= half_period;
        }
        state->steamvr_throttled_wait_streak = throttled
            ? state->steamvr_throttled_wait_streak + 1
            : 0;
        streak = state->steamvr_throttled_wait_streak;
    }
    if (throttled) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            streak,
            elapsed_nanoseconds > 0
                ? static_cast<std::uint64_t>(elapsed_nanoseconds)
                : 0,
            static_cast<std::uint64_t>(cycle.predicted_display_period),
            static_cast<std::uint64_t>(baseline));
    }
    return streak >= 3;
}

void consume_application_frame(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order,
    bool match_latest_by_submission) {
    std::scoped_lock lock(state->mutex);
    if (consume_in_submission_order) {
        if (!state->pending_frames.empty()) {
            state->pending_frames.pop_front();
        }
        return;
    }
    if (match_latest_by_submission) {
        state->pending_frames.clear();
        return;
    }
    const auto frame = std::find_if(
        state->pending_frames.begin(),
        state->pending_frames.end(),
        [display_time](const PendingApplicationFrame& candidate) {
            return candidate.display_time == display_time;
        });
    if (frame == state->pending_frames.end()) {
        return;
    }
    state->pending_frames.erase(state->pending_frames.begin(), std::next(frame));
}

// Native depth history follows the selected comparison route even when the
// application does not enumerate its images again after a menu switch.
// Caller holds call_mutex, mutex and the session GPU transaction lock.
HRESULT reconcile_depth_history_for_route(SwapchainState& chain) {
    if (chain.session->graphics_binding != SessionGraphicsBinding::d3d12 ||
        required_release_state(chain) != D3D12_RESOURCE_STATE_DEPTH_WRITE) return S_OK;
    if (chain.session->nvidia_new_route) {
        if (chain.d3d12_history) {
            const auto result = chain.d3d12_history->wait_for_idle();
            if (FAILED(result)) return result;
            chain.last_released_capture.reset();
            chain.d3d12_history.reset();
        }
        return S_OK;
    }
    if (chain.d3d12_history || chain.enumerated_d3d12_images.empty() ||
        (chain.create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return S_OK;
    std::vector<ID3D12Resource*> resources;
    for (const auto& image : chain.enumerated_d3d12_images) resources.push_back(image.Get());
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    const auto result = history->initialize(chain.session->d3d12_device.Get(),
        chain.session->d3d12_queue.Get(), resources, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (SUCCEEDED(result)) chain.d3d12_history = std::move(history);
    return result;
}

// Frame-call ownership is held. Drain exact old descriptors before changing
// buffer shape; reserved staging capacity covers both multipliers.
[[nodiscard]] XrResult switch_djules_pipeline(const std::shared_ptr<SessionState>& state,bool target,bool prefer_fps) {
    if(!djules_native_scheduler(*state)) return XR_SUCCESS;
    prefer_fps = target || prefer_fps;
    if(target==state->djules_triple_requested && prefer_fps==state->djules_deep_pipeline) return XR_SUCCESS;
    if(continuous_presenter_active(state)) {
        const auto result=wait_for_presenter_idle(state);
        if(XR_FAILED(result)) return result;
    }
    clear_generation_continuity(state);
    {
        std::scoped_lock content(state->presenter_content_mutex);
        std::scoped_lock lock(state->presenter_mutex);
        state->djules_triple_requested=target;
        state->djules_deep_pipeline=prefer_fps;
        ++state->djules_configuration_epoch;
        state->djules_triple_release={};
        state->three_slots.configure(djules_triple_director(*state)?3:2);
        state->application_served_serial=state->presenter_frame_serial;
        state->presenter_last_frame.reset();
    }
    if(state->fps_overlay) state->fps_overlay->reset_metrics();
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
        700,target?3:2,state->djules_deep_pipeline && !target,2);
    xrfg::embedded::register_djules_pipeline(state->control_id, prefer_fps);
    return XR_SUCCESS;
}

[[nodiscard]] XrResult poll_djules_pipeline(const std::shared_ptr<SessionState>& state) {
    if(!state->djules_pipeline_switchable || !djules_native_scheduler(*state)) return XR_SUCCESS;
    const auto now=std::chrono::steady_clock::now();
    if(now<state->djules_pipeline_poll) return XR_SUCCESS;
    state->djules_pipeline_poll=now+std::chrono::milliseconds(250);
    const auto desired=xrfg::embedded::djules_pipeline_snapshot();
    const auto target=std::pair{desired.triple_frame_gen,desired.prefer_fps};
    // One owned INI and two matching observations for both live policy controls.
    XrResult result=XR_SUCCESS;
    if(state->djules_pipeline_candidate && *state->djules_pipeline_candidate==target)
        result=switch_djules_pipeline(state,target.first,target.second);
    state->djules_pipeline_candidate=target;
    return result;
}

// Called with frame ownership and presenter content excluded. No GUI thread
// touches GPU objects; no new captures may enter during this transaction.
void apply_embedded_control(const std::shared_ptr<SessionState>& state) {
    const auto control = xrfg::embedded::snapshot();
    if (state->control_revision == control.revision) return;
    if (continuous_presenter_active(state))
        static_cast<void>(wait_for_presenter_idle(state));
    state->menu_enabled = false;
    {
        std::scoped_lock lock(state->presenter_mutex);
        state->presenter_last_frame.reset();
    }
    auto swapchains = find_swapchains(state);
    std::vector<std::unique_lock<std::mutex>> capture_locks;
    for (const auto& chain : swapchains) capture_locks.emplace_back(chain->call_mutex);
    std::scoped_lock gpu_lock(state->gpu_mutex);
    HRESULT result = S_OK;
    const auto backend = control.desired.backend == 0
        ? xrfg::D3D12OpticalFlowBackend::fidelity_fx : xrfg::D3D12OpticalFlowBackend::nvidia;
    const bool nvidia_new_route = control.desired.backend == 2;
    const auto generation_mode = static_cast<xrfg::D3D12FrameGenerationMode>(control.desired.frame_generation);
    const xrfg::D3D12NvidiaOpticalFlowOptions options{
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(control.desired.scale), control.desired.backward,
        nvidia_new_route, xrfg::flow_encoding_for_route(nvidia_new_route), nvidia_new_route};
    const bool changed = state->control_reconfigure_required || backend != state->optical_flow_backend ||
        options.preset != state->nvidia_options.preset ||
        options.input_scale != state->nvidia_options.input_scale ||
        options.bidirectional != state->nvidia_options.bidirectional ||
        generation_mode != state->frame_generation_mode ||
        nvidia_new_route != state->nvidia_new_route;
    // Enumeration is excluded by frame_call_mutex, so newly created contexts
    // also see this tuple. A partial failure remains bypass, never mixed flow.
    state->optical_flow_backend = backend;
    state->nvidia_options = options;
    state->frame_generation_mode = generation_mode;
    state->nvidia_new_route = nvidia_new_route;
    state->dlss_motion_vectors = control.desired.motion_vectors == 1 ||
        generation_mode == xrfg::D3D12FrameGenerationMode::fsr3 ||
        state->luke_afw_enabled;
    for (const auto& chain : swapchains) {
        std::unique_lock lock(chain->mutex);
        if (changed) {
            result = reconcile_depth_history_for_route(*chain);
            if (FAILED(result)) break;
        }
        // djules75 a99413d: control revisions must respect the same eligibility,
        // previous-refusal and session-budget gates as deferred generation.
        const bool eligible = chain->generation_eligible_pending &&
            !chain->generation_declined &&
            !state->generation_budget_exhausted.load(std::memory_order_acquire);
        if (!chain->frame_generation &&
            (control.desired.enabled || state->luke_afw_enabled) && eligible) {
            std::vector<ID3D11Texture2D*> sources;
            for (const auto& image : chain->enumerated_d3d11_images) sources.push_back(image.Get());
            lock.unlock();
            SwapchainEligibilityReason reason{};
            std::uint64_t detail{};
            auto candidate = state->graphics_binding == SessionGraphicsBinding::d3d11
                ? create_d3d11_frame_generation_swapchains(chain, sources, &reason, &detail)
                : create_d3d12_frame_generation_swapchains(chain, &reason, &detail);
            lock.lock();
            const std::uint64_t auxiliary =
                (static_cast<std::uint64_t>(chain->enumerated_image_count) << 32) |
                chain->create_info.arraySize;
            if (candidate) {
                chain->frame_generation = std::move(candidate);
                chain->generation_eligible_pending = false;
                lock.unlock();
                log_swapchain_eligibility(chain, SwapchainEligibilityReason::ready,
                    chain->enumerated_image_count, auxiliary);
                lock.lock();
            } else {
                chain->generation_declined = true;
                if (reason == SwapchainEligibilityReason::synthesis_initialize_failed)
                    result = static_cast<HRESULT>(detail);
                lock.unlock();
                log_swapchain_eligibility(chain, reason, detail, auxiliary);
                lock.lock();
            }
        }
        if (chain->frame_generation && chain->frame_generation->synthesizer) {
            auto& synthesis = chain->frame_generation->synthesizer;
            result = synthesis->wait_for_idle();
            if (SUCCEEDED(result) && changed) {
                result = synthesis->reconfigure(backend, options,
                    state->luke_afw_enabled
                        ? xrfg::D3D12FrameGenerationMode::ofxr
                        : generation_mode);
            }
        }
        if (SUCCEEDED(result) && chain->frame_generation &&
            chain->frame_generation->afw_temporal_synthesizer) {
            auto& temporal = chain->frame_generation->afw_temporal_synthesizer;
            result = temporal->wait_for_idle();
            if (SUCCEEDED(result) && changed) {
                result = temporal->reconfigure(
                    backend, options, xrfg::D3D12FrameGenerationMode::ofxr);
            }
        }
        if (SUCCEEDED(result) && chain->frame_generation &&
            chain->frame_generation->afw_final_history) {
            result = chain->frame_generation->afw_final_history->invalidate();
        }
        if (SUCCEEDED(result) && chain->d3d12_history) result = chain->d3d12_history->wait_for_idle();
        if (SUCCEEDED(result) && chain->frame_generation && chain->frame_generation->d3d11_interop)
            result = chain->frame_generation->d3d11_interop->wait_for_idle();
        // This runs in EndFrame, AFTER the application's Release. The history
        // and its current capture survive backend reconfiguration; only the
        // previous synthesis anchor is retired above. Keep this frame's owned
        // capture and matching guides so the drained director can prime the
        // new route. Erasing them here hands a capture-less job to the worker
        // and turns an ordinary menu switch into a fatal presenter failure.
        if (FAILED(result)) break;
    }
    // OptiScaler sees the desired menu value during its earlier DLSS evaluate.
    // A disable stops new publications there. Keep the private snapshot slots
    // alive: the DLSS command list that recorded their CopyResource operations
    // is owned and submitted by the game, so an OFXR fence cannot prove that an
    // open/not-yet-submitted producer list has finished referencing them.
    // Re-enable resets temporal ownership while reusing the retained slots.
    if (changed || !state->dlss_motion_vectors) {
        std::scoped_lock guide_lock(state->luke_guide_mutex);
        state->luke_afw_released_eye.reset();
        state->luke_afw_last_guide_publication = {};
        state->luke_afw_last_evaluated_publication = 0;
        state->luke_afw_last_pair.reset();
        state->luke_frame_guide_resolved = false;
        state->luke_last_consumed_guide_publication = 0;
        state->luke_frame_motion_vectors.reset();
    }
    {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        if (SUCCEEDED(result)) {
            state->optical_flow_backend = backend;
            state->nvidia_options = options;
        }
    }
    state->control_revision = control.revision;
    state->control_reconfigure_required = FAILED(result);
    state->menu_enabled = SUCCEEDED(result) && control.desired.enabled;
    ++state->djules_configuration_epoch;
    { std::scoped_lock lock(state->presenter_mutex);
      state->three_slots.configure(djules_triple_director(*state)?3:2);
      state->djules_triple_release={}; }

    if (state->fps_overlay) state->fps_overlay->reset_metrics();
    xrfg::embedded::applied(state->control_id, control.revision, state->menu_enabled, result);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::embedded_configuration,
        result, control.revision, optical_flow_configuration_code(backend, options),
        (state->menu_enabled ? 1ULL : 0ULL) | (state->nvidia_new_route ? 2ULL : 0ULL));
}

// Correlate source-release fingerprints with submitted eye metadata, even
// when a submission retains an eye without releasing a new source image.
void capture_application_metadata(const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* info) noexcept {
    if (!state->frame_capture) return;
    try {
        const bool down=(GetAsyncKeyState(VK_CONTROL)&0x8000) &&
            (GetAsyncKeyState(VK_SHIFT)&0x8000) && (GetAsyncKeyState(VK_F10)&0x8000);
        const bool trigger=down&&!state->diagnostic_key_down;
        state->diagnostic_key_down=down;
        // No GPU mutex, queries, thread, copies or disk work outside a capture.
        if (!trigger && !state->frame_capture->busy()) return;
        std::scoped_lock gpu_lock(state->gpu_mutex);
        if(trigger) static_cast<void>(state->frame_capture->start(true));
        state->frame_capture->tick(true);
        ++state->diagnostic_app_frame;
        if (!state->frame_capture->active() || !info || !info->layers) return;
        for (std::uint32_t l=0;l<info->layerCount;++l) {
            if (!info->layers[l] || info->layers[l]->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[l]);
            for (std::uint32_t eye=0;eye<projection->viewCount;++eye) {
                const auto& v=projection->views[eye];
                const auto chain=find_swapchain(v.subImage.swapchain);
                std::uint64_t serial=0,capture=0;
                if(chain) { std::scoped_lock lock(chain->mutex);
                    if(chain->last_released_capture) serial=chain->last_released_capture->serial;
                    capture=chain->last_diagnostic_capture; }
                std::ostringstream row;row<<std::setprecision(9);
                row<<"{\"kind\":\"view\",\"app_frame\":"<<state->diagnostic_app_frame<<",\"qpc\":"<<xrfg::FrameCapture::qpc()
                   <<",\"display_time\":"<<info->displayTime<<",\"layer\":"<<l<<",\"eye\":"<<eye
                   <<",\"space\":\""<<handle_value(projection->space)<<"\",\"swapchain\":\""<<handle_value(v.subImage.swapchain)
                   <<"\",\"serial\":"<<serial<<",\"capture\":"<<capture<<",\"slice\":"<<v.subImage.imageArrayIndex
                   <<",\"rect\":["<<v.subImage.imageRect.offset.x<<','<<v.subImage.imageRect.offset.y<<','
                   <<v.subImage.imageRect.extent.width<<','<<v.subImage.imageRect.extent.height<<"],\"position\":["
                   <<v.pose.position.x<<','<<v.pose.position.y<<','<<v.pose.position.z<<"],\"orientation\":["
                   <<v.pose.orientation.x<<','<<v.pose.orientation.y<<','<<v.pose.orientation.z<<','<<v.pose.orientation.w
                   <<"],\"fov\":["<<v.fov.angleLeft<<','<<v.fov.angleRight<<','<<v.fov.angleUp<<','<<v.fov.angleDown
                   <<"],\"upstream_engine_id\":null}";
                state->frame_capture->record(row.str());
            }
        }
    } catch (...) {}
}

struct LukePairDecision {
    enum class Action { normal, accept, drop } action{Action::normal};
    std::optional<xrfg::LukeRenderIdentity> identity;
    bool reset{};
};

LukePairDecision luke_pair_decision(std::span<const ProjectionResourceMapping> mappings) {
    LukePairDecision decision;
    bool any_identity = false, complete = true, skipped = false, all_admitted = true;
    bool captures_unchanged = true;
    for (const auto& mapping : mappings) {
        auto chain = find_swapchain(mapping.application_swapchain);
        if (!chain) { complete = false; continue; }
        std::scoped_lock lock(chain->mutex);
        skipped |= chain->luke_released_duplicate;
        const auto identity = chain->luke_released_identity;
        any_identity |= identity.has_value();
        if (!identity) {
            decision.reset |= chain->luke_admitted_identity.has_value();
            complete = false; continue;
        }
        if (!decision.identity) decision.identity = identity;
        else if (decision.identity != identity) complete = false;
        if (!chain->last_released_capture) complete = false;
        all_admitted &= chain->luke_admitted_identity == identity;
        captures_unchanged &= chain->last_released_capture &&
            chain->last_released_capture->serial == chain->luke_admitted_capture_serial;
        if (chain->luke_admitted_identity &&
            (identity->object != chain->luke_admitted_identity->object ||
             identity->render < chain->luke_admitted_identity->render)) decision.reset = true;
    }
    if (!any_identity && !skipped) return decision;
    if (!complete || skipped || all_admitted) {
        decision.action = LukePairDecision::Action::drop;
        decision.reset |= !complete || !all_admitted || !captures_unchanged;
    } else decision.action = LukePairDecision::Action::accept;
    return decision;
}

void commit_luke_pair(std::span<const ProjectionResourceMapping> mappings,
    std::optional<xrfg::LukeRenderIdentity> identity) {
    for (const auto& mapping : mappings) {
        if (auto chain = find_swapchain(mapping.application_swapchain)) {
            std::scoped_lock lock(chain->mutex);
            chain->luke_admitted_identity = identity;
            chain->luke_admitted_capture_serial = identity && chain->last_released_capture
                ? chain->last_released_capture->serial : 0;
        }
    }
}

// projection_view_rect, for every view whose rectangle or array index
// differs from the last record for its swapchain.
void log_projection_view_rects(
    SessionState& state, const ProjectionSnapshot& snapshot) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().enabled()) {
            return;
        }
        std::scoped_lock lock(state.logged_view_rects_mutex);
        for (const auto& layer : snapshot.layers) {
            for (std::size_t view_index = 0; view_index < layer.views.size(); ++view_index) {
                const XrSwapchainSubImage& sub_image = layer.views[view_index].subImage;
                const std::array<std::uint64_t, 3> rect{
                    static_cast<std::uint64_t>(sub_image.imageArrayIndex),
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sub_image.imageRect.offset.x)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.offset.y),
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sub_image.imageRect.extent.width)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.extent.height)};
                if (view_index >= 4) continue;
                if (!state.logged_view_rects.contains(sub_image.swapchain) && state.logged_view_rects.size() >= 64) continue;
                auto& last = state.logged_view_rects[sub_image.swapchain][view_index];
                if (last && *last == rect) {
                    continue;
                }
                last = rect;
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::projection_view_rect,
                    static_cast<std::int64_t>((rect[0] << 8) | (view_index & 0xFFU)),
                    handle_value(sub_image.swapchain),
                    rect[1],
                    rect[2]);
            }
        }
    } catch (...) {
    }
}


void log_video_memory_periodically(SessionState& state) noexcept {
    if (!xrfg::bridge_flight_logger().enabled()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < state.video_memory_poll_at) {
        return;
    }
    // Every second for the first minute: what a runtime or the game does
    // right after arming lands inside the first few seconds.
    if (state.video_memory_first_poll == std::chrono::steady_clock::time_point{}) {
        state.video_memory_first_poll = now;
    }
    state.video_memory_poll_at = now +
        (now - state.video_memory_first_poll < std::chrono::seconds(60)
             ? std::chrono::seconds(1)
             : std::chrono::seconds(5));
    log_video_memory(state, VideoMemoryStage::periodic, 0);
}

// A bridged depth swapchain on D3D11BridgePath::depth_private has runtime
// images nothing ever writes, so depth information naming it must not reach
// the runtime. The application's end-frame data is read-only, so the
// projection layers whose views carry such a node are copied with the node
// unlinked; every other layer pointer is forwarded as it came. The storage
// lives for the xrEndFrame call, which is as long as anything downstream
// reads the application's data: an owned frame copies what it keeps, a
// borrowed one is waited for before this call returns.
struct PrivateDepthStrip {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::deque<ProjectionLayerCopy> projections;
};

[[nodiscard]] bool names_private_depth_swapchain(const void* next) noexcept {
    const auto* header = static_cast<const XrBaseInStructure*>(next);
    if (header == nullptr ||
        header->type != XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
        return false;
    }
    const auto* depth = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(header);
    const auto swapchain_state = find_swapchain(depth->subImage.swapchain);
    if (!swapchain_state) {
        return false;
    }
    std::scoped_lock lock(swapchain_state->mutex);
    return swapchain_state->private_depth;
}

// Returns the end info to submit: the application's own unless a projection
// view's chain starts with depth information naming a private depth
// swapchain, in which case a copy without those nodes. Only a leading run
// of such nodes is unlinked: the chain's other structures are of types this
// layer does not know the size of, so a node behind one of them stays.
[[nodiscard]] const XrFrameEndInfo* strip_private_depth(
    const SessionState& state,
    const XrFrameEndInfo* end_info,
    PrivateDepthStrip& storage) {
    if (end_info == nullptr || end_info->layers == nullptr ||
        !state.has_private_depth_swapchain.load(std::memory_order_acquire)) {
        return end_info;
    }
    bool any = false;
    for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount && !any; ++layer_index) {
        const auto* layer = end_info->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        for (std::uint32_t view_index = 0;
             projection->views != nullptr && view_index < projection->viewCount; ++view_index) {
            if (names_private_depth_swapchain(projection->views[view_index].next)) {
                any = true;
                break;
            }
        }
    }
    if (!any) {
        return end_info;
    }
    storage.info = *end_info;
    storage.layers.assign(end_info->layers, end_info->layers + end_info->layerCount);
    for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount; ++layer_index) {
        const auto* layer = end_info->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->views == nullptr) {
            continue;
        }
        bool affected = false;
        for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
            affected = affected ||
                names_private_depth_swapchain(projection->views[view_index].next);
        }
        if (!affected) {
            continue;
        }
        ProjectionLayerCopy& copy = storage.projections.emplace_back();
        copy.layer_index = layer_index;
        copy.layer = *projection;
        copy.views.assign(projection->views, projection->views + projection->viewCount);
        for (XrCompositionLayerProjectionView& view : copy.views) {
            while (names_private_depth_swapchain(view.next)) {
                view.next = static_cast<const XrBaseInStructure*>(view.next)->next;
            }
        }
        copy.layer.views = copy.views.data();
        storage.layers[layer_index] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&copy.layer);
    }
    storage.info.layers = storage.layers.data();
    return &storage.info;
}


XrResult layer_end_frame_impl(
    XrSession session,
    const XrFrameEndInfo* end_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->end_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    const std::uint32_t diagnostic_capture_frame =
        xrfg::two_frame_capture_begin_end_frame();
    xrfg::bridge_flight_logger().refresh_settings();
    struct CaptureFrameScope {
        std::uint32_t frame;
        ~CaptureFrameScope() {
            xrfg::two_frame_capture_finish_end_frame(frame);
        }
    } diagnostic_capture_scope{diagnostic_capture_frame};

    std::optional<std::uint32_t> luke_afw_released_eye;
    if ((state->graphics_binding_capabilities & 8ULL) != 0) {
        std::scoped_lock guide_lock(state->luke_guide_mutex);
        luke_afw_released_eye = state->luke_afw_released_eye;
        state->luke_afw_released_eye.reset();
        state->luke_frame_guide_resolved = false;
        state->luke_frame_motion_vectors.reset();
    }

    // The inline second cycle releases this lock before the next app wait.
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    observe_application_frame_thread(*state, true);
    if (presenter_forbidden(*state)) {
        state->pipelined_presenter_start_requested = state->presenter_start_requested = false;
        state->pipelined_presenter_mode = false;
    }
    mark_application_release(state.get());
    { std::scoped_lock lock(state->binding_join_mutex);
      state->app_end_frame_release_value = state->app_release_counter; }
    PrivateDepthStrip private_depth_storage;
    end_info = strip_private_depth(*state, end_info, private_depth_storage);
    if (state->steamvr_delivery && end_info && end_info->layerCount &&
        state->application_frames_submitted < 100 &&
        ++state->application_frames_submitted == 100)
        state->steamvr_delivery->mark_established();
    log_compositor_delivery(*state);
    const auto application_end_now = std::chrono::steady_clock::now();
    const bool frame_had_overlapping_wait =
        state->application_frame_has_overlapping_wait;
    const bool application_frame_in_progress =
        state->application_frame_in_progress;
    const bool pipelined_presenter_mode = state->pipelined_presenter_mode;
    // Both promotions hand the presenter its first frame at the top of an
    // xrEndFrame, before the inline second cycle runs, so the handoff never
    // overlaps a frame this thread has already submitted.
    const bool pipelined_presenter_start_requested =
        state->pipelined_presenter_start_requested ||
        state->presenter_start_requested;
    const bool consume_in_submission_order =
        pipelined_presenter_mode || frame_had_overlapping_wait;
    const bool match_latest_by_submission =
        application_frame_in_progress && !consume_in_submission_order;
    state->application_frame_in_progress = false;
    state->application_frame_has_overlapping_wait = false;
    if (!frame_had_overlapping_wait && !pipelined_presenter_mode) {
        state->pipelined_wait_streak = 0;
    }
    const bool use_continuous_presenter = continuous_presenter_active(state);
    if (use_continuous_presenter &&
        (end_info == nullptr || end_info->type != XR_TYPE_FRAME_END_INFO)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::unique_lock<std::mutex> presenter_content_lock;
    // Drains the presenter and takes the content lock. Used by the paths that
    // hand the runtime the application's own composition unchanged: they have
    // no private output of their own, so the retained repeat has to be settled
    // before they submit.
    const auto enter_presenter_exclusive = [&]() -> XrResult {
        if (!use_continuous_presenter || presenter_content_lock.owns_lock()) {
            return XR_SUCCESS;
        }
        const XrResult idle_result = wait_for_presenter_idle(state);
        if (XR_FAILED(idle_result)) {
            return idle_result;
        }
        presenter_content_lock =
            std::unique_lock<std::mutex>(state->presenter_content_mutex);
        return XR_SUCCESS;
    };

    apply_embedded_control(state);
    const auto pipeline_result=poll_djules_pipeline(state);
    if(XR_FAILED(pipeline_result)) return pipeline_result;
    const auto afw_control = xrfg::embedded::afw_snapshot();
    const bool afw_selected = state->luke_afw_enabled &&
        afw_control.desired.mode != 0;
    if (state->luke_afw_previous_selected &&
        *state->luke_afw_previous_selected != afw_selected) {
        // The two generators have distinct temporal anchors. A menu switch
        // must not pair either generator with the other one's last frame.
        clear_generation_continuity(state);
        {
            std::scoped_lock guide_lock(state->luke_guide_mutex);
            state->luke_afw_last_pair.reset();
            state->luke_afw_last_evaluated_publication = 0;
        }
        for (const auto& chain : find_swapchains(state)) {
            std::shared_ptr<xrfg::D3D12SwapchainHistory> final_history;
            {
                std::scoped_lock chain_lock(chain->mutex);
                if (chain->frame_generation)
                    final_history = chain->frame_generation->afw_final_history;
            }
            if (final_history) {
                std::scoped_lock gpu_lock(state->gpu_mutex);
                static_cast<void>(final_history->invalidate());
            }
        }
    }
    state->luke_afw_previous_selected = afw_selected;
    capture_application_metadata(state,end_info);
    const bool manually_disarmed = state->manual_control.stop_requested();
    const bool presenter_route_mismatch = use_continuous_presenter &&
        (three_slot_generation_session(state) != state->three_slot_director ||
         fsr_two_slot_session(state) != state->fsr_two_slot_presenter ||
         queued_nvidia_session(state) != state->queued_nvidia_presenter);
    if (manually_disarmed && !state->manual_stop_applied) {
        // The existing queue has been drained before taking the content lock.
        // Do not unload hooks/resources, cancel submitted GPU work, or hand
        // overlapping frame calls to a different owner mid-cycle.
        clear_generation_continuity(state);
        {
            std::scoped_lock presenter_lock(state->presenter_mutex);
            state->presenter_last_frame.reset();
        }
        state->manual_stop_applied = true;
        if (state->fps_overlay) state->fps_overlay->suspend();
    }
    if (state->fps_overlay && !manually_disarmed) {
        std::scoped_lock gpu_lock(state->gpu_mutex);
        state->fps_overlay->application_frame(end_info);
    }

    const auto submit_borrowed_to_presenter = [&]() -> XrResult {
        auto request = enqueue_presenter_submission(state, nullptr, end_info);
        if (presenter_content_lock.owns_lock()) {
            presenter_content_lock.unlock();
        }
        return wait_for_presenter_submission(state, request);
    };
    if (presenter_forbidden(*state) && use_continuous_presenter) {
        // A loop may change threads after promotion. Finish this virtual app
        // frame through its existing physical owner before draining that owner;
        // a direct downstream EndFrame here would have no matching BeginFrame.
        const auto result = submit_borrowed_to_presenter();
        if (XR_SUCCEEDED(result) && end_info)
            consume_application_frame(state, end_info->displayTime,
                consume_in_submission_order, match_latest_by_submission);
        stop_continuous_presenter(state);
        clear_generation_continuity(state);
        return result;
    }
    const auto start_requested_pipelined_presenter =
        [&](std::shared_ptr<GeneratedFrameEndInfo> seed_frame) -> XrResult {
            if (!pipelined_presenter_start_requested) {
                return XR_SUCCESS;
            }
            if (three_slot_generation_session(state)) {
                // One ownership rendezvous at promotion, not a steady-state
                // producer gate. The director cannot inherit an unfinished
                // inline prime/deferred endpoint: its first worker submission
                // would otherwise flush that copy and immediately see BUSY.
                // No GPU/presenter/swapchain call lock is held during this wait.
                if (FAILED(wait_for_previous_session_synthesis(state))) {
                    return XR_ERROR_RUNTIME_FAILURE;
                }
            }
            if (!start_continuous_presenter(
                    state,
                    std::move(seed_frame),
                    true)) {
                return XR_ERROR_RUNTIME_FAILURE;
            }
            state->pipelined_presenter_start_requested = false;
            state->presenter_start_requested = false;
            return wait_for_presenter_idle(state);
        };
    const auto bypass_generation =
        [&](GenerationPrepareReason reason) -> XrResult {
            if (state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3)
                state->fsr_cadence.bypass(static_cast<std::int64_t>(reason));
            update_luke_application_clock(*state, false, false);
            // A borrowed frame advances source capture serials without advancing
            // synthesis history. Retire the old anchor BEFORE draining/taking
            // the content lock, so the next valid frame primes instead of
            // pairing across a missing serial and entering quarantine.
            bool has_anchor;
            { std::scoped_lock lock(state->mutex); has_anchor = state->previous_projection.has_value(); }
            if (has_anchor) clear_generation_continuity(state);
            const XrResult exclusive_result = enter_presenter_exclusive();
            if (XR_FAILED(exclusive_result)) {
                return exclusive_result;
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::generation_prepare,
                static_cast<std::int64_t>(reason),
                0,
                0,
                0);
            const auto end_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::downstream_first_end_frame,
                handle_value(session),
                end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
                end_info ? end_info->layerCount : 0);
            const XrResult end_result = use_continuous_presenter
                ? submit_borrowed_to_presenter()
                : with_runtime_entry(state, [&] {
                      join_runtime_queue_to_application(state.get(), state->app_end_frame_release_value);
                      return state->fps_overlay
                          ? state->fps_overlay->end_frame(end_info, false)
                          : state->dispatch->end_frame(session, end_info);
                  });
            xrfg::bridge_flight_logger().end(
                end_token,
                xrfg::BridgeFlightOperation::downstream_first_end_frame,
                end_result,
                0,
                0,
                0);
            if (XR_SUCCEEDED(end_result) && end_info != nullptr) {
                consume_application_frame(
                    state,
                    end_info->displayTime,
                    consume_in_submission_order,
                    match_latest_by_submission);
            }
            if (XR_SUCCEEDED(end_result) && !use_continuous_presenter) {
                const XrResult start_result =
                    start_requested_pipelined_presenter(nullptr);
                if (XR_FAILED(start_result)) {
                    return start_result;
                }
            }
            if (use_continuous_presenter && (!pipelined_presenter_mode || presenter_route_mismatch)) {
                stop_continuous_presenter(state);
                state->pipelined_presenter_start_requested = false;
                state->presenter_start_requested = false;
                if (pipelined_presenter_mode && !presenter_forbidden(*state)) {
                    // The application's outstanding Wait/Begin tickets are virtual.
                    // Finish the old physical cycle above, join its owner, and
                    // start the selected owner before returning this boundary.
                    // Even Off keeps a borrowed-frame presenter for a pipelined
                    // app: those tickets cannot become unmatched runtime Begins.
                    if (!start_continuous_presenter(state, nullptr, true))
                        return XR_ERROR_RUNTIME_FAILURE;
                } else {
                    state->pipelined_presenter_mode = false;
                    if (state->application_wait_pending_begin &&
                        !state->runtime_frame_waited_unbegun)
                        state->application_begin_needs_wait = true;
                }
                std::scoped_lock lock(state->mutex);
                state->steamvr_throttled_wait_streak = 0;
                // Both promotion routes start over, or the demotion would be
                // undone by the next frame that measures the same runtime.
                state->unpaced_wait_streak = 0;
            }
            return end_result;
        };

    // Menu backend changes can happen after a presenter already exists. End
    // this virtual application frame through its current owner before stopping
    // it; the next promotion selects the appropriate backend's director. Never
    // hand a synthesis job to an old presenter with no worker thread.
    if (use_continuous_presenter) {
        const bool desired_director = three_slot_generation_session(state);
        bool route_changed = false;
        {
            std::scoped_lock lock(state->presenter_mutex);
            route_changed = desired_director != state->three_slot_director ||
                fsr_two_slot_session(state) != state->fsr_two_slot_presenter ||
                queued_nvidia_session(state) != state->queued_nvidia_presenter;
        }
        if (route_changed)
            return bypass_generation(GenerationPrepareReason::presenter_route_changed);
    }
    bool generation_cooling_down = false;
    bool structural_quarantine_active = false;
    {
        std::scoped_lock lock(state->mutex);
        if (state->generation_resume_wall_time !=
            std::chrono::steady_clock::time_point{}) {
            if (application_end_now >= state->generation_resume_wall_time) {
                state->generation_resume_wall_time = {};
            } else {
                generation_cooling_down = true;
                structural_quarantine_active = true;
            }
        }
        if (state->generation_resume_display_time != 0) {
            if (end_info != nullptr &&
                end_info->displayTime >= state->generation_resume_display_time) {
                state->generation_resume_display_time = 0;
            } else {
                generation_cooling_down = true;
            }
        }
    }
    if (manually_disarmed ||
        ((generation_cooling_down || !state->menu_enabled) &&
          !afw_selected)) {
        return bypass_generation(manually_disarmed
                ? GenerationPrepareReason::manual_disarmed
                : structural_quarantine_active
                    ? GenerationPrepareReason::structural_quarantine_active
                    : GenerationPrepareReason::cooldown_active);
    }

    ProjectionSnapshot current_snapshot{};
    const bool has_projection =
        capture_projection_snapshot(end_info, &current_snapshot);
    current_snapshot.app_release_value = state->app_end_frame_release_value;
    ProjectionMappingResult resource_mappings{};
    if (has_projection) {
        resource_mappings = build_projection_resource_mappings(current_snapshot);
        log_projection_view_rects(*state, current_snapshot);
    } else {
        resource_mappings.reason = ProjectionMappingReason::no_projection_views;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::projection_mapping,
        static_cast<std::int64_t>(resource_mappings.reason),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        (static_cast<std::uint64_t>(current_snapshot.layers.size()) << 32) |
            resource_mappings.mappings.size(),
        resource_mappings.detail);
    // The first frame that names a swapchain as a projection view is where the
    // deferred generation resources are taken - but taking them here, between
    // the application's xrEndFrame and the submission that follows it, puts
    // the cost of creating them on the frame's own deadline. Synthesizer
    // initialization measured 80 ms and 46 ms in one captured session, which
    // is seven display periods: the submission left 92 ms after the
    // application entered xrEndFrame, the runtime rejected its display time
    // with XR_ERROR_TIME_INVALID, and the failure quarantined generation for a
    // second. Generation then engaged several seconds into the session instead
    // of immediately.
    //
    // So the frame that triggers arming passes through, and the arming happens
    // after its submission has gone out. It costs one frame at world load
    // rather than a rejected submission and a second of outage, and the next
    // frame generates.
    const std::span<const ProjectionResourceMapping> projection_mappings(
        resource_mappings.mappings.data(),
        resource_mappings.mappings.size());
    const bool generation_arming_pending =
        resource_mappings.ready() &&
        projection_frame_generation_pending(state, projection_mappings);
    if (!resource_mappings.ready() || generation_arming_pending) {
        clear_generation_continuity(state);
        if (!resource_mappings.ready() &&
            resource_mappings.reason !=
                ProjectionMappingReason::no_projection_views) {
            schedule_generation_quarantine(
                state,
                GenerationQuarantineReason::projection_mapping_failed,
                (static_cast<std::uint64_t>(resource_mappings.reason) << 56) |
                    (resource_mappings.detail & 0x00FFFFFFFFFFFFFFULL));
        }
        const XrResult passthrough_result =
            bypass_generation(GenerationPrepareReason::empty_mappings);
        if (!generation_arming_pending) {
            return passthrough_result;
        }
        // The application's frame is already downstream; nothing this costs is
        // on its deadline any more.
        if (!ensure_projection_frame_generation(state, projection_mappings)) {
            // A refusal means the runtime has no swapchains left for the
            // application either. A queued submission names a private
            // swapchain, so the presenter must be idle and its content lock
            // held before one is destroyed.
            const XrResult exclusive_result = enter_presenter_exclusive();
            if (XR_FAILED(exclusive_result)) {
                return exclusive_result;
            }
            release_session_generation_budget(state);
        }
        return passthrough_result;
    }
    const std::optional<PendingApplicationFrame> application_frame =
        latest_pending_application_frame(
            state,
            current_snapshot.display_time,
            consume_in_submission_order,
            match_latest_by_submission);
    const std::optional<XrDuration> application_display_period =
        application_frame
            ? std::optional<XrDuration>{application_frame->display_period}
            : std::nullopt;
    const bool latest_application_frame = application_frame.has_value();
    const std::uint64_t frame_serial = application_frame
        ? application_frame->serial : 0;
    if (state->luke_afw_enabled) {
        // result 2: application end matched to the wait ticket. c packs the
        // F10 capture index and whether the matching ticket was found.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            2, frame_serial,
            static_cast<std::uint64_t>(current_snapshot.display_time),
            (static_cast<std::uint64_t>(diagnostic_capture_frame) << 32) |
                (latest_application_frame ? 1ULL : 0ULL));
        xrfg::two_frame_capture_timeline(
            "frame", 2, frame_serial,
            static_cast<std::uint64_t>(current_snapshot.display_time),
            (static_cast<std::uint64_t>(diagnostic_capture_frame) << 32) |
                (latest_application_frame ? 1ULL : 0ULL));
        log_luke_projection_poses(end_info, frame_serial, 1);
        const bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11_down && !state->luke_f11_down) {
            // result 9 marks the observed state. No F10 GPU capture, fence
            // wait, pose change, or runtime-slot change occurs here.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_frame_contract,
                9, frame_serial,
                static_cast<std::uint64_t>(current_snapshot.display_time),
                0);
        }
        state->luke_f11_down = f11_down;
    }
    std::optional<ProjectionSnapshot> previous_snapshot;
    {
        std::scoped_lock lock(state->mutex);
        previous_snapshot = state->previous_projection;
    }
    const bool snapshots_compatible =
        !previous_snapshot ||
        projection_snapshots_compatible(*previous_snapshot, current_snapshot);
    bool metadata_pairable =
        latest_application_frame && previous_snapshot && snapshots_compatible;
    const bool use_three_slot = use_continuous_presenter && three_slot_generation_session(state);
    if (previous_snapshot && !snapshots_compatible) {
        if (!projection_resource_layout_compatible(
                *previous_snapshot,
                current_snapshot)) {
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::projection_changed,
                static_cast<std::uint64_t>(current_snapshot.display_time));
            return bypass_generation(
                GenerationPrepareReason::structural_quarantine_active);
        }
        // Recenter/reference-space and layer-flag changes are safe to prime
        // immediately; do not turn an ordinary pose-space transition into a
        // one-second outage.
        clear_generation_continuity(state);
    }
    if (state->luke_afw_enabled) {
        const bool f6_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
        if (f6_down && !state->luke_afw_f6_down) {
            const bool accepted = xrfg::embedded::toggle_afw_debug();
            const auto debug = xrfg::embedded::afw_snapshot().desired.debug;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                accepted ? (debug ? 1 : 0) : E_FAIL,
                24, debug ? 1 : 0, 0);
        }
        state->luke_afw_f6_down = f6_down;
    }
    // AFW Off does not override OFXR's independent menu switch. With both
    // switches Off, the early bypass above presents Luke's original AER frame.
    if (state->luke_afw_enabled) {
        const std::uint64_t flags =
            (afw_selected ? 1ULL : 0ULL) |
            (state->menu_enabled ? 2ULL : 0ULL) |
            (state->menu_enabled && !generation_cooling_down
                ? 4ULL : 0ULL) |
            (generation_cooling_down ? 8ULL : 0ULL) |
            (static_cast<std::uint64_t>(afw_control.desired.mode) << 8) |
            (static_cast<std::uint64_t>(state->frame_generation_mode) << 16);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            6, frame_serial, flags, afw_control.revision);
    }
    PreparedProjectionFrame afw_produced;
    GeneratedFrameEndInfo afw_terminal_submission;
    ProjectionSnapshot afw_terminal_snapshot;
    std::shared_ptr<const CompletedAfwFrame> completed_afw;
    if (afw_selected) {
        std::vector<PostAfwFrameInput> inputs;
        // ONE producer, before selecting the consumer. In particular no OFXR
        // worker/director capacity wait precedes AFW production.
        const bool capture_temporal_input = state->menu_enabled &&
            !generation_cooling_down &&
            state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr;
        afw_produced = prepare_luke_afw_frame(state, current_snapshot,
            projection_mappings, luke_afw_released_eye, frame_serial,
            capture_temporal_input, inputs);
        if (state->fps_overlay)
            state->fps_overlay->set_afw_active(afw_produced.kind != PreparedGenerationKind::none);
        xrfg::embedded::report_afw_prepare(static_cast<std::int64_t>(afw_produced.reason));
        if (afw_produced.kind == PreparedGenerationKind::none)
            return bypass_generation(afw_produced.reason);
        // Build the validated AFW->XR output once, before selecting its sink.
        // Direct delivery and OFXR must consume this very same projection,
        // rather than independently remapping the original AER submission.
        if (!build_afw_terminal_submission(end_info, current_snapshot, afw_produced,
                afw_terminal_submission, afw_terminal_snapshot))
            return bypass_generation(GenerationPrepareReason::generated_end_info_failed);
        if (!inputs.empty()) {
            completed_afw = freeze_completed_afw_frame(state, &afw_terminal_submission.info,
                afw_terminal_snapshot, std::move(inputs), frame_serial);
            if (!completed_afw)
                return bypass_generation(GenerationPrepareReason::generated_end_info_failed);
        }
    }
    if (use_three_slot) {
        if (!completed_afw && !composition_can_be_owned(state, end_info))
            return bypass_generation(GenerationPrepareReason::presenter_unsafe_composition);
        // In AFW the peer is reconstructed, not a second AER source release.
        // The original Luke pair filter cannot classify that completed pair.
        const auto luke = afw_selected ? LukePairDecision{} : luke_pair_decision(projection_mappings);
        update_luke_application_clock(*state, afw_selected || luke.identity.has_value() || luke.action == LukePairDecision::Action::drop,
            luke.action == LukePairDecision::Action::drop);
        if (luke.reset) {
            clear_generation_continuity(state);
            metadata_pairable = false;
        }
        if (luke.action == LukePairDecision::Action::drop && latest_application_frame) {
            { std::scoped_lock lock(state->presenter_mutex);
              if (XR_FAILED(state->presenter_failure)) return state->presenter_failure;
              if (state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING; }
            // This is a virtual app cycle; the director alone owns runtime
            // Wait/Begin/End. No borrowed repeat and no new synthesis job.
            consume_application_frame(state, current_snapshot.display_time,
                                      consume_in_submission_order,
                                      match_latest_by_submission);
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
                luke.reset ? 2 : 1, luke.identity ? luke.identity->render : ~0ULL,
                luke.identity ? luke.identity->object : 0, current_snapshot.display_time);
            return XR_SUCCESS;
        }
        if (luke.action == LukePairDecision::Action::drop)
            return bypass_generation(GenerationPrepareReason::empty_mappings);
        current_snapshot.luke_identity = luke.identity;
        if (afw_selected) {
            if (!completed_afw) {
                // Held pair: no fresh real/synthetic job and no borrowed source.
                update_luke_application_clock(*state, true, false);
                consume_application_frame(state, current_snapshot.display_time,
                    consume_in_submission_order, match_latest_by_submission);
                // Keep the existing duplicate-cycle bound, but put its wait
                // at the end of production, never in the next game's WaitFrame.
                std::unique_lock lock(state->presenter_mutex);
                const auto period = std::clamp<XrDuration>(
                    state->presenter_frame_state.predictedDisplayPeriod, 2'000'000, 50'000'000);
                state->presenter_condition.wait_until(lock,
                    state->application_wait_returned_at + std::chrono::nanoseconds(period), [&] {
                        return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) ||
                            !state->menu_enabled || state->manual_control.stop_requested();
                    });
                return XR_FAILED(state->presenter_failure) ? state->presenter_failure
                    : state->presenter_stop_requested ? XR_ERROR_SESSION_NOT_RUNNING : XR_SUCCESS;
            }
        }
        const XrResult handoff = handoff_three_slot_frame(
            state, end_info, current_snapshot, projection_mappings, metadata_pairable,
            completed_afw);
        if (XR_SUCCEEDED(handoff)) {
            commit_luke_pair(projection_mappings, luke.identity);
            if (luke.identity)
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
                    luke.reset ? 3 : 0, luke.identity->render, luke.identity->object,
                    current_snapshot.display_time);
            consume_application_frame(state, current_snapshot.display_time,
                                      consume_in_submission_order,
                                      match_latest_by_submission);
            std::scoped_lock lock(state->mutex);
            state->previous_projection = current_snapshot;
            state->generation_failure_streak = 0;
        }
        if(XR_SUCCEEDED(handoff)) { frame_call_lock.unlock(); delay_djules_application_release(state); }
        return handoff;
    }
    if (use_continuous_presenter && !state->luke_afw_enabled) {
        const XrResult capacity = wait_for_presenter_capacity(state,
            (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) ? 0 : 1);
        if (XR_FAILED(capacity)) return capacity;
    }
    // For the remaining inline backends the synthetic is displayed one display period before the current frame,
    // so it should show the scene as it was one period before the current
    // capture - which is halfway between the two captures only when they are
    // two display periods apart, that is when the application is running at
    // exactly half the display rate. Away from that the fixed midpoint places
    // the synthetic at the wrong instant, and because the error tracks the
    // application's frame interval it changes every frame rather than being a
    // constant offset nobody would see. Alternating early and late is what
    // reads as judder.
    const float interpolation_fraction = synthetic_interpolation_fraction(
        state, previous_snapshot, current_snapshot);
    const auto mapping_span = std::span<const ProjectionResourceMapping>(
        resource_mappings.mappings.data(),
        resource_mappings.mappings.size());
    const xrfg::D3D12PresentationTransform final_color_transform{};
    PreparedProjectionFrame prepared = afw_selected
        ? completed_afw && state->menu_enabled && !generation_cooling_down &&
            state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr
            ? prepare_projection_frame(current_snapshot, mapping_span,
                metadata_pairable, interpolation_fraction, false,
                &final_color_transform, completed_afw->inputs)
            : std::move(afw_produced)
        : prepare_projection_frame(
              current_snapshot, mapping_span, metadata_pairable, interpolation_fraction, false, nullptr, {},
              synthetic_interpolation_fraction(state,previous_snapshot,current_snapshot,1));
    if (state->fps_overlay) {
        state->fps_overlay->set_afw_active(
            afw_selected && prepared.kind != PreparedGenerationKind::none);
    }

    if ((state->graphics_binding_capabilities & 8ULL) != 0) {
        for (const ProjectionResourceMapping& mapping :
             resource_mappings.mappings) {
            if (mapping.views.empty()) continue;
            const ProjectionViewReference& reference = mapping.views.front();
            if (reference.projection_index >= current_snapshot.layers.size() ||
                reference.view_index >= current_snapshot.layers[
                    reference.projection_index].views.size()) {
                continue;
            }
            const auto& view = current_snapshot.layers[
                reference.projection_index].views[reference.view_index];
            const auto swapchain_state = find_swapchain(
                mapping.application_swapchain);
            std::int64_t logical_eye = -1;
            std::uint64_t capture_serial = 0;
            if (swapchain_state) {
                std::scoped_lock lock(swapchain_state->mutex);
                if (swapchain_state->dlss_logical_eye_index) {
                    logical_eye = static_cast<std::int64_t>(
                        *swapchain_state->dlss_logical_eye_index);
                }
                if (swapchain_state->last_released_capture) {
                    capture_serial =
                        swapchain_state->last_released_capture->serial;
                }
            }
            if (diagnostic_capture_frame != 0 && capture_serial != 0) {
                xrfg::two_frame_capture_annotate_projection(
                    diagnostic_capture_frame,
                    handle_value(mapping.application_swapchain), capture_serial,
                    static_cast<std::uint64_t>(current_snapshot.display_time),
                    static_cast<std::int32_t>(reference.view_index),
                    view.subImage.imageArrayIndex,
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.offset.x)),
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.offset.y)),
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.extent.width)),
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.extent.height)));
            }
            // result=logical eye; a=display time; b=application swapchain;
            // c=(projection index, view index, array slice, capture serial).
            const std::uint64_t packed_view =
                ((static_cast<std::uint64_t>(reference.projection_index) &
                  0xffULL) << 56) |
                ((static_cast<std::uint64_t>(reference.view_index) &
                  0xffULL) << 48) |
                ((static_cast<std::uint64_t>(view.subImage.imageArrayIndex) &
                  0xffULL) << 40) |
                (capture_serial & 0xffffffffffULL);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_projection_contract,
                logical_eye,
                static_cast<std::uint64_t>(current_snapshot.display_time),
                handle_value(mapping.application_swapchain),
                packed_view);
        }
    }
    if (use_continuous_presenter && !state->luke_afw_enabled) {
        presenter_content_lock =
            std::unique_lock<std::mutex>(state->presenter_content_mutex);
    }
    GenerationPrepareReason prepare_reason = prepared.reason;
    XrSwapchain failed_prepare_swapchain = prepared.failed_swapchain;
    if (!afw_selected &&
        prepared.kind == PreparedGenerationKind::none &&
        should_quarantine_generation_failure(prepare_reason)) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::generation_prepare,
            static_cast<std::int64_t>(prepare_reason),
            handle_value(failed_prepare_swapchain),
            static_cast<std::uint64_t>(prepared.kind),
            (metadata_pairable ? 1u : 0u) |
                (latest_application_frame ? 2u : 0u));
        schedule_generation_quarantine(
            state,
            GenerationQuarantineReason::generation_prepare_failed,
            (static_cast<std::uint64_t>(prepare_reason) << 56) |
                (handle_value(failed_prepare_swapchain) &
                 0x00FFFFFFFFFFFFFFULL));
        return bypass_generation(
            GenerationPrepareReason::structural_quarantine_active);
    }

    GeneratedFrameEndInfo first_generated{};
    GeneratedFrameEndInfo current_generated{},extra_generated{};
    const XrFrameEndInfo* produced_end_info = completed_afw
        ? &completed_afw->submission->info : afw_selected ? &afw_terminal_submission.info : end_info;
    const ProjectionSnapshot& produced_snapshot = completed_afw
        ? completed_afw->snapshot : afw_selected ? afw_terminal_snapshot : current_snapshot;
    const XrFrameEndInfo* submitted_end_info = end_info;
    bool pair_ready = false;
    std::vector<ProjectionResourceDestination> current_destinations;
    std::vector<ProjectionResourceDestination> synthetic_destinations;
    current_destinations.reserve(prepared.resources.size());
    synthetic_destinations.reserve(prepared.resources.size());
    for (const PreparedProjectionResource& resource : prepared.resources) {
        XrSwapchain produced_swapchain = resource.application_swapchain;
        if (completed_afw) {
            const auto& inputs = completed_afw->inputs;
            const auto input = std::find_if(inputs.begin(), inputs.end(),
                [&](const auto& value) { return value.application_swapchain == produced_swapchain; });
            if (input == inputs.end()) return XR_ERROR_LAYER_INVALID;
            produced_swapchain = input->completed_swapchain;
        }
        current_destinations.push_back({
            produced_swapchain,
            resource.generation.current_handle,
        });
        synthetic_destinations.push_back({
            produced_swapchain,
            resource.generation.synthetic_handle,
        });
    }
    if (afw_selected && !completed_afw) {
        // OFXR off: deliver the terminal output unchanged. This exact object
        // is also the source of freeze_completed_afw_frame when OFXR is on.
        submitted_end_info = &afw_terminal_submission.info;
    } else if (prepared.kind == PreparedGenerationKind::prime) {
        if (build_generated_frame_end_info(
                produced_end_info,
                produced_snapshot,
                false,
                std::span<const ProjectionResourceDestination>(
                    current_destinations.data(), current_destinations.size()),
                &first_generated)) {
            submitted_end_info = &first_generated.info;
        } else {
            prepare_reason = GenerationPrepareReason::generated_end_info_failed;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::generated_end_info_failed);
            prepared = {};
        }
    } else if (prepared.kind == PreparedGenerationKind::pair && previous_snapshot) {
        const bool synthetic_built = build_generated_frame_end_info(
            produced_end_info,
            produced_snapshot,
            true,
            std::span<const ProjectionResourceDestination>(
                synthetic_destinations.data(), synthetic_destinations.size()),
            &first_generated);
        const bool current_built = build_generated_frame_end_info(
            produced_end_info,
            produced_snapshot,
            false,
            std::span<const ProjectionResourceDestination>(
                current_destinations.data(), current_destinations.size()),
            &current_generated);
        if (synthetic_built && current_built) {
            // The synthetic frame owns the deferred copies: they must reach
            // the queue after it has been handed to the runtime.
            if (!state->queued_nvidia_presenter)
            for (const PreparedProjectionResource& resource :
                 prepared.resources) {
                if (resource.generation.synthesizer && !resource.generation.staging_generation) {
                    first_generated.pending_current_copies.push_back(
                        resource.generation.synthesizer);
                }
            }
            submitted_end_info = &first_generated.info;
            pair_ready = true;
        } else {
            prepare_reason = GenerationPrepareReason::generated_end_info_failed;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::generated_end_info_failed);
            prepared = {};
        }
    } else if (!prepared.anchor_is_current && !prepared.preserve_previous) {
        clear_generation_continuity(state);
    }

    bool extra_ready=pair_ready && djules_triple_active(*state);
    if(extra_ready) {
        extra_ready=build_generated_frame_end_info(produced_end_info,produced_snapshot,true,
            synthetic_destinations,&extra_generated);
        if(!extra_ready) {
            enter_generation_quarantine(state,GenerationQuarantineReason::generated_end_info_failed);
            return bypass_generation(GenerationPrepareReason::generated_end_info_failed);
        }
        extra_generated.synthetic=true;
    }
    if(prepared.kind==PreparedGenerationKind::prime) attach_djules_outputs(prepared,first_generated);
    else if(pair_ready) attach_djules_outputs(prepared,current_generated,&first_generated,extra_ready?&extra_generated:nullptr);
    if (completed_afw) {
        first_generated.composition_images = completed_afw->submission->composition_images;
        current_generated.composition_images = completed_afw->submission->composition_images;
    }

    if (state->luke_afw_enabled && frame_serial != 0) {
        if (pair_ready) {
            log_luke_projection_poses(&first_generated.info, frame_serial, 2);
            log_luke_projection_poses(&current_generated.info, frame_serial, 3);
        } else if (prepared.kind == PreparedGenerationKind::prime) {
            log_luke_projection_poses(&first_generated.info, frame_serial, 3);
        }
    }

    if ((state->graphics_binding_capabilities & 8ULL) != 0 &&
        (prepared.kind == PreparedGenerationKind::prime || pair_ready)) {
        for (const ProjectionResourceDestination& destination :
             current_destinations) {
            // result=0 means the real current frame.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_output_contract,
                0,
                static_cast<std::uint64_t>(current_snapshot.display_time),
                handle_value(destination.application_swapchain),
                handle_value(destination.destination_swapchain));
        }
        if (pair_ready) {
            for (const ProjectionResourceDestination& destination :
                 synthetic_destinations) {
                // result=1 means the generated midpoint frame.
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::luke_output_contract,
                    1,
                    static_cast<std::uint64_t>(current_snapshot.display_time),
                    handle_value(destination.application_swapchain),
                    handle_value(destination.destination_swapchain));
            }
        }
    }

    std::shared_ptr<GeneratedFrameEndInfo> presenter_first_frame;
    mark_application_release(state.get());
    std::uint64_t generated_release_value = 0;
    { std::scoped_lock lock(state->binding_join_mutex);
      generated_release_value = state->app_release_counter; }
    first_generated.app_release_value = generated_release_value;
    current_generated.app_release_value = generated_release_value;
    extra_generated.app_release_value = generated_release_value;
    first_generated.synthetic = pair_ready;
    first_generated.djules_leader=extra_ready;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_current_frame,presenter_extra_frame;
    if (use_continuous_presenter &&
        (prepared.kind == PreparedGenerationKind::prime || pair_ready)) {
        presenter_first_frame =
            make_presenter_owned_frame(std::move(first_generated));
        if (pair_ready) {
            presenter_current_frame =
                make_presenter_owned_frame(std::move(current_generated));
            if(extra_ready) presenter_extra_frame=make_presenter_owned_frame(std::move(extra_generated));
        }
        if (!presenter_first_frame ||
            (pair_ready && !presenter_current_frame) || (extra_ready && !presenter_extra_frame)) {
            presenter_first_frame.reset();
            presenter_current_frame.reset();
            submitted_end_info = end_info;
            pair_ready = false;
            prepare_reason =
                GenerationPrepareReason::presenter_unsafe_composition;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::presenter_composition_failed);
            prepared = {};
        } else {
            submitted_end_info = &presenter_first_frame->info;
            if (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
                // Auxiliary HUD/quad pixels need the same ownership guarantee
                // as projection outputs while the application renders ahead.
                XrResult owned = own_composition_images(state, *presenter_first_frame);
                if (XR_SUCCEEDED(owned) && presenter_current_frame) {
                    // HUD is identical on S/B: one owned snapshot shared by
                    // both entries, not two full-resolution copies per pair.
                    for(auto& owned:{presenter_current_frame,presenter_extra_frame}) {
                        if(!owned) continue;
                        auto& current=*owned;
                        current.composition_layers = presenter_first_frame->composition_layers;
                        current.composition_images = presenter_first_frame->composition_images;
                        std::size_t auxiliary = 0;
                        for (std::size_t index = 0; index < current.layer_pointers.size(); ++index) {
                            const bool projection = std::any_of(current.projections.begin(), current.projections.end(),
                                [index](const auto& layer) { return layer.layer_index == index; });
                            if (!projection)
                                current.layer_pointers[index] = composition_layer_header(current.composition_layers.at(auxiliary++));
                        }
                    }
                }
                if (XR_FAILED(owned)) {
                    presenter_first_frame.reset();
                    presenter_current_frame.reset();
                    return bypass_generation(GenerationPrepareReason::presenter_unsafe_composition);
                }
            }
        }
    }

    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::generation_prepare,
        static_cast<std::int64_t>(prepare_reason),
        handle_value(failed_prepare_swapchain),
        static_cast<std::uint64_t>(prepared.kind),
            (metadata_pairable ? 1u : 0u) |
            (latest_application_frame ? 2u : 0u));
    if (afw_selected) {
        xrfg::embedded::report_afw_prepare(
            static_cast<std::int64_t>(prepare_reason));
        // result 3: which physical slot gets the first submission.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            3, frame_serial,
            submitted_end_info
                ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
                : 0,
            static_cast<std::uint64_t>(prepared.kind) |
                (pair_ready ? 1ULL << 8 : 0) |
                (submitted_end_info
                    ? static_cast<std::uint64_t>(submitted_end_info->layerCount)
                          << 16
                    : 0));
        xrfg::two_frame_capture_timeline(
            "frame", 3, frame_serial,
            submitted_end_info
                ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
                : 0,
            static_cast<std::uint64_t>(prepared.kind) |
                (pair_ready ? 1ULL << 8 : 0) |
                (submitted_end_info
                    ? static_cast<std::uint64_t>(submitted_end_info->layerCount)
                          << 16
                    : 0));
    }

    const auto first_end_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::downstream_first_end_frame,
        handle_value(session),
        submitted_end_info
            ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
            : 0,
        submitted_end_info ? submitted_end_info->layerCount : 0);
    const auto first_end_started = std::chrono::steady_clock::now();
    if (pair_ready && state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3)
        state->fsr_cadence.generated(state->fps_overlay &&
            state->fsr_cadence.marker_enabled());
    XrResult result = XR_SUCCESS;
    if (use_continuous_presenter) {
        if (pair_ready) {
            result = enqueue_presenter_pair(
                state,
                presenter_first_frame,
                presenter_current_frame,presenter_extra_frame);
            if (presenter_content_lock.owns_lock()) {
                presenter_content_lock.unlock();
            }
            if (XR_SUCCEEDED(result) && pipelined_presenter_mode) {
                // Let a whole pair stay outstanding. The presenter paces its
                // submissions a display period apart, so any smaller bound
                // puts one of those periods on the application's critical
                // path: waiting for the queue to empty cost both periods and
                // held it to 36/s, waiting for one cost a single period and
                // held it to 34.6/s, with 11.93 ms of every 28.9 ms frame
                // spent blocked here. What should govern the application is
                // its own virtual wait, one pair per two display periods, and
                // that only gets to act once this stops pre-empting it.
                //
                // The pair enqueued here is drained by the time the next one
                // arrives, so this bound is a runaway guard rather than part
                // of the steady-state cadence.
                if (!state->fsr_two_slot_presenter && !state->queued_nvidia_presenter)
                    result = wait_for_presenter_capacity(state, 2);
            }
        } else if (presenter_first_frame) {
            auto request = enqueue_presenter_submission(
                state,
                presenter_first_frame,
                nullptr);
            if (presenter_content_lock.owns_lock()) {
                presenter_content_lock.unlock();
            }
            result = wait_for_presenter_submission(state, request);
        } else {
            result = submit_borrowed_to_presenter();
        }
    } else {
        join_runtime_queue_to_application(state.get(), generated_release_value);
        result = with_runtime_entry(state, [&] {
            log_luke_physical_boundary(state, submitted_end_info, frame_serial, 4, &prepared);
            if(submitted_end_info==&first_generated.info &&
               !handover_djules_outputs(state.get(),first_generated.pending_outputs)) return XR_ERROR_RUNTIME_FAILURE;
            return state->fps_overlay
                ? state->fps_overlay->end_frame(submitted_end_info, pair_ready)
                : state->dispatch->end_frame(session, submitted_end_info);
        });
        log_luke_physical_boundary(state, submitted_end_info, frame_serial, 5, &prepared, result);
    }
    const auto first_end_elapsed =
        std::chrono::steady_clock::now() - first_end_started;
    xrfg::bridge_flight_logger().end(
        first_end_token,
        xrfg::BridgeFlightOperation::downstream_first_end_frame,
        result,
        static_cast<std::uint64_t>(prepared.kind),
        pair_ready ? 1u : 0u,
        latest_application_frame ? 1u : 0u);
    if (XR_FAILED(result)) {
        // A rejected display time says nothing about the generation
        // resources. No swapchain changed shape, no private image is in an
        // uncertain ownership phase - the runtime was handed a time it
        // considers past, which is the application's own: MSFS 2024 submits a
        // display time older than the previous frame's after a hitch, 3 times
        // in 2350 frames in one capture. Clear continuity so the next frame
        // primes, and leave the quarantine for failures that really do mean
        // the resources are no longer safe to use. Quarantining cost a full
        // second of generation for each of those three frames.
        if (result == XR_ERROR_TIME_INVALID) {
            clear_generation_continuity(state);
            return result;
        }
        enter_generation_quarantine(
            state,
            GenerationQuarantineReason::downstream_end_failed,
            static_cast<std::uint32_t>(result));
        return result;
    }
    if (pair_ready) {
        state->generation_steady_state_established = true;
    }
    if (use_continuous_presenter && !pipelined_presenter_mode) {
        // A frame the layer could not generate from is already handled: it was
        // passed through unchanged above. Demoting the presenter for it buys
        // nothing and costs a great deal, because the promotion that follows
        // is automatic - three throttled waits later the presenter is back.
        //
        // Cyberpunk 2077 through the Luke Ross mod fails one frame in fourteen
        // on the D3D11 interop path, and every one of those failures was
        // isolated: 175 failures in 59 s, 380 in 66 s, never two in a row. So
        // the presenter was stopped and restarted three times a second, 524
        // transitions in one session, each teardown submitting a frame with
        // nothing in it - black until the shutdown frame repeated the last
        // composition, and a stale pose under a newer image afterwards.
        // Neither artifact is worth having, and both exist only because the
        // presenter is being torn down mid-stride.
        //
        // Demote on a run of failures instead, which is what "generation is
        // not working here" actually looks like, using the same count of three
        // the promotion uses in the other direction.
        constexpr std::uint32_t kGenerationFailureDemotionStreak = 3;
        bool demote = false;
        {
            std::scoped_lock lock(state->mutex);
            if (presenter_first_frame) {
                state->generation_failure_streak = 0;
            } else if (++state->generation_failure_streak >=
                       kGenerationFailureDemotionStreak) {
                demote = true;
            }
        }
        if (demote) {
            // Not under state->mutex: stopping the presenter joins its thread.
            stop_continuous_presenter(state);
            std::scoped_lock lock(state->mutex);
            state->steamvr_throttled_wait_streak = 0;
            state->unpaced_wait_streak = 0;
            state->generation_failure_streak = 0;
        }
    }

    consume_application_frame(
        state,
        current_snapshot.display_time,
        consume_in_submission_order,
        match_latest_by_submission);
    if (!use_continuous_presenter && pair_ready && application_display_period &&
        !pipelined_presenter_mode &&
        xrfg::should_enter_generation_cooldown(
            state->optical_flow_backend ==
                xrfg::D3D12OpticalFlowBackend::nvidia,
            first_end_elapsed,
            *application_display_period)) {
        {
            std::scoped_lock lock(state->mutex);
            state->generation_resume_display_time =
                generation_resume_time(current_snapshot.display_time);
        }
        clear_generation_continuity(state);
        return result;
    }
    if (prepared.anchor_is_current) {
        std::scoped_lock lock(state->mutex);
        state->previous_projection = current_snapshot;
    } else if (!prepared.preserve_previous) {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
    }

    const bool promote_three_slot_after_inline = !use_continuous_presenter &&
        pipelined_presenter_start_requested &&
        (three_slot_generation_session(state) || fsr_two_slot_session(state) || queued_nvidia_session(state));
    if (!use_continuous_presenter && pipelined_presenter_start_requested &&
        !promote_three_slot_after_inline) {
        std::shared_ptr<GeneratedFrameEndInfo> seed_frame;
        if (pair_ready) {
            seed_frame = make_presenter_owned_frame(
                std::move(current_generated));
        } else if (prepared.kind == PreparedGenerationKind::prime) {
            seed_frame = make_presenter_owned_frame(
                std::move(first_generated));
        }
        const XrResult start_result =
            start_requested_pipelined_presenter(std::move(seed_frame));
        return XR_FAILED(start_result) ? start_result : result;
    }

    if (!pair_ready) {
        if (promote_three_slot_after_inline) {
            const XrResult start_result = start_requested_pipelined_presenter(nullptr);
            return XR_FAILED(start_result) ? start_result : result;
        }
        if (!use_continuous_presenter && (fsr_two_slot_session(state) || queued_nvidia_session(state)) &&
            prepared.kind == PreparedGenerationKind::prime) {
            // Prime is already presented: bootstrap prediction from that XR
            // cycle, not a repeated seed or an empty runtime submission.
            if (!start_continuous_presenter(state, nullptr, true))
                return XR_ERROR_RUNTIME_FAILURE;
        }
        return result;
    }

    if (use_continuous_presenter) {
        if (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
            // S/B are queued now. Allow the application to render ahead;
            // admission at its next EndFrame prevents output reuse/backlog.
            if (presenter_content_lock.owns_lock()) presenter_content_lock.unlock();
            frame_call_lock.unlock();
            return result;
        }
        // The once-per-pair hold. The layer submits one pair per application
        // frame and a pair costs two presenter frames, so without this an
        // application that renders faster than half the display rate produces
        // frames the pairing has no room for - they lose the history ring's
        // capture slot and are rendered and thrown away.
        //
        // It sits here rather than in the virtual wait for two reasons. The
        // frame is already handed over, so the presenter has composition to
        // work with and its count keeps advancing - gating the wait instead
        // deadlocked on the first frame, before anything had been enqueued.
        // And the application is released at the top of its next period rather
        // than the end of this one, so it renders straight away and synthesis
        // is queued with most of a period still in front of it.
        //
        // The frame lock goes first: an application whose wait runs on another
        // thread must not be shut out of xrWaitFrame while this waits.
        frame_call_lock.unlock();
        wait_for_presenter_pair(state);
        delay_djules_application_release(state);
        return result;
    }

    // The synthetic has gone downstream; submit its current copy before the
    // cycle that hands the runtime the frame which reads it.
    for (const auto& synthesizer : first_generated.pending_current_copies) {
        if (synthesizer) {
            static_cast<void>(synthesizer->flush_current_copy(
                state->d3d12_synthesis_queue
                    ? runtime_queue(*state)
                    : nullptr));
        }
    }
    std::optional<XrFrameState> adopted_frame;
    if (state->runtime_frame_waited_unbegun && state->last_inline_frame_state_valid) {
        adopted_frame = state->last_inline_frame_state;
        state->runtime_frame_waited_unbegun = false;
        state->application_begin_needs_wait = true;
    }
    if(extra_ready) {
        const auto middle_cycle=submit_current_cycle(state,extra_generated.info,frame_serial,&prepared,
            adopted_frame,&extra_generated,true);
        if(!middle_cycle.completed || XR_FAILED(middle_cycle.result)) {
            clear_generation_continuity(state); return middle_cycle.result;
        }
        adopted_frame.reset();
    }
    const InternalCycleResult current_cycle =
        submit_current_cycle(state, current_generated.info, frame_serial, &prepared, adopted_frame,&current_generated);
    // How far apart the runtime actually received the two frames of this
    // pair: from the synthetic's hand-over completing to the real frame's.
    const auto inline_pair_gap = std::chrono::steady_clock::now() -
        (first_end_started + first_end_elapsed);
    if (state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3) {
        const auto ns = [](auto value) { return static_cast<std::uint64_t>((std::max)(
            std::int64_t{0}, std::chrono::duration_cast<std::chrono::nanoseconds>(value).count())); };
        state->fsr_cadence.pair(current_cycle.completed, ns(inline_pair_gap),
            static_cast<std::uint64_t>((std::max)(XrDuration{0}, current_cycle.predicted_display_period)),
            ns(current_cycle.wait_elapsed), current_cycle.display_time - first_generated.info.displayTime);
    }
    if (!current_cycle.completed) {
        // The synthetic submission has already completed. A transient runtime
        // failure in the optional second cycle is recovered by the established
        // continuity reset; treating it as a structural resize would retain a
        // private image in an uncertain ownership phase for the whole timeout.
        clear_generation_continuity(state);
    } else if ((fsr_two_slot_session(state) || queued_nvidia_session(state)) && !use_continuous_presenter) {
        if (!start_continuous_presenter(state, nullptr, true))
            return XR_ERROR_RUNTIME_FAILURE;
    } else if (promote_three_slot_after_inline) {
        // Finish the final inline S(A,B),B exactly once before giving XR
        // cycle ownership to the director. B is now presented, so no seed is
        // discarded before presentation or repeated by the new scheduler.
        const XrResult start_result = start_requested_pipelined_presenter(nullptr);
        return XR_FAILED(start_result) ? start_result : result;
    } else if (!presenter_forbidden(*state) && state->frame_generation_mode !=
                   xrfg::D3D12FrameGenerationMode::fsr3 &&
               (three_slot_generation_session(state) ||
                steamvr_wait_requires_continuous_presenter(
                    state,
                    current_cycle) ||
                runtime_wait_lacks_pacing(state) ||
                inline_pair_lands_in_one_scanout(state, inline_pair_gap))) {
        // Request the promotion; do not perform it here. submit_current_cycle
        // has already submitted this frame, so seeding a freshly started
        // presenter thread with it handed a second owner to composition layers
        // and private swapchain leases that this frame still holds. The next
        // xrEndFrame finishes its inline pair before starting a three-slot
        // director; other presenters take their still-unpresented seed through
        // their existing handoff above.
        state->presenter_start_requested = true;
    }
    return XR_FAILED(current_cycle.result) ? current_cycle.result : result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    return guard_c_api_boundary([&] {
        return layer_get_instance_proc_addr_impl(instance, name, function);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::instance_create);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_create_api_layer_instance_impl(create_info, layer_info, instance);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::instance_create,
        result,
        instance && XR_SUCCEEDED(result) ? handle_value(*instance) : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_instance(XrInstance instance) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::instance_destroy,
        handle_value(instance));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_destroy_instance_impl(instance);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::instance_destroy,
        result,
        handle_value(instance));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_session(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_create,
        handle_value(instance));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_create_session_impl(instance, create_info, session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_create,
        result,
        handle_value(instance),
        session && XR_SUCCEEDED(result) ? handle_value(*session) : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_session(XrSession session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_destroy,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_destroy_session_impl(session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_destroy,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_begin_session(
    XrSession session,
    const XrSessionBeginInfo* begin_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_begin,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_begin_session_impl(session, begin_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_begin,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_end_session(XrSession session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_end,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_end_session_impl(session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_end,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_wait_frame(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_wait_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_frame_impl(session, wait_info, frame_state);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_wait_frame,
        result,
        frame_state && XR_SUCCEEDED(result)
            ? static_cast<std::uint64_t>(frame_state->predictedDisplayTime)
            : 0,
        frame_state && XR_SUCCEEDED(result)
            ? static_cast<std::uint64_t>(frame_state->predictedDisplayPeriod)
            : 0,
        frame_state && XR_SUCCEEDED(result) ? frame_state->shouldRender : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_begin_frame(
    XrSession session,
    const XrFrameBeginInfo* begin_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_begin_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_begin_frame_impl(session, begin_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_begin_frame,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_swapchain(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    return guard_c_api_boundary([&] {
        return layer_create_swapchain_impl(session, create_info, swapchain);
    });
}

// A composition layer names a space, and at any pipeline depth above zero a
// submission naming one can still be queued when the application destroys it.
// The runtime then rejects that submission, presenter_failure latches, and
// every later frame call fails for the life of the session - seen as the
// session freezing on recentre, which is when MSFS rebuilds its reference
// space. Settle the presenter first so nothing queued names this handle.
//
// Upstream never needed this because the application waited for the queue to
// empty inside xrEndFrame; it does not any more.
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_space(XrSpace space) {
    return guard_c_api_boundary([&]() -> XrResult {
        std::shared_ptr<Dispatch> dispatch;
        std::vector<std::shared_ptr<SessionState>> sessions;
        {
            std::scoped_lock lock(g_state_mutex);
            for (const auto& [handle, session] : g_sessions) {
                static_cast<void>(handle);
                if (session) {
                    sessions.push_back(session);
                    dispatch = session->dispatch;
                }
            }
            if (const auto owner = g_observed_spaces.find(space); owner != g_observed_spaces.end())
                dispatch = owner->second.dispatch;
        }
        if (!dispatch || dispatch->destroy_space == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
            return handle_value(a->handle) < handle_value(b->handle);
        });
        std::vector<PresenterResourceLifetimeGuard> guards;
        guards.reserve(sessions.size());
        for (const auto& session : sessions) {
            guards.emplace_back(session);
        }
        const auto result = dispatch->destroy_space(space);
        if (XR_SUCCEEDED(result)) {
            std::scoped_lock lock(g_state_mutex);
            g_observed_spaces.erase(space);
        }
        return result;
    });
}

// Pure observation. The event is forwarded exactly as the runtime produced it
// and is never consumed, reordered or synthesized: the application sees the
// same queue it would without the layer.
//
// It exists because a session that stops being displayed is invisible from
// everywhere else in this layer. When a runtime drops a session out of the
// visible state it reports shouldRender=false and stops blocking xrWaitFrame,
// the application stops submitting projection layers, and generation fails
// open - which in a capture is indistinguishable from the layer breaking. A
// captured MSFS 2024 session did exactly that 104 s in and never recovered,
// and nothing in the log could say whether the runtime or the layer started
// it. The state transition is the answer, and the layer only sees it here.
XRAPI_ATTR XrResult XRAPI_CALL layer_poll_event(
    XrInstance instance,
    XrEventDataBuffer* event_data) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->poll_event == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->poll_event(instance, event_data);
        // XR_EVENT_UNAVAILABLE is the common answer and carries no buffer, so
        // this costs one comparison on the overwhelming majority of calls.
        if (result == XR_SUCCESS && event_data != nullptr &&
            event_data->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* state_changed =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(
                    event_data);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::session_state,
                static_cast<std::int64_t>(state_changed->state),
                handle_value(state_changed->session),
                static_cast<std::uint64_t>(state_changed->time),
                0);
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_swapchain(XrSwapchain swapchain) {
    return guard_c_api_boundary([&] {
        return layer_destroy_swapchain_impl(swapchain);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    return guard_c_api_boundary([&] {
        return layer_enumerate_swapchain_images_impl(
            swapchain,
            image_capacity_input,
            image_count_output,
            images);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_acquire,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_acquire_swapchain_image_impl(swapchain, acquire_info, index);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_acquire,
        result,
        handle_value(swapchain),
        index && XR_SUCCEEDED(result) ? *index : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_wait,
        handle_value(swapchain),
        wait_info ? static_cast<std::uint64_t>(wait_info->timeout) : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_swapchain_image_impl(swapchain, wait_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_wait,
        result,
        handle_value(swapchain));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_release,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_release_swapchain_image_impl(swapchain, release_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_release,
        result,
        handle_value(swapchain));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_end_frame(
    XrSession session,
    const XrFrameEndInfo* end_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_end_frame,
        handle_value(session),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        end_info ? end_info->layerCount : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_end_frame_impl(session, end_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_end_frame,
        result,
        handle_value(session),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        end_info ? end_info->layerCount : 0);
    return result;
}

}  // namespace

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* layer_request) {

    xrfg::initialize_bridge_flight_logger();
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::negotiation,
        loader_info ? loader_info->minInterfaceVersion : 0,
        loader_info ? loader_info->maxInterfaceVersion : 0,
        loader_info ? loader_info->maxApiVersion : 0);
    const XrResult result = guard_c_api_boundary([&] {
        if (loader_info == nullptr || layer_request == nullptr || layer_name == nullptr ||
            std::strcmp(layer_name, kLayerName) != 0 ||
            loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
            loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
            loader_info->structSize != sizeof(XrNegotiateLoaderInfo) ||
            layer_request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
            layer_request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
            layer_request->structSize != sizeof(XrNegotiateApiLayerRequest)) {
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        if (loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
            loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
            loader_info->minApiVersion > kLayerApiVersion ||
            loader_info->maxApiVersion < kLayerApiVersion) {
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        // A refusal here is the one way to keep the layer out of a process
        // entirely: the loader drops an implicit layer whose negotiation
        // fails and carries on without it, so an excluded process has no
        // hook, no history and no private swapchain from this layer.
        const std::wstring executable =
            xrfg::implicit_layer::current_executable_name();
        if (xrfg::implicit_layer::executable_is_excluded(
                executable,
                xrfg::implicit_layer::read_excluded_processes(
                    current_layer_directory()))) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::process_excluded,
                0,
                executable.size(),
                0,
                0);
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        layer_request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        layer_request->layerApiVersion = kLayerApiVersion;
        layer_request->getInstanceProcAddr = layer_get_instance_proc_addr;
        layer_request->createApiLayerInstance = layer_create_api_layer_instance;
        xrfg::embedded::set_layer_active(true);
        return XR_SUCCESS;
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::negotiation,
        result,
        layer_request ? layer_request->layerInterfaceVersion : 0,
        layer_request ? layer_request->layerApiVersion : 0);
    return result;
}
