#pragma once

#include "local_movement.h"
#include "session_authority.h"
#include "ssne/native_movement_step.h"
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace port::coop {

// Presentation prediction for completed native10ms movement commands. The
// transport owns command production/history and stream/life identity. This
// cache never invents physics time from render frames or a UI sequence ACK.
class CommandMovement {
public:
    using PeerPositions=std::array<std::optional<MovementPoint>,max_players>;
private:
    using State=ssne::NativeMovementState;
    using Command=ssne::NativeMovementCommand;
    static constexpr float dt=.01f;
    State anchor_{},state_{};
    uint64_t anchor_completed_=0,completed_=0;
    bool valid_=false;
    std::vector<Command> history_;
    PeerPositions peers_{};
    bool player_contact_=false;
    bool view_was_blocked_=false,view_reconciling_=false;
    uint64_t view_completed_=0;
    double view_time_=0.;
    std::optional<MovementPoint> view_position_;
    std::optional<State> water_entry_;

    static bool same(const State& a,const State& b){
        return a.position==b.position && a.velocity==b.velocity && a.phase==b.phase &&
            a.pending_jump==b.pending_jump && a.grounded==b.grounded && a.sliding==b.sliding &&
            a.curve==b.curve && a.ground==b.ground && a.contact_kind==b.contact_kind;
    }
    static bool same(const Command& a,const Command& b){
        return a.sequence==b.sequence && a.stick_x==b.stick_x && a.stick_y==b.stick_y &&
            a.jump_pressed==b.jump_pressed && a.yaw==b.yaw;
    }
    static bool finite_state(const State& state){
        const auto finite=[](const auto& values){
            return std::all_of(values.begin(),values.end(),[](float v){return std::isfinite(v) && std::abs(v)<10000000.f;});
        };
        if(state.phase==2 || state.phase>3 || state.pending_jump>1 || state.contact_kind>1 || !finite(state.position) ||
           !finite(state.velocity) || movement_length(state.velocity)>=160.f || !finite(state.curve) || !finite(state.ground))return false;
        if(state.curve[5]<0.f || state.curve[5]>.2f)return false;
        return state.phase!=1 || (state.curve[0]>.1f && state.curve[0]<=12.f &&
            state.curve[1]>=.12f && state.curve[1]<=2.f && state.curve[2]>=0.f &&
            state.curve[2]<=state.curve[1]*.55f+.011f);
    }
    static bool finite_tuning(const MovementTuning& t){
        return std::isfinite(t.ground_acceleration) && t.ground_acceleration>0.f && t.ground_acceleration<=1600.f &&
            std::isfinite(t.ground_decay) && t.ground_decay>=0.f && t.ground_decay<=60.f &&
            std::isfinite(t.air_acceleration) && t.air_acceleration>=0.f && t.air_acceleration<=1600.f &&
            std::isfinite(t.air_decay) && t.air_decay>=0.f && t.air_decay<=60.f &&
            std::isfinite(t.jump_height) && t.jump_height>.1f && t.jump_height<=12.f &&
            std::isfinite(t.jump_duration) && t.jump_duration>=.12f && t.jump_duration<=2.f &&
            std::isfinite(t.minimum_ground_motion) && t.minimum_ground_motion>=0.f &&
            std::isfinite(t.default_ground_acceleration) && t.default_ground_acceleration>=0.f &&
            t.default_ground_acceleration<=1600.f && std::isfinite(t.default_ground_decay) &&
            t.default_ground_decay>=0.f && t.default_ground_decay<=60.f &&
            (t.default_ground_acceleration>0.f || t.default_ground_decay==0.f);
    }

    template<class Resolve>
    bool step(const Command& command,const MovementTuning& tuning,Resolve&& resolve){
        auto& s=state_;
        // Original controller+C is examined before the new logical L edge.
        // An edge first queues a jump; the following eligible native step
        // starts the curve at t0, whose displacement is still zero.
        if(s.pending_jump){
            s.pending_jump=0;
            if(s.phase==0 && s.grounded){
                s.phase=1;
                s.curve[0]=tuning.jump_height;s.curve[1]=tuning.jump_duration;s.curve[2]=0.f;
                s.curve[3]=s.curve[4]=s.position[1];
            }
        }else if(command.jump_pressed && s.phase==0 && s.grounded)s.pending_jump=1;

        const auto axes=ssne::native_movement_axes(uint8_t(int(command.stick_x)+128),uint8_t(int(command.stick_y)+128));
        const float sine=float(std::sin(double(command.yaw))),cosine=float(std::cos(double(command.yaw)));
        const float side=-axes[0],forward=axes[1];
        const float sx=float(double(sine)*side),cx=float(double(cosine)*side);
        const float x=float(double(sine)*forward-cx),z=float(double(cosine)*forward+sx);
        const bool walking=s.phase==0;
        // Phase0 can retain a sliding contact while its ground-support flag
        // is false. PAL halves the default acceleration and decay in this
        // branch and continues applying half gravity; it is not normal rest.
        const bool sliding=walking && s.sliding;
        const bool default_parameters=tuning.default_ground_acceleration>0.f;
        const float slide_acceleration=default_parameters?tuning.default_ground_acceleration:tuning.ground_acceleration;
        const float slide_decay=default_parameters?tuning.default_ground_decay:tuning.ground_decay;
        const float acceleration=sliding?float(double(slide_acceleration)*.5f):
            walking?tuning.ground_acceleration:tuning.air_acceleration;
        const float decay=sliding?float(double(slide_decay)*.5f):
            walking?tuning.ground_decay:tuning.air_decay;
        for(const auto [axis,input]:{std::pair{0u,x},std::pair{2u,z}}){
            const float drive=float(double(input)*acceleration);
            const float force=float(-double(s.velocity[axis])*decay+drive);
            const float change=float(double(force)*dt);
            s.velocity[axis]=float(double(s.velocity[axis])+change);
        }
        MovementPoint desired=s.position;
        float vertical_motion=0.f;
        desired[0]=float(double(desired[0])+float(double(s.velocity[0])*dt));
        desired[2]=float(double(desired[2])+float(double(s.velocity[2])*dt));
        if(s.phase==1){
            constexpr float pi=3.14159265358979323846f;
            const float ratio=float(double(s.curve[2])/s.curve[1]);
            const float angle=float(double(ratio)*pi),wave=float(std::sin(double(angle)));
            const float height=float(double(wave)*s.curve[0]+s.curve[3]);
            const float rise=float(double(height)-s.curve[4]);
            s.curve[4]=height;s.curve[5]=float(double(ratio)*.2f);
            if(rise<0.f)s.curve[6]=float(double(s.curve[6])-rise);
            s.velocity[1]=float(double(rise)*50.f);
            vertical_motion=rise;
            desired[1]=float(double(desired[1])+rise);
        }else{
            s.curve[5]=std::max(0.f,float(double(s.curve[5])-.01f));
            if(s.phase==3){
                s.velocity[1]=float(double(s.velocity[1])-float(98.f*dt));
                vertical_motion=float(double(s.velocity[1])*dt);
                desired[1]=float(double(desired[1])+vertical_motion);
            }else if(sliding){
                // The original compares the old velocity before integrating:
                // exactly -147 receives gravity once, values below it clamp.
                s.velocity[1]=s.velocity[1]<-147.f?-147.f:
                    float(double(s.velocity[1])+float(-49.f*dt));
                vertical_motion=float(double(s.velocity[1])*dt);
                desired[1]=float(double(desired[1])+vertical_motion);
            }else s.velocity[1]=0.f;
        }
        auto ground=s.ground;
        MovementCollisionContext query{MovementResolvePurpose::Motion,dt,ground};
        const auto contact=resolve(s.position,desired,walking,query);
        if(!contact)return false;
        if(contact->entered_water){
            water_entry_=s;
            water_entry_->position=contact->position;
            return false;
        }
        player_contact_|=contact->player_contact;
        s.ground=ground;
        s.position=contact->position;s.grounded=contact->grounded;
        s.sliding=contact->sliding;s.contact_kind=contact->contact_kind;
        if(s.contact_kind==0){
            if(s.phase==0){s.phase=3;s.curve[6]=0.f;s.velocity[1]=0.f;}
            s.curve[6]=vertical_motion<0.f?float(double(s.curve[6])-vertical_motion):0.f;
        }else{
            if(s.phase==1 || s.phase==3)s.phase=0;
        }
        if(s.phase==1){
            s.curve[2]=float(double(s.curve[2])+dt);
            if(s.curve[2]>=float(double(s.curve[1])*.55f))s.phase=3;
        }
        if(s.phase!=1)std::fill_n(s.curve.begin(),5,0.f);
        return finite_state(s);
    }
public:
    void reset(){
        anchor_={};state_={};anchor_completed_=completed_=0;valid_=false;history_.clear();peers_={};
        player_contact_=view_was_blocked_=view_reconciling_=false;
        view_completed_=0;view_time_=0.;view_position_.reset();water_entry_.reset();
    }
    uint64_t completed() const {return completed_;}
    std::optional<State> state() const {return valid_?std::optional<State>{state_}:std::nullopt;}
    const std::optional<State>& water_entry() const {return water_entry_;}
    std::optional<float> head_compression() const {return valid_?std::optional<float>{state_.curve[5]}:std::nullopt;}
    bool falling(bool fallback=false) const {return valid_?state_.phase==3 && state_.velocity[1]<0.f:fallback;}

    template<class Sweep>
    MovementPoint camera_position(double now,const MovementTuning& tuning,Sweep&& sweep){
        const auto target=state_.position;
        auto position=target;
        if(state_.phase==0 && view_position_ && completed_>=view_completed_ &&
           (view_reconciling_ || view_was_blocked_ || player_contact_)){
            const auto delta=movement_difference(target,*view_position_);
            const float distance=movement_length(delta);
            const float seconds=float(std::clamp(now-view_time_,0.,.05));
            const float step_seconds=float(std::min<uint64_t>(completed_-view_completed_,5))*.01f;
            const float speed=tuning.ground_decay>0.f?
                std::clamp(tuning.ground_acceleration/tuning.ground_decay,1.f,160.f):160.f;
            const float limit=std::max(speed,movement_length(state_.velocity))*std::max(seconds,step_seconds)*1.4f;
            view_reconciling_=distance>limit;
            if(view_reconciling_){
                // A newer checkpoint can advance multiple previously blocked
                // commands at once. Keep the correction in the view only.
                for(unsigned axis=0;axis<3;++axis)
                    position[axis]=(*view_position_)[axis]+delta[axis]*(limit/distance);
                const auto clipped=sweep(*view_position_,position);
                if(clipped)position=*clipped;
                else{position=target;view_reconciling_=false;}
            }
        }else view_reconciling_=false;
        view_position_=position;view_completed_=completed_;view_time_=now;
        view_was_blocked_=player_contact_;
        return position;
    }

    template<class Resolve>
    std::optional<MovementPoint> sample(uint64_t completed,const State& authority,std::span<const Command> commands,
                                       const MovementTuning& tuning,Resolve&& resolve,const PeerPositions& peers={}){
        if(!finite_state(authority) || !finite_tuning(tuning) || commands.size()>512 ||
           completed==std::numeric_limits<uint64_t>::max()) {reset();return {};}
        uint64_t expected=completed;
        for(const auto& command:commands){
            if(expected==std::numeric_limits<uint64_t>::max() || command.sequence!=++expected ||
               !std::isfinite(command.yaw) || std::abs(command.yaw)>1000000.f){reset();return {};}
        }
        if(valid_ && completed<anchor_completed_){reset();return {};}
        const bool changed=!valid_ || completed!=anchor_completed_ || !same(authority,anchor_) || peers!=peers_;
        if(changed){
            anchor_=authority;state_=authority;anchor_completed_=completed_=completed;history_.clear();peers_=peers;valid_=true;
            player_contact_=false;
        }else{
            if(commands.size()<history_.size()){reset();return {};}
            for(size_t i=0;i<history_.size();++i)if(!same(commands[i],history_[i])){reset();return {};}
        }
        for(size_t i=history_.size();i<commands.size();++i){
            if(!step(commands[i],tuning,resolve)){
                const auto water=water_entry_;
                reset();
                water_entry_=water;
                return {};
            }
            completed_=commands[i].sequence;history_.push_back(commands[i]);
        }
        return state_.position;
    }
};
}
