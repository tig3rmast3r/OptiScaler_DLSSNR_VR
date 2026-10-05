#pragma once
#include <cstdint>

namespace xrfg {
struct LukeIngressDeviceFacts {
    bool output_device{}, motion_device{}, command_device{}, queue_device{};
    bool depth_present{}, depth_device{};
    bool output_adapter{}, motion_adapter{}, depth_adapter{}, queue_list_identity{};
};
// Diagnostic expansion of the existing admission predicate, not a device
// selector. Missing optional depth is allowed; queue/list identity stays strict.
[[nodiscard]] constexpr std::uint32_t luke_ingress_device_failures(
    const LukeIngressDeviceFacts& f) noexcept {
    std::uint32_t bits{};
    if (!f.output_device) bits |= 1u << 0;
    if (!f.motion_device) bits |= 1u << 1;
    if (!f.command_device) bits |= 1u << 2;
    if (!f.queue_device) bits |= 1u << 3;
    if (f.depth_present && !f.depth_device) bits |= 1u << 4;
    if (!f.output_adapter) bits |= 1u << 5;
    if (!f.motion_adapter) bits |= 1u << 6;
    if (f.depth_present && !f.depth_adapter) bits |= 1u << 7;
    if (!f.queue_list_identity) bits |= 1u << 8;
    return bits;
}
} // namespace xrfg
