#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/ram_timing.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace xrfg {
namespace {

#ifndef XRFG_IMPLEMENTATION_VERSION
#define XRFG_IMPLEMENTATION_VERSION 0
#endif

constexpr wchar_t kConfigurationFileName[] = L"ofxr_bridge.ini";
constexpr wchar_t kConfigurationSection[] = L"diagnostics";
constexpr std::uint64_t kMegabyte = 1024ull * 1024ull;

[[nodiscard]] const char* operation_name(
    BridgeFlightOperation operation) noexcept {
    switch (operation) {
    case BridgeFlightOperation::steamvr_delivery_attach: return "steamvr_delivery_attach";
    case BridgeFlightOperation::view_configuration: return "view_configuration";
    case BridgeFlightOperation::vulkan_negotiation: return "vulkan_negotiation";
    case BridgeFlightOperation::vulkan_bridge: return "vulkan_bridge";
    case BridgeFlightOperation::process_excluded: return "process_excluded";
    case BridgeFlightOperation::projection_view_rect: return "projection_view_rect";
    case BridgeFlightOperation::steamvr_delivery: return "steamvr_delivery";
    case BridgeFlightOperation::d3d11_bridge: return "d3d11_bridge";
    case BridgeFlightOperation::vram_usage: return "vram_usage";
    case BridgeFlightOperation::fsr_eye_binding: return "fsr_eye_binding";
    case BridgeFlightOperation::fsr_hudless_pair: return "fsr_hudless_pair";
    case BridgeFlightOperation::fsr_recovery: return "fsr_recovery";
    case BridgeFlightOperation::fsr_context_lifecycle: return "fsr_context_lifecycle";
    case BridgeFlightOperation::fsr_submit_stage: return "fsr_submit_stage";
    case BridgeFlightOperation::fsr_eye_resources: return "fsr_eye_resources";
    case BridgeFlightOperation::fsr_eye_rects: return "fsr_eye_rects";
    case BridgeFlightOperation::fsr_eye_motion: return "fsr_eye_motion";
    case BridgeFlightOperation::fsr_eye_pose: return "fsr_eye_pose";
    case BridgeFlightOperation::fsr_eye_orientation: return "fsr_eye_orientation";
    case BridgeFlightOperation::fsr_eye_fov: return "fsr_eye_fov";
    case BridgeFlightOperation::fsr_eye_clip_vertex: return "fsr_eye_clip_vertex";

    case BridgeFlightOperation::logger: return "logger";
    case BridgeFlightOperation::negotiation: return "negotiation";
    case BridgeFlightOperation::instance_create: return "instance_create";
    case BridgeFlightOperation::instance_destroy: return "instance_destroy";
    case BridgeFlightOperation::session_create: return "session_create";
    case BridgeFlightOperation::session_destroy: return "session_destroy";
    case BridgeFlightOperation::session_begin: return "session_begin";
    case BridgeFlightOperation::session_end: return "session_end";
    case BridgeFlightOperation::application_wait_frame: return "app_wait_frame";
    case BridgeFlightOperation::application_begin_frame: return "app_begin_frame";
    case BridgeFlightOperation::application_end_frame: return "app_end_frame";
    case BridgeFlightOperation::application_swapchain_acquire:
        return "app_swapchain_acquire";
    case BridgeFlightOperation::application_swapchain_wait:
        return "app_swapchain_wait";
    case BridgeFlightOperation::application_swapchain_release:
        return "app_swapchain_release";
    case BridgeFlightOperation::private_swapchain_acquire:
        return "private_swapchain_acquire";
    case BridgeFlightOperation::private_swapchain_wait:
        return "private_swapchain_wait";
    case BridgeFlightOperation::private_swapchain_release:
        return "private_swapchain_release";
    case BridgeFlightOperation::synthesis_initialize: return "synthesis_initialize";
    case BridgeFlightOperation::synthesis_prime: return "synthesis_prime";
    case BridgeFlightOperation::synthesis_pair: return "synthesis_pair";
    case BridgeFlightOperation::downstream_first_end_frame:
        return "downstream_first_end_frame";
    case BridgeFlightOperation::internal_wait_frame: return "internal_wait_frame";
    case BridgeFlightOperation::internal_begin_frame: return "internal_begin_frame";
    case BridgeFlightOperation::internal_end_frame: return "internal_end_frame";
    case BridgeFlightOperation::continuity_reset: return "continuity_reset";
    case BridgeFlightOperation::gpu_drain: return "gpu_drain";
    case BridgeFlightOperation::swapchain_create: return "swapchain_create";
    case BridgeFlightOperation::swapchain_eligibility: return "swapchain_eligibility";
    case BridgeFlightOperation::projection_mapping: return "projection_mapping";
    case BridgeFlightOperation::generation_prepare: return "generation_prepare";
    case BridgeFlightOperation::session_binding: return "session_binding";
    case BridgeFlightOperation::swapchain_image: return "swapchain_image";
    case BridgeFlightOperation::d3d11_capture: return "d3d11_capture";
    case BridgeFlightOperation::d3d11_publish: return "d3d11_publish";
    case BridgeFlightOperation::history_capture: return "history_capture";
    case BridgeFlightOperation::three_slot_worker_wait: return "three_slot_worker_wait";
    case BridgeFlightOperation::three_slot_buffer_wait: return "three_slot_buffer_wait";
    case BridgeFlightOperation::three_slot_handoff: return "three_slot_handoff";
    case BridgeFlightOperation::three_slot_gpu_ready: return "three_slot_gpu_ready";
    case BridgeFlightOperation::three_slot_present: return "three_slot_present";
    case BridgeFlightOperation::composition_layer: return "composition_layer";
    case BridgeFlightOperation::composition_publish: return "composition_publish";
    case BridgeFlightOperation::timing_worker_run: return "timing_worker_run";
    case BridgeFlightOperation::timing_slot_wait: return "timing_slot_wait";
    case BridgeFlightOperation::timing_director_wait: return "timing_director_wait";
    case BridgeFlightOperation::timing_worker_publish: return "timing_worker_publish";
    case BridgeFlightOperation::timing_runtime_gate: return "timing_runtime_gate";
    case BridgeFlightOperation::timing_worker_prepare: return "timing_worker_prepare";
    case BridgeFlightOperation::timing_synthesis_complete: return "timing_synthesis_complete";
    case BridgeFlightOperation::timing_publication_complete: return "timing_publication_complete";
    case BridgeFlightOperation::timing_aux_complete: return "timing_aux_complete";
    case BridgeFlightOperation::timing_worker_build: return "timing_worker_build";
    case BridgeFlightOperation::runtime_identity: return "runtime_identity";
    case BridgeFlightOperation::presenter_submission:
        return "presenter_submission";
    case BridgeFlightOperation::presenter_source:
        return "presenter_source";
    case BridgeFlightOperation::presenter_clock_anchor:
        return "presenter_clock_anchor";
    case BridgeFlightOperation::presenter_clock_capability:
        return "presenter_clock_capability";
    case BridgeFlightOperation::presenter_queue_drop:
        return "presenter_queue_drop";
    case BridgeFlightOperation::presenter_transition:
        return "presenter_transition";
    case BridgeFlightOperation::nvidia_gpu_stages:
        return "nvidia_gpu_stages";
    case BridgeFlightOperation::nvidia_gpu_total:
        return "nvidia_gpu_total";
    case BridgeFlightOperation::presenter_pace:
        return "presenter_pace";
    case BridgeFlightOperation::runtime_entry_section:
        return "runtime_entry_section";
    case BridgeFlightOperation::synthesis_frame_start_wait:
        return "synthesis_frame_start_wait";
    case BridgeFlightOperation::embedded_configuration: return "embedded_configuration";
    case BridgeFlightOperation::synthesis_gpu_span:
        return "synthesis_gpu_span";
    case BridgeFlightOperation::virtual_clock_clamp:
        return "virtual_clock_clamp";
    case BridgeFlightOperation::session_state:
        return "session_state";
    case BridgeFlightOperation::presenter_pair_release:
        return "presenter_pair_release";
    case BridgeFlightOperation::luke_guide_contract: return "luke_guide_contract";
    case BridgeFlightOperation::luke_projection_contract:
        return "luke_projection_contract";
    case BridgeFlightOperation::luke_output_contract: return "luke_output_contract";
    case BridgeFlightOperation::luke_frame_contract: return "luke_frame_contract";
    case BridgeFlightOperation::luke_pose_contract: return "luke_pose_contract";
    case BridgeFlightOperation::luke_afw_stage: return "luke_afw_stage";
    case BridgeFlightOperation::gpu_fence_snapshot: return "gpu_fence_snapshot";
    case BridgeFlightOperation::gpu_device_status: return "gpu_device_status";
    }
    if (operation == BridgeFlightOperation::luke_release) return "luke_release";
    if (operation == BridgeFlightOperation::luke_filter) return "luke_filter";
    if (operation == BridgeFlightOperation::luke_cycle_wait) return "luke_cycle_wait";
    return "unknown";
}

[[nodiscard]] std::filesystem::path fallback_log_directory() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(buffer.data()) / L"OFXR Bridge" / L"Logs";
}

