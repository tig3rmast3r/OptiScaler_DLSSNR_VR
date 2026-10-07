#ifdef OFXR_TEST_DJULES_ROUTE
void djules_binding_queue_test() {
    auto s = session();
    s->presenter_active = false;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
    check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(s->d3d12_device.ReleaseAndGetAddressOf()))), "binding queue test device");
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(s->d3d12_device->CreateCommandQueue(&desc,
        IID_PPV_ARGS(s->d3d12_queue.ReleaseAndGetAddressOf()))) &&
        SUCCEEDED(s->d3d12_device->CreateCommandQueue(&desc,
            IID_PPV_ARGS(s->binding_queue.ReleaseAndGetAddressOf()))), "separate binding/application queues");
    check(SUCCEEDED(s->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(s->app_release_fence.ReleaseAndGetAddressOf()))) &&
        SUCCEEDED(s->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(s->reacquire_fence.ReleaseAndGetAddressOf()))), "binding join fences");
    Microsoft::WRL::ComPtr<ID3D12Fence> blocker, completed;
    check(SUCCEEDED(s->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker))) &&
        SUCCEEDED(s->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed))), "fixture fences");
    mark_application_release(s.get());
    check(s->app_release_counter == 1, "first release cursor");
    check(SUCCEEDED(s->d3d12_queue->Wait(blocker.Get(), 1)), "hold future application work");
    mark_application_release(s.get());
    check(s->app_release_counter == 2, "future release cursor");
    join_runtime_queue_to_application(s.get(), 1);
    check(runtime_queue(*s) == s->binding_queue.Get() &&
        SUCCEEDED(runtime_queue(*s)->Signal(completed.Get(), 1)), "binding handover marker");
    const HRESULT first = wait_composition_fence(completed.Get(), 1);
    blocker->Signal(1); // Always unblock future work, including on failure.
    check(SUCCEEDED(first), "held frame inherited unrelated future application work");
    carry_reacquire_to_application(s.get());
    check(s->reacquire_counter == 1 && SUCCEEDED(s->d3d12_queue->Signal(completed.Get(), 2)) &&
        SUCCEEDED(wait_composition_fence(completed.Get(), 2)), "reacquire guarantee did not reach application queue");
    drain_binding_queue(s.get());
    s->binding_queue.Reset();
    check(runtime_queue(*s) == s->d3d12_queue.Get(), "ordinary binding queue changed");
    std::cout << "Real WARP queues: immutable release cursor ignores later work; reacquire guarantee crosses back\n";
}
#endif
