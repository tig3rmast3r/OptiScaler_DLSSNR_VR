#pragma once
#include <dxgiformat.h>
namespace xrfg {
inline constexpr DXGI_FORMAT kLukeAfwMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
// Motion is XY of a typed float2/float4 texture. Extra ZW channels are not motion.
// Never reinterpret typeless, integer or UNORM data.
[[nodiscard]] constexpr DXGI_FORMAT luke_motion_srv_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
} // namespace xrfg
