#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include "native_movement_input.h"

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
    float minimum_ground_motion=.025f;
    // PAL's sliding branch uses the unmodified walking parameter objects,
    // even when skates or a combo changed the regular grounded values.
    // A zero pair keeps callers that supply only the regular values valid.
    float default_ground_acceleration=0,default_ground_decay=0;
};
struct MovementContact {
    MovementPoint position{};
    bool grounded=false;
    bool entered_water=false;
    uint32_t contact_kind=0;
    bool sliding=false;
    bool player_contact=false;
};
enum class MovementResolvePurpose { Motion, Correction, Support };
struct MovementCollisionContext {
    MovementResolvePurpose purpose;
    float dt;
    // PAL's pending lift, contact point and cooldown belong to the local
    // walking controller. Probes and reconciliation receive a disposable copy.
    std::array<float,5>& ground_state;
};
struct SwimInput {
    MovementPoint acceleration{};
    float drag=1.f;
    bool surface=false;
    float capsule_diameter=0.f;
    float surface_jump_speed=0.f;
};

class LocalMovement {
    enum class Phase { Ground, JumpCurve, Fall, EnterWater, Swim };
    struct Step {uint64_t sequence;MovementPoint delta;};
    // The authoritative PAL walking controller runs with 10ms physics steps.
    // Its lift is consumed on the following step, so prediction shares that clock.
    static constexpr double tick=.01;
    static constexpr float dt=float(tick);
    std::deque<Step> steps_;
    MovementPoint position_{},velocity_{},outstanding_{};
    std::array<float,5> ground_state_{};
    double previous_=0,elapsed_=0;
    float jump_time_=0,blocked_correction_time_=0;
    float jump_start_height_=0,previous_jump_height_=0;
    float head_compression_=0;
    std::optional<float> walked_off_height_;
    struct SurfaceJump {
        double started;
        bool host_left_water=false;
    };
    std::optional<SurfaceJump> surface_jump_;
    uint64_t jump_seen_=0;
    uint64_t water_entry_input_=0;
    Phase phase_=Phase::Ground;
    bool initialized_=false;
    bool head_compression_valid_=false;
    struct StopView {
        MovementPoint eye{},body{},direction{},offset{},start{},anchor{};
        double at=0.;
        uint64_t last_driven=0;
        bool valid=false,pressed=false,following=false,settled=false;
    } view_;
    struct TransientContact {
        MovementPoint anchor{};
        double since=0.;
        bool active=false,holding=true;
    } contact_;

    static bool native_ground_motion_suppressed(const MovementPoint& velocity,const MovementTuning& tuning){
        // PAL retains braking velocity after its horizontal 10ms displacement
        // is too small to move the walking capsule. That is already a visible
        // stop, not another 200ms of motion while velocity approaches zero.
        if(tuning.minimum_ground_motion>0.f && std::isfinite(tuning.minimum_ground_motion)){
            if(std::abs(velocity[1])>=.06f)return false; // Keep vertical pushes/support changes active.
            return std::hypot(velocity[0],velocity[2])*dt<tuning.minimum_ground_motion;
        }
        return movement_length(velocity)<.06f;
    }

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
    std::optional<float> head_compression() const {
        if(!initialized_ || !head_compression_valid_ || phase_==Phase::Swim || phase_==Phase::EnterWater)return {};
        return head_compression_;
    }
    bool entering_water() const {return initialized_ && phase_==Phase::EnterWater;}
    bool surface_jump_active() const {return surface_jump_.has_value();}
    bool leaving_water() const {return surface_jump_active() && !entering_water();}
    std::optional<MovementPoint> swimming_position() const {
        if(!initialized_ || (phase_!=Phase::EnterWater && phase_!=Phase::Swim))return {};
        return position_;
    }
    bool falling(bool fallback) const {
        return initialized_?(phase_==Phase::Fall && velocity_[1]<0.f):fallback;
    }

