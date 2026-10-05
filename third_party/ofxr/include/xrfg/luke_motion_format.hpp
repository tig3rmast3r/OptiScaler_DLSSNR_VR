#pragma once
#include <dxgiformat.h>
namespace xrfg {
inline constexpr DXGI_FORMAT kLukeAfwMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
// Typed float2 input only: never reinterpret typeless, integer or UNORM data.
[[nodiscard]] constexpr DXGI_FORMAT luke_motion_srv_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT: return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
} // namespace xrfg
