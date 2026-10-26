#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <Windows.h>
namespace xrfg {
inline std::atomic_bool diagnostic_ring_enabled{};
inline std::mutex diagnostic_ring_sink_mutex;
inline std::function<void(std::string)> diagnostic_ring_sink;
inline std::int64_t diagnostic_ring_qpc() noexcept { LARGE_INTEGER v{};QueryPerformanceCounter(&v);return v.QuadPart; }
inline void diagnostic_ring_record(std::string row) noexcept {
    if(!diagnostic_ring_enabled.load(std::memory_order_relaxed))return;
    try {
        std::function<void(std::string)> sink;
        {std::scoped_lock lock(diagnostic_ring_sink_mutex);sink=diagnostic_ring_sink;}
        if(sink)sink(std::move(row));
    }catch(...){}
}
}
