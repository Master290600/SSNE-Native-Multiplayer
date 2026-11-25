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
Outcome simulate_queued(unsigned fps,bool use_completed_steps){
    NativeGaitMotion motion;
    constexpr float step=.01f;
    const float dt=1.f/fps;
    float scheduled_remainder=0;
    double command_credit=0;
    uint64_t completed=0;
    std::array<float,3> position{};
    Outcome result;
    auto completed_steps=[&]()->std::optional<uint64_t>{
        if(use_completed_steps)return completed;
        return {};
    };
    motion.sample(0,position,scheduled_remainder,dt,step,completed_steps());
    for(unsigned frame=1;frame<=fps*4;++frame){
        scheduled_remainder+=dt;
        while(scheduled_remainder+1e-7f>=step)scheduled_remainder-=step;
        command_credit+=dt;
        const bool moving=frame<=fps*3;
        // Execute up to four commands in one frame when a batch arrives.
        if(frame%(fps/30)==0)while(command_credit+1e-7>=step){
            command_credit-=step;++completed;if(moving)position[0]+=12*step;
        }
        const auto sample=motion.sample(frame,position,scheduled_remainder,dt,step,completed_steps());
        if(use_completed_steps)require(sample.reconstructed && !sample.duplicate);
        if(frame>fps && moving){
            result.distance+=sample.displacement[0];
            result.moving_zeros+=std::abs(sample.displacement[0])<1e-6f;
        }
        if(frame>fps*3+fps/10)result.stopped_nonzero+=std::abs(sample.displacement[0])>1e-6f;
    }
    return result;
}
int main(){
    for(auto fps:{60u,120u,144u,240u,360u}){const auto r=simulate(fps);require(std::abs(r.distance-24)<.015 && !r.moving_zeros && !r.stopped_nonzero);}
    for(auto fps:{60u,120u,240u,360u}){
        const auto old=simulate_queued(fps,false),corrected=simulate_queued(fps,true);
        require(old.moving_zeros && std::abs(corrected.distance-24)<.015 && !corrected.moving_zeros && !corrected.stopped_nonzero);
    }
    NativeGaitMotion motion;std::array<float,3> p{};require(!motion.sample(5,p,0,.004f,1.f/120).reconstructed);p[0]=100;require(!motion.sample(6,p,.004f,.004f,1.f/120).reconstructed);
    require(!motion.sample(1,p,0,.004f,1.f/120).reconstructed);motion.reset();require(!motion.sample(2,p,0,.004f,1.f/120).reconstructed);
    require(!motion.sample(3,p,0,0,1.f/120).reconstructed);require(!motion.sample(4,p,0,.004f,1.f/120).reconstructed);
    require(!motion.sample(5,{0,0,0},0,.01f,.01f,0).reconstructed);
    require(motion.sample(6,{.12f,0,0},0,.01f,.01f,1).reconstructed);
    require(motion.sample(7,{.12f,0,0},0,.03f,.01f,1).displacement[0]>.3f);
    require(motion.sample(8,{.12f,0,0},0,.02f,.01f,1).displacement[0]==0.f);
    require(!motion.sample(9,{.12f,0,0},0,.01f,.01f,0).reconstructed);
    std::cout<<"Native gait motion: legacy and delayed command batches, duplicates, bounded stop, warp and reset passed\n";
}
