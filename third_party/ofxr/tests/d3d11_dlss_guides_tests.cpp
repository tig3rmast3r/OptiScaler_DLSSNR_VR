#include "xrfg/d3d11_dlss_guides.hpp"
#include "xrfg/d3d11_guide_transport.hpp"
#include <dxgi1_4.h>
#include <iostream>
#include <vector>
#include <array>
#include <cstring>
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#ifdef XRFG_TEST_D11_OBSERVER
#include <d3dcompiler.h>
#include "../../../OptiScaler/ofxr/csp_super_resolution.hpp"
#include "../../../OptiScaler/ofxr/d3d11_upscaler_bindings.hpp"
#include <unordered_map>
#include <variant>
#include <type_traits>
namespace ofxr_bridge {void observe_d3d11_guide_context(ID3D11DeviceContext*);}
#endif
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks{},failures{};
void expect(bool value,const char* message) {++checks;if (!value) {++failures;std::cerr<<"FAIL "<<message<<'\n';}}
void require(HRESULT result,const char* message) {expect(SUCCEEDED(result),message);if (FAILED(result)) {std::cerr<<std::hex<<static_cast<unsigned>(result)<<std::dec<<'\n';throw result;}}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* device,DXGI_FORMAT format,unsigned components,float value) {
    std::vector<float> pixels(8*8*components,value);
    std::vector<std::uint16_t> half(8*8*components,DirectX::PackedVector::XMConvertFloatToHalf(value));
    const bool use_half=format==DXGI_FORMAT_R16G16B16A16_FLOAT || format==DXGI_FORMAT_R16G16_FLOAT;
    D3D11_SUBRESOURCE_DATA data{use_half?static_cast<void*>(half.data()):static_cast<void*>(pixels.data()),8*components*(use_half?sizeof(std::uint16_t):sizeof(float)),0};
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=8;desc.MipLevels=desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> result;require(device->CreateTexture2D(&desc,&data,result.GetAddressOf()),"source texture");return result;
}
float read11(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source,unsigned x=0,unsigned y=0) {
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;require(device->CreateTexture2D(&desc,nullptr,staging.GetAddressOf()),"readback texture");
    context->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"D11 GPU readback");
    float value{};
    const auto* pixel=static_cast<const std::byte*>(mapped.pData)+y*mapped.RowPitch+x*(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4);
    if (desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format==DXGI_FORMAT_R16G16_FLOAT) {std::uint16_t half{};std::memcpy(&half,pixel,sizeof(half));value=DirectX::PackedVector::XMConvertHalfToFloat(half);}
    else std::memcpy(&value,pixel,sizeof(value));
    context->Unmap(staging.Get(),0);return value;
}
#ifdef XRFG_TEST_D11_OBSERVER
void upscaler_bindings_test(D3D_FEATURE_LEVEL requested) {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,&requested,1,
        D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf()),"binding lifetime WARP/debug device");
    auto input=texture(device.Get(),DXGI_FORMAT_R32_FLOAT,1,.25f);
    auto target=texture(device.Get(),DXGI_FORMAT_R32_FLOAT,1,.75f);
    D3D11_TEXTURE2D_DESC desc{};input->GetDesc(&desc);desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output;require(device->CreateTexture2D(&desc,nullptr,output.GetAddressOf()),"binding UAV texture");
    desc.Format=DXGI_FORMAT_D32_FLOAT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;require(device->CreateTexture2D(&desc,nullptr,depth.GetAddressOf()),"binding depth texture");
    ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;
    require(device->CreateShaderResourceView(input.Get(),nullptr,srv.GetAddressOf()),"binding SRV");
    require(device->CreateUnorderedAccessView(output.Get(),nullptr,uav.GetAddressOf()),"binding UAV");
    require(device->CreateRenderTargetView(target.Get(),nullptr,rtv.GetAddressOf()),"binding RTV");
    require(device->CreateDepthStencilView(depth.Get(),nullptr,dsv.GetAddressOf()),"binding DSV");
    D3D11_SAMPLER_DESC sampler_desc{};sampler_desc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU=sampler_desc.AddressV=sampler_desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;require(device->CreateSamplerState(&sampler_desc,sampler.GetAddressOf()),"binding sampler");
    const float constants[4]{.5f,0,0,0};D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth=sizeof(constants);buffer_desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA buffer_data{constants,0,0};ComPtr<ID3D11Buffer> buffer;
    require(device->CreateBuffer(&buffer_desc,&buffer_data,buffer.GetAddressOf()),"binding constant buffer");
    const char* shader_source="Texture2D<float> source:register(t0);RWTexture2D<float> result:register(u0);"
        "cbuffer Constants:register(b0) {float4 factor;} [numthreads(1,1,1)] "
        "void main(uint3 id:SV_DispatchThreadID) {result[id.xy]=source.Load(int3(id.xy,0))*factor.x;}";
    ComPtr<ID3DBlob> binary;require(D3DCompile(shader_source,std::strlen(shader_source),nullptr,nullptr,nullptr,
        "main","cs_5_0",0,0,binary.GetAddressOf(),nullptr),"binding compute shader compile");
    ComPtr<ID3D11ComputeShader> shader;require(device->CreateComputeShader(binary->GetBufferPointer(),binary->GetBufferSize(),
        nullptr,shader.GetAddressOf()),"binding compute shader");
    auto* original_srv=srv.Get();auto* original_uav=uav.Get();auto* original_sampler=sampler.Get();
    auto* original_buffer=buffer.Get();auto* original_rtv=rtv.Get();auto* original_dsv=dsv.Get();auto* original_shader=shader.Get();
    context->CSSetShaderResources(0,1,&original_srv);context->CSSetUnorderedAccessViews(0,1,&original_uav,nullptr);
    context->CSSetSamplers(0,1,&original_sampler);context->CSSetConstantBuffers(0,1,&original_buffer);
    context->CSSetShader(original_shader,nullptr,0);context->OMSetRenderTargets(1,&original_rtv,original_dsv);
    auto replacement=texture(device.Get(),DXGI_FORMAT_R32_FLOAT,1,.9f);
    ComPtr<ID3D11ShaderResourceView> replacement_view;
    require(device->CreateShaderResourceView(replacement.Get(),nullptr,replacement_view.GetAddressOf()),"model replacement SRV");
    {
        ofxr_bridge::D3D11UpscalerBindings saved(context.Get());
        context->ClearState(); // Model drops the context's references.
        srv.Reset();uav.Reset();sampler.Reset();buffer.Reset();rtv.Reset();dsv.Reset();shader.Reset();
        input.Reset();target.Reset();output.Reset();depth.Reset(); // Snapshot is the sole owner.
        auto* view=replacement_view.Get();context->CSSetShaderResources(1,1,&view);
        context->CSSetUnorderedAccessViews(1,1,&original_uav,nullptr);
        context->CSSetSamplers(1,1,&original_sampler);context->CSSetConstantBuffers(1,1,&original_buffer);
        ID3D11RenderTargetView* model_targets[]{nullptr,original_rtv};context->OMSetRenderTargets(2,model_targets,nullptr);
    }
    ComPtr<ID3D11ShaderResourceView> restored_srv,empty_srv;
    ComPtr<ID3D11UnorderedAccessView> restored_uav,empty_uav;
    ComPtr<ID3D11SamplerState> restored_sampler,empty_sampler;ComPtr<ID3D11Buffer> restored_buffer,empty_buffer;
    ComPtr<ID3D11RenderTargetView> restored_rtv,empty_rtv;ComPtr<ID3D11DepthStencilView> restored_dsv;
    ComPtr<ID3D11ComputeShader> restored_shader;
    context->CSGetShaderResources(0,1,restored_srv.GetAddressOf());context->CSGetShaderResources(1,1,empty_srv.GetAddressOf());
    context->CSGetUnorderedAccessViews(0,1,restored_uav.GetAddressOf());context->CSGetUnorderedAccessViews(1,1,empty_uav.GetAddressOf());
    context->CSGetSamplers(0,1,restored_sampler.GetAddressOf());context->CSGetSamplers(1,1,empty_sampler.GetAddressOf());
    context->CSGetConstantBuffers(0,1,restored_buffer.GetAddressOf());context->CSGetConstantBuffers(1,1,empty_buffer.GetAddressOf());
    context->OMGetRenderTargets(1,restored_rtv.GetAddressOf(),restored_dsv.GetAddressOf());
    context->CSGetShader(restored_shader.GetAddressOf(),nullptr,nullptr);
    expect(restored_srv.Get()==original_srv,"snapshot owns restored SRV after model releases its owner");
    expect(restored_uav.Get()==original_uav,"snapshot owns restored UAV");
    expect(restored_sampler.Get()==original_sampler,"snapshot owns restored sampler");
    expect(restored_buffer.Get()==original_buffer,"snapshot owns restored constant buffer");
    expect(restored_rtv.Get()==original_rtv && restored_dsv.Get()==original_dsv,"snapshot owns restored outputs");
    expect(restored_shader.Get()==original_shader,"snapshot owns restored compute shader");
    std::array<ID3D11RenderTargetView*,2> restored_targets{};context->OMGetRenderTargets(2,restored_targets.data(),nullptr);
    expect(restored_targets[0]==original_rtv && !restored_targets[1],"restoration removes model targets from formerly empty slots");
    for (auto* view:restored_targets) if (view) view->Release();
    expect(!empty_srv && !empty_uav && !empty_sampler && !empty_buffer,"restoration removes model bindings from empty slots");
    context->Dispatch(8,8,1);
    ComPtr<ID3D11Resource> result;restored_uav->GetResource(result.GetAddressOf());
    ComPtr<ID3D11Texture2D> result_texture;require(result.As(&result_texture),"restored output identity");
    expect(std::abs(read11(device.Get(),context.Get(),result_texture.Get())-.125f)<.001f,"restored shader and resources execute real GPU work");
    const auto failed_model=[&]() {
        ofxr_bridge::D3D11UpscalerBindings saved(context.Get());
        context->ClearState();auto* view=replacement_view.Get();context->CSSetShaderResources(1,1,&view);
        return false; // Same lifetime as EvaluateInternal failure / early pass return.
    };
    expect(!failed_model(),"model failure returns early");
    restored_srv.Reset();context->CSGetShaderResources(0,1,restored_srv.GetAddressOf());
    empty_srv.Reset();context->CSGetShaderResources(1,1,empty_srv.GetAddressOf());
    expect(restored_srv.Get()==original_srv && !empty_srv,"early failure restores bindings including nulls");
    const auto throwing_model=[&]() {ofxr_bridge::D3D11UpscalerBindings saved(context.Get());context->ClearState();throw 1;};
    try {throwing_model();} catch (int) {}
    restored_uav.Reset();context->CSGetUnorderedAccessViews(0,1,restored_uav.GetAddressOf());
    expect(restored_uav.Get()==original_uav,"exception unwinding restores owned bindings");
    context->ClearState();
    ComPtr<ID3D11InfoQueue> info;require(device.As(&info),"binding fixture debug queue");unsigned errors{};
    for (UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<std::byte> storage(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());info->GetMessage(i,message,&size);
        if (message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR) {++errors;std::cerr<<message->pDescription<<'\n';}
    }
    expect(!errors,"binding retention has zero D11 validation errors at exact feature level");
}
class TestSrParameters final : public NVSDK_NGX_Parameter {
    std::unordered_map<std::string,std::variant<double,void*>> values;
    template<class T> void write(const char* key,T value) {if constexpr (std::is_pointer_v<T>) values[key]=static_cast<void*>(value);else values[key]=static_cast<double>(value);}
    template<class T> NVSDK_NGX_Result read(const char* key,T* value) const {
        const auto entry=values.find(key);if (entry==values.end()) return NVSDK_NGX_Result_Fail;
        if constexpr (std::is_pointer_v<T>) {if (const auto* pointer=std::get_if<void*>(&entry->second)) {*value=static_cast<T>(*pointer);return NVSDK_NGX_Result_Success;}}
        else if (const auto* number=std::get_if<double>(&entry->second)) {*value=static_cast<T>(*number);return NVSDK_NGX_Result_Success;}
        return NVSDK_NGX_Result_Fail;
    }
public:
#define TEST_SR_PARAMETER(Type) void Set(const char* key,Type value) override {write(key,value);} NVSDK_NGX_Result Get(const char* key,Type* value) const override {return read(key,value);}
    TEST_SR_PARAMETER(unsigned long long) TEST_SR_PARAMETER(float) TEST_SR_PARAMETER(double)
    TEST_SR_PARAMETER(unsigned int) TEST_SR_PARAMETER(int) TEST_SR_PARAMETER(void*)
    TEST_SR_PARAMETER(ID3D11Resource*) TEST_SR_PARAMETER(ID3D12Resource*)
#undef TEST_SR_PARAMETER
    void Reset() override {values.clear();}
};
struct SyntheticSrFrame final : xrfg::D3D11SuperResolutionFrame {
    ID3D11Resource* expected_color{};
    unsigned calls{},submits{};
    std::uint64_t id{};
    ComPtr<ID3D11ShaderResourceView> result;
    HRESULT render(ID3D11DeviceContext*,ID3D11Resource* color,ID3D11Resource* destination,ID3D11ShaderResourceView** output) override {
        ++calls;expect(color==expected_color,"SR consumes exact postprocessed carrier, not cached pre-postprocess color");
        ComPtr<ID3D11Texture2D> target;require(destination->QueryInterface(IID_PPV_ARGS(target.GetAddressOf())),"SR target texture");
        D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);expect(desc.Width==16 && desc.Height==16,"SR actual final target is full size");
        return result.CopyTo(output);
    }
    void submitted(ID3D11DeviceContext*) noexcept override {++submits;xrfg::retire_d3d11_sr_frame(id);}
};
ComPtr<ID3D11Texture2D> full_texture(ID3D11Device* device,float first,float last) {
    std::vector<std::uint16_t> pixels(16*16*4,DirectX::PackedVector::XMConvertFloatToHalf(first));
    pixels[(15*16+15)*4]=DirectX::PackedVector::XMConvertFloatToHalf(last);
    D3D11_SUBRESOURCE_DATA data{pixels.data(),16*8,0};D3D11_TEXTURE2D_DESC desc{};
    desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=1;desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> result;require(device->CreateTexture2D(&desc,&data,result.GetAddressOf()),"owned full-size synthetic result");return result;
}
void sr_consumer_test(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11VertexShader* vs,ID3D11PixelShader* ps) {
    std::array<ComPtr<ID3D11Texture2D>,2> carriers{texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,4,.1f),texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,4,.9f)};
    std::array<ComPtr<ID3D11Texture2D>,2> xr_images{full_texture(device,0,0),full_texture(device,0,0)};
    std::array<ComPtr<ID3D11Texture2D>,2> results{full_texture(device,.25f,.75f),full_texture(device,.875f,.125f)};
    std::array<std::shared_ptr<SyntheticSrFrame>,2> frames;
    for (unsigned eye=0;eye<2;++eye) {
        frames[eye]=std::make_shared<SyntheticSrFrame>();frames[eye]->expected_color=carriers[eye].Get();
        require(device->CreateShaderResourceView(results[eye].Get(),nullptr,frames[eye]->result.GetAddressOf()),"synthetic model result view");
        frames[eye]->id=xrfg::register_d3d11_sr_frame(frames[eye]);
        xrfg::set_d3d11_sr_origin(carriers[eye].Get(),{frames[eye]->id,8,8});
        auto* image=xr_images[eye].Get();xrfg::register_d3d11_sr_destinations(91,910+eye,&image,1);
    }
    expect(xrfg::has_d3d11_sr_destinations(device),"XR device has actual enumerated destinations");
    auto intermediate=texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,4,0);
    ComPtr<ID3D11RenderTargetView> small_target;require(device->CreateRenderTargetView(intermediate.Get(),nullptr,small_target.GetAddressOf()),"small postprocess RTV");
    ComPtr<ID3D11ShaderResourceView> source;require(device->CreateShaderResourceView(carriers[0].Get(),nullptr,source.GetAddressOf()),"carrier SRV");
    context->ClearState();context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs,nullptr,0);context->PSSetShader(ps,nullptr,0);
    auto* source_ptr=source.Get();context->PSSetShaderResources(0,1,&source_ptr);auto* small_rtv=small_target.Get();context->OMSetRenderTargets(1,&small_rtv,nullptr);
    D3D11_VIEWPORT viewport{0,0,8,8,0,1};context->RSSetViewports(1,&viewport);context->Draw(3,0);
    expect(xrfg::get_d3d11_sr_origin(intermediate.Get()).frame==frames[0]->id,"full-size postprocess propagates carrier origin");
    expect(frames[0]->calls==0,"small postprocess never dispatches SR");
    frames[0]->expected_color=intermediate.Get();
    auto unrelated=full_texture(device,0,0);ComPtr<ID3D11RenderTargetView> unrelated_target;
    require(device->CreateRenderTargetView(unrelated.Get(),nullptr,unrelated_target.GetAddressOf()),"unregistered full-size target");
    auto* unrelated_rtv=unrelated_target.Get();context->OMSetRenderTargets(1,&unrelated_rtv,nullptr);
    viewport.Width=viewport.Height=16;context->RSSetViewports(1,&viewport);context->Draw(3,0);
    expect(frames[0]->calls==0,"same extent without XR identity never consumes SR");
    for (unsigned eye : {1u,0u}) {
        source.Reset();require(device->CreateShaderResourceView(eye?carriers[1].Get():intermediate.Get(),nullptr,source.GetAddressOf()),"actual per-eye carrier view");
        source_ptr=source.Get();context->PSSetShaderResources(0,1,&source_ptr);
        ComPtr<ID3D11RenderTargetView> target;require(device->CreateRenderTargetView(xr_images[eye].Get(),nullptr,target.GetAddressOf()),"XR eye RTV");
        auto* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
        viewport.Width=8;context->RSSetViewports(1,&viewport);context->Draw(3,0);
        expect(frames[eye]->calls==0,"cropped final draw cannot consume SR");
        viewport.Width=16;context->RSSetViewports(1,&viewport);
        D3D11_BLEND_DESC blend_desc{};blend_desc.RenderTarget[0].BlendEnable=TRUE;
        blend_desc.RenderTarget[0].SrcBlend=blend_desc.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlend=blend_desc.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
        blend_desc.RenderTarget[0].BlendOp=blend_desc.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> blend;require(device->CreateBlendState(&blend_desc,blend.GetAddressOf()),"blend rejection state");
        context->OMSetBlendState(blend.Get(),nullptr,~0u);context->Draw(3,0);
        expect(frames[eye]->calls==0,"blended final draw cannot consume SR");context->OMSetBlendState(nullptr,nullptr,~0u);
        ComPtr<ID3D11ShaderResourceView> mixed;require(device->CreateShaderResourceView(carriers[1-eye].Get(),nullptr,mixed.GetAddressOf()),"other-eye source");
        auto* mixed_ptr=mixed.Get();context->PSSetShaderResources(1,1,&mixed_ptr);
        if (eye==1) {context->Draw(3,0);expect(frames[eye]->calls==0,"mixed pending eyes rejected");}
        mixed_ptr=nullptr;context->PSSetShaderResources(1,1,&mixed_ptr);
        context->Draw(3,0);
        expect(frames[eye]->calls==1 && frames[eye]->submits==1,"exact XR eye consumes its frame once in reverse order");
        ID3D11ShaderResourceView* restored{};context->PSGetShaderResources(0,1,&restored);
        expect(restored==source.Get(),"original CSP source restored after SR presentation");if (restored) restored->Release();
        context->OMSetRenderTargets(0,nullptr,nullptr);
        expect(std::abs(read11(device,context,xr_images[eye].Get())-(eye?.875f:.25f))<.001f,"real XR pixels use the owned full-size result");
        expect(std::abs(read11(device,context,xr_images[eye].Get(),15,15)-(eye?.125f:.75f))<.001f,"full-size corner survives: no small-buffer/downscale-back presentation");
        expect(!xrfg::get_d3d11_sr_origin(eye?carriers[1].Get():intermediate.Get()).frame,"consumed origin cannot be reused");
        xrfg::unregister_d3d11_sr_destinations(910+eye);
        expect(!xrfg::is_d3d11_sr_destination(xr_images[eye].Get()),"destroyed swapchain removes exact SR destination");
    }
    auto expired=std::make_shared<SyntheticSrFrame>();const auto expired_id=xrfg::register_d3d11_sr_frame(expired);
    xrfg::set_d3d11_sr_origin(intermediate.Get(),{expired_id,8,8});expired.reset();
    expect(!xrfg::get_d3d11_sr_origin(intermediate.Get()).frame,"expired frame never borrows another publication");xrfg::retire_d3d11_sr_frame(expired_id);
    expect(!xrfg::d3d11_sr_pending(),"all completed or expired SR frames retired");context->ClearState();
}
void owned_sr_test(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11VertexShader* vs,ID3D11PixelShader* ps,bool staging=false) {
    // Exercise the production owner, its real GPU copies, context-state scope,
    // output UAV and retirement. Only the NVIDIA model and table allocator are
    // substituted, so this test does not assert actual DLSS model execution.
    auto color=texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,4,.25f);
    auto motion=texture(device,DXGI_FORMAT_R16G16_FLOAT,2,.5f);
    auto depth=texture(device,DXGI_FORMAT_R32_FLOAT,1,.75f);
    auto carrier=texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,4,0);
    auto post=texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,4,0);
    auto xr_image=full_texture(device,0,0);
    TestSrParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Width,8u);parameters.Set(NVSDK_NGX_Parameter_Height,8u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth,8u);parameters.Set(NVSDK_NGX_Parameter_OutHeight,8u);
    parameters.Set(NVSDK_NGX_Parameter_PerfQualityValue,1u);parameters.Set(NVSDK_NGX_Parameter_Reset,0u);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,1.25f);
    parameters.Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D11Resource*>(color.Get()));
    parameters.Set(NVSDK_NGX_Parameter_Output,static_cast<ID3D11Resource*>(carrier.Get()));
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<ID3D11Resource*>(motion.Get()));
    parameters.Set(NVSDK_NGX_Parameter_Depth,static_cast<ID3D11Resource*>(depth.Get()));
    unsigned allocations{},destructions{},dispatches{};
    ofxr_bridge::CspD3D11SuperResolution::ParametersApi api{
        [&](NVSDK_NGX_Parameter** output) {*output=new TestSrParameters;++allocations;return NVSDK_NGX_Result_Success;},
        [&](NVSDK_NGX_Parameter* value) {delete static_cast<TestSrParameters*>(value);++destructions;}
    };
    ofxr_bridge::CspD3D11SuperResolution owned(context,&parameters,api,[&](ID3D11DeviceContext* ctx,NVSDK_NGX_Parameter* local,unsigned width,unsigned height) {
        ++dispatches;expect(width==16 && height==16,"production owner supplies actual full XR output extent");
        unsigned input_width{},output_width{};local->Get(NVSDK_NGX_Parameter_Width,&input_width);local->Get(NVSDK_NGX_Parameter_OutWidth,&output_width);
        expect(input_width==8 && output_width==16,"owned NGX table declares genuine SR, not one-to-one AA");
        float jitter{};local->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&jitter);expect(jitter==1.25f,"deferred scalar snapshot survives caller table mutation");
        ID3D11Resource* local_color{},*local_output{},*local_motion{},*local_depth{};
        local->Get(NVSDK_NGX_Parameter_Color,&local_color);local->Get(NVSDK_NGX_Parameter_Output,&local_output);
        local->Get(NVSDK_NGX_Parameter_MotionVectors,&local_motion);local->Get(NVSDK_NGX_Parameter_Depth,&local_depth);
        expect(local_color==post.Get(),"native model sees current postprocessed eye carrier");
        expect(local_motion!=motion.Get() && local_depth!=depth.Get(),"native guides are owned copies");
        ComPtr<ID3D11Texture2D> mv_texture,depth_texture,out_texture;
        require(local_motion->QueryInterface(IID_PPV_ARGS(mv_texture.GetAddressOf())),"owned MV texture");
        require(local_depth->QueryInterface(IID_PPV_ARGS(depth_texture.GetAddressOf())),"owned depth texture");
        require(local_output->QueryInterface(IID_PPV_ARGS(out_texture.GetAddressOf())),"owned output texture");
        expect(std::abs(read11(device,ctx,mv_texture.Get())-.5f)<.001f,"MV GPU copy survives later source overwrite");
        expect(std::abs(read11(device,ctx,depth_texture.Get())-.75f)<.001f,"depth GPU copy survives later source overwrite");
        D3D11_TEXTURE2D_DESC output_desc{};out_texture->GetDesc(&output_desc);
        expect(output_desc.Width==16 && output_desc.Height==16 && (output_desc.BindFlags&D3D11_BIND_UNORDERED_ACCESS),"production output is a real full-size UAV");
        ctx->ClearState();ComPtr<ID3D11RenderTargetView> view;require(device->CreateRenderTargetView(local_output,nullptr,view.GetAddressOf()),"synthetic native output RTV");
        const float result[4]{.625f,.625f,.625f,1};ctx->ClearRenderTargetView(view.Get(),result);
        return true;
    });
    expect(owned.ready() && allocations==1,"production owned SR initialization");
    require(owned.prepare(context,&parameters)?S_OK:E_FAIL,"production snapshots native evaluate");
    expect(dispatches==0,"native model is deferred until the final eye consumer");
    const auto old_id=xrfg::get_d3d11_sr_origin(carrier.Get()).frame;
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,17.f);
    ComPtr<ID3D11RenderTargetView> motion_rtv,depth_rtv;
    require(device->CreateRenderTargetView(motion.Get(),nullptr,motion_rtv.GetAddressOf()),"mutable motion RTV");
    require(device->CreateRenderTargetView(depth.Get(),nullptr,depth_rtv.GetAddressOf()),"mutable depth RTV");
    const float changed[4]{.875f,.875f,.875f,.875f};context->ClearRenderTargetView(motion_rtv.Get(),changed);context->ClearRenderTargetView(depth_rtv.Get(),changed);
    context->CopyResource(post.Get(),carrier.Get());
    auto* image=xr_image.Get();xrfg::register_d3d11_sr_destinations(92,920,&image,1);
    ComPtr<ID3D11Texture2D> staging_image;ComPtr<ID3D11PixelShader> staging_ps;
    if (staging) {
        staging_image=full_texture(device,0,0);
        ComPtr<ID3D11Resource> resolved;
        expect(FAILED(xrfg::resolve_d3d11_sr_destination(staging_image.Get(),resolved.GetAddressOf())),"same-size staging has no speculative XR ownership");
        context->CopyResource(xr_image.Get(),staging_image.Get());
        require(xrfg::resolve_d3d11_sr_destination(staging_image.Get(),resolved.GetAddressOf()),"actual complete XR copy establishes staging destination");
        expect(resolved.Get()==xr_image.Get() && !xrfg::is_d3d11_sr_destination(staging_image.Get()),"staging resolves real XR image without impersonating it");
        const char* conversion="Texture2D<float4> scene:register(t0);float4 main(float4 p:SV_Position):SV_Target {return scene.Load(int3(p.xy,0))*.5;}";
        ComPtr<ID3DBlob> bytecode;require(D3DCompile(conversion,std::strlen(conversion),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,bytecode.GetAddressOf(),nullptr),"CSP staging color conversion shader");
        require(device->CreatePixelShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,staging_ps.GetAddressOf()),"CSP staging color conversion object");
    }
    auto* presentation_ps=staging?staging_ps.Get():ps;
    ComPtr<ID3D11ShaderResourceView> source;ComPtr<ID3D11RenderTargetView> target;
    require(device->CreateShaderResourceView(post.Get(),nullptr,source.GetAddressOf()),"production postprocessed view");
    require(device->CreateRenderTargetView(staging?staging_image.Get():xr_image.Get(),nullptr,target.GetAddressOf()),"production presentation destination");
    auto* rtv=target.Get();auto* srv=source.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->PSSetShaderResources(0,1,&srv);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs,nullptr,0);context->PSSetShader(presentation_ps,nullptr,0);
    D3D11_VIEWPORT viewport{0,0,16,16,0,1};context->RSSetViewports(1,&viewport);context->Draw(3,0);
    expect(dispatches==1 && !xrfg::resolve_d3d11_sr_frame(old_id),"production native dispatch consumed once and retired");
    ID3D11ShaderResourceView* restored{};context->PSGetShaderResources(0,1,&restored);
    expect(restored==source.Get(),"production context scope preserves CSP source binding");if (restored) restored->Release();
    ComPtr<ID3D11PixelShader> restored_ps;context->PSGetShader(restored_ps.GetAddressOf(),nullptr,nullptr);expect(restored_ps.Get()==presentation_ps,"SDK state changes do not disturb the pending CSP shader");
    unsigned unchanged_width{};parameters.Get(NVSDK_NGX_Parameter_OutWidth,&unchanged_width);
    ID3D11Resource* unchanged_output{};parameters.Get(NVSDK_NGX_Parameter_Output,&unchanged_output);
    expect(unchanged_width==8 && unchanged_output==carrier.Get(),"CSP table never sees the larger owned output");
    context->OMSetRenderTargets(0,nullptr,nullptr);
    if (staging) context->CopyResource(xr_image.Get(),staging_image.Get());
    expect(std::abs(read11(device,context,xr_image.Get(),15,15)-(staging?.3125f:.625f))<.001f,"actual full XR corner preserves owned SR and CSP color conversion through the final copy");
    expect(owned.retire() && destructions==1,"native parameters/resources retire after real GPU completion");
    expect(!owned.prepare(context,&parameters),"retired owner cannot accept another pending eye");
    xrfg::unregister_d3d11_guide_owner(92);expect(!xrfg::is_d3d11_sr_destination(xr_image.Get()),"session retires exact SR destination");
    if (staging) {ComPtr<ID3D11Resource> resolved;expect(FAILED(xrfg::resolve_d3d11_sr_destination(staging_image.Get(),resolved.GetAddressOf())),"session teardown removes staging ownership");}
    context->ClearState();
}
void staging_destination_test(ID3D11Device* device,ID3D11DeviceContext* context) {
    // Learn only actual complete eye copies. A rotating swapchain is one
    // destination domain; copying the same staging color to two domains is not.
    auto frame=std::make_shared<SyntheticSrFrame>();frame->id=xrfg::register_d3d11_sr_frame(frame);
    auto staging=full_texture(device,0,0),first=full_texture(device,0,0),second=full_texture(device,0,0),other=full_texture(device,0,0);
    ID3D11Texture2D* images[]{first.Get(),second.Get()};xrfg::register_d3d11_sr_destinations(94,940,images,2);
    ComPtr<ID3D11Resource> resolved;D3D11_BOX partial{0,0,0,8,16,1};
    context->CopySubresourceRegion(first.Get(),0,0,0,0,staging.Get(),0,&partial);
    expect(FAILED(xrfg::resolve_d3d11_sr_destination(staging.Get(),resolved.GetAddressOf())),"partial XR copy cannot establish staging ownership");
    context->CopyResource(first.Get(),staging.Get());
    require(xrfg::resolve_d3d11_sr_destination(staging.Get(),resolved.GetAddressOf()),"first real XR staging copy");
    expect(resolved.Get()==first.Get(),"first exact image supplies staging extent");resolved.Reset();
    context->CopyResource(second.Get(),staging.Get());
    require(xrfg::resolve_d3d11_sr_destination(staging.Get(),resolved.GetAddressOf()),"rotating XR staging copy");
    expect(resolved.Get()==second.Get(),"swapchain image rotation preserves authoritative destination");resolved.Reset();
    auto* other_image=other.Get();xrfg::register_d3d11_sr_destinations(94,941,&other_image,1);context->CopyResource(other.Get(),staging.Get());
    expect(FAILED(xrfg::resolve_d3d11_sr_destination(staging.Get(),resolved.GetAddressOf())),"staging shared between XR domains is rejected, never last-copy wins");
    auto small_color=texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,4,0);xrfg::observe_d3d11_sr_destination_copy(first.Get(),small_color.Get());
    expect(FAILED(xrfg::resolve_d3d11_sr_destination(small_color.Get(),resolved.GetAddressOf())),"different-size copy cannot prove a staging destination");
    xrfg::unregister_d3d11_sr_destinations(940);xrfg::unregister_d3d11_sr_destinations(941);
    expect(FAILED(xrfg::resolve_d3d11_sr_destination(staging.Get(),resolved.GetAddressOf())),"swapchain teardown removes learned staging references");
    xrfg::register_d3d11_sr_destinations(95,950,images,2);context->CopyResource(first.Get(),staging.Get());
    require(xrfg::resolve_d3d11_sr_destination(staging.Get(),resolved.GetAddressOf()),"new swapchain can relearn formerly ambiguous staging");resolved.Reset();
    xrfg::register_d3d11_sr_destinations(95,950,images,2);
    expect(FAILED(xrfg::resolve_d3d11_sr_destination(staging.Get(),resolved.GetAddressOf())),"image reenumbering invalidates prior staging links");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.ArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> typeless,srgb;require(device->CreateTexture2D(&desc,nullptr,typeless.GetAddressOf()),"CSP typeless color staging");
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;require(device->CreateTexture2D(&desc,nullptr,srgb.GetAddressOf()),"XR typed sRGB eye");
    auto* srgb_image=srgb.Get();xrfg::register_d3d11_sr_destinations(95,951,&srgb_image,1);context->CopyResource(srgb.Get(),typeless.Get());
    require(xrfg::resolve_d3d11_sr_destination(typeless.Get(),resolved.GetAddressOf()),"legal typeless-to-sRGB complete copy proves staging ownership");
    expect(resolved.Get()==srgb.Get(),"typed XR image remains authoritative");resolved.Reset();
    xrfg::unregister_d3d11_guide_owner(95);
    expect(FAILED(xrfg::resolve_d3d11_sr_destination(typeless.Get(),resolved.GetAddressOf())),"session teardown removes typed staging links");
    xrfg::retire_d3d11_sr_frame(frame->id);context->ClearState();
}
#endif
}
int main() {
    ComPtr<ID3D12Debug> debug12;
    const bool validation12=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug12.GetAddressOf())));
    if (validation12) debug12->EnableDebugLayer();
    ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    bool validation11=true;
    HRESULT creation=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,d11.GetAddressOf(),&level,context.GetAddressOf());
    if (FAILED(creation)) {validation11=false;creation=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,d11.GetAddressOf(),&level,context.GetAddressOf());}
    if (FAILED(creation)) return 77;
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D12Device> d12;ComPtr<ID3D12CommandQueue> queue;
    try {
        require(d11.As(&dxgi),"DXGI device");require(dxgi->GetAdapter(adapter.GetAddressOf()),"same adapter");
        require(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(d12.GetAddressOf())),"D12 owner");
        D3D12_COMMAND_QUEUE_DESC q{};require(d12->CreateCommandQueue(&q,IID_PPV_ARGS(queue.GetAddressOf())),"owner queue");
        ComPtr<ID3D12Device> acquired;ComPtr<ID3D12CommandQueue> acquired_queue;std::uint64_t generation{};
        expect(FAILED(xrfg::acquire_d3d11_guide_owner(d11.Get(),acquired.GetAddressOf(),acquired_queue.GetAddressOf(),&generation)),"no speculative owner");
        xrfg::register_d3d11_guide_owner(1,d11.Get(),d12.Get(),queue.Get());
        require(xrfg::acquire_d3d11_guide_owner(d11.Get(),acquired.GetAddressOf(),acquired_queue.GetAddressOf(),&generation),"exact owner");
        expect(acquired.Get()==d12.Get() && acquired_queue.Get()==queue.Get() && generation,"actual queue/device returned");
        expect(xrfg::claim_d3d11_guides(d11.Get(),generation),"claim owner");expect(xrfg::d3d11_guides_claimed(d11.Get()),"strict resource resolution active");