    void reset(){
        view_={};
        contact_={};
        reset_prediction();
    }
    void enter_water(double now,const MovementAnchor& host,const MovementIntent& input,
                     const MovementPoint& position,MovementPoint velocity){
        reset();
        initialized_=true;
        phase_=Phase::EnterWater;
        position_=position;
        const float speed=movement_length(velocity);
        if(speed>5.f)for(auto& axis:velocity)axis*=5.f/speed;
        velocity_=velocity;
        jump_seen_=input.jump;
        water_entry_input_=input.sequence;
        previous_=now;
        elapsed_=0;
        if(input.sequence>host.acknowledged)
            record(input.sequence,movement_difference(position,host.position));
    }
    template<class Clip>
    MovementPoint follow_brush_push(double now,const MovementPoint& host,Clip&& clip,bool preserve_velocity=false){
        // The leaf controls the capsule until PAL releases it. Start at
        // the last local eye, clipped from the host's confirmed side.
        // Switching to the remote interpolation buffer would replay a
        // section of the approach every time a door starts pushing us.
        auto point=host;
        auto contact=preserve_velocity?contact_:TransientContact{};
        if(preserve_velocity && !contact.active)
            contact={view_.valid?view_.anchor:host,now,true,true};
        if(initialized_ && now-previous_<.25){
            const auto previous=view_.valid?view_.eye:position_;
            if(previous!=host && movement_length(movement_difference(previous,host))<8.f){
                const auto allowed=clip(host,previous);
                float keep=float(std::exp(-18.*std::clamp(now-previous_,0.,.25)));
                if(preserve_velocity && contact.holding){
                    const auto advance=movement_difference(host,contact.anchor);
                    const auto lead=movement_difference(previous,contact.anchor);
                    const float towards=advance[0]*lead[0]+advance[2]*lead[2];
                    // A brief unsupported contact can arrive while the host
                    // is still acknowledging the approach. Keep the verified
                    // local eye until it catches up, but honor a real push back.
                    if(towards<-.00001f || now-contact.since>.15)contact.holding=false;
                    if(contact.holding)keep=1.f;
                }
                for(unsigned i=0;i<3;++i)point[i]=host[i]+(allowed[i]-host[i])*keep;
            }
        }
        contact.anchor=host;
        const auto resume_velocity=preserve_velocity?velocity_:MovementPoint{};
        reset();
        contact_=contact;
        initialized_=true;phase_=Phase::Ground;position_=point;velocity_=resume_velocity;
        previous_=now;elapsed_=0;
        return point;
    }
private:
    void reset_prediction(){
        steps_.clear();
        outstanding_={};
        blocked_correction_time_=0;
        walked_off_height_.reset();
        surface_jump_.reset();
        ground_state_={};
        head_compression_=0;
        head_compression_valid_=false;
        initialized_=false;
    }
    // The resolver may decline an unsafe prediction. Never move the gameplay actor.
    template<class Resolve>
    std::optional<MovementPoint> simulate(double now,const MovementAnchor& host,const MovementIntent& input,
                                        const MovementTuning& tuning,Resolve&& resolve,const SwimInput* swim=nullptr,
                                        const SwimInput* entry_swim=nullptr){
        if(surface_jump_){
            if(!swim)surface_jump_->host_left_water=true;
            // The dry samples can arrive after the local capsule has already
            // re-entered. Wait for the host's next swim phase instead of
            // replaying its entire fall on the local camera.
            const bool reentry_confirmed=entering_water() && swim && surface_jump_->host_left_water;
            if(reentry_confirmed || now-surface_jump_->started>.95){
                if(leaving_water() && swim)reset_prediction();
                else surface_jump_.reset();
            }
        }
        if(!initialized_){
            if(!host.grounded && !swim)return {};
            position_=host.position;
            velocity_=host.velocity;
            phase_=swim?Phase::Swim:Phase::Ground;
            jump_seen_=host.acknowledged;
            previous_=now;
            elapsed_=0.;
            initialized_=true;
            head_compression_=0;
            head_compression_valid_=!swim;
        }
        if(swim && phase_!=Phase::Swim && !surface_jump_active()){
            if(phase_!=Phase::EnterWater)velocity_=host.velocity;
            phase_=Phase::Swim;
            head_compression_valid_=false;
            blocked_correction_time_=0;
        }else if(!swim && (phase_==Phase::Swim ||
                (phase_==Phase::EnterWater && !surface_jump_active() &&
                 (!entry_swim || host.acknowledged>=water_entry_input_)))){
            phase_=host.grounded?Phase::Ground:Phase::Fall;
            velocity_=host.velocity;
            jump_seen_=std::max(input.jump,host.acknowledged);
            head_compression_=0;
            head_compression_valid_=true;
        }
        const bool wants_move=input.right || input.forward;
        const auto jump=std::max(jump_seen_,host.acknowledged);
        acknowledge(host.acknowledged);
        const double frame_time=std::clamp(now-previous_,0.,.05);
        elapsed_+=frame_time;
        previous_=now;
        const bool settling=!wants_move && input.jump<=jump && phase_==Phase::Ground && host.grounded;
        // The local eye has already reached this part of the level. Giving
        // it back to the remote interpolation buffer at rest would play the
        // end of the step a second time, starting from an older position.
        constexpr float settled_distance=.001f;
        if(settling && steps_.empty() && ground_state_[0]==0.f &&
           native_ground_motion_suppressed(velocity_,tuning) && native_ground_motion_suppressed(host.velocity,tuning)){
            const auto separation=movement_difference(host.position,position_);
            const float distance=movement_length(separation);
            if(distance<=settled_distance){reset_prediction();return host.position;}
            if(distance<.10f){
                // Both resting positions are collision-resolved. A further
                // sweep of a millimetre can stick to the rim of a bevel; ease
                // just the eye onto the confirmed endpoint in render time.
                const float weight=float(-std::expm1(-12.*frame_time));
                for(unsigned i=0;i<3;++i)position_[i]+=separation[i]*weight;
                elapsed_=0;
                return position_;
            }
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
            const auto* water_input=leaving_water()?nullptr:(swim?swim:(entering_water()?entry_swim:nullptr));
            if(water_input && input.jump>jump_seen_){
                jump_seen_=input.jump;
                if(!surface_jump_active() && water_input->surface && input.jump>host.acknowledged &&
                   water_input->surface_jump_speed>0.f){
                    // PAL 800AACC0 consumes JumpPressed in swim phase 2.
                    // It clears horizontal speed, gives the body an upward
                    // velocity, then switches to the original land controller.
                    velocity_={0.f,water_input->surface_jump_speed,0.f};
                    phase_=Phase::Fall;
                    ground_state_={};
                    surface_jump_=SurfaceJump{now};
                    head_compression_=0.f;
                    continue;
                }
            }
            if(!water_input && input.jump>std::max(jump_seen_,host.acknowledged)){
                jump_seen_=input.jump;
                if(phase_==Phase::Ground){
                    // Confirm current support locally before starting a deliberate jump.
                    const auto support=resolve(position_,position_,true,MovementResolvePurpose::Support);
                    if(support && support->grounded){
                        phase_=Phase::JumpCurve;
                        jump_time_=0;
                        jump_start_height_=previous_jump_height_=position_[1];
                    }
                }
            }

            // Reconcile against the host pose plus the locally resolved steps
            // that the host has yet to acknowledge. Sweep corrections as well.
            const auto confirmed_error=movement_difference(host.position,position_);
            auto error=confirmed_error;
            for(unsigned axis=0;axis<3;++axis)error[axis]+=outstanding_[axis];
            // A jump snapshot acknowledges an input sample, not the native
            // curve clock within that sample. Adding its vertical phase lead
            // to a locally running original curve would raise the same jump
            // twice. Ground authority can still correct a denied jump once
            // the press is acknowledged; falling and collision keep their
            // normal reconciliation paths.
            if(phase_==Phase::JumpCurve && (!host.grounded || input.jump>host.acknowledged))error[1]=0;
            if(phase_==Phase::Ground && !host.grounded)error[1]=0;
            // The swimming anchor predates the switch. Its height is no
            // longer a valid target for the locally running launch arc.
            if(surface_jump_active()){
                if(swim || entering_water())error={};
                else error[1]=0.f;
            }
            const auto discrepancy=movement_length(error);
            bool blocked_correction=false;
            if(discrepancy>(settling ? settled_distance : .04f)){
                auto corrected=position_;
                const float ratio=std::min(.13f, .10f/discrepancy);
                for(unsigned axis=0;axis<3;++axis)corrected[axis]+=error[axis]*ratio;
                const auto result=resolve(position_,corrected,phase_==Phase::Ground,MovementResolvePurpose::Correction);
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
                head_compression_valid_=false;
                MovementPoint desired=position_;
                for(unsigned i=0;i<3;++i){
                    velocity_[i]+=(water_input->acceleration[i]-water_input->drag*velocity_[i])*dt;
                    desired[i]+=velocity_[i]*dt;
                }
                if(water_input->surface && desired[1]>std::max(position_[1],host.position[1])){
                    desired[1]=std::max(position_[1],host.position[1]);
                    velocity_[1]=0.f;
                }
                const auto water=resolve(position_,desired,false,MovementResolvePurpose::Motion);
                if(!water){reset();return {};}
                if(velocity_[1]>0.f && water->position[1]<desired[1]-.001f)
                    velocity_[1]=0.f;
                if(input.sequence>host.acknowledged)
                    record(input.sequence,movement_difference(water->position,position_));
                position_=water->position;
                continue;
            }
            const bool walking=phase_==Phase::Ground;
            head_compression_valid_=true;
            if(phase_!=Phase::JumpCurve)
                head_compression_=std::max(0.f,float(double(head_compression_)-.01f));
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
                // PAL samples the current curve time before advancing its
                // clock. Store the rounded absolute target, including the
                // launch height, so the next displacement uses the same
                // subtraction even when collision changes the actual body.
                const float ratio=float(double(jump_time_)/tuning.jump_duration);
                head_compression_=float(double(ratio)*.2f);
                const float angle=float(double(ratio)*pi);
                const float sine=float(std::sin(double(angle)));
                const float height=float(double(sine)*tuning.jump_height+jump_start_height_);
                const float rise=float(double(height)-previous_jump_height_);
                previous_jump_height_=height;
                desired[1]+=rise;
                velocity_[1]=float(double(rise)*50.f);
            }else if(phase_==Phase::Fall){
                velocity_[1]-=98.f*dt;
                desired[1]+=velocity_[1]*dt;
            }else velocity_[1]=0;

            if(walking && position_==desired && ground_state_[0]==0.f)continue;
            const auto result=resolve(position_,desired,walking,MovementResolvePurpose::Motion);
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
            else if(!leaving_water() && desired[1]>position_[1] &&
                    result->position[1]<desired[1]-.025f){
                phase_=Phase::Fall;
                velocity_[1]=0;
            }else if(phase_==Phase::Fall && result->grounded &&
                     (leaving_water() || desired[1]<=position_[1])){
                phase_=Phase::Ground;
                velocity_[1]=0;
                walked_off_height_.reset();
            }
            if(phase_==Phase::JumpCurve){
                jump_time_=float(double(jump_time_)+dt);
                if(jump_time_>=float(double(tuning.jump_duration)*.55f))phase_=Phase::Fall;
            }
            position_=result->position;
            if(input.sequence>host.acknowledged)record(input.sequence,delta);
        }
        if(leaving_water() && !swim && phase_==Phase::Ground && host.grounded)
            surface_jump_.reset();
        return position_;
    }
