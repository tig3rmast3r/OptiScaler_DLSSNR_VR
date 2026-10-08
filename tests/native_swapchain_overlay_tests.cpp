#include <menu/NativeSwapchainOverlay.h>
#include <menu/InUpscalerDx12Renderer.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
using Microsoft::WRL::ComPtr;
static unsigned checks = 0, draws = 0, resets = 0;
static IDXGISwapChain* expected = nullptr;
static IUnknown* expectedQueue = nullptr;
static HWND expectedWindow = nullptr;
static unsigned frameworkCalls = 0;
using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
static Present cachedNativePresent = nullptr;
static std::unique_ptr<InUpscalerDx12Renderer> renderer;
struct Commands { ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; };
static std::vector<Commands> submitted;
static void Check(bool okay, const char* message)
{
    ++checks;
    if (!okay) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
static void Drain(IUnknown* object)
{
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&queue))), "drain queue");
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))), "drain device");
    ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "drain fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr && SUCCEEDED(queue->Signal(fence.Get(), 1)), "drain signal");
    Check(SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "drain completion");
    CloseHandle(event);
    for (auto& frame : submitted) MenuGpuLifetime::ForgetRecording(frame.list.Get());
    submitted.clear(); MenuGpuLifetime::Collect();
}
static void Draw(IDXGISwapChain* swapchain, UINT, UINT, const DXGI_PRESENT_PARAMETERS*,
                 IUnknown* queue, HWND window, bool)
{
    Check(swapchain == expected, "menu receives exact native object, never a proxy");
    Check(queue == expectedQueue, "menu owns current native presentation queue");
    Check(window == expectedWindow, "menu owns correct window");
    ComPtr<ID3D12CommandQueue> nativeQueue;
    Check(SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&nativeQueue))), "draw queue");
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(nativeQueue->GetDevice(IID_PPV_ARGS(&device))), "draw device");
    ComPtr<ID3D12Resource> buffer;
    auto extended = static_cast<IDXGISwapChain3*>(swapchain);
    Check(SUCCEEDED(extended->GetBuffer(extended->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))), "native backbuffer");
    Commands frame;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))), "owned allocator");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr, IID_PPV_ARGS(&frame.list))), "owned recording");
    auto& io = ImGui::GetIO(); io.DisplaySize = {320,240}; io.DeltaTime = 1.f/60;
    ImGui::NewFrame(); ImGui::SetNextWindowPos({10,10}); ImGui::SetNextWindowSize({260,180});
    ImGui::Begin("native menu", nullptr, ImGuiWindowFlags_NoSavedSettings); ImGui::TextUnformatted("native cached Present, immutable GPU menu");
    ImGui::End(); ImGui::Render();
    Check(renderer->Render(ImGui::GetDrawData(), frame.list.Get(), buffer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D12_RESOURCE_STATE_PRESENT), "immutable renderer accepts native PRESENT input");
    Check(SUCCEEDED(frame.list->Close()), "owned recording closes");
    ID3D12CommandList* lists[] {frame.list.Get()}; nativeQueue->ExecuteCommandLists(1, lists);
    submitted.push_back(std::move(frame));
    ++draws;
}
static void Reset(bool clearQueue, HWND window)
{
    Check(clearQueue && window == expectedWindow, "resize/release resets queue and GPU resource generation");
    Drain(expectedQueue);
    ++resets;
}
static HRESULT STDMETHODCALLTYPE FrameworkPresent(IDXGISwapChain* swapchain, UINT interval, UINT flags)
{
    ++frameworkCalls;
    // Model REFramework's per-instance hook and Steam's cached native entry.
    return cachedNativePresent(swapchain, interval, flags);
}
int main()
{
    ComPtr<ID3D12Debug> debug;
    Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))), "debug layer");
    debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    Check(SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory))), "DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP");
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "device");
    ComPtr<ID3D12InfoQueue> info;
    Check(SUCCEEDED(device.As(&info)), "validation messages");
    Check(SUCCEEDED(MenuCommandState::InstallDevice(device.Get())), "immutable renderer actual-device coverage");
    ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr;
    renderer = std::make_unique<InUpscalerDx12Renderer>(device.Get());
    Check(renderer->Init(), "immutable native menu renderer");
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    ComPtr<ID3D12CommandQueue> queue, secondQueue;
    Check(SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))), "queue");
    Check(SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&secondQueue))), "second queue");
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NativeSwapchainMenuTest";
    Check(RegisterClassW(&wc) != 0, "window class");
    HWND window = CreateWindowW(wc.lpszClassName, L"Native swapchain fixture", WS_OVERLAPPEDWINDOW,
                                 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    Check(window != nullptr, "window");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = 320; desc.Height = 240; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    Check(SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain)), "native chain");
    ComPtr<IDXGISwapChain3> extended;
    Check(SUCCEEDED(chain.As(&extended)), "native chain3");
    expected = chain.Get(); expectedQueue = queue.Get(); expectedWindow = window;
    auto nativeTable = *reinterpret_cast<void***>(chain.Get());
    cachedNativePresent = reinterpret_cast<Present>(nativeTable[8]);
    Check(SUCCEEDED(NativeSwapchainOverlay::Attach(chain.Get(), queue.Get(), window, Draw, Reset)), "attach");
    ComPtr<IUnknown> identity;
    Check(SUCCEEDED(chain.As(&identity)) && identity.Get() == chain.Get(), "COM identity unchanged after attach");
    identity.Reset();
    Check(SUCCEEDED(chain->Present(0, DXGI_PRESENT_TEST)) && draws == 0, "test Present cannot draw");
    Check(SUCCEEDED(cachedNativePresent(chain.Get(), 0, 0)) && draws == 1, "cached native Present survives attachment");
    std::array<void*, 41> frameworkTable {};
    for (unsigned i = 0; i < frameworkTable.size(); ++i) frameworkTable[i] = nativeTable[i];
    frameworkTable[8] = reinterpret_cast<void*>(&FrameworkPresent);
    *reinterpret_cast<void***>(chain.Get()) = frameworkTable.data();
    Check(SUCCEEDED(chain->Present(0, 0)) && draws == 2 && frameworkCalls == 1, "framework instance hook chain");
    *reinterpret_cast<void***>(chain.Get()) = nativeTable;
    DXGI_PRESENT_PARAMETERS parameters {};
    Check(SUCCEEDED(chain->Present1(0, 0, &parameters)) && draws == 3, "Present1 draws once");
    auto beforeReset = resets;
    Check(SUCCEEDED(chain->ResizeBuffers(2, 400, 280, DXGI_FORMAT_UNKNOWN, 0)) && resets > beforeReset, "native resize");
    Check(SUCCEEDED(chain->Present(0, 0)) && draws == 4, "present after resize");
    UINT masks[2] {1, 1}; IUnknown* queues[2] {secondQueue.Get(), secondQueue.Get()};
    Check(SUCCEEDED(extended->ResizeBuffers1(2, 400, 280, DXGI_FORMAT_UNKNOWN, 0, masks, queues)), "resize changes queue");
    expectedQueue = secondQueue.Get();
    Check(SUCCEEDED(chain->Present(0, 0)) && draws == 5, "new queue generation");
    extended.Reset();
    Drain(secondQueue.Get());
    beforeReset = resets;
    chain.Reset();
    Check(resets > beforeReset, "native final release removes binding");
    Check(SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain)), "recreate");
    Check(SUCCEEDED(chain->Present(0, 0)) && draws == 5, "unregistered recreated chain cannot inherit old binding");
    Drain(queue.Get());
    chain.Reset();
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0; info->GetMessage(i, nullptr, &size);
        auto storage = std::make_unique<unsigned char[]>(size);
        auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
        if (SUCCEEDED(info->GetMessage(i, message, &size)))
            Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION,
                  message->pDescription);
    }
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    renderer.reset(); ImGui::DestroyContext();
    std::printf("PASS native swapchain overlay: %u checks, cached native/instance Present, resize, queue change, release; no GPU validation errors\n", checks);
}
