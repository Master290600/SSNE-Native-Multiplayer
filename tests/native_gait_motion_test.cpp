#include "ssne/native_gait_motion.h"
#include <stdexcept>
#include <cmath>
#include <iostream>
using ssne::NativeGaitMotion;
void require(bool value){if(!value)throw std::runtime_error("Native gait motion regression failed");}
struct Outcome{double distance=0;unsigned moving_zeros=0,stopped_nonzero=0;};
Outcome simulate(unsigned fps){
    NativeGaitMotion motion;constexpr float step=1.f/120;const float dt=1.f/fps;float remainder=0;std::array<float,3> position{};Outcome result;
    motion.sample(0,position,remainder,dt,step);
    for(unsigned frame=1;frame<=fps*4;++frame){
        const bool moving=frame<=fps*3;remainder+=dt;
        while(remainder+1e-7f>=step){remainder-=step;if(moving)position[0]+=12*step;}
        auto sample=motion.sample(frame,position,remainder,dt,step);require(sample.reconstructed && !sample.duplicate);
        auto duplicate=motion.sample(frame,position,remainder,dt,step);require(duplicate.duplicate);
        if(frame>fps && frame<=fps*3){result.distance+=sample.displacement[0];result.moving_zeros+=std::abs(sample.displacement[0])<1e-6f;}
        if(frame>fps*3+fps/10)result.stopped_nonzero+=std::abs(sample.displacement[0])>1e-6f;
    }
    return result;
}
int main(){
    for(auto fps:{60u,120u,144u,240u,360u}){const auto r=simulate(fps);require(std::abs(r.distance-24)<.015 && !r.moving_zeros && !r.stopped_nonzero);}
    NativeGaitMotion motion;std::array<float,3> p{};require(!motion.sample(5,p,0,.004f,1.f/120).reconstructed);p[0]=100;require(!motion.sample(6,p,.004f,.004f,1.f/120).reconstructed);
    require(!motion.sample(1,p,0,.004f,1.f/120).reconstructed);motion.reset();require(!motion.sample(2,p,0,.004f,1.f/120).reconstructed);
    require(!motion.sample(3,p,0,0,1.f/120).reconstructed);require(!motion.sample(4,p,0,.004f,1.f/120).reconstructed);
    std::cout<<"Native gait motion: 60/120/144/240/360FPS physical stepping, duplicates, bounded stop, warp and reset passed\n";
}
