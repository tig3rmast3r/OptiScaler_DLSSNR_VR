#include "xrfg/ram_timing.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
constexpr unsigned kSaveWindowSeconds = 3600;

bool rising_edge(bool down, bool& previous) noexcept {
    const bool pressed = down && !previous;
    previous = down;
    return pressed;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--selftest") == 0) {
        xrfg::RamTimingCapture capture;
        if (!capture.initialize(true)) return 3;
        capture.event(xrfg::BridgeFlightOperation::luke_frame_contract,
            6, 42, 7, 3);
        capture.event(xrfg::BridgeFlightOperation::luke_afw_stage,
            0, 52, 1, 2);
        capture.event(xrfg::BridgeFlightOperation::luke_afw_stage,
            0, 18, 1, 2);
        capture.event(xrfg::BridgeFlightOperation::gpu_synthesis,
            4, 12, 31, 42);
        using O = xrfg::BridgeFlightOperation;
        const O diagnostic_operations[] = {
            O::luke_inline_clock, O::luke_physical_submit, O::luke_publication_fence,
            O::luke_publication_images, O::luke_publication_slots, O::luke_input_guide,
            O::luke_input_camera, O::luke_locate_context, O::luke_locate_pose,
            O::luke_inline_view, O::luke_inline_pose_bits, O::presenter_clock_capability,
            O::luke_space_created, O::luke_space_locate, O::luke_space_time, O::luke_render_camera,
            O::luke_temporal_pair, O::luke_temporal_pose,
            O::three_slot_handoff, O::three_slot_gpu_ready, O::three_slot_present,
            O::timing_worker_prepare, O::generation_prepare};
        for (auto operation : diagnostic_operations) capture.event(operation, 600, 42, 43, 44);
        capture.event(xrfg::BridgeFlightOperation::nvidia_gpu_stages,
            0, 0, 0, 0);
        capture.event(xrfg::BridgeFlightOperation::private_swapchain_acquire,
            0, 15, 2, 0);
        capture.event(xrfg::BridgeFlightOperation::private_swapchain_release,
            0, 15, 2, 0);
        std::filesystem::path saved;
        std::wstring error;
        if (!xrfg::save_ram_timing(GetCurrentProcessId(), argv[2],
                &saved, &error, kSaveWindowSeconds)) return 4;
        std::ifstream file(saved, std::ios::binary);
        const std::string csv(std::istreambuf_iterator<char>{file}, {});
        bool prior = false;
        if (!rising_edge(true, prior) || rising_edge(true, prior) ||
            rising_edge(false, prior) || !rising_edge(true, prior)) return 6;
        const char* diagnostic_names[] = {
            "luke_inline_clock", "luke_physical_submit", "luke_publication_fence",
            "luke_publication_images", "luke_publication_slots", "luke_input_guide",
            "luke_input_camera", "luke_locate_context", "luke_locate_pose",
            "luke_inline_view", "luke_inline_pose_bits", "presenter_clock_capability",
            "luke_space_created", "luke_space_locate", "luke_space_time", "luke_render_camera",
            "luke_temporal_pair", "luke_temporal_pose",
            "three_slot_handoff", "three_slot_gpu_ready", "three_slot_present",
            "timing_worker_prepare", "generation_prepare"};
        for (const auto* name : diagnostic_names)
            if (csv.find(std::string(",") + name + ",600,42,43,44") == std::string::npos) return 7;
        return csv.find("schema=225 window_seconds=3600") != std::string::npos &&
            csv.find(",luke_frame_contract,6,42,7,3") != std::string::npos &&
            csv.find(",luke_afw_stage,0,52,1,2") != std::string::npos &&
            csv.find(",luke_afw_stage,0,18,1,2") != std::string::npos &&
            csv.find(",gpu_synthesis,4,12,31,42") != std::string::npos &&
            csv.find(",private_swapchain_acquire,0,15,2,0") != std::string::npos &&
            csv.find(",private_swapchain_release,0,15,2,0") != std::string::npos &&
            csv.find("nvidia_gpu_stages") == std::string::npos ? 0 : 5;
    }
    if (argc != 2 || argv[1] == nullptr || argv[1][0] == L'\0') return 2;
    const std::filesystem::path output_directory(argv[1]);
    bool prior_f11 = false;

    // Only poll the key: no process scan, snapshot or disk write until F11.
    // Explicitly launched saver stays available until the user/session ends.
    // A silent one-hour expiry previously lost later F11 captures.
    while (true) {
        const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (rising_edge(down, prior_f11)) {
            std::filesystem::create_directories(output_directory);
            std::ofstream status(output_directory / L"f11-helper-status.log", std::ios::app);
            SYSTEMTIME now{}; GetLocalTime(&now);
            status << now.wYear << '-' << now.wMonth << '-' << now.wDay << ' '
                << now.wHour << ':' << now.wMinute << ':' << now.wSecond << " F11\n";
            unsigned saved_count{};
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W process{};
                process.dwSize = sizeof(process);
                if (Process32FirstW(snapshot, &process)) {
                    do {
                        std::filesystem::path saved;
                        std::wstring error;
                        // The OFXR-owned mapping, not an executable whitelist,
                        // selects live capture clients. No scan or I/O until F11.
                        if (xrfg::save_ram_timing(
                            process.th32ProcessID, output_directory,
                            &saved, &error, kSaveWindowSeconds)) {
                            ++saved_count;
                            status << "pid=" << process.th32ProcessID
                                << " saved=" << saved.string() << '\n';
                        }
                    } while (Process32NextW(snapshot, &process));
                }
                CloseHandle(snapshot);
            }
            if (!saved_count) status << "No enabled OFXR RAM capture found; press F11 while game is running\n";
        }
        Sleep(25);
    }
    return 0;
}
