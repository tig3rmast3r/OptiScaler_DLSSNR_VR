// Exercises the production enumeration and menu transaction with real WARP
// allocations. Runtime depth images stay available; only the unused ring goes.
ID3D12Resource* depth_test_runtime_image{};
void depth_history_test() {
    auto s = session();
    s->presenter_active = false;
    s->graphics_binding = SessionGraphicsBinding::d3d12;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "depth factory");
    check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "depth WARP");
    check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&s->d3d12_device))), "depth device");
    D3D12_COMMAND_QUEUE_DESC queue{};
    check(SUCCEEDED(s->d3d12_device->CreateCommandQueue(&queue,
        IID_PPV_ARGS(&s->d3d12_queue))), "depth queue");
    const auto original = xrfg::embedded::snapshot().desired;
    s->dispatch->release_swapchain_image = [](XrSwapchain,
        const XrSwapchainImageReleaseInfo*) -> XrResult { return XR_SUCCESS; };
    const auto select = [&](int backend) {
        auto choice = original; choice.backend = backend; choice.frame_generation = 0;
        check(xrfg::embedded::request(choice), "depth route request");
        apply_embedded_control(s);
    };
    for (bool color : {false, true}) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = color ? XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                               : XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.width = info.height = 32; info.arraySize = info.faceCount = info.mipCount = info.sampleCount = 1;
        info.format = color ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_D32_FLOAT;
        auto chain = std::make_shared<SwapchainState>(s, info);
        chain->handle = reinterpret_cast<XrSwapchain>(0x319);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = 32; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = static_cast<DXGI_FORMAT>(info.format); desc.SampleDesc.Count = 1;
        desc.Flags = color ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        check(SUCCEEDED(s->d3d12_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, color ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE,
            nullptr, IID_PPV_ARGS(&texture))), "depth runtime image");
        depth_test_runtime_image = texture.Get();
        s->dispatch->enumerate_swapchain_images = [](XrSwapchain, uint32_t capacity,
            uint32_t* count, XrSwapchainImageBaseHeader* output) -> XrResult {
            *count = 1;
            if (capacity) reinterpret_cast<XrSwapchainImageD3D12KHR*>(output)->texture = depth_test_runtime_image;
            return XR_SUCCESS;
        };
        { std::scoped_lock lock(g_state_mutex); g_swapchains[chain->handle] = chain; }
        // No presenter/private generation is needed to check depth ownership.
        s->generation_budget_exhausted = true;
        const auto enumerate = [&] {
            XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR}; uint32_t count{};
            check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(chain->handle, 1, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(&image))), "depth enumeration");
            check(count == 1 && image.texture == texture.Get(), "runtime image replaced");
            check(chain->enumerated_d3d12_images[0].Get() == texture.Get(), "depth guide source lost");
        };
        const auto release_depth = [&](bool expect_capture) {
            if (color) return;
            chain->acquired_indices.push_back(0); chain->waited_count = 1;
            s->menu_enabled = true;
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            check(XR_SUCCEEDED(release_swapchain_with_identity(chain, &release, std::nullopt)), "depth release");
            check(chain->last_released_capture.has_value() == expect_capture, "depth copy policy mismatch");
            check(chain->last_released_index == 0 && chain->acquired_indices.empty() && chain->waited_count == 0,
                "depth release ownership lost");
        };
        select(1); enumerate();
        check(chain->d3d12_history && chain->d3d12_history->initialized(), "reference history missing");
        release_depth(true);
        auto reference = chain->d3d12_history;
        std::weak_ptr<xrfg::D3D12SwapchainHistory> retired = reference;
        reference.reset();
        select(2);
        check(bool(chain->d3d12_history) == color, "new route retained depth history or lost color");
        if (!color) check(retired.expired(), "depth ring not freed at menu switch");
        release_depth(false);
        enumerate(); enumerate();
        check(bool(chain->d3d12_history) == color, "enumeration allocated new depth ring");
        check(!chain->generation_eligible_pending && !chain->frame_generation, "depth/budget generation armed");
        select(1);
        check(chain->d3d12_history && chain->d3d12_history->initialized(), "reverse switch lost reference history");
        release_depth(true);
        select(0);
        check(chain->d3d12_history && chain->d3d12_history->initialized(), "FidelityFX reference changed");
        select(2);
        check(bool(chain->d3d12_history) == color, "repeat switch history mismatch");
        // Protected or ambiguous usage never acquires a color-history ring.
        chain->create_info.createFlags = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
        chain->d3d12_history.reset(); enumerate();
        check(!chain->d3d12_history, "protected history allocated");
        select(1); check(!chain->d3d12_history, "protected history allocated on switch");
        chain->create_info.createFlags = 0;
        chain->create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
            XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        enumerate(); check(!chain->d3d12_history, "ambiguous history allocated");
        { std::scoped_lock lock(g_state_mutex); g_swapchains.erase(chain->handle); }
    }
    check(xrfg::embedded::request(original), "restore depth control");
    std::cout << "D24 WARP: depth rings freed/recreated on route switches; color, runtime depth, protected and ambiguous cases preserved\n";
}
