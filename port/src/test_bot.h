#pragma once
#include "netplay.h"
#include "ssne/bot.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
namespace port::netplay {
class CompanionBot {
    uint32_t target_ = 0;
    std::array<float,3> previous_{};
    unsigned stalled_ = 0, escape_ = 0;
    bool moving_ = false;
    std::deque<std::array<float,3>> trail_;
public:
    struct Checkpoint {uint32_t target=0;std::array<float,3> previous{};unsigned stalled=0,escape=0;bool moving=false;std::deque<std::array<float,3>> trail;};
    Checkpoint checkpoint() const {return {target_,previous_,stalled_,escape_,moving_,trail_};}
    bool restore_checkpoint(Checkpoint state) {
        auto valid=[](const auto& p){return std::all_of(p.begin(),p.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1e9f;});};
        if(state.stalled>75 || state.escape>60 || state.trail.size()>128 || !valid(state.previous) || !std::all_of(state.trail.begin(),state.trail.end(),valid))return false;
        target_=state.target;previous_=state.previous;stalled_=state.stalled;escape_=state.escape;moving_=state.moving;trail_.swap(state.trail);return true;
    }
    uint32_t target() const { return target_; }
    Input update(const ssne::BotWorld& world, unsigned decision_ticks=1,const std::array<float,3>* fire_origin=nullptr,
                 const std::function<bool(const std::array<float,3>&)>& route_clear={},bool budgeted_turn=false,bool autonomous=false,float target_range=-1.f) {
        Input input;
        if (!world.host.id || !world.client.id || !std::isfinite(world.yaw) || !std::isfinite(world.pitch)) { *this={}; return input; }
        const auto& p=world.client.position;
        auto horizontal=[&](const ssne::BotActor& a) { return std::hypot(a.position[0]-p[0],a.position[2]-p[2]); };
        float host_distance=horizontal(world.host);
        if (!std::isfinite(host_distance)) return input;
        // Follow the host's recent route around corners rather than always
        // walking directly into the wall between the two players.
        if (trail_.empty() || std::hypot(world.host.position[0]-trail_.back()[0],world.host.position[2]-trail_.back()[2])>1.5f || std::abs(world.host.position[1]-trail_.back()[1])>.6f)
            trail_.push_back(world.host.position);
        if (trail_.size()>128) trail_.pop_front();
        // Rejoin the route after passing a waypoint or returning to the leader.
        // Height matters: a point on a floor above us has not been reached.
        auto reached=trail_.begin();
        for(auto it=trail_.begin();it!=trail_.end();++it)
            if(std::hypot((*it)[0]-p[0],(*it)[2]-p[2])<2.f && std::abs((*it)[1]-p[1])<1.f)reached=it;
        trail_.erase(trail_.begin(),reached);
        while(trail_.size()>1 && std::hypot(trail_.front()[0]-p[0],trail_.front()[2]-p[2])<2.f && std::abs(trail_.front()[1]-p[1])<1.f)trail_.pop_front();
        // Only bypass an old route if native swept collision AND support-floor
        // queries confirm the shortcut. A visual line of sight is insufficient.
        if(trail_.size()>1 && route_clear && route_clear(trail_.back()))trail_.erase(trail_.begin(),std::prev(trail_.end()));
        const ssne::BotActor* enemy=nullptr;
        const float range=std::isfinite(target_range)?(target_range<0.f?(autonomous?60.f:30.f):std::clamp(target_range,0.f,100.f)):0.f;
        float best=range+1.f;
        // Stay with the host; retain a target to avoid flickering between enemies.
        for (const auto& actor:world.enemies) {
            float distance=horizontal(actor);
            if (!actor.id || !std::isfinite(actor.health) || actor.health<=0 || !std::isfinite(distance) || distance<.1f || distance>range || !std::isfinite(actor.position[1]) || !std::isfinite(actor.height)) continue;
            if (!autonomous && host_distance>=18 && distance>8) continue; // defend while catching up
            float score=distance*(actor.id==target_?.7f:1.f);
            if (score<best) { best=score; enemy=&actor; }
        }
        target_=enemy?enemy->id:0;
        const auto& aim=enemy?*enemy:world.host;
        auto eye=p;eye[1]+=1.5f;if(fire_origin)eye=*fire_origin;
        float dx=aim.position[0]-eye[0], dz=aim.position[2]-eye[2];
        float desired_yaw=std::atan2(dx,dz);
        float desired_pitch=enemy ? -std::atan2(aim.position[1]+aim.height*.5f-eye[1],std::hypot(dx,dz)) : 0;
        float yaw_error=std::remainder(desired_yaw-world.yaw,6.283185307f);
        float pitch_error=std::clamp(desired_pitch,-1.1f,1.1f)-world.pitch;
        // A finite turn budget, spent once over subsequent native updates.
        // Replaying a fixed delta every tick overshoots at high frame rates.
        input.yaw=int32_t(std::clamp(yaw_error*.85f,-.5f,.5f)*1000000);
        // Direct mouse input subtracts its vertical delta from guest pitch.
        input.pitch=int32_t(-std::clamp(pitch_error*.85f,-.35f,.35f)*1000000);
        if(!budgeted_turn){ // Legacy per-frame client test controller.
            input.yaw=int32_t(std::clamp(yaw_error*.25f,-.055f,.055f)*1000000);
            input.pitch=int32_t(-std::clamp(pitch_error*.25f,-.035f,.035f)*1000000);
        }
        if (enemy && std::abs(yaw_error)<.07f && std::abs(pitch_error)<.08f) {
            input.pad.buttons|=ssne::PAD_R; input.pad.trigger_r=255;
        }
        if (moving_ && std::hypot(p[0]-previous_[0],p[2]-previous_[2])<.008f) stalled_+=decision_ticks;
        else stalled_=0;
        if (stalled_>75) { escape_=60; stalled_=0; }
        previous_=p;
        moving_=host_distance>(autonomous?.3f:3.5f) || std::abs(world.host.position[1]-p[1])>1.2f;
        if(!moving_){stalled_=0;escape_=0;}
        if (moving_) {
            auto destination=autonomous?world.host.position:trail_.front();
            float route_distance=std::hypot(destination[0]-p[0],destination[2]-p[2]);
            if (route_distance<.1f) { destination=world.host.position; route_distance=host_distance; }
            // Directly below a leader, retain the route without dividing by zero.
            route_distance=std::max(route_distance,.1f);
            float x=(destination[0]-p[0])/route_distance;
            float z=(destination[2]-p[2])/route_distance;
            float right=x*std::cos(world.yaw)-z*std::sin(world.yaw);
            float forward=x*std::sin(world.yaw)+z*std::cos(world.yaw);
            float speed=host_distance>7?105.f:75.f;
            input.pad.stick_x=uint8_t(std::clamp(128+int(std::round(right*speed)),0,255));
            input.pad.stick_y=uint8_t(std::clamp(128+int(std::round(forward*speed)),0,255));
            if (world.host.position[1]-p[1]>.8f && stalled_>20 && stalled_<=20+decision_ticks) {
                input.pad.buttons|=ssne::PAD_L;input.pad.trigger_l=255;
            }
        }
        if (escape_) {
            // L is the native gameplay jump, A is use. Emit the first pulse
            // before spending this decision's ticks (the AFK budget is eight).
            if (escape_==60) {input.pad.buttons|=ssne::PAD_L;input.pad.trigger_l=255;}
            escape_=escape_>decision_ticks?escape_-decision_ticks:0;
            input.pad.stick_x=escape_>30?230:26;
        }
        return input;
    }
};
}
