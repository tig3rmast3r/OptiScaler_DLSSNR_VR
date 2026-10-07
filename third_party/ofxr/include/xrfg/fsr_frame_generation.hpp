#pragma once

#include <cstdint>

namespace xrfg {

enum class FsrFrameGenerationStatus : std::uint32_t {
    disabled,
    waiting_for_guides,
    runtime_missing,
    initialization_failed,
    dispatch_failed,
    used,
    stereo_unavailable,
    stereo_recovering,
};

struct FsrFrameGenerationStatistics {
    std::uint64_t attempts{};
    std::uint64_t used{};
    std::uint64_t failures{};
    FsrFrameGenerationStatus status{FsrFrameGenerationStatus::disabled};
};

void report_fsr_frame_generation_status(
    FsrFrameGenerationStatus status) noexcept;
void report_fsr_frame_generation_attempt(bool used) noexcept;
FsrFrameGenerationStatistics fsr_frame_generation_statistics() noexcept;

}  // namespace xrfg
