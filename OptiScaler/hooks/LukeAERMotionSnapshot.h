#pragma once
#include <nvsdk_ngx_vk.h>
#include <cstdint>

namespace luke_vulkan {
// Descriptor VALUE only, captured at actual native NGX ingress. Never retain
// the borrowed CPU descriptor pointer or reuse a prior evaluation's value.
struct AERMotionSnapshot {
    NVSDK_NGX_Resource_VK resource{};
    bool valid{};
    void reset() noexcept { resource={}; valid=false; }
    template<class Parameters> void capture(const Parameters& parameters) {
        reset();
        void* borrowed=nullptr;
        if(parameters.Get(NVSDK_NGX_Parameter_MotionVectors,&borrowed)!=NVSDK_NGX_Result_Success || !borrowed) return;
        const auto& input=*static_cast<const NVSDK_NGX_Resource_VK*>(borrowed);
        if(input.Type!=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
            !input.Resource.ImageViewInfo.Image || !input.Resource.ImageViewInfo.ImageView ||
            !input.Resource.ImageViewInfo.Width || !input.Resource.ImageViewInfo.Height) return;
        resource=input; valid=true;
    }
};
// One evaluation owns the frozen GPU image. A command handle alone is not
// sufficient: games reuse their command buffers on later evaluations.
struct FrozenMotionReceipt {
    VkCommandBuffer command{};
    std::uint64_t evaluation{};
    bool valid{};
    void reset() noexcept { command={}; evaluation=0; valid=false; }
    void publish(VkCommandBuffer c, std::uint64_t e) noexcept {
        command=c; evaluation=e; valid=c!=VK_NULL_HANDLE;
    }
    bool matches(VkCommandBuffer c, std::uint64_t e) const noexcept {
        return valid && command==c && evaluation==e;
    }
};
}
