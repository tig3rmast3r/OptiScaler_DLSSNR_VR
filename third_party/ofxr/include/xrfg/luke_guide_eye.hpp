#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace xrfg {

// The two consecutive Luke NGX publications are ordered in time, not by eye.
// Project their camera separation onto the producer's camera-right direction
// to recover the physical eye independently of the serial's startup phase.
[[nodiscard]] inline std::optional<std::size_t> luke_guide_index_for_eye(
    const std::array<float, 3>& older_position,
    const std::array<float, 3>& older_right,
    const std::array<float, 3>& newer_position,
    const std::array<float, 3>& newer_right,
    std::uint32_t physical_eye) noexcept {
    if (physical_eye > 1) return std::nullopt;
    float separation = 0.0F;
    float right_agreement = 0.0F;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(older_position[axis]) ||
            !std::isfinite(newer_position[axis]) ||
            !std::isfinite(older_right[axis]) ||
            !std::isfinite(newer_right[axis])) return std::nullopt;
        separation += (newer_position[axis] - older_position[axis]) *
            (older_right[axis] + newer_right[axis]) * 0.5F;
        right_agreement += older_right[axis] * newer_right[axis];
    }
    if (!std::isfinite(separation) || separation == 0.0F ||
        !std::isfinite(right_agreement) || right_agreement <= 0.0F)
        return std::nullopt;
    const std::size_t left_index = separation > 0.0F ? 0U : 1U;
    return physical_eye == 0 ? left_index : 1U - left_index;
}

// AER publication parity remains stable within an NGX stream. After one
// physical-eye anchor, camera motion must not reassign later guides.
[[nodiscard]] constexpr std::optional<std::size_t>
luke_guide_index_for_serial_eye(
    std::uint64_t older_publication, std::uint64_t newer_publication,
    std::uint64_t last_publication, std::uint32_t last_eye,
    std::uint32_t released_eye) noexcept {
    if (older_publication == 0 || newer_publication <= older_publication ||
        last_publication == 0 || last_eye > 1 || released_eye > 1)
        return std::nullopt;
    const auto matches = [&](std::uint64_t publication) {
        return publication > last_publication &&
            (last_eye ^ ((publication - last_publication) & 1U)) ==
                released_eye;
    };
    if (matches(older_publication)) return 0;
    if (matches(newer_publication)) return 1;
    return std::nullopt;
}

}  // namespace xrfg
