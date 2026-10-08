#include "pch.h"
#include "NativeSwapchainMenu.h"
#include "InUpscalerDx12Renderer.h"
#include "menu_common.h"
#include <Logger.h>
#include <Util.h>
#include <deque>

namespace NativeSwapchainMenu
{
namespace
{
using Microsoft::WRL::ComPtr;
struct Frame
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 value {};
};
struct Resources
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 value = 0;
    std::deque<Frame> frames;
    std::unique_ptr<InUpscalerDx12Renderer> renderer;
    ~Resources() { if (event) CloseHandle(event); }
    void Collect()
    {
        const auto completed = fence->GetCompletedValue();
        while (!frames.empty() && completed != UINT64_MAX && frames.front().value <= completed)
        {
            MenuGpuLifetime::ForgetRecording(frames.front().list.Get());
            frames.pop_front();
        }
        MenuGpuLifetime::Collect();
    }
    bool Drain()
    {
        if (FAILED(device->GetDeviceRemovedReason())) { frames.clear(); MenuGpuLifetime::Collect(); return true; }
        // Also order behind native Present: its work can follow our menu fence.
        auto next = ++value;
        if (FAILED(queue->Signal(fence.Get(), next))) return false;
        if (fence->GetCompletedValue() < next)
        {
            if (FAILED(fence->SetEventOnCompletion(next, event)) || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
                return false;
        }
        if (fence->GetCompletedValue() == UINT64_MAX) return FAILED(device->GetDeviceRemovedReason());
        // This fresh queue fence covers all submitted frames, including one
        // whose earlier Signal failed and therefore has no retirement value.
        for (auto& frame : frames) MenuGpuLifetime::ForgetRecording(frame.list.Get());
        frames.clear();
        MenuGpuLifetime::Collect();
        return true;
    }
};
std::mutex mutex;
std::shared_ptr<Resources> resources;
HWND ownerWindow = nullptr;
// A live-device signal failure cannot release submitted command allocators.
// This is retirement ownership, not another rendering route.
auto& Quarantined() { static auto* owners = new std::vector<std::shared_ptr<Resources>>; return *owners; }
void Retire()
{
    if (!resources) return;
    if (!resources->Drain())
    {
        LOG_ERROR("V33025 native menu queue drain failed; pending GPU ownership retained");
        resources->renderer.reset();
        Quarantined().push_back(resources);
    }
    resources.reset();
}
}
void Reset(bool, HWND window)
{
    std::lock_guard lock(mutex);
    if (ownerWindow == window) Retire();
}
void Draw(IDXGISwapChain* swapchain, UINT, UINT, const DXGI_PRESENT_PARAMETERS*,
          IUnknown* rawQueue, HWND window, bool)
{
    std::lock_guard lock(mutex);
    if (!Config::Instance()->OverlayMenu.value_or_default() || Config::Instance()->RenderMenuInUpscaler.value_or_default())
        return;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(rawQueue->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    if (resources && (resources->queue.Get() != queue.Get() || ownerWindow != window)) Retire();
    if (!resources)
    {
        auto next = std::make_shared<Resources>(); next->queue = queue;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&next->device))) ||
            FAILED(MenuCommandState::InstallDevice(next->device.Get())) ||
            FAILED(next->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&next->fence)))) return;
        next->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!next->event) return;
        if (!MenuCommon::IsInited() || MenuCommon::Handle() != window) MenuCommon::Init(window, false);
        next->renderer = std::make_unique<InUpscalerDx12Renderer>(next->device.Get());
        if (!next->renderer->Init()) return;
        MenuCommon::Dx12Inited();
        ownerWindow = window; resources = std::move(next);
    }
    auto& state = *resources;
    state.Collect();
    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc))) return;
    MenuCommon::SetRenderExtent(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
    if (!MenuCommon::RenderMenu()) return;
    ImGui::Render();
    // No allocator is reset while submitted. Saturation skips only a menu draw,
    // without blocking native presentation or changing OFXR's pacing.
    if (state.frames.size() >= 8) return;
    auto extended = static_cast<IDXGISwapChain3*>(swapchain); // Attach verified the native interface.
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(extended->GetBuffer(extended->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) return;
    Frame frame;
    if (FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))) ||
        FAILED(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&frame.list)))) return;
    // The accepted immutable renderer owns descriptors, textures and draw data.
    // Its input is PRESENT here; upscaler callers retain the existing UAV default.
    if (!state.renderer->Render(ImGui::GetDrawData(), frame.list.Get(), buffer.Get(),
                                desc.BufferDesc.Format, D3D12_RESOURCE_STATE_PRESENT) ||
        FAILED(frame.list->Close())) return;
    ID3D12CommandList* lists[] {frame.list.Get()};
    state.queue->ExecuteCommandLists(1, lists);
    frame.value = ++state.value;
    if (FAILED(state.queue->Signal(state.fence.Get(), frame.value))) frame.value = UINT64_MAX;
    state.frames.push_back(std::move(frame));
}
}
