#pragma once
#include "xrfg/bridge_flight_logger.hpp"
#include <memory>
#include <string>

namespace xrfg {
// Win64 shared-memory recorder. One bounded ring per emitting OS thread.
// The external tray freezes/copies it; the game never writes capture files.
class RamTimingCapture final {
public:
    RamTimingCapture();
    ~RamTimingCapture();
    bool initialize(bool pose_profile = false) noexcept;
    BridgeFlightToken begin(BridgeFlightOperation, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
    void end(BridgeFlightToken, BridgeFlightOperation, std::int64_t,
        std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
    void event(BridgeFlightOperation, std::int64_t,
        std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
inline constexpr std::uint64_t kRamTimingToken = 1ULL << 63;
// Tray/test only; no calls to either function on a frame thread.
bool save_ram_timing(unsigned long pid, const std::filesystem::path& directory,
    std::filesystem::path* saved, std::wstring* error,
    unsigned window_seconds = 15);
bool pause_ram_timing(unsigned long pid, bool paused) noexcept;
std::wstring command_ram_timing(const std::filesystem::path& directory, bool save, bool paused);
} // namespace xrfg