#ifdef XRFG_TEST_D11_OBSERVER
        upscaler_bindings_test(D3D_FEATURE_LEVEL_11_0);
        upscaler_bindings_test(D3D_FEATURE_LEVEL_11_1);
        ofxr_bridge::observe_d3d11_guide_context(context.Get());
#endif
        xrfg::register_d3d11_guide_owner(2,d11.Get(),d12.Get(),queue.Get());
        expect(FAILED(xrfg::acquire_d3d11_guide_owner(d11.Get(),acquired.ReleaseAndGetAddressOf(),acquired_queue.ReleaseAndGetAddressOf(),&generation)),"ambiguous sessions rejected");
        xrfg::unregister_d3d11_guide_owner(2);
        require(xrfg::acquire_d3d11_guide_owner(d11.Get(),acquired.GetAddressOf(),acquired_queue.GetAddressOf(),&generation),"owner resumes");
        auto color=texture(d11.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,4,.25f);
        auto motion=texture(d11.Get(),DXGI_FORMAT_R16G16_FLOAT,2,.5f);
        auto depth=texture(d11.Get(),DXGI_FORMAT_R32_FLOAT,1,.75f);
        auto eye0=texture(d11.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,4,.125f),eye1=texture(d11.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,4,.875f);
        auto final0=texture(d11.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,4,0),final1=texture(d11.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,4,0);
        std::array<xrfg::D3D11GuideTransport,2> transport;
        for (auto& bridge:transport) require(bridge.initialize(context.Get(),d12.Get(),queue.Get()),"per-stream shared transport");
        xrfg::configure_dlss_motion_vector_tracking(true);
        std::array<std::shared_ptr<const xrfg::DlssMotionVectorFrame>,2> held_first;
        for (unsigned iteration=0;iteration<20;++iteration) {
            // Deliberately reverse submission order on alternating frames.
            for (unsigned order=0;order<2;++order) {
                const unsigned eye=(iteration&1)?1-order:order;auto& bridge=transport[eye];
                require(bridge.begin(),"allocator/fence retirement");
                require(bridge.copy(xrfg::D3D11GuideTransport::color,color.Get()),"color copy");
                require(bridge.copy(xrfg::D3D11GuideTransport::motion,motion.Get()),"motion copy");
                require(bridge.copy(xrfg::D3D11GuideTransport::depth,depth.Get()),"depth copy");
                require(bridge.copy(xrfg::D3D11GuideTransport::output,eye?eye1.Get():eye0.Get()),"completed SR output copy");
                if (iteration==19) {
                    // Stand in for a DX12 model writing the transported color:
                    // validate genuine GPU writes, then DX11 observes the result.
                    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap_desc.NumDescriptors=1;
                    ComPtr<ID3D12DescriptorHeap> heap;require(d12->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(heap.GetAddressOf())),"model output RTV");
                    auto* result=bridge.resource(xrfg::D3D11GuideTransport::output);
                    const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();d12->CreateRenderTargetView(result,nullptr,rtv);
                    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=result;
                    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
                    bridge.list()->ResourceBarrier(1,&barrier);const float model_color[]{.5f,.5f,.5f,.5f};bridge.list()->ClearRenderTargetView(rtv,model_color,0,nullptr);
                    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);bridge.list()->ResourceBarrier(1,&barrier);
                }
                xrfg::DlssMotionVectorPublication publication{};publication.stream=100+eye;
                publication.output=bridge.resource(xrfg::D3D11GuideTransport::output);publication.motion_vectors=bridge.resource(xrfg::D3D11GuideTransport::motion);
                publication.depth=bridge.resource(xrfg::D3D11GuideTransport::depth);publication.producer_queue=queue.Get();publication.producer_command_list=bridge.list();
                publication.verified_producer_device=d12.Get();publication.direct_output_receipt=true;
                publication.output_width=publication.output_height=publication.motion_width=publication.motion_height=publication.depth_width=publication.depth_height=8;
                publication.resource_state=publication.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;publication.scale_x=-8;publication.scale_y=8;
                auto frame=xrfg::publish_dlss_motion_vectors(publication);expect(bool(frame),"owned immutable guide snapshot");
                if (iteration==0) held_first[eye]=frame;
                require(bridge.submit(),"actual submission and fence receipt");
                if (!frame) continue;
                auto* source=eye?eye1.Get():eye0.Get();auto* target=eye?final1.Get():final0.Get();
                xrfg::set_d3d11_guide_receipt(source,{generation,frame->stream,frame->publication,8,8});
                context->CopyResource(target,source);expect(xrfg::propagate_d3d11_guide_receipt(target,source),"complete resource copy carries identity");
                auto resolved=xrfg::resolve_d3d11_dlss_guides(target,queue.Get());
                expect(resolved && resolved->eye_count==1 && resolved->eyes[0]->stream==100+eye,"eye determined by resource, not call order");
                expect(resolved && resolved->eyes[0]->publication==frame->publication && resolved->eyes[0]->producer_fence_value,"exact publication and real fence");
                require(bridge.copy_back(xrfg::D3D11GuideTransport::output,color.Get()),"completed D12-to-D11 copyback");
                require(bridge.drain(),"retire copyback read before shared resource reuse");
                expect(read11(d11.Get(),context.Get(),color.Get())==(iteration==19?.5f:(eye?.875f:.125f)),"actual GPU copyback pixels");
            }
        }
