#pragma once
#include <cmath>
#include <limits>
namespace xrfg {
// Row-vector perspective matrices encode view-space forward in m[11].
// OpenXR angular rays use right/up tangents; choose producer Z so clip W
// is positive for the forward ray, preserving off-axis center and scale.
[[nodiscard]] inline double luke_projection_ray_ndc(double tangent,
    double focal, double center, double perspective, double forward_w) noexcept {
    if (!std::isfinite(forward_w) || forward_w == 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    const double forward_z = std::copysign(1.0, forward_w);
    const double denominator = perspective * tangent + forward_w * forward_z;
    return denominator == 0.0
        ? std::numeric_limits<double>::quiet_NaN()
        : (focal * tangent + center * forward_z) / denominator;
}
} // namespace xrfg
