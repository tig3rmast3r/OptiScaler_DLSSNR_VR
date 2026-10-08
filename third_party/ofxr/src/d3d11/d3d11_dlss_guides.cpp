#include "xrfg/d3d11_dlss_guides.hpp"
#include <mutex>
#include <unordered_map>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID receipt_guid{0x62935ac7, 0x5aac, 0x47d9, {0xaa,0x59,0x97,0x61,0x46,0x3f,0x96,0x28}};
struct Owner { ComPtr<IUnknown> identity; ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; std::uint64_t generation{}; bool claimed{}; };
struct OwnerRegistry {std::mutex mutex;std::unordered_map<std::uintptr_t,Owner> values;std::uint64_t generation{1};};
// Late SessionState destructors may still unregister after static containers
// start tearing down. Session erasure releases all retained COM owners.
auto* const owner_registry=new OwnerRegistry;
auto& owner_mutex=owner_registry->mutex;
auto& owners=owner_registry->values;
auto& next_generation=owner_registry->generation;
ComPtr<IUnknown> identity(IUnknown* object) {
    ComPtr<IUnknown> result;
    if (object) object->QueryInterface(IID_PPV_ARGS(result.GetAddressOf()));
    return result;
}
bool shape(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& description) {
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) return false;
    texture->GetDesc(&description);
    return description.ArraySize == 1 && description.MipLevels == 1 && description.SampleDesc.Count == 1;
}
}
void register_d3d11_guide_owner(std::uintptr_t session, ID3D11Device* source, ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    try {
        auto key=identity(source); ComPtr<ID3D12Device> queue_device;
        if (!session || !key || !device || !queue || FAILED(queue->GetDevice(IID_PPV_ARGS(queue_device.GetAddressOf()))) ||
            identity(device).Get()!=identity(queue_device.Get()).Get()) return;
        std::scoped_lock lock(owner_mutex);
        if (!next_generation) return;
        owners[session]={key,device,queue,next_generation++,false};
    } catch (...) {}
}
void unregister_d3d11_guide_owner(std::uintptr_t session) noexcept {
    try { std::scoped_lock lock(owner_mutex); owners.erase(session); } catch (...) {}
}
HRESULT acquire_d3d11_guide_owner(ID3D11Device* source, ID3D12Device** device, ID3D12CommandQueue** queue, std::uint64_t* generation) noexcept {
    if (!device || !queue || !generation) return E_INVALIDARG;
    *device=nullptr; *queue=nullptr; *generation=0;
    try {
        auto key=identity(source); std::scoped_lock lock(owner_mutex);
        const Owner* found{};
        for (const auto& [session,owner]:owners) {
            static_cast<void>(session);
            if (key && owner.identity.Get()==key.Get()) { if (found) return E_UNEXPECTED; found=&owner; }
        }
        if (!found) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        found->device.CopyTo(device); found->queue.CopyTo(queue); *generation=found->generation; return S_OK;
    } catch (...) { return E_FAIL; }
}
bool claim_d3d11_guides(ID3D11Device* source, std::uint64_t generation) noexcept {
    try { auto key=identity(source); std::scoped_lock lock(owner_mutex);
        for (auto& [session,owner]:owners) { static_cast<void>(session);
            if (key && owner.identity.Get()==key.Get() && owner.generation==generation) { owner.claimed=true; return true; }
        }
    } catch (...) {} return false;
}
bool d3d11_guides_claimed(ID3D11Device* source) noexcept {
    try {
        auto key=identity(source); std::scoped_lock lock(owner_mutex); const Owner* found{};
        for (const auto& [session,owner]:owners) {
            static_cast<void>(session);
            if (key && owner.identity.Get()==key.Get()) {if (found) return false;found=&owner;}
        }
        return found && found->claimed;
    } catch (...) {return false;}
}
void clear_d3d11_guide_receipt(ID3D11Resource* resource) noexcept {
    if (resource) resource->SetPrivateData(receipt_guid,0,nullptr);
}
void set_d3d11_guide_receipt(ID3D11Resource* resource, const D3D11GuideReceipt& receipt) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    if (!receipt.owner || !receipt.stream || !receipt.publication || !shape(resource,description) ||
        description.Width!=receipt.width || description.Height!=receipt.height) { clear_d3d11_guide_receipt(resource); return; }
    resource->SetPrivateData(receipt_guid,sizeof(receipt),&receipt);
}
D3D11GuideReceipt get_d3d11_guide_receipt(ID3D11Resource* resource) noexcept {
    D3D11GuideReceipt receipt{}; UINT size=sizeof(receipt);
    if (!resource || FAILED(resource->GetPrivateData(receipt_guid,&size,&receipt)) || size!=sizeof(receipt)) return {};
    D3D11_TEXTURE2D_DESC description{};
    if (!shape(resource,description) || description.Width!=receipt.width || description.Height!=receipt.height) return {};
    return receipt;
}
bool propagate_d3d11_guide_receipt(ID3D11Resource* destination, ID3D11Resource* source) noexcept {
    const auto receipt=get_d3d11_guide_receipt(source);
    set_d3d11_guide_receipt(destination,receipt);
    return get_d3d11_guide_receipt(destination).publication!=0;
}
std::shared_ptr<const DlssMotionVectorSet> resolve_d3d11_dlss_guides(ID3D11Resource* resource, ID3D12CommandQueue* consumer) noexcept {
    try {
        const auto receipt=get_d3d11_guide_receipt(resource);
        ComPtr<ID3D11Device> source; if (!resource || !receipt.publication) return {};
        resource->GetDevice(source.GetAddressOf());
        ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; std::uint64_t generation{};
        if (FAILED(acquire_d3d11_guide_owner(source.Get(),device.GetAddressOf(),queue.GetAddressOf(),&generation)) || generation!=receipt.owner ||
            identity(queue.Get()).Get()!=identity(consumer).Get()) return {};
        auto frame=resolve_dlss_guide_publication(receipt.stream,receipt.publication,consumer);
        if (!frame) return {};
        auto result=std::make_shared<DlssMotionVectorSet>(); result->eye_count=1; result->eyes[0]=std::move(frame); return result;
    } catch (...) { return {}; }
}
}
