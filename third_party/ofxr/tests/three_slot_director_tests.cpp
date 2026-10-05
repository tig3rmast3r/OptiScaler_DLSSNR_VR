// Compile the actual layer implementation into this test: no test-only switch
// or alternate director implementation is added to the distributed DLL.
#include "../src/layer/openxr_layer.cpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include "ram_timing_fixture.hpp"

namespace {
std::mutex test_mutex;
std::vector<std::pair<unsigned, bool>> presented;
std::atomic<unsigned> waits{}, begins{}, ends{};
std::atomic<bool> fail_runtime{}, blocked_end{}, entered_end{};
std::condition_variable test_condition;
XrTime next_time = 1000000000;
std::unordered_map<XrSwapchain, ID3D11Texture2D*> gpu_outputs;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> readback_context;
Microsoft::WRL::ComPtr<ID3D11Texture2D> readback;
Microsoft::WRL::ComPtr<ID3D11Texture2D> auxiliary_readback;
bool gpu_pixels_correct = true;
bool three_layer_test{};
Microsoft::WRL::ComPtr<ID3D11Device> test_device;
std::unordered_map<XrSwapchain, Microsoft::WRL::ComPtr<ID3D11Texture2D>> auxiliary_outputs;
std::atomic<unsigned> auxiliary_creates{}, auxiliary_destroys{};
bool auxiliary_pixels_correct = true;

XrResult XRAPI_CALL fake_wait(XrSession, const XrFrameWaitInfo*, XrFrameState* out) {
    ++waits;
    next_time += 2000000;
    out->predictedDisplayTime = next_time;
    out->predictedDisplayPeriod = 2000000;
    out->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_begin(XrSession, const XrFrameBeginInfo*) { ++begins; return XR_SUCCESS; }
XrResult XRAPI_CALL fake_end(XrSession, const XrFrameEndInfo* info) {
    ++ends;
    std::unique_lock lock(test_mutex);
    entered_end = true;
    test_condition.notify_all();
    test_condition.wait(lock, [] { return !blocked_end.load(); });
    if (fail_runtime) return XR_ERROR_RUNTIME_FAILURE;
    const auto* layer = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
    const unsigned packed = gpu_outputs.empty()
        ? static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(layer->space))
        : static_cast<unsigned>(layer->views[0].pose.position.y * 2);
    bool synthetic = (packed & 1) != 0;
    if (!gpu_outputs.empty() && layer->viewCount) {
        const auto handle = layer->views[0].subImage.swapchain;
        synthetic = reinterpret_cast<std::uintptr_t>(handle) >= 0x910;
        if (!synthetic) {
            for (unsigned eye = 0; eye != 2; ++eye) {
                const auto& sub = layer->views[eye].subImage;
                readback_context->CopyResource(readback.Get(), gpu_outputs.at(sub.swapchain));
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(readback_context->Map(readback.Get(), sub.imageArrayIndex, D3D11_MAP_READ, 0, &mapped))) {
                    gpu_pixels_correct = false;
                    continue;
                }
                const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
                gpu_pixels_correct &= pixel[0] == packed / 2 && pixel[1] == (eye ? 160 : 80);
                readback_context->Unmap(readback.Get(), sub.imageArrayIndex);
            }
        }
    }
    if (three_layer_test && !info->layers[1]->next) {
        auxiliary_pixels_correct &= info->layerCount == 3;
        for (unsigned i = 1; i < info->layerCount; ++i) {
            const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
            auxiliary_pixels_correct &= quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD &&
                quad->eyeVisibility == (i == 1 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT) &&
                quad->subImage.imageArrayIndex == i-1 &&
                quad->subImage.imageRect.offset.x == 3 && quad->subImage.imageRect.extent.width == 240 &&
                quad->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT &&
                quad->pose.position.x == static_cast<float>(i);
            const auto found = auxiliary_outputs.find(quad->subImage.swapchain);
            if (found == auxiliary_outputs.end()) { auxiliary_pixels_correct = false; continue; }
            readback_context->CopyResource(auxiliary_readback.Get(), found->second.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(readback_context->Map(auxiliary_readback.Get(), i-1, D3D11_MAP_READ, 0, &mapped))) {
                auxiliary_pixels_correct = false; continue;
            }
            const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
            auxiliary_pixels_correct &= pixel[0] == (i == 1 ? packed/2 : 55) && pixel[1] == 60*i;
            readback_context->Unmap(auxiliary_readback.Get(), i-1);
        }
    }
    presented.emplace_back(packed / 2, synthetic);
    return XR_SUCCESS;
}

void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }

std::shared_ptr<ThreeSlotFrame> frame(unsigned sequence, bool synthetic = false) {
    auto entry = std::make_shared<ThreeSlotFrame>();
    entry->source_sequence = sequence;
    entry->previous_sequence = synthetic ? sequence - 1 : 0;
    auto owned = std::make_shared<GeneratedFrameEndInfo>();
    owned->synthetic = synthetic;
    owned->projections.resize(1);
    auto& p = owned->projections[0];
    p.layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    p.layer.space = reinterpret_cast<XrSpace>(std::uintptr_t(sequence * 2 + synthetic));
    owned->layer_pointers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&p.layer));
    owned->info.layers = owned->layer_pointers.data();
    owned->info.layerCount = 1;
    owned->info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    entry->frame = owned;
    return entry;
}

