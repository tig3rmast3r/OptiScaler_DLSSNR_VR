#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <mutex>

namespace xrfg {
// CPU references protect preparation; receipts protect actual GPU reads.
// An unreceipted submission is never guessed safe by frame count.
class DlssSnapshotRetention final {
    struct Completion { Microsoft::WRL::ComPtr<ID3D12Fence> fence; std::uint64_t value{}; };
    std::mutex mutex_;
    std::array<Completion, 4> completions_{};
    unsigned pending_{};
    static bool complete(const Completion& c) noexcept {
        if (!c.fence) return true;
        const auto value = c.fence->GetCompletedValue();
        return value != UINT64_MAX && value >= c.value;
    }
public:
    void begin_gpu_read() noexcept { std::scoped_lock lock(mutex_); ++pending_; }
    HRESULT retire_gpu_read(ID3D12Fence* fence, std::uint64_t value) noexcept {
        std::scoped_lock lock(mutex_);
        if (!pending_ || !fence || !value) return E_INVALIDARG;
        for (auto& c : completions_) if (complete(c)) c = {};
        for (auto& c : completions_) if (c.fence.Get() == fence) {
            if (value > c.value) c.value = value;
            --pending_; return S_OK;
        }
        for (auto& c : completions_) if (!c.fence) {
            c.fence = fence; c.value = value; --pending_; return S_OK;
        }
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    bool reusable() noexcept {
        std::scoped_lock lock(mutex_);
        if (pending_) return false;
        for (const auto& c : completions_) if (!complete(c)) return false;
        return true;
    }
};
} // namespace xrfg
