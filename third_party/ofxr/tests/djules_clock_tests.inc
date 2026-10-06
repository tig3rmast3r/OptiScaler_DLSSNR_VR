#ifdef OFXR_DJULES_COMPARISON
void djules_phase_acquisition_test() {
    using namespace std::chrono;
    const auto period = nanoseconds(11'111'111);
    check(calculate_djules_phase_step(nanoseconds(13'000'000), period) == nanoseconds(2'500'000),
        "extra compositor interval was folded away or corrected in the wrong direction");
    check(calculate_djules_phase_step(nanoseconds(2'000'000), period) == nanoseconds(-250'000),
        "late submission was shifted later");
    check(calculate_djules_phase_step(nanoseconds(3'250'000), period) == nanoseconds::zero() &&
        calculate_djules_phase_step(nanoseconds(2'750'000), period) == nanoseconds::zero(),
        "phase dead band caused continual jitter");
    check(calculate_djules_phase_step(nanoseconds(100'000'000), period) == period / 2 &&
        calculate_djules_phase_step(nanoseconds(-100'000'000), period) == -period / 2,
        "absurd phase sample escaped half-period bound");
    check(calculate_djules_phase_step(std::nullopt, period) == nanoseconds::zero() &&
        calculate_djules_phase_step(nanoseconds(13'000'000), nanoseconds::zero()) == nanoseconds::zero(),
        "unavailable/invalid clock moved phase");
    DjulesAbsoluteSchedule grid;
    const auto origin = steady_clock::time_point(milliseconds(1000));
    const auto slot = grid.claim(origin, period);
    grid.submitted(slot, period, origin + milliseconds(1), nanoseconds(2'500'000));
    check(grid.claim(origin + milliseconds(2), period) == origin + period + nanoseconds(2'500'000),
        "acquired phase was overwritten by next absolute-grid advance");
    std::cout << "Djules phase: unwrapped error, real-frame acquisition law, dead band and bounded grid correction\n";
}

void djules_absolute_grid_test() {
    using namespace std::chrono;
    DjulesAbsoluteSchedule grid;
    const auto origin = steady_clock::time_point(milliseconds(1000));
    const auto period = nanoseconds(milliseconds(10));
    check(grid.claim(origin, period) == origin, "absolute grid seed");
    grid.submitted(origin, period, origin + milliseconds(1));
    check(grid.claim(origin + milliseconds(9), period) == origin + milliseconds(10),
        "handover loop cost compounded into next slot");
    check(grid.claim(origin + milliseconds(11), period) == origin + milliseconds(10),
        "ordinary late synthetic reset the absolute grid");
    grid.submitted(origin + milliseconds(10), period, origin + milliseconds(25));
    check(grid.claim(origin + milliseconds(26), period) == origin + milliseconds(30),
        "slow handover did not skip to an aligned slot with quarter-period lead");
    grid.submitted(origin + milliseconds(30), period, origin + seconds(2));
    check(grid.claim(origin + seconds(2), period) == origin + milliseconds(2010),
        "long stall produced catch-up burst or lost absolute phase");
    check(grid.claim(origin + seconds(2), nanoseconds(milliseconds(12))) == origin + seconds(2),
        "actual refresh mode change did not reseed grid");
    grid.submitted(origin, nanoseconds::zero(), origin + milliseconds(1));
    check(grid.claim(origin + milliseconds(2), nanoseconds::zero()) == origin + milliseconds(2),
        "invalid period divided by zero or retained stale deadline");
    check(djules_handover_floor(origin + milliseconds(2), origin, period) == origin + milliseconds(5),
        "non-SteamVR floor did not hold half a period after End return");
    check(djules_handover_floor(origin + milliseconds(7), origin, period) == origin + milliseconds(7),
        "already paced runtime acquired an additional clock");
    check(djules_handover_floor(origin, {}, period) == origin &&
        djules_handover_floor(origin, origin, nanoseconds::zero()) == origin,
        "first/invalid floor acquired stale deadline");
    std::cout << "Djules absolute grid: fixed slots, loop jitter, aligned stall recovery and no catch-up burst\n";
}

void djules_triple_buffer_test() {
    xrfg::DjulesOutputBuffer<int> buffer; buffer.configure(3);
    check(buffer.capacity()==4 && buffer.push_real(1),"3X prime capacity");
    for(int i=0;i<3;++i) check(buffer.shift_empty(),"3X prime shift");
    check(buffer.slot(0)->value==1 && !buffer.releasable(),"3X lost retained endpoint");
    check(buffer.push_real(2) && !buffer.can_accept_real(),"3X admission unbounded");
    check(!buffer.push_synthetic_pair(11,12),"3X replaced in-flight A before End acknowledgement");
    check(buffer.release_front() && buffer.push_synthetic_pair(11,12),"3X pair did not follow A End");
    check(buffer.slot(0)->value==11 && buffer.release_front() &&
        buffer.slot(0)->value==12 && buffer.release_front(),"3X first/second synthetic order");
    check(buffer.slot(0)->value==2 && !buffer.releasable() && buffer.can_accept_real(),"3X B was repeated or lost before C admission");
    check(buffer.push_real(3) && buffer.release_front() && buffer.push_synthetic_pair(21,22),"3X next-pair ownership");
    buffer.clear(); buffer.configure(2);
    check(buffer.capacity()==3 && buffer.push_real(4) && buffer.shift_empty() && buffer.shift_empty(),"2X comparison capacity changed");
    std::cout << "DJ 3X: bounded four descriptors, A/S1/S2/B FIFO, End acknowledgements, 2X invariant\n";
}

void djules_readiness_test() {
    using namespace std::chrono;
    const auto p=nanoseconds(milliseconds(10));
    check(hold_djules_synthetic(milliseconds(9),p,true,true,false),"deep synthetic lost minimum age");
    check(!hold_djules_synthetic(milliseconds(10),p,true,true,false),"ready owned output held past deadline");
    check(hold_djules_synthetic(milliseconds(29),p,false,true,false),"unwritten output lost readiness window");
    check(!hold_djules_synthetic(milliseconds(30),p,false,true,false),"readiness hold exceeded three periods");
    check(!hold_djules_synthetic(milliseconds(1),p,false,false,false),"startup repeated missing/unowned image");
    check(!hold_djules_synthetic(milliseconds(1),p,false,true,true),"pipelined game inherited deep hold");
    check(!hold_djules_synthetic(milliseconds(1),nanoseconds::zero(),false,true,false),"invalid period held output");
    std::cout << "DJ deep readiness: owned repeat only, one-period age, three-period unwritten cap, pipeline guard\n";
}

void djules_inline_policy_test() {
    auto s = session();
    s->menu_enabled = true; s->nvidia_new_route = true; s->luke_afw_enabled = false;
    s->graphics_binding = SessionGraphicsBinding::d3d12;
    s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
    s->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::nvidia;
    s->dispatch->inline_unless_pipelined = true;
    s->minimum_runtime_display_period = 10'000'000;
    check(djules_inline_runtime(*s) && !three_slot_generation_session(s) && !queued_nvidia_session(s),
        "VD/Pimax non-pipelined session acquired unsolicited director");
    for (int i=0;i<60;++i)
        check(!inline_pair_lands_in_one_scanout(s, std::chrono::microseconds(10)),
            "stall-dependent bunched detector promoted VD/Pimax");
    check(!presenter_forbidden(*s), "runtime policy blocked genuine overlapping-Wait detection");
    s->pipelined_presenter_mode = true;
    check(!djules_inline_runtime(*s) && queued_nvidia_session(s),
        "genuinely pipelined VD/Pimax could not acquire frame-call owner");
    s->pipelined_presenter_mode = false;
    s->nvidia_new_route = false;
    check(!djules_inline_runtime(*s), "reference inherited optional inline runtime policy");
    s->nvidia_new_route = true; s->luke_afw_enabled = true;
    check(!djules_inline_runtime(*s) && three_slot_generation_session(s), "AFW lost accepted director");
    std::cout << "DJ VD/Pimax: fixed inline shape, genuine pipeline override, reference/AFW guards\n";
}

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
