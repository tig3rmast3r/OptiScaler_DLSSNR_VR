#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

namespace xrfg {

// EndFrame's frame_call_mutex owns this probe. No additional synchronization,
// allocation, I/O, GPU queries, or worker thread on the measured frame path.
// These are submission statistics, NEVER a claim of headset scanout visibility.
struct FsrCadenceRow {
    std::uint64_t second{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t generated{}, marker_pairs{}, pairs{}, cycle_errors{};
    std::uint64_t short_gap{}, no_period{}, nonpositive_target_delta{};
    std::uint64_t gap_ns{}, gap_min_ns{std::numeric_limits<std::uint64_t>::max()}, gap_max_ns{};
    std::uint64_t period_ns{}, wait_ns{}, target_delta_ns{}, bypass{};
    std::array<std::uint64_t, 24> reasons{};

    void pair(bool completed, std::uint64_t gap, std::uint64_t period,
              std::uint64_t wait, std::int64_t target_delta) noexcept {
        if (!completed) { ++cycle_errors; return; }
        ++pairs;
        gap_ns += gap;
        gap_min_ns = (std::min)(gap_min_ns, gap);
        gap_max_ns = (std::max)(gap_max_ns, gap);
        period_ns += period;
        wait_ns += wait;
        if (!period) ++no_period;
        else if (gap < period) ++short_gap;
        if (target_delta <= 0) ++nonpositive_target_delta;
        else target_delta_ns += static_cast<std::uint64_t>(target_delta);
    }
};

class FsrCadenceProbe {
public:
    static constexpr std::size_t capacity = 600; // final ten minutes, bounded RAM
    explicit FsrCadenceProbe(const std::filesystem::path& directory) noexcept {
        try {
            const auto ini = directory / L"ofxr_bridge.ini";
            enabled_ = GetPrivateProfileIntW(L"diagnostics", L"cadence_logging_enabled", 0, ini.c_str()) != 0;
            marker_ = GetPrivateProfileIntW(L"diagnostics", L"synthetic_marker_enabled", 0, ini.c_str());
            if (!enabled_) return;
            SYSTEMTIME now{}; GetLocalTime(&now);
            wchar_t name[160]{};
            swprintf_s(name, L"ofxr-fsr-cadence-%04u%02u%02u-%02u%02u%02u-pid%lu-session%llu.log",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                GetCurrentProcessId(), ++next_session_);
            path_ = directory / name;
            origin_ = std::chrono::steady_clock::now();
        } catch (...) { enabled_ = false; }
    }
    ~FsrCadenceProbe() { flush(); }
    FsrCadenceProbe(const FsrCadenceProbe&) = delete;
    FsrCadenceProbe& operator=(const FsrCadenceProbe&) = delete;

    bool marker_enabled() const noexcept {
        return marker_ > 0;
    }
    void generated(bool marker) noexcept {
        if (!enabled_) return;
        auto& r = row(); ++r.generated; if (marker) ++r.marker_pairs;
    }
    void bypass(std::int64_t reason) noexcept {
        if (!enabled_) return;
        auto& r = row(); ++r.bypass;
        if (reason >= 0 && reason < static_cast<std::int64_t>(r.reasons.size())) ++r.reasons[reason];
    }
    void pair(bool completed, std::uint64_t gap, std::uint64_t period,
              std::uint64_t wait, std::int64_t target_delta) noexcept {
        if (enabled_) row().pair(completed, gap, period, wait, target_delta);
    }
    // Caller must exclude EndFrame. File creation happens only on session
    // shutdown/explicit flush, never on a measured frame. A killed process can
    // lose this diagnostic; the existing crash recorders remain independent.
    void flush() noexcept {
        if (!enabled_ || path_.empty()) return;
        FILE* file{};
        if (_wfopen_s(&file, path_.c_str(), L"wb") != 0 || !file) return;
        std::fprintf(file, "FSR cadence probe: RAM aggregation; disk write only at shutdown; submission != headset scanout\n"
            "second generated marker_requested_pairs submitted_pairs cycle_errors bypass short_gap no_period target_nonpositive "
            "gap_mean_ms gap_min_ms gap_max_ms period_mean_ms wait_mean_ms target_delta_mean_ms bypass_reason_counts\n");
        std::uint64_t maximum{};
        bool any{};
        for (const auto& r : rows_) if (r.second != std::numeric_limits<std::uint64_t>::max()) {
            maximum = (std::max)(maximum, r.second); any = true;
        }
        if (any) for (auto second = maximum >= capacity ? maximum - capacity + 1 : 0; second <= maximum; ++second) {
            const auto& r = rows_[second % capacity];
            if (r.second != second) continue;
            const double divisor = r.pairs ? static_cast<double>(r.pairs) * 1.e6 : 1.;
            std::fprintf(file, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %.6f %.6f %.6f %.6f %.6f %.6f",
                r.second, r.generated, r.marker_pairs, r.pairs, r.cycle_errors, r.bypass, r.short_gap,
                r.no_period, r.nonpositive_target_delta, r.gap_ns / divisor,
                r.pairs ? r.gap_min_ns / 1.e6 : 0., r.gap_max_ns / 1.e6,
                r.period_ns / divisor, r.wait_ns / divisor, r.target_delta_ns / divisor);
            for (std::size_t i = 0; i < r.reasons.size(); ++i) if (r.reasons[i])
                std::fprintf(file, " %zu:%llu", i, r.reasons[i]);
            std::fprintf(file, "\n");
        }
        std::fclose(file);
    }
private:
    FsrCadenceRow& row() noexcept {
        const auto elapsed = std::chrono::steady_clock::now() - origin_;
        const auto second = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        auto& r = rows_[second % capacity];
        if (r.second != second) { r = {}; r.second = second; }
        return r;
    }
    inline static std::atomic<std::uint64_t> next_session_{};
    bool enabled_{};
    int marker_{-1};
    std::filesystem::path path_;
    std::chrono::steady_clock::time_point origin_{};
    std::array<FsrCadenceRow, capacity> rows_{};
};
} // namespace xrfg
