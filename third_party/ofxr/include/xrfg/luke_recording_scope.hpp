#pragma once
#include <d3d12.h>
#include <utility>
namespace xrfg {
// An aborted recording is closed and discarded, never submitted. Restoring
// tracked states belongs to the caller since no recorded barriers executed.
template<class Abort>
class LukeRecordingScope {
    ID3D12GraphicsCommandList* list_;
    Abort abort_;
    bool active_{true};
public:
    LukeRecordingScope(ID3D12GraphicsCommandList* list, Abort abort) noexcept
        : list_(list), abort_(std::move(abort)) {}
    LukeRecordingScope(const LukeRecordingScope&) = delete;
    ~LukeRecordingScope() noexcept { if (active_) abort_(list_->Close()); }
    HRESULT close() noexcept { active_ = false; return list_->Close(); }
};
} // namespace xrfg
