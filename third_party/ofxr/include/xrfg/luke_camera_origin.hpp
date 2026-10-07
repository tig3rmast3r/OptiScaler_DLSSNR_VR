#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace xrfg {

// AFW retains cameras across both eyes. A common origin must therefore remain
// fixed for the whole producer stream/epoch, rather than following each frame.
// This removes the large absolute-world translations before any float matrix
// products without changing stereo separation or temporal camera motion.
class LukeCameraOrigin {
    std::array<double, 3> origin_{};
    std::uint64_t stream_{};
    std::uint64_t epoch_{};
    bool initialized_{};

public:
    // nullopt: invalid input, state unchanged. true: new origin (the caller
    // must discard old AFW/camera histories). false: existing origin retained.
    [[nodiscard]] std::optional<bool> update(std::uint64_t stream,
        std::uint64_t epoch, const std::array<float, 3>& position,
        bool force_reset = false) noexcept {
        for (const float value : position)
            if (!std::isfinite(value)) return std::nullopt;

        const bool changed = !initialized_ || stream != stream_ ||
            epoch != epoch_ || force_reset;
        if (!changed) {
            for (unsigned axis = 0; axis < 3; ++axis)
                if (std::abs(static_cast<double>(position[axis]) - origin_[axis]) >
                    std::numeric_limits<float>::max()) return std::nullopt;
            return false;
        }
        for (unsigned axis = 0; axis < 3; ++axis)
            origin_[axis] = static_cast<double>(position[axis]);
        stream_ = stream;
        epoch_ = epoch;
        initialized_ = true;
        return true;
    }

    [[nodiscard]] std::array<float, 3> position_relative(
        const std::array<float, 3>& position) const noexcept {
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        std::array<float, 3> result{invalid, invalid, invalid};
        if (!initialized_) return result;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double relative = static_cast<double>(position[axis]) - origin_[axis];
            if (!std::isfinite(relative) ||
                std::abs(relative) > std::numeric_limits<float>::max())
                return {invalid, invalid, invalid};
            result[axis] = static_cast<float>(relative);
        }
        return result;
    }

    [[nodiscard]] const std::array<double, 3>& origin() const noexcept {
        return origin_;
    }
};

}  // namespace xrfg
