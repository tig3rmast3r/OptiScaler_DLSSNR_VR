#pragma once
#include <cstdint>

namespace nr_forwarder {
// Calls required by the surviving NR implementation, in export-table order:
// create, evaluate, release, extras, native contract, float slot, float probe.
constexpr std::uint32_t required_exports = 0x7f;
constexpr bool complete(std::uint32_t present) noexcept {
    return (present & required_exports) == required_exports;
}
}
