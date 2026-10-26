#pragma once
#include "xrfg/interface_metrics.hpp"
#include <cmath>

namespace xrfg {
[[nodiscard]] inline bool presented_output_valid(const InterfaceTelemetry& value) noexcept {
    return value.rates_valid && (value.output_source == InterfaceSource::steamvr_delivery ||
        value.output_source == InterfaceSource::steamvr_scene_delivery) &&
        std::isfinite(value.output_fps) && value.output_fps >= 0;
}

// Known output sources have different boundaries. Accepted unique images are
// useful delivery measurements, but never claim those are physical scanout.
[[nodiscard]] inline bool output_rate_valid(const InterfaceTelemetry& value) noexcept {
    return presented_output_valid(value) || (value.rates_valid &&
        (value.output_source==InterfaceSource::accepted_scene_submissions ||
         value.output_source==InterfaceSource::runtime_accepted_delivery) &&
        std::isfinite(value.output_fps) && value.output_fps>=0);
}

// Read through the delivery source. gpu_render_timing() only observes its cache;
// a meter must not depend on a different overlay polling that cache first.
template<class Delivery>
void poll_presented_output(Delivery* delivery, std::int64_t now, InterfaceTelemetry& value) noexcept {
    value.output_fps = 0;
    value.output_source = InterfaceSource::none;
    if (!delivery) return;
    const auto rate = delivery->delivered_fps(now);
    if (rate && std::isfinite(*rate) && *rate >= 0) {
        value.output_fps = *rate;
        value.output_source = InterfaceSource::steamvr_delivery;
    }
}
} // namespace xrfg
