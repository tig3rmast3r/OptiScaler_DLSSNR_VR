// Real D3D12/NVIDIA and the actual release/EndFrame/worker/director code.
// The runtime alone is mocked. Sources are poisoned after intercepted release.
#include "../src/layer/openxr_layer.cpp"
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include "ram_timing_fixture.hpp"

namespace {
#ifdef OFXR_TEST_DJULES_ROUTE
constexpr bool kTestDjules=true;
#else
constexpr bool kTestDjules=false;
#endif
using Microsoft::WRL::ComPtr;
std::shared_ptr<SessionState> session_state;
std::mutex test_gpu, resources_mutex;
std::unordered_map<XrSwapchain, ComPtr<ID3D12Resource>> images;
std::unordered_set<XrSwapchain> synthetic_handles;
std::vector<std::pair<unsigned, bool>> submissions;
std::atomic<unsigned> waits{}, begins{}, ends{}, created{}, destroyed{};
unsigned empty_submissions{};
XrTime runtime_time = 1000000000;
bool correct = true;
bool alternating_eyes{};
bool promotion_test{};
bool fsr_test{}, fsr_missing{}, fsr_packed{};
bool fsr_queue{}, fsr_queue_slow{};
bool fsr_queue_switch{};
bool fsr_stereo_guard{};
std::mutex guard_journal_mutex;
bool guard_runtime_waited{}, guard_runtime_begun{};
bool legacy_queue{};
bool legacy_early_current{};
bool midpoint_cadence{};
bool deep_readiness{};
bool triple_core{};
bool triple_inline{};
bool triple_switch{};
bool deep_switch{};
bool lower_vram_test{}, direct_output_test{};
XrSwapchain last_synthetic_handle{};
unsigned last_synthetic_frame{};
bool fsr_queue_gpu_pending{};
ComPtr<ID3D12Fence> fsr_gpu_gate;
std::jthread fsr_gpu_watchdog;
std::atomic<bool> fsr_wait_before_gpu_complete{};
std::vector<std::chrono::steady_clock::time_point> output_times;
std::vector<XrTime> output_display_times;
ComPtr<ID3D12Fence> promotion_gate;
std::jthread promotion_unblock;
std::atomic<bool> promotion_gate_released{};
unsigned promotion_gate_count{};
void require(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
void hr(HRESULT r, const char* s) { require(SUCCEEDED(r), s); }

ComPtr<ID3D12Resource> resource(unsigned slices, bool buffer = false,
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT, UINT64 bytes = 0,
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON,
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = heap_type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = buffer ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = buffer ? bytes : 256; d.Height = buffer ? 1 : 128;
    d.DepthOrArraySize = static_cast<UINT16>(buffer ? 1 : slices); d.MipLevels = 1;
    d.Format = buffer ? DXGI_FORMAT_UNKNOWN : format;
    d.SampleDesc.Count = 1;
    d.Layout = buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = buffer ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;
    hr(session_state->d3d12_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,initial,nullptr,
        IID_PPV_ARGS(&r)), "resource");
    return r;
}

template<class Record> void commands(Record record, ID3D12CommandQueue* execution_queue=nullptr) {
    if(!execution_queue) execution_queue=session_state->d3d12_queue.Get();
    std::scoped_lock lock(test_gpu);
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    hr(session_state->d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    hr(session_state->d3d12_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    record(list.Get()); hr(list->Close(),"close");
    ID3D12CommandList* lists[]={list.Get()}; execution_queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;
    hr(session_state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    hr(execution_queue->Signal(fence.Get(),1),"signal");
    hr(wait_composition_fence(fence.Get(),1),"test GPU completion");
}
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; list->ResourceBarrier(1,&x);
}
void fill(ID3D12Resource* r, unsigned red, unsigned green, bool stereo = false) {
    auto desc=r->GetDesc(); std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layout(desc.DepthOrArraySize);
    UINT64 bytes=0; session_state->d3d12_device->GetCopyableFootprints(&desc,0,desc.DepthOrArraySize,0,layout.data(),nullptr,nullptr,&bytes);
    auto upload=resource(1,true,D3D12_HEAP_TYPE_UPLOAD,bytes,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped=nullptr; hr(upload->Map(0,nullptr,&mapped),"upload map");
    for(unsigned slice=0;slice<desc.DepthOrArraySize;++slice) for(unsigned y=0;y<128;++y) {
        auto* row=reinterpret_cast<unsigned*>(static_cast<unsigned char*>(mapped)+layout[slice].Offset+y*layout[slice].Footprint.RowPitch);
        for(unsigned x=0;x<256;++x) row[x]=0xff200000u|((stereo && (fsr_packed ? x>=128 : slice!=0) ? 160 : green)<<8)|red;
    }
    upload->Unmap(0,nullptr);
    commands([&](auto* list){
        barrier(list,r,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        for(unsigned slice=0;slice<desc.DepthOrArraySize;++slice) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=r; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=slice;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=layout[slice];
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        barrier(list,r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
    });
}
unsigned pixel(ID3D12Resource* r, unsigned slice,
    D3D12_RESOURCE_STATES submitted_state = D3D12_RESOURCE_STATE_COMMON,
    unsigned sample_x = 128) {
    auto desc=r->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 bytes=0;
    session_state->d3d12_device->GetCopyableFootprints(&desc,slice,1,0,&layout,nullptr,nullptr,&bytes);
    auto readback=resource(1,true,D3D12_HEAP_TYPE_READBACK,bytes,D3D12_RESOURCE_STATE_COPY_DEST);
    commands([&](auto* list){
        barrier(list,r,submitted_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=r; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=slice;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=layout;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier(list,r,D3D12_RESOURCE_STATE_COPY_SOURCE,submitted_state);
#ifdef OFXR_TEST_DJULES_ROUTE
    },(deep_readiness||triple_core)?runtime_queue(*session_state):nullptr);
#else
    });
#endif
    void* mapped=nullptr; hr(readback->Map(0,nullptr,&mapped),"readback");
    const unsigned value=*reinterpret_cast<unsigned*>(static_cast<unsigned char*>(mapped)+layout.Offset+64*layout.Footprint.RowPitch+sample_x*4);
    readback->Unmap(0,nullptr); return value;
}
XrResult XRAPI_CALL fake_create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* h) {
    auto r=resource(info->arraySize);
    *h=reinterpret_cast<XrSwapchain>(std::uintptr_t(0x1000+ ++created));
    std::scoped_lock lock(resources_mutex); images[*h]=r; return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_enumerate(XrSwapchain h,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* out) {
    *count=1;
    if(capacity){std::scoped_lock lock(resources_mutex);reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)->texture=images.at(h).Get();}
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_destroy(XrSwapchain h) {
    std::scoped_lock lock(resources_mutex); destroyed+=static_cast<unsigned>(images.erase(h)); return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_wait(XrSession,const XrFrameWaitInfo*,XrFrameState* s) {
    if(fsr_stereo_guard) {
        std::scoped_lock lock(guard_journal_mutex);
        require(!guard_runtime_waited && !guard_runtime_begun,"guard double physical Wait");
        guard_runtime_waited=true;
    }
    if(fsr_queue_gpu_pending && fsr_gpu_gate && fsr_gpu_gate->GetCompletedValue()==0) {
        fsr_wait_before_gpu_complete=true;
        hr(fsr_gpu_gate->Signal(1), "release pending application GPU work at XR Wait");
    }
    if(fsr_test && !fsr_queue) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ++waits; runtime_time+=8333333; s->predictedDisplayTime=runtime_time;
    s->predictedDisplayPeriod=8333333; s->shouldRender=XR_TRUE; return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_begin(XrSession,const XrFrameBeginInfo*) {
    if(fsr_stereo_guard) {
        std::scoped_lock lock(guard_journal_mutex);
        require(guard_runtime_waited && !guard_runtime_begun,"guard unmatched physical Begin");
        guard_runtime_waited=false;guard_runtime_begun=true;
    }
    ++begins; return XR_SUCCESS;
}
ComPtr<ID3D12Resource> submitted_image(XrSwapchain handle) {
    {
        std::scoped_lock lock(resources_mutex);
        const auto found=images.find(handle);
        if(found!=images.end()) return found->second;
    }
    std::scoped_lock lock(g_state_mutex);
    const auto found=g_swapchains.find(handle);
    require(found!=g_swapchains.end() && !found->second->enumerated_d3d12_images.empty(),
        "submitted image is not known to the fake runtime");
    return found->second->enumerated_d3d12_images[0];
}
bool is_private_image(XrSwapchain handle) {
    std::scoped_lock lock(resources_mutex);
    return images.contains(handle);
}
XrResult XRAPI_CALL fake_end(XrSession,const XrFrameEndInfo* end) {
    if(fsr_stereo_guard) {
        std::scoped_lock lock(guard_journal_mutex);
        require(guard_runtime_begun && !guard_runtime_waited,"guard unmatched physical End");
        guard_runtime_begun=false;
    }
    // Cadence receipts measure entry into EndFrame, before its simulated
    // compositor stall. Empty call-order probes carry no output receipt.
    if (end->layerCount) {
        output_times.push_back(std::chrono::steady_clock::now());
        output_display_times.push_back(end->displayTime);
    }
    if(fsr_queue_slow && ends.load() == 5) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ++ends;
    if (!end->layerCount) { ++empty_submissions; return XR_SUCCESS; }
    const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[0]);
    bool synthetic=synthetic_handles.contains(projection->views[0].subImage.swapchain);
    const unsigned frame_id=static_cast<unsigned>(projection->views[0].pose.position.y);
    if (direct_output_test && (triple_core || triple_inline) && synthetic) {
        if(last_synthetic_frame==frame_id)
            require(last_synthetic_handle!=projection->views[0].subImage.swapchain,"3X synthetic outputs alias one direct handle");
        last_synthetic_handle=projection->views[0].subImage.swapchain; last_synthetic_frame=frame_id;
    }
    if (legacy_early_current && synthetic && session_state->queued_nvidia_presenter) {
        std::shared_ptr<GeneratedFrameEndInfo> next;
        {
            std::scoped_lock lock(session_state->presenter_mutex);
            require(!session_state->presenter_submissions.empty(), "B queued alongside S");
            next=session_state->presenter_submissions.front()->owned_frame;
        }
        require(next != nullptr, "private B ownership");
        require(next->pending_current_copies.empty(), "B copy is already submitted before S EndFrame");
        const auto* b=reinterpret_cast<const XrCompositionLayerProjection*>(next->info.layers[0]);
        for (unsigned eye=0;eye<2;++eye) {
            const auto sub=b->views[eye].subImage;
            const auto image=submitted_image(sub.swapchain);
            require((pixel(image.Get(),sub.imageArrayIndex)&255)==
                static_cast<unsigned>(b->views[eye].pose.position.y),
                "fresh private B pixels available before S EndFrame returns");
        }
    }
    unsigned real_id=0;
    for(unsigned eye=0;eye<projection->viewCount;++eye) {
        auto sub=projection->views[eye].subImage;
        ComPtr<ID3D12Resource> image=submitted_image(sub.swapchain);
        const unsigned p=pixel(image.Get(),sub.imageArrayIndex,
            is_private_image(sub.swapchain)
                ? D3D12_RESOURCE_STATE_COMMON
                : D3D12_RESOURCE_STATE_RENDER_TARGET,
            sub.imageRect.offset.x+sub.imageRect.extent.width/2);
        if(((p>>8)&255)!=(eye?160u:80u))
            std::cerr<<"eye mismatch frame="<<frame_id<<" synthetic="<<synthetic<<" eye="<<eye<<" pixel="<<std::hex<<p<<std::dec<<'\n';
        correct &= ((p>>8)&255)==(eye?160u:80u);
        if(!synthetic) {
            if(eye==0) real_id=p&255;
            const unsigned expected=alternating_eyes && frame_id>1 && frame_id%2!=eye ? frame_id-1:frame_id;
            if((p&255)!=expected)std::cerr<<"real mismatch frame="<<frame_id<<" eye="<<eye<<" expected="<<expected<<" got="<<(p&255)<<'\n';
            correct &= (p&255)==expected;
        }
    }
    correct &= end->layerCount==(promotion_test && !session_state->three_slot_director ? 1u : 3u);
    for(unsigned i=1;i<end->layerCount;++i) {
        const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(end->layers[i]);
        correct &= q->type==XR_TYPE_COMPOSITION_LAYER_QUAD && q->subImage.imageArrayIndex==i-1;
        ComPtr<ID3D12Resource> image=submitted_image(q->subImage.swapchain);
        const unsigned p=pixel(image.Get(),q->subImage.imageArrayIndex,
            is_private_image(q->subImage.swapchain)
                ? D3D12_RESOURCE_STATE_COMMON
                : D3D12_RESOURCE_STATE_RENDER_TARGET);
        const unsigned expected=i==2 ? 55 : static_cast<unsigned>(q->pose.position.y);
        if((p&255)!=expected || ((p>>8)&255)!=60*i)
            std::cerr<<"HUD mismatch frame="<<frame_id<<" layer="<<i<<" expected="<<expected<<" pixel="<<std::hex<<p<<std::dec<<'\n';
        correct &= (p&255)==expected && ((p>>8)&255)==60*i;
    }
    submissions.emplace_back(frame_id,synthetic);
    // Real EndFrame promotion: the synthetic has been consumed, but its
    // deferred real endpoint has not even been submitted yet. Hold that GPU
    // queue so a premature director start cannot hide behind a fast copy.
    if(promotion_test && synthetic && session_state->presenter_start_requested &&
       !session_state->three_slot_director) {
        ++promotion_gate_count;
        hr(session_state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&promotion_gate)),"promotion fence");
        auto* endpoint_queue=session_state->d3d12_synthesis_queue.Get();
        // Staging already contains B. Its remaining operation is the handover
        // copy on the binding queue; block that actual endpoint dependency.
        if(advanced_native_outputs(*session_state)) endpoint_queue=runtime_queue(*session_state);
        hr(endpoint_queue->Wait(promotion_gate.Get(),1),"delay final endpoint dependency");
        promotion_unblock=std::jthread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            promotion_gate_released=true;
            promotion_gate->Signal(1);
        });
    }
    return XR_SUCCESS;
}
std::shared_ptr<SwapchainState> source(unsigned id,unsigned slices,bool projection,bool static_image=false) {
    auto c=std::make_shared<SwapchainState>(session_state, XrSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO});
    c->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t(id));
    c->create_info={XR_TYPE_SWAPCHAIN_CREATE_INFO}; c->create_info.width=256;c->create_info.height=128;
    c->create_info.arraySize=slices;c->create_info.faceCount=1;c->create_info.mipCount=1;c->create_info.sampleCount=1;
    c->create_info.format=DXGI_FORMAT_R8G8B8A8_UNORM;c->create_info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    if(static_image)c->create_info.createFlags=XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    c->enumerated_d3d12_images.push_back(resource(slices,false,D3D12_HEAP_TYPE_DEFAULT,0,D3D12_RESOURCE_STATE_RENDER_TARGET));
    c->enumerated_image_count=1;
    if(projection) {
        c->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* sources[]={c->enumerated_d3d12_images[0].Get()};
        hr(c->d3d12_history->initialize(session_state->d3d12_device.Get(),session_state->d3d12_queue.Get(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET),"history");
        SwapchainEligibilityReason reason{};uint64_t detail{};
        c->frame_generation=create_d3d12_frame_generation_swapchains(c,&reason,&detail);
        require(c->frame_generation!=nullptr,"native generation");
        for(auto& image:c->frame_generation->synthetic)synthetic_handles.insert(image.handle);
    }
    {std::scoped_lock lock(g_state_mutex);g_swapchains[c->handle]=c;}
    return c;
}
void release(const std::shared_ptr<SwapchainState>& c, bool poison = true) {
    c->acquired_indices.push_back(0);c->waited_count=1;
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    require(XR_SUCCEEDED(layer_release_swapchain_image_impl(c->handle,&info)),"source release");
    if(!fsr_test && poison) fill(c->enumerated_d3d12_images[0].Get(),239,238); // queued director must own a private capture
}
}

