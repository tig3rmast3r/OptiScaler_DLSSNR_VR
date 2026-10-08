#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DlssNr
{
inline bool NrFenceRetired(std::uint64_t completed, std::uint64_t required)
{
    return completed != ~std::uint64_t { 0 } && completed >= required;
}

struct NrSubmissionReceipt
{
    unsigned int handle = 0;
    std::uint64_t recording = 0;
    void* list = nullptr;
};

// Caller holds the flight mutex. A list pointer is not a recording identity:
// D3D12 permits Reset with a different allocator while an earlier execution is
// outstanding. Detach receipts BEFORE Execute, never look them up afterwards.
class NrSubmissionLedger
{
    std::uint64_t next_ = 0;
    std::unordered_map<unsigned int, NrSubmissionReceipt> active_;
    std::unordered_map<void*, std::vector<NrSubmissionReceipt>> recorded_;

public:
    bool HasRecordings() const { return !recorded_.empty(); }

    NrSubmissionReceipt Record(unsigned int handle, void* list)
    {
        NrSubmissionReceipt receipt { handle, ++next_, list };
        active_[handle] = receipt;
        recorded_[list].push_back(receipt);
        return receipt;
    }

    std::vector<NrSubmissionReceipt> Detach(void* list)
    {
        const auto found = recorded_.find(list);
        if (found == recorded_.end())
            return {};
        auto receipts = std::move(found->second);
        recorded_.erase(found);
        return receipts;
    }

    bool Complete(const NrSubmissionReceipt& receipt)
    {
        const auto found = active_.find(receipt.handle);
        if (found == active_.end() || found->second.recording != receipt.recording ||
            found->second.list != receipt.list)
            return false;
        active_.erase(found);
        return true;
    }

    void Forget(unsigned int handle)
    {
        active_.erase(handle);
        // Detached receipts must remain harmless even if the handle is recreated.
        // Recorded entries are discarded lazily by Complete's serial check.
    }

    void Clear()
    {
        active_.clear();
        recorded_.clear();
        // Do not recycle serials across a reset: detached callbacks can still exist.
    }
};
} // namespace DlssNr
