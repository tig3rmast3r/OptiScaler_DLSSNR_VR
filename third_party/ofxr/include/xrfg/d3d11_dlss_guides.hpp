#pragma once
#include "dlss_motion_vectors.hpp"
#include <d3d11_4.h>

namespace xrfg {
// One actual OpenXR D3D11 device -> its authoritative DX12 bridge owner.
void register_d3d11_guide_owner(std::uintptr_t session, ID3D11Device*, ID3D12Device*, ID3D12CommandQueue*) noexcept;
void unregister_d3d11_guide_owner(std::uintptr_t session) noexcept;
HRESULT acquire_d3d11_guide_owner(ID3D11Device*, ID3D12Device**, ID3D12CommandQueue**, std::uint64_t*) noexcept;
bool claim_d3d11_guides(ID3D11Device*, std::uint64_t generation) noexcept;
bool d3d11_guides_claimed(ID3D11Device*) noexcept;
struct D3D11GuideReceipt {
    std::uint64_t owner{}, stream{}, publication{};
    std::uint32_t width{}, height{};
};
void set_d3d11_guide_receipt(ID3D11Resource*, const D3D11GuideReceipt&) noexcept;
D3D11GuideReceipt get_d3d11_guide_receipt(ID3D11Resource*) noexcept;
void clear_d3d11_guide_receipt(ID3D11Resource*) noexcept;
// Only a complete, same-size, single-slice copy carries ownership.
bool propagate_d3d11_guide_receipt(ID3D11Resource* destination, ID3D11Resource* source) noexcept;
std::shared_ptr<const DlssMotionVectorSet> resolve_d3d11_dlss_guides(ID3D11Resource*, ID3D12CommandQueue*) noexcept;
// Exact receipt lookup; never select another stream or a later publication.
std::shared_ptr<const DlssMotionVectorFrame> resolve_dlss_guide_publication(
    std::uint64_t stream, std::uint64_t publication, ID3D12CommandQueue*) noexcept;
}