std::shared_ptr<SessionState> session() {
    auto dispatch = std::make_shared<Dispatch>();
    dispatch->wait_frame = fake_wait; dispatch->begin_frame = fake_begin; dispatch->end_frame = fake_end;
    auto state = std::make_shared<SessionState>(dispatch);
    state->handle = reinterpret_cast<XrSession>(0x207);
    state->graphics_binding = SessionGraphicsBinding::d3d11;
    state->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
    state->nvidia_new_route = true;
    state->presenter_active = true;
    state->three_slot_director = true;
    state->presenter_frame_state_valid = true;
    state->presenter_frame_state.predictedDisplayPeriod = 2000000;
    state->presenter_frame_state.predictedDisplayTime = next_time;
    state->presenter_frame_state.shouldRender = XR_TRUE;
    return state;
}

template<class Predicate>
void until(const std::shared_ptr<SessionState>& state, Predicate condition) {
    std::unique_lock lock(state->presenter_mutex);
    check(state->presenter_condition.wait_for(lock, std::chrono::seconds(3), condition), "timed out");
}

void gpu_pipeline(bool with_layers, bool transition = false, bool diagnostic = false, bool luke = false, bool gap = false) {
    three_layer_test = with_layers;
    auto state = session();
    auto generation = std::make_shared<FrameGenerationSwapchainState>();
    try {
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &state->d3d11_device, nullptr, &state->d3d11_context)), "D3D11 hardware");
        check(SUCCEEDED(xrfg::create_d3d12_device_for_d3d11(state->d3d11_device.Get(),
            &state->d3d12_device, &state->d3d12_queue)), "D3D12 bridge");
        const auto diagnostic_root=std::filesystem::current_path()/("capture-integration-"+std::to_string(GetCurrentProcessId()));
        if(diagnostic) {
            std::filesystem::create_directories(diagnostic_root);
            {std::ofstream ini(diagnostic_root/"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\nmax_file_mb=8\n";}
            xrfg::bridge_flight_logger().initialize(diagnostic_root);
            state->frame_capture=std::make_unique<xrfg::FrameCapture>(state->d3d11_device.Get(),state->d3d11_context.Get(),diagnostic_root/"captures");
            check(state->frame_capture->start(true,4),"integration capture start");
        }
        state->dispatch->acquire_swapchain_image = [](XrSwapchain,
            const XrSwapchainImageAcquireInfo*, std::uint32_t* index) -> XrResult {
            *index = 0; return XR_SUCCESS;
        };
        state->dispatch->wait_swapchain_image = [](XrSwapchain,
            const XrSwapchainImageWaitInfo*) -> XrResult { return XR_SUCCESS; };
        state->dispatch->release_swapchain_image = [](XrSwapchain,
            const XrSwapchainImageReleaseInfo*) -> XrResult { return XR_SUCCESS; };
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 256; desc.Height = 128; desc.ArraySize = with_layers ? 1 : 2; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 5> images;
        for (auto& image : images)
            check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&desc, nullptr, &image)), "texture");
        test_device = state->d3d11_device;
        state->dispatch->create_swapchain = [](XrSession, const XrSwapchainCreateInfo* info,
            XrSwapchain* handle) -> XrResult {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = info->width; d.Height = info->height; d.ArraySize = info->arraySize;
            d.MipLevels = info->mipCount; d.Format = static_cast<DXGI_FORMAT>(info->format);
            d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> image;
            if (FAILED(test_device->CreateTexture2D(&d, nullptr, &image))) return XR_ERROR_RUNTIME_FAILURE;
            *handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(0xa00 + ++auxiliary_creates));
            std::scoped_lock lock(test_mutex);
            auxiliary_outputs[*handle] = image;
            return XR_SUCCESS;
        };
        state->dispatch->enumerate_swapchain_images = [](XrSwapchain handle, std::uint32_t capacity,
            std::uint32_t* count, XrSwapchainImageBaseHeader* images) -> XrResult {
            *count = 1;
            if (capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)->texture = auxiliary_outputs.at(handle).Get();
            return XR_SUCCESS;
        };
        state->dispatch->destroy_swapchain = [](XrSwapchain handle) -> XrResult {
            std::scoped_lock lock(test_mutex);
            auxiliary_destroys += static_cast<unsigned>(auxiliary_outputs.erase(handle));
            return XR_SUCCESS;
        };
        const auto initialize_generation = [&](const auto& generation, const auto& images, unsigned base) {
        ID3D11Texture2D* sources[] = {images[0].Get()};
        ID3D11Texture2D* reals[] = {images[1].Get(), images[2].Get()};
        ID3D11Texture2D* synthetics[] = {images[3].Get(), images[4].Get()};
        generation->d3d11_interop = std::make_shared<xrfg::D3D11D3D12SwapchainInterop>();
        auto& interop = generation->d3d11_interop;
        check(SUCCEEDED(interop->initialize(state->d3d11_device.Get(), state->d3d11_context.Get(),
            state->d3d12_device.Get(), state->d3d12_queue.Get(), sources, reals, synthetics)), "interop");
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        check(SUCCEEDED(history->initialize(state->d3d12_device.Get(), state->d3d12_queue.Get(),
            interop->source_images(), D3D12_RESOURCE_STATE_COMMON)), "history");
        generation->synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        check(SUCCEEDED(generation->synthesizer->initialize(state->d3d12_device.Get(),
            state->d3d12_queue.Get(), history, interop->current_destination_images(),
            interop->synthetic_destination_images(), desc.Format, D3D12_RESOURCE_STATE_COMMON,
            xrfg::D3D12OpticalFlowBackend::nvidia)), "NVIDIA synthesis");
        generation->current_images_per_slot = generation->synthetic_images_per_slot = 1;
        for (unsigned i = 0; i != 2; ++i) {
            generation->current[i].handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(base+i));
            generation->synthetic[i].handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(base+0x10+i));
            gpu_outputs[generation->current[i].handle] = reals[i];
            gpu_outputs[generation->synthetic[i].handle] = synthetics[i];
        }
        return history;
        };
        const auto history = initialize_generation(generation, images, 0x900);
        const auto interop = generation->d3d11_interop;
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 5> right_images;
        auto right_generation = std::make_shared<FrameGenerationSwapchainState>();
        std::shared_ptr<xrfg::D3D12SwapchainHistory> right_history;
        if (with_layers) {
            for (auto& image : right_images)
                check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&desc, nullptr, &image)), "right texture");
            right_history = initialize_generation(right_generation, right_images, 0x920);
        }
        readback_context = state->d3d11_context;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&desc, nullptr, &readback)), "readback");
        auto aux_desc = desc; aux_desc.ArraySize = 2;
        check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&aux_desc, nullptr, &auxiliary_readback)), "aux readback");
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.width = 256; info.height = 128; info.arraySize = desc.ArraySize; info.mipCount = 1;
        info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        auto chain = std::make_shared<SwapchainState>(state, info);
        chain->handle = reinterpret_cast<XrSwapchain>(0x800);
        gpu_outputs[chain->handle] = images[0].Get();
        chain->d3d12_history = history; chain->frame_generation = generation;
        chain->enumerated_d3d11_images.push_back(images[0]);
        auto right_chain = std::make_shared<SwapchainState>(state, info);
        right_chain->handle = reinterpret_cast<XrSwapchain>(0x801);
        right_chain->d3d12_history = right_history; right_chain->frame_generation = right_generation;
        if (with_layers) gpu_outputs[right_chain->handle] = right_images[0].Get();
        if (with_layers) right_chain->enumerated_d3d11_images.push_back(right_images[0]);
        std::array<std::shared_ptr<SwapchainState>, 2> auxiliary;
        if (with_layers) {
            for (unsigned i = 0; i != 2; ++i) {
                auto auxiliary_info = info; auxiliary_info.arraySize = 2;
                auxiliary[i] = std::make_shared<SwapchainState>(state, auxiliary_info);
                auxiliary[i]->handle = reinterpret_cast<XrSwapchain>(std::uintptr_t(0x820+i));
                if (i == 1) auxiliary[i]->create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
                // Use the same dimensions/array layout as the projection for
                // readback, but distinct textures and per-layer eye markers.
                D3D11_TEXTURE2D_DESC d{}; images[0]->GetDesc(&d);
                d.ArraySize = 2;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> image;
                check(SUCCEEDED(state->d3d11_device->CreateTexture2D(&d, nullptr, &image)), "aux source");
                auxiliary[i]->enumerated_d3d11_images.push_back(image);
            }
        }
        state->control_revision = xrfg::embedded::snapshot().revision;
        { std::scoped_lock lock(g_state_mutex);
          g_swapchains[chain->handle] = chain; g_sessions[state->handle] = state;
          if (with_layers) g_swapchains[right_chain->handle] = right_chain;
          for (const auto& a : auxiliary) if (a) g_swapchains[a->handle] = a; }
        state->synthesis_thread = std::thread(synthesis_worker_main, state);
        state->presenter_thread = std::thread(three_slot_director_main, state);
        presented.clear(); waits = begins = ends = 0;
        for (unsigned n = 1; n <= 8; ++n) {
            if (transition && n == 6)
                check(start_continuous_presenter(state, nullptr, true), "re-promotion");
            XrFrameState luke_prediction{XR_TYPE_FRAME_STATE};
            if (luke) {
                check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle,nullptr,&luke_prediction)), "GPU Luke Wait");
                check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle,nullptr)), "GPU Luke Begin");
            }
            // This is the producer handoff, not a frame-start pacing gate.
            check(XR_SUCCEEDED(wait_for_synthesis_handoff(state)), "GPU worker rendezvous");
            std::vector<std::uint32_t> pixels(256 * 128);
            for (unsigned eye = 0; eye != 2; ++eye) {
                std::fill(pixels.begin(), pixels.end(), 0xff200000u | ((eye ? 160u : 80u) << 8) | n);
                if (!with_layers || eye == 0) state->d3d11_context->UpdateSubresource(images[0].Get(), eye, nullptr,
                    pixels.data(), 256 * 4, 0);
                if (with_layers && eye == 1) state->d3d11_context->UpdateSubresource(right_images[0].Get(), 0, nullptr,
                    pixels.data(), 256*4, 0);
            }
            xrfg::D3D12HistoryCaptureTicket capture{};
            if(diagnostic || luke || gap) {
                for(const auto& c:{chain,right_chain}) {
                    c->acquired_indices.push_back(0);c->front_waited=true;
                    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    check(XR_SUCCEEDED(luke ? release_swapchain_with_identity(c,&release,xrfg::LukeRenderIdentity{0x12340000,n})
                        : layer_release_swapchain_image_impl(c->handle,&release)),"projection release");
                    if(diagnostic) check(c->last_diagnostic_capture!=0,"release hook missed diagnostic sample");
                }
            } else {
            check(SUCCEEDED(interop->prepare_capture(0)), "capture transfer");
            check(SUCCEEDED(history->capture(0, &capture)), "capture ring");
            check(SUCCEEDED(interop->finish_capture()), "capture finish");
            check(SUCCEEDED(history->commit(capture)), "capture commit");
            { std::scoped_lock lock(chain->mutex); chain->last_released_capture = capture; }
            if (with_layers) {
                const auto& bridge = right_generation->d3d11_interop;
                check(SUCCEEDED(bridge->prepare_capture(0)), "right capture transfer");
                check(SUCCEEDED(right_history->capture(0, &capture)), "right capture ring");
                check(SUCCEEDED(bridge->finish_capture()), "right capture finish");
                check(SUCCEEDED(right_history->commit(capture)), "right capture commit");
                std::scoped_lock lock(right_chain->mutex); right_chain->last_released_capture = capture;
            }
            }
            if(gap && (n==4 || n==6)) {
                auto c=n==4 ? chain : right_chain;
                c->acquired_indices.push_back(0);c->front_waited=true;
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                check(XR_SUCCEEDED(layer_release_swapchain_image_impl(c->handle,&release)),"extra projection release");
            }
            std::array<XrCompositionLayerQuad, 2> quads{};
            XrBaseInStructure unsupported_extension{XR_TYPE_UNKNOWN};
            if (with_layers) {
                for (unsigned i = 0; i != 2; ++i) {
                    auto& aux = auxiliary[i];
                    if (i == 0 || n == 1) { // static second layer releases ONLY once
                        std::fill(pixels.begin(), pixels.end(), 0xff200000u | (60u*(i+1) << 8) | (i ? 55 : n));
                        for (unsigned eye = 0; eye != 2; ++eye)
                            state->d3d11_context->UpdateSubresource(aux->enumerated_d3d11_images[0].Get(), eye,
                                nullptr, pixels.data(), 256*4, 0);
                        aux->acquired_indices.push_back(0); aux->front_waited = true;
                        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        check(XR_SUCCEEDED(layer_release_swapchain_image_impl(aux->handle, &release)), "aux release");
                        // Poison the app texture BEFORE EndFrame. Any capture
                        // at EndFrame instead of intercepted release fails.
                        std::fill(pixels.begin(), pixels.end(), 0xffeeffffu);
                        for (unsigned eye = 0; eye != 2; ++eye)
                            state->d3d11_context->UpdateSubresource(aux->enumerated_d3d11_images[0].Get(), eye,
                                nullptr, pixels.data(), 256*4, 0);
                    }
                    auto& q = quads[i]; q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                    q.eyeVisibility = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
                    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    q.subImage.swapchain = aux->handle; q.subImage.imageArrayIndex = i;
                    q.subImage.imageRect = {{3, 0}, {240, 128}};
                    q.pose.orientation.w = 1; q.pose.position.x = static_cast<float>(i+1);
                }
                if (transition && n == 5) quads[0].next = &unsupported_extension;
            }
            std::array<XrCompositionLayerProjectionView, 2> views;
            for (unsigned eye = 0; eye != 2; ++eye) {
                auto& view = views[eye]; view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose.orientation.w = 1; view.pose.position.x = eye ? 0.032F : -0.032F;
                view.pose.position.y = static_cast<float>(n);
                view.fov = {-0.7F, 0.7F, 0.7F, -0.7F};
                view.subImage.swapchain = chain->handle; view.subImage.imageArrayIndex = with_layers ? 0 : eye;
                if (with_layers && eye == 1) view.subImage.swapchain = right_chain->handle;
                view.subImage.imageRect.extent = {256, 128};
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = reinterpret_cast<XrSpace>(0x100);
            projection.viewCount = 2; projection.views = views.data();
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection),
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[0]),
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[1])};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = 1000000000 + n * 4000000;
            if (luke) end.displayTime = luke_prediction.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount = with_layers ? 3 : 1; end.layers = layers;
            ProjectionSnapshot snapshot;
            check(capture_projection_snapshot(&end, &snapshot), "snapshot");
            auto mappings = build_projection_resource_mappings(snapshot);
            check(mappings.ready(), "stereo mappings");
            check(mappings.mappings.size() == (with_layers ? 2 : 1), "split-eye mapping count");
            if (!luke) { std::scoped_lock lock(state->mutex); state->pending_frames.push_back({end.displayTime, 2000000}); }
            check(XR_SUCCEEDED(layer_end_frame_impl(state->handle, &end)), "actual layer EndFrame");
            if(luke) {
                const auto sequence = state->next_source_sequence;
                const auto anchor = state->previous_projection->display_time;
                check(state->luke_clock_active && !state->luke_duplicate_cycle, "GPU accepted source clock");
                check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle,nullptr,&luke_prediction)), "GPU duplicate Wait");
                check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle,nullptr)), "GPU duplicate Begin");
                for (const auto& c : {chain,right_chain}) {
                    const auto serial = c->last_released_capture->serial;
                    c->acquired_indices.push_back(0); c->front_waited=true;
                    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    check(XR_SUCCEEDED(release_swapchain_with_identity(c,&release,
                        xrfg::LukeRenderIdentity{0x12340000,n})),"duplicate Release");
                    check(c->luke_released_duplicate && c->last_released_capture->serial==serial,
                          "duplicate changed worker private source");
                }
                end.displayTime = luke_prediction.predictedDisplayTime;
                check(XR_SUCCEEDED(layer_end_frame_impl(state->handle,&end)),"duplicate End");
                check(state->next_source_sequence==sequence && state->previous_projection->display_time==anchor,
                    "duplicate reached director or changed anchor");
                check(state->pending_frames.empty(),"duplicate virtual frame not consumed");
                check(state->luke_duplicate_cycle, "duplicate did not arm bounded cycle wait");
            }
            if (transition && n == 5) {
                check(!continuous_presenter_active(state), "unsupported extension not demoted");
                check(!state->previous_projection, "bypass retained stale metadata anchor");
                check(state->generation_resume_wall_time == std::chrono::steady_clock::time_point{}, "bypass quarantined");
            } else check(continuous_presenter_active(state), "EndFrame demoted owned composition");
        }
        check(XR_SUCCEEDED(wait_for_presenter_idle(state)), "GPU pipeline drain");
        stop_continuous_presenter(state);
        if(diagnostic) {
            state->frame_capture->shutdown();
            unsigned samples=0,views=0,handoffs=0,submissions=0,consumers=0;
            for(const auto& directory:std::filesystem::directory_iterator(diagnostic_root/"captures")) {
                std::ifstream trace(directory.path()/"trace.jsonl");std::string line;
                while(std::getline(trace,line)) {
                    if(line.find("\"kind\":\"sample\"")!=std::string::npos)++samples;
                    if(line.find("\"kind\":\"view\"")!=std::string::npos){++views;check(line.find("\"capture\":0")==std::string::npos,"view lost capture correlation");}
                    if(line.find("\"kind\":\"handoff\"")!=std::string::npos)++handoffs;
                    if(line.find("\"kind\":\"present\"")!=std::string::npos)++submissions;
                    if(line.find("\"kind\":\"consume\"")!=std::string::npos)++consumers;
                }
            }
            check(samples==16&&views==16&&handoffs==8&&submissions==15&&consumers==16,"incomplete correlated trace");
            xrfg::bridge_flight_logger().shutdown();
        }
        std::vector<std::pair<unsigned, bool>> expected;
        for (unsigned i = 1; i <= 8; ++i) {
            if (i > 1 && !(transition && (i == 5 || i == 6)) && !(gap && (i==4 || i==6))) expected.emplace_back(i, true);
            expected.emplace_back(i, false);
        }
        check(presented == expected && waits == expected.size() && begins == waits && ends == waits, "GPU order/count");
        check(presented.back() == std::pair<unsigned,bool>{8,false}, "GPU final real");
        check(gpu_pixels_correct, "real image stale or eyes crossed");
        check(auxiliary_pixels_correct, "auxiliary ownership, pixels or metadata changed");
        for (auto& aux : auxiliary) if (aux) destroy_composition_capture(aux);
        check(auxiliary_creates == auxiliary_destroys, "auxiliary swapchain leak");
        if (with_layers) check(auxiliary_creates == 4, "output pool not bounded to two handles per source");
        check(SUCCEEDED(generation->synthesizer->wait_for_idle()), "GPU shutdown");
        if (with_layers) check(SUCCEEDED(right_generation->synthesizer->wait_for_idle()), "right GPU shutdown");
        { std::scoped_lock lock(g_state_mutex); g_swapchains.clear(); g_sessions.clear(); }
        gpu_outputs.clear(); readback.Reset(); auxiliary_readback.Reset(); readback_context.Reset();
        std::cout << "Actual EndFrame + NVIDIA/D3D11 worker/director: ordered unique real/synthetic; stereo pixels verified; "
                  << (with_layers ? "3 layers / 2 split-eye mappings, static/dynamic source poisoned before EndFrame, metadata and 4-handle pool verified" : "projection only")
                  << (transition ? "; unsupported-layer bypass + re-prime without serial-gap failure" : "") << '\n';
    } catch (...) {
        stop_continuous_presenter(state);
        for (const auto& c : find_swapchains(state)) if (c->composition_capture) destroy_composition_capture(c);
        { std::scoped_lock lock(g_state_mutex); g_swapchains.clear(); g_sessions.clear(); }
        gpu_outputs.clear(); readback.Reset(); auxiliary_readback.Reset(); readback_context.Reset();
        throw;
    }
}
}

