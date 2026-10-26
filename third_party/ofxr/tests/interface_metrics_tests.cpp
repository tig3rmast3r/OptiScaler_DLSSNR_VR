#include "xrfg/interface_metrics.hpp"
#include "xrfg/fps_overlay_model.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstring>
using namespace xrfg;
namespace {
unsigned checks{};
void check(bool pass, const char* text) { ++checks; if (!pass) throw std::runtime_error(text); }
void timing() {
    InterfaceTimingWindow window;
    const InterfaceFrameStamp frame{1, 100'000'000, 80'000'000};
    check(!window.submitted(frame, 110'000'000, false,true,false,true), "failed submit does not consume identity");
    check(!window.submitted(frame, 110'000'000, true,false,false,true), "empty real excluded");
    check(!window.submitted(frame, 110'000'000, true,true,true,true), "synthetic excluded");
    check(!window.submitted(frame, 110'000'000, true,true,false,false), "repeat/native copied output excluded");
    check(window.submitted(frame, 150'000'000, true,true,false,true, 145'000'000), "pipeline hold retained on first successful real");
    check(!window.submitted(frame, 160'000'000, true,true,false,true), "identity cannot double count");
    InterfaceTelemetry m{}; window.snapshot(160'000'000,m);
    check(m.delay_valid && m.ofxr_delay_ms == 50 && m.cpu_valid && m.cpu_render_ms == 20, "real entry and begin own their exact interval");
    check(m.presentation_valid && m.presentation_ms == -5, "late presentation retains negative sign");
    check(window.submitted({2,180'000'000,0}, 200'000'000,true,true,false,true), "missing clock converter remains eligible for OFXR time");
    window.snapshot(210'000'000,m);
    check(m.ofxr_delay_ms == 35 && m.presentation_ms == -5 && m.cpu_render_ms == 20, "median ignores unavailable sources independently");
    window.snapshot(1'210'000'001,m);
    check(!m.delay_valid && !m.presentation_valid && !m.cpu_valid, "one-second sample window expires");
    check(!window.submitted(frame, 1'220'000'000,true,true,false,true), "expired frame identity still cannot be re-counted");
    check(!window.submitted({3,300,0},299,true,true,false,true), "clock regression rejected");
    check(!window.submitted({4,0,0},400,true,true,false,true), "no entry clock yields no latency");
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
    const auto converted=interface_qpc_ns(qpc.QuadPart), now=interface_clock_ns();
    check(converted>0 && now>=converted && now-converted<50'000'000, "converted predicted display uses same QPC domain");

}
void complete_cycle() {
    auto cycle=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);
    cycle->call(100'000'000,110'000'000); // Wait includes runtime/OFXR waits.
    cycle->call(115'000'000,117'000'000); // Begin; game gaps remain outside OFXR time.
    cycle->call(120'000'000,124'000'000);
    cycle->call(122'000'000,126'000'000); // Concurrent eyes: union, not sum.
    cycle->call(140'000'000,165'000'000); // End includes tail after actual submission.
    InterfaceTimingWindow window;
    InterfaceFrameStamp frame{10,140'000'000,117'000'000,cycle};
    check(window.submitted(frame,150'000'000,true,true,false,true,180'000'000,155'000'000), "real acceptance retains same Wait ticket");
    InterfaceTelemetry out{};window.snapshot(160'000'000,out);
    check(!out.cycle_valid && !out.ofxr_time_valid, "producer still inside End is not a completed sample");
    cycle->producer_returned(165'000'000);window.snapshot(170'000'000,out);
    check(out.cycle_valid && out.cycle_length_ms==65 && out.ofxr_time_ms==43, "production plus waits include post-submit producer tail; eye overlaps counted once");
    check(out.cycle_source==InterfaceSource::frame_cycle_qpc && out.ofxr_time_source==InterfaceSource::producer_call_qpc && !out.gpu_valid, "CPU boundary never claims GPU completion");
    auto next=std::make_shared<InterfaceCycle>(151'000'000,151'000'000);
    next->call(151'000'000,171'000'000);next->producer_returned(195'000'000);
    window.snapshot(180'000'000,out);
    check(out.cycle_length_ms==65 && out.ofxr_time_ms==43, "overlapping next Wait cannot overwrite retained previous frame");
    auto held=std::make_shared<InterfaceCycle>(200'000'000,200'000'000);
    held->call(200'000'000,205'000'000);held->call(240'000'000,245'000'000);
    held->producer_returned(245'000'000);
    InterfaceTimingWindow asynchronous;
    check(asynchronous.submitted({11,240'000'000,205'000'000,held},280'000'000,true,true,false,true,0,285'000'000), "held real accepted after producer returns");
    asynchronous.snapshot(290'000'000,out);
    check(out.cycle_length_ms==85 && out.ofxr_time_ms==10, "director retention contributes full age even outside producer calls");
    check(!asynchronous.submitted({11,240'000'000,205'000'000,held},300'000'000,true,true,false,true), "held real cannot double count");
    asynchronous.snapshot(1'290'000'001,out);
    check(!out.cycle_valid && !out.ofxr_time_valid, "new counters expire after one second");
    InterfaceCycle overflow(1);
    for(std::int64_t i=0;i<65;++i)overflow.call(1+i*2,2+i*2);
    overflow.producer_returned(200);float a{},b{};
    check(!overflow.read(201,a,b), "bounded interval overflow is N/A rather than an invented partial total");
    InterfaceCycle regression(100);regression.call(99,105);regression.producer_returned(200);
    check(!regression.read(201,a,b), "clock/identity regression has no timing");
}
void native_delivery_cycle() {
    auto c=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);
    c->call(100'000'000,103'000'000);c->producer_returned(109'000'000);
    c->synthetic_accepted(103'500'000,11'111'111);
    float ms{},calls{};
    check(c->read_estimated(109'000'000,ms) && ms<15,"reproduce old virtual15ms estimate");
    auto generated=std::make_shared<InterfaceDeliveryReceipt>(true);
    auto real=std::make_shared<InterfaceDeliveryReceipt>(false);
    c->attach_native_delivery(generated);c->attach_native_delivery(real);
    InterfaceTimingWindow window;InterfaceFrameStamp stamp{607,105'000'000,100'000'000,c};
    check(window.submitted(stamp,109'000'000,true,true,false,true,110'000'000,109'000'000),"retain accepted virtual identity while native delivery is pending");
    InterfaceTelemetry pending;window.snapshot(110'000'000,pending);
    check(!pending.cycle_valid && !pending.cycle_estimated_valid && !pending.delay_valid,"queued virtual success cannot publish partial native timing");
    generated->completed(true,145'000'000,146'000'000,155'000'000);
    real->completed(true,156'111'111,157'000'000,166'111'111);
    check(c->read(109'000'000,calls,ms) && calls==3 && ms==57,"native full cycle includes47ms beyond virtual return, producer call total unchanged");
    check(c->read_estimated(109'000'000,ms),"actual real prediction is valid independently of generation confirmation");
    generated->synthetic_confirmed=true;
    check(c->read_estimated(109'000'000,ms) && std::abs(ms-66.111111F)<.001F,"native predicted real slot counted once, no additional refresh");
    InterfaceTelemetry actual;window.snapshot(160'000'000,actual);
    check(actual.cycle_source==InterfaceSource::native_frame_cycle_qpc && actual.cycle_estimated_source==InterfaceSource::native_predicted_display_qpc,"physical observations have distinct source labels");
    check(std::abs(actual.ofxr_delay_ms-51.111111F)<.001F && std::abs(actual.presentation_ms-10.F)<.001F,"delay and presentation use real native handoff rather than virtual submission");
    auto failed=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);failed->producer_returned(109'000'000);
    auto rejected=std::make_shared<InterfaceDeliveryReceipt>(false);failed->attach_native_delivery(rejected);
    rejected->completed(false,150'000'000,151'000'000,160'000'000);
    check(!failed->read(109'000'000,calls,ms),"failed or empty physical End never becomes a delivered-cycle sample");
    auto no_clock=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);no_clock->producer_returned(109'000'000);
    auto no_clock_real=std::make_shared<InterfaceDeliveryReceipt>(false);no_clock->attach_native_delivery(generated);no_clock->attach_native_delivery(no_clock_real);
    no_clock_real->completed(true,156'111'111,157'000'000,0);
    check(no_clock->read(109'000'000,calls,ms) && ms==57 && !no_clock->read_estimated(109'000'000,ms),"native handoff remains measurable without inventing an unknown display target");
    auto triple=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);triple->producer_returned(109'000'000);
    auto second=std::make_shared<InterfaceDeliveryReceipt>(true);second->synthetic_confirmed=true;second->completed(true,156'111'111,157'000'000,166'111'111);
    auto triple_real=std::make_shared<InterfaceDeliveryReceipt>(false);triple_real->completed(true,167'222'222,168'000'000,177'222'222);
    triple->attach_native_delivery(generated);triple->attach_native_delivery(second);triple->attach_native_delivery(triple_real);
    check(triple->read_estimated(109'000'000,ms) && std::abs(ms-77.222222F)<.001F,"3X uses actual real target, two delivered synthetic slots are not added twice");
    window.snapshot(1'200'000'000,actual);check(!actual.cycle_valid,"native observations expire normally");
}
void stage_partition_and_baseline() {
    auto c=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);c->producer_returned(125'000'000);
    c->call(100'000'000,105'000'000);c->call(110'000'000,112'000'000);
    c->call(111'000'000,114'000'000); // overlapping eye calls count once
    c->call(120'000'000,125'000'000); // End preparation is outside the game phase
    auto real=std::make_shared<InterfaceDeliveryReceipt>(false);
    c->attach_native_delivery(real);real->completed(true,130'000'000,131'000'000,145'000'000,129'000'000);
    InterfaceTimingWindow window;
    check(window.submitted({608,120'000'000,105'000'000,c},123'000'000,true,true,false,true,0,125'000'000),"disabled baseline source identity");
    InterfaceTelemetry m;window.snapshot(150'000'000,m);
    check(m.cycle_estimated_valid && m.cycle_estimated_ms==45 && m.stages_valid,"baseline cycle needs no synthetic frame");
    check(m.game_time_valid && m.game_time_ms==20 && m.generation_time_ms==0 && m.delivery_time_ms==25,"disabled baseline partitions the engine source handoff without relabeling pre-handoff waits");
    auto generated=std::make_shared<InterfaceDeliveryReceipt>(true);c->attach_native_delivery(generated);
    generated->completed(true,127'000'000,128'000'000,138'000'000,126'000'000);
    window.snapshot(150'000'000,m);check(!m.stages_valid && m.cycle_estimated_valid,"unconfirmed generated pixels cannot supply a preparation boundary");
    generated->synthetic_confirmed=true;window.snapshot(150'000'000,m);
    check(m.stages_valid && m.game_time_ms==20 && m.generation_time_ms==6 && m.delivery_time_ms==19,"native preparation and delivery partition one exact real cycle");
    auto extra=std::make_shared<InterfaceDeliveryReceipt>(true);extra->synthetic_confirmed=true;
    extra->completed(true,129'000'000,130'000'000,140'000'000,128'000'000);c->attach_native_delivery(extra);
    window.snapshot(150'000'000,m);
    check(m.stages_valid && m.generation_time_ms==6 && m.cycle_estimated_ms==45,"3X first-output preparation and actual real target are not added twice");
    auto legacy=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);legacy->producer_returned(125'000'000);legacy->real_prediction(140'000'000);
    float estimated{},game{},gen{},delivery{};
    check(legacy->read_estimated(125'000'000,estimated) && estimated==40 &&
        legacy->stages(120'000'000,125'000'000,estimated,game,gen,delivery) && game==20 && gen==0 && delivery==20,
        "nonprivate baseline uses a converted actual real prediction, not a synthetic slot constant");
    legacy->real_prediction(0);check(!legacy->read_estimated(125'000'000,estimated),"unknown baseline display clock stays N/A");
    generated->prepared_ns=0;window.snapshot(150'000'000,m);
    check(!m.stages_valid && m.cycle_estimated_valid,"missing readiness cannot fabricate generation duration");
    window.snapshot(1'200'000'000,m);check(!m.stages_valid && !m.cycle_estimated_valid,"all four measurements expire");
}
void estimated_cycle() {
    auto make=[](std::int64_t returned=125'000'000,std::int64_t wait_ended=105'000'000) {
        auto c=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);
        c->call(100'000'000,wait_ended);c->producer_returned(returned);return c;
    };
    float ms{};
    auto c=make();
    check(!c->read_estimated(121'000'000,ms),"without a synthetic receipt estimated cycle is N/A");
    c->synthetic_accepted(120'000'000,10'000'000);
    check(c->read_estimated(120'100'000,ms) && ms==30,"inline S/B receives one display slot, not just delivery age");
    check(c->read_estimated(130'000'000,ms) && ms==30,"real at inclusive first-slot boundary needs only one period");
    check(c->read_estimated(130'000'001,ms) && ms==40,"late real advances to next display slot");
    check(c->read_estimated(139'000'000,ms) && ms==40,"queue wait is included without adding another full period");
    auto tail=make(150'000'000);tail->synthetic_accepted(120'000'000,10'000'000);
    check(tail->read_estimated(121'000'000,ms) && ms==50,"producer tail remains part of the complete cycle");
    auto triple=make();triple->synthetic_accepted(120'100'000,10'000'000);
    triple->synthetic_accepted(120'000'000,10'000'000); // Native confirmation out of order.
    triple->synthetic_accepted(120'100'000,10'000'000); // Duplicate confirmation.
    check(triple->read_estimated(120'200'000,ms) && ms==40,"two distinct synthetics count two slots, confirmations may arrive out of order");
    auto unknown=make();unknown->synthetic_accepted(120'000'000,0);
    check(!unknown->read_estimated(125'000'000,ms),"unknown display period is N/A");
    auto changed=make();changed->synthetic_accepted(120'000'000,10'000'000);changed->synthetic_accepted(120'100'000,8'333'333);
    check(!changed->read_estimated(125'000'000,ms),"refresh change within pair cannot fabricate a slot estimate");
    check(!c->read_estimated(119'000'000,ms),"real receipt earlier than generated is invalid");
    auto unfinished=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);unfinished->synthetic_accepted(120'000'000,10'000'000);
    check(!unfinished->read_estimated(121'000'000,ms),"estimate waits for producer completion");
    // V631 Ghostwire/VDXR: CPU S acceptance +8.33ms read about10ms,
    // discarding the later real runtime target and its outstanding GPU work.
    auto predicted=make(106'500'000,100'500'000);
    predicted->synthetic_accepted(101'666'667,8'333'333);
    predicted->real_prediction(134'814'000);
    check(predicted->read_estimated(102'000'000,ms) && std::abs(ms-34.814F)<.001F,
        "VDXR generated estimate must retain the converted real target, not just CPU acceptance plus one slot");
    auto predicted_triple=make(106'500'000,100'500'000);
    predicted_triple->synthetic_accepted(101'666'667,8'333'333);
    predicted_triple->synthetic_accepted(102'000'000,8'333'333);
    predicted_triple->real_prediction(151'500'000);
    check(predicted_triple->read_estimated(102'500'000,ms) && std::abs(ms-51.5F)<.001F,
        "3X real target includes its slots; merge by max, never add those slots again");
    predicted->real_prediction(101'000'000);
    check(predicted->read_estimated(102'000'000,ms) && std::abs(ms-10.F)<.001F,
        "an unpaced early runtime target cannot undercut the existing pair-slot bound");
    InterfaceTimingWindow window;
    check(window.submitted({50,120'000'000,0,c},120'050'000,true,true,false,true,0,120'100'000),"estimated cycle needs no XrTime conversion");
    InterfaceTelemetry m{};window.snapshot(130'000'000,m);
    check(m.cycle_valid && m.cycle_length_ms==25 && m.cycle_estimated_valid && m.cycle_estimated_ms==30 &&
        m.cycle_estimated_source==InterfaceSource::frame_cycle_slot_estimate,"measured and estimated cycles remain distinct and independently labeled");
    window.snapshot(1'130'000'001,m);
    check(!m.cycle_estimated_valid && !m.cycle_valid,"both counters expire with the same one-second sample window");
}
}
int main() {try { timing(); complete_cycle(); estimated_cycle(); native_delivery_cycle(); stage_partition_and_baseline();
check(!interface_measurement_requested(), "closed menu has no timing demand");
request_interface_measurement();check(interface_measurement_requested(), "OptiScaler menu requests measurements");
std::cout << checks << " embedded timing checks passed\n";return 0;
} catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}}
