#include <pch.h>
#include "d3d11_guide_bridge.hpp"
#include <detours/detours.h>
#include "../../third_party/ofxr/include/xrfg/d3d11_dlss_guides.hpp"
namespace ofxr_bridge {
namespace {
using Microsoft::WRL::ComPtr;
using Copy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using Region=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
using Draw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using Indexed=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using Clear=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*);
using Dispatch=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT);
using Update=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
using Map=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
Copy original_copy{}; Region original_region{}; Draw original_draw{}; Indexed original_indexed{}; Clear original_clear{}; Dispatch original_dispatch{};
Update original_update{}; Map original_map{};
std::mutex hook_mutex;
bool claimed(ID3D11DeviceContext* context) {
    ComPtr<ID3D11Device> device; context->GetDevice(device.GetAddressOf()); return xrfg::d3d11_guides_claimed(device.Get());
}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext* context,ID3D11Resource* target,ID3D11Resource* source) {
    original_copy(context,target,source); if (claimed(context)) xrfg::propagate_d3d11_guide_receipt(target,source);
}
void STDMETHODCALLTYPE region(ID3D11DeviceContext* context,ID3D11Resource* target,UINT dst,UINT x,UINT y,UINT z,ID3D11Resource* source,UINT src,const D3D11_BOX* box) {
    original_region(context,target,dst,x,y,z,source,src,box); if (!claimed(context)) return;
    const auto receipt=xrfg::get_d3d11_guide_receipt(source);
    bool full=dst==0 && src==0 && x==0 && y==0 && z==0;
    if (box) full=full && box->left==0 && box->top==0 && box->front==0 && box->right==receipt.width && box->bottom==receipt.height && box->back==1;
    if (full) xrfg::propagate_d3d11_guide_receipt(target,source); else xrfg::clear_d3d11_guide_receipt(target);
}
void STDMETHODCALLTYPE clear(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,const FLOAT* color) {
    original_clear(context,target,color);
    if (claimed(context) && target) {ComPtr<ID3D11Resource> resource;target->GetResource(resource.GetAddressOf());xrfg::clear_d3d11_guide_receipt(resource.Get());}
}
void fullscreen(ID3D11DeviceContext* context,UINT count,UINT start) {
    if ((count!=3 && count!=6) || start!=0 || !claimed(context)) return;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{}; ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,depth.GetAddressOf());
    unsigned target_count{};ID3D11RenderTargetView* target{};
    for (auto* candidate:targets) if (candidate) {++target_count;target=candidate;}
    ComPtr<ID3D11Resource> destination;if (target) target->GetResource(destination.GetAddressOf());
    bool valid=target_count==1 && !depth;
    ComPtr<ID3D11BlendState> blend;FLOAT blend_factor[4]{};UINT blend_mask{};
    context->OMGetBlendState(blend.GetAddressOf(),blend_factor,&blend_mask);
    if (blend) {D3D11_BLEND_DESC desc{};blend->GetDesc(&desc);
        if (desc.RenderTarget[0].BlendEnable || desc.RenderTarget[0].RenderTargetWriteMask!=D3D11_COLOR_WRITE_ENABLE_ALL) valid=false;
    }
    D3D11_VIEWPORT viewport{};UINT viewports=1;context->RSGetViewports(&viewports,&viewport);
    if (viewports!=1 || viewport.TopLeftX!=0 || viewport.TopLeftY!=0) valid=false;
    ComPtr<ID3D11GeometryShader> geometry;context->GSGetShader(geometry.GetAddressOf(),nullptr,nullptr);
    ComPtr<ID3D11HullShader> hull;context->HSGetShader(hull.GetAddressOf(),nullptr,nullptr);
    ComPtr<ID3D11DomainShader> domain;context->DSGetShader(domain.GetAddressOf(),nullptr,nullptr);
    if (geometry || hull || domain) valid=false;
    ComPtr<ID3D11RasterizerState> raster;context->RSGetState(raster.GetAddressOf());
    if (raster) {D3D11_RASTERIZER_DESC desc{};raster->GetDesc(&desc);
        if (desc.ScissorEnable) {D3D11_RECT rect{};UINT rectangles=1;context->RSGetScissorRects(&rectangles,&rect);
            if (rectangles!=1 || rect.left>0 || rect.top>0 || rect.right<viewport.Width || rect.bottom<viewport.Height) valid=false;
        }
    }
    xrfg::D3D11GuideReceipt receipt{};
    if (destination) {
        ID3D11ShaderResourceView* sources[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        context->PSGetShaderResources(0,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,sources);
        for (auto* source:sources) {if (!source) continue;
            ComPtr<ID3D11Resource> resource;source->GetResource(resource.GetAddressOf());
            const auto candidate=xrfg::get_d3d11_guide_receipt(resource.Get());
            if (candidate.publication) {
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};source->GetDesc(&desc);
                if (desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || desc.Texture2D.MostDetailedMip!=0 ||
                    viewport.Width!=candidate.width || viewport.Height!=candidate.height ||
                    (receipt.publication && (receipt.owner!=candidate.owner || receipt.stream!=candidate.stream || receipt.publication!=candidate.publication))) valid=false;
                receipt=candidate;
            }
            source->Release();
        }
    }
    if (valid && receipt.publication) xrfg::set_d3d11_guide_receipt(destination.Get(),receipt);
    else if (receipt.publication) {
        for (auto* candidate:targets) if (candidate) {ComPtr<ID3D11Resource> resource;candidate->GetResource(resource.GetAddressOf());xrfg::clear_d3d11_guide_receipt(resource.Get());}
    }
    if (receipt.publication) {
        static std::atomic<unsigned> samples{};
        if (samples.fetch_add(1,std::memory_order_relaxed)<24)
            LOG_WARN("V33028 DX11 fullscreen provenance valid={} stream={} publication={} target={:p} viewport={}x{}",
                valid,receipt.stream,receipt.publication,static_cast<void*>(destination.Get()),viewport.Width,viewport.Height);
    }
    // Untagged small overlays preserve scene ownership. Partial copies,
    // clears and ambiguous full-frame passes invalidate it.
    for (auto* candidate:targets) if (candidate) candidate->Release();
}
void STDMETHODCALLTYPE draw(ID3D11DeviceContext* context,UINT count,UINT start) {original_draw(context,count,start);fullscreen(context,count,start);}
void STDMETHODCALLTYPE indexed(ID3D11DeviceContext* context,UINT count,UINT start,INT base) {original_indexed(context,count,start,base);if (base==0) fullscreen(context,count,start);}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* context,UINT x,UINT y,UINT z) {
    original_dispatch(context,x,y,z);if (!claimed(context)) return;
    ID3D11UnorderedAccessView* targets[D3D11_1_UAV_SLOT_COUNT]{};context->CSGetUnorderedAccessViews(0,D3D11_1_UAV_SLOT_COUNT,targets);
    for (auto* target:targets) if (target) {ComPtr<ID3D11Resource> resource;target->GetResource(resource.GetAddressOf());xrfg::clear_d3d11_guide_receipt(resource.Get());target->Release();}
}
void STDMETHODCALLTYPE update(ID3D11DeviceContext* context,ID3D11Resource* target,UINT subresource,const D3D11_BOX* box,const void* data,UINT pitch,UINT slice_pitch) {
    original_update(context,target,subresource,box,data,pitch,slice_pitch);
    if (claimed(context)) xrfg::clear_d3d11_guide_receipt(target);
}
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* context,ID3D11Resource* target,UINT subresource,D3D11_MAP mode,UINT flags,D3D11_MAPPED_SUBRESOURCE* result) {
    const HRESULT value=original_map(context,target,subresource,mode,flags,result);
    if (SUCCEEDED(value) && mode!=D3D11_MAP_READ && claimed(context)) xrfg::clear_d3d11_guide_receipt(target);
    return value;
}
}
void observe_d3d11_guide_context(ID3D11DeviceContext* context) {
    if (!context) return;
    std::scoped_lock lock(hook_mutex);if (original_copy) return;
    auto** table=*reinterpret_cast<void***>(context);
    original_copy=reinterpret_cast<Copy>(table[47]);original_region=reinterpret_cast<Region>(table[46]);
    original_draw=reinterpret_cast<Draw>(table[13]);original_indexed=reinterpret_cast<Indexed>(table[12]);
    original_clear=reinterpret_cast<Clear>(table[50]);original_dispatch=reinterpret_cast<Dispatch>(table[41]);
    original_update=reinterpret_cast<Update>(table[48]);original_map=reinterpret_cast<Map>(table[14]);
    LONG result=DetourTransactionBegin();if (result==NO_ERROR) result=DetourUpdateThread(GetCurrentThread());
#define OBSERVE_D11(Name,Hook) if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&Name),Hook)
    OBSERVE_D11(original_copy,copy);OBSERVE_D11(original_region,region);OBSERVE_D11(original_draw,draw);
    OBSERVE_D11(original_indexed,indexed);OBSERVE_D11(original_clear,clear);OBSERVE_D11(original_dispatch,dispatch);
    OBSERVE_D11(original_update,update);OBSERVE_D11(original_map,map);
#undef OBSERVE_D11
    if (result==NO_ERROR) result=DetourTransactionCommit();else DetourTransactionAbort();
    if (result!=NO_ERROR) {original_copy=nullptr;original_region=nullptr;original_draw=nullptr;original_indexed=nullptr;original_clear=nullptr;original_dispatch=nullptr;original_update=nullptr;original_map=nullptr;}
    LOG_WARN("V33028 DX11 resource provenance observer result={}",result);
}
}
