#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <cmath>

namespace xrfg {
struct SceneIdentity {
    std::uint64_t owner{},serial{};
    unsigned output{};
    bool operator==(const SceneIdentity&) const = default;
    explicit operator bool() const noexcept {return owner && serial;}
};
// Records describe successful runtime submissions, not assumed display slots.
// An absent/ambiguous identity invalidates a measured window; never fill holes
// with the refresh rate, a submission sum or a guessed time-to-frame mapping.
class ScenePresentationRate {
    struct Receipt {std::uint32_t frame{};SceneIdentity scene{};bool valid{},ambiguous{};};
    std::array<Receipt,512> receipts_{};
public:
    void record(std::uint32_t frame,SceneIdentity scene) noexcept {
        auto& r=receipts_[frame%receipts_.size()];
        if(r.valid&&r.frame==frame&&r.scene!=scene) {r.ambiguous=true;return;}
        if(!r.valid||r.frame!=frame)r={frame,scene,true,false};
    }
    struct Rate {float output{},real{},generated{};};
    template<class Timing> std::optional<Rate> rate(std::span<const Timing> frames) const noexcept {
        auto end=frames.size();while(end&&!frames[end-1].m_nNumFramePresents)--end;
        if(end<2)return {};
        const double duration=frames[end-1].m_flSystemTimeInSeconds-frames[0].m_flSystemTimeInSeconds;
        if(!(duration>=.05))return {};
        std::array<SceneIdentity,128> shown{};unsigned count{},real{},generated{};
        for(std::size_t i=0;i<end;++i) {
            if(i&&(frames[i].m_nFrameIndex<=frames[i-1].m_nFrameIndex ||
                frames[i].m_flSystemTimeInSeconds<=frames[i-1].m_flSystemTimeInSeconds))return {};
            if(!frames[i].m_nNumFramePresents)continue;
            const auto& r=receipts_[frames[i].m_nFrameIndex%receipts_.size()];
            if(!r.valid||r.frame!=frames[i].m_nFrameIndex||r.ambiguous||!r.scene)return {};
            bool repeated=false;for(unsigned j=0;j<count;++j)repeated|=shown[j]==r.scene;
            if(repeated)continue;
            if(count==shown.size())return {};
            shown[count++]=r.scene;
            // First record establishes the boundary, as in the compositor rate.
            if(i) {if(r.scene.output)++generated;else ++real;}
        }
        return Rate{float((real+generated)/duration),float(real/duration),float(generated/duration)};
    }
};
}
