#pragma once

#include "xrfg/pose.hpp"

#include <array>
#include <cmath>

namespace xrfg {

using LukeCameraBasis = std::array<std::array<float, 3>, 3>;

// The rows are local right, up and back vectors in world coordinates. Map the
// submitted OpenXR source-to-destination eye rotation into the NGX camera's
// local frame; the two world coordinate systems need not share an origin.
[[nodiscard]] inline bool luke_destination_camera_basis(
    Quaternion source_orientation, Quaternion destination_orientation,
    const LukeCameraBasis& source_basis, LukeCameraBasis& destination_basis) noexcept {
    const auto rows = [](Quaternion q, LukeCameraBasis& result) noexcept {
        const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        if (!std::isfinite(norm) || norm < 0.25F || norm > 4.0F) return false;
        const float scale = 1.0F / std::sqrt(norm);
        q.x *= scale;
        q.y *= scale;
        q.z *= scale;
        q.w *= scale;
        result = {{
            {1.0F - 2.0F * (q.y * q.y + q.z * q.z),
                2.0F * (q.x * q.y + q.z * q.w),
                2.0F * (q.x * q.z - q.y * q.w)},
            {2.0F * (q.x * q.y - q.z * q.w),
                1.0F - 2.0F * (q.x * q.x + q.z * q.z),
                2.0F * (q.y * q.z + q.x * q.w)},
            {2.0F * (q.x * q.z + q.y * q.w),
                2.0F * (q.y * q.z - q.x * q.w),
                1.0F - 2.0F * (q.x * q.x + q.y * q.y)}
        }};
        return true;
    };
    LukeCameraBasis source_xr{}, destination_xr{};
    if (!rows(source_orientation, source_xr) ||
        !rows(destination_orientation, destination_xr)) return false;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t coordinate = 0; coordinate < 3; ++coordinate) {
            float value = 0.0F;
            for (std::size_t source_axis = 0; source_axis < 3; ++source_axis) {
                float relative = 0.0F;
                for (std::size_t xr_axis = 0; xr_axis < 3; ++xr_axis)
                    relative += destination_xr[row][xr_axis] *
                        source_xr[source_axis][xr_axis];
                value += relative * source_basis[source_axis][coordinate];
            }
            if (!std::isfinite(value)) return false;
            destination_basis[row][coordinate] = value;
        }
    }
    return true;
}

// The NGX forward vector is the physical viewing direction; projection W
// selects whether local view Z points forwards or backwards.
[[nodiscard]] inline bool luke_view_camera_basis(const LukeCameraBasis& physical,
    float perspective_w, LukeCameraBasis& view) noexcept {
    if (!std::isfinite(perspective_w) || perspective_w == 0.0F) return false;
    view = physical;
    for (auto& row : view) for (float value : row)
        if (!std::isfinite(value)) return false;
    const float forward_z = std::copysign(1.0F, perspective_w);
    for (float& value : view[2]) value *= forward_z;
    return true;
}

[[nodiscard]] inline bool luke_destination_view_camera_basis(
    Quaternion source_orientation, Quaternion destination_orientation,
    const LukeCameraBasis& source_view, float perspective_w,
    LukeCameraBasis& destination_view) noexcept {
    if (!std::isfinite(perspective_w) || perspective_w == 0.0F) return false;
    const float back_to_view = -std::copysign(1.0F, perspective_w);
    auto xr_basis = source_view;
    for (float& value : xr_basis[2]) value *= back_to_view;
    if (!luke_destination_camera_basis(source_orientation, destination_orientation,
            xr_basis, destination_view)) return false;
    for (float& value : destination_view[2]) value *= back_to_view;
    return true;
}

}  // namespace xrfg
