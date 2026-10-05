#include "xrfg/d3d11_color_identity.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <new>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char shader_source[] = R"(
Texture2DArray<float4> image : register(t0);
RWByteAddressBuffer outputHash : register(u0);
groupshared uint4 localHash[256];
uint mix(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15;
                  x *= 0x846ca68b; return x ^ (x >> 16); }
[numthreads(16,16,1)]
void main(uint3 p : SV_DispatchThreadID, uint i : SV_GroupIndex) {
    uint w,h,slices; image.GetDimensions(w,h,slices);
    uint4 v=0;
    if(p.x<w && p.y<h && p.z<slices) {
        uint4 bits=asuint(image.Load(int4(p,0)));
        uint n=p.x+w*(p.y+h*p.z);
        v=uint4(mix(bits.x^mix(n^0x13579bdf)),
                mix(bits.y^mix(n^0x2468ace0)),
                mix(bits.z^mix(n^0x98765432)),
                mix(bits.w^mix(n^0xfedcba98)));
    }
    localHash[i]=v; GroupMemoryBarrierWithGroupSync();
    for(uint step=128;step>0;step>>=1) {
        if(i<step) localHash[i]+=localHash[i+step];
        GroupMemoryBarrierWithGroupSync();
    }
    if(i==0) {
        outputHash.InterlockedAdd(0,localHash[0].x);
        outputHash.InterlockedAdd(4,localHash[0].y);
        outputHash.InterlockedAdd(8,localHash[0].z);
        outputHash.InterlockedAdd(12,localHash[0].w);
    }
})";
void record(const ColorIdentityTag& tag, std::uint64_t stage, std::uint64_t data) {
    bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
        static_cast<std::int64_t>(tag.role * 256 + tag.eye),
        stage, tag.application_serial, data);
}
DXGI_FORMAT typed_format(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}
}
struct D3D11ColorIdentityProbe::Impl {
    struct Slot {
        ComPtr<ID3D11Buffer> readback;
        ComPtr<ID3D11Query> ready;
        ColorIdentityResult result;
        bool pending{};
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Texture2D> image;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Buffer> hash;
    ComPtr<ID3D11UnorderedAccessView> uav;
    std::array<Slot,96> slots;
    D3D11_TEXTURE2D_DESC description{};
    HRESULT initialized{S_FALSE};