void embedded_eligibility_test() {
    check(xrfg::embedded::snapshot().desired.enabled, "control defaults");
    for (auto binding : {SessionGraphicsBinding::d3d11, SessionGraphicsBinding::d3d12}) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            auto s = std::make_shared<SessionState>(std::make_shared<Dispatch>());
            s->handle = reinterpret_cast<XrSession>(0x213);
            s->graphics_binding = binding;
            s->control_revision = 0;
            s->generation_budget_exhausted = mode == 2;
            const XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            auto chain = std::make_shared<SwapchainState>(s, info);
            chain->handle = reinterpret_cast<XrSwapchain>(0x213);
            chain->session = s;
            chain->generation_eligible_pending = mode != 0;
            chain->generation_declined = mode == 1;
            { std::scoped_lock lock(g_state_mutex); g_swapchains[chain->handle] = chain; }
            apply_embedded_control(s);
            // Eligible attempt fails without a graphics binding: refusal must
            // latch. Ineligible/budget-exhausted chains must remain untouched.
            check(chain->generation_declined == (mode == 1 || mode == 3),
                "control eligibility/refusal gate");
            check(!chain->frame_generation, "unexpected control allocation");
            { std::scoped_lock lock(g_state_mutex); g_swapchains.erase(chain->handle); }
        }
    }
}

