#pragma once

#include <d3d12.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace xrfg {
// Compact Manchester token: sync16 + token32 + CRC16. Spatial copies share ID.
// Tokens are process-unique; the full publication stays in the guide registry.
inline constexpr unsigned kLukeOriginBits = 64;
inline constexpr unsigned kLukeOriginWidth = kLukeOriginBits * 4;
constexpr std::uint16_t luke_origin_crc(std::uint32_t token) noexcept {
    std::uint16_t crc = 0xffff;
    for (int byte = 3; byte >= 0; --byte) {
        crc ^= static_cast<std::uint16_t>((token >> (byte * 8)) & 255) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
constexpr bool luke_origin_bit(std::uint32_t token, unsigned bit) noexcept {
    if (bit < 16) return ((0xd3a5U >> (15 - bit)) & 1) != 0;
    if (bit < 48) return ((token >> (47 - bit)) & 1) != 0;
    return ((luke_origin_crc(token) >> (63 - bit)) & 1) != 0;
}
constexpr bool luke_origin_white(std::uint32_t token, unsigned x) noexcept {
    return (x % 4 < 2) == luke_origin_bit(token, x / 4);
}
struct LukeColorOriginDecode { std::uint32_t token{}; bool ambiguous{}; };
inline LukeColorOriginDecode decode_luke_origin_row(std::span<const float> row) noexcept {
    try {
        if (row.size() < 96 || row.size() > 16384) return {};
        // The physical R8 row can retain only three quantization steps of
        // marker contrast after overlay dimming. CRC/preamble still govern
        // identity; equal/erased Manchester pairs remain undecodable.
        constexpr float gradient_contrast = .001f;
        constexpr double edge_contrast = .002, bit_contrast = .001;
        struct Edge { double x; bool falling; };
        std::vector<Edge> edges; edges.reserve(row.size());
        for (std::size_t x = 0; x + 1 < row.size();) {
            const float first = row[x + 1] - row[x];
            if (!std::isfinite(first) || std::abs(first) < gradient_contrast) { ++x; continue; }
            const bool falling = first < 0;
            double weight{}, position{};
            do {
                const float difference = row[x + 1] - row[x];
                if (!std::isfinite(difference) || std::abs(difference) < gradient_contrast || (difference < 0) != falling) break;
                const double magnitude = std::abs(difference);
                weight += magnitude; position += (x + .5) * magnitude; ++x;
            } while (x + 1 < row.size());
            if (weight >= edge_contrast) edges.push_back({position / weight, falling});
        }
        const auto sample = [&](double x) {
            if (x < 0 || x > static_cast<double>(row.size() - 1)) return static_cast<double>(NAN);
            const auto a = static_cast<std::size_t>(x), b = std::min(a + 1, row.size() - 1);
            return row[a] + (row[b] - row[a]) * (x - a);
        };
        const auto prefix_matches = [&](double origin, double cell) {
            for (unsigned bit = 0; bit < 16; ++bit) {
                const double difference = sample(origin + (2. * bit - .5) * cell)
                    - sample(origin + (2. * bit + .5) * cell);
                if (!std::isfinite(difference) || std::abs(difference) < bit_contrast ||
                    (difference > 0) != (((0xd3a5U >> (15 - bit)) & 1) != 0)) return false;
            }
            return true;
        };
        const auto fit_edges = [&](double& origin, double& cell, unsigned count, bool known_prefix) {
            double n{}, sx{}, sy{}, sxx{}, sxy{};
            for (unsigned bit = 0; bit < count; ++bit) {
                const double target = origin + 2. * bit * cell;
                double distance = .4 * cell;
                const Edge* best{};
                auto it = std::lower_bound(edges.begin(), edges.end(), target - distance,
                    [](const Edge& edge, double value) { return edge.x < value; });
                for (; it != edges.end() && it->x <= target + distance; ++it) {
                    if (known_prefix && it->falling != (((0xd3a5U >> (15 - bit)) & 1) != 0)) continue;
                    const double error = std::abs(it->x - target);
                    if (error < distance) { distance = error; best = &*it; }
                }
                if (!best) continue;
                const double x = 2. * bit, y = best->x - origin;
                ++n; sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            if (n < (known_prefix ? 16. : 64.)) return false;
            const double pitch = (n * sxy - sx * sy) / (n * sxx - sx * sx);
            if (pitch < .7 || pitch > 4.05) return false;
            origin += (sy - pitch * sx) / n; cell = pitch;
            return true;
        };
        LukeColorOriginDecode result{};
        // Preamble bit0 and bit15 both have mandatory falling midpoint edges,
        // 30 cells apart. Infer a continuous pitch; never scan a rigid scale grid.
        for (std::size_t first = 0; first < edges.size(); ++first) {
            if (!edges[first].falling) continue;
            for (std::size_t last = first + 1; last < edges.size(); ++last) {
                const double length = edges[last].x - edges[first].x;
                if (length > 120.) break;
                if (length < 22.5 || !edges[last].falling) continue;
                double origin = edges[first].x, cell = length / 30.;
                if (!prefix_matches(origin, cell) || !fit_edges(origin, cell, 16, true)) continue;
                fit_edges(origin, cell, kLukeOriginBits, false);
                std::uint16_t checksum{}; std::uint32_t token{}; bool valid = true;
                for (unsigned bit = 0; bit < kLukeOriginBits; ++bit) {
                    const double difference = sample(origin + (2. * bit - .5) * cell)
                        - sample(origin + (2. * bit + .5) * cell);
                    if (!std::isfinite(difference) || std::abs(difference) < bit_contrast) { valid = false; break; }
                    const unsigned value = difference > 0 ? 1 : 0;
                    if (bit < 16) {
                        if (value != ((0xd3a5U >> (15 - bit)) & 1)) { valid = false; break; }
                    } else if (bit < 48) token = (token << 1) | value;
                    else checksum = static_cast<std::uint16_t>((checksum << 1) | value);
                }
                if (!valid || !token || checksum != luke_origin_crc(token)) continue;
                if (result.token && result.token != token) result.ambiguous = true;
                result.token = token;
            }
        }
        return result;
    } catch (...) { return {}; }
}
inline unsigned luke_origin_pixel_bytes(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    default: return 0;
    }
}
inline void luke_origin_pixel(std::byte* pixel, DXGI_FORMAT format, bool white) noexcept {
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const auto level = static_cast<std::uint16_t>(white ? 0x3c00 : 0);
        const std::uint16_t values[]{level, level, level, 0x3c00};
        std::memcpy(pixel, values, sizeof(values));
    } else if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        const float level = white ? 1.f : 0.f, values[]{level, level, level, 1.f};
        std::memcpy(pixel, values, sizeof(values));
    } else {
        const std::uint32_t value = format == DXGI_FORMAT_R10G10B10A2_UNORM
            ? (white ? 0xffffffffU : 0xc0000000U) : (white ? 0xffffffffU : 0xff000000U);
        std::memcpy(pixel, &value, sizeof(value));
    }
}
inline float luke_origin_luma(const std::byte* pixel, DXGI_FORMAT format) noexcept {
    // Decode all color channels in the stored/encoded domain. Closed color
    // processing can erase blue-only Manchester contrast while RGB retains it.
    // Ignore alpha; keep the existing marker, decoder thresholds and CRC.
    const auto luminance = [](float red, float green, float blue) {
        return .2126f * red + .7152f * green + .0722f * blue;
    };
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t channels[4]{}; std::memcpy(channels, pixel, sizeof(channels));
        const auto half = [](std::uint16_t bits) {
            const int exponent = (bits >> 10) & 31; const unsigned mantissa = bits & 1023;
            if (exponent == 31) return static_cast<float>(NAN);
            const float value = exponent ? std::ldexp(1.f + mantissa / 1024.f, exponent - 15)
                : std::ldexp(static_cast<float>(mantissa), -24);
            return bits & 0x8000 ? -value : value;
        };
        return luminance(half(channels[0]), half(channels[1]), half(channels[2]));
    }
    if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        float channels[4]{}; std::memcpy(channels, pixel, sizeof(channels));
        return luminance(channels[0], channels[1], channels[2]);
    }
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::uint32_t value{}; std::memcpy(&value, pixel, sizeof(value));
        return luminance((value & 1023) / 1023.f,
            ((value >> 10) & 1023) / 1023.f, ((value >> 20) & 1023) / 1023.f);
    }
    const auto channel = [&](unsigned index) {
        return std::to_integer<unsigned char>(pixel[index]) / 255.f;
    };
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return luminance(channel(0), channel(1), channel(2));
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return luminance(channel(2), channel(1), channel(0));
    default: return NAN;
    }
}
struct LukeColorOriginRect { unsigned x{}, y{}, width{}, height{}; };
inline constexpr unsigned kLukeOriginHeight = 8;
inline constexpr unsigned kLukeOriginMargin = 2;
struct LukeColorOriginLayout {
    std::array<LukeColorOriginRect,8> bands{};
    unsigned band_count{};
    LukeColorOriginRect scan;
};
// Cover every possible unscaled horizontal eye crop with complete, disjoint
// copies of ONE token. A narrow common intersection need not contain a marker.
// Geometry selects positions, never an eye, frame or alternate metadata source.
inline bool compact_luke_origin_layout(unsigned output_width, unsigned output_height,
    unsigned color_width, unsigned color_height, LukeColorOriginLayout& layout) noexcept {
    layout = {};
    if (!color_width || !color_height || output_width>16384 || output_height>8192 ||
        color_width > output_width || color_height > output_height ||
        color_width<kLukeOriginWidth+2*kLukeOriginMargin) return false;
    const unsigned dx = output_width - color_width, dy = output_height - color_height;
    if (static_cast<std::uint64_t>(dy) + kLukeOriginHeight + 2 * kLukeOriginMargin > color_height)
        return false;
    LukeColorOriginLayout candidate{};
    const unsigned capacity=color_width-kLukeOriginWidth-2*kLukeOriginMargin;
    unsigned x=std::min(dx,capacity)+kLukeOriginMargin,uncovered=0;
    for(;;) {
        if(candidate.band_count==candidate.bands.size()||
            x+kLukeOriginWidth+kLukeOriginMargin>output_width)return false;
        // A complete band survives crop starts in this inclusive interval.
        const unsigned end=x+kLukeOriginWidth+kLukeOriginMargin;
        const unsigned first=end>color_width?end-color_width:0;
        const unsigned last=x-kLukeOriginMargin;
        if(first>uncovered)return false; // Reject gaps; no inferred crop.
        candidate.bands[candidate.band_count++]={x,dy+kLukeOriginMargin,kLukeOriginWidth,kLukeOriginHeight};
        candidate.scan.width=std::min(color_width,end);
        if(last>=dx)break;
        uncovered=last+1;
        // Next placement covers the remaining crop interval without overwriting
        // any Manchester cell in the preceding copy.
        x=std::max(std::min(dx,uncovered+capacity)+kLukeOriginMargin,
            x+kLukeOriginWidth+2*kLukeOriginMargin);
    }
    candidate.scan.height=dy+kLukeOriginHeight+2*kLukeOriginMargin;
    layout=candidate;
    return true;
}
// Geometry negotiation is independent of identity. The first consumer knows
// the actual eye extent before any marker read; SR warmup may precede it.
void configure_luke_color_origin_extent(unsigned color_width, unsigned color_height) noexcept;

// Two immutable black/white cell uploads per device/format/height. No per-frame
// upload allocation, CPU rewrite, producer-submission receipt or 64-frame cap.
HRESULT mark_luke_color_origin(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
    std::uint32_t token, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    ID3D12Device* native_device = nullptr, LukeColorOriginRect rect = {}) noexcept;
// Reads only the compact corner ROI in one GPU submission/fence/map. Every
// row is independently CRC-checked; distinct valid tokens reject the batch.
// Missing/ambiguous/expired marker is an error; no selected-metadata fallback.
HRESULT read_luke_color_origin(ID3D12CommandQueue* queue, ID3D12Resource* color,
    std::uint32_t* token, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
    DWORD timeout_ms = 250) noexcept;
} // namespace xrfg
