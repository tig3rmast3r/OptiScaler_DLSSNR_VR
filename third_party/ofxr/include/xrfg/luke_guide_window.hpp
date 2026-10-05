#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace xrfg {

struct LukeGuideWindow {
    std::uint32_t x{}, y{}, width{}, height{};
};

// Project the physical OpenXR FOV through the producer's full DLSS matrix.
// The resulting ROI is in the guide's own pixel space, so it scales with any
// render/output extent and is not a title-specific pixel offset.
[[nodiscard]] inline std::optional<LukeGuideWindow> luke_guide_window(
    const std::array<float, 16>& projection,
    float angle_left, float angle_right, float angle_up, float angle_down,
    std::uint32_t output_width, std::uint32_t output_height,
    std::uint32_t guide_width, std::uint32_t guide_height,
    std::uint32_t color_width, std::uint32_t color_height) noexcept {
    if (!output_width || !output_height || !guide_width || !guide_height ||
        !color_width || !color_height || color_width > output_width ||
        color_height > output_height) return std::nullopt;
    for (float value : projection)
        if (!std::isfinite(value)) return std::nullopt;
    const auto ndc = [](double tangent, double focal, double center,
        double perspective, double depth) noexcept {
        const double denominator = perspective * tangent - depth;
        return denominator == 0.0
            ? std::numeric_limits<double>::quiet_NaN()
            : (focal * tangent - center) / denominator;
    };
    const double left = ndc(std::tan(angle_left), projection[0],
        projection[8], projection[3], projection[11]);
    const double right = ndc(std::tan(angle_right), projection[0],
        projection[8], projection[3], projection[11]);
    const double top = ndc(std::tan(angle_up), projection[5],
        projection[9], projection[7], projection[11]);
    const double bottom = ndc(std::tan(angle_down), projection[5],
        projection[9], projection[7], projection[11]);
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(top) || !std::isfinite(bottom) ||
        left >= right || bottom >= top) return std::nullopt;
    const double x0 = (left + 1.0) * output_width * 0.5;
    const double x1 = (right + 1.0) * output_width * 0.5;
    const double y0 = (1.0 - top) * output_height * 0.5;
    const double y1 = (1.0 - bottom) * output_height * 0.5;
    // A one-pixel difference is ordinary raster rounding; a materially
    // different FOV/colour extent is not a safe unscaled AFW guide window.
    if (x0 < -1.0 || y0 < -1.0 || x1 > output_width + 1.0 ||
        y1 > output_height + 1.0 ||
        std::abs((x1 - x0) - color_width) > 1.0 ||
        std::abs((y1 - y0) - color_height) > 1.0)
        return std::nullopt;
    const auto extent = [](std::uint32_t guide, std::uint32_t color,
        std::uint32_t output) noexcept {
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(guide) * color + output - 1) / output);
    };
    const std::uint32_t width = extent(guide_width, color_width,
        output_width);
    const std::uint32_t height = extent(guide_height, color_height,
        output_height);
    if (!width || !height || width > guide_width || height > guide_height)
        return std::nullopt;
    const auto rounded_x = std::llround(x0 * guide_width / output_width);
    const auto rounded_y = std::llround(y0 * guide_height / output_height);
    if (rounded_x < 0 || rounded_y < 0 ||
        rounded_x > guide_width || rounded_y > guide_height)
        return std::nullopt;
    // Quantizing the viewport origin and extent independently can put the
    // final texel one pixel past the resource edge. Keep that last texel in
    // bounds without changing the projected span or stretching the image.
    if (rounded_x > static_cast<std::int64_t>(guide_width - width) + 1 ||
        rounded_y > static_cast<std::int64_t>(guide_height - height) + 1)
        return std::nullopt;
    LukeGuideWindow result{
        static_cast<std::uint32_t>(std::min<std::int64_t>(rounded_x,
            guide_width - width)),
        static_cast<std::uint32_t>(std::min<std::int64_t>(rounded_y,
            guide_height - height)),
        width, height};
    return result;
}

}  // namespace xrfg
