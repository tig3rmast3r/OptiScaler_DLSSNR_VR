#pragma once
#include <chrono>
#include <algorithm>
#include <cstdint>
namespace xrfg {
struct DjulesTripleRelease {
    std::chrono::nanoseconds delay{},ceiling{-1};
    unsigned frames{},late{},last_late{},hold{};
    bool observe(bool ready,std::chrono::nanoseconds period) noexcept {
        using namespace std::chrono;
        if(period<=nanoseconds::zero()) return false;
        if(!ready) ++late;
        if(++frames<30) return false;
        frames=0;last_late=late;late=0;
        if(ceiling<nanoseconds::zero()) ceiling=period*2;
        if(last_late>=2) {
            if(delay>nanoseconds::zero()) {
                ceiling=std::max(nanoseconds::zero(),delay-milliseconds(3));
                delay=ceiling;hold=10;
            }
        } else if(last_late==0) {
            if(delay<ceiling) delay=std::min(ceiling,delay+milliseconds(2));
            else if(hold) --hold;
            else { ceiling=std::min(period*2,ceiling+milliseconds(2));delay=ceiling; }
        }
        return true;
    }
};
}
