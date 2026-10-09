#include <pch.h>
#include "csp_super_resolution.hpp"
#include "d3d11_guide_bridge.hpp"
#include "../../third_party/ofxr/include/xrfg/d3d11_dlss_guides.hpp"
#include <array>
#include <vector>
#include <cmath>

namespace ofxr_bridge {
namespace {
using Microsoft::WRL::ComPtr;
bool same(IUnknown* a,IUnknown* b) {
    ComPtr<IUnknown> left,right;
    if (a) a->QueryInterface(IID_PPV_ARGS(left.GetAddressOf()));
    if (b) b->QueryInterface(IID_PPV_ARGS(right.GetAddressOf()));
    return left && left.Get()==right.Get();
}
bool shape(ID3D11Resource* resource,D3D11_TEXTURE2D_DESC& desc) {
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) return false;
    texture->GetDesc(&desc);
    return desc.ArraySize==1 && desc.MipLevels==1 && desc.SampleDesc.Count==1;
}
constexpr const char* integer_keys[]={
    NVSDK_NGX_Parameter_Width,NVSDK_NGX_Parameter_Height,NVSDK_NGX_Parameter_OutWidth,NVSDK_NGX_Parameter_OutHeight,
    NVSDK_NGX_Parameter_PerfQualityValue,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,NVSDK_NGX_Parameter_Reset,
    NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
    NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
    NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
    NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
    NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
    NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X,NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y,
    NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
    NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
    NVSDK_NGX_Parameter_TonemapperType,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance
};
constexpr const char* float_keys[]={
    NVSDK_NGX_Parameter_Sharpness,NVSDK_NGX_Parameter_Jitter_Offset_X,NVSDK_NGX_Parameter_Jitter_Offset_Y,
    NVSDK_NGX_Parameter_MV_Scale_X,NVSDK_NGX_Parameter_MV_Scale_Y,
    NVSDK_NGX_Parameter_MV_Offset_X,NVSDK_NGX_Parameter_MV_Offset_Y,
    NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,NVSDK_NGX_Parameter_DLSS_Pre_Exposure,NVSDK_NGX_Parameter_DLSS_Exposure_Scale
};
constexpr const char* resource_keys[]={
    NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_ExposureTexture,
    NVSDK_NGX_Parameter_TransparencyMask,NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask
};
void scalars(NVSDK_NGX_Parameter* source,NVSDK_NGX_Parameter* target) {
    for (const auto* key:integer_keys) {unsigned value{};if (source->Get(key,&value)==NVSDK_NGX_Result_Success) target->Set(key,value);}
    for (const auto* key:float_keys) {float value{};if (source->Get(key,&value)==NVSDK_NGX_Result_Success) target->Set(key,value);}
}
struct ContextStateScope {
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> saved;
    ContextStateScope(ID3D11DeviceContext1* ctx,ID3DDeviceContextState* clean):context(ctx) {
        context->SwapDeviceContextState(clean,saved.GetAddressOf());
    }
    ~ContextStateScope() {context->SwapDeviceContextState(saved.Get(),nullptr);}
};
}
struct CspD3D11SuperResolution::Impl : std::enable_shared_from_this<Impl> {
    struct Frame final : xrfg::D3D11SuperResolutionFrame {
        std::weak_ptr<Impl> owner;
        std::uint64_t serial{};
        HRESULT render(ID3D11DeviceContext* context,ID3D11Resource* color,ID3D11Resource* destination,ID3D11ShaderResourceView** result) override {
            const auto state=owner.lock();return state?state->render(serial,context,color,destination,result):E_ABORT;
        }
        void submitted(ID3D11DeviceContext* context) noexcept override {
            if (const auto state=owner.lock()) state->submitted(serial,context);
        }
    };
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3DDeviceContextState> clean_state;
    ComPtr<ID3D11Fence> fence;
    HANDLE event{};
    NVSDK_NGX_Parameter* parameters{};
    ParametersApi parameters_api;
    Render callback;
    std::array<ComPtr<ID3D11Texture2D>,std::size(resource_keys)> guides;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11ShaderResourceView> output_view;
    std::shared_ptr<Frame> frame;
    std::uint64_t fence_value{},serial{};
    unsigned width{},height{},render_width{},render_height{},warnings{},completed{};
    bool closed{},submitted_work{},force_reset{true};
    ~Impl() {
        if (parameters && parameters_api.destroy) parameters_api.destroy(parameters);
        if (event) CloseHandle(event);
    }
    void warn(const char* reason) {if (warnings++<12) LOG_WARN("CSP owned SR: {}",reason);}
    bool signal() {
        submitted_work=true;
        if (FAILED(context->Signal(fence.Get(),++fence_value))) return false;
        return true;
    }
    bool drain_locked() {
        if (!submitted_work || FAILED(device->GetDeviceRemovedReason())) return true;
        if (fence->GetCompletedValue()>=fence_value) return true;
        if (FAILED(fence->SetEventOnCompletion(fence_value,event))) return false;
        context->Flush();return WaitForSingleObject(event,2000)==WAIT_OBJECT_0;
    }
    bool copy_guide(std::size_t index,ID3D11Resource* source) {
        D3D11_TEXTURE2D_DESC desc{};if (!shape(source,desc)) return false;
        ComPtr<ID3D11Device> source_device;source->GetDevice(source_device.GetAddressOf());
        if (!same(source_device.Get(),device.Get())) return false;
        D3D11_TEXTURE2D_DESC existing{};if (guides[index]) guides[index]->GetDesc(&existing);
        if (!guides[index] || existing.Width!=desc.Width || existing.Height!=desc.Height || existing.Format!=desc.Format) {
            if (!drain_locked()) return false;
            desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=desc.MiscFlags=0;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> replacement;
            if (FAILED(device->CreateTexture2D(&desc,nullptr,replacement.GetAddressOf()))) return false;
            guides[index]=std::move(replacement);
        }
        context->CopyResource(guides[index].Get(),source);
        parameters->Set(resource_keys[index],static_cast<ID3D11Resource*>(guides[index].Get()));return true;
    }
    void drop_frame() {
        xrfg::retire_d3d11_sr_frame(serial);serial=0;frame.reset();
    }
    HRESULT render(std::uint64_t id,ID3D11DeviceContext* ctx,ID3D11Resource* color,ID3D11Resource* destination,ID3D11ShaderResourceView** result) {
        if (!result) return E_POINTER;*result=nullptr;
        std::scoped_lock lock(mutex);
        if (closed || !id || id!=serial || !same(ctx,context.Get()) || !xrfg::is_d3d11_sr_destination(destination)) return E_ABORT;
        D3D11_TEXTURE2D_DESC source{},target{};
        if (!shape(color,source) || !shape(destination,target) || source.Width!=width || source.Height!=height ||
            target.Width<width || target.Height<height ||
            std::abs(static_cast<double>(target.Width)/width-static_cast<double>(target.Height)/height)>0.01) return E_INVALIDARG;
        ComPtr<ID3D11Device> target_device;destination->GetDevice(target_device.GetAddressOf());
        if (!same(target_device.Get(),device.Get())) return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC existing{};if (output) output->GetDesc(&existing);
        if ((render_width!=width || render_height!=height) && !drain_locked()) return DXGI_ERROR_WAS_STILL_DRAWING;
        if (!output || existing.Width!=target.Width || existing.Height!=target.Height || existing.Format!=source.Format) {
            if (!drain_locked()) {warn("resize waits for unretired GPU work");return DXGI_ERROR_WAS_STILL_DRAWING;}
            auto desc=source;desc.Width=target.Width;desc.Height=target.Height;
            desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=desc.MiscFlags=0;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> replacement;ComPtr<ID3D11ShaderResourceView> view;
            HRESULT hr=device->CreateTexture2D(&desc,nullptr,replacement.GetAddressOf());
            if (SUCCEEDED(hr)) hr=device->CreateShaderResourceView(replacement.Get(),nullptr,view.GetAddressOf());
            if (FAILED(hr)) {warn("full-size UAV/SRV allocation failed");return hr;}
            output=std::move(replacement);output_view=std::move(view);force_reset=true;
        }
        parameters->Set(NVSDK_NGX_Parameter_Color,color);
        xrfg::clear_d3d11_guide_receipt(output.Get());
        xrfg::clear_d3d11_sr_origin(output.Get());
        parameters->Set(NVSDK_NGX_Parameter_Output,static_cast<ID3D11Resource*>(output.Get()));
        parameters->Set(NVSDK_NGX_Parameter_OutWidth,target.Width);parameters->Set(NVSDK_NGX_Parameter_OutHeight,target.Height);
        if (force_reset) parameters->Set(NVSDK_NGX_Parameter_Reset,1u);
        bool success=false;
        {
            // DLSS/NR and their shader passes must not disturb the CSP draw
            // currently intercepted, including null bindings and PS/IA state.
            ContextStateScope state(context.Get(),clean_state.Get());
            try {success=callback(ctx,parameters,target.Width,target.Height);} catch (...) {warn("native DLSS callback threw");}
        }
        if (!signal()) success=false;
        if (!success) {force_reset=true;warn("native DLSS full-size dispatch failed");drop_frame();return E_FAIL;}
        render_width=width;render_height=height;
        force_reset=false;
        if (completed++<4) LOG_INFO("CSP owned SR: native DLSS {}x{} -> {}x{}, exact XR eye output",width,height,target.Width,target.Height);
        return output_view.CopyTo(result);
    }
    void submitted(std::uint64_t id,ID3D11DeviceContext* ctx) noexcept {
        try {std::scoped_lock lock(mutex);
            if (!closed && id==serial && same(ctx,context.Get())) {if (!signal()) warn("final eye fence signal failed");drop_frame();}
        } catch (...) {}
    }
};
CspD3D11SuperResolution::CspD3D11SuperResolution(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* initial,ParametersApi parameters_api,Render callback) {
    auto state=std::make_shared<Impl>();context->GetDevice(state->device.GetAddressOf());
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11Device1> device1;
    if (context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || FAILED(context->QueryInterface(IID_PPV_ARGS(state->context.GetAddressOf()))) ||
        FAILED(state->device.As(&device5)) || FAILED(state->device.As(&device1)) ||
        FAILED(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(state->fence.GetAddressOf())))) return;
    const auto level=state->device->GetFeatureLevel();D3D_FEATURE_LEVEL selected{};
    if (FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&selected,state->clean_state.GetAddressOf()))) return;
    state->event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    state->parameters_api=std::move(parameters_api);
    if (!state->event || !state->parameters_api.allocate || !state->parameters_api.destroy ||
        state->parameters_api.allocate(&state->parameters)!=NVSDK_NGX_Result_Success) return;
    scalars(initial,state->parameters);state->callback=std::move(callback);impl_=std::move(state);
    observe_d3d11_guide_context(context);
}
CspD3D11SuperResolution::~CspD3D11SuperResolution() {retire();}
bool CspD3D11SuperResolution::ready() const {return static_cast<bool>(impl_);}
bool CspD3D11SuperResolution::prepare(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* input) {
    if (!impl_) return false;auto& state=*impl_;std::scoped_lock lock(state.mutex);
    if (state.closed || !same(context,state.context.Get())) return false;
    if (state.serial) {state.force_reset=true;state.drop_frame();state.warn("previous carrier was not consumed by an XR eye draw");}
    scalars(input,state.parameters);
    unsigned reset{};input->Get(NVSDK_NGX_Parameter_Reset,&reset);state.parameters->Set(NVSDK_NGX_Parameter_Reset,reset);
    ID3D11Resource* color{},*output{};input->Get(NVSDK_NGX_Parameter_Color,&color);input->Get(NVSDK_NGX_Parameter_Output,&output);
    D3D11_TEXTURE2D_DESC color_desc{},output_desc{};
    if (!shape(color,color_desc) || !shape(output,output_desc) || color_desc.Width!=output_desc.Width ||
        color_desc.Height!=output_desc.Height || color_desc.Format!=output_desc.Format) {state.warn("carrier input/output contract mismatch");return false;}
    // Crop offsets cannot be transported through a full-frame postprocess.
    for (const auto* key:{NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                         NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y}) {
        unsigned value{};input->Get(key,&value);if (value) {state.warn("subrect carrier rejected");return false;}
    }
    state.width=color_desc.Width;state.height=color_desc.Height;
    state.parameters->Set(NVSDK_NGX_Parameter_Width,state.width);state.parameters->Set(NVSDK_NGX_Parameter_Height,state.height);
    state.parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,state.width);
    state.parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,state.height);
    for (std::size_t index=0;index<std::size(resource_keys);++index) {
        ID3D11Resource* resource{};input->Get(resource_keys[index],&resource);
        state.parameters->Set(resource_keys[index],static_cast<ID3D11Resource*>(nullptr));
        if (!resource && index<2) {state.warn("missing native motion/depth");state.signal();return false;}
        if (resource && !state.copy_guide(index,resource)) {state.warn("native guide snapshot failed");state.signal();return false;}
    }
    if (!same(color,output)) context->CopyResource(output,color);
    if (!state.signal()) return false;
    state.frame=std::make_shared<Impl::Frame>();state.frame->owner=impl_;
    state.serial=xrfg::register_d3d11_sr_frame(state.frame);state.frame->serial=state.serial;
    xrfg::clear_d3d11_guide_receipt(output);
    xrfg::set_d3d11_sr_origin(output,{state.serial,state.width,state.height});
    return state.serial!=0;
}
bool CspD3D11SuperResolution::retire() {
    if (!impl_) return true;auto state=impl_;bool safe{};
    {std::scoped_lock lock(state->mutex);state->closed=true;state->callback={};state->drop_frame();safe=state->drain_locked();}
    if (!safe) {
        // Do not release resources or the native model still referenced by GPU
        // commands. This exceptional ownership group lives until process exit.
        static auto* retained=new std::vector<std::shared_ptr<Impl>>;
        static auto* retained_mutex=new std::mutex;
        std::scoped_lock lock(*retained_mutex);retained->push_back(state);
        LOG_WARN("CSP owned SR: retaining unretired GPU resources and native handle");
    }
    impl_.reset();return safe;
}
}
