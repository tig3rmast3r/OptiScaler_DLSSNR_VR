#include "xrfg/embedded_control.hpp"
#include "xrfg/implicit_layer.hpp"
#include <windows.h>
#include <map>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cwchar>

namespace xrfg::embedded {
namespace {
std::atomic_bool g_layer_active{false};
std::filesystem::path directory() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&directory), &module);
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    return std::filesystem::path(path).parent_path();
}
const std::filesystem::path& ini() { static auto path = directory() / L"ofxr_bridge.ini"; return path; }
bool luke_afw_requires_dlss_guides() {
    return GetPrivateProfileIntW(
        L"experimental", L"luke_afw", 0, ini().c_str()) != 0;
}
struct Receipt { std::uint64_t revision{}; bool enabled{}; long error{}; };
struct Control {
    std::mutex mutex;
    Settings settings;
    std::uint64_t revision{1}, next_id{1};
    std::map<std::uint64_t, Receipt> sessions;
    Control() {
        const auto options = implicit_layer::read_nvidia_options(directory());
        settings.backend = static_cast<int>(implicit_layer::read_flow_backend(directory()));
        wchar_t route[16]{};
        GetPrivateProfileStringW(L"ofxr", L"nvidia_route", L"legacy", route, 16, ini().c_str());
        if (settings.backend == 1 && _wcsicmp(route, L"new") == 0) settings.backend = 2;
        settings.preset = static_cast<int>(options.preset);
        settings.scale = static_cast<int>(options.input_scale);
        settings.backward = options.bidirectional;
        settings.enabled = GetPrivateProfileIntW(L"ofxr", L"enabled", 1, ini().c_str()) != 0;
        wchar_t motion[16]{};
        GetPrivateProfileStringW(L"ofxr", L"motion_vectors", L"off", motion, 16, ini().c_str());
        settings.motion_vectors = _wcsicmp(motion, L"dlss") == 0 ? 1 : 0;
        if (luke_afw_requires_dlss_guides() && !settings.motion_vectors) {
            settings.motion_vectors = 1;
            WritePrivateProfileStringW(
                L"ofxr", L"motion_vectors", L"dlss", ini().c_str());
        }
        wchar_t generation[16]{};
        GetPrivateProfileStringW(L"ofxr", L"frame_generation", L"ofxr", generation, 16, ini().c_str());
        settings.frame_generation = _wcsicmp(generation, L"fsr3") == 0 ? 1 : 0;
    }
};
Control& control() { static Control state; return state; }

float read_float(const wchar_t* section, const wchar_t* key,
    float fallback) {
    wchar_t fallback_text[32]{};
    swprintf_s(fallback_text, L"%.3f", fallback);
    wchar_t value[32]{};
    GetPrivateProfileStringW(
        section, key, fallback_text, value, 32, ini().c_str());
    wchar_t* end{};
    const float parsed = std::wcstof(value, &end);
    return end != value && std::isfinite(parsed) ? parsed : fallback;
}

