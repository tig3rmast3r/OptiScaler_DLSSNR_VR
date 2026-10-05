#include "xrfg/fsr_cadence_probe.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        xrfg::FsrCadenceRow row;
        row.pair(true, 5'000'000, 11'111'100, 2'000'000, 11'111'100);
        row.pair(true, 15'000'000, 11'111'100, 8'000'000, 22'222'200);
        row.pair(false, 1, 1, 1, 0);
        check(row.pairs == 2 && row.cycle_errors == 1 && row.short_gap == 1, "pair counts");
        check(row.gap_ns == 20'000'000 && row.gap_min_ns == 5'000'000 && row.gap_max_ns == 15'000'000, "gap accumulation");
        row.pair(true, 0, 0, 0, 0);
        check(row.no_period == 1 && row.nonpositive_target_delta == 1, "invalid target/period counts");
        wchar_t temp[32768]{}; GetTempPathW(32768, temp);
        const auto root = std::filesystem::path(temp) /
            (L"ofxr-cadence-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        const auto ini = root / L"ofxr_bridge.ini";
        auto log_count = [&] {
            std::size_t count{};
            for (const auto& file : std::filesystem::directory_iterator(root))
                if (file.path().extension() == L".log") ++count;
            return count;
        };
        {
            xrfg::FsrCadenceProbe disabled(root);
            check(!disabled.marker_enabled(), "logging must not enable image markers");
            disabled.generated(false); disabled.pair(true, 1, 2, 0, 2); disabled.flush();
            check(log_count() == 0, "disabled probe wrote a file");
        }
        {
            std::ofstream file(ini); file << "[diagnostics]\nlogging_enabled=0\ncadence_logging_enabled=1\n";
        }
        {
            xrfg::FsrCadenceProbe enabled(root);
            check(!enabled.marker_enabled(), "cadence logging must not enable image markers");
            enabled.generated(true); enabled.bypass(5);
            enabled.pair(true, 5'000'000, 11'111'100, 1'000'000, 11'111'100);
            check(log_count() == 0, "probe wrote to disk on frame path");
            enabled.flush();
            check(log_count() == 1, "shutdown flush missing");
        }
        {
            std::ofstream file(ini); file << "[diagnostics]\ncadence_logging_enabled=1\nsynthetic_marker_enabled=0\n";
        }
        { xrfg::FsrCadenceProbe without_marker(root); check(!without_marker.marker_enabled(), "explicit marker-off ignored"); }
        { std::ofstream file(ini); file << "[diagnostics]\nsynthetic_marker_enabled=1\n"; }
        { xrfg::FsrCadenceProbe marker(root); check(marker.marker_enabled(), "explicit marker-on ignored"); }
        for (const auto& file : std::filesystem::directory_iterator(root)) std::filesystem::remove(file.path());
        std::filesystem::remove(root);
        std::cout << "FSR cadence: counters, independent marker, no frame-path I/O, flush and disabled mode passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