void luke_admission_test() {
    auto s=session();
    // No live Luke DLL or injected hook is required: the production Release
    // transaction receives the same typed identity its real reader supplies.
    s->dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; };
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::array<std::shared_ptr<SwapchainState>,2> chains;
    std::array<ProjectionResourceMapping,2> mappings;
    const xrfg::LukeRenderIdentity id{0x12340000,10};
    for(unsigned i=0;i<2;++i) {
        auto c=std::make_shared<SwapchainState>(s,info);
        c->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t{0x990}+i);
        c->frame_generation=std::make_shared<FrameGenerationSwapchainState>();
        c->last_released_capture=xrfg::D3D12HistoryCaptureTicket{};
        c->last_released_capture->serial=7;
        c->luke_released_identity=id;
        chains[i]=c; mappings[i].application_swapchain=c->handle;
        std::scoped_lock lock(g_state_mutex);g_swapchains[c->handle]=c;
    }
    using A=LukePairDecision::Action;
    check(luke_pair_decision(mappings).action==A::accept,"new identity rejected");
    commit_luke_pair(mappings,id);
    s->synthesis_busy=true; // Would block forever if duplicate entered worker gate.
    for (const auto& c:chains) {
        c->acquired_indices.push_back(2); c->front_waited=true;
        const auto start=std::chrono::steady_clock::now();
        check(XR_SUCCEEDED(release_swapchain_with_identity(c,nullptr,id)),"fast duplicate release failed");
        check(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(50),"duplicate waited");
        check(c->acquired_indices.empty() && !c->front_waited && c->last_released_index==2,
              "duplicate ownership bookkeeping");
        check(c->last_released_capture->serial==7,"private source overwritten");
    }
    auto d=luke_pair_decision(mappings);
    check(d.action==A::drop && !d.reset,"duplicate reset valid history");
    s->presenter_failure=XR_ERROR_RUNTIME_FAILURE;
    check(release_swapchain_with_identity(chains[0],nullptr,id)==XR_ERROR_RUNTIME_FAILURE,
          "duplicate hid director failure");
    s->presenter_failure=XR_SUCCESS;
    chains[0]->acquired_indices.push_back(1);chains[0]->front_waited=true;
    s->dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*) { return XR_ERROR_RUNTIME_FAILURE; };
    check(release_swapchain_with_identity(chains[0],nullptr,id)==XR_ERROR_RUNTIME_FAILURE &&
        chains[0]->acquired_indices.size()==1 && chains[0]->front_waited,"failed release lost ownership");
    chains[0]->acquired_indices.clear();chains[0]->front_waited=false;
    chains[1]->luke_released_identity=xrfg::LukeRenderIdentity{id.object,11};
    chains[1]->luke_released_duplicate=false;
    check(luke_pair_decision(mappings).action==A::drop && luke_pair_decision(mappings).reset,
          "mixed pair admitted");
    for(auto& c:chains) {c->luke_released_identity=xrfg::LukeRenderIdentity{id.object,30};c->luke_released_duplicate=false;}
    check(luke_pair_decision(mappings).action==A::accept,"jump incorrectly filled or rejected");
    for(auto& c:chains)c->luke_released_identity=xrfg::LukeRenderIdentity{id.object,1};
    check(luke_pair_decision(mappings).action==A::accept && luke_pair_decision(mappings).reset,"reset not reprime");
    for(auto& c:chains) {c->luke_released_identity.reset();c->luke_admitted_identity.reset();}
    check(luke_pair_decision(mappings).action==A::normal,"non-Luke route changed");
    s->synthesis_busy=false;s->presenter_active=false;
    { std::scoped_lock lock(g_state_mutex);for(auto& c:chains)g_swapchains.erase(c->handle); }
}