struct AfwControl {
    std::mutex mutex;
    AfwSnapshot snapshot;
    AfwControl() {
        auto& settings = snapshot.desired;
        settings.mode = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"experimental", L"luke_afw_mode", 3, ini().c_str())), 0, 3);
        settings.debug = GetPrivateProfileIntW(
            L"experimental", L"luke_afw_debug", 0,
            ini().c_str()) != 0;
        settings.clear_before_warping = GetPrivateProfileIntW(
            L"experimental", L"luke_afw_clear_before_warping", 0,
            ini().c_str()) != 0;
        settings.ignore_motion_threshold = std::clamp(read_float(
            L"experimental", L"luke_afw_ignore_motion_threshold", 2.5F),
            0.0F, 64.0F);
        settings.depth_dilate_mode = std::clamp(static_cast<int>(
            GetPrivateProfileIntW(L"experimental", L"luke_afw_depth_dilate_mode",
                1, ini().c_str())), 0, 2);
    }
};
AfwControl& afw_control() { static AfwControl state; return state; }
bool write(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    return WritePrivateProfileStringW(section, key, value, ini().c_str()) != FALSE;
}
constexpr const wchar_t* positions[]{L"off", L"upper_left", L"upper_right", L"lower_left", L"lower_right"};
}
Snapshot snapshot() {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    Snapshot s; s.desired = c.settings; s.revision = c.revision;
    s.sessions = static_cast<unsigned>(c.sessions.size());
    for (auto& [id, r] : c.sessions) {
        if (r.revision != c.revision) ++s.pending;
        else if (r.error) { ++s.errors; s.last_error = r.error; }
        else if (r.enabled) ++s.ready;
        else ++s.bypass;
    }
    return s;
}
bool request(Settings s) {
    if (s.backend < 0 || s.backend > 2 || s.preset < 0 || s.preset > 2 ||
        s.scale < 0 || s.scale > 2 || s.motion_vectors < 0 || s.motion_vectors > 1 ||
        s.frame_generation < 0 || s.frame_generation > 1) return false;
    // This UI option controls OFXR's temporal guide choice, but Luke AFW
    // independently needs the NGX guide publication even with color-only OFXR.
    if (luke_afw_requires_dlss_guides()) s.motion_vectors = 1;
    auto& c = control(); std::scoped_lock lock(c.mutex);
    const auto old = c.settings;
    bool ok = true;
    // Do not rewrite diagnostics, comments, or unrelated configuration.
    if (old.enabled != s.enabled) ok &= write(L"ofxr", L"enabled", s.enabled ? L"1" : L"0");
    if (old.backend != s.backend) ok &= write(L"ofxr", L"backend", s.backend ? L"nvidia" : L"fidelityfx");
    if (old.backend != s.backend) ok &= write(L"ofxr", L"nvidia_route", s.backend == 2 ? L"new" : L"legacy");
    const wchar_t* presets[]{L"slow", L"medium", L"fast"};
    const wchar_t* scales[]{L"100", L"75", L"50"};
    if (old.preset != s.preset) ok &= write(L"ofxr", L"nvidia_preset", presets[s.preset]);
    if (old.scale != s.scale) ok &= write(L"ofxr", L"nvidia_input_scale", scales[s.scale]);
    if (old.backward != s.backward) ok &= write(L"ofxr", L"nvidia_bidirectional", s.backward ? L"1" : L"0");
    if (old.motion_vectors != s.motion_vectors) ok &= write(L"ofxr", L"motion_vectors", s.motion_vectors ? L"dlss" : L"off");
    if (old.frame_generation != s.frame_generation) ok &= write(L"ofxr", L"frame_generation", s.frame_generation ? L"fsr3" : L"ofxr");
    if (old != s) { c.settings = s; ++c.revision; }
    return ok;
}

AfwSnapshot afw_snapshot() {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    return c.snapshot;
}

bool request_afw(AfwSettings settings) {
    if (settings.mode < 0 || settings.mode > 3 ||
        settings.depth_dilate_mode < 0 || settings.depth_dilate_mode > 2 ||
        !std::isfinite(settings.ignore_motion_threshold) ||
        settings.ignore_motion_threshold < 0.0F ||
        settings.ignore_motion_threshold > 64.0F) return false;
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    const auto old = c.snapshot.desired;
    bool ok = true;
    if (old.mode != settings.mode) {
        const wchar_t* modes[]{L"0", L"1", L"2", L"3"};
        ok &= write(L"experimental", L"luke_afw_mode", modes[settings.mode]);
    }
    if (old.debug != settings.debug)
        ok &= write(L"experimental", L"luke_afw_debug",
            settings.debug ? L"1" : L"0");
    if (old.clear_before_warping != settings.clear_before_warping)
        ok &= write(L"experimental", L"luke_afw_clear_before_warping",
            settings.clear_before_warping ? L"1" : L"0");
    if (old.ignore_motion_threshold != settings.ignore_motion_threshold) {
        wchar_t value[32]{};
        swprintf_s(value, L"%.3f", settings.ignore_motion_threshold);
        ok &= write(L"experimental", L"luke_afw_ignore_motion_threshold", value);
    }
    if (old.depth_dilate_mode != settings.depth_dilate_mode) {
        const wchar_t* modes[]{L"0", L"1", L"2"};
        ok &= write(L"experimental", L"luke_afw_depth_dilate_mode",
            modes[settings.depth_dilate_mode]);
    }
    if (!(old == settings)) {
        c.snapshot.desired = settings;
        ++c.snapshot.revision;
    }
    return ok;
}

