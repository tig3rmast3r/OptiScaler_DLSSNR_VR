#include "pch.h"
#include "NativeSwapchainOverlay.h"
#include <detours/detours.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace NativeSwapchainOverlay
{
namespace
{
using Microsoft::WRL::ComPtr;
using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                          const UINT*, IUnknown* const*);
using Release = ULONG (STDMETHODCALLTYPE*)(IUnknown*);
struct Binding
{
    // Keep the queue alive during callback/submission, never the swapchain or
    // its back buffers. Native Release and Resize must retain their ownership.
    ComPtr<ID3D12CommandQueue> queue;
    HWND window = nullptr;
    Draw draw = nullptr;
    Reset reset = nullptr;
};
std::mutex bindingsMutex, hookMutex;
std::unordered_map<IDXGISwapChain*, std::shared_ptr<Binding>> bindings;
Present originalPresent = nullptr;
Present1 originalPresent1 = nullptr;
Resize originalResize = nullptr;
Resize1 originalResize1 = nullptr;
Release originalRelease = nullptr;
bool hooked = false;
thread_local unsigned callbackDepth = 0;

std::shared_ptr<Binding> Find(IDXGISwapChain* swapchain)
{
    std::lock_guard lock(bindingsMutex);
    auto found = bindings.find(swapchain);
    return found == bindings.end() ? nullptr : found->second;
}
void DrawFrame(IDXGISwapChain* swapchain, UINT interval, UINT flags,
               const DXGI_PRESENT_PARAMETERS* parameters)
{
    if ((flags & DXGI_PRESENT_TEST) != 0 || callbackDepth != 0) return;
    auto binding = Find(swapchain);
    if (!binding) return;
    struct Scope { Scope() { ++callbackDepth; } ~Scope() { --callbackDepth; } } scope;
    binding->draw(swapchain, interval, flags, parameters, binding->queue.Get(), binding->window, false);
}
HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* swapchain, UINT interval, UINT flags)
{
    DrawFrame(swapchain, interval, flags, nullptr);
    return originalPresent(swapchain, interval, flags);
}
HRESULT STDMETHODCALLTYPE OnPresent1(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* parameters)
{
    // The native Present1 implementation may call Present internally. One menu
    // submission belongs to the outer call, including that native tail call.
    DrawFrame(swapchain, interval, flags, parameters);
    struct Scope { Scope() { ++callbackDepth; } ~Scope() { --callbackDepth; } } scope;
    return originalPresent1(swapchain, interval, flags, parameters);
}
HRESULT STDMETHODCALLTYPE OnResize(IDXGISwapChain* swapchain, UINT count, UINT width, UINT height,
                                    DXGI_FORMAT format, UINT flags)
{
    auto binding = Find(swapchain);
    if (binding) binding->reset(true, binding->window);
    return originalResize(swapchain, count, width, height, format, flags);
}
HRESULT STDMETHODCALLTYPE OnResize1(IDXGISwapChain3* swapchain, UINT count, UINT width, UINT height,
                                     DXGI_FORMAT format, UINT flags, const UINT* masks,
                                     IUnknown* const* queues)
{
    auto binding = Find(swapchain);
    if (binding) binding->reset(true, binding->window);
    auto result = originalResize1(swapchain, count, width, height, format, flags, masks, queues);
    // ResizeBuffers1 can change presentation queues. Never render using a queue
    // from the previous buffer generation. A uniform new queue is required.
    if (binding && SUCCEEDED(result) && queues && count)
    {
        ComPtr<ID3D12CommandQueue> queue;
        bool uniform = queues[0] && SUCCEEDED(queues[0]->QueryInterface(IID_PPV_ARGS(&queue)));
        for (UINT index = 1; uniform && index < count; ++index)
            uniform = queues[index] == queues[0];
        std::lock_guard lock(bindingsMutex);
        auto found = bindings.find(swapchain);
        if (found != bindings.end() && found->second == binding)
        {
            if (uniform)
            {
                auto updated = std::make_shared<Binding>(*binding);
                updated->queue = queue;
                found->second = std::move(updated);
            }
            else bindings.erase(found);
        }
    }
    return result;
}
ULONG STDMETHODCALLTYPE OnRelease(IUnknown* object)
{
    // The real native Release returns its real count. A lookup after destruction
    // uses the pointer as a key only, never dereferences the destroyed object.
    auto result = originalRelease(object);
    if (result == 0)
    {
        std::shared_ptr<Binding> retired;
        {
            std::lock_guard lock(bindingsMutex);
            auto found = bindings.find(reinterpret_cast<IDXGISwapChain*>(object));
            if (found != bindings.end()) { retired = std::move(found->second); bindings.erase(found); }
        }
        if (retired) retired->reset(true, retired->window);
    }
    return result;
}
HRESULT Install(IDXGISwapChain3* swapchain)
{
    std::lock_guard lock(hookMutex);
    auto table = *reinterpret_cast<void***>(swapchain);
    void* entries[] {table[8], table[22], table[13], table[39], table[2]};
    if (hooked)
    {
        // Read the function addresses through an instance's vtable: other
        // overlays are free to replace that table. Do not rewrite their table.
        return S_OK;
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&Attach), &pinned))
        return HRESULT_FROM_WIN32(GetLastError());
    originalPresent = reinterpret_cast<Present>(entries[0]);
    originalPresent1 = reinterpret_cast<Present1>(entries[1]);
    originalResize = reinterpret_cast<Resize>(entries[2]);
    originalResize1 = reinterpret_cast<Resize1>(entries[3]);
    originalRelease = reinterpret_cast<Release>(entries[4]);
    auto result = DetourTransactionBegin();
    if (result != NO_ERROR) return HRESULT_FROM_WIN32(result);
    result = DetourUpdateThread(GetCurrentThread());
    if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&originalPresent), OnPresent);
    if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&originalPresent1), OnPresent1);
    if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&originalResize), OnResize);
    if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&originalResize1), OnResize1);
    if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&originalRelease), OnRelease);
    if (result != NO_ERROR) { DetourTransactionAbort(); return HRESULT_FROM_WIN32(result); }
    result = DetourTransactionCommit();
    if (result != NO_ERROR) return HRESULT_FROM_WIN32(result);
    hooked = true;
    return S_OK;
}
}
HRESULT Attach(IDXGISwapChain* swapchain, IUnknown* device, HWND window, Draw draw, Reset reset)
{
    if (!swapchain || !device || !window || !draw || !reset) return E_INVALIDARG;
    auto binding = std::make_shared<Binding>();
    auto result = device->QueryInterface(IID_PPV_ARGS(&binding->queue));
    if (FAILED(result)) return result;
    if (binding->queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    ComPtr<IDXGISwapChain3> extended;
    result = swapchain->QueryInterface(IID_PPV_ARGS(&extended));
    if (FAILED(result)) return result;
    result = Install(extended.Get());
    if (FAILED(result)) return result;
    binding->window = window;
    binding->draw = draw;
    binding->reset = reset;
    std::lock_guard lock(bindingsMutex);
    bindings[swapchain] = std::move(binding);
    return S_OK;
}
}
