#pragma once
#include <d3d11_1.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace xrfg {
struct ColorIdentityTag {
    std::uint64_t application_serial{};
    std::uint64_t capture_serial{};
    std::uint64_t swapchain{};
    std::uint64_t pose_hash{};
    std::uint32_t image_index{};
    std::uint32_t eye{};
    std::uint32_t role{}; // 0 = actual AFW terminal, 1 = OFXR B, 2 = OFXR S
    std::uint32_t xr_image_index{}; // Local acquired index, not flattened interop index.
};
struct ColorIdentityResult {
    ColorIdentityTag tag;
    std::array<std::uint32_t, 4> hash{};
    std::uint32_t width{}, height{}, slices{}, format{};
};

// Full mip-zero color fingerprint. Reads the actual acquired XR texture, never
// its D3D12 mirror. Asynchronous staging ring; no CPU waits/flushes/visible mark.
// A busy ring is explicitly reported, never silently treated as verification.
class D3D11ColorIdentityProbe final {
public:
    D3D11ColorIdentityProbe(ID3D11Device*, ID3D11DeviceContext*);
    ~D3D11ColorIdentityProbe();
    HRESULT queue(ID3D11Texture2D*, const ColorIdentityTag&) noexcept;
    void poll(std::vector<ColorIdentityResult>* completed = nullptr) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