bool toggle_afw_debug() {
    auto settings = afw_snapshot().desired;
    settings.debug = !settings.debug;
    return request_afw(settings);
}

void report_afw_ingress(bool published, bool camera) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.ingress_frames;
    if (published) ++c.snapshot.published_ingress_frames;
    if (!camera) ++c.snapshot.missing_camera_ingress_frames;
}

void report_afw_guide_rejection(std::uint32_t mask) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.rejected_guide_pairs;
    c.snapshot.last_guide_rejection = mask;
}

void report_afw_prepare(std::int64_t reason) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.projection_frames;
    if (reason == 0) ++c.snapshot.ready_frames;
    else if (reason == 5) ++c.snapshot.missing_capture_frames;
    else if (reason == 18) ++c.snapshot.busy_frames;
    else ++c.snapshot.failed_frames;
}

void report_afw_evaluation(std::uint64_t publication,
    std::uint32_t source_eye, std::uint32_t motion_width,
    std::uint32_t motion_height, std::uint32_t output_width,
    std::uint32_t output_height, float motion_scale_x,
    float motion_scale_y, long result) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.evaluations;
    if (result >= 0) ++c.snapshot.successful_evaluations;
    c.snapshot.last_publication = publication;
    c.snapshot.last_source_eye = source_eye;
    c.snapshot.last_motion_width = motion_width;
    c.snapshot.last_motion_height = motion_height;
    c.snapshot.last_output_width = output_width;
    c.snapshot.last_output_height = output_height;
    c.snapshot.last_motion_scale_x = motion_scale_x;
    c.snapshot.last_motion_scale_y = motion_scale_y;
    c.snapshot.last_result = result;
}
std::uint64_t attach() { auto& c = control(); std::scoped_lock lock(c.mutex); auto id = c.next_id++; c.sessions.emplace(id, Receipt{}); return id; }
void detach(std::uint64_t id) { auto& c = control(); std::scoped_lock lock(c.mutex); c.sessions.erase(id); }
void applied(std::uint64_t id, std::uint64_t revision, bool enabled, long error) {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    if (auto it = c.sessions.find(id); it != c.sessions.end()) it->second = {revision, enabled, error};
}
void set_layer_active(bool active) noexcept { g_layer_active.store(active, std::memory_order_release); }
bool layer_active() noexcept { return g_layer_active.load(std::memory_order_acquire); }
bool logging_setting() { return GetPrivateProfileIntW(L"diagnostics", L"logging_enabled", 0, ini().c_str()) != 0; }
bool set_logging_setting(bool enabled) { return write(L"diagnostics", L"logging_enabled", enabled ? L"1" : L"0"); }
bool cadence_setting() { return GetPrivateProfileIntW(L"diagnostics", L"cadence_logging_enabled", 0, ini().c_str()) != 0; }
bool set_cadence_setting(bool enabled) { return write(L"diagnostics", L"cadence_logging_enabled", enabled ? L"1" : L"0"); }
bool marker_setting() { return GetPrivateProfileIntW(L"diagnostics", L"synthetic_marker_enabled", logging_setting() || cadence_setting(), ini().c_str()) != 0; }
bool set_marker_setting(bool enabled) { return write(L"diagnostics", L"synthetic_marker_enabled", enabled ? L"1" : L"0"); }
int overlay_setting() {
    wchar_t value[32]{}; GetPrivateProfileStringW(L"overlay", L"position", L"upper_right", value, 32, ini().c_str());
    for (int i = 0; i < 5; ++i) if (_wcsicmp(value, positions[i]) == 0) return i;
    return 2;
}
bool set_overlay_setting(int position) { return position >= 0 && position < 5 && write(L"overlay", L"position", positions[position]); }
}

// Private, versioned POD ABI for the combined-module host regression harness.
extern "C" __declspec(dllexport) int OFXR_EmbeddedRequestV1(int enabled, int backend, int preset, int scale, int backward) noexcept {
    try { return xrfg::embedded::request({enabled != 0, backend, preset, scale, backward != 0, 0, 0}) ? 1 : 0; }
    catch (...) { return 0; }
}