int main(int argc,char** argv) {
    struct RestoreRoute {
        std::optional<xrfg::embedded::Settings> previous;
        ~RestoreRoute() { if(previous) static_cast<void>(xrfg::embedded::request(*previous)); }
    } restore_route;
    RamTimingFixture capture(argc>1 && (std::string_view(argv[1])=="ram" || std::string_view(argv[1]).starts_with("gap-")));
    ComPtr<ID3D12Fence> blocked_synthesis, policy_gate;
    std::jthread policy_unblock;
    std::vector<std::shared_ptr<SwapchainState>> chains;
    try {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        auto dispatch=std::make_shared<Dispatch>(); session_state=std::make_shared<SessionState>(dispatch);
        auto& s=session_state;s->handle=reinterpret_cast<XrSession>(0x211);s->graphics_binding=SessionGraphicsBinding::d3d12;
        s->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::nvidia;
        s->nvidia_new_route = true; s->djules_route=kTestDjules;
        const std::string_view mode = argc>1 ? argv[1] : "";
        lower_vram_test=mode.ends_with("lower-vram"); direct_output_test=mode.ends_with("direct-output");
        if(lower_vram_test) s->lower_vram=1;
        if(direct_output_test) s->lower_vram=0;
        if(mode=="legacy-queue-lower-vram" || mode=="legacy-queue-direct-output") s->nvidia_new_route=false;
        if(mode=="fidelityfx-lower-vram") { s->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx; s->nvidia_new_route=false; s->djules_route=false; }
#ifdef OFXR_TEST_DJULES_ROUTE
        deep_readiness=argc>1 && std::string_view(argv[1])=="deep-readiness";
        deep_switch=argc>1 && std::string_view(argv[1])=="deep-switch";
        deep_readiness=deep_readiness || deep_switch;
        s->djules_deep_pipeline=deep_readiness;
        triple_switch=argc>1 && std::string_view(argv[1])=="triple-switch";
        triple_core=triple_switch || (argc>1 && std::string_view(argv[1]).starts_with("triple-core"));
        triple_inline=argc>1 && std::string_view(argv[1]).starts_with("triple-inline");
        s->djules_triple_requested=triple_core||triple_inline;
        if(triple_inline) { dispatch->virtual_desktop_runtime=false; dispatch->inline_unless_pipelined=true; }
#endif
        fsr_test=argc>1 && std::string_view(argv[1]).starts_with("fsr-");
        legacy_queue=argc>1 && std::string_view(argv[1]).starts_with("legacy-queue");
        legacy_early_current=argc>1 && std::string_view(argv[1])=="legacy-queue-early-b";
        midpoint_cadence=argc>1 && std::string_view(argv[1])=="midpoint-cadence";
        if(legacy_queue) {
            s->nvidia_new_route=false;
            s->minimum_runtime_display_period=8333333;
            ProjectionSnapshot previous, current;
            previous.display_time=1000000000;
            for(int periods=1;periods<=5;++periods) {
                current.display_time=previous.display_time+periods*8333333;
                require(synthetic_interpolation_fraction(s,previous,current)==0.5F,
                    "legacy NVIDIA fraction must stay at the validated midpoint");
            }
            current.display_time=previous.display_time+3*8333333;
            s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
            require(synthetic_interpolation_fraction(s,previous,current)>0.66F,
                "FSR fraction contract changed during legacy-only correction");
            s->frame_generation_mode={};
            dispatch->steamvr_runtime=std::string_view(argv[1]) != "legacy-queue-vdxr";
        }
        fsr_queue=argc>1 && std::string_view(argv[1]).starts_with("fsr-queue");
        fsr_queue_slow=argc>1 && std::string_view(argv[1]) == "fsr-queue-slow";
        fsr_queue_slow=fsr_queue_slow || (argc>1 && std::string_view(argv[1]) == "legacy-queue-slow");
        fsr_queue_switch=argc>1 && std::string_view(argv[1]) == "fsr-queue-switch";
        fsr_queue_gpu_pending=argc>1 && std::string_view(argv[1]) == "fsr-queue-gpu-pending";
        fsr_stereo_guard=argc>1 && std::string_view(argv[1]).ends_with("stereo-guard");
        fsr_missing=argc>1 && std::string_view(argv[1])=="fsr-inline-wait";
        if(fsr_test) {
            s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
            dispatch->steamvr_runtime=fsr_queue;
        }
        s->control_revision=xrfg::embedded::snapshot().revision;
        hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&s->d3d12_device)),"device");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(s->d3d12_device->CreateCommandQueue(&q,IID_PPV_ARGS(&s->d3d12_queue)),"application queue");
        hr(s->d3d12_device->CreateCommandQueue(&q,IID_PPV_ARGS(&s->d3d12_synthesis_queue)),"synthesis queue");
