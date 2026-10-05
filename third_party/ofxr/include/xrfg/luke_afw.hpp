#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/embedded_control.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

namespace xrfg {

// Experimental Luke Ross spatial stereo completion.  Luke remains the
// alternating-eye producer; this object consumes one final real eye plus the
// matching DLSS depth/motion bundle and writes only the missing peer eye.
class LukeAfwSynthesizer final {
public:
    LukeAfwSynthesizer() noexcept;
    ~LukeAfwSynthesizer();

    LukeAfwSynthesizer(const LukeAfwSynthesizer&) = delete;
    LukeAfwSynthesizer& operator=(const LukeAfwSynthesizer&) = delete;

    [[nodiscard]] HRESULT initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        const std::filesystem::path& plugin_path) noexcept;

    [[nodiscard]] HRESULT synthesize(
        ID3D12Resource* source_color,
        ID3D12Resource* destination_color,
        const DlssMotionVectorFrame& guide,
        const D3D12ReprojectionView& source_view,
        const D3D12ReprojectionView& destination_view,
        std::uint32_t source_eye,
        float xr_eye_baseline,
        const embedded::AfwSettings& settings) noexcept;

    [[nodiscard]] bool initialized() const noexcept;

private:
    struct Impl;
    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
