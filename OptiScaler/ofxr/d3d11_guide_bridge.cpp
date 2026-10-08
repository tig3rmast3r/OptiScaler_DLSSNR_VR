#include <pch.h>
#include "d3d11_guide_bridge.hpp"
#include "ofxr_bridge_client.hpp"
#include <NVNGX_Parameter.h>
#include <upscalers/IFeature.h>
#include <dlssnr/DlssNr.h>
#include "../../third_party/ofxr/include/xrfg/d3d11_guide_transport.hpp"
#include "../../third_party/ofxr/include/xrfg/d3d11_dlss_guides.hpp"
#include "../../third_party/ofxr/include/xrfg/bridge_flight_logger.hpp"
#include <cmath>

namespace ofxr_bridge {
namespace {
using Microsoft::WRL::ComPtr;
using Transport=xrfg::D3D11GuideTransport;
// Writes stay in an owned table, including all scalar changes by NR/NGX. The
// original CSP parameter block never sees DX12 pointers or NR dimensions.
class Parameters final : public NVSDK_NGX_Parameter {
    NVSDK_NGX_Parameter* source_;
    NVNGX_Parameters local_{API::DX12,false};
public:
    explicit Parameters(NVSDK_NGX_Parameter* source):source_(source) {}
#define NR_PARAMETER_TYPE(Type) \
    void Set(const char* key,Type value) override { local_.Set(key,value); } \
    NVSDK_NGX_Result Get(const char* key,Type* value) const override { \
        const auto result=local_.Get(key,value); return result==NVSDK_NGX_Result_Success ? result : source_->Get(key,value); }
    NR_PARAMETER_TYPE(unsigned long long)
    NR_PARAMETER_TYPE(float)
    NR_PARAMETER_TYPE(double)
    NR_PARAMETER_TYPE(unsigned int)
    NR_PARAMETER_TYPE(int)
    NR_PARAMETER_TYPE(void*)
    NR_PARAMETER_TYPE(ID3D11Resource*)
    NR_PARAMETER_TYPE(ID3D12Resource*)
#undef NR_PARAMETER_TYPE
    void Reset() override { local_.Reset(); }
};
ID3D11Resource* resource(NVSDK_NGX_Parameter* parameters,const char* key) {
    ID3D11Resource* result{};
    if (parameters->Get(key,&result)!=NVSDK_NGX_Result_Success) parameters->Get(key,reinterpret_cast<void**>(&result));
    return result;
}
template<class T> T scalar(NVSDK_NGX_Parameter* parameters,const char* key,T fallback) {
    T value=fallback; return parameters->Get(key,&value)==NVSDK_NGX_Result_Success ? value : fallback;
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    if (!resource || from==to) return;
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=resource; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=from; barrier.Transition.StateAfter=to; list->ResourceBarrier(1,&barrier);
}
}
struct D3D11GuideBridge::Impl {
    std::unique_ptr<Transport> transport;
    std::uint64_t owner{},stream{};
    bool begun{},pre_nr{},post_nr{},guides{};
    unsigned notices{};
    unsigned descriptions{};
    HRESULT last_error{S_FALSE};
    void report(HRESULT result,const char* phase) {
        if (result==last_error || notices>=24) return;
        last_error=result; ++notices;
        LOG_WARN("V33028 DX11 guide bridge {} result=0x{:X} stream={} owner={}",phase,static_cast<unsigned>(result),stream,owner);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::d3d11_bridge,result,810,stream,owner);
    }
    void nr(NVSDK_NGX_Parameter* parameters,bool before) {
        Parameters local(parameters);
        const char* names[]{NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_Output};
        const int role_count=before ? Transport::output : Transport::role_count;
        for (int i=0;i<Transport::role_count;++i) local.Set(names[i],i<role_count ? transport->resource(static_cast<Transport::Role>(i)) : nullptr);
        local.Set(NVSDK_NGX_Parameter_ExposureTexture,static_cast<ID3D12Resource*>(nullptr));
        local.Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,static_cast<ID3D12Resource*>(nullptr));
        auto* list=transport->list();
        for (int i=0;i<role_count;++i) transition(list,transport->resource(static_cast<Transport::Role>(i)),D3D12_RESOURCE_STATE_COMMON,
            i==Transport::output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (before) DlssNr::EvaluateBeforeUpscale(static_cast<unsigned>(stream),list,&local,transport->queue());
        else DlssNr::EvaluateAfterUpscale(static_cast<unsigned>(stream),list,&local,transport->queue());
        for (int i=0;i<role_count;++i) transition(list,transport->resource(static_cast<Transport::Role>(i)),
            i==Transport::output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
};
D3D11GuideBridge::D3D11GuideBridge():impl_(std::make_unique<Impl>()) {}
D3D11GuideBridge::~D3D11GuideBridge() {
    if (impl_->transport) impl_->transport->drain();
    if (State::Instance().isShuttingDown) return;
    if (impl_->stream) {
        DlssNr::ReleaseForHandle(static_cast<unsigned>(impl_->stream));
        xrfg::retire_dlss_motion_vector_stream(impl_->stream);
    }
}
bool D3D11GuideBridge::begin(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* parameters,IFeature* feature) {
    auto& value=*impl_; value.begun=false; value.stream=feature->Handle()->Id;
    const auto desired=snapshot().desired;
    value.guides=desired.enabled && (desired.motion_vectors==1 || desired.frame_generation==1);
    const bool nr_enabled=Config::Instance()->DlssNrEnabled.value_or_default();
    const bool nr_before=Config::Instance()->DlssNrRunBeforeSr.value_or_default();
    value.pre_nr=nr_enabled && nr_before; value.post_nr=nr_enabled && !nr_before;
    auto* original_output=resource(parameters,NVSDK_NGX_Parameter_Output);
    xrfg::clear_d3d11_guide_receipt(original_output);
    if (!value.guides && !nr_enabled) return false;
    ComPtr<ID3D11Device> source; context->GetDevice(source.GetAddressOf());
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; std::uint64_t generation{};
    HRESULT result=xrfg::acquire_d3d11_guide_owner(source.Get(),device.GetAddressOf(),queue.GetAddressOf(),&generation);
    if (FAILED(result)) {value.report(result,"owner unavailable"); return false;}
    if (!value.transport || value.owner!=generation) {
        value.transport.reset();
        if (value.owner) DlssNr::ReleaseForHandle(static_cast<unsigned>(value.stream));
        xrfg::retire_dlss_motion_vector_stream(value.stream);
        value.owner=generation; value.transport=std::make_unique<Transport>();
        result=value.transport->initialize(context,device.Get(),queue.Get());
        if (FAILED(result)) {value.report(result,"initialize");value.transport.reset();return false;}
        ensure_d3d12_submission_hook(device.Get()); observe_d3d11_guide_context(context);
    }
    if (value.guides) {
        xrfg::claim_d3d11_guides(source.Get(),value.owner);
        xrfg::configure_dlss_motion_vector_tracking(true);
    }
    result=value.transport->begin();
    const char* keys[]{NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth};
    for (int i=0;i<3 && SUCCEEDED(result);++i) {
        auto* input=resource(parameters,keys[i]);
        if (value.descriptions<4 && input) {
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(input->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) {
                D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                LOG_WARN("V33028 DX11 stream={} input={} resource={:p} size={}x{} format={} array={} samples={}",
                    value.stream,keys[i],static_cast<void*>(input),desc.Width,desc.Height,static_cast<unsigned>(desc.Format),desc.ArraySize,desc.SampleDesc.Count);
            }
        }
        result=value.transport->copy(static_cast<Transport::Role>(i),input);
    }
    ++value.descriptions;
    if (value.descriptions<=4) LOG_WARN("V33028 DX11 guide metadata stream={} render={}x{} display={}x{} scale=({}, {}) jitter=({}, {}) lowRes={} inverted={}",
        value.stream,feature->RenderWidth(),feature->RenderHeight(),feature->DisplayWidth(),feature->DisplayHeight(),
        scalar(parameters,NVSDK_NGX_Parameter_MV_Scale_X,1.f),scalar(parameters,NVSDK_NGX_Parameter_MV_Scale_Y,1.f),
        scalar(parameters,NVSDK_NGX_Parameter_Jitter_Offset_X,0.f),scalar(parameters,NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f),feature->LowResMV(),feature->DepthInverted());
    if (FAILED(result)) {
        if (value.transport->list()) value.transport->submit();
        value.report(result,"input copy"); return false;
    }
    if (value.pre_nr) {
        value.nr(parameters,true); result=value.transport->submit();
        if (SUCCEEDED(result)) result=value.transport->copy_back(Transport::color,resource(parameters,NVSDK_NGX_Parameter_Color));
        if (FAILED(result)) {value.report(result,"pre-SR NR transport");return false;}
    }
    value.begun=true; return true;
}
void D3D11GuideBridge::complete(ID3D11DeviceContext*,NVSDK_NGX_Parameter* parameters,IFeature* feature,bool success) {
    auto& value=*impl_; if (!value.begun) return; value.begun=false;
    auto* output=resource(parameters,NVSDK_NGX_Parameter_Output);
    HRESULT result=S_OK;
    if (value.pre_nr) result=value.transport->begin();
    if (success && SUCCEEDED(result)) result=value.transport->copy(Transport::output,output);
    std::shared_ptr<const xrfg::DlssMotionVectorFrame> frame;
    if (success && SUCCEEDED(result) && value.guides) {
        xrfg::DlssMotionVectorPublication publication{};
        publication.stream=value.stream; publication.output=value.transport->resource(Transport::output);
        publication.motion_vectors=value.transport->resource(Transport::motion); publication.depth=value.transport->resource(Transport::depth);
        publication.producer_queue=value.transport->queue(); publication.producer_command_list=value.transport->list();
        publication.verified_producer_device=value.transport->device(); publication.direct_output_receipt=true;
        const auto out=publication.output->GetDesc(), mv=publication.motion_vectors->GetDesc(), depth=publication.depth->GetDesc();
        // Initial support is full-frame, one texture per eye. Reject subrects
        // rather than silently publishing guides in a different coordinate space.
        const float mx=scalar(parameters,NVSDK_NGX_Parameter_MV_Offset_X,0.f),my=scalar(parameters,NVSDK_NGX_Parameter_MV_Offset_Y,0.f);
        const bool origin_is_zero=
            scalar(parameters,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,0u)==0 &&
            scalar(parameters,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,0u)==0 &&
            scalar(parameters,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,0u)==0 &&
            scalar(parameters,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,0u)==0 &&
            scalar(parameters,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,0u)==0 &&
            scalar(parameters,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,0u)==0;
        const unsigned render_width=scalar(parameters,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,feature->RenderWidth());
        const unsigned render_height=scalar(parameters,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,feature->RenderHeight());
        if (origin_is_zero && mx==0 && my==0 && out.Width==feature->DisplayWidth() && out.Height==feature->DisplayHeight() &&
            depth.Width==render_width && depth.Height==render_height &&
            mv.Width==(feature->LowResMV()?render_width:feature->DisplayWidth()) && mv.Height==(feature->LowResMV()?render_height:feature->DisplayHeight())) {
            publication.output_width=static_cast<unsigned>(out.Width); publication.output_height=out.Height;
            publication.motion_width=static_cast<unsigned>(mv.Width); publication.motion_height=mv.Height;
            publication.depth_width=static_cast<unsigned>(depth.Width); publication.depth_height=depth.Height;
            publication.scale_x=scalar(parameters,NVSDK_NGX_Parameter_MV_Scale_X,1.f);
            publication.scale_y=scalar(parameters,NVSDK_NGX_Parameter_MV_Scale_Y,1.f);
            publication.jitter_x=scalar(parameters,NVSDK_NGX_Parameter_Jitter_Offset_X,0.f);
            publication.jitter_y=scalar(parameters,NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f);
            publication.reset=scalar(parameters,NVSDK_NGX_Parameter_Reset,0)!=0;
            publication.jittered=feature->JitteredMV(); publication.depth_inverted=feature->DepthInverted();
            publication.frame_time_delta_ms=scalar(parameters,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,16.6667f);
            publication.camera_near=Config::Instance()->FsrCameraNear.value_or_default();
            const float cameraFarValue=Config::Instance()->FsrCameraFar.value_or_default(); publication.depth_infinite=cameraFarValue==0;
            publication.camera_far=cameraFarValue==0 ? std::max(1.f,publication.camera_near+1.f) : cameraFarValue;
            publication.resource_state=publication.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
            frame=xrfg::publish_dlss_motion_vectors(publication);
        }
        if (!frame) value.report(E_INVALIDARG,"guide contract rejected");
    }
    auto* nr_output=value.transport->resource(Transport::output);
    const bool output_uav=nr_output && (nr_output->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)!=0;
    const bool post_nr=success && SUCCEEDED(result) && value.post_nr && output_uav;
    if (success && SUCCEEDED(result) && value.post_nr && !output_uav)
        value.report(DXGI_ERROR_UNSUPPORTED,"post-SR NR requires typed UAV output");
    if (post_nr) value.nr(parameters,false);
    if (value.transport->list()) {
        const HRESULT submitted=value.transport->submit(); if (SUCCEEDED(result)) result=submitted;
    }
    if (post_nr && SUCCEEDED(result)) result=value.transport->copy_back(Transport::output,output);
    if (frame && SUCCEEDED(result)) xrfg::set_d3d11_guide_receipt(output,
        {value.owner,frame->stream,frame->publication,frame->output_width,frame->output_height});
    if (value.descriptions<=4) LOG_WARN("V33028 DX11 guide completion stream={} publication={} output={:p} result=0x{:X} preNR={} postNR={}",
        value.stream,frame?frame->publication:0,static_cast<void*>(output),static_cast<unsigned>(result),value.pre_nr,post_nr);
    value.report(result,frame?"publication submitted":"completed without publication");
}
}
