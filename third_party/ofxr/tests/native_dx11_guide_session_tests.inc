// Exercise production session creation/release with actual native devices.
#include "xrfg/d3d11_guide_transport.hpp"
LUID native_guide_adapter{};
void native_dx11_guide_session_test() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> source;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&source,nullptr,&context)),"session DX11 device");
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_desc{};
    check(SUCCEEDED(source.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapter_desc)),"session adapter");native_guide_adapter=adapter_desc.AdapterLuid;
    for (bool converted : {false,true}) {
        auto dispatch=std::make_shared<Dispatch>();dispatch->d3d11_bridge=converted;
        dispatch->get_instance_proc_addr=[](XrInstance,const char*,PFN_xrVoidFunction* function)->XrResult {
            *function=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;
        };
        dispatch->get_d3d12_graphics_requirements=[](XrInstance,XrSystemId,XrGraphicsRequirementsD3D12KHR* requirements)->XrResult {
            requirements->adapterLuid=native_guide_adapter;requirements->minFeatureLevel=D3D_FEATURE_LEVEL_11_0;return XR_SUCCESS;
        };
        dispatch->create_session=[](XrInstance,const XrSessionCreateInfo*,XrSession* handle)->XrResult {
            *handle=reinterpret_cast<XrSession>(0x33029);return XR_SUCCESS;
        };
        dispatch->destroy_session=[](XrSession)->XrResult {return XR_SUCCESS;};
        dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*)->XrResult {return XR_SUCCESS;};
        const auto instance=reinterpret_cast<XrInstance>(0x33029);
        {std::scoped_lock lock(g_state_mutex);g_instances[instance]=dispatch;}
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=source.Get();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};info.next=&binding;info.systemId=1;
        XrSession handle{};check(XR_SUCCEEDED(layer_create_session_impl(instance,&info,&handle)),"actual session creation");
        auto state=find_session(handle);check(state && bool(state->d3d11_bridge)==converted,"native/converted session path");
        ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;std::uint64_t owner{};
        check(SUCCEEDED(xrfg::acquire_d3d11_guide_owner(source.Get(),&device,&queue,&owner)),"session registers original application device");
        check(device.Get()==state->d3d12_device.Get() && queue.Get()==state->d3d12_queue.Get(),"actual session queue/device identity");
        check(xrfg::claim_d3d11_guides(source.Get(),owner),"guide owner claim");
        if (converted) {
            state->menu_enabled=true;state->dlss_motion_vectors=true;
            xrfg::configure_dlss_motion_vector_tracking(true);
            for (unsigned eye : {1u,0u}) {
                XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=ci.height=8;
                ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                ci.format=DXGI_FORMAT_R16G16B16A16_FLOAT;
                D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=8;
                desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=static_cast<DXGI_FORMAT>(ci.format);
                desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> runtime_image;
                check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&runtime_image))),"runtime color");
                auto chain=std::make_shared<SwapchainState>(state,ci);chain->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t{0x330290}+eye);
                chain->dlss_logical_eye_count=2;chain->dlss_logical_eye_index=eye;chain->bridge=std::make_shared<xrfg::D3D11BridgeSwapchain>();
                ID3D12Resource* runtime_images[]{runtime_image.Get()};xrfg::D3D11BridgeSwapchainDescription requested{};
                requested.requested_format=static_cast<DXGI_FORMAT>(ci.format);requested.requested_sample_count=1;std::uint32_t failure_stage{};
                check(SUCCEEDED(chain->bridge->initialize(source.Get(),context.Get(),device.Get(),queue.Get(),runtime_images,requested,&failure_stage)),"actual application bridge");
                auto* actual_eye=chain->bridge->d3d11_images()[0];chain->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
                check(SUCCEEDED(chain->d3d12_history->initialize(device.Get(),queue.Get(),chain->bridge->shared_images(),D3D12_RESOURCE_STATE_COMMON)),"actual history");
                xrfg::D3D11GuideTransport transport;
                check(SUCCEEDED(transport.initialize(context.Get(),device.Get(),queue.Get())) && SUCCEEDED(transport.begin()),"guide transport");
                const auto make_input=[&](DXGI_FORMAT format) {
                    D3D11_TEXTURE2D_DESC input{};input.Width=input.Height=8;input.ArraySize=input.MipLevels=1;input.SampleDesc.Count=1;
                    input.Format=format;input.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                    ComPtr<ID3D11Texture2D> texture;check(SUCCEEDED(source->CreateTexture2D(&input,nullptr,&texture)),"guide input");return texture;
                };
                auto mv=make_input(DXGI_FORMAT_R16G16_FLOAT),depth=make_input(DXGI_FORMAT_R32_FLOAT);
                check(SUCCEEDED(transport.copy(xrfg::D3D11GuideTransport::motion,mv.Get())) &&
                    SUCCEEDED(transport.copy(xrfg::D3D11GuideTransport::depth,depth.Get())) &&
                    SUCCEEDED(transport.copy(xrfg::D3D11GuideTransport::output,actual_eye)),"owned guide GPU copies");
                xrfg::DlssMotionVectorPublication publication{};publication.stream=330290+eye;publication.direct_output_receipt=true;
                publication.output=transport.resource(xrfg::D3D11GuideTransport::output);publication.motion_vectors=transport.resource(xrfg::D3D11GuideTransport::motion);
                publication.depth=transport.resource(xrfg::D3D11GuideTransport::depth);publication.producer_command_list=transport.list();publication.producer_queue=queue.Get();publication.verified_producer_device=device.Get();
                publication.output_width=publication.output_height=publication.motion_width=publication.motion_height=publication.depth_width=publication.depth_height=8;
                publication.scale_x=publication.scale_y=1;publication.camera_near=.1f;publication.camera_far=100;
                publication.resource_state=publication.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
                auto frame=xrfg::publish_dlss_motion_vectors(publication);check(bool(frame),"guide publication");
                check(SUCCEEDED(transport.submit()),"actual producer submission");
                xrfg::set_d3d11_guide_receipt(actual_eye,{owner,frame->stream,frame->publication,8,8});
                {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
                const auto release=[&] {
                    chain->ownership_tracking_valid=true;chain->acquired_indices.push_back(0);chain->waited_count=1;
                    check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,nullptr)),"actual converted release");
                };
                release();auto resolved=chain->last_released_motion_vectors;
                check(resolved && resolved->eye_count==1 && resolved->eyes[0] && resolved->eyes[0]->publication==frame->publication &&
                    resolved->eyes[0]->stream==publication.stream,"actual eye keeps exact publication including right-eye index 1");
                xrfg::clear_d3d11_guide_receipt(actual_eye);release();check(!chain->last_released_motion_vectors,"missing receipt never borrows a guide");
                check(SUCCEEDED(transport.drain()) && SUCCEEDED(chain->d3d12_history->wait_for_idle()) &&
                    SUCCEEDED(chain->bridge->wait_for_idle()),"real GPU completion");
                xrfg::retire_dlss_motion_vector_stream(publication.stream);
                {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(chain->handle);}
            }
        }
        check(XR_SUCCEEDED(layer_destroy_session_impl(handle)),"session destruction");
        check(FAILED(xrfg::acquire_d3d11_guide_owner(source.Get(),device.ReleaseAndGetAddressOf(),queue.ReleaseAndGetAddressOf(),&owner)),"destroyed owner retired");
        {std::scoped_lock lock(g_state_mutex);g_instances.erase(instance);}
    }
    std::cout<<"Actual native/converted DX11 session ownership and two-eye release receipts PASS\n";
}
