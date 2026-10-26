#include "xrfg/private_output_distributor.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void require(bool v,const char* what){if(!v)throw std::runtime_error(what);}
int main(){try {
    constexpr std::int64_t ms=1'000'000;
    xrfg::PrivateRuntimeBudget budget;
    require(budget.lead(11*ms)==1'100'000,"unsampled native entry must reserve the requested extra1ms, not a whole frame");
    budget.observe(5*ms,11*ms);
    require(budget.lead(11*ms)==5*ms+1'100'000,"first observation lost the requested extra1ms or invented variation");
    unsigned misses{};
    xrfg::PrivateRuntimeBudget ramp;
    for(int n=0;n<24;++n) {
        const auto work=200'000+std::int64_t(n)*400'000;
        if(n>2 && ramp.lead(11*ms)<work)++misses;
        ramp.observe(work,11*ms);
    }
    require(misses==0,"native entry missed every step despite already observed rising service time");
    ramp.observe(50*ms,11*ms);require(ramp.lead(11*ms)<=23'100'000 && ramp.lead(11*ms)>11*ms,"overlapped native forecast lost its extra margin or exceeded the bounded pipeline");
    for(int n=0;n<2000;++n)ramp.observe(0,11*ms);
    require(ramp.lead(11*ms)<1'101'000,"past stall did not recover to the requested1.1ms floor");
    xrfg::PrivateOutputSchedule margin;
    margin.period(11*ms);margin.ready(20*ms);require(margin.delivered(20*ms),"margin first output");
    require(margin.runtime_ready(30*ms,2*ms)==1*ms && margin.deadline()==32*ms,"missing native margin was clipped instead of becoming exact presentation latency");
    require(margin.runtime_ready(30*ms,2*ms)==0 && margin.delay()==1*ms,"same margin charged repeatedly");
    require(!margin.eligible(31*ms) && margin.delivered(32*ms),"native margin changed the minimum physical period");
    require(margin.runtime_ready(40*ms,2*ms)==0 && margin.deadline()==43*ms,"retained native margin unnecessarily grew on early input");
    require(margin.delivered(43*ms) && margin.delay()==1*ms,"native margin was lost on the following cycle");
    xrfg::PrivatePhysicalSchedule grid;
    require(grid.clock({20*ms,11*ms,100}),"physical anchor rejected");
    require(grid.observe_work(3*ms,3)==3*ms && grid.delay()==3*ms,"private latency is not applied as packet eligibility");
    require(grid.claim(24*ms,27*ms,25*ms,27*ms,1*ms)==30*ms && grid.slot()==101,"first physical slot not anchored to headset");
    require(grid.delivered(30*ms+20'000),"physical first output");
    require(grid.claim(27*ms,30*ms,39*ms,40*ms,1*ms)==41*ms && grid.slot()==102,"late CPU entry permanently moved the physical phase");
    require(grid.delivered(41*ms),"next absolute slot did not recover the same clock phase");
    require(grid.observe_work(8*ms,3)==5*ms && grid.delay()==8*ms,"late GPU work did not add only its exact missing buffered time");
    require(grid.claim(47*ms,55*ms,50*ms,55*ms,1*ms)==63*ms && grid.slot()==104,"late prepared packet moved phase instead of skipping an expired display slot");
    require(grid.delivered(63*ms),"late GPU packet was discarded");
    require(grid.observe_work(2*ms,3)==0 && grid.delay()==8*ms,"buffered GPU time was lost or charged again");
    require(grid.claim(57*ms,60*ms,65*ms,65*ms,1*ms)==74*ms && grid.slot()==105,"retained buffer did not apply to next packet on the same grid");
    require(grid.delivered(74*ms),"buffered following output");
    require(grid.observe_work(10'000*ms,3)==14*ms && grid.delay()==22*ms,"pause created unbounded replay latency");
    for(const auto p:{8'333'333LL,11'111'111LL,13'333'333LL}) {
        xrfg::PrivatePhysicalSchedule c;const std::int64_t base=1'000'000'000;
        require(c.clock({base,p,1000}),"variable physical clock");
        std::int64_t last{};std::uint64_t slot{};
        for(int n=0;n<1000;++n) {
            // Irregular75fps input, bounded late-copy service, no assumed90fps.
            const auto source=base+std::int64_t(n)*13'333'333;
            const auto work=(n%17==0?7:2)*ms;
            c.observe_work(work,3);const auto at=c.claim(source,source+work,source,source+work,100'000);
            require((at+ms-base)%p==0 && c.slot()>slot && at>=source+c.delay(),"output left physical grid, reused a slot or bypassed held packet latency");
            if(n)require(at-last>=p,"fixed slots ran faster than headset");
            require(c.delivered(at),"finite irregular input was lost");last=at;slot=c.slot();
        }
    }
    // End itself must keep a fixed 1ms phase lead at every supported refresh,
    // including repeated raw clock observations. Never run at P-minus-lead.
    for(const auto p:{8'333'333LL,11'111'111LL,13'333'333LL}) {
        xrfg::PrivatePhysicalSchedule early;const std::int64_t base=1'000'000'000;
        std::int64_t last{};
        for(std::int64_t n=1;n<=10'000;++n) {
            const xrfg::PrivatePhysicalClock observed{base+(n-1)*p,p,1000+static_cast<std::uint64_t>(n-1)};
            require(early.clock(observed) && early.clock(observed),"raw clock resample rejected");
            const auto expected=base+n*p-ms;
            const auto at=early.claim(expected-3*ms,expected-2*ms,expected-3*ms,expected-2*ms,100'000);
            require(at==expected && early.slot()==1000+static_cast<std::uint64_t>(n),"actual End edge is not 1ms before the unchanged physical slot");
            require(!n || !last || at-last==p,"End advance changed frequency or accumulated with clock resamples");
            require(early.delivered(at),"advanced physical End edge refused");last=at;
        }
    }
    xrfg::PrivateOutputSchedule clock;
    clock.period(11*ms);clock.ready(20*ms);
    require(clock.eligible(20*ms) && clock.delivered(20*ms),"first output");
    clock.ready(30*ms);
    require(!clock.eligible(30*ms) && clock.delivered(31*ms),"short second delivery permitted");
    clock.ready(40*ms);require(clock.delivered(42*ms),"third output");
    clock.ready(57*ms);
    require(clock.delay()==4*ms && clock.deadline()==57*ms,"late head rounded to a refresh");
    clock.ready(57*ms);require(clock.delay()==4*ms,"same lateness accumulated twice");
    require(clock.delivered(57*ms),"late head lost");
    clock.ready(60*ms);
    require(clock.delay()==4*ms && !clock.eligible(64*ms) && clock.delivered(68*ms),"late output followed by burst catchup");
    clock.ready(69*ms);
    require(clock.delivered(83*ms) && clock.delay()==8*ms,"CPU handoff delay not retained");
    clock.ready(84*ms);require(!clock.eligible(90*ms) && clock.delivered(94*ms),"post-overrun interval shorter than refresh");
    xrfg::PrivateOutputSchedule adaptive;
    adaptive.period(11*ms);require(adaptive.input_ready(20*ms,3)==0,"initial frame unnecessarily prebuffered");
    adaptive.ready(20*ms);require(adaptive.delivered(20*ms),"adaptive first output");
    require(adaptive.input_ready(35*ms,3)==4*ms && adaptive.deadline()==35*ms,"source lateness did not build an exact refill margin");
    adaptive.ready(36*ms);require(adaptive.delivered(36*ms) && adaptive.deadline()==51*ms,"refill was not retained after delivering the late frame");
    require(adaptive.input_ready(45*ms,3)==0,"early source added an unnecessary wait");
    adaptive.ready(46*ms);require(adaptive.delivered(51*ms) && adaptive.delay()==9*ms,"refill phase was lost or charged on every frame");
    // A 75fps producer on a90Hz display must keep EVERY packet, not await
    // imaginary90fps input. Refills are bounded by owned pool capacity and
    // otherwise ready input receives no fixed per-frame latency tax.
    for(int i=1;i<100;++i) {
        const auto source=51*ms+i*13'333'333LL;
        const auto before=adaptive.deadline();
        const auto added=adaptive.input_ready(source,3);
        require(added==std::min(std::max(std::int64_t{},source-before),22*ms),"adaptive margin rounded to an entire refresh");
        adaptive.ready(source);const auto at=std::max(source,adaptive.deadline());
        require(adaptive.delivered(at),"sub-refresh-rate input was dropped");
    }
    const auto paused=adaptive.deadline()+10'000*ms;
    require(adaptive.input_ready(paused,3)==22*ms,"paused producer caused an unbounded replay pause");
    for(auto period:{8'333'333LL,11'111'111LL,13'333'333LL}) {
        xrfg::PrivateOutputSchedule c;c.period(period);c.ready(1);
        auto at=std::int64_t{1};require(c.delivered(at),"fresh refresh clock");
        for(int i=0;i<200;++i){c.ready(at+1);require(!c.eligible(at+period-1),"refresh minus1ns accepted");at+=period;require(c.delivered(at),"fixed refresh cadence");}
        // Early preparation changes neither cadence nor held lateness, even
        // over many refreshes. It is not period-minus100us pacing.
        for(int i=0;i<10000;++i) {
            c.ready(at+1);
            const auto due=c.deadline(),prepare=c.preparation_deadline();
            require(due-prepare==100'000 && !c.eligible(prepare),"preparation borrowed a refresh");
            require(c.deadline()==due && c.delay()==0,"early preparation moved phase");
            at+=period;require(c.delivered(at),"early preparation changed frequency");
        }
        c.ready(at+period+250'000);
        require(c.delay()==250'000 && c.delivered(at+period+250'000),"late preparation discarded exact overrun");
        at+=period+250'000;c.ready(at+1);
        require(!c.eligible(c.preparation_deadline()) && c.delivered(at+period),"late preparation caused catch-up");
    }
    // Producer completion depends only on taking ownership of a private slot.
    // A pending consumer handoff keeps its slot until explicit retirement.
    xrfg::PrivateOutputFifo<unsigned> fifo(2);
    require(!fifo.accept(99),"unreserved acceptance");
    require(fifo.reserve(2) && fifo.accept(10) && fifo.accept(20),"private pair acceptance");
    auto a=fifo.pop();require(a && *a==10 && fifo.occupied()==2,"dequeue freed an in-use image");
    std::promise<void> entered;auto signaled=entered.get_future();
    auto producer=std::async(std::launch::async,[&]{entered.set_value();return fifo.reserve() && fifo.accept(30);});
    signaled.wait();
    require(producer.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,"full pool did not backpressure producer");
    fifo.retire();require(producer.get(),"producer not released by slot retirement");
    auto b=fifo.pop();auto c=fifo.pop();require(b && c && *b==20 && *c==30,"FIFO order/old output overwritten");
    fifo.retire(2);require(fifo.occupied()==0,"retired pool leaked slots");
    require(fifo.reserve(2),"reservation reuse");fifo.cancel(2);require(fifo.occupied()==0,"failed producer reservation leaked");
    require(fifo.reserve(2),"stop control reserve");
    auto blocked=std::async(std::launch::async,[&]{return fifo.reserve();});
    fifo.stop();require(!blocked.get() && !fifo.pop(),"stop did not release producer/consumer");
    std::cout<<"Private distributor: fixed1ms End phase and100us preparation without drift over10000refreshes; exact held lateness; strict90/120/75Hz; FIFO ownership; backpressure; stop PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
