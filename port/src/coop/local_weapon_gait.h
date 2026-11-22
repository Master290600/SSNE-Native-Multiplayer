#pragma once

#include "client_presentation.h"

namespace port::coop {

struct WeaponGaitInput {
    float magnitude;
    bool falling;
};

// PAL 800A032C builds both hand roots from the same two camera axes. Movement
// strength is the capped stick length (+310), not velocity or distance between
// network snapshots. +36C holds the stride phase during a descent.
class LocalWeaponGait {
    double last_=-1.,stride_=0.,breath_=0.;

public:
    std::array<float,2> sample(double now,WeaponGaitInput input){
        constexpr double radians=3.14159265358979323846/180.;
        const auto dt=last_>=0.?std::clamp(now-last_,0.,.25):0.;
        last_=now;
        const float magnitude=std::clamp(input.magnitude,0.f,1.f);
        if(!input.falling)stride_=std::fmod(stride_+540.*magnitude*dt,360.);
        breath_=magnitude<=.001f?std::fmod(breath_+50.*dt,360.):0.;
        const float sway=.02f*magnitude*float(std::sin(stride_*radians));
        return {sway,.005f*float(std::sin(breath_*radians))-std::abs(sway)};
    }

    static void apply(PresentationMatrix& weapon,const PresentationMatrix& camera,
                      const std::array<float,2>& offset){
        // PAL writes this absolute translation after mirroring and rotating
        // the weapon for equip/holster. Those axes and the fire/reload bones
        // remain intact. Adding to the received root would double the stride.
        for(unsigned row=0;row<3;++row){
            const unsigned i=row*4;
            weapon[i+3]=camera[i+3]+camera[i]*offset[0]+camera[i+1]*offset[1];
        }
    }
};

} // namespace port::coop
