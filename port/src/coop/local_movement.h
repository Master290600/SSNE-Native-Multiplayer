#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>

namespace port::coop {
using MovementPoint=std::array<float,3>;

inline MovementPoint movement_difference(const MovementPoint& a,const MovementPoint& b){
    return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};
}
inline float movement_length(const MovementPoint& v){return std::hypot(v[0],v[1],v[2]);}

struct MovementAnchor {
    MovementPoint position{},velocity{};
    uint64_t acknowledged=0;
    bool grounded=false;
};
struct MovementIntent {
    uint64_t sequence=0,jump=0;
    float right=0,forward=0,yaw=0;
};
struct MovementTuning {
    float ground_acceleration=0,ground_decay=0,air_acceleration=0,air_decay=0;
    float jump_height=0,jump_duration=0;
};
struct MovementContact {
    MovementPoint position{};
    bool grounded=false;
    bool entered_water=false;
};
struct SwimInput {
    MovementPoint acceleration{};
    float drag=1.f;
    bool surface=false;
    float capsule_diameter=0.f;
};

class LocalMovement {
    enum class Phase { Ground, JumpCurve, Fall, EnterWater, Swim };
    struct Step {uint64_t sequence;MovementPoint delta;};
    static constexpr double tick=1./60;
    static constexpr float dt=float(tick);
    std::deque<Step> steps_;
    MovementPoint position_{},velocity_{},outstanding_{};
    double previous_=0,elapsed_=0;
    float jump_time_=0,blocked_correction_time_=0;
    std::optional<float> walked_off_height_;
    uint64_t jump_seen_=0;
    uint64_t water_entry_input_=0;
    Phase phase_=Phase::Ground;
    bool initialized_=false;

    void acknowledge(uint64_t sequence){
        while(!steps_.empty() && steps_.front().sequence<=sequence){
            outstanding_=movement_difference(outstanding_,steps_.front().delta);
            steps_.pop_front();
        }
        if(steps_.empty())outstanding_={};
    }
    void record(uint64_t sequence,const MovementPoint& delta){
        if(movement_length(delta)<.0001f)return;
        if(steps_.empty() || steps_.back().sequence!=sequence)steps_.push_back({sequence,{}});
        for(unsigned axis=0;axis<3;++axis){
            steps_.back().delta[axis]+=delta[axis];
            outstanding_[axis]+=delta[axis];
        }
    }
public:
    bool entering_water() const {return initialized_ && phase_==Phase::EnterWater;}
    bool falling(bool fallback) const {
        return initialized_?(phase_==Phase::Fall && velocity_[1]<0.f):fallback;
    }

