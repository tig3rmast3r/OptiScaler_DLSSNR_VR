#pragma once

#include "provider_api.hpp"
#include "../../third_party/ofxr/include/xrfg/embedded_control.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace xrfg { struct DlssMotionVectorFrame; }

namespace ofxr_bridge {

[[nodiscard]] bool available() noexcept;
[[nodiscard]] bool embedded_layer_active() noexcept;
[[nodiscard]] bool supports_fsr() noexcept;
[[nodiscard]] OFXR_ControlSnapshotV2 snapshot() noexcept;
[[nodiscard]] bool request(const OFXR_ControlSettingsV2& settings) noexcept;
[[nodiscard]] xrfg::embedded::AfwSnapshot afw_snapshot() noexcept;
[[nodiscard]] bool request_afw(
    const xrfg::embedded::AfwSettings& settings) noexcept;

[[nodiscard]] bool logging_setting() noexcept;
[[nodiscard]] bool set_logging_setting(bool enabled) noexcept;
[[nodiscard]] bool cadence_setting() noexcept;
[[nodiscard]] bool set_cadence_setting(bool enabled) noexcept;
[[nodiscard]] bool marker_setting() noexcept;
[[nodiscard]] bool set_marker_setting(bool enabled) noexcept;
[[nodiscard]] int overlay_setting() noexcept;
[[nodiscard]] bool set_overlay_setting(int position) noexcept;

void configure_dlss_guides(bool enabled) noexcept;
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
    int camera_motion_included) noexcept;
void begin_streamline_evaluation(std::uint32_t frame) noexcept;
void end_streamline_evaluation() noexcept;
[[nodiscard]] bool apply_streamline_camera(
    OFXR_DlssGuidePublicationV2& publication) noexcept;
[[nodiscard]] bool publish_dlss_guides(
    const OFXR_DlssGuidePublicationV2& publication,
    ID3D12Device* verifiedProducerDevice = nullptr,
    std::uint32_t productionCameraToken = 0) noexcept;
void retire_dlss_guide_stream(std::uint64_t stream) noexcept;
void register_luke_vulkan_device(std::uintptr_t device, LUID luid, unsigned node, bool valid) noexcept;
void unregister_luke_vulkan_device(std::uintptr_t device) noexcept;
HRESULT request_luke_vulkan_context(std::uintptr_t device, ID3D12Device** owner, ID3D12CommandQueue** queue) noexcept;
// Consume immediately after ingress, before NGX, never reselect after evaluation.
std::shared_ptr<const xrfg::DlssMotionVectorFrame> take_luke_origin_frame() noexcept;
void report_d3d12_submission(
    ID3D12CommandQueue* queue,
    std::uint32_t commandListCount,
    ID3D12CommandList* const* commandLists) noexcept;
void ensure_d3d12_submission_hook(ID3D12Device* device) noexcept;
[[nodiscard]] OFXR_DlssGuideStatisticsV2
dlss_guide_statistics() noexcept;

}  // namespace ofxr_bridge
