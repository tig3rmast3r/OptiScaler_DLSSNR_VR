#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace xrfg {
class RamTimingCapture;

enum class BridgeFlightOperation : std::uint32_t {
    logger,
    negotiation,
    instance_create,
    instance_destroy,
    session_create,
    session_destroy,
    session_begin,
    session_end,
    application_wait_frame,
    application_begin_frame,
    application_end_frame,
    application_swapchain_acquire,
    application_swapchain_wait,
    application_swapchain_release,
    private_swapchain_acquire,
    private_swapchain_wait,
    private_swapchain_release,
    synthesis_initialize,
    synthesis_prime,
    synthesis_pair,
    downstream_first_end_frame,
    internal_wait_frame,
    internal_begin_frame,
    internal_end_frame,
    continuity_reset,
    gpu_drain,
    swapchain_create,
    swapchain_eligibility,
    projection_mapping,
    generation_prepare,
    session_binding,
    swapchain_image,
    d3d11_capture,
    d3d11_publish,
    runtime_identity,
    presenter_submission,
    // Correlates a presenter request with the original application display
    // time: a=request sequence, b=source XrTime, c=pair sequence (zero repeat).
    presenter_source,
    // Only when the application enabled the Win32 time conversion extension:
    // a=predicted XrTime, b=the same instant on this logger's microsecond
    // timeline, c=QPC frequency. Repeated occasionally to detect drift.
    presenter_clock_anchor,
    // result=1 if the app enabled the extension and the runtime exposed its
    // function, a=1 if the app requested it, b=1 if loading succeeded.
    presenter_clock_capability,
    presenter_queue_drop,
    presenter_transition,
    nvidia_gpu_stages,
    nvidia_gpu_total,
    // How long the presenter held itself back before starting a runtime frame
    // cycle, and how far it was from its own schedule when it did. Without
    // this the pace and the loop's other costs are indistinguishable inside
    // one gap between records.
    presenter_pace,
    synthesis_frame_start_wait,
    embedded_configuration,
    // When the GPU actually began and finished a pair's synthesis, on the
    // same timeline as every other record. Duration alone cannot say whether
    // the pixels existed when the presenter handed the frame over; only the
    // finish time against that submission can.
    synthesis_gpu_span,
    // The application asked for more frames than the presenter produced and
    // the virtual clock was held to the runtime's timeline instead of
    // stepping past it. a is how far the step overshot, in nanoseconds.
    virtual_clock_clamp,
    // A runtime session state transition, exactly as the application receives
    // it. result is the new XrSessionState; a is the session, b the event's
    // own time. A runtime that stops asking for frames says so here and
    // nowhere else the layer can see.
    session_state,
    // The once-per-pair hold releasing the application. result is the serial
    // surplus the release discarded - the presenter had run that many frames
    // beyond the two this hold is for, so the application had already missed
    // its slot and the hold throttled nothing. a is how long it waited in
    // microseconds, b the presenter's frame serial, c the serial the
    // application had been served to. Zero surplus is the hold working; a
    // surplus is the application running free, gated only by whatever it
    // blocks on next.
    presenter_pair_release,
    // Adaptive NVIDIA cadence: result=1 when a synthetic slot is budgeted;
    // a=application-end interval in ns, b=display period in ns,
    // c=accumulated synthetic credit in thousandths before consumption.
    // How hard the runtime-entry gate is actually working on a D3D11 session.
    // result is the number of entries in this window that had to wait, a the
    // mean wait and b the longest, both nanoseconds, c the longest any single
    // entry held the gate. Zero contention across a session means the two
    // threads were never inside the runtime together, so the gate is not what
    // is protecting it - the earlier device-section attempt reported exactly
    // that and it was read as a clean result rather than as a refutation.
    runtime_entry_section,
    // result is the HRESULT returned by history->capture; a is the
    // application swapchain, b the image index, c the captured slot on
    // success or UINT32_MAX on failure.
    history_capture,
    // Three-slot route: waits bracket the two producer handoffs. Handoff and
    // ready carry a=current source id, b=previous id. Present carries the same
    // identity, result=1 synthetic/0 real, c=runtime predicted display time.
    three_slot_worker_wait,
    three_slot_buffer_wait,
    three_slot_handoff,
    three_slot_gpu_ready,
    three_slot_present,
    // result=layer type, a=layer index, b=source swapchain,
    // c=0 supported, 1 unsupported structure, 2 missing release snapshot.
    composition_layer,
    // result=HRESULT, a=source, b=owned output, c=publication fence value.
    composition_publish,
    timing_worker_run,
    timing_slot_wait,
    timing_director_wait,
    timing_worker_publish,
    timing_runtime_gate,
    timing_worker_prepare,
    timing_synthesis_complete,
    timing_publication_complete,
    timing_aux_complete,
    timing_worker_build,
    gpu_history,
    gpu_synthesis,
    gpu_timing_error,
    luke_release,
    luke_filter,
    luke_cycle_wait,
    // Embedded-only diagnostics appended to preserve V223 operation IDs.
    fsr_eye_binding,
    fsr_eye_resources,
    fsr_eye_rects,
    fsr_eye_motion,
    fsr_eye_pose,
    fsr_eye_orientation,
    fsr_eye_fov,
    fsr_eye_clip_vertex,
    fsr_hudless_pair,
    fsr_context_lifecycle,
    fsr_submit_stage,
    fsr_recovery,
    // Luke/RealVR diagnostic-only contracts. These records connect the
    // post-ReShade colour capture to its physical OpenXR view, the DLSS guide
    // consumed with it, and the private swapchain eventually submitted.
    luke_guide_contract,
    luke_projection_contract,
    luke_output_contract,
    // V23286: immutable application frame serial through both runtime slots.
    luke_frame_contract,
    luke_pose_contract,
    // V23162: breadcrumb each PureDark AFW recording/submission boundary so a
    // process-ending plugin or driver fault retains the last completed stage.
    luke_afw_stage,
    gpu_fence_snapshot,
    gpu_device_status,
    // V30013: append-only RAM operation IDs; no image/pose scheduling changes.
    luke_inline_clock,       // result=XrResult, a=frame, b=XrTime, c=QPC (0 on failure)
    luke_physical_submit,    // result=boundary*100 or boundary*100+1(return), a=frame,b=time,c=result/count
    luke_publication_fence,  // result=boundary*100+resource*10+field(0 ready/1 copy target/2 completed)
    luke_publication_images, // a=frame,b=application swapchain,c=currentIndex<<32|syntheticIndex
    luke_publication_slots,  // a=frame,b=current private swapchain,c=synthetic private swapchain
    luke_input_guide,        // result=eye,a=frame,b=publication,c=epoch
    luke_input_camera,       // result=eye,a=frame,b=publication,c=guide camera hash
    luke_locate_context,    // result=XrResult,a=latest wait serial,b=requested time,c=space
    luke_locate_pose,       // result=view,a=latest wait serial,b=requested time,c=pose/FOV hash
    luke_inline_view,       // result=boundary*100+layer*10+view,a=frame,b=space,c=private swapchain
    luke_inline_pose_bits,  // result=viewCode*10+component,a=frame,b/c=raw float pairs
    luke_space_created,    // result=reference type(-1 action),a=space,b=session,c=0
    luke_space_locate,     // result=XrResult,a=sample ID,b=space,c=base space
    luke_space_time,       // result=location flags,a=sample ID,b=requested XrTime,c=latest app wait serial
    luke_render_camera,    // result=eye*100+matrix*10+row,a=guide publication,b/c=4 raw floats
    luke_temporal_pair,    // result=0,a=producer fence,b=previous capture,c=current capture; result1=destinations
    luke_temporal_pose,    // result=phase*100+view*10+pair,a=producer fence,b=capture,c=2 raw floats
    vram_usage, // D35: stage, current adapter usage, budget, subject.
    d3d11_bridge,
};

