#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace port::coop {
struct NativeDoorPose {
    unsigned segment=0;
    float progress=0;
    bool operator==(const NativeDoorPose&)const=default;
};

// A brush can share its pose across many network revisions. Time advances here
// on the render clock, including frames between two confirmed path parameters.
class NativeDoorClock {
    uint32_t object_=0;
    unsigned count_=0;
    double from_=0,target_=0,start_=0,duration_=0,rate_=0;
    NativeDoorPose authority_{};
    double phase(double now)const{
        if(duration_<=0 || now>=start_+duration_)return target_;
        return std::lerp(from_,target_,std::clamp((now-start_)/duration_,0.,1.));
    }
public:
    void confirm(uint32_t object,unsigned count,NativeDoorPose next,double segment_seconds,double now){
        auto snap=[&]{
            object_=object;count_=count;authority_=next;
            from_=target_=double(next.segment)+next.progress;
            start_=now;duration_=0;rate_=0;
        };
        if(object!=object_ || count!=count_ || !count || next.segment>=count ||
           !std::isfinite(segment_seconds) || segment_seconds<=0){
            snap();return;
        }
        if(next==authority_){stop_collision_prediction();return;}
        double delta=double(next.progress)-authority_.progress;
        if(next.segment!=authority_.segment){
            if(next.segment!=(authority_.segment+1)%count){snap();return;}
            delta+=1.; // Native keyframes advance forward around the authored ring.
        }
        // Only advance an observed movement on this segment. A segment
        // change can have a different authored duration or end in a wait.
        rate_=next.segment==authority_.segment && delta!=0.?std::copysign(1./segment_seconds,delta):0.;
        from_=phase(now);target_+=delta;start_=now;authority_=next;
        const auto distance=std::abs(target_-from_);
        // A discontinuity spanning a whole unobserved segment is a new state.
        if(distance>1.25){snap();return;}
        duration_=std::min(1.25,distance*segment_seconds);
    }
    NativeDoorPose pose(double now)const{
        const auto value=phase(now);
        if(!count_ || value==target_)return authority_;
        const auto whole=std::floor(value);
        const auto index=(int64_t(whole)%int64_t(count_)+count_)%count_;
        return {unsigned(index),float(value-whole)};
    }
    uint32_t object()const{return object_;}
    NativeDoorPose authority()const{return authority_;}
    void stop_collision_prediction(){rate_=0.;}
    float collision_progress(double now,double round_trip)const{
        if(rate_==0. || authority_.progress<=0.f || authority_.progress>=1.f)return authority_.progress;
        // The latest pose was captured before the snapshot arrived, and
        // input sampled here will be simulated after its trip to the host.
        const auto age=std::max(0.,now-start_);
        const auto lead=std::clamp(age+round_trip,0.,.40);
        return float(std::clamp(double(authority_.progress)+rate_*lead,0.,1.));
    }
};
}

