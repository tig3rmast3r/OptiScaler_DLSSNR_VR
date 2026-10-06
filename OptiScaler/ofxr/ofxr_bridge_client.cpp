#include "pch.h"

#include "ofxr_bridge_client.hpp"

#include "../State.h"
#include "../hooks/Streamline_Hooks.h"
#include "../hooks/D3D_NativeInterface.h"
#include "../inputs/NVNGX_DLSS.h"
#include "../hooks/LukeVulkanProducer.h"
#include "../hooks/LukeSourceContext.h"
#include "../../third_party/ofxr/include/xrfg/bridge_flight_logger.hpp"
#include <SysUtils.h>

#include "../../third_party/ofxr/include/xrfg/dlss_motion_vectors.hpp"
#include "../../third_party/ofxr/include/xrfg/embedded_control.hpp"
#include "../../third_party/ofxr/include/xrfg/fsr_frame_generation.hpp"
#include "../../third_party/ofxr/include/xrfg/two_frame_capture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

namespace {

struct StreamlineCameraEntry {
    std::uint32_t frame{};
    std::uint32_t diagnostic_viewport{luke_source::unknown};
    std::array<float, 16> view_to_clip{};
    std::array<float, 16> clip_to_view{};
    std::array<float, 3> position{};
    std::array<float, 3> up{};
    std::array<float, 3> right{};
    std::array<float, 3> forward{};
    float near_plane{};
    float far_plane{};
    float fov_vertical{};
    bool depth_inverted{};
    std::array<float, 16> clip_to_previous_clip{}, previous_clip_to_clip{};
    int camera_motion_included{-1};
    bool valid{};
};

std::mutex g_streamline_camera_mutex;
std::array<StreamlineCameraEntry, 8> g_streamline_cameras{};
thread_local StreamlineCameraEntry g_active_streamline_camera{};
thread_local std::shared_ptr<const xrfg::DlssMotionVectorFrame> g_luke_origin_frame;
luke_vulkan::Producer g_luke_vulkan_producer;

bool finite_values(const float* values, std::size_t count) noexcept {
    if (values == nullptr) return false;
    for (std::size_t index = 0; index < count; ++index)
        if (!std::isfinite(values[index])) return false;
    return true;
}

float length_squared(const float* value) noexcept {
    return value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2];
}

bool plausible_basis(const float* up, const float* right,
    const float* forward) noexcept {
    if (!finite_values(up, 3) || !finite_values(right, 3) ||
        !finite_values(forward, 3)) return false;
    const float up_length = length_squared(up);
    const float right_length = length_squared(right);
    const float forward_length = length_squared(forward);
    return up_length > 0.25F && up_length < 2.25F &&
        right_length > 0.25F && right_length < 2.25F &&
        forward_length > 0.25F && forward_length < 2.25F;
}

using GetVersion = std::uint32_t (*)() noexcept;
using GetControlV1 = int (*)(OFXR_ControlSnapshotV1*) noexcept;
using RequestControlV1 = int (*)(const OFXR_ControlSettingsV1*) noexcept;
using GetControlV2 = int (*)(OFXR_ControlSnapshotV2*) noexcept;
using RequestControlV2 = int (*)(const OFXR_ControlSettingsV2*) noexcept;
using RegisterProviderV2 = int (*)(
    const OFXR_OptiScalerProviderIdentityV2*) noexcept;
using GetInteger = int (*)() noexcept;
using SetInteger = int (*)(int) noexcept;
using ConfigureVectors = void (*)(int) noexcept;
using PublishVectorsV1 = int (*)(
    const OFXR_DlssMotionVectorPublicationV1*) noexcept;
using PublishGuidesV2 = int (*)(const OFXR_DlssGuidePublicationV2*) noexcept;
using RetireStream = void (*)(std::uint64_t) noexcept;
using GetStatisticsV1 = int (*)(OFXR_DlssMotionVectorStatisticsV1*) noexcept;
using GetStatisticsV2 = int (*)(OFXR_DlssGuideStatisticsV2*) noexcept;

