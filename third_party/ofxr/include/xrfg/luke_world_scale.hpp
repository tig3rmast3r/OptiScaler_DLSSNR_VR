#pragma once

#include <array>
#include <cmath>

namespace xrfg {

// Both eye positions must come from one xrLocateViews result. Mixing retained
// AER poses from different times folds headset displacement into the IPD.
inline bool luke_xr_eye_baseline(const std::array<float, 3>& left,
    const std::array<float, 3>& right, float& result) noexcept {
    double squared_distance = 0.0;
    for (std::size_t axis = 0; axis < left.size(); ++axis) {
        if (!std::isfinite(left[axis]) || !std::isfinite(right[axis]))
            return false;
        const double delta = static_cast<double>(right[axis]) - left[axis];
        squared_distance += delta * delta;
    }
    const double distance = std::sqrt(squared_distance);
    if (!std::isfinite(distance) || distance < 0.01F || distance > 0.20F)
        return false;
    result = static_cast<float>(distance);
    return true;
}

// Luke's WorldScale multiplies physical HMD displacement on presentation;
// the captured game-camera separation is physical IPD / WorldScale.
inline bool luke_game_eye_baseline(float xr_baseline, float world_scale,
    float& game_baseline) noexcept {
    if (!std::isfinite(xr_baseline) || !std::isfinite(world_scale) ||
        xr_baseline < 0.01F || xr_baseline > 0.20F ||
        world_scale < 0.2F || world_scale > 5.0F) return false;
    game_baseline = xr_baseline / world_scale;
    return std::isfinite(game_baseline) && game_baseline > 0.0F;
}

}  // namespace xrfg