void luke_cycle_clock_test() {
    using Clock = std::chrono::steady_clock;
    auto s = session();
    { std::scoped_lock lock(g_state_mutex); g_sessions[s->handle] = s; }
    s->synthesis_busy = true; // No worker/director thread: neither can make progress.
    const auto cycle = [&] {
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle, nullptr, &prediction)), "Luke clock Wait");
        check(XR_SUCCEEDED(layer_begin_frame_impl(s->handle, nullptr)), "Luke clock Begin");
        consume_application_frame(s, prediction.predictedDisplayTime, false, true);
        s->application_frame_in_progress = false;
        return prediction;
    };
    cycle();
    update_luke_application_clock(*s, true, false);
    check(s->luke_clock_active, "recognized route failed to enable clock");
    const auto origin = s->luke_clock_origin;
    const auto origin_wall = s->luke_clock_wall_origin;
    auto last = s->last_virtual_display_time;
    for (const XrDuration period : {8333333LL, 11111111LL, 13888889LL}) {
        s->presenter_frame_state.predictedDisplayPeriod = period;
        for (unsigned i = 0; i < 8; ++i) {
            update_luke_application_clock(*s, true, true);
            const auto deadline = s->application_wait_returned_at + std::chrono::nanoseconds(period);
            const auto prediction = cycle();
            check(Clock::now() >= deadline, "duplicate escaped native-period deadline");
            check(prediction.predictedDisplayTime - last >= period, "duplicate nanosecond clock survived");
            check(prediction.predictedDisplayPeriod == period, "Luke period doubled");
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin_wall).count();
            const auto clock_elapsed = prediction.predictedDisplayTime - origin;
            check(clock_elapsed <= elapsed && elapsed - clock_elapsed < 5'000'000,
                  "Luke prediction follows calls/director instead of elapsed time");
            last = prediction.predictedDisplayTime;
        }
    }
    check(waits == 0 && begins == 0 && ends == 0, "duplicate called physical runtime");
    // A fresh admission must not inherit the duplicate deadline, even with
    // synthesis held busy and a long headset period in the test fixture.
    s->presenter_frame_state.predictedDisplayPeriod = 50'000'000;
    update_luke_application_clock(*s, true, false);
    auto start = Clock::now(); cycle();
    check(Clock::now() - start < std::chrono::milliseconds(25), "fresh input paced on duplicate timer");
    // Already spent a period producing: no additional full-period sleep.
    update_luke_application_clock(*s, true, true);
    s->application_wait_returned_at -= std::chrono::milliseconds(100);
    start = Clock::now(); cycle();
    check(Clock::now() - start < std::chrono::milliseconds(25), "slow cycle got additive wait");
    // Error/stop cancellation releases the CV, rather than waiting for a tick.
    for (bool stop : {false, true}) {
        update_luke_application_clock(*s, true, true);
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            { std::scoped_lock lock(s->presenter_mutex);
              if (stop) s->presenter_stop_requested = true;
              else s->presenter_failure = XR_ERROR_RUNTIME_FAILURE; }
            s->presenter_condition.notify_all();
        });
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        const auto result = layer_wait_frame_impl(s->handle, nullptr, &prediction);
        cancel.join();
        check(result == (stop ? XR_ERROR_SESSION_NOT_RUNNING : XR_ERROR_RUNTIME_FAILURE),
              "Luke duplicate hid stop/error");
        s->presenter_failure = XR_SUCCESS; s->presenter_stop_requested = false;
    }
    update_luke_application_clock(*s, false, false);
    check(!s->luke_clock_active && !s->luke_duplicate_cycle, "unsupported route kept Luke clock");
    s->synthesis_busy = false; s->presenter_active = false;
    { std::scoped_lock lock(g_state_mutex); g_sessions.erase(s->handle); }
}