struct Dispatch {
    HMODULE module{};
    bool v2{};
    GetVersion get_version{};
    GetControlV1 get_control_v1{};
    RequestControlV1 request_control_v1{};
    GetControlV2 get_control_v2{};
    RequestControlV2 request_control_v2{};
    GetInteger get_logging{};
    SetInteger set_logging{};
    GetInteger get_overlay{};
    SetInteger set_overlay{};
    ConfigureVectors configure_vectors{};
    PublishVectorsV1 publish_vectors_v1{};
    PublishGuidesV2 publish_guides_v2{};
    RetireStream retire_stream{};
    GetStatisticsV1 get_statistics_v1{};
    GetStatisticsV2 get_statistics_v2{};
};

std::mutex& dispatch_mutex() {
    static std::mutex mutex;
    return mutex;
}

Dispatch& cached_dispatch() {
    static Dispatch value;
    return value;
}

template <typename Function>
Function resolve(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

bool complete(const Dispatch& value) noexcept {
    const bool common = value.module != nullptr && value.get_version != nullptr &&
        value.get_logging != nullptr && value.set_logging != nullptr &&
        value.get_overlay != nullptr && value.set_overlay != nullptr &&
        value.configure_vectors != nullptr && value.retire_stream != nullptr;
    if (!common) return false;
    return value.v2
        ? value.get_control_v2 != nullptr && value.request_control_v2 != nullptr &&
              value.publish_guides_v2 != nullptr && value.get_statistics_v2 != nullptr
        : value.get_control_v1 != nullptr && value.request_control_v1 != nullptr &&
              value.publish_vectors_v1 != nullptr && value.get_statistics_v1 != nullptr;
}

Dispatch current_dispatch() noexcept {
    // The permanent external OFXR DLL is only the OpenXR-loader bootstrap when
    // this module is present. It delegates the whole session here before the
    // initial NVIDIA/FSR selection is known. Never publish DLSS guides or send
    // a later hot-switch request back to the external bootstrap: both modes
    // must share this embedded layer instance and its temporal ownership.
    if (xrfg::embedded::layer_active()) return {};
    std::scoped_lock lock(dispatch_mutex());
    auto& cached = cached_dispatch();
    const HMODULE module =
        GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll");
    if (module == nullptr) {
        cached = {};
        return {};
    }
    if (cached.module == module && complete(cached)) return cached;

    Dispatch candidate{};
    candidate.module = module;
    candidate.get_logging = resolve<GetInteger>(module, "OFXR_GetLoggingSettingV1");
    candidate.set_logging = resolve<SetInteger>(module, "OFXR_SetLoggingSettingV1");
    candidate.get_overlay = resolve<GetInteger>(module, "OFXR_GetOverlaySettingV1");
    candidate.set_overlay = resolve<SetInteger>(module, "OFXR_SetOverlaySettingV1");
    candidate.get_version = resolve<GetVersion>(module, "OFXR_GetProviderApiVersionV2");
    candidate.get_control_v2 = resolve<GetControlV2>(module, "OFXR_GetControlSnapshotV2");
    candidate.request_control_v2 = resolve<RequestControlV2>(module, "OFXR_RequestControlV2");
    candidate.configure_vectors = resolve<ConfigureVectors>(module, "OFXR_ConfigureDlssGuidesV2");
    candidate.publish_guides_v2 = resolve<PublishGuidesV2>(module, "OFXR_PublishDlssGuidesV2");
    candidate.retire_stream = resolve<RetireStream>(module, "OFXR_RetireDlssGuideStreamV2");
    candidate.get_statistics_v2 = resolve<GetStatisticsV2>(module, "OFXR_GetDlssGuideStatisticsV2");
    auto register_provider = resolve<RegisterProviderV2>(
        module, "OFXR_RegisterOptiScalerProviderV2");
    candidate.v2 = true;
    if (complete(candidate) &&
        candidate.get_version() == OFXR_PROVIDER_API_VERSION_V2 &&
        register_provider != nullptr) {
        OFXR_OptiScalerProviderIdentityV2 identity{};
        identity.capabilities = OFXR_OPTISCALER_CAP_DLSS_GUIDES_V2 |
            OFXR_OPTISCALER_CAP_IN_GAME_MENU_V2 |
            OFXR_OPTISCALER_CAP_FSR_FRAME_GENERATION_V2;
        if (register_provider(&identity) != 0) {
            cached = candidate;
            return cached;
        }
    }

    candidate = {};
    candidate.module = module;
    candidate.get_version = resolve<GetVersion>(module, "OFXR_GetProviderApiVersionV1");
    candidate.get_control_v1 = resolve<GetControlV1>(module, "OFXR_GetControlSnapshotV1");
    candidate.request_control_v1 = resolve<RequestControlV1>(module, "OFXR_RequestControlV1");
    candidate.get_logging = resolve<GetInteger>(module, "OFXR_GetLoggingSettingV1");
    candidate.set_logging = resolve<SetInteger>(module, "OFXR_SetLoggingSettingV1");
    candidate.get_overlay = resolve<GetInteger>(module, "OFXR_GetOverlaySettingV1");
    candidate.set_overlay = resolve<SetInteger>(module, "OFXR_SetOverlaySettingV1");
    candidate.configure_vectors = resolve<ConfigureVectors>(module, "OFXR_ConfigureDlssMotionVectorsV1");
    candidate.publish_vectors_v1 = resolve<PublishVectorsV1>(module, "OFXR_PublishDlssMotionVectorsV1");
    candidate.retire_stream = resolve<RetireStream>(module, "OFXR_RetireDlssMotionVectorStreamV1");
    candidate.get_statistics_v1 = resolve<GetStatisticsV1>(module, "OFXR_GetDlssMotionVectorStatisticsV1");
    if (!complete(candidate) ||
        candidate.get_version() != OFXR_PROVIDER_API_VERSION_V1) {
        cached = {};
        return {};
    }
    cached = candidate;
    return cached;
}

OFXR_DlssMotionVectorPublicationV1 as_v1(
    const OFXR_DlssGuidePublicationV2& input) noexcept {
    OFXR_DlssMotionVectorPublicationV1 output{};
    output.stream = input.stream;
    output.output = input.output;
    output.motion_vectors = input.motion_vectors;
    output.producer_queue = input.producer_queue;
    output.producer_command_list = input.producer_command_list;
    output.output_x = input.output_x;
    output.output_y = input.output_y;
    output.output_width = input.output_width;
    output.output_height = input.output_height;
    output.motion_x = input.motion_x;
    output.motion_y = input.motion_y;
    output.motion_width = input.motion_width;
    output.motion_height = input.motion_height;
    output.scale_x = input.scale_x;
    output.scale_y = input.scale_y;
    output.jitter_x = input.jitter_x;
    output.jitter_y = input.jitter_y;
    output.resource_state = input.resource_state;
    output.jittered = input.jittered;
    output.reset = input.reset;
    return output;
}

}  // namespace

