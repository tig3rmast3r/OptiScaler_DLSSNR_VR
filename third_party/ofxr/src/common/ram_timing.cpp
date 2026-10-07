#include "xrfg/ram_timing.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <new>
#include <vector>

namespace xrfg {
namespace {
// Startup GPU workers are persistent owners too: V224's eight lanes excluded
// the later SR/XR/AFW threads. Cover 64 independent threads in a bounded 1 GiB
// record pool. Hot lanes retain a shorter window; the saver reports truncation.
// No disk I/O during capture; each lane remains independently owned.
constexpr unsigned kLanes = 64, kCapacity = 262144, kSchema = 225;
constexpr std::uint64_t kMagic = 0x4f46585252414d31ULL;
static_assert(std::atomic<LONG>::is_always_lock_free);
struct Record {
    std::int64_t qpc;
    std::uint64_t sequence;
    std::int64_t result;
    std::uint64_t a, b, c;
    std::uint32_t operation, tid;
    char phase;
    char padding[7];
};
static_assert(sizeof(Record) == 64);
struct alignas(64) Lane {
    std::atomic<LONG> owner{}, busy{};
    std::uint64_t written{}, next_sequence{};
    std::array<Record, kCapacity> records;
};
struct Shared {
    std::uint64_t magic{};
    unsigned schema{kSchema};
    std::atomic<LONG> paused{}, unmapped_threads{};
    std::int64_t frequency{}, epoch{};
    wchar_t executable[512]{};
    std::array<Lane, kLanes> lanes;
};
static_assert(sizeof(Shared) <= 0xffffffffULL, "mapping size must fit the low DWORD");
std::wstring mapping_name(DWORD pid) {
    return L"Local\\OFXR-RamTiming-V225-" + std::to_wstring(pid);
}
bool selected_generic(BridgeFlightOperation op) noexcept {
    using O = BridgeFlightOperation;
    switch (op) {
    case O::application_wait_frame: case O::application_begin_frame:
    case O::application_end_frame: case O::application_swapchain_release:
    case O::application_swapchain_acquire: case O::application_swapchain_wait:
    case O::private_swapchain_acquire: case O::private_swapchain_wait: case O::private_swapchain_release:
    case O::internal_wait_frame: case O::internal_begin_frame: case O::internal_end_frame:
    case O::presenter_pace: case O::three_slot_worker_wait: case O::three_slot_buffer_wait:
    case O::three_slot_handoff: case O::three_slot_gpu_ready: case O::three_slot_present:
    case O::timing_worker_run: case O::timing_slot_wait: case O::timing_director_wait:
    case O::timing_worker_publish: case O::timing_runtime_gate:
    case O::timing_worker_prepare: case O::timing_synthesis_complete:
    case O::timing_publication_complete: case O::timing_aux_complete: case O::timing_worker_build:
    case O::d3d11_publish: case O::d3d11_capture: case O::synthesis_pair: case O::synthesis_prime:
    case O::session_binding: case O::runtime_identity: case O::session_state:
    case O::gpu_history: case O::gpu_synthesis: case O::gpu_timing_error:
    case O::luke_release: case O::luke_filter: case O::luke_cycle_wait:
        return true;
    default: return false;
    }
}
bool selected_pose(BridgeFlightOperation op, std::uint64_t a) noexcept {
    using O = BridgeFlightOperation;
    switch (op) {
    case O::application_wait_frame: case O::application_end_frame:
    case O::downstream_first_end_frame: case O::internal_wait_frame:
    case O::internal_begin_frame: case O::internal_end_frame:
    case O::d3d11_publish: case O::synthesis_pair: case O::synthesis_prime:
    case O::private_swapchain_acquire: case O::private_swapchain_wait:
    case O::private_swapchain_release:
    case O::embedded_configuration: case O::session_state:
    case O::luke_frame_contract: case O::luke_pose_contract:
    case O::gpu_history: case O::gpu_synthesis: case O::gpu_timing_error:
    case O::luke_inline_clock: case O::luke_physical_submit:
    case O::luke_publication_fence: case O::luke_publication_images:
    case O::luke_publication_slots: case O::luke_input_guide: case O::luke_input_camera:
    case O::luke_locate_context: case O::luke_locate_pose:
    case O::luke_inline_view: case O::luke_inline_pose_bits:
    case O::luke_space_created: case O::luke_space_locate:
    case O::luke_space_time: case O::luke_render_camera:
    case O::luke_temporal_pair: case O::luke_temporal_pose:
    case O::three_slot_worker_wait: case O::three_slot_buffer_wait:
    case O::three_slot_handoff: case O::three_slot_gpu_ready: case O::three_slot_present:
    case O::timing_worker_run: case O::timing_worker_prepare:
    case O::timing_synthesis_complete: case O::timing_publication_complete:
    case O::timing_aux_complete: case O::timing_worker_build:
    case O::timing_slot_wait: case O::timing_director_wait:
    case O::timing_worker_publish: case O::timing_runtime_gate:
    case O::generation_prepare:
    case O::composition_layer: case O::composition_publish:
    case O::presenter_clock_capability:
        return true;
    case O::luke_afw_stage:
        // AFW CPU/queue boundaries plus guide, XR release, hold and pair.
        // The existing GPU timestamp queries are now visible in this profile.
        return a == 13 || a == 18 || a == 19 || a == 22 ||
            a == 23 || a == 26 || a == 27 || a == 28 || a == 31 ||
            a == 32 || a == 33 || a == 34 || a == 35 ||
            a == 36 || a == 52 || a == 53 || (a >= 54 && a <= 68) ||
            (a >= 71 && a <= 82) || (a >= 86 && a <= 104) || (a >= 122 && a <= 132) ||
            (a >= 944 && a <= 949);
    default: return false;
    }
}
const char* name(unsigned value) noexcept {
    using O = BridgeFlightOperation;
    switch (static_cast<O>(value)) {
#define ENTRY(x) case O::x: return #x
    ENTRY(application_wait_frame); ENTRY(application_begin_frame); ENTRY(application_end_frame);
    ENTRY(application_swapchain_release); ENTRY(internal_wait_frame); ENTRY(internal_begin_frame);
    ENTRY(downstream_first_end_frame); ENTRY(embedded_configuration);
    ENTRY(luke_frame_contract); ENTRY(luke_pose_contract); ENTRY(luke_afw_stage);
    ENTRY(application_swapchain_acquire); ENTRY(application_swapchain_wait);
    ENTRY(private_swapchain_acquire); ENTRY(private_swapchain_wait); ENTRY(private_swapchain_release);
    ENTRY(internal_end_frame); ENTRY(presenter_pace); ENTRY(three_slot_worker_wait);
    ENTRY(three_slot_buffer_wait); ENTRY(three_slot_handoff); ENTRY(three_slot_gpu_ready);
    ENTRY(three_slot_present); ENTRY(timing_worker_run); ENTRY(timing_slot_wait);
    ENTRY(timing_director_wait); ENTRY(timing_worker_publish); ENTRY(timing_runtime_gate);
    ENTRY(timing_worker_prepare); ENTRY(timing_synthesis_complete); ENTRY(timing_publication_complete);
    ENTRY(timing_aux_complete); ENTRY(timing_worker_build); ENTRY(d3d11_publish);
    ENTRY(d3d11_capture); ENTRY(synthesis_pair); ENTRY(synthesis_prime);
    ENTRY(session_binding); ENTRY(runtime_identity); ENTRY(session_state);
    ENTRY(gpu_history); ENTRY(gpu_synthesis); ENTRY(gpu_timing_error);
    ENTRY(luke_release); ENTRY(luke_filter); ENTRY(luke_cycle_wait);
    ENTRY(luke_inline_clock); ENTRY(luke_physical_submit);
    ENTRY(luke_publication_fence); ENTRY(luke_publication_images);
    ENTRY(luke_publication_slots); ENTRY(luke_input_guide); ENTRY(luke_input_camera);
    ENTRY(luke_locate_context); ENTRY(luke_locate_pose);
    ENTRY(luke_inline_view); ENTRY(luke_inline_pose_bits); ENTRY(presenter_clock_capability);
    ENTRY(luke_space_created); ENTRY(luke_space_locate);
    ENTRY(luke_space_time); ENTRY(luke_render_camera);
    ENTRY(luke_temporal_pair); ENTRY(luke_temporal_pose);
    ENTRY(generation_prepare);
    ENTRY(composition_layer); ENTRY(composition_publish);
#undef ENTRY
    default: return "unknown";
    }
}
struct Mapping {
    HANDLE handle{}; Shared* data{};
    ~Mapping() { if (data) UnmapViewOfFile(data); if (handle) CloseHandle(handle); }
    bool open(DWORD pid) {
        handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name(pid).c_str());
        if (!handle) return false;
        data = static_cast<Shared*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        return data && data->magic == kMagic && data->schema == kSchema;
    }
};
}
struct RamTimingCapture::Impl {
    Mapping mapping;
    bool pose_profile{};
    BridgeFlightToken write(char phase, BridgeFlightOperation op, BridgeFlightToken token,
        std::int64_t result, std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
        if (!(pose_profile ? selected_pose(op, a) : selected_generic(op))) return {};
        auto* s = mapping.data;
        if (s->paused.load() != 0) return {};
        struct Cache { Shared* owner{}; std::int64_t epoch{}; Lane* lane{}; };
        thread_local Cache cache;
        const DWORD tid = GetCurrentThreadId();
        if (cache.owner != s || cache.epoch != s->epoch) {
            cache = {s, s->epoch, nullptr};
            for (auto& lane : s->lanes) {
                LONG old = 0;
                if (lane.owner.compare_exchange_strong(old, static_cast<LONG>(tid)) ||
                    old == static_cast<LONG>(tid)) { cache.lane = &lane; break; }
            }
            if (!cache.lane) ++s->unmapped_threads;
        }
        if (!cache.lane) return {};
        auto& lane = *cache.lane;
        lane.busy.store(1);
        // Freeze protocol: publish busy BEFORE rechecking pause. A late writer
        // cannot write after the tray observes all pre-freeze writers quiescent.
        if (s->paused.load() != 0) {
            lane.busy.store(0); return {};
        }
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        if (phase != 'E') token = {kRamTimingToken | ++lane.next_sequence, now.QuadPart};
        lane.records[lane.written % kCapacity] = {
            now.QuadPart, token.sequence, result, a, b, c, static_cast<unsigned>(op), tid, phase, {}};
        ++lane.written;
        lane.busy.store(0);
        return token;
    }
};
RamTimingCapture::RamTimingCapture() = default;
RamTimingCapture::~RamTimingCapture() = default;
bool RamTimingCapture::initialize(bool pose_profile) noexcept {
    try {
        if (impl_) return true;
        auto p = std::make_unique<Impl>();
        p->pose_profile = pose_profile;
        p->mapping.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(sizeof(Shared)), mapping_name(GetCurrentProcessId()).c_str());
        if (!p->mapping.handle || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        p->mapping.data = static_cast<Shared*>(MapViewOfFile(p->mapping.handle,
            FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!p->mapping.data) return false;
        // New pagefile mappings are zero-filled. Prefault before constructing
        // headers; do not instantiate a huge value-initialized aggregate.
        auto* pages = reinterpret_cast<volatile unsigned char*>(p->mapping.data);
        for (std::size_t offset=0; offset<sizeof(Shared); offset+=4096) pages[offset]=0;
        auto* s = new (p->mapping.data) Shared;
        LARGE_INTEGER q{}, f{}; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        s->epoch = q.QuadPart; s->frequency = f.QuadPart;
        GetModuleFileNameW(nullptr, s->executable, 512);
        MemoryBarrier(); s->magic = kMagic;
        impl_ = std::move(p); return true;
    } catch (...) { return false; }
}
BridgeFlightToken RamTimingCapture::begin(BridgeFlightOperation op, std::uint64_t a,
    std::uint64_t b, std::uint64_t c) noexcept {
    return impl_ ? impl_->write('B', op, {}, 0, a, b, c) : BridgeFlightToken{};
}
void RamTimingCapture::end(BridgeFlightToken token, BridgeFlightOperation op, std::int64_t r,
    std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    if (impl_ && (token.sequence & kRamTimingToken)) impl_->write('E', op, token, r, a, b, c);
}
void RamTimingCapture::event(BridgeFlightOperation op, std::int64_t r,
    std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    if (impl_) impl_->write('I', op, {}, r, a, b, c);
}
bool pause_ram_timing(unsigned long pid, bool paused) noexcept {
    try { Mapping m; if (!m.open(pid)) return false;
        m.data->paused.store(paused ? 1 : 0); return true;
    } catch (...) { return false; }
}
bool save_ram_timing(unsigned long pid, const std::filesystem::path& directory,
    std::filesystem::path* saved, std::wstring* error, unsigned window_seconds) {
    Mapping m;
    if (!m.open(pid)) return false;
    auto& s = *m.data;
    const LONG prior = s.paused.exchange(1);
    struct Resume { Shared& s; LONG prior; bool active{true};
        ~Resume() { if(active) s.paused.store(prior); } } resume{s, prior};
    LARGE_INTEGER trigger{}; QueryPerformanceCounter(&trigger);
    const auto deadline = GetTickCount64() + 2000;
    for (;;) {
        bool busy = false;
        for (auto& lane : s.lanes) busy |= lane.busy.load() != 0;
        if (!busy) break;
        if (GetTickCount64() >= deadline) {
            if (error) *error = L"RAM snapshot busy: retry; no frame thread was blocked.";
            return false;
        }
        Sleep(1); // Tray only; producers never wait for this consumer.
    }
    const auto cutoff = trigger.QuadPart -
        static_cast<std::int64_t>(window_seconds) * s.frequency;
    std::vector<Record> records;
    std::array<std::uint64_t, kLanes> totals{};
    std::array<std::int64_t, kLanes> oldest{};
    for (unsigned i = 0; i < kLanes; ++i) {
        const auto& lane = s.lanes[i]; totals[i] = lane.written;
        const auto count = std::min<std::uint64_t>(lane.written, kCapacity);
        if (count) oldest[i] = lane.records[(lane.written-count)%kCapacity].qpc;
        for (auto n = lane.written - count; n < lane.written; ++n) {
            const auto& r = lane.records[n % kCapacity];
            if (r.qpc >= cutoff && r.qpc <= trigger.QuadPart) records.push_back(r);
        }
    }
    // Resume before sorting/formatting/disk I/O. Those costs are NOT in the saved window.
    s.paused.store(prior);
    resume.active = false;
    // Preserve actual per-lane recording order when adjacent QPC ticks tie.
    std::stable_sort(records.begin(), records.end(), [](auto& a, auto& b) { return a.qpc < b.qpc; });
    std::filesystem::create_directories(directory);
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t filename[180]{};
    swprintf_s(filename, L"ofxr-timing-v225-%04u%02u%02u-%02u%02u%02u-%03u-pid%lu-%lld.csv",
        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,pid,trigger.QuadPart);
    const auto path = directory / filename;
    std::ofstream out(path, std::ios::binary);
    out << "# schema=" << kSchema << " window_seconds=" << window_seconds << " pid=" << pid << " qpc_frequency=" << s.frequency
        << " trigger_qpc=" << trigger.QuadPart << " paused_before=" << prior
        << " unmapped_threads=" << s.unmapped_threads.load()
        << " lane_count=" << kLanes << "\n";
    // ASCII path sufficient as context; full executable can contain non-ASCII.
    out << "# executable=" << std::filesystem::path(s.executable).generic_string() << "\n";
    for (unsigned i=0;i<kLanes;++i) if(totals[i])
        out << "# lane=" << i << " thread=" << s.lanes[i].owner.load()
            << " total_events=" << totals[i] << " capacity=" << kCapacity
            << " oldest_retained_qpc=" << oldest[i]
            << " truncated_window=" << (totals[i]>kCapacity && oldest[i]>cutoff ? 1 : 0) << "\n";
    out << "qpc,thread,sequence,phase,operation,result,a,b,c\n";
    for (const auto& r : records) out << r.qpc << ',' << r.tid << ',' << r.sequence << ',' << r.phase
        << ',' << name(r.operation) << ',' << r.result << ',' << r.a << ',' << r.b << ',' << r.c << '\n';
    out.close();
    if (!out) { if(error) *error=L"Could not write RAM timing CSV."; return false; }
    if (saved) *saved=path;
    return true;
}
std::wstring command_ram_timing(const std::filesystem::path& directory, bool save, bool paused) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return L"Cannot enumerate capture processes.";
    unsigned count=0; std::wstring failures;
    PROCESSENTRY32W p{}; p.dwSize=sizeof(p);
    if(Process32FirstW(snapshot,&p)) do {
        try {
            std::wstring error;
            if(save ? save_ram_timing(p.th32ProcessID,directory,nullptr,&error)
                    : pause_ram_timing(p.th32ProcessID,paused)) ++count;
            if(!error.empty()) failures += error + L"\n";
        } catch (...) { failures += L"Capture failed (I/O or memory); retry.\n"; }
    } while(Process32NextW(snapshot,&p));
    CloseHandle(snapshot);
    return (count ? std::to_wstring(count) + (save ? L" capture(s) saved: " + directory.wstring()
        : paused ? L" RAM capture(s) paused" : L" RAM capture(s) resumed")
        : L"No active V225 RAM capture found.") + (failures.empty()? L"" : L"\n"+failures);
}
} // namespace xrfg
