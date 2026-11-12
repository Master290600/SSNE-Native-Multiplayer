#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ssne {
// Native player movement is quantized to physics steps. The body animator is
// sampled at the render rate and needs a displacement for that same interval.
// Only its private previous-position field is reconstructed by the caller.
class NativeGaitMotion {
    bool initialized_=false;
    uint32_t frame_=0;
    double remainder_=0,step_=0,age_=0;
    std::array<float,3> position_{},velocity_{};
public:
    struct Sample {bool reconstructed=false,duplicate=false;std::array<float,3> displacement{};};
    void reset(){*this={};}
    Sample sample(uint32_t frame,const std::array<float,3>& position,float remainder,float dt,float step){
        Sample result;
        auto seed=[&]{initialized_=true;frame_=frame;position_=position;remainder_=remainder;step_=step;age_=0;velocity_={};};
        bool valid=std::isfinite(dt) && dt>0 && dt<=.1f && std::isfinite(step) && step>=.0001f && step<=.1f && std::isfinite(remainder) && std::abs(remainder)<=.25f;
        for(auto v:position)valid&=std::isfinite(v) && std::abs(v)<1000000.f;
        if(!valid){reset();return result;}
        if(!initialized_ || frame<frame_ || std::abs(double(step)-step_)>1e-7){seed();return result;}
        if(frame==frame_){result.duplicate=true;return result;}
        std::array<float,3> delta;double distance2=0;
        for(unsigned i=0;i<3;++i){delta[i]=position[i]-position_[i];distance2+=double(delta[i])*delta[i];}
        if(distance2>16.){seed();return result;} // Teleport/respawn, not a gait sample.
        const double advanced=remainder_+dt-remainder;
        const auto steps=std::lround(advanced/step);
        if(steps<0 || steps>32 || std::abs(advanced-double(steps)*step)>std::max(1e-6,double(step)*.02)){
            seed();return result;
        }
        if(steps){
            const double seconds=double(steps)*step;
            for(unsigned i=0;i<3;++i)velocity_[i]=float(delta[i]/seconds);
            age_=0;
        }else{
            // No movement step has happened, so retain the last physical
            // velocity briefly. A collision/real stop clears it on its next step.
            if(distance2>1e-8){seed();return result;}
            age_+=dt;if(age_>double(step)*2+1e-6)velocity_={};
        }
        frame_=frame;position_=position;remainder_=remainder;
        result.reconstructed=true;
        for(unsigned i=0;i<3;++i)result.displacement[i]=velocity_[i]*dt;
        return result;
    }
};
}