    void reset(){
        steps_.clear();
        outstanding_={};
        blocked_correction_time_=0;
        walked_off_height_.reset();
        initialized_=false;
    }
    // The resolver may decline an unsafe prediction. Never move the gameplay actor.
    template<class Resolve>
    std::optional<MovementPoint> sample(double now,const MovementAnchor& host,const MovementIntent& input,
                                        const MovementTuning& tuning,Resolve&& resolve,const SwimInput* swim=nullptr,
                                        const SwimInput* entry_swim=nullptr){
        if(!initialized_){
            if(!host.grounded && !swim)return {};
            position_=host.position;
            velocity_=host.velocity;
            phase_=swim?Phase::Swim:Phase::Ground;
            jump_seen_=host.acknowledged;
            previous_=now;
            elapsed_=0.;
            initialized_=true;
        }
        if(swim && phase_!=Phase::Swim){
            if(phase_!=Phase::EnterWater)velocity_=host.velocity;
            else velocity_[1]=host.velocity[1];
            phase_=Phase::Swim;
            blocked_correction_time_=0;
        }else if(!swim && (phase_==Phase::Swim ||
                (phase_==Phase::EnterWater && (!entry_swim || host.acknowledged>=water_entry_input_)))){
            phase_=host.grounded?Phase::Ground:Phase::Fall;
            velocity_=host.velocity;
            jump_seen_=std::max(input.jump,host.acknowledged);
        }
        const bool wants_move=input.right || input.forward;
        const auto jump=std::max(jump_seen_,host.acknowledged);
        acknowledge(host.acknowledged);
        elapsed_+=std::clamp(now-previous_,0.,.05);
        previous_=now;
        if(!wants_move && input.jump<=jump && phase_==Phase::Ground && host.grounded &&
           steps_.empty() && movement_length(velocity_)<.06f &&
           movement_length(movement_difference(position_,host.position))<.10f){
            reset();
            return {};
        }
        if(steps_.size()>=480 || movement_length(movement_difference(position_,host.position))>96.f){
            reset();
            return {};
        }

        const auto sine=std::sin(input.yaw),cosine=std::cos(input.yaw);
        const float x=input.right*cosine+input.forward*sine;
        const float z=input.forward*cosine-input.right*sine;
        while(elapsed_>=tick){
            elapsed_-=tick;
            const auto* water_input=swim?swim:(entering_water()?entry_swim:nullptr);
            if(!water_input && input.jump>std::max(jump_seen_,host.acknowledged)){
                jump_seen_=input.jump;
                if(phase_==Phase::Ground){
                    // Confirm current support locally before starting a deliberate jump.
                    const auto support=resolve(position_,position_,true);
                    if(support && support->grounded){
                        phase_=Phase::JumpCurve;
                        jump_time_=0;
                    }
                }
            }

            // Reconcile against the host pose plus the locally resolved steps
            // that the host has yet to acknowledge. Sweep corrections as well.
            const auto confirmed_error=movement_difference(host.position,position_);
            auto error=confirmed_error;
            for(unsigned axis=0;axis<3;++axis)error[axis]+=outstanding_[axis];
            if(phase_==Phase::Ground && !host.grounded)error[1]=0;
            const auto discrepancy=movement_length(error);
            bool blocked_correction=false;
            if(discrepancy>.04f){
                auto corrected=position_;
                const float ratio=std::min(.13f, .10f/discrepancy);
                for(unsigned axis=0;axis<3;++axis)corrected[axis]+=error[axis]*ratio;
                const auto result=resolve(position_,corrected,phase_==Phase::Ground);
                if(!result){reset();return {};}
                // A local ledge may stop the sphere even though PAL already
                // moved the acknowledged actor past it. Measure progress in
                // the requested direction; a sideways slide is not progress
                // towards that anchor.
                if(discrepancy>.5f && host.grounded && host.acknowledged){
                    const auto travelled=movement_difference(result->position,position_);
                    float travelled_along=0,confirmed_along=0;
                    for(unsigned axis=0;axis<3;++axis){
                        travelled_along+=travelled[axis]*error[axis];
                        confirmed_along+=confirmed_error[axis]*error[axis];
                    }
                    // Fall back only if the confirmed actor reached the obstructed
                    // side within the correction being attempted. An old pose
                    // can lie in the same direction but far behind pending steps.
                    blocked_correction=movement_length(confirmed_error)<=discrepancy+.04f
                        && confirmed_along>.5f*discrepancy
                        && travelled_along<.25f*ratio*discrepancy*discrepancy;
                }
                position_=result->position;
            }
            blocked_correction_time_=blocked_correction?blocked_correction_time_+dt:0.f;
            if(blocked_correction_time_>=.12f){
                reset();
                return {};
            }

            if(water_input){
                MovementPoint desired=position_;
                for(unsigned i=0;i<3;++i){
                    velocity_[i]+=(water_input->acceleration[i]-water_input->drag*velocity_[i])*dt;
                    desired[i]+=velocity_[i]*dt;
                }
                if(water_input->surface && desired[1]>std::max(position_[1],host.position[1])){
                    desired[1]=std::max(position_[1],host.position[1]);
                    velocity_[1]=0.f;
                }
                const auto water=resolve(position_,desired,false);
                if(!water){reset();return {};}
                if(velocity_[1]>0.f && water->position[1]<desired[1]-.001f)
                    velocity_[1]=0.f;
                if(input.sequence>host.acknowledged)
                    record(input.sequence,movement_difference(water->position,position_));
                position_=water->position;
                continue;
            }
            const bool walking=phase_==Phase::Ground;
            const float acceleration=walking?tuning.ground_acceleration:tuning.air_acceleration;
            const float decay=walking?tuning.ground_decay:tuning.air_decay;
            velocity_[0]+=(acceleration*x-decay*velocity_[0])*dt;
            velocity_[2]+=(acceleration*z-decay*velocity_[2])*dt;
            if(walking && !wants_move && std::hypot(velocity_[0],velocity_[2])<.06f)
                velocity_[0]=velocity_[2]=0;
            MovementPoint desired=position_;
            desired[0]+=velocity_[0]*dt;
            desired[2]+=velocity_[2]*dt;
            if(phase_==Phase::JumpCurve){
                constexpr float pi=3.14159265358979323846f;
                const float old=jump_time_;
                jump_time_=std::min(jump_time_+dt,.55f*tuning.jump_duration);
                const float rise=tuning.jump_height*(std::sin(pi*jump_time_/tuning.jump_duration)-
                                                     std::sin(pi*old/tuning.jump_duration));
                desired[1]+=rise;
                velocity_[1]=rise*50.f*((1.f/120)/dt);
                if(jump_time_>=.55f*tuning.jump_duration)phase_=Phase::Fall;
            }else if(phase_==Phase::Fall){
                velocity_[1]-=98.f*dt;
                desired[1]+=velocity_[1]*dt;
            }else velocity_[1]=0;

            if(walking && position_==desired)continue;
            const auto result=resolve(position_,desired,walking);
            if(!result){reset();return {};}
            const auto delta=movement_difference(result->position,position_);
            if(entry_swim && result->entered_water){
                // A walking PAL capsule can settle down a rounded bank by a
                // body diameter without acquiring an equivalent fall velocity.
                // A jump or a deeper fall keeps its actual downward momentum.
                const float drop=walking?0.f:(walked_off_height_?*walked_off_height_-result->position[1]:-1.f);
                if(drop>=0.f && drop<=entry_swim->capsule_diameter && velocity_[1]<0.f)
                    velocity_[1]=std::clamp(host.velocity[1],velocity_[1],0.f);
                // PAL 800A9D08 limits the entire incoming velocity vector to
                // five units/s when entering water, before applying water drag.
                const float speed=movement_length(velocity_);
                if(speed>5.f)for(float& v:velocity_)v*=5.f/speed;
                water_entry_input_=input.sequence;
                phase_=Phase::EnterWater;
                walked_off_height_.reset();
            }else if(walking){
                phase_=result->grounded?Phase::Ground:Phase::Fall;
                if(!result->grounded)walked_off_height_=position_[1];
                else walked_off_height_.reset();
            }
            else if(desired[1]>position_[1] && result->position[1]<desired[1]-.025f){
                phase_=Phase::Fall;
                velocity_[1]=0;
            }else if(desired[1]<=position_[1] && result->grounded){
                phase_=Phase::Ground;
                velocity_[1]=0;
                walked_off_height_.reset();
            }
            position_=result->position;
            if(input.sequence>host.acknowledged)record(input.sequence,delta);
        }
        return position_;
    }
};
}