#ifdef XRFG_TEST_D11_OBSERVER
        const char* vertex="float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
        const char* pixel="Texture2D<float4> scene:register(t0);float4 main(float4 p:SV_Position):SV_Target {return scene.Load(int3(p.xy,0));}";
        ComPtr<ID3DBlob> vb,pb;require(D3DCompile(vertex,std::strlen(vertex),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,vb.GetAddressOf(),nullptr),"fullscreen vertex shader");
        require(D3DCompile(pixel,std::strlen(pixel),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,pb.GetAddressOf(),nullptr),"color-only postprocess shader");
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        require(d11->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,vs.GetAddressOf()),"VS object");
        require(d11->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,ps.GetAddressOf()),"PS object");
        ComPtr<ID3D11ShaderResourceView> left_view,right_view;ComPtr<ID3D11RenderTargetView> target_view;
        require(d11->CreateShaderResourceView(eye0.Get(),nullptr,left_view.GetAddressOf()),"left color input");
        require(d11->CreateShaderResourceView(eye1.Get(),nullptr,right_view.GetAddressOf()),"right color input");
        require(d11->CreateRenderTargetView(final0.Get(),nullptr,target_view.GetAddressOf()),"postprocess target");
        ID3D11RenderTargetView* target=target_view.Get();context->OMSetRenderTargets(1,&target,nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        D3D11_VIEWPORT viewport{0,0,8,8,0,1};context->RSSetViewports(1,&viewport);
        ID3D11ShaderResourceView* sources[]{left_view.Get(),nullptr};context->PSSetShaderResources(0,2,sources);
        const float black[4]{};context->ClearRenderTargetView(target,black);
        expect(!xrfg::get_d3d11_guide_receipt(final0.Get()).publication,"actual clear invalidates prior receipt");
        context->Draw(3,0);auto post=xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get());
        expect(post && post->eyes[0]->stream==100,"actual fullscreen draw carries same-eye receipt");
        ID3D11ShaderResourceView* preserved{};context->PSGetShaderResources(0,1,&preserved);
        expect(preserved==left_view.Get(),"observer preserves game bindings");if (preserved) preserved->Release();
        sources[1]=right_view.Get();context->PSSetShaderResources(0,2,sources);context->Draw(3,0);
        expect(!xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get()),"mixed-eye shader sources rejected");
        sources[1]=nullptr;context->PSSetShaderResources(0,2,sources);context->Draw(3,0);
        viewport.Width=4;context->RSSetViewports(1,&viewport);context->Draw(3,0);
        expect(!xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get()),"cropped viewport rejects false full-frame ownership");
        context->ClearState();context->CopyResource(final0.Get(),eye0.Get());
        expect(bool(xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get())),"actual complete copy propagates receipt");
        D3D11_BOX partial{0,0,0,4,8,1};context->CopySubresourceRegion(final0.Get(),0,0,0,0,eye0.Get(),0,&partial);
        expect(!xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get()),"actual partial copy invalidates receipt");
        context->CopyResource(final0.Get(),eye0.Get());
        std::array<std::uint16_t,8*8*4> replacement{};context->UpdateSubresource(final0.Get(),0,nullptr,replacement.data(),8*4*sizeof(std::uint16_t),0);
        expect(!xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get()),"CPU write invalidates receipt");
        context->CopyResource(final0.Get(),eye0.Get());
        sr_consumer_test(d11.Get(),context.Get(),vs.Get(),ps.Get());
        owned_sr_test(d11.Get(),context.Get(),vs.Get(),ps.Get());
        owned_sr_test(d11.Get(),context.Get(),vs.Get(),ps.Get(),true);
        staging_destination_test(d11.Get(),context.Get());