[[nodiscard]] std::wstring log_file_name() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<wchar_t, 128> name{};
    _snwprintf_s(
        name.data(),
        name.size(),
        _TRUNCATE,
        L"ofxr-bridge-flight-%04u%02u%02u-%02u%02u%02u-pid%lu.log",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned long>(GetCurrentProcessId()));
    return name.data();
}

[[nodiscard]] HANDLE create_log_file(
    const std::filesystem::path& directory,
    std::filesystem::path* output_path) noexcept {
    try {
        if (directory.empty() || output_path == nullptr) {
            return INVALID_HANDLE_VALUE;
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return INVALID_HANDLE_VALUE;
        }
        const auto path = directory / log_file_name();
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            *output_path = path;
        }
        return file;
    } catch (...) {
        return INVALID_HANDLE_VALUE;
    }
}

[[nodiscard]] std::filesystem::path current_module_directory() noexcept {
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&initialize_bridge_flight_logger),
                &module)) {
            return {};
        }
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(
            module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return {};
        }
        return std::filesystem::path(path.data()).parent_path();
    } catch (...) {
        return {};
    }
}

} // namespace

// Rendering threads enqueue raw records only. A single diagnostic writer owns
// formatting, disk writes, wrapping and flushing; queue pressure drops records
// instead of blocking the game, synthesis worker or director.
struct BridgeFlightLogger::Impl {
    struct Record {
        std::uint64_t sequence{}, duration{}, a{}, b{}, c{};
        std::int64_t counter{}, result{};
        DWORD thread{};
        BridgeFlightOperation operation{};
        char phase{};
    };
    struct Slot { std::atomic<std::uint64_t> turn{}; Record record{}; };
    static constexpr std::uint64_t capacity = 262144;
    HANDLE file{INVALID_HANDLE_VALUE};
    LARGE_INTEGER frequency{}, origin{};
    std::atomic<std::uint64_t> next_sequence{1};
    std::filesystem::path path;
    std::uint64_t maximum_bytes{32 * kMegabyte};
    bool flush_each_event{};
    std::atomic<bool> active{false}, stop{false};
    std::unique_ptr<Slot[]> slots;
    std::atomic<std::uint64_t> enqueue_position{0}, dropped{0};
    std::uint64_t dequeue_position{}, file_position{};
    std::array<char, 65536> batch{};
    std::size_t batch_size{};
    std::thread writer;
    std::mutex flush_mutex;
    std::condition_variable flush_condition;
    std::atomic<std::uint64_t> flush_target{0}, flush_requested{0}, flush_completed{0};

