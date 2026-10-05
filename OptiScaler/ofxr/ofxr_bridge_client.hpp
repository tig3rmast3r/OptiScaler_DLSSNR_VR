#pragma once

#include "provider_api.hpp"

#include <cstdint>

namespace ofxr_bridge {

[[nodiscard]] bool available() noexcept;
[[nodiscard]] bool supports_fsr() noexcept;
[[nodiscard]] OFXR_ControlSnapshotV2 snapshot() noexcept;
[[nodiscard]] bool request(const OFXR_ControlSettingsV2& settings) noexcept;

[[nodiscard]] bool logging_setting() noexcept;
[[nodiscard]] bool set_logging_setting(bool enabled) noexcept;
[[nodiscard]] bool cadence_setting() noexcept;
[[nodiscard]] bool set_cadence_setting(bool enabled) noexcept;
[[nodiscard]] bool marker_setting() noexcept;
[[nodiscard]] bool set_marker_setting(bool enabled) noexcept;
[[nodiscard]] int overlay_setting() noexcept;
[[nodiscard]] bool set_overlay_setting(int position) noexcept;

void configure_dlss_guides(bool enabled) noexcept;
[[nodiscard]] bool publish_dlss_guides(
    const OFXR_DlssGuidePublicationV2& publication,
    ID3D12Device* verifiedProducerDevice = nullptr) noexcept;
void retire_dlss_guide_stream(std::uint64_t stream) noexcept;
[[nodiscard]] OFXR_DlssGuideStatisticsV2
dlss_guide_statistics() noexcept;

}  // namespace ofxr_bridge