extern "C" __declspec(dllexport) int OFXR_OptiScalerProviderV1(
    OFXR_OptiScalerProviderIdentityV1* output) noexcept {
    if (output == nullptr ||
        output->struct_size < sizeof(OFXR_OptiScalerProviderIdentityV1)) return 0;
    OFXR_OptiScalerProviderIdentityV1 identity{};
    identity.capabilities = OFXR_OPTISCALER_CAP_DLSS_MOTION_VECTORS_V1 |
        OFXR_OPTISCALER_CAP_IN_GAME_MENU_V1;
    *output = identity;
    return 1;
}

extern "C" __declspec(dllexport) int OFXR_OptiScalerProviderV2(
    OFXR_OptiScalerProviderIdentityV2* output) noexcept {
    if (output == nullptr ||
        output->struct_size < sizeof(OFXR_OptiScalerProviderIdentityV2)) return 0;
    OFXR_OptiScalerProviderIdentityV2 identity{};
    identity.capabilities = OFXR_OPTISCALER_CAP_DLSS_GUIDES_V2 |
        OFXR_OPTISCALER_CAP_IN_GAME_MENU_V2 |
        OFXR_OPTISCALER_CAP_FSR_FRAME_GENERATION_V2;
    *output = identity;
    return 1;
}

extern "C" __declspec(dllexport) int OFXR_SetEmbeddedLayerActiveV1(
    int active) noexcept {
    xrfg::embedded::set_layer_active(active != 0);
    LOG_INFO("V30067 OpenXR embedded ownership active={}", active != 0);
    if (active != 0) {
        std::scoped_lock lock(dispatch_mutex());
        cached_dispatch() = {};
    }
    return 1;
}

extern "C" __declspec(dllexport) int OFXR_GetEmbeddedLayerActiveV1() noexcept {
    return xrfg::embedded::layer_active() ? 1 : 0;
}

