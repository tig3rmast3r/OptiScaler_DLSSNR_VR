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
float read11(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;require(device->CreateTexture2D(&desc,nullptr,staging.GetAddressOf()),"readback texture");
    context->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"D11 GPU readback");
    float value{};
    if (desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT) {std::uint16_t half{};std::memcpy(&half,mapped.pData,sizeof(half));value=DirectX::PackedVector::XMConvertHalfToFloat(half);}
    else std::memcpy(&value,mapped.pData,sizeof(value));
    context->Unmap(staging.Get(),0);return value;
}
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