void native_stereo_time_match_test() {
    auto state = session();
    state->pending_frames.push_back({100, 11});
    check(latest_pending_application_period(state, 100, false, true) == 11,
          "exact Native frame time failed");
    check(latest_pending_application_period(state, 105, false, true) == 11,
          "Native Stereo Fix time rewrite rejected");
    consume_application_frame(state, 105, false, true);
    check(state->pending_frames.empty(), "rewritten Native frame was not consumed");

    state->pending_frames.push_back({100, 11});
    state->pending_frames.push_back({200, 12});
    check(latest_pending_application_period(state, 205, false, true) == 12,
          "Native Stereo Fix selected a stale pending frame");
    check(!latest_pending_application_period(state, 205, false, false),
          "ambiguous frame time was accepted without begin ownership");
    check(latest_pending_application_period(state, 205, true, false) == 11,
          "overlapping wait lost submission order");
    consume_application_frame(state, 205, false, true);
    check(state->pending_frames.empty(), "stale pending Native frames survived");
}

int main(int argc, char** argv) {
    auto state = session();
    try {
        native_stereo_time_match_test();
        check(three_slot_generation_session(state), "Nvidia (new) must select director");
        state->nvidia_new_route = false;
        check(!three_slot_generation_session(state), "Nvidia legacy must exclude director");
        state->nvidia_new_route = true;
        state->frame_generation_mode = xrfg::D3D12FrameGenerationMode::fsr3;
        check(!three_slot_generation_session(state), "FSR must remain inline");
        state->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
        state->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        check(!three_slot_generation_session(state), "FidelityFX must exclude NVIDIA director");
        state->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
        if (argc > 1) {
            const auto mode = std::string_view(argv[1]);
            if(mode == "luke-unit") { luke_admission_test(); return 0; }
            if(mode == "luke-clock") { luke_cycle_clock_test(); return 0; }
            if(mode == "luke-gpu") { gpu_pipeline(true,false,false,true); return 0; }
            if(mode == "gap") { gpu_pipeline(true,false,false,false,true); return 0; }
            if(mode == "luke-ram") {
                RamTimingFixture capture(true);gpu_pipeline(true,false,false,true);capture.verify(true);return 0;
            }
            if(mode == "ram") {
                RamTimingFixture capture(true);
                gpu_pipeline(false,false,false);capture.verify(true);return 0;
            }
            gpu_pipeline(mode == "composition" || mode == "transition" || mode == "capture", mode == "transition", mode == "capture"); return 0;
        }
        embedded_eligibility_test();
        // Apply both menu revisions to the real session implementation. The
        // GPU backend stays NVIDIA: only presenter ownership is switched.
        {
            auto selected = session();
            selected->presenter_active = false;
            const auto original = xrfg::embedded::snapshot().desired;
            auto choice = original;
            choice.backend = 1;
            choice.frame_generation = 0;
            check(xrfg::embedded::request(choice), "request Nvidia legacy");
            apply_embedded_control(selected);
            check(!three_slot_generation_session(selected), "legacy menu revision applied");
            choice.backend = 2;
            check(xrfg::embedded::request(choice), "request Nvidia new");
            apply_embedded_control(selected);
            check(three_slot_generation_session(selected), "new menu revision applied");
            choice.backend = 1;
            check(xrfg::embedded::request(choice), "request legacy again");
            apply_embedded_control(selected);
            check(!three_slot_generation_session(selected), "reverse menu revision applied");
            check(xrfg::embedded::request(original), "restore test configuration");
        }
        // Virtual WaitFrame must return even while synthesis is deliberately
        // marked busy and no director is running. No tick/fence gate survives.
        { std::scoped_lock lock(g_state_mutex); g_sessions[state->handle] = state; }
        state->synthesis_busy = true;
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        const auto start = std::chrono::steady_clock::now();
        check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "virtual wait");
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50), "producer paced");
        check(waits == 0, "producer entered runtime");
        check(prediction.predictedDisplayPeriod == 2000000, "director advertised doubled period");
        check(prediction.predictedDisplayTime == next_time + 4000000, "prediction anchor changed");
        // Exercise the real WaitFrame hook at several headset rates. Only
        // advertised period changes: the two-period anchor/ceiling survives.
        for (const XrDuration period : {8333333LL, 11111111LL, 13888889LL}) {
            state->presenter_frame_state.predictedDisplayPeriod = period;
            state->last_virtual_display_time = 0;
            state->application_wait_pending_begin = false;
            check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "native-period wait");
            check(prediction.predictedDisplayPeriod == period, "headset period not preserved");
            check(prediction.predictedDisplayTime == next_time + 2*period, "pair lookahead changed");
            state->application_wait_pending_begin = false;
            check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "second virtual wait");
            check(prediction.predictedDisplayPeriod == period, "second period changed");
            check(prediction.predictedDisplayTime == next_time + 4*period, "prediction step changed");
        }
        state->three_slot_director = false;
        state->last_virtual_display_time = 0;
        state->application_wait_pending_begin = false;
        check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "other presenter wait");
        check(prediction.predictedDisplayPeriod == 2*13888889LL, "non-director route changed");
        state->three_slot_director = true;
        state->menu_enabled = false;
        state->last_virtual_display_time = 0;
        state->application_wait_pending_begin = false;
        check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle, nullptr, &prediction)), "disabled wait");
        check(prediction.predictedDisplayPeriod == 13888889LL, "disabled period changed");
        check(prediction.predictedDisplayTime == next_time + 13888889LL, "disabled anchor changed");
        state->menu_enabled = true;
        state->presenter_frame_state.predictedDisplayPeriod = 2000000;
        state->last_virtual_display_time = 0;
        check(waits == 0, "metadata test entered runtime");
        state->synthesis_busy = false;
        state->application_wait_pending_begin = false;
        { std::scoped_lock lock(g_state_mutex); g_sessions.clear(); }

        // Real director, mock pixels. Verify downstream blocking keeps A's
        // slot owned and prevents an early synthetic from overwriting it.
        state->three_slots.push_real(frame(1));
        state->three_slots.shift_empty(); state->three_slots.shift_empty();
        state->presenter_thread = std::thread(three_slot_director_main, state);
        for (unsigned n = 2; n <= 25; ++n) {
            until(state, [&] { return state->three_slots.can_accept_real(); });
            {
                std::scoped_lock lock(state->presenter_mutex);
                blocked_end = (n % 3 == 0); entered_end = false;
                check(state->three_slots.push_real(frame(n)), "real admission");
            }
            state->presenter_condition.notify_all();
            if (blocked_end) {
                std::unique_lock lock(test_mutex);
                check(test_condition.wait_for(lock, std::chrono::seconds(3), [] { return entered_end.load(); }), "end enter");
                lock.unlock();
                { std::scoped_lock state_lock(state->presenter_mutex);
                  check(!state->three_slots.can_insert_synthetic(), "inflight overwrite"); }
                blocked_end = false; test_condition.notify_all();
            }
            until(state, [&] { return state->three_slots.can_insert_synthetic(); });
            // Model delayed completion without changing the director's code.
            if (n % 4 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            { std::scoped_lock lock(state->presenter_mutex);
              check(state->three_slots.push_synthetic(frame(n, true)), "synthetic insertion"); }
            state->presenter_condition.notify_all();
        }
        until(state, [&] { return state->three_slots.can_accept_real(); });
        check(XR_SUCCEEDED(wait_for_presenter_idle(state)), "drain");
        stop_continuous_presenter(state);
        check(presented.size() == 49, "lost or repeated frame");
        for (unsigned i = 0; i < 24; ++i) {
            check(presented[2*i] == std::pair<unsigned,bool>{i+1,false}, "real sequence");
            check(presented[2*i+1] == std::pair<unsigned,bool>{i+2,true}, "synthetic sequence");
        }
        check(presented.back() == std::pair<unsigned,bool>{25,false}, "last real drain");
        check(waits == begins && begins == ends && ends == 49, "frame cycle mismatch");

        auto failing = session();
        failing->three_slots.push_real(frame(1));
        failing->three_slots.shift_empty(); failing->three_slots.shift_empty();
        failing->three_slots.push_real(frame(2));
        fail_runtime = true;
        failing->presenter_thread = std::thread(three_slot_director_main, failing);
        until(failing, [&] { return XR_FAILED(failing->presenter_failure); });
        check(XR_FAILED(wait_for_synthesis_handoff(failing)), "runtime failure not propagated");
        stop_continuous_presenter(failing);
        fail_runtime = false;

        auto held = session();
        held->three_slots.push_real(frame(1));
        held->three_slots.shift_empty(); held->three_slots.shift_empty();
        const auto before_stop = ends.load();
        held->presenter_thread = std::thread(three_slot_director_main, held);
        stop_continuous_presenter(held);
        check(ends == before_stop, "stop repeated a retained frame");

        // The actual worker error path must wake both producer rendezvous and
        // director. A missing mapping cannot silently strand a busy worker.
        auto failed = session();
        auto job = std::make_shared<SynthesisJob>();
        job->current = frame(1); failed->synthesis_job = job; failed->synthesis_busy = true;
        failed->synthesis_thread = std::thread(synthesis_worker_main, failed);
        check(XR_FAILED(wait_for_synthesis_handoff(failed)), "worker error not propagated");
        stop_continuous_presenter(failed);
        std::cout << "Actual director: 49 legal ordered cycles, slow synthesis/runtime, drain, unpaced producer, worker failure\n";
        return 0;
    } catch (const std::exception& e) {
        blocked_end = false; test_condition.notify_all(); stop_continuous_presenter(state);
        std::cerr << e.what() << '\n'; return 1;
    }
}
