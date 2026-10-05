#include "pch.h"

#include "ofxr_bridge_client.hpp"

#include "../../third_party/ofxr/include/xrfg/dlss_motion_vectors.hpp"
#include "../../third_party/ofxr/include/xrfg/embedded_control.hpp"
#include "../../third_party/ofxr/include/xrfg/fsr_frame_generation.hpp"

#include <mutex>
#include <atomic>

namespace {

std::atomic_bool g_embedded_layer_active{false};

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
    // The permanent external OFXR DLL remains the OpenXR-loader bootstrap. In
    // FSR mode it delegates layer negotiation back into this module, so using
    // the bootstrap's provider API here would split controls/guides away from
    // the actual embedded layer instance.
    if (g_embedded_layer_active.load(std::memory_order_acquire)) return {};
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
    g_embedded_layer_active.store(active != 0, std::memory_order_release);
    if (active != 0) {
        std::scoped_lock lock(dispatch_mutex());
        cached_dispatch() = {};
    }
    return 1;
}

namespace ofxr_bridge {

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
bool publish_dlss_guides(
    const OFXR_DlssGuidePublicationV2& publication,
    ID3D12Device* verifiedProducerDevice) noexcept {
    const auto api = current_dispatch();
    if (!complete(api)) {
        xrfg::DlssMotionVectorPublication embedded{};
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
        embedded.verified_producer_device = verifiedProducerDevice;
        xrfg::publish_dlss_motion_vectors(embedded);
        return true;
    }
    if (api.v2) return api.publish_guides_v2(&publication) != 0;
    const auto old = as_v1(publication);
    return api.publish_vectors_v1(&old) != 0;
}
void retire_dlss_guide_stream(std::uint64_t stream) noexcept {
    const auto api = current_dispatch();
    if (complete(api))
        api.retire_stream(stream);
    else
        xrfg::retire_dlss_motion_vector_stream(stream);
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
