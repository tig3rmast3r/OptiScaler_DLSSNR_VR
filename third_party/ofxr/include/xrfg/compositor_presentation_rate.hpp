#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace xrfg {
struct CompositorPresentationRate {
    float fps{};
    std::uint32_t intervals{};
    std::uint32_t mispresented{};
};

// OpenVR records in ascending order. A positive NumFramePresents means the
// frame was shown, even if NumMisPresented says it landed on another vsync.
// Repeated scanouts count once. Zero-present trailing records are unsettled.
template<class Timing>
[[nodiscard]] std::optional<CompositorPresentationRate>
compositor_presentation_rate(std::span<const Timing> records) noexcept {
    std::size_t end = records.size();
    while (end && records[end - 1].m_nNumFramePresents == 0) --end;
    if (end < 2) return std::nullopt;
    const double span = records[end - 1].m_flSystemTimeInSeconds -
        records.front().m_flSystemTimeInSeconds;
    if (!(span >= 0.05)) return std::nullopt;
    std::uint32_t shown = 0, mispresented = 0;
    for (std::size_t i = 0; i < end; ++i) {
        if (i && (records[i].m_nFrameIndex <= records[i - 1].m_nFrameIndex ||
            records[i].m_flSystemTimeInSeconds <= records[i - 1].m_flSystemTimeInSeconds))
            return std::nullopt;
        if (records[i].m_nNumFramePresents) {
            ++shown;
            if (records[i].m_nNumMisPresented) ++mispresented;
        }
    }
    const auto intervals = shown - (records.front().m_nNumFramePresents ? 1U : 0U);
    return CompositorPresentationRate{static_cast<float>(intervals / span),
        intervals, mispresented};
}
} // namespace xrfg
