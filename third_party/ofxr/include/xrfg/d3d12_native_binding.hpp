#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <utility>

namespace xrfg::d3d_native {
inline constexpr GUID streamline_native_guid{
    0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
inline constexpr GUID reshade_unwrapped_guid{
    0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

// Streamline/ReShade QI contracts own one reference on successful unwrapping.
// Input must own its underlying objects throughout use of the borrowed result.
inline bool get_borrowed(IUnknown* input, IUnknown** output,
    HRESULT* query_result = nullptr) noexcept {
    if (query_result) *query_result = E_POINTER;
    if (!output) return false;
    *output = nullptr;
    if (!input) return false;
    IUnknown* visited[8]{};
    auto* current = input;
    bool unwrapped = false;
    for (unsigned depth = 0; depth < 8; ++depth) {
        visited[depth] = current;
        IUnknown* next{};
        auto result = current->QueryInterface(streamline_native_guid,
            reinterpret_cast<void**>(&next));
        if (result == E_NOINTERFACE) {
            next = nullptr;
            result = current->QueryInterface(reshade_unwrapped_guid,
                reinterpret_cast<void**>(&next));
        }
        if (query_result) *query_result = result;
        if (result == E_NOINTERFACE) {
            *output = current;
            if (query_result && unwrapped) *query_result = S_OK;
            return true;
        }
        if (FAILED(result) || !next) return false;
        next->Release();
        if (next == current) {
            *output = current;
            return true;
        }
        for (unsigned i = 0; i <= depth; ++i) {
            if (visited[i] == next) {
                if (query_result) *query_result = E_UNEXPECTED;
                return false;
            }
        }
        current = next;
        unwrapped = true;
    }
    if (query_result) *query_result = E_UNEXPECTED;
    return false;
}

// Keep the application's binding untouched downstream, retaining canonical
// objects for guide ownership and bridge GPU allocations/synchronization.
// Adapter equality alone never admits a device/queue pair.
inline HRESULT canonical_binding(ID3D12Device* input_device,
    ID3D12CommandQueue* input_queue,
    Microsoft::WRL::ComPtr<ID3D12Device>& device,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>& queue) noexcept {
    using Microsoft::WRL::ComPtr;
    device.Reset(); queue.Reset();
    IUnknown *native_device{}, *native_queue{};
    if (!get_borrowed(input_device, &native_device) ||
        !get_borrowed(input_queue, &native_queue)) return E_INVALIDARG;
    ComPtr<ID3D12Device> candidate_device;
    ComPtr<ID3D12CommandQueue> candidate_queue;
    auto result = native_device->QueryInterface(IID_PPV_ARGS(&candidate_device));
    if (FAILED(result)) return result;
    result = native_queue->QueryInterface(IID_PPV_ARGS(&candidate_queue));
    if (FAILED(result)) return result;
    ComPtr<ID3D12Device> reported_device;
    result = candidate_queue->GetDevice(IID_PPV_ARGS(&reported_device));
    if (FAILED(result)) return result;
    IUnknown* native_owner{};
    if (!get_borrowed(reported_device.Get(), &native_owner)) return E_INVALIDARG;
    ComPtr<IUnknown> device_identity, queue_owner_identity;
    result = candidate_device.As(&device_identity);
    if (FAILED(result)) return result;
    result = native_owner->QueryInterface(IID_PPV_ARGS(&queue_owner_identity));
    if (FAILED(result)) return result;
    if (device_identity.Get() != queue_owner_identity.Get()) return E_INVALIDARG;
    device = std::move(candidate_device); queue = std::move(candidate_queue);
    return S_OK;
}
} // namespace xrfg::d3d_native
