#pragma once
#include <chrono>
namespace port::coop {
// Wall-clock publication budget. Native simulation, input and rendering have
// their own cadence. Advance the deadline rather than sleeping or catching up
// with bursts; below the budget every native frame remains eligible.
class HostCaptureClock {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr auto interval=std::chrono::milliseconds(16);
    bool due(Clock::time_point now,Clock::duration cadence=interval){
        if(cadence<=Clock::duration::zero()){reset();return true;}
        if(!armed_ || now<last_ || cadence!=cadence_){armed_=true;cadence_=cadence;last_=now;next_=now+cadence;return true;}
        last_=now;if(now<next_)return false;
        next_+=cadence*((now-next_)/cadence+1);return true;
    }
    void reset(){armed_=false;}
private:
    bool armed_=false;
    Clock::time_point next_{},last_{};
    Clock::duration cadence_{};
};
}
