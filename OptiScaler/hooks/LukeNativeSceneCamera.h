#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <detours/detours.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "D3D_NativeInterface.h"
#include "D3D_DeviceOwner.h"
#include "KcdCameraPacket.h"

namespace luke_scene_camera {
struct Receipt {
    bool installed{}, valid{};
    LONG install_error{};
    unsigned failure{}, attempts{}, reads{}, candidates{}, owner_mask{}, slot{};
    bool graphics{};
    HRESULT error{S_OK};
    std::uint32_t token{};
    std::uintptr_t resource{}, signature{};
    D3D12_GPU_VIRTUAL_ADDRESS location{};
    kcd_camera::Camera camera{};
};
namespace detail {
struct Resource { D3D12_GPU_VIRTUAL_ADDRESS gpu{}; std::uint64_t size{}; };
struct Roots {
    std::array<ID3D12RootSignature*,2> signatures{};
    std::array<std::array<D3D12_GPU_VIRTUAL_ADDRESS,32>,2> bindings{};
};
inline std::recursive_mutex mutex;
inline std::mutex install_mutex;
inline std::unordered_map<ID3D12Resource*,Resource> resources;
inline std::unordered_map<ID3D12GraphicsCommandList*,Roots> roots;
inline bool installed{}, shared_release{};
inline LONG install_error{};
inline std::uint32_t evaluations{};
using Address=D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
using ResourceRelease=ULONG(STDMETHODCALLTYPE*)(ID3D12Resource*);
using ListRelease=ULONG(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using Reset=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
using Bind=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS);
using Signature=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12RootSignature*);
inline Address original_address{};
inline ResourceRelease original_resource_release{};
inline ListRelease original_list_release{};
inline Reset original_reset{};
inline Bind original_compute{},original_graphics{};
inline Signature original_compute_signature{},original_graphics_signature{};
template<class Child> inline Microsoft::WRL::ComPtr<ID3D12Device> device_of(Child* child) {
    Microsoft::WRL::ComPtr<ID3D12Device> reported,retained;
    if(FAILED(child->GetDevice(IID_PPV_ARGS(reported.GetAddressOf()))))return retained;
    IUnknown* native=nullptr;
    if(d3d_native::get_borrowed(reported.Get(),&native))native->QueryInterface(IID_PPV_ARGS(retained.GetAddressOf()));
    return retained;
}
inline D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE address(ID3D12Resource* resource) {
    const auto gpu=original_address(resource);
    if(!gpu)return gpu;
    std::scoped_lock lock(mutex);
    if(!installed||resources.contains(resource)||resources.size()>=4096)return gpu;
    const auto desc=resource->GetDesc();
    if(desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)resources.emplace(resource,Resource{gpu,desc.Width});
    return gpu;
}
inline ULONG STDMETHODCALLTYPE release_resource(ID3D12Resource* resource) {
    std::scoped_lock lock(mutex);
    const auto result=original_resource_release(resource);
    if(!result) {
        if(const auto it=resources.find(resource);it!=resources.end()) {
            const auto range=it->second;
            for(auto& [list,ctx]:roots)for(auto& domain:ctx.bindings)for(auto& gpu:domain)
                if(gpu>=range.gpu&&gpu-range.gpu<range.size)gpu=0;
            resources.erase(it);
        }
        if(shared_release)roots.erase(reinterpret_cast<ID3D12GraphicsCommandList*>(resource));
    }
    return result;
}
inline ULONG STDMETHODCALLTYPE release_list(ID3D12GraphicsCommandList* list) {
    std::scoped_lock lock(mutex);const auto result=original_list_release(list);
    if(!result)roots.erase(list);return result;
}
inline Roots* context(ID3D12GraphicsCommandList* list) {
    if(auto it=roots.find(list);it!=roots.end())return &it->second;
    if(roots.size()>=256)return nullptr;
    return &roots.emplace(list,Roots{}).first->second;
}
inline void bind(ID3D12GraphicsCommandList* list,bool graphics,UINT slot,D3D12_GPU_VIRTUAL_ADDRESS gpu) {
    if(slot>=32)return;
    std::scoped_lock lock(mutex);
    if(installed)if(auto* ctx=context(list))ctx->bindings[graphics?1:0][slot]=gpu;
}
inline void STDMETHODCALLTYPE compute(ID3D12GraphicsCommandList* list,UINT slot,D3D12_GPU_VIRTUAL_ADDRESS gpu) {
    bind(list,false,slot,gpu);original_compute(list,slot,gpu);
}
inline void STDMETHODCALLTYPE graphics(ID3D12GraphicsCommandList* list,UINT slot,D3D12_GPU_VIRTUAL_ADDRESS gpu) {
    bind(list,true,slot,gpu);original_graphics(list,slot,gpu);
}
inline void signature(ID3D12GraphicsCommandList* list,bool graphics,ID3D12RootSignature* value) {
    std::scoped_lock lock(mutex);
    if(!installed)return;
    if(auto* ctx=context(list)) {
        const unsigned domain=graphics?1:0;
        if(ctx->signatures[domain]!=value)ctx->bindings[domain]={};
        ctx->signatures[domain]=value;
    }
}
inline void STDMETHODCALLTYPE compute_signature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* value) {
    signature(list,false,value);original_compute_signature(list,value);
}
inline void STDMETHODCALLTYPE graphics_signature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* value) {
    signature(list,true,value);original_graphics_signature(list,value);
}
inline HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline) {
    const auto result=original_reset(list,allocator,pipeline);
    if(SUCCEEDED(result)){std::scoped_lock lock(mutex);roots.erase(list);}return result;
}
} // detail
inline bool install(ID3D12Device* device) {
    std::scoped_lock installation(detail::install_mutex);
    if (detail::installed) return true;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    const auto cleanup = [&] {
        if (list) { list->Close(); list->Release(); }
        if (allocator) allocator->Release();
        if (resource) resource->Release();
    };
    if (!device || FAILED(device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&resource))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator, nullptr, IID_PPV_ARGS(&list)))) {
        detail::install_error = ERROR_NOT_SUPPORTED;
        cleanup();
        return false;
    }
    auto** resource_vtable = *reinterpret_cast<void***>(resource);
    detail::original_address = reinterpret_cast<detail::Address>(resource_vtable[11]);
    auto** list_vtable = *reinterpret_cast<void***>(list);
    detail::original_resource_release = reinterpret_cast<detail::ResourceRelease>(resource_vtable[2]);
    detail::original_list_release = reinterpret_cast<detail::ListRelease>(list_vtable[2]);
    detail::original_reset = reinterpret_cast<detail::Reset>(list_vtable[10]);
    detail::original_compute_signature = reinterpret_cast<detail::Signature>(list_vtable[29]);
    detail::original_graphics_signature = reinterpret_cast<detail::Signature>(list_vtable[30]);
    detail::original_compute = reinterpret_cast<detail::Bind>(list_vtable[37]);
    detail::original_graphics = reinterpret_cast<detail::Bind>(list_vtable[38]);
    detail::shared_release = resource_vtable[2] == list_vtable[2];
    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR) {
        error = DetourUpdateThread(GetCurrentThread());
        const auto attach = [&](auto& original, auto hook) {
            if (error == NO_ERROR) error = DetourAttach(
                reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(hook));
        };
        attach(detail::original_address, detail::address);
        attach(detail::original_resource_release, detail::release_resource);
        if (!detail::shared_release) attach(detail::original_list_release, detail::release_list);
        attach(detail::original_reset, detail::reset);
        attach(detail::original_compute_signature, detail::compute_signature);
        attach(detail::original_graphics_signature, detail::graphics_signature);
        attach(detail::original_compute, detail::compute);
        attach(detail::original_graphics, detail::graphics);
        if (error == NO_ERROR) error = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    {
        std::scoped_lock lock(detail::mutex);
        detail::install_error = error;
        detail::installed = error == NO_ERROR;
    }
    cleanup();
    return detail::installed;
}

