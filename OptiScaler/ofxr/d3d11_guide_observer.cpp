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
using ClearUint=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const UINT*);
using ClearFloat=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const FLOAT*);
using Dispatch=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT);
using Update=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
using Map=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
Copy original_copy{};Region original_region{};Draw original_draw{};Indexed original_indexed{};Clear original_clear{};Dispatch original_dispatch{};
Update original_update{};Map original_map{};std::mutex hook_mutex;
ClearUint original_clear_uint{};ClearFloat original_clear_float{};
thread_local bool sr_rendering{};
bool claimed(ID3D11DeviceContext* context) {
    ComPtr<ID3D11Device> device;context->GetDevice(device.GetAddressOf());return xrfg::d3d11_guides_claimed(device.Get());
}
void invalidate(ID3D11Resource* resource) {
    xrfg::clear_d3d11_guide_receipt(resource);xrfg::clear_d3d11_sr_origin(resource);
}
void propagate_origin(ID3D11Resource* target,ID3D11Resource* source) {
    xrfg::set_d3d11_sr_origin(target,xrfg::get_d3d11_sr_origin(source));
}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext* context,ID3D11Resource* target,ID3D11Resource* source) {
    if (sr_rendering) {original_copy(context,target,source);return;}
    // At 100% CSP can present an eye by a complete copy instead of a shader
    // blit. Consume only an exact registered destination and identical extent.
    const auto origin=xrfg::get_d3d11_sr_origin(source);
    if (origin.frame && xrfg::is_d3d11_sr_destination(target)) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) {
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            const auto frame=xrfg::resolve_d3d11_sr_frame(origin.frame);
            if (frame && desc.Width==origin.width && desc.Height==origin.height) {
                ComPtr<ID3D11ShaderResourceView> result;HRESULT rendered=E_FAIL;sr_rendering=true;
                try {rendered=frame->render(context,source,target,result.GetAddressOf());} catch (...) {}
                sr_rendering=false;
                if (SUCCEEDED(rendered) && result) {
                    ComPtr<ID3D11Resource> output;result->GetResource(output.GetAddressOf());
                    original_copy(context,target,output.Get());xrfg::propagate_d3d11_guide_receipt(target,output.Get());
                    xrfg::clear_d3d11_sr_origin(target);frame->submitted(context);return;
                }
            }
        }
    }
    original_copy(context,target,source);
    if (xrfg::d3d11_sr_pending()) xrfg::observe_d3d11_sr_destination_copy(target,source);
    if (claimed(context)) xrfg::propagate_d3d11_guide_receipt(target,source);
    if (xrfg::d3d11_sr_pending()) propagate_origin(target,source);
    else xrfg::clear_d3d11_sr_origin(target);
}
void STDMETHODCALLTYPE region(ID3D11DeviceContext* context,ID3D11Resource* target,UINT dst,UINT x,UINT y,UINT z,ID3D11Resource* source,UINT src,const D3D11_BOX* box) {
    original_region(context,target,dst,x,y,z,source,src,box);
    const auto receipt=xrfg::get_d3d11_guide_receipt(source);
    const auto origin=xrfg::get_d3d11_sr_origin(source);
    const auto width=origin.frame?origin.width:receipt.width,height=origin.frame?origin.height:receipt.height;
    const bool full=dst==0 && src==0 && x==0 && y==0 && z==0 &&
        (!box || (box->left==0 && box->top==0 && box->front==0 && box->right==width && box->bottom==height && box->back==1));
    if (claimed(context)) {
        if (full) xrfg::propagate_d3d11_guide_receipt(target,source);else xrfg::clear_d3d11_guide_receipt(target);
    }
    if (full && origin.frame) xrfg::set_d3d11_sr_origin(target,origin);else xrfg::clear_d3d11_sr_origin(target);
}
void STDMETHODCALLTYPE clear(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,const FLOAT* color) {
    original_clear(context,target,color);
    if (target) {ComPtr<ID3D11Resource> resource;target->GetResource(resource.GetAddressOf());invalidate(resource.Get());}
}
void STDMETHODCALLTYPE clear_uint(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* target,const UINT* values) {
    original_clear_uint(context,target,values);
    if (!sr_rendering && target) {ComPtr<ID3D11Resource> resource;target->GetResource(resource.GetAddressOf());invalidate(resource.Get());}
}
void STDMETHODCALLTYPE clear_float(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* target,const FLOAT* values) {
    original_clear_float(context,target,values);
    if (!sr_rendering && target) {ComPtr<ID3D11Resource> resource;target->GetResource(resource.GetAddressOf());invalidate(resource.Get());}
}
struct FullscreenPass {
    bool valid{};
    ComPtr<ID3D11Resource> target;
    D3D11_TEXTURE2D_DESC target_desc{};
    D3D11_VIEWPORT viewport{};
    std::array<ComPtr<ID3D11ShaderResourceView>,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> sources;
};
FullscreenPass inspect(ID3D11DeviceContext* context,UINT count,UINT start) {
    FullscreenPass pass;
    ID3D11ShaderResourceView* sources[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    context->PSGetShaderResources(0,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,sources);
    for (std::size_t i=0;i<pass.sources.size();++i) pass.sources[i].Attach(sources[i]);
    bool valid=!start && (count==3 || count==6 || count==4);
    D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
    valid=valid && topology==(count==4?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ComPtr<ID3D11DepthStencilView> depth;context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,depth.GetAddressOf());
    unsigned target_count{};
    for (auto* target:targets) if (target) {++target_count;target->GetResource(pass.target.ReleaseAndGetAddressOf());target->Release();}
    ComPtr<ID3D11Texture2D> texture;
    if (!pass.target || FAILED(pass.target.As(&texture))) return pass;
    valid=valid && target_count==1 && !depth;
    texture->GetDesc(&pass.target_desc);
    UINT viewports=1;context->RSGetViewports(&viewports,&pass.viewport);
    if (viewports!=1 || pass.viewport.TopLeftX!=0 || pass.viewport.TopLeftY!=0 ||
        pass.viewport.Width!=pass.target_desc.Width || pass.viewport.Height!=pass.target_desc.Height ||
        pass.target_desc.ArraySize!=1 || pass.target_desc.MipLevels!=1 || pass.target_desc.SampleDesc.Count!=1) valid=false;
    ComPtr<ID3D11BlendState> blend;FLOAT factor[4]{};UINT mask{};context->OMGetBlendState(blend.GetAddressOf(),factor,&mask);
    if (!(mask&1)) valid=false;
    if (blend) {D3D11_BLEND_DESC desc{};blend->GetDesc(&desc);
        if (desc.RenderTarget[0].BlendEnable || desc.RenderTarget[0].RenderTargetWriteMask!=D3D11_COLOR_WRITE_ENABLE_ALL) valid=false;}
    ComPtr<ID3D11GeometryShader> geometry;context->GSGetShader(geometry.GetAddressOf(),nullptr,nullptr);
    ComPtr<ID3D11HullShader> hull;context->HSGetShader(hull.GetAddressOf(),nullptr,nullptr);
    ComPtr<ID3D11DomainShader> domain;context->DSGetShader(domain.GetAddressOf(),nullptr,nullptr);
    if (geometry || hull || domain) valid=false;
    ComPtr<ID3D11RasterizerState> raster;context->RSGetState(raster.GetAddressOf());
    if (raster) {D3D11_RASTERIZER_DESC desc{};raster->GetDesc(&desc);
        if (desc.ScissorEnable) {D3D11_RECT rect{};UINT rectangles=1;context->RSGetScissorRects(&rectangles,&rect);
            if (rectangles!=1 || rect.left>0 || rect.top>0 || rect.right<pass.viewport.Width || rect.bottom<pass.viewport.Height) valid=false;}}
    pass.valid=valid;return pass;
}
bool full_source(ID3D11ShaderResourceView* source) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};source->GetDesc(&desc);
    return desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D && desc.Texture2D.MostDetailedMip==0;
}
void publish(const FullscreenPass& pass) {
    if (!pass.target) return;
    auto valid=pass.valid;
    xrfg::D3D11GuideReceipt receipt{};xrfg::D3D11SuperResolutionOrigin origin{};
    for (const auto& source:pass.sources) {if (!source) continue;
        ComPtr<ID3D11Resource> resource;source->GetResource(resource.GetAddressOf());
        const auto candidate=xrfg::get_d3d11_guide_receipt(resource.Get());
        if (candidate.publication) {
            if (!full_source(source.Get()) || pass.viewport.Width!=candidate.width || pass.viewport.Height!=candidate.height ||
                (receipt.publication && (receipt.owner!=candidate.owner || receipt.stream!=candidate.stream || receipt.publication!=candidate.publication))) valid=false;
            receipt=candidate;
        }
        const auto sr=xrfg::get_d3d11_sr_origin(resource.Get());
        if (sr.frame) {
            if (!full_source(source.Get()) || pass.viewport.Width!=sr.width || pass.viewport.Height!=sr.height ||
                (origin.frame && origin.frame!=sr.frame)) valid=false;
            origin=sr;
        }
    }
    if (receipt.publication) {
        if (valid) xrfg::set_d3d11_guide_receipt(pass.target.Get(),receipt);else xrfg::clear_d3d11_guide_receipt(pass.target.Get());
    }
    if (origin.frame) {
        if (valid) xrfg::set_d3d11_sr_origin(pass.target.Get(),origin);else xrfg::clear_d3d11_sr_origin(pass.target.Get());
    }
}
struct SrDrawScope {
    ID3D11DeviceContext* context;
    FullscreenPass pass;
    std::shared_ptr<xrfg::D3D11SuperResolutionFrame> frame;
    UINT slot{};ComPtr<ID3D11ShaderResourceView> substituted;
    SrDrawScope(ID3D11DeviceContext* ctx,UINT count,UINT start,bool indexed_base_is_zero=true):context(ctx),pass(inspect(ctx,count,start)) {
        pass.valid=pass.valid && indexed_base_is_zero;
        if (!pass.valid || !xrfg::d3d11_sr_pending()) return;
        ComPtr<ID3D11Resource> actual_image;
        if (FAILED(xrfg::resolve_d3d11_sr_destination(pass.target.Get(),actual_image.GetAddressOf()))) return;
        xrfg::D3D11SuperResolutionOrigin origin{};ComPtr<ID3D11Resource> color;unsigned tagged{};
        for (UINT i=0;i<pass.sources.size();++i) {if (!pass.sources[i]) continue;
            ComPtr<ID3D11Resource> resource;pass.sources[i]->GetResource(resource.GetAddressOf());
            const auto candidate=xrfg::get_d3d11_sr_origin(resource.Get());
            if (!candidate.frame) continue;
            ++tagged;origin=candidate;slot=i;color=resource;
            if (!full_source(pass.sources[i].Get())) return;
        }
        if (tagged!=1) return;
        const auto pending=xrfg::resolve_d3d11_sr_frame(origin.frame);if (!pending) return;
        sr_rendering=true;HRESULT result=E_FAIL;
        try {result=pending->render(context,color.Get(),actual_image.Get(),substituted.GetAddressOf());} catch (...) {}
        sr_rendering=false;
        if (FAILED(result) || !substituted) return;
        auto* replacement=substituted.Get();context->PSSetShaderResources(slot,1,&replacement);frame=pending;
    }
    void complete() {
        // The actual SR source and proven presentation target have the same
        // full extent. CSP's final shader/color conversion and XR copy survive.
        if (frame) {
            FullscreenPass final_pass=pass;final_pass.sources[slot]=substituted;publish(final_pass);
            frame->submitted(context);
            auto* original=pass.sources[slot].Get();context->PSSetShaderResources(slot,1,&original);
        } else publish(pass);
    }
};
void STDMETHODCALLTYPE draw(ID3D11DeviceContext* context,UINT count,UINT start) {
    if (sr_rendering || (!xrfg::d3d11_sr_pending() && !claimed(context))) {original_draw(context,count,start);return;}
    SrDrawScope scope(context,count,start);original_draw(context,count,start);scope.complete();
}
void STDMETHODCALLTYPE indexed(ID3D11DeviceContext* context,UINT count,UINT start,INT base) {
    if (sr_rendering || (!xrfg::d3d11_sr_pending() && !claimed(context))) {original_indexed(context,count,start,base);return;}
    SrDrawScope scope(context,count,start,base==0);original_indexed(context,count,start,base);scope.complete();
}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* context,UINT x,UINT y,UINT z) {
    original_dispatch(context,x,y,z);if (sr_rendering) return;
    ID3D11UnorderedAccessView* targets[D3D11_1_UAV_SLOT_COUNT]{};context->CSGetUnorderedAccessViews(0,D3D11_1_UAV_SLOT_COUNT,targets);
    for (auto* target:targets) if (target) {ComPtr<ID3D11Resource> resource;target->GetResource(resource.GetAddressOf());invalidate(resource.Get());target->Release();}
}
void STDMETHODCALLTYPE update(ID3D11DeviceContext* context,ID3D11Resource* target,UINT subresource,const D3D11_BOX* box,const void* data,UINT row,UINT depth) {
    original_update(context,target,subresource,box,data,row,depth);invalidate(target);
}
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* context,ID3D11Resource* target,UINT subresource,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* data) {
    const auto result=original_map(context,target,subresource,type,flags,data);
    if (SUCCEEDED(result) && type!=D3D11_MAP_READ) invalidate(target);return result;
}
}
void observe_d3d11_guide_context(ID3D11DeviceContext* context) {
    if (!context) return;std::scoped_lock lock(hook_mutex);if (original_copy) return;
    auto** table=*reinterpret_cast<void***>(context);
    original_copy=reinterpret_cast<Copy>(table[47]);original_region=reinterpret_cast<Region>(table[46]);
    original_draw=reinterpret_cast<Draw>(table[13]);original_indexed=reinterpret_cast<Indexed>(table[12]);
    original_clear=reinterpret_cast<Clear>(table[50]);original_dispatch=reinterpret_cast<Dispatch>(table[41]);
    original_update=reinterpret_cast<Update>(table[48]);original_map=reinterpret_cast<Map>(table[14]);
    original_clear_uint=reinterpret_cast<ClearUint>(table[51]);original_clear_float=reinterpret_cast<ClearFloat>(table[52]);
    LONG result=DetourTransactionBegin();if (result==NO_ERROR) result=DetourUpdateThread(GetCurrentThread());
#define OBSERVE_D11(original,hook) if(result==NO_ERROR) result=DetourAttach(reinterpret_cast<PVOID*>(&original),reinterpret_cast<PVOID>(hook))
    OBSERVE_D11(original_copy,copy);OBSERVE_D11(original_region,region);OBSERVE_D11(original_draw,draw);
    OBSERVE_D11(original_indexed,indexed);OBSERVE_D11(original_clear,clear);OBSERVE_D11(original_dispatch,dispatch);
    OBSERVE_D11(original_update,update);OBSERVE_D11(original_map,map);
    OBSERVE_D11(original_clear_uint,clear_uint);OBSERVE_D11(original_clear_float,clear_float);
#undef OBSERVE_D11
    if (result==NO_ERROR) result=DetourTransactionCommit();else DetourTransactionAbort();
    if (result!=NO_ERROR) {original_copy=nullptr;original_region=nullptr;original_draw=nullptr;original_indexed=nullptr;original_clear=nullptr;original_dispatch=nullptr;original_update=nullptr;original_map=nullptr;original_clear_uint=nullptr;original_clear_float=nullptr;}
}
}
