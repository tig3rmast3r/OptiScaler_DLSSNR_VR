#pragma once

#include <d3d12.h>

namespace xrfg {
// Keep deadline-bound final copies at least level with synthesis. A failed
// HIGH request retries the original descriptor on a NEW dedicated queue;
// independence from producer waits is never replaced by an application queue.
template<class Create>
HRESULT create_private_delivery_queue(D3D12_COMMAND_QUEUE_DESC original,
    bool high_supported,Create&& create) {
    if(high_supported && original.Priority<D3D12_COMMAND_QUEUE_PRIORITY_HIGH) {
        auto high=original;high.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        const auto result=create(high);if(SUCCEEDED(result))return result;
    }
    return create(original);
}
} // namespace xrfg