struct BridgeFlightToken {
    std::uint64_t sequence{};
    std::int64_t start_counter{};
};

class BridgeFlightLogger final {
public:
    BridgeFlightLogger() noexcept;
    ~BridgeFlightLogger();

    BridgeFlightLogger(const BridgeFlightLogger&) = delete;
    BridgeFlightLogger& operator=(const BridgeFlightLogger&) = delete;

    void initialize(const std::filesystem::path& module_directory) noexcept;
    void shutdown() noexcept;
    // Explicit diagnostic/teardown use only; never called on a render path.
    void flush() noexcept;
    [[nodiscard]] bool gpu_instrumentation_enabled() const noexcept { return gpu_instrumentation_enabled_; }

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool ram_enabled() const noexcept;
    [[nodiscard]] std::filesystem::path log_path() const;

    // Places a QueryPerformanceCounter value on this log's own timeline,
    // so a GPU timestamp calibrated to QPC can be compared directly with
    // the ms column of every other record.
    [[nodiscard]] std::int64_t microseconds_for_counter(
        std::int64_t counter) const noexcept;

    [[nodiscard]] BridgeFlightToken begin(
        BridgeFlightOperation operation,
        std::uint64_t a = 0,
        std::uint64_t b = 0,
        std::uint64_t c = 0) noexcept;

    void end(
        BridgeFlightToken token,
        BridgeFlightOperation operation,
        std::int64_t result,
        std::uint64_t a = 0,
        std::uint64_t b = 0,
        std::uint64_t c = 0) noexcept;

    void event(
        BridgeFlightOperation operation,
        std::int64_t result = 0,
        std::uint64_t a = 0,
        std::uint64_t b = 0,
        std::uint64_t c = 0) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<RamTimingCapture> ram_;
    bool gpu_instrumentation_enabled_{};
};

[[nodiscard]] BridgeFlightLogger& bridge_flight_logger() noexcept;
void initialize_bridge_flight_logger() noexcept;

} // namespace xrfg