// Combined-module-only handoff. Luke Ross presents through a D3D11 OpenXR
// device even though its post-ReShade DLSS guides are produced by the game's
// D3D12 queue. Returning that exact native queue lets the embedded layer build
// its D3D11 sharing resources on the guide device instead of an unrelated
// private D3D12 device. Ordinary OpenXR hosts are deliberately rejected.
extern "C" __declspec(dllexport) int OFXR_GetLukeRossD3D12ContextV1(
    ID3D12Device** device,
    ID3D12CommandQueue** queue) noexcept {
    if (device != nullptr) *device = nullptr;
    if (queue != nullptr) *queue = nullptr;
    if (device == nullptr || queue == nullptr) return 0;
    try {
        if (!LukeRossDlssIngress::Detected()) return 0;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> vulkan_queue;
        if (g_luke_vulkan_producer.present() &&
            (State::Instance().api == API::Vulkan || !State::Instance().currentCommandQueue)) {
            Microsoft::WRL::ComPtr<ID3D12Device> vulkan_owner;
            const auto result = ofxr_bridge::request_luke_vulkan_context(0, vulkan_owner.GetAddressOf(), vulkan_queue.GetAddressOf());
            if (FAILED(result)) {
                LOG_WARN("V30068 Luke Vulkan early producer owner rejected result=0x{:X}", static_cast<unsigned>(result));
                return 0;
            }
        }
        ID3D12CommandQueue* producer_queue_raw = vulkan_queue
            ? vulkan_queue.Get() : State::Instance().currentCommandQueue;
        static std::atomic_uint contextReceipts{0};
        if (contextReceipts.fetch_add(1, std::memory_order_relaxed) < 4)
            LOG_INFO("V30067 Luke OpenXR producer context embedded={} vulkan={} queue={:p}",
                xrfg::embedded::layer_active(), bool(vulkan_queue),
                static_cast<void*>(producer_queue_raw));
        IUnknown* native_queue = nullptr;
        if (!d3d_native::get_borrowed(producer_queue_raw, &native_queue)) return 0;
        producer_queue_raw = static_cast<ID3D12CommandQueue*>(native_queue);
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> producer_queue =
            producer_queue_raw;
        if (!producer_queue ||
            producer_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return 0;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> producer_device;
        if (FAILED(producer_queue->GetDevice(
                IID_PPV_ARGS(producer_device.GetAddressOf())))) {
            return 0;
        }
        // A native queue can still report a device proxy. Match the
        // guide ingress's native-device resolution, not merely its adapter.
        IUnknown* native_device = nullptr;
        if (!d3d_native::get_borrowed(producer_device.Get(), &native_device)) return 0;
        {
            Microsoft::WRL::ComPtr<ID3D12Device> canonical_device;
            if (FAILED(static_cast<ID3D12Device*>(native_device)->QueryInterface(
                    IID_PPV_ARGS(canonical_device.GetAddressOf())))) return 0;
            producer_device = std::move(canonical_device);
        }
        if (FAILED(producer_device.CopyTo(device))) return 0;
        if (FAILED(producer_queue.CopyTo(queue))) {
            (*device)->Release();
            *device = nullptr;
            return 0;
        }
        return 1;
    } catch (...) {
        if (*device != nullptr) {
            (*device)->Release();
            *device = nullptr;
        }
        if (*queue != nullptr) {
            (*queue)->Release();
            *queue = nullptr;
        }
        return 0;
    }
}

namespace ofxr_bridge {
void register_luke_vulkan_device(std::uintptr_t device, LUID luid, unsigned node, bool valid) noexcept {
    try { g_luke_vulkan_producer.record(device, luid, node, valid); } catch (...) {}
}
void unregister_luke_vulkan_device(std::uintptr_t device) noexcept {
    try { g_luke_vulkan_producer.forget(device); } catch (...) {}
}
HRESULT request_luke_vulkan_context(std::uintptr_t device, ID3D12Device** owner, ID3D12CommandQueue** queue) noexcept {
    try {
        ScopedSkipSpoofingGlobal skip;
        return g_luke_vulkan_producer.get(device, owner, queue);
    } catch (...) { return E_FAIL; }
}

bool available() noexcept { return true; }
bool supports_fsr() noexcept {
    const auto api = current_dispatch();
    return !complete(api) || api.v2;
}

OFXR_ControlSnapshotV2 snapshot() noexcept {
    OFXR_ControlSnapshotV2 value{};
    const auto api = current_dispatch();
    if (!complete(api)) {
        const auto embedded = xrfg::embedded::snapshot();
        value.desired.enabled = embedded.desired.enabled ? 1 : 0;
        value.desired.backend = embedded.desired.backend;
        value.desired.preset = embedded.desired.preset;
        value.desired.scale = embedded.desired.scale;
        value.desired.backward = embedded.desired.backward ? 1 : 0;
        value.desired.motion_vectors = embedded.desired.motion_vectors;
        value.desired.frame_generation = embedded.desired.frame_generation;
        value.revision = embedded.revision;
        value.sessions = embedded.sessions;
        value.pending = embedded.pending;
        value.bypass = embedded.bypass;
        value.ready = embedded.ready;
        value.errors = embedded.errors;
        value.last_error = embedded.last_error;
        return value;
    }
    if (api.v2) {
        if (api.get_control_v2(&value) == 0)
            value.last_error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return value;
    }
    OFXR_ControlSnapshotV1 old{};
    if (api.get_control_v1(&old) == 0) {
        value.last_error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return value;
    }
    value.desired.enabled = old.desired.enabled;
    value.desired.backend = old.desired.backend;
    value.desired.preset = old.desired.preset;
    value.desired.scale = old.desired.scale;
    value.desired.backward = old.desired.backward;
    value.desired.motion_vectors = old.desired.motion_vectors;
    value.revision = old.revision;
    value.sessions = old.sessions;
    value.pending = old.pending;
    value.bypass = old.bypass;
    value.ready = old.ready;
    value.errors = old.errors;
    value.last_error = old.last_error;
    return value;
}

bool embedded_layer_active() noexcept {
    return xrfg::embedded::layer_active();
}

bool request(const OFXR_ControlSettingsV2& settings) noexcept {
    const auto api = current_dispatch();
    if (!complete(api)) {
        xrfg::embedded::Settings embedded{};
        embedded.enabled = settings.enabled != 0;
        embedded.backend = settings.backend;
        embedded.preset = settings.preset;
        embedded.scale = settings.scale;
        embedded.backward = settings.backward != 0;
        embedded.motion_vectors = settings.motion_vectors;
        embedded.frame_generation = settings.frame_generation;
        return xrfg::embedded::request(embedded);
    }
    if (api.v2) return api.request_control_v2(&settings) != 0;
    if (settings.frame_generation != 0) return false;
    OFXR_ControlSettingsV1 old{};
    old.enabled = settings.enabled;
    old.backend = settings.backend;
    old.preset = settings.preset;
    old.scale = settings.scale;
    old.backward = settings.backward;
    old.motion_vectors = settings.motion_vectors;
    return api.request_control_v1(&old) != 0;
}

xrfg::embedded::AfwSnapshot afw_snapshot() noexcept {
    return xrfg::embedded::afw_snapshot();
}

bool request_afw(
    const xrfg::embedded::AfwSettings& settings) noexcept {
    return xrfg::embedded::request_afw(settings);
}

bool logging_setting() noexcept {
    const auto api = current_dispatch();
    return complete(api) ? api.get_logging() != 0
                         : xrfg::embedded::logging_setting();
}
bool cadence_setting() noexcept { return xrfg::embedded::cadence_setting(); }
bool set_cadence_setting(bool enabled) noexcept { return xrfg::embedded::set_cadence_setting(enabled); }
bool marker_setting() noexcept { return xrfg::embedded::marker_setting(); }
bool set_marker_setting(bool enabled) noexcept { return xrfg::embedded::set_marker_setting(enabled); }
bool set_logging_setting(bool enabled) noexcept {
    const auto api = current_dispatch();
    return complete(api) ? api.set_logging(enabled ? 1 : 0) != 0
                         : xrfg::embedded::set_logging_setting(enabled);
}
int overlay_setting() noexcept {
    const auto api = current_dispatch();
    return complete(api) ? api.get_overlay() : xrfg::embedded::overlay_setting();
}
bool set_overlay_setting(int position) noexcept {
    const auto api = current_dispatch();
    return complete(api) ? api.set_overlay(position) != 0
                         : xrfg::embedded::set_overlay_setting(position);
}
void configure_dlss_guides(bool enabled) noexcept {
    const auto api = current_dispatch();
    if (complete(api))
        api.configure_vectors(enabled ? 1 : 0);
    else
        xrfg::configure_dlss_motion_vector_tracking(enabled);
}
void cache_streamline_camera(
    std::uint32_t frame, std::uint32_t diagnostic_viewport,
    const float* camera_view_to_clip,
    const float* camera_clip_to_view,
    const float* camera_position,
    const float* camera_up,
    const float* camera_right,
    const float* camera_forward,
    float camera_near,
    float camera_far,
    float camera_fov_vertical,
    bool depth_inverted,
    const float* clip_to_previous_clip,
    const float* previous_clip_to_clip,
    int camera_motion_included) noexcept {
    StreamlineCameraEntry camera{};
    if (!finite_values(camera_view_to_clip, 16) ||
        !finite_values(camera_clip_to_view, 16) ||
        !finite_values(camera_position, 3) ||
        !plausible_basis(camera_up, camera_right, camera_forward)) return;
    camera.frame = frame;
    camera.diagnostic_viewport = diagnostic_viewport;
    std::copy_n(camera_view_to_clip, 16, camera.view_to_clip.begin());
    std::copy_n(camera_clip_to_view, 16, camera.clip_to_view.begin());
    std::copy_n(camera_position, 3, camera.position.begin());
    std::copy_n(camera_up, 3, camera.up.begin());
    std::copy_n(camera_right, 3, camera.right.begin());
    std::copy_n(camera_forward, 3, camera.forward.begin());
    camera.near_plane = camera_near;
    camera.far_plane = camera_far;
    camera.fov_vertical = camera_fov_vertical;
    camera.depth_inverted = depth_inverted;
    camera.camera_motion_included = camera_motion_included;
    if (finite_values(clip_to_previous_clip, 16) && finite_values(previous_clip_to_clip, 16)) {
        std::copy_n(clip_to_previous_clip, 16, camera.clip_to_previous_clip.begin());
        std::copy_n(previous_clip_to_clip, 16, camera.previous_clip_to_clip.begin());
    }
    camera.valid = true;
    std::scoped_lock lock(g_streamline_camera_mutex);
    g_streamline_cameras[frame % g_streamline_cameras.size()] = camera;
}

void begin_streamline_evaluation(std::uint32_t frame) noexcept {
    g_active_streamline_camera = {};
    std::scoped_lock lock(g_streamline_camera_mutex);
    const auto& candidate =
        g_streamline_cameras[frame % g_streamline_cameras.size()];
    if (candidate.valid && candidate.frame == frame)
        g_active_streamline_camera = candidate;
    auto& trace=xrfg::bridge_flight_logger();
    if(trace.ram_enabled()) {
        auto hash=luke_source::hash(candidate.view_to_clip.data(),16);
        hash=luke_source::hash(candidate.clip_to_view.data(),16,hash);
        hash=luke_source::hash(candidate.position.data(),3,hash);
        hash=luke_source::hash(candidate.right.data(),3,hash);
        hash=luke_source::hash(candidate.up.data(),3,hash);
        hash=luke_source::hash(candidate.forward.data(),3,hash);
        trace.event(xrfg::BridgeFlightOperation::luke_afw_stage,
            static_cast<std::int64_t>(luke_source::key(frame,luke_source::active.viewport)),
            400,luke_source::key(candidate.frame,candidate.diagnostic_viewport),hash);
        trace.event(xrfg::BridgeFlightOperation::luke_afw_stage,
            static_cast<std::int64_t>(luke_source::key(frame,luke_source::active.viewport)),
            401,candidate.valid && candidate.frame==frame,
            reinterpret_cast<std::uintptr_t>(g_active_streamline_camera.view_to_clip.data()));
    }
}

void end_streamline_evaluation() noexcept {
    g_active_streamline_camera = {};
}

bool apply_streamline_camera(
    OFXR_DlssGuidePublicationV2& publication) noexcept {
    const auto& camera = g_active_streamline_camera;
    if (!camera.valid) return false;
    std::copy(camera.view_to_clip.begin(), camera.view_to_clip.end(),
              publication.camera_view_to_clip);
    std::copy(camera.clip_to_view.begin(), camera.clip_to_view.end(),
              publication.camera_clip_to_view);
    std::copy(camera.position.begin(), camera.position.end(),
              publication.camera_position);
    std::copy(camera.up.begin(), camera.up.end(), publication.camera_up);
    std::copy(camera.right.begin(), camera.right.end(),
              publication.camera_right);
    std::copy(camera.forward.begin(), camera.forward.end(),
              publication.camera_forward);
    publication.camera_near = camera.near_plane;
    publication.camera_far = camera.far_plane;
    publication.camera_fov_vertical = camera.fov_vertical;
    publication.depth_inverted = camera.depth_inverted ? 1 : 0;
    publication.depth_infinite = camera.far_plane == 0.0F ? 1 : 0;
    publication.camera_data_valid = 1;
    publication.camera_matrices_valid = 1;
    return true;
}

bool publish_dlss_guides(
    const OFXR_DlssGuidePublicationV2& publication,
    ID3D12Device* verifiedProducerDevice,
    std::uint32_t productionCameraToken) noexcept {
    g_luke_origin_frame.reset();
    const auto api = current_dispatch();
    if (!complete(api)) {
        xrfg::DlssMotionVectorPublication embedded{};
        embedded.protect_snapshot_lifetime = LukeRossDlssIngress::Detected();
        embedded.camera_frame_token = productionCameraToken ? productionCameraToken : g_active_streamline_camera.frame;
        embedded.camera_frame_token_valid = productionCameraToken ? publication.camera_data_valid != 0 : g_active_streamline_camera.valid;
        embedded.raw_camera_motion_included = productionCameraToken ? -1 : g_active_streamline_camera.camera_motion_included;
        embedded.raw_clip_to_previous_clip = productionCameraToken ? std::array<float,16>{} : g_active_streamline_camera.clip_to_previous_clip;
        embedded.raw_previous_clip_to_clip = productionCameraToken ? std::array<float,16>{} : g_active_streamline_camera.previous_clip_to_clip;
        embedded.stream = publication.stream;
        embedded.output = publication.output;
        embedded.motion_vectors = publication.motion_vectors;
        embedded.producer_queue = publication.producer_queue;
        embedded.producer_command_list = publication.producer_command_list;
        embedded.output_x = publication.output_x;
        embedded.output_y = publication.output_y;
        embedded.output_width = publication.output_width;
        embedded.output_height = publication.output_height;
        embedded.motion_x = publication.motion_x;
        embedded.motion_y = publication.motion_y;
        embedded.motion_width = publication.motion_width;
        embedded.motion_height = publication.motion_height;
        embedded.scale_x = publication.scale_x;
        embedded.scale_y = publication.scale_y;
        embedded.jitter_x = publication.jitter_x;
        embedded.jitter_y = publication.jitter_y;
        embedded.resource_state = static_cast<D3D12_RESOURCE_STATES>(
            publication.resource_state);
        embedded.jittered = publication.jittered != 0;
        embedded.reset = publication.reset != 0;
        embedded.depth = publication.depth;
        embedded.depth_x = publication.depth_x;
        embedded.depth_y = publication.depth_y;
        embedded.depth_width = publication.depth_width;
        embedded.depth_height = publication.depth_height;
        embedded.depth_resource_state = static_cast<D3D12_RESOURCE_STATES>(
            publication.depth_resource_state);
        embedded.frame_time_delta_ms = publication.frame_time_delta_ms;
        embedded.camera_near = publication.camera_near;
        embedded.camera_far = publication.camera_far;
        embedded.depth_inverted = publication.depth_inverted != 0;
        embedded.depth_infinite = publication.depth_infinite != 0;
        std::copy_n(publication.camera_position, 3,
                    embedded.camera_position.begin());
        std::copy_n(publication.camera_up, 3, embedded.camera_up.begin());
        std::copy_n(publication.camera_right, 3,
                    embedded.camera_right.begin());
        std::copy_n(publication.camera_forward, 3,
                    embedded.camera_forward.begin());
        embedded.camera_fov_vertical = publication.camera_fov_vertical;
        embedded.camera_data_valid = publication.camera_data_valid != 0;
        std::copy_n(publication.camera_view_to_clip, 16,
                    embedded.camera_view_to_clip.begin());
        std::copy_n(publication.camera_clip_to_view, 16,
                    embedded.camera_clip_to_view.begin());
        embedded.camera_matrices_valid =
            publication.camera_matrices_valid != 0;
        embedded.output_resource_state =
            static_cast<D3D12_RESOURCE_STATES>(
                publication.output_resource_state);
        embedded.snapshot_output = publication.snapshot_output != 0;
        embedded.input_color = publication.input_color;
        embedded.input_color_x = publication.input_color_x;
        embedded.input_color_y = publication.input_color_y;
        embedded.input_color_width = publication.input_color_width;
        embedded.input_color_height = publication.input_color_height;
        embedded.input_color_resource_state =
            static_cast<D3D12_RESOURCE_STATES>(
                publication.input_color_resource_state);
        embedded.verified_producer_device = verifiedProducerDevice;
        const auto frame = xrfg::publish_dlss_motion_vectors(embedded);
        if (frame && embedded.protect_snapshot_lifetime &&
            xrfg::embedded::afw_snapshot().desired.mode != 0)
            g_luke_origin_frame = frame;
        return frame != nullptr;
    }
    if (api.v2) return api.publish_guides_v2(&publication) != 0;
    const auto old = as_v1(publication);
    return api.publish_vectors_v1(&old) != 0;
}
std::shared_ptr<const xrfg::DlssMotionVectorFrame> take_luke_origin_frame() noexcept {
    return std::exchange(g_luke_origin_frame, {});
}
void retire_dlss_guide_stream(std::uint64_t stream) noexcept {
    const auto api = current_dispatch();
    if (complete(api))
        api.retire_stream(stream);
    else
        xrfg::retire_dlss_motion_vector_stream(stream);
}
void report_d3d12_submission(
    ID3D12CommandQueue* queue,
    std::uint32_t commandListCount,
    ID3D12CommandList* const* commandLists) noexcept {
    xrfg::two_frame_capture_submission(queue, commandListCount, commandLists);
    // Native verified-device publications use the local registry even when
    // the embedded API is complete. Its producer receipts must not be skipped.
    xrfg::report_dlss_motion_vector_submission(
        queue, commandListCount, commandLists);
}
OFXR_DlssGuideStatisticsV2 dlss_guide_statistics() noexcept {
    OFXR_DlssGuideStatisticsV2 value{};
    const auto api = current_dispatch();
    if (!complete(api)) {
        const auto guides = xrfg::dlss_motion_vector_statistics();
        const auto fsr = xrfg::fsr_frame_generation_statistics();
        value.published = guides.published;
        value.depth_publications = guides.depth_publications;
        value.invalid_publications = guides.invalid_publications;
        value.snapshot_copies = guides.snapshot_copies;
        value.snapshot_failures = guides.snapshot_failures;
        value.resolve_calls = guides.resolve_calls;
        value.resolve_missing_streams = guides.resolve_missing_streams;
        value.resolve_stale_pairs = guides.resolve_stale_pairs;
        value.resolve_queue_mismatches = guides.resolve_queue_mismatches;
        value.matched = guides.matched;
        value.temporal_rejections = guides.temporal_rejections;
        value.invalid_rejections = guides.invalid_rejections;
        value.used = guides.used;
        value.last_used_publication = guides.last_used_publication;
        value.status = static_cast<std::uint32_t>(guides.status);
        value.fsr_attempts = fsr.attempts;
        value.fsr_used = fsr.used;
        value.fsr_failures = fsr.failures;
        value.fsr_status = static_cast<std::uint32_t>(fsr.status);
        return value;
    }
    if (api.v2) {
        (void)api.get_statistics_v2(&value);
        return value;
    }
    OFXR_DlssMotionVectorStatisticsV1 old{};
    if (api.get_statistics_v1(&old) != 0) {
        value.published = old.published;
        value.invalid_publications = old.invalid_publications;
        value.snapshot_copies = old.snapshot_copies;
        value.snapshot_failures = old.snapshot_failures;
        value.resolve_calls = old.resolve_calls;
        value.resolve_missing_streams = old.resolve_missing_streams;
        value.resolve_stale_pairs = old.resolve_stale_pairs;
        value.resolve_queue_mismatches = old.resolve_queue_mismatches;
        value.matched = old.matched;
        value.temporal_rejections = old.temporal_rejections;
        value.invalid_rejections = old.invalid_rejections;
        value.used = old.used;
        value.last_used_publication = old.last_used_publication;
        value.status = old.status;
    }
    return value;
}

}  // namespace ofxr_bridge
