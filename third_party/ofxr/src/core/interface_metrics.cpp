#include "xrfg/interface_metrics.hpp"
#include <chrono>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif
namespace xrfg {
namespace {
std::atomic<std::int64_t> measurement_until{};
std::mutex telemetry_mutex;
InterfaceTelemetry telemetry;
}
void request_interface_measurement() noexcept {
    measurement_until.store(interface_clock_ns() + 3'000'000'000, std::memory_order_relaxed);
}
bool interface_measurement_requested() noexcept {
    auto until = measurement_until.load(std::memory_order_relaxed);
    if (!until) return false;
    if (interface_clock_ns() < until) return true;
    measurement_until.compare_exchange_strong(until, 0, std::memory_order_relaxed);
    return false;
}
void publish_interface_telemetry(const InterfaceTelemetry& value) noexcept {
    try { std::scoped_lock lock(telemetry_mutex); telemetry = value; } catch (...) {}
}
InterfaceTelemetry interface_telemetry() noexcept {
    try {
        std::scoped_lock lock(telemetry_mutex);
#ifdef _WIN32
        const auto now = GetTickCount64();
#else
        const auto now = static_cast<std::uint64_t>(interface_clock_ns() / 1'000'000);
#endif
        return telemetry.updated_ms && now >= telemetry.updated_ms && now - telemetry.updated_ms < 2000
            ? telemetry : InterfaceTelemetry{};
    } catch (...) { return {}; }
}
std::int64_t interface_qpc_ns(std::int64_t ticks) noexcept {
#ifdef _WIN32
    static const auto frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return frequency > 0 ? static_cast<std::int64_t>(static_cast<long double>(ticks) * 1e9L / frequency) : 0;
#else
    return ticks;
#endif
}
std::int64_t interface_clock_ns() noexcept {
#ifdef _WIN32
    LARGE_INTEGER qpc{}; return QueryPerformanceCounter(&qpc) ? interface_qpc_ns(qpc.QuadPart) : 0;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
}
