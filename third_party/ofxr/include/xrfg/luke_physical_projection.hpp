#pragma once

#include "xrfg/luke_projection_ray.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace xrfg {

// Convert the producer's row-vector full-output projection to the actual
// angular viewport of one OpenXR eye. This is the continuous counterpart of
// luke_guide_window's raster crop: no assumed resolution or pixel offset.
[[nodiscard]] inline bool luke_physical_eye_projection(
    std::array<float, 16>& projection, float angle_left, float angle_right,
    float angle_up, float angle_down) noexcept {
    for (const float value : projection)
        if (!std::isfinite(value)) return false;
    const double left = luke_projection_ray_ndc(std::tan(angle_left), projection[0],
        projection[8], projection[3], projection[11]);
    const double right = luke_projection_ray_ndc(std::tan(angle_right), projection[0],
        projection[8], projection[3], projection[11]);
    const double top = luke_projection_ray_ndc(std::tan(angle_up), projection[5],
        projection[9], projection[7], projection[11]);
    const double bottom = luke_projection_ray_ndc(std::tan(angle_down), projection[5],
        projection[9], projection[7], projection[11]);
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(top) || !std::isfinite(bottom) ||
        left >= right || bottom >= top) return false;
    const double scale_x = (right - left) * 0.5;
    const double scale_y = (top - bottom) * 0.5;
    const double center_x = (left + right) * 0.5;
    const double center_y = (top + bottom) * 0.5;
    if (scale_x <= 0.0 || scale_y <= 0.0) return false;
    for (unsigned row = 0; row < 4; ++row) {
        const unsigned base = row * 4;
        const double homogeneous = projection[base + 3];
        projection[base] = static_cast<float>(
            (projection[base] - center_x * homogeneous) / scale_x);
        projection[base + 1] = static_cast<float>(
            (projection[base + 1] - center_y * homogeneous) / scale_y);
        if (!std::isfinite(projection[base]) ||
            !std::isfinite(projection[base + 1])) return false;
    }
    return true;
}

}  // namespace xrfg
