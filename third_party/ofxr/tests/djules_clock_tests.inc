#ifdef OFXR_DJULES_COMPARISON
void djules_display_clock_test() {
    auto s = session();
    s->presenter_active = false;
    s->graphics_binding = SessionGraphicsBinding::d3d12;
    s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
    s->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
    s->nvidia_new_route = true;
    s->luke_afw_enabled = false;
    s->dispatch->steamvr_runtime = true;
    s->presenter_display_period = 27'777'778;
    check(adopt_djules_display_period(*s, std::chrono::nanoseconds(13'888'889)) &&
        s->presenter_display_period == 13'888'889, "throttled prediction became HMD period");
    check(adopt_djules_display_period(*s, std::chrono::nanoseconds(11'111'111)) &&
        adopt_djules_display_period(*s, std::chrono::nanoseconds(13'888'889)),
        "actual HMD mode change was pinned to the old minimum");
    for (const auto invalid : {1LL, 1'999'999LL, 50'000'001LL})
        check(!adopt_djules_display_period(*s, std::chrono::nanoseconds(invalid)) &&
            s->presenter_display_period == 13'888'889, "invalid display period changed cadence");
    check(!adopt_djules_display_period(*s, std::nullopt), "missing compositor period accepted");
    s->nvidia_new_route = false;
    check(!adopt_djules_display_period(*s, std::chrono::nanoseconds(8'333'333)), "legacy route adopted optional clock");
    s->nvidia_new_route = true;
    s->luke_afw_enabled = true;
    check(!adopt_djules_display_period(*s, std::chrono::nanoseconds(8'333'333)), "AFW pair changed clock policy");
    s->luke_afw_enabled = false;
    s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::fsr3;
    check(!adopt_djules_display_period(*s, std::chrono::nanoseconds(8'333'333)), "FSR changed clock policy");
    s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
    s->dispatch->steamvr_runtime = false;
    check(!adopt_djules_display_period(*s, std::chrono::nanoseconds(8'333'333)) &&
        s->presenter_display_period == 13'888'889, "non-SteamVR adopted compositor clock");
    std::cout << "Djules display clock: actual HMD period, mode changes, invalid bounds and reference/AFW/FSR guards\n";
}
#endif