    void start() {
        slots = std::make_unique<Slot[]>(capacity);
        for (std::uint64_t i = 0; i < capacity; ++i) slots[i].turn.store(i);
        active.store(true);
        writer = std::thread([this] { run(); });
    }
    void write(std::uint64_t sequence, std::int64_t counter, char phase,
        BridgeFlightOperation operation, std::int64_t result, std::uint64_t duration,
        std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
        if (!active.load(std::memory_order_relaxed)) return;
        auto position = enqueue_position.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            auto& slot = slots[position & (capacity - 1)];
            const auto turn = slot.turn.load(std::memory_order_acquire);
            if (turn == position) {
                if (!enqueue_position.compare_exchange_weak(position, position + 1,
                        std::memory_order_relaxed)) continue;
                slot.record = {sequence, duration, a, b, c, counter, result,
                    GetCurrentThreadId(), operation, phase};
                slot.turn.store(position + 1, std::memory_order_release);
                return;
            }
            if (turn < position) break; // full: never wait for the writer
            position = enqueue_position.load(std::memory_order_relaxed);
        }
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    bool pop(Record& record) noexcept {
        auto& slot = slots[dequeue_position & (capacity - 1)];
        if (slot.turn.load(std::memory_order_acquire) != dequeue_position + 1) return false;
        record = slot.record;
        slot.turn.store(dequeue_position + capacity, std::memory_order_release);
        ++dequeue_position;
        return true;
    }
    bool write_batch() noexcept {
        if (batch_size == 0) return true;
        DWORD written{};
        const bool good = WriteFile(file, batch.data(), static_cast<DWORD>(batch_size),
            &written, nullptr) && written == batch_size;
        batch_size = 0;
        if (!good) active.store(false);
        return good;
    }
    void serialize(const Record& record) noexcept {
        const double ms = frequency.QuadPart > 0
            ? static_cast<double>(record.counter - origin.QuadPart) * 1000.0 / frequency.QuadPart : 0.0;
        std::array<char, 384> line{};
        const int length = std::snprintf(line.data(), line.size(),
            "seq=%llu ms=%.3f tid=%lu phase=%c op=%s result=%lld dur_us=%llu a=%llu b=%llu c=%llu\r\n",
            static_cast<unsigned long long>(record.sequence), ms, static_cast<unsigned long>(record.thread),
            record.phase, operation_name(record.operation), static_cast<long long>(record.result),
            static_cast<unsigned long long>(record.duration), static_cast<unsigned long long>(record.a),
            static_cast<unsigned long long>(record.b), static_cast<unsigned long long>(record.c));
        if (length <= 0 || static_cast<std::size_t>(length) >= line.size()) return;
        if (file_position + length > maximum_bytes) {
            if (!write_batch()) return;
            LARGE_INTEGER beginning{};
            if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
                active.store(false);
                return;
            }
            file_position = 0;
        }
        if (batch_size + length > batch.size() && !write_batch()) return;
        std::memcpy(batch.data() + batch_size, line.data(), length);
        batch_size += length;
        file_position += length;
        if (flush_each_event) {
            static_cast<void>(write_batch());
            static_cast<void>(FlushFileBuffers(file)); // writer thread only
        }
    }
    void run() noexcept {
        for (;;) {
            Record record{};
            bool found = false;
            for (unsigned i = 0; i < 4096 && pop(record); ++i) {
                found = true;
                if (active.load()) serialize(record);
            }
            const auto lost = dropped.exchange(0);
            if (lost && active.load()) {
                LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
                serialize({next_sequence.fetch_add(1), 0, lost, capacity, 0,
                    now.QuadPart, -1, GetCurrentThreadId(), BridgeFlightOperation::logger, 'I'});
            }
            static_cast<void>(write_batch());
            const auto request = flush_requested.load(std::memory_order_acquire);
            if (request > flush_completed.load() && dequeue_position >= flush_target.load()) {
                static_cast<void>(FlushFileBuffers(file));
                { std::scoped_lock lock(flush_mutex); flush_completed.store(request); }
                flush_condition.notify_all();
            }
            if (stop.load() && dequeue_position == enqueue_position.load()) break;
            if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        static_cast<void>(FlushFileBuffers(file));
    }
    void flush() {
        std::unique_lock lock(flush_mutex);
        flush_target.store(enqueue_position.load(), std::memory_order_release);
        const auto request = flush_requested.fetch_add(1, std::memory_order_release) + 1;
        flush_condition.wait(lock, [&] { return flush_completed.load() >= request; });
    }
    void finish() noexcept {
        stop.store(true);
        if (writer.joinable()) writer.join();
        active.store(false);
    }
    ~Impl() {
        finish();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

BridgeFlightLogger::BridgeFlightLogger() noexcept = default;

BridgeFlightLogger::~BridgeFlightLogger() {
    shutdown();
}

void BridgeFlightLogger::initialize(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (impl_ || ram_) {
            return;
        }
        const auto config_path = module_directory / kConfigurationFileName;
        gpu_instrumentation_enabled_ = GetPrivateProfileIntW(
            kConfigurationSection, L"gpu_instrumentation_enabled", 0,
            config_path.c_str()) != 0;
        if (GetPrivateProfileIntW(kConfigurationSection, L"timing_capture", 0,
                config_path.c_str()) != 0) {
            auto capture = std::make_unique<RamTimingCapture>();
            const bool pose_profile = GetPrivateProfileIntW(
                kConfigurationSection, L"pose_cadence_capture", 0,
                config_path.c_str()) != 0;
            if (capture->initialize(pose_profile)) ram_ = std::move(capture);
            // Exclusive RAM mode. Neither capture nor normal logging enables
            // GPU work; only the independent explicit instrumentation flag can.
            return;
        }
        if (GetPrivateProfileIntW(
                kConfigurationSection,
                L"logging_enabled",
                0,
                config_path.c_str()) == 0) {
            return;
        }

        auto implementation = std::make_unique<Impl>();
        const UINT maximum_mb = std::clamp<UINT>(
            GetPrivateProfileIntW(
                kConfigurationSection,
                L"max_file_mb",
                32,
                config_path.c_str()),
            1,
            256);
        implementation->maximum_bytes =
            static_cast<std::uint64_t>(maximum_mb) * kMegabyte;
        implementation->flush_each_event = GetPrivateProfileIntW(
            kConfigurationSection,
            L"flush_each_event",
            0,
            config_path.c_str()) != 0;
        implementation->file = create_log_file(
            module_directory, &implementation->path);
        if (implementation->file == INVALID_HANDLE_VALUE) {
            implementation->file = create_log_file(
                fallback_log_directory(), &implementation->path);
        }
        if (implementation->file == INVALID_HANDLE_VALUE ||
            !QueryPerformanceFrequency(&implementation->frequency) ||
            !QueryPerformanceCounter(&implementation->origin)) {
            return;
        }
        implementation->start();
        impl_ = std::move(implementation);
        event(
            BridgeFlightOperation::logger,
            XRFG_IMPLEMENTATION_VERSION,
            maximum_mb,
            impl_->flush_each_event ? 1u : 0u,
            GetCurrentProcessId());
    } catch (...) {
        impl_.reset();
    }
}

void BridgeFlightLogger::shutdown() noexcept {
    gpu_instrumentation_enabled_ = false;
    ram_.reset();
    try {
        if (!impl_) {
            return;
        }
        event(BridgeFlightOperation::logger, 0);
        impl_.reset();
    } catch (...) {
        impl_.reset();
    }
}

bool BridgeFlightLogger::enabled() const noexcept {
    return impl_ && impl_->active.load(std::memory_order_relaxed);
}

void BridgeFlightLogger::flush() noexcept {
    try { if (impl_) impl_->flush(); } catch (...) {}
}

bool BridgeFlightLogger::ram_enabled() const noexcept {
    return ram_ != nullptr;
}

std::int64_t BridgeFlightLogger::microseconds_for_counter(
    std::int64_t counter) const noexcept {
    if (impl_ == nullptr || impl_->frequency.QuadPart <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(
        (counter - impl_->origin.QuadPart) * 1000000ll /
        impl_->frequency.QuadPart);
}

std::filesystem::path BridgeFlightLogger::log_path() const {
    return impl_ ? impl_->path : std::filesystem::path{};
}

BridgeFlightToken BridgeFlightLogger::begin(
    BridgeFlightOperation operation,
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c) noexcept {
    if (ram_) return ram_->begin(operation, a, b, c);
    if (!enabled()) {
        return {};
    }
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return {};
    }
    const BridgeFlightToken token{
        impl_->next_sequence.fetch_add(1, std::memory_order_relaxed),
        now.QuadPart};
    impl_->write(
        token.sequence, token.start_counter, 'B', operation, 0, 0, a, b, c);
    return token;
}

void BridgeFlightLogger::end(
    BridgeFlightToken token,
    BridgeFlightOperation operation,
    std::int64_t result,
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c) noexcept {
    if (ram_) { ram_->end(token, operation, result, a, b, c); return; }
    if (!enabled() || token.sequence == 0) {
        return;
    }
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return;
    }
    const std::uint64_t duration = impl_->frequency.QuadPart > 0 &&
            now.QuadPart >= token.start_counter
        ? static_cast<std::uint64_t>(
              (now.QuadPart - token.start_counter) * 1000000ll /
              impl_->frequency.QuadPart)
        : 0;
    impl_->write(
        token.sequence, now.QuadPart, 'E', operation, result, duration, a, b, c);
}

void BridgeFlightLogger::event(
    BridgeFlightOperation operation,
    std::int64_t result,
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c) noexcept {
    if (ram_) { ram_->event(operation, result, a, b, c); return; }
    if (!enabled()) {
        return;
    }
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) {
        return;
    }
    impl_->write(
        impl_->next_sequence.fetch_add(1, std::memory_order_relaxed),
        now.QuadPart,
        'I',
        operation,
        result,
        0,
        a,
        b,
        c);
}

BridgeFlightLogger& bridge_flight_logger() noexcept {
    static BridgeFlightLogger logger;
    return logger;
}

void initialize_bridge_flight_logger() noexcept {
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            bridge_flight_logger().initialize(current_module_directory());
        });
    } catch (...) {
    }
}

} // namespace xrfg