#ifdef OFXR_TEST_DJULES_ROUTE
        if(deep_readiness||triple_core) {
            dispatch->steamvr_runtime=true;
            hr(s->d3d12_device->CreateCommandQueue(&q,IID_PPV_ARGS(&s->binding_queue)),"DJ runtime binding queue");
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->app_release_fence)),"DJ app release fence");
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->reacquire_fence)),"DJ reacquire fence");
        }
#endif
        dispatch->create_swapchain=fake_create;dispatch->enumerate_swapchain_images=fake_enumerate;dispatch->destroy_swapchain=fake_destroy;
        dispatch->acquire_swapchain_image=[](XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* index)->XrResult{*index=0;return XR_SUCCESS;};
        dispatch->wait_swapchain_image=[](XrSwapchain,const XrSwapchainImageWaitInfo*)->XrResult{return XR_SUCCESS;};
        dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*)->XrResult{return XR_SUCCESS;};
        dispatch->wait_frame=fake_wait;dispatch->begin_frame=fake_begin;dispatch->end_frame=fake_end;
        {std::scoped_lock lock(g_state_mutex);g_sessions[s->handle]=s;}
        promotion_test=argc>1 && std::string_view(argv[1]).starts_with("promotion");
        const bool gap_test=argc>1 && std::string_view(argv[1]).starts_with("gap-");
        fsr_packed=mode=="fsr-packed" || mode=="fsr-queue-packed";
        const bool array=fsr_test || (argc>1 && (std::string_view(argv[1])=="array" || std::string_view(argv[1])=="promotion-array" || std::string_view(argv[1])=="gap-array"));
        alternating_eyes=argc>1 && std::string_view(argv[1])=="repeat";
        auto left=source(0x201,array && !fsr_packed?2:1,true);chains.push_back(left);
        auto right=array?left:source(0x202,1,true);if(!array)chains.push_back(right);
        auto dynamic=source(0x203,2,false);auto fixed=source(0x204,2,false,true);
        chains.push_back(dynamic);chains.push_back(fixed);
        if(argc>1 && std::string_view(argv[1])=="route-shapes") {
            restore_route.previous=xrfg::embedded::snapshot().desired;
            fill(left->enumerated_d3d12_images[0].Get(),42,80);
            release(left);
            const auto history=left->d3d12_history;
            const auto capture=left->last_released_capture;
            require(capture.has_value(),"owned capture before route change");
            for(const int lower:{-1,0,1}) for(const int route:{1,3,2,1,3,2}) {
                auto desired=*restore_route.previous;
                desired.enabled=true;desired.backend=route;desired.frame_generation=0;desired.lower_vram=lower;
                require(xrfg::embedded::request(desired),"GPU route request");
                apply_embedded_control(s);
                require(!s->control_reconfigure_required && s->menu_enabled,"GPU route reconfiguration");
                require(left->d3d12_history==history && left->last_released_capture.has_value(),"route replaced source ownership");
                for(const auto& chain:{left,right}) {
                    const auto generation=chain->frame_generation;
                    require(generation && generation->staged_outputs==(lower<0 ? route>=2 : lower!=0),"stale private output shape");
                    require(generation->direct_extra_outputs==(!generation->staged_outputs && route==3),"stale direct 3X capacity");
                    require(generation->staging_stride==(generation->staged_outputs && route==3 ? 2U : 1U),"stale staged 3X capacity");
                    for(const auto& image:generation->synthetic)synthetic_handles.insert(image.handle);
                }
            }
        }
        for(const auto& chain:{left,right}) {
            if(lower_vram_test || direct_output_test)
                require(chain->frame_generation && chain->frame_generation->staged_outputs==lower_vram_test,
                    "explicit output storage was ignored");
        }
        ComPtr<ID3D12Resource> depth, motion;
        if(fsr_test && !fsr_missing) {
            depth=resource(1,false,D3D12_HEAP_TYPE_DEFAULT,0,D3D12_RESOURCE_STATE_RENDER_TARGET,DXGI_FORMAT_R32_FLOAT);
            motion=resource(1,false,D3D12_HEAP_TYPE_DEFAULT,0,D3D12_RESOURCE_STATE_RENDER_TARGET,DXGI_FORMAT_R16G16_FLOAT);
            D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
            ComPtr<ID3D12DescriptorHeap> heap;hr(s->d3d12_device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"guide heap");
            auto d=heap->GetCPUDescriptorHandleForHeapStart();auto m=d;
            m.ptr+=s->d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            s->d3d12_device->CreateRenderTargetView(depth.Get(),nullptr,d);s->d3d12_device->CreateRenderTargetView(motion.Get(),nullptr,m);
            commands([&](auto* list){const float z[4]{.5F,.5F,.5F,1},v[4]{};
                list->ClearRenderTargetView(d,z,0,nullptr);list->ClearRenderTargetView(m,v,0,nullptr);
                barrier(list,depth.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                barrier(list,motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);});
        }
        s->last_inline_frame_state={XR_TYPE_FRAME_STATE};s->last_inline_frame_state.predictedDisplayPeriod=8333333;
        s->last_inline_frame_state.predictedDisplayTime=runtime_time;s->last_inline_frame_state.shouldRender=XR_TRUE;s->last_inline_frame_state_valid=true;
        if(fsr_test) require(!three_slot_generation_session(s),
            "FSR was admitted to the three-slot director");
        if(!promotion_test && !fsr_test && !legacy_queue && !triple_inline && s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia) {
            require(start_continuous_presenter(s,nullptr,false),"promotion");
            if(s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia)
                require(s->three_slot_director && s->synthesis_thread.joinable(),"native did not enter three lanes");
            else require(!s->three_slot_director && !s->synthesis_thread.joinable(),"FidelityFX entered NVIDIA director");
        }
        const unsigned real_count=fsr_stereo_guard?13:8;
        std::optional<XrFrameState> guard_next;
        if(fsr_stereo_guard && fsr_queue) {
            s->pipelined_presenter_mode=true;
            require(start_continuous_presenter(s,nullptr,true),"guard initial virtual owner");
        }
        for(unsigned n=1;n<=real_count;++n) {
#ifdef OFXR_TEST_DJULES_ROUTE
            if(triple_switch && (n==5 || n==7))
                require(XR_SUCCEEDED(switch_djules_pipeline(s,n==7,false)),"live GPU multiplier switch");
            if(deep_switch && (n==5 || n==7)) {
                if(n==5) require(policy_gate->GetCompletedValue()==0,"policy fixture did not retain pending GPU work");
                require(XR_SUCCEEDED(switch_djules_pipeline(s,false,n==7)),"live GPU deep preference switch");
                require(s->djules_deep_pipeline==(n==7),"live preference not applied");
                if(n==5) require(policy_gate->GetCompletedValue()==1,"policy crossed unfinished GPU work");
            }
#endif
            require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"worker rendezvous");
            if(midpoint_cadence && n>2)
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            // RE Village: releases need not be one-to-one with EndFrames.
            // Exercise left-only, right-only, then both-eye double captures.
            if(gap_test && (n==4 || n==6 || n==7)) {
                auto extra=n==6 ? right : left;
                fill(extra->enumerated_d3d12_images[0].Get(),230,200,array);release(extra);
                if(n==7 && !array) {fill(right->enumerated_d3d12_images[0].Get(),230,200);release(right);}
            }
            if(!alternating_eyes || n==1 || n%2==0){fill(left->enumerated_d3d12_images[0].Get(),n,80,array);release(left);}
            if(!array && (!alternating_eyes || n==1 || n%2==1)){fill(right->enumerated_d3d12_images[0].Get(),n,160);release(right);}
            if(fsr_test && !fsr_missing) {
                auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
                for(unsigned eye=0;eye<2;++eye) {
                    auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=eye+1;g->epoch=1;g->serial=n;g->previous_serial=n-1;g->publication=2*n+eye;
                    g->producer_queue=s->d3d12_queue;g->output=left->enumerated_d3d12_images[0];g->output_slice=fsr_packed?0:eye;
                    g->motion_vectors=motion;g->depth=depth;g->output_width=g->depth_width=g->motion_width=256;
                    g->output_height=g->depth_height=g->motion_height=128;g->frame_time_delta_ms=16.6667F;
                    if(fsr_packed) {
                        g->output_width=g->depth_width=g->motion_width=128;
                        g->output_x=g->depth_x=g->motion_x=eye*128;
                    }
                    guides->eyes[eye]=std::move(g);
                }
                if(fsr_stereo_guard && (n==3 || n==4)) guides->eyes[1]=guides->eyes[0];
                if(fsr_stereo_guard && n==6) {
                    auto missing=std::make_shared<xrfg::DlssMotionVectorFrame>(*guides->eyes[1]);
                    missing->motion_vectors.Reset();guides->eyes[1]=std::move(missing);
                }
                std::scoped_lock lock(left->mutex);left->last_released_motion_vectors=std::move(guides);
            }
            // Legacy's first prime is inline with borrowed auxiliary images;
            // poisoning those before its synchronous End violates that input
            // contract. Once queued, poison the dynamic HUD to test ownership.
            // Inline submissions borrow the application's released auxiliary
            // image until synchronous End; only asynchronous consumers require
            // an independently retained release capture.
            fill(dynamic->enumerated_d3d12_images[0].Get(),n,60);release(dynamic,s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia && !triple_inline && (!legacy_queue || n>1));
            if(n==1){fill(fixed->enumerated_d3d12_images[0].Get(),55,120);release(fixed,s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia && !triple_inline && !legacy_queue);}
            XrFrameState prediction{XR_TYPE_FRAME_STATE};
            if(guard_next) {prediction=*guard_next;guard_next.reset();}
            else require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&prediction)),"producer wait");
            if(!fsr_stereo_guard)
                require(prediction.predictedDisplayPeriod==(triple_switch && (n==5 || n==6)?8333333 : (triple_core||triple_inline)?24999999 : legacy_queue && n>1 ? 16666666 : 8333333),"advertised period");
            require(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,nullptr)),"producer begin");
            if(fsr_queue_gpu_pending && n==3) {
                require(XR_SUCCEEDED(wait_for_presenter_idle(s)), "pending GPU test drain");
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fsr_gpu_gate)), "FSR pending GPU gate");
                hr(s->d3d12_queue->Wait(fsr_gpu_gate.Get(),1), "hold application GPU work");
                fsr_gpu_watchdog=std::jthread([] {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    fsr_gpu_gate->Signal(1); // Unblock a regressed CPU wait instead of hanging the test.
                });
            }
            std::array<XrCompositionLayerProjectionView,2> views;
            for(unsigned eye=0;eye<2;++eye) {
                auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;v.pose.position.y=float(n);
                v.pose.position.x=eye?.032F:-.032F;v.fov={-.7F,.7F,.7F,-.7F};
                v.subImage.swapchain=eye?right->handle:left->handle;v.subImage.imageArrayIndex=array?eye:0;v.subImage.imageRect.extent={256,128};
                if(fsr_packed) {
                    v.subImage.imageArrayIndex=0;
                    v.subImage.imageRect.offset.x=eye*128;
                    v.subImage.imageRect.extent.width=128;
                }
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(0x100);
            projection.viewCount=2;projection.views=views.data();
            if(fsr_stereo_guard && n==10) projection.viewCount=1;
            std::array<XrCompositionLayerQuad,2> quads;
            for(unsigned i=0;i<2;++i){auto& v=quads[i];v={XR_TYPE_COMPOSITION_LAYER_QUAD};v.pose.orientation.w=1;v.pose.position.y=float(n);
                v.subImage.swapchain=i?fixed->handle:dynamic->handle;v.subImage.imageArrayIndex=i;v.subImage.imageRect.extent={256,128};}
            const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection),
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[0]),reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[1])};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount=promotion_test && !s->three_slot_director ? 1 : 3;end.layers=layers;
            if(fsr_stereo_guard && n==1) {
                ProjectionSnapshot audit;
                require(capture_projection_snapshot(&end,&audit),"guard audit projection");
                const auto mapped=build_projection_resource_mappings(audit);
                require(mapped.ready(),"guard audit mappings");
                const auto original=left->last_released_motion_vectors;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::ready,
                    "guard rejected independent stereo guides");
                auto shared=std::make_shared<xrfg::DlssMotionVectorSet>(*original);
                shared->eyes[1]=shared->eyes[0];
                left->last_released_motion_vectors=shared;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::shared_eye_domain,
                    "guard admitted duplicated mono guides");
                auto a=std::make_shared<xrfg::DlssMotionVectorFrame>(*shared->eyes[0]);
                auto b=std::make_shared<xrfg::DlssMotionVectorFrame>(*a);
                a->motion_width=b->motion_width=128;b->motion_x=128;
                shared->eyes={a,b};
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::ready,
                    "guard rejected non-overlapping packed stereo motion");
                b->motion_x=64;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::shared_eye_domain,
                    "guard admitted overlapping shared motion domain");
                b->motion_x=0;b->motion_width=256;b->motion_slice=1;
                a->motion_vectors=b->motion_vectors=resource(2,false,D3D12_HEAP_TYPE_DEFAULT,
                    0,D3D12_RESOURCE_STATE_COMMON,DXGI_FORMAT_R16G16_FLOAT);
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::ready,
                    "guard rejected distinct array stereo motion slices");
                b->motion_slice=2;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::invalid_guide_region,
                    "guard admitted out-of-bounds motion slice");
                b->motion_slice=1;
                b->depth.Reset();
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::missing_guides,
                    "guard admitted missing depth");
                audit.layers[0].views.resize(1);
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::non_stereo_projection,
                    "guard admitted mono projection");
                left->last_released_motion_vectors=original;
            }
            const unsigned independence_frame=(fsr_test || legacy_queue || triple_inline || s->optical_flow_backend!=xrfg::D3D12OpticalFlowBackend::nvidia) ? 0 : promotion_test ? 5 : 2;
            if(deep_switch && n==4) {
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&policy_gate)),"policy GPU gate");
                hr(s->d3d12_synthesis_queue->Wait(policy_gate.Get(),1),"hold policy GPU work");
                policy_unblock=std::jthread([gate=policy_gate] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    gate->Signal(1);
                });
            }
            if(n==independence_frame){hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocked_synthesis)),"test gate");
                hr(s->d3d12_synthesis_queue->Wait(blocked_synthesis.Get(),1),"block synthesis");}
            if(fsr_stereo_guard && fsr_queue && n<real_count) {
                XrFrameState next{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&next)),"guard outstanding virtual Wait");
                guard_next=next;
            }
            const auto fsr_used_before=xrfg::fsr_frame_generation_statistics().used;
            require(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&end)),"actual EndFrame");
            if(fsr_stereo_guard) {
                require(XR_SUCCEEDED(wait_for_presenter_idle(s)),"guard boundary drain");
                const bool generated=n==2 || n==9 || n==13;
                require(xrfg::fsr_frame_generation_statistics().used==fsr_used_before+(generated?1:0),
                    "guard dispatched with invalid inputs or failed to resume");
                const bool paused=(n>=3 && n<=7) || n==10 || n==11;
                require(s->fsr_stereo_admission.suspended==paused,"guard suspension/recovery state");
                if(paused) {
                    require(!s->previous_projection,"guard retained pre-menu projection anchor");
                    const auto status=xrfg::fsr_frame_generation_statistics().status;
                    require(status==xrfg::FsrFrameGenerationStatus::stereo_unavailable ||
                        status==xrfg::FsrFrameGenerationStatus::stereo_recovering,"guard status lost");
                }
            }
            if(legacy_queue) {
                require(continuous_presenter_active(s) && s->queued_nvidia_presenter &&
                    !s->three_slot_director && !s->synthesis_thread.joinable(),
                    "legacy NVIDIA did not use fresh-output consumer");
                {
                    std::scoped_lock lock(s->presenter_mutex);
                    require(s->outstanding_presenter_submissions<=2, "legacy backlog exceeds pair");
                }
                if(n==4) {
                    require(XR_SUCCEEDED(wait_for_presenter_idle(s)), "legacy starvation drain");
                    const auto count=ends.load(), wait_count=waits.load();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    require(ends==count && waits==wait_count,
                        "legacy consumer repeated or opened XR cycles while starved");
                }
            }
            if(fsr_test) {
                require(!s->presenter_start_requested,
                    "FSR was queued for automatic presenter promotion");
                require(!s->three_slot_director && !s->synthesis_thread.joinable(), "FSR used NVIDIA worker");
                require(continuous_presenter_active(s) == (fsr_queue && !fsr_missing), "FSR runtime route");
                if(fsr_queue && !fsr_missing) {
                    std::scoped_lock lock(s->presenter_mutex);
                    require(s->outstanding_presenter_submissions <= 2, "FSR output backlog exceeds two");
                }
            }
            if(fsr_queue_switch && n==8) {
                require(XR_SUCCEEDED(wait_for_presenter_idle(s)), "FSR switch drain");
                s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
                s->pipelined_presenter_mode=true; // Also test real-app overlapping waits.
                XrFrameState switch_prediction{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&switch_prediction)), "switch Wait");
                require(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,nullptr)), "switch Begin");
                end.displayTime=switch_prediction.predictedDisplayTime;
                require(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&end)), "switch End");
                require(continuous_presenter_active(s) && !s->fsr_two_slot_presenter && s->queued_nvidia_presenter,
                    "selected NVIDIA owner did not replace the FSR consumer");
                // The former fixture stopped after End, before the failing Begin.
                XrFrameState following{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&following)), "following Wait");
                require(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,nullptr)), "following Begin");
                end.displayTime=following.predictedDisplayTime;
                // Empty composition checks the live frame call chain without
                // adding another GPU-generated output to this pixel fixture.
                auto empty=end; empty.layerCount=0; empty.layers=nullptr;
                require(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&empty)), "following End");
            }
            if(gap_test && (n==4 || n==6 || n==7)) {
                require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"gap prime completed");
                require(!left->frame_generation->synthesizer->has_capture_gap(*left->last_released_capture) &&
                    !right->frame_generation->synthesizer->has_capture_gap(*right->last_released_capture),"both histories not re-anchored");
                require(s->three_slot_director && XR_SUCCEEDED(s->presenter_failure),"gap stopped director");
            }
            if(promotion_test && n==3) {
                require(s->three_slot_director && s->synthesis_thread.joinable(),"EndFrame did not promote");
                require(promotion_gate_count==1,"deferred-copy promotion not exercised");
                require(promotion_gate_released,"director started before delayed endpoint completed");
                require(submissions.size()==5 && submissions.back()==std::pair<unsigned,bool>{3,false},"promotion skipped current endpoint");
            }
            if(n==independence_frame) {
                // A blocked private GPU queue must not enqueue its wait on the
                // application's GPU queue. A marker on that queue still completes.
                ComPtr<ID3D12Fence> marker;hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&marker)),"marker");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                hr(s->d3d12_queue->Signal(marker.Get(),1),"independent marker");
                HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                hr(marker->SetEventOnCompletion(1,event),"marker event");
                const DWORD status=WaitForSingleObject(event,2000);CloseHandle(event);
                hr(blocked_synthesis->Signal(1),"unblock synthesis");
                require(status==WAIT_OBJECT_0,"application GPU queue blocked by synthesis");
            }
        }
        require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"final worker");
        if(!fsr_queue_switch) require(XR_SUCCEEDED(wait_for_presenter_idle(s)),"director drain");
        stop_continuous_presenter(s);
        unsigned repeats=0;
        if(deep_readiness) {
            std::vector<std::pair<unsigned,bool>> fresh;
            for(const auto& receipt:submissions) {
                if(!fresh.empty() && receipt==fresh.back()) {
                    require(!receipt.second,"deep repeat reused synthetic rather than owned prior real"); ++repeats;
                } else fresh.push_back(receipt);
            }
            require(repeats>0,"deep GPU fixture never exercised an owned repeat");
            require(submissions.size()==fresh.size()+repeats,"deep physical output accounting");
            submissions=std::move(fresh);
        }
        const unsigned expected_count=fsr_stereo_guard?16:deep_switch?13:triple_switch?17:(triple_core||triple_inline)?22:fsr_queue_switch ? 16 : fsr_missing ? 8 : gap_test ? 12 : promotion_test ? 14 : 15;
        const unsigned expected_cycles = expected_count + repeats + empty_submissions;
        require(submissions.size()==expected_count && waits==expected_cycles && begins==expected_cycles && ends==expected_cycles,"unique output/cycle count");
        require(empty_submissions == (fsr_queue_switch ? 1U : 0U), "unexpected empty physical submission");
        if(fsr_stereo_guard) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=real_count;++n) {
                if(n==2 || n==9 || n==13) expected.emplace_back(n,true);
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"guard lost/repeated/reordered real or synthetic outputs");
            require(!guard_runtime_waited && !guard_runtime_begun,"guard left an open physical cycle");
        } else if(fsr_missing) {
            for(unsigned i=0;i<8;++i) require(submissions[i]==std::pair<unsigned,bool>{i+1,false},"FSR missing guide repeated/lost real");
        } else if(gap_test) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=8;++n) {
                if(n>1 && n!=4 && n!=6 && n!=7) expected.emplace_back(n,true);
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"gap must skip only synthetic, keep every real, resume pairing");
        } else if(promotion_test) {
            const std::array<unsigned,8> real_positions={0,2,4,5,7,9,11,13};
            unsigned next_real=0;
            for(unsigned i=0;i<submissions.size();++i) {
                if(next_real<real_positions.size() && i==real_positions[next_real]) {
                    require(submissions[i]==std::pair<unsigned,bool>{next_real+1,false},"promotion real endpoint order");
                    ++next_real;
                } else require(submissions[i].second,"promotion synthetic order");
            }
        } else if(deep_switch) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=8;++n) {
                if(n>1 && n!=5 && n!=7) expected.emplace_back(n,true);
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"deep toggle lost or reordered owned output");
        } else if(triple_switch) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=8;++n) {
                if(n>1 && n!=5 && n!=7) {
                    expected.emplace_back(n,true);
                    if(n!=6) expected.emplace_back(n,true);
                }
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"live shape lost, repeated or reordered an owned output");
        } else if(triple_core||triple_inline) {
            for(unsigned i=0;i<7;++i) {
                require(submissions[3*i]==std::pair<unsigned,bool>{i+1,false},"3X real order");
                require(submissions[3*i+1]==std::pair<unsigned,bool>{i+2,true} &&
                    submissions[3*i+2]==std::pair<unsigned,bool>{i+2,true},"3X independent synthetic deliveries");
            }
        } else for(unsigned i=0;i<7;++i){require(submissions[2*i]==std::pair<unsigned,bool>{i+1,false},"real order");require(submissions[2*i+1].second,"synthetic order");}
        require(submissions.back()==std::pair<unsigned,bool>{real_count,false},"last real");require(correct,"stale/cross-eye/auxiliary pixels");
        if(midpoint_cadence) {
            require(output_times.size()==submissions.size(), "midpoint runtime receipts");
            for(unsigned i=1;i<submissions.size();++i)
                if(submissions[i-1].second && !submissions[i].second)
#ifdef OFXR_TEST_DJULES_ROUTE
                    // This fixture's native runtime is neither SteamVR nor VD:
                    // final D26 uses its wait plus a half-period floor (8.33/2).
                    require(output_times[i]-output_times[i-1]>=std::chrono::milliseconds(4),
                        "djules non-SteamVR handover violated the half-period floor");
#else
                    require(output_times[i]-output_times[i-1]>=std::chrono::milliseconds(6),
                        "real endpoint stole a delayed midpoint interval");
#endif
        }
        if(fsr_queue_gpu_pending) require(fsr_wait_before_gpu_complete,
            "FSR consumer CPU-waited before entering XR with pending GPU work");
        if(fsr_queue || legacy_queue) {
            require(waits == expected_cycles && output_times.size() == expected_count, "FSR repeated or lost output");
            require(output_display_times.size()==expected_count, "missing runtime output slot receipt");
            for(unsigned i=1;i<output_times.size();++i) {
                require(output_display_times[i]-output_display_times[i-1]==8333333,
                    "output skipped or reused a physical runtime slot");
                // FSR reanchors after a long End. Reference NVIDIA deliberately
                // keeps its grid through normal wake/End lateness; the fake
                // runtime's synchronous pixel readbacks must not impose an
                // unrelated minimum wall-time gap. Its exact lateness/starvation
                // contract is checked deterministically by TwoOutputSchedule.
                if(fsr_queue)
                    require(output_times[i]-output_times[i-1] >= std::chrono::milliseconds(6), "FSR bunched output slots");
            }
        }
        if(fsr_test) {const auto stats=xrfg::fsr_frame_generation_statistics();
            require(fsr_missing ? stats.attempts==0 : stats.used==(fsr_stereo_guard?3:7) && stats.failures==0,"FSR dispatch count/failed");}
        for(auto& c:chains){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
        require(created==destroyed,"private XR swapchain leak");
        ComPtr<ID3D12InfoQueue> info;
        if(SUCCEEDED(s->d3d12_device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);
            std::vector<unsigned char> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);
            if(m->Severity==D3D12_MESSAGE_SEVERITY_ERROR || m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){std::cerr<<m->pDescription<<'\n';correct=false;}}
        require(correct,"D3D12 debug errors");
        if(promotion_unblock.joinable())promotion_unblock.join();
        promotion_gate.Reset();
        if(fsr_gpu_watchdog.joinable())fsr_gpu_watchdog.join();
        fsr_gpu_gate.Reset();
        std::cout<<"Native D3D12 "<<(array?"array":"split")<<": 8 real/"<<(expected_count-8)<<" synthetic; FSR="<<fsr_test<<"; delayed promotion="<<promotion_test<<"; private poisoned-source eyes + static/dynamic quads; independent GPU queue; no debug errors\n";
        {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
        chains.clear();left.reset();right.reset();dynamic.reset();fixed.reset();
        // Release test-owned session/device before process-static layer globals.
        // Keeping it global past their teardown invalidates driver lifetimes.
        session_state.reset();
        capture.verify(false, true);
        return 0;
    } catch(const std::exception& e) {
        std::filesystem::path trace; std::wstring error;
        xrfg::save_ram_timing(GetCurrentProcessId(),std::filesystem::current_path(),&trace,&error);
        if(promotion_gate)promotion_gate->Signal(1);
        if(promotion_unblock.joinable())promotion_unblock.join();
        if(blocked_synthesis)blocked_synthesis->Signal(1);
        if(fsr_gpu_gate)fsr_gpu_gate->Signal(1);
        if(fsr_gpu_watchdog.joinable())fsr_gpu_watchdog.join();
        if(session_state)stop_continuous_presenter(session_state);
        for(auto& c:chains){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
        {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
        chains.clear();images.clear();promotion_gate.Reset();blocked_synthesis.Reset();fsr_gpu_gate.Reset();session_state.reset();
        std::cerr<<e.what()<<'\n';return 1;
    }
}
