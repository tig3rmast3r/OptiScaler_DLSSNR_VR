// Shipping API/worker/generator code, with only the downstream runtime mocked.
#include "../src/layer/openxr_layer.cpp"
#define main private_transport_core_main
#include "private_openxr_runtime_tests.cpp"
#undef main
namespace xrfg_fixture {
struct ControlProfile {};
bool request_control(const xrfg::embedded::Settings& s) { return xrfg::embedded::request(s); }
}
#include <d3d12sdklayers.h>

int main(int argc,char** argv) {
    std::string route=argc>1?argv[1]:"new";
    const bool separate_queue=route.starts_with("separate-");if(separate_queue)route=route.substr(9);
    int a{},b{};if(route.size()>3 && route[route.size()-3]=='-'){a=route[route.size()-2]-'0';b=route.back()-'0';route.resize(route.size()-3);}
    const bool switching=route=="switch";if(switching)route="new";
    const bool bridge_session=route=="bridge-session";
    const bool native12=!bridge_session && !route.starts_with("d11-");if(route.starts_with("d11-"))route=route.substr(4);
    const bool triple=route.ends_with('3');if(triple)route.pop_back();
    xrfg_fixture::ControlProfile profile;
    ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Fixture f(native12);f.transport->stop();f.transport.reset();f.throttled=false;f.real_common=true;f.expected_views=2;f.unblock();
    f.create_state=[] {std::scoped_lock lock(g_state_mutex);return g_swapchains.empty()?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COMMON;};
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);dispatch->steamvr_runtime=true;
    dispatch->wait_frame=Fixture::wait;dispatch->begin_frame=Fixture::begin;dispatch->end_frame=Fixture::end;
    dispatch->create_swapchain=Fixture::create;dispatch->destroy_swapchain=Fixture::destroy;dispatch->enumerate_swapchain_images=Fixture::enumerate;
    dispatch->acquire_swapchain_image=Fixture::acquire;dispatch->wait_swapchain_image=Fixture::wait_image;dispatch->release_swapchain_image=Fixture::release;dispatch->convert_time_to_qpc=Fixture::to_qpc;
    if(route=="session" || bridge_session) {
        if (bridge_session) {
            dispatch->d3d11_bridge=true;
            dispatch->get_d3d12_graphics_requirements=+[](XrInstance,XrSystemId,XrGraphicsRequirementsD3D12KHR* requirements)->XrResult {
                ComPtr<IDXGIDevice> device;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC description{};
                ok(current->device11.As(&device),"bridge DXGI identity");ok(device->GetAdapter(&adapter),"bridge adapter");
                ok(adapter->GetDesc(&description),"bridge adapter LUID");
                requirements->adapterLuid=description.AdapterLuid;requirements->minFeatureLevel=D3D_FEATURE_LEVEL_11_0;return XR_SUCCESS;
            };
        }
        dispatch->get_instance_proc_addr=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult{*out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;};
        dispatch->create_session=+[](XrInstance,const XrSessionCreateInfo* info,XrSession* out)->XrResult{
            auto& f=*current;auto* binding=static_cast<const XrGraphicsBindingD3D12KHR*>(info->next);
            check(binding && binding->type==XR_TYPE_GRAPHICS_BINDING_D3D12_KHR,"actual raw D12 binding");
            check(binding->queue!=f.queue.Get(),"real XR must not receive producer queue");
            f.delivery_queue=binding->queue;f.device12=binding->device;*out=reinterpret_cast<XrSession>(2);return XR_SUCCESS;
        };
        dispatch->destroy_session=+[](XrSession)->XrResult{return XR_SUCCESS;};
        {std::scoped_lock lock(g_state_mutex);g_instances[dispatch->instance]=dispatch;}
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};binding.device=f.device12.Get();binding.queue=f.queue.Get();
        XrGraphicsBindingD3D11KHR bridge_binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};bridge_binding.device=f.device11.Get();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};info.next=bridge_session?static_cast<const void*>(&bridge_binding):static_cast<const void*>(&binding);info.systemId=1;
        XrSession handle{};xr_ok(layer_create_session_impl(dispatch->instance,&info,&handle),"shipping session creates private delivery binding");
        auto created=find_session(handle);check(created && created->private_runtime,"shipping session installed transport");
        f.transport=created->private_runtime; // fixture teardown owns a valid stopped endpoint
        check(created->private_delivery_queue.Get()==f.delivery_queue.Get() &&
              created->private_delivery_queue.Get()!=runtime_queue(*created),"raw binding and private preparation queues are distinct");
        if (bridge_session) check(created->d3d11_bridge && created->d3d11_bridge->device.Get()==f.device11.Get() &&
            created->graphics_binding==SessionGraphicsBinding::d3d12 && info.next==&bridge_binding &&
            bridge_binding.device==f.device11.Get(),"CSP original D11 binding survives translated private D12 delivery");
        else check(binding.queue==f.queue.Get() && info.next==&binding,"application binding metadata remains owned by application");
        xr_ok(layer_destroy_session_impl(handle),"shipping private delivery session cleanup");
        {std::scoped_lock lock(g_state_mutex);g_instances.clear();}
        std::puts("Shipping xrCreateSession receives dedicated consumer queue; private producer queue retained PASS");return 0;
    }
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);state->graphics_binding=native12?SessionGraphicsBinding::d3d12:SessionGraphicsBinding::d3d11;
    if(!native12)ok(xrfg::create_d3d12_device_for_d3d11(f.device11.Get(),f.device12.GetAddressOf(),f.queue.GetAddressOf()),"D11 generator interop device");
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->d3d11_device=f.device11;state->d3d11_context=f.context;
    D3D12_COMMAND_QUEUE_DESC queue_info{};queue_info.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(native12)ok(f.device12->CreateCommandQueue(&queue_info,IID_PPV_ARGS(&state->d3d12_synthesis_queue)),"separate generator queue");
    if(native12)state->private_delivery_queue=f.delivery_queue;
    if(separate_queue) {
        ok(f.device12->CreateCommandQueue(&queue_info,IID_PPV_ARGS(&state->binding_queue)),"independent real binding queue");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&state->app_release_fence)),"input publication fence");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&state->reacquire_fence)),"legacy binding reacquire fence");
    }
    state->optical_flow_backend=route=="fidelity"?xrfg::D3D12OpticalFlowBackend::fidelity_fx:xrfg::D3D12OpticalFlowBackend::nvidia;
    state->nvidia_new_route=route!="ref" && route!="fidelity";state->djules_route=route=="djules";
    state->separate_synthesis=a;state->lower_vram=b;
    state->djules_triple_requested=triple;state->djules_deep_pipeline=triple;
    const bool vectors=route=="fsr";
    if(vectors)state->dlss_motion_vectors=true;
    if(route=="fsr")state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
    state->control_revision=xrfg::embedded::snapshot().revision;
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;g_instances[dispatch->instance]=dispatch;}
    ok(install_private_runtime(state),"install real SteamVR private dispatch");
    f.transport=state->private_runtime;xr_ok(f.transport->start(),"start common consumer");
    if(separate_queue) {
        ComPtr<ID3D12Fence> gate,progress;
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"binding dependency gate");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&progress)),"app progress fence");
        ok(state->binding_queue->Wait(gate.Get(),1),"hold real binding work");
        carry_reacquire_to_application(state.get());
        ok(f.queue->Signal(progress.Get(),1),"app must proceed independently of real binding work");
        const auto independent=wait_composition_fence(progress.Get(),1,30);
        ok(gate->Signal(1),"release real binding independently");
        ok(wait_composition_fence(progress.Get(),1),"reacquire proof drain");
        check(SUCCEEDED(independent),"private reacquire must not join the real runtime queue");
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=256;info.height=128;info.arraySize=2;info.faceCount=info.mipCount=info.sampleCount=1;info.format=DXGI_FORMAT_R8G8B8A8_UNORM;info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchain chain{};xr_ok(layer_create_swapchain_impl(state->handle,&info,&chain),"shipping application swapchain");
    std::array<XrSwapchainImageD3D12KHR,3> images;for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};unsigned count{};
    if(native12)xr_ok(layer_enumerate_swapchain_images_impl(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"shipping enumerate/capture setup");
    else{std::array<XrSwapchainImageD3D11KHR,3> d11;for(auto& image:d11)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};xr_ok(layer_enumerate_swapchain_images_impl(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(d11.data())),"shipping D11 enumerate/capture setup");}
    ComPtr<ID3D12Resource> depth,motion;
    if(vectors) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=256;desc.Height=128;desc.DepthOrArraySize=2;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;desc.Format=DXGI_FORMAT_R32_FLOAT;
        ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&depth)),"owned depth guide");desc.Format=DXGI_FORMAT_R16G16_FLOAT;
        ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&motion)),"owned motion guide");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;ComPtr<ID3D12DescriptorHeap> views;ok(f.device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"guide RTVs");
        auto d=views->GetCPUDescriptorHandleForHeapStart(),m=d;m.ptr+=f.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);f.device12->CreateRenderTargetView(depth.Get(),nullptr,d);f.device12->CreateRenderTargetView(motion.Get(),nullptr,m);
        f.commands([&](ID3D12GraphicsCommandList* list){const float z[]={.5F,.5F,.5F,1},v[]={0,0,0,0};list->ClearRenderTargetView(d,z,0,nullptr);list->ClearRenderTargetView(m,v,0,nullptr);D3D12_RESOURCE_BARRIER b[2]{};for(auto& x:b){x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;x.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;x.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}b[0].Transition.pResource=depth.Get();b[1].Transition.pResource=motion.Get();list->ResourceBarrier(2,b);});
    }
    f.release_callback=[&](XrSwapchain handle,unsigned index){auto c=find_swapchain(handle);{std::scoped_lock lock(c->mutex);c->acquired_indices.push_back(index);++c->waited_count;}XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};return layer_release_swapchain_image_impl(handle,&release);};
    for(unsigned n=1;n<=8;++n) {
        if(switching && (n==3 || n==5 || n==7)) {
            auto desired=xrfg::embedded::snapshot().desired;desired.enabled=true;desired.backend=n==3?1:n==5?3:0;
            desired.frame_generation=0;desired.separate_synthesis=0;desired.lower_vram=0;
            check(xrfg_fixture::request_control(desired),"request route hot handoff");
            ok(apply_embedded_control(state),"apply route hot handoff");check(state->private_runtime==f.transport,"route switch must retain one common distributor");
        }
        std::fprintf(stderr,"frame %u write (physical=%u)\n",n,f.real_ends.load());
        f.write(chain,n*20);
        if(vectors){auto c=find_swapchain(chain);auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
            for(unsigned eye=0;eye<2;++eye){auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=eye+1;g->epoch=1;g->serial=n;g->previous_serial=n-1;g->publication=2*n+eye;g->producer_queue=f.queue;g->output=images[*c->last_released_index].texture;g->output_slice=eye;g->depth=depth;g->motion_vectors=motion;g->motion_slice=eye;g->output_width=g->depth_width=g->motion_width=256;g->output_height=g->depth_height=g->motion_height=128;guides->eyes[eye]=g;}c->last_released_motion_vectors=guides;}
        XrFrameState prediction{XR_TYPE_FRAME_STATE};xr_ok(layer_wait_frame_impl(state->handle,nullptr,&prediction),"shipping virtual Wait");xr_ok(layer_begin_frame_impl(state->handle,nullptr),"shipping virtual Begin");
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye){auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;v.pose.position.x=eye?.032F:-.032F;v.pose.position.y=float(n);v.fov={-.7F,.7F,.7F,-.7F};v.subImage.swapchain=chain;v.subImage.imageArrayIndex=eye;v.subImage.imageRect.extent={256,128};}
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
        xr_ok(layer_end_frame_impl(state->handle,&end),"shipping OFXR End handoff");
        std::fprintf(stderr,"frame %u accepted\n",n);
    }
    xr_ok(wait_for_synthesis_handoff(state),"worker complete");
    if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"generator transport drained");
    xr_ok(f.transport->drain(),"common physical FIFO drained");
    stop_continuous_presenter(state);
    auto generated=count>0 && f.real_ends>=(switching?10:12);
    std::printf("%s physical outputs=%u\n",route.c_str(),f.real_ends.load());
    check(generated,"actual generator must produce beyond eight real inputs");
    for(unsigned i=1;i<f.handoffs.size();++i)check(f.handoffs[i]-f.handoffs[i-1]>=period,"all route outputs obey common physical cadence");
    f.transport->stop();
    for(auto& c:find_swapchains(state)){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
    xr_ok(layer_destroy_swapchain_impl(chain),"shipping destroy");
    ComPtr<ID3D12InfoQueue> messages;
    if(SUCCEEDED(f.device12.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes{};messages->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());messages->GetMessage(i,message,&bytes);if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12: %s\n",message->pDescription);check(false,"actual generator resource state/ownership validation");}}
    {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_swapchains.clear();g_instances.clear();}
    std::printf("%s shipping generator -> private bridge -> common consumer PASS\n",route.c_str());return 0;
}