#endif
        const auto valid=xrfg::get_d3d11_guide_receipt(final0.Get());
        xrfg::set_d3d11_guide_receipt(final0.Get(),{generation,100,valid.publication-1,8,8});
        expect(!xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get()),"stale publication never resolves latest");
        xrfg::set_d3d11_guide_receipt(final0.Get(),valid);xrfg::retire_dlss_motion_vector_stream(100);
        expect(!xrfg::resolve_d3d11_dlss_guides(final0.Get(),queue.Get()),"retired stream rejected");
        xrfg::clear_d3d11_guide_receipt(final1.Get());expect(!xrfg::resolve_d3d11_dlss_guides(final1.Get(),queue.Get()),"unknown eye cannot borrow other eye");
        xrfg::unregister_d3d11_guide_owner(1);xrfg::register_d3d11_guide_owner(3,d11.Get(),d12.Get(),queue.Get());
        expect(!xrfg::resolve_d3d11_dlss_guides(eye1.Get(),queue.Get()),"old session receipt rejected");
        xrfg::unregister_d3d11_guide_owner(3);xrfg::retire_dlss_motion_vector_stream(101);
        // sRGB output cannot carry a typed UAV. Guide capture must still work
        // with its legal RT/shared flags, independently of model composition.
        D3D11_TEXTURE2D_DESC bgra_desc{};bgra_desc.Width=bgra_desc.Height=8;bgra_desc.ArraySize=bgra_desc.MipLevels=1;
        bgra_desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;bgra_desc.SampleDesc.Count=1;bgra_desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        std::array<std::uint32_t,64> bgra_pixels{};bgra_pixels.fill(0xff406080);D3D11_SUBRESOURCE_DATA bgra_data{bgra_pixels.data(),8*sizeof(std::uint32_t),0};
        ComPtr<ID3D11Texture2D> bgra;require(d11->CreateTexture2D(&bgra_desc,&bgra_data,bgra.GetAddressOf()),"BGRA source");
        require(transport[0].begin(),"BGRA transport recording");
        require(transport[0].copy(xrfg::D3D11GuideTransport::output,bgra.Get()),"BGRA shared copy without illegal UAV");
        expect((transport[0].resource(xrfg::D3D11GuideTransport::output)->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)==0,"BGRA support controls actual resource flags");
        require(transport[0].submit(),"BGRA submission");require(transport[0].copy_back(xrfg::D3D11GuideTransport::output,bgra.Get()),"BGRA copyback");
        require(transport[0].drain(),"BGRA GPU retirement");
        expect(SUCCEEDED(d12->GetDeviceRemovedReason()),"GPU device remains healthy");
        unsigned validation_errors{};
        ComPtr<ID3D11InfoQueue> info11;if (validation11 && SUCCEEDED(d11.As(&info11))) {
            for (UINT64 i=0;i<info11->GetNumStoredMessages();++i) {SIZE_T size{};info11->GetMessage(i,nullptr,&size);std::vector<std::byte> storage(size);
                auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());info11->GetMessage(i,message,&size);
                if (message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR) {++validation_errors;std::cerr<<message->pDescription<<'\n';}
            }
        }
        ComPtr<ID3D12InfoQueue> info12;if (validation12 && SUCCEEDED(d12.As(&info12))) {
            for (UINT64 i=0;i<info12->GetNumStoredMessages();++i) {SIZE_T size{};info12->GetMessage(i,nullptr,&size);std::vector<std::byte> storage(size);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());info12->GetMessage(i,message,&size);
                if (message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {++validation_errors;std::cerr<<message->pDescription<<'\n';}
            }
        }
        std::cout<<"D11 validation="<<validation11<<" D12 validation="<<validation12<<" GPU errors="<<validation_errors<<'\n';
        expect(validation_errors==0,"GPU validation has no errors");
    } catch (...) {++failures;}
    std::cout<<"D3D11 guide GPU checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