inline bool uninstall() {
    std::scoped_lock installation(detail::install_mutex);
    if (!detail::installed) return true;
    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR) {
        error = DetourUpdateThread(GetCurrentThread());
        const auto detach = [&](auto& original, auto hook) {
            if (error == NO_ERROR) error = DetourDetach(
                reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(hook));
        };
        detach(detail::original_address, detail::address);
        detach(detail::original_resource_release, detail::release_resource);
        if (!detail::shared_release) detach(detail::original_list_release, detail::release_list);
        detach(detail::original_reset, detail::reset);
        detach(detail::original_compute_signature, detail::compute_signature);
        detach(detail::original_graphics_signature, detail::graphics_signature);
        detach(detail::original_compute, detail::compute);
        detach(detail::original_graphics, detail::graphics);
        if (error == NO_ERROR) error = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    std::scoped_lock lock(detail::mutex);
    if (error != NO_ERROR) { detail::install_error = error; return false; }
    detail::installed = false;
    detail::resources.clear();
    detail::roots.clear();


    return true;
}



// Consume only this production's still-bound constants on its exact native list.
// All addresses are consumed once. No global newest camera, old packet, eye guess,
// time window, GPU readback, retained resource reference or camera fallback.
inline Receipt take(ID3D12GraphicsCommandList* list,unsigned width,unsigned height) {
    std::scoped_lock lock(detail::mutex);
    Receipt result{};result.installed=detail::installed;result.install_error=detail::install_error;
    result.token=++detail::evaluations;
    if(!result.token)result.token=++detail::evaluations;
    IUnknown* native=nullptr;
    if(!detail::installed||!d3d_native::get_borrowed(list,&native)){result.failure=1;return result;}
    auto* actual=reinterpret_cast<ID3D12GraphicsCommandList*>(native);
    const auto it=detail::roots.find(actual);
    if(it==detail::roots.end()){result.failure=2;return result;}
    auto& ctx=it->second;
    const auto bindings=ctx.bindings;
    ctx.bindings={}; // A repeated NGX call cannot borrow the preceding production.
    const auto list_device=detail::device_of(actual);
    std::array<D3D12_GPU_VIRTUAL_ADDRESS,4> seen{};unsigned seen_count{};
    // The observed KCD2 per-view layout binds at slots1/2 in either domain.
    for(unsigned domain:{1u,0u})for(unsigned slot:{1u,2u}) {
        const auto gpu=bindings[domain][slot];
        if(!gpu||!ctx.signatures[domain])continue;
        bool duplicate=false;for(unsigned n=0;n<seen_count;++n)duplicate|=seen[n]==gpu;
        if(duplicate)continue;seen[seen_count++]=gpu;++result.attempts;
        for(const auto& [resource,range]:detail::resources) {
            if(gpu<range.gpu||gpu-range.gpu>=range.size)continue;
            const auto offset=gpu-range.gpu;
            if(range.size-offset<kcd_camera::packet_bytes){result.failure|=4;continue;}
            const auto resource_device=detail::device_of(resource);
            d3d_owner::Receipt proof;
            if(!d3d_owner::same(list_device.Get(),resource_device.Get(),&proof)) {
                result.failure|=8;result.error=E_ACCESSDENIED;continue;
            }
            std::array<unsigned char,kcd_camera::packet_bytes> bytes{};
            void* cpu=nullptr;D3D12_RANGE read_range{static_cast<SIZE_T>(offset),static_cast<SIZE_T>(offset)+bytes.size()};
            const auto mapped=resource->Map(0,&read_range,&cpu);
            result.error=mapped;
            if(FAILED(mapped)){result.failure|=16;break;}
            SIZE_T read{};
            const bool copied=cpu&&ReadProcessMemory(GetCurrentProcess(),static_cast<const unsigned char*>(cpu)+offset,
                bytes.data(),bytes.size(),&read)&&read==bytes.size();
            D3D12_RANGE no_write{};resource->Unmap(0,&no_write);
            if(!copied){result.failure|=32;break;}
            ++result.reads;kcd_camera::Camera camera{};
            if(!kcd_camera::decode(bytes.data(),bytes.size(),width,height,camera)){result.failure|=64;break;}
            ++result.candidates;
            if(result.candidates==1) {
                result.camera=camera;result.resource=reinterpret_cast<std::uintptr_t>(resource);
                result.signature=reinterpret_cast<std::uintptr_t>(ctx.signatures[domain]);
                result.location=gpu;result.graphics=domain==1;result.slot=slot;result.owner_mask=proof.mask;
            } else if(!kcd_camera::equivalent(camera,result.camera))result.failure|=128;
            break;
        }
    }
    result.valid=result.candidates!=0&&(result.failure&128)==0;
    if(!result.valid)result.camera={};
    return result;
}
} // namespace luke_scene_camera
