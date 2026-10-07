#pragma once
#include <cstdint>

namespace xrfg::embedded {
// Set by successful OpenXR negotiation; shared by layer and host publication.
void set_layer_active(bool active) noexcept;
bool layer_active() noexcept;
// CPU-only interface: no NGX/FFX structures cross the host/backend ABI boundary.
struct Settings {
    bool enabled{true};
    int backend{}; // 0 FidelityFX, 1 NVIDIA legacy, 2 NVIDIA new, 3 NVIDIA djules
    int preset{1}; // 0 slow, 1 medium, 2 fast
    int scale{2}; // 0 full, 1 three-quarter, 2 half
    bool backward{};
    int motion_vectors{}; // 0 optical flow, 1 DLSS ingress
    int frame_generation{}; // 0 OFXR synthesis, 1 FSR 3 frame generation
    bool operator==(const Settings&) const = default;
};
struct Snapshot {
    Settings desired{};
    std::uint64_t revision{1};
    unsigned sessions{}, pending{}, bypass{}, ready{}, errors{};
    long last_error{};
};

// Laboratory-only controls for the embedded Luke Ross / PureDark AFW path.
// This has an independent revision so changing a warp interpretation never
// drains or rebuilds the ordinary OFXR synthesis backend.
struct AfwSettings {
    int mode{3}; // 0 off, 1 alternate eye, 2 previous frame, 3 combined
    bool debug{};
    bool clear_before_warping{};
    float ignore_motion_threshold{2.5F};
    int depth_dilate_mode{1}; // 0 off, 1 horizontal 1x3, 2 square 3x3
    bool operator==(const AfwSettings&) const = default;
};

struct AfwSnapshot {
    AfwSettings desired{};
    std::uint64_t revision{1};
    std::uint64_t projection_frames{};
    std::uint64_t ready_frames{};
    std::uint64_t missing_capture_frames{};
    std::uint64_t busy_frames{};
    std::uint64_t failed_frames{};
    std::uint64_t evaluations{};
    std::uint64_t successful_evaluations{};
    std::uint64_t ingress_frames{};
    std::uint64_t published_ingress_frames{};
    std::uint64_t missing_camera_ingress_frames{};
    std::uint64_t rejected_guide_pairs{};
    std::uint32_t last_guide_rejection{};
    std::uint64_t last_publication{};
    std::uint32_t last_source_eye{};
    std::uint32_t last_motion_width{};
    std::uint32_t last_motion_height{};
    std::uint32_t last_output_width{};
    std::uint32_t last_output_height{};
    float last_motion_scale_x{};
    float last_motion_scale_y{};
    long last_result{};
};

Snapshot snapshot();
bool request(Settings settings); // persists only owned keys; false means INI write failed
AfwSnapshot afw_snapshot();
bool request_afw(AfwSettings settings);
bool toggle_afw_debug();
void report_afw_prepare(std::int64_t reason);
void report_afw_ingress(bool published, bool camera);
void report_afw_guide_rejection(std::uint32_t mask);
void report_afw_evaluation(std::uint64_t publication,
    std::uint32_t source_eye, std::uint32_t motion_width,
    std::uint32_t motion_height, std::uint32_t output_width,
    std::uint32_t output_height, float motion_scale_x,
    float motion_scale_y, long result);
std::uint64_t attach();
void detach(std::uint64_t id);
void applied(std::uint64_t id, std::uint64_t revision, bool enabled, long error);
bool logging_setting();
bool set_logging_setting(bool enabled); // next process: logger is initialized once
bool cadence_setting();
bool set_cadence_setting(bool enabled);
bool marker_setting();
bool set_marker_setting(bool enabled);
int overlay_setting();
bool set_overlay_setting(int position); // existing overlay polls its INI live

// The imported DJ deep policy is session-bound, as in the upstream tray.
// Editing it saves the next session's preference, without a renderer revision.
struct DjulesPipelineSnapshot {
    bool prefer_fps{true};
    bool triple_frame_gen{};
    bool pending{};
};
DjulesPipelineSnapshot djules_pipeline_snapshot();
bool set_djules_prefer_fps(bool enabled);
bool set_djules_triple_frame_gen(bool enabled);
void register_djules_pipeline(std::uint64_t id, bool prefer_fps, bool triple = false);
}
