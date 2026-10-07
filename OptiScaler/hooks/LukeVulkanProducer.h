#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <mutex>
#include <unordered_map>
#include <cstdint>

namespace luke_vulkan {
// Native device identity is known before Luke creates its XR session. Allocate
// the ONE XR owner pair on first request, not at the later DLSS feature init.
// Native capture shares its device, NOT its queue: capture can enqueue a wait
// for a Vulkan submission which the producer thread has not submitted yet.
inline HRESULT create_capture_queue(ID3D12Device* owner,
    ID3D12CommandQueue* xrQueue, ID3D12CommandQueue** captureQueue) {
    if (!captureQueue) return E_POINTER;
    *captureQueue = nullptr;
    if (!owner || !xrQueue) return E_POINTER;
    auto description = xrQueue->GetDesc();
    if (description.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Device> queueOwner;
    auto result = xrQueue->GetDevice(IID_PPV_ARGS(&queueOwner));
    Microsoft::WRL::ComPtr<IUnknown> ownerIdentity, queueIdentity;
    if (SUCCEEDED(result)) result = owner->QueryInterface(IID_PPV_ARGS(&ownerIdentity));
    if (SUCCEEDED(result)) result = queueOwner->QueryInterface(IID_PPV_ARGS(&queueIdentity));
    if (FAILED(result)) return result;
    if (ownerIdentity.Get() != queueIdentity.Get()) return E_INVALIDARG;
    description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    result = owner->CreateCommandQueue(&description, IID_PPV_ARGS(captureQueue));
    if (SUCCEEDED(result)) (*captureQueue)->SetName(L"Luke Vulkan Native Capture Queue");
    return result;
}

class Producer {
    struct Device { LUID luid{}; unsigned node{}; bool valid{}; };
    std::mutex mutex;
    std::unordered_map<std::uintptr_t, Device> devices;
    std::uintptr_t selected{};
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
public:
    void record(std::uintptr_t key, LUID luid, unsigned node, bool valid) {
        std::lock_guard lock(mutex);
        devices.insert_or_assign(key, Device{luid, node, valid});
    }
    void forget(std::uintptr_t key) {
        std::lock_guard lock(mutex);
        devices.erase(key);
        if (selected == key) { queue.Reset(); owner.Reset(); selected = 0; }
    }
    bool present() {
        std::lock_guard lock(mutex); return !devices.empty();
    }
    HRESULT get(std::uintptr_t key, ID3D12Device** outOwner, ID3D12CommandQueue** outQueue) {
        if (!outOwner || !outQueue) return E_POINTER;
        *outOwner = nullptr; *outQueue = nullptr;
        std::lock_guard lock(mutex);
        if (devices.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (devices.size() != 1) return E_INVALIDARG;
        if (!key) key = devices.begin()->first;
        const auto found = devices.find(key);
        if (found == devices.end() || !found->second.valid || found->second.node != 1)
            return E_INVALIDARG;
        if (!owner) {
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            auto result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (SUCCEEDED(result)) result = factory->EnumAdapterByLuid(found->second.luid, IID_PPV_ARGS(&adapter));
            Microsoft::WRL::ComPtr<ID3D12Device> candidate;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> candidateQueue;
            if (SUCCEEDED(result)) result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate));
            if (SUCCEEDED(result)) {
                const auto actual = candidate->GetAdapterLuid();
                if (actual.LowPart != found->second.luid.LowPart || actual.HighPart != found->second.luid.HighPart)
                    result = E_INVALIDARG;
            }
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (SUCCEEDED(result)) result = candidate->CreateCommandQueue(&description, IID_PPV_ARGS(&candidateQueue));
            if (FAILED(result)) return result;
            owner = std::move(candidate); queue = std::move(candidateQueue); selected = key;
        }
        if (selected != key) return E_INVALIDARG;
        owner.CopyTo(outOwner); queue.CopyTo(outQueue);
        return S_OK;
    }
};
}
