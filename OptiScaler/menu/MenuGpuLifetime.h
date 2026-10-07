#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>

// A recording owns immutable menu draws. Executions independently retain those
// draws until a fence placed AFTER the real ExecuteCommandLists retires. Reset
// may detach a recording while its earlier execution remains outstanding.
namespace MenuGpuLifetime
{
using Microsoft::WRL::ComPtr;
using Leases = std::vector<std::shared_ptr<void>>;
inline constexpr GUID RecordingGuid{0x65b242fc,0x4140,0x49f0,{0xb2,0x86,0xa2,0x19,0x73,0x07,0x81,0x20}};
inline std::atomic_bool queueHookReady{false}, resetHookReady{false};
inline bool Ready() { return queueHookReady.load() && resetHookReady.load(); }

class Recording final : public IUnknown
{
    std::atomic_ulong references_{1};
public:
    std::mutex mutex;
    Leases draws;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *result = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto count = --references_; if (!count) delete this; return count;
    }
};

inline ComPtr<Recording> GetRecording(ID3D12CommandList* list)
{
    ComPtr<Recording> owner;
    UINT bytes = sizeof(IUnknown*);
    IUnknown* object = nullptr;
    if (list && SUCCEEDED(list->GetPrivateData(RecordingGuid, &bytes, &object)) && object)
        owner.Attach(static_cast<Recording*>(object));
    return owner;
}
inline bool Retain(ID3D12GraphicsCommandList* list, std::shared_ptr<void> lease)
{
    auto owner = GetRecording(list);
    if (!owner)
    {
        owner.Attach(new Recording());
        if (FAILED(list->SetPrivateDataInterface(RecordingGuid, owner.Get()))) return false;
    }
    std::lock_guard lock(owner->mutex);
    owner->draws.push_back(std::move(lease));
    return true;
}
inline void ForgetRecording(ID3D12GraphicsCommandList* list)
{
    if (list) list->SetPrivateDataInterface(RecordingGuid, nullptr);
}
inline Leases BeforeSubmit(UINT count, ID3D12CommandList* const* lists)
{
    Leases leases;
    for (UINT i=0; lists && i<count; ++i)
    {
        auto owner = GetRecording(lists[i]);
        if (!owner) continue;
        std::lock_guard lock(owner->mutex);
        leases.insert(leases.end(), owner->draws.begin(), owner->draws.end());
    }
    return leases;
}
struct Submission
{
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Device> device;
    Leases draws;
};
struct Retirement
{
    std::mutex mutex;
    std::vector<Submission> pending;
};
inline Retirement& Ledger()
{
    // No static teardown can release live application GPU work during DLL unload.
    // Collection runs on every real queue submission, including when the menu hides.
    static auto* state = new Retirement();
    return *state;
}
inline void Collect()
{
    auto& state = Ledger();
    std::lock_guard lock(state.mutex);
    std::erase_if(state.pending, [](const Submission& s) {
        if (s.device && FAILED(s.device->GetDeviceRemovedReason())) return true;
        if (!s.fence) return false; // failed signal on a live device: keep ownership
        const auto completed = s.fence->GetCompletedValue();
        return completed != UINT64_MAX && completed >= 1;
    });
}
inline void AfterSubmit(ID3D12CommandQueue* queue, Leases leases)
{
    Collect();
    if (leases.empty()) return;
    Submission entry;
    entry.draws = std::move(leases);
    if (queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&entry.device))))
    {
        if (FAILED(entry.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&entry.fence))) ||
            FAILED(queue->Signal(entry.fence.Get(), 1))) entry.fence.Reset();
    } // Unknown queue/device or failed signal: quarantine instead of releasing GPU references.
    auto& state = Ledger();
    std::lock_guard lock(state.mutex);
    state.pending.push_back(std::move(entry));
}
inline size_t PendingCount()
{
    auto& state = Ledger(); std::lock_guard lock(state.mutex); return state.pending.size();
}
}