    HRESULT initialize() {
        if(initialized != S_FALSE) return initialized;
        ComPtr<ID3D11Device1> device1;
        HRESULT hr=device.As(&device1);
        if(SUCCEEDED(hr)) hr=context.As(&context1);
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL level{};
        if(SUCCEEDED(hr)) hr=device1->CreateDeviceContextState(0, levels, 1,
            D3D11_SDK_VERSION, __uuidof(ID3D11Device), &level, &state);
        ComPtr<ID3DBlob> code, errors;
        if(SUCCEEDED(hr)) hr=D3DCompile(shader_source, sizeof(shader_source)-1,
            "AFWColorIdentity", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0,
            &code, &errors);
        if(SUCCEEDED(hr)) hr=device->CreateComputeShader(code->GetBufferPointer(),
            code->GetBufferSize(), nullptr, &shader);
        D3D11_BUFFER_DESC b{}; b.ByteWidth=16; b.Usage=D3D11_USAGE_DEFAULT;
        b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if(SUCCEEDED(hr)) hr=device->CreateBuffer(&b,nullptr,&hash);
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format=DXGI_FORMAT_R32_TYPELESS; u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements=4; u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if(SUCCEEDED(hr)) hr=device->CreateUnorderedAccessView(hash.Get(),&u,&uav);
        b.Usage=D3D11_USAGE_STAGING; b.BindFlags=0; b.MiscFlags=0;
        b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        const D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
        for(auto& slot:slots) {
            if(SUCCEEDED(hr)) hr=device->CreateBuffer(&b,nullptr,&slot.readback);
            if(SUCCEEDED(hr)) hr=device->CreateQuery(&q,&slot.ready);
        }
        initialized=hr;
        return hr;
    }
    HRESULT prepare_image(const D3D11_TEXTURE2D_DESC& source) {
        if(source.SampleDesc.Count!=1 || !source.ArraySize || !source.MipLevels)
            return E_INVALIDARG;
        if(image && description.Width==source.Width && description.Height==source.Height &&
            description.ArraySize==source.ArraySize && description.Format==source.Format) return S_OK;
        srv.Reset(); image.Reset();
        auto d=source; d.MipLevels=1; d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags=0; d.MiscFlags=0;
        HRESULT hr=device->CreateTexture2D(&d,nullptr,&image);
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format=typed_format(d.Format); s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        s.Texture2DArray.MipLevels=1; s.Texture2DArray.ArraySize=d.ArraySize;
        if(SUCCEEDED(hr)) hr=device->CreateShaderResourceView(image.Get(),&s,&srv);
        if(SUCCEEDED(hr)) description=source;
        return hr;
    }
};
D3D11ColorIdentityProbe::D3D11ColorIdentityProbe(ID3D11Device* d, ID3D11DeviceContext* c)
    : impl_(std::make_unique<Impl>()) { impl_->device=d; impl_->context=c; }
D3D11ColorIdentityProbe::~D3D11ColorIdentityProbe()=default;

void D3D11ColorIdentityProbe::poll(std::vector<ColorIdentityResult>* completed) noexcept {
    try {
        for(auto& slot:impl_->slots) {
            if(!slot.pending) continue;
            BOOL ready=FALSE;
            const HRESULT hr=impl_->context->GetData(slot.ready.Get(), &ready, sizeof(ready),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE || (SUCCEEDED(hr) && !ready)) continue;
            if(FAILED(hr)) { record(slot.result.tag,61,static_cast<std::uint32_t>(hr));
                slot.pending=false; continue; }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT map=impl_->context->Map(slot.readback.Get(),0,D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(map==DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if(FAILED(map)) { record(slot.result.tag,61,static_cast<std::uint32_t>(map));
                slot.pending=false; continue; }
            std::memcpy(slot.result.hash.data(),mapped.pData,16);
            impl_->context->Unmap(slot.readback.Get(),0);
            const auto& h=slot.result.hash;
            record(slot.result.tag,58,(static_cast<std::uint64_t>(h[0])<<32)|h[1]);
            record(slot.result.tag,59,(static_cast<std::uint64_t>(h[2])<<32)|h[3]);
            slot.pending=false;
            if(completed) completed->push_back(slot.result);
        }
    } catch(...) {}
}

HRESULT D3D11ColorIdentityProbe::queue(ID3D11Texture2D* source,
    const ColorIdentityTag& tag) noexcept {
    try {
        poll();
        HRESULT hr=source && impl_->device && impl_->context ? impl_->initialize() : E_POINTER;
        if(FAILED(hr)) { record(tag,61,static_cast<std::uint32_t>(hr)); return hr; }
        Impl::Slot* available=nullptr;
        for(auto& slot:impl_->slots) if(!slot.pending) { available=&slot; break; }
        if(!available) { record(tag,61,ERROR_BUSY); return HRESULT_FROM_WIN32(ERROR_BUSY); }
        D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d);
        hr=impl_->prepare_image(d);
        if(FAILED(hr)) { record(tag,61,static_cast<std::uint32_t>(hr)); return hr; }
        // Full context-state swap preserves all shader stages, bindings and
        // counters. The application context/its work are not redirected.
        ComPtr<ID3DDeviceContextState> previous;
        impl_->context1->SwapDeviceContextState(impl_->state.Get(), &previous);
        for(UINT slice=0;slice<d.ArraySize;++slice)
            impl_->context->CopySubresourceRegion(impl_->image.Get(),slice,0,0,0,
                source,D3D11CalcSubresource(0,slice,d.MipLevels),nullptr);
        const UINT zero[4]{};
        impl_->context->ClearUnorderedAccessViewUint(impl_->uav.Get(),zero);
        auto* srv=impl_->srv.Get(); auto* uav=impl_->uav.Get();
        impl_->context->CSSetShader(impl_->shader.Get(),nullptr,0);
        impl_->context->CSSetShaderResources(0,1,&srv);
        impl_->context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        impl_->context->Dispatch((d.Width+15)/16,(d.Height+15)/16,d.ArraySize);
        impl_->context->CopyResource(available->readback.Get(),impl_->hash.Get());
        impl_->context->End(available->ready.Get());
        impl_->context1->SwapDeviceContextState(previous.Get(),nullptr);
        available->result={tag,{},d.Width,d.Height,d.ArraySize,static_cast<UINT>(d.Format)};
        available->pending=true;
        record(tag,57,tag.image_index);
        record(tag,62,tag.capture_serial);
        record(tag,63,tag.pose_hash);
        record(tag,64,tag.swapchain);
        record(tag,65,(static_cast<std::uint64_t>(d.Width)<<32)|d.Height);
        record(tag,66,(static_cast<std::uint64_t>(d.ArraySize)<<32)|static_cast<UINT>(d.Format));
        record(tag,67,tag.xr_image_index);
        return S_OK;
    } catch(const std::bad_alloc&) { record(tag,61,E_OUTOFMEMORY); return E_OUTOFMEMORY; }
      catch(...) { record(tag,61,E_FAIL); return E_FAIL; }
}
}