public:
    template<class Resolve>
    std::optional<MovementPoint> sample(double now,const MovementAnchor& host,const MovementIntent& input,
                                        const MovementTuning& tuning,Resolve&& resolve,const SwimInput* swim=nullptr,
                                        const SwimInput* entry_swim=nullptr){
        contact_={};
        auto collide=[&](const MovementPoint& from,const MovementPoint& to,bool walking,
                         MovementResolvePurpose purpose){
            auto state=purpose==MovementResolvePurpose::Motion && walking?ground_state_:std::array<float,5>{};
            MovementCollisionContext query{purpose,purpose==MovementResolvePurpose::Motion?dt:0.f,state};
            const auto contact=[&](){
                if constexpr(requires {resolve(from,to,walking,query);})return resolve(from,to,walking,query);
                else return resolve(from,to,walking);
            }();
            if(contact && purpose==MovementResolvePurpose::Motion)ground_state_=state;
            return contact;
        };
        const auto body=simulate(now,host,input,tuning,collide,swim,entry_swim);
        if(!body || swim || !host.grounded || phase_!=Phase::Ground){
            view_={};
            return body;
        }
        const bool pressed=input.right || input.forward;
        auto& view=view_;
        if(!view.valid){
            view.eye=view.body=*body;
            view.start=view.anchor=host.position;
            view.at=now;
            view.valid=true;
        }
        const float frame=float(std::clamp(now-view.at,0.,.05));
        const auto advance=movement_difference(host.position,view.anchor);
        const auto along=[&](const MovementPoint& v){
            return v[0]*view.direction[0]+v[2]*view.direction[2];
        };
        if(pressed){
            if(!view.pressed)view.start=host.position;
            const float length=std::hypot(input.right,input.forward);
            const float sine=std::sin(input.yaw),cosine=std::cos(input.yaw);
            view.direction={(input.right*cosine+input.forward*sine)/length,0,
                            (input.forward*cosine-input.right*sine)/length};
            view.last_driven=input.sequence;
            view.following=view.settled=false;
        }else if(view.pressed){
            view.following=true;
        }
        if(view.following){
            // Only absorb reversal of our own walk estimate. A push back,
            // a relocation after settling, or a tap the host never applied
            // must still move the eye to the actual authoritative player.
            if(along(advance)<-.001f || (view.settled && movement_length(advance)>.001f))
                view.following=false;
            if(host.acknowledged>=view.last_driven && ground_state_[0]==0.f &&
               native_ground_motion_suppressed(host.velocity,tuning)){
                view.settled=true;
                // The host has finished this step. Retire the temporary
                // key-up smoothing offset gradually, including a successful
                // voluntary step, instead of keeping a detached eye forever.
                view.following=false;
            }
        }
        if(view.following && tuning.ground_decay>0.f){
            // The native host and the client can put key-up on opposite
            // simulation ticks. Absorb at most one walking tick along the
            // released direction. The gameplay position is already corrected.
            const float limit=std::min(.25f,dt*tuning.ground_acceleration/tuning.ground_decay);
            const float held=std::clamp(along(movement_difference(view.eye,*body)),0.f,limit);
            for(unsigned i:{0u,2u})view.offset[i]=held*view.direction[i];
        }else{
            const float decay=std::exp(-20.f*frame);
            for(unsigned i:{0u,2u})view.offset[i]*=decay;
        }
        MovementPoint eye=*body;
        if(std::hypot(view.offset[0],view.offset[2])>.0005f){
            eye[0]+=view.offset[0];eye[2]+=view.offset[2];
            if(eye!=view.eye || *body!=view.body){
                // A visual offset is safe only on the same walkable surface.
                // Let a wall or a ledge take precedence over stop smoothing.
                const auto contact=collide(*body,eye,true,MovementResolvePurpose::Correction);
                if(!contact || !contact->grounded || contact->entered_water ||
                        std::abs(contact->position[1]-(*body)[1])>=.01f ||
                        std::hypot(contact->position[0]-eye[0],contact->position[2]-eye[2])>.001f)
                    eye=*body;
            }
        }
        view.offset=movement_difference(eye,*body);
        view.eye=eye;view.body=*body;view.anchor=host.position;
        view.pressed=pressed;view.at=now;
        return eye;
    }
};
}

