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

    struct FollowGoal {
        std::array<float,3> position;
        bool directly_reachable;
    };

    FollowGoal follow_goal(const std::array<float,3>& follower, const std::array<float,3>& leader, bool grounded,
                           const std::function<bool(const std::array<float,3>&)>& route_clear) {
        auto horizontal = [](const auto& a, const auto& b) { return std::hypot(a[0]-b[0], a[2]-b[2]); };
        auto reached = [&](const auto& point) {
            return horizontal(follower,point)<.42f && std::abs(follower[1]-point[1])<.38f;
        };
        if(grounded) {
            if(!trail_.empty() && (horizontal(trail_.back(),leader)>24.f || std::abs(trail_.back()[1]-leader[1])>12.f))
                trail_.clear();
            if(trail_.empty() || horizontal(trail_.back(),leader)>.48f || std::abs(trail_.back()[1]-leader[1])>.28f)
                trail_.push_back(leader);
            if(trail_.size()>128)trail_.pop_front();
        }
        if(trail_.empty())return {follower,true};

        const auto latest = grounded ? leader : trail_.back();
        if(reached(latest) || route_clear(latest)) {
            // The current leader or last landing is reachable without a detour.
            trail_.erase(trail_.begin(),std::prev(trail_.end()));
            return {latest,true};
        }
        while(trail_.size()>1 && reached(trail_.front()))trail_.pop_front();

        // Try the most recent nearby points on a reachable tread. A point
        // several steps above or below us should not hide a clear next turn.
        if(trail_.size()>2) {
            unsigned checks=0;
            for(auto shortcut=std::prev(trail_.end(),2);shortcut!=trail_.begin() && checks<2;--shortcut){
                if(horizontal(follower,*shortcut)>10.f || std::abs(follower[1]-(*shortcut)[1])>.85f)continue;
                ++checks;
                if(route_clear(*shortcut)){trail_.erase(trail_.begin(),shortcut);break;}
            }
        }
        return {trail_.size()>1 ? trail_.front() : latest,false};
    }

    Input update(const ssne::BotWorld& world, unsigned decision_ticks=1,const std::array<float,3>* fire_origin=nullptr,
                 const std::function<bool(const std::array<float,3>&)>& route_clear={},bool budgeted_turn=false,bool autonomous=false,float target_range=-1.f,
                 bool leader_grounded=true,const std::array<float,3>* navigation_waypoint=nullptr) {
        Input input;
        if (!world.host.id || !world.client.id || !std::isfinite(world.yaw) || !std::isfinite(world.pitch)) { *this={}; return input; }
        const auto& p=world.client.position;
        auto valid=[](const auto& point){return std::all_of(point.begin(),point.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1e9f;});};
        if(!valid(p) || !valid(world.host.position)){*this={};return input;}
        decision_ticks=std::clamp(decision_ticks,1u,75u);
        auto horizontal=[&](const ssne::BotActor& a) { return std::hypot(a.position[0]-p[0],a.position[2]-p[2]); };
        float host_distance=horizontal(world.host);
        if (!std::isfinite(host_distance)) return input;
        // Follow the host's recent route around corners rather than always
        // walking directly into the wall between the two players.
        // Native support evidence distinguishes a walkable leader sample from
        // a jump. A height-only breadcrumb can never be reached after landing.
        // Autonomous goals are already validated navigation waypoints.
        if (!navigation_waypoint && (autonomous || leader_grounded) && (trail_.empty() || std::hypot(world.host.position[0]-trail_.back()[0],world.host.position[2]-trail_.back()[2])>1.5f || std::abs(world.host.position[1]-trail_.back()[1])>.6f))
            trail_.push_back(world.host.position);
        if (trail_.size()>128) trail_.pop_front();
        const auto follow_position=navigation_waypoint?*navigation_waypoint:
            !autonomous && !leader_grounded?(trail_.empty()?p:trail_.back()):world.host.position;
        const float follow_distance=std::hypot(follow_position[0]-p[0],follow_position[2]-p[2]);
        // Rejoin the route after passing a waypoint or returning to the leader.
        // Height matters: a point on a floor above us has not been reached.
        auto reached_point=[&](const auto& point){
            if(std::hypot(point[0]-p[0],point[2]-p[2])<.75f && std::abs(point[1]-p[1])<.6f)return true;
            const float x=p[0]-previous_[0],z=p[2]-previous_[2],length=x*x+z*z;
            if(!moving_ || length<.001f || length>25.f)return false;
            const float t=std::clamp(((point[0]-previous_[0])*x+(point[2]-previous_[2])*z)/length,0.f,1.f);
            return std::hypot(point[0]-previous_[0]-t*x,point[2]-previous_[2]-t*z)<.5f && std::abs(point[1]-previous_[1]-t*(p[1]-previous_[1]))<.6f;
        };
        if(!navigation_waypoint){
        auto reached=trail_.begin();
        for(auto it=trail_.begin();it!=trail_.end();++it)if(reached_point(*it))reached=it;
        trail_.erase(trail_.begin(),reached);
        while(trail_.size()>1 && reached_point(trail_.front()))trail_.pop_front();
        // Check at most two route shortcuts. A far-away newest breadcrumb must
        // not hide a reachable intermediate corner in the human's route.
        if(trail_.size()>1 && route_clear){
            auto shortcut=trail_.end();
            for(auto it=trail_.begin();it!=trail_.end();++it)
                if(std::hypot((*it)[0]-p[0],(*it)[2]-p[2])<=12.f)shortcut=it;
            if(shortcut!=trail_.end() && route_clear(*shortcut))trail_.erase(trail_.begin(),shortcut);
            else if(trail_.size()>2 && route_clear(trail_[1]))trail_.pop_front();
        }
        if(!trail_.empty() && follow_distance<2.f && std::abs(follow_position[1]-p[1])<.6f && !autonomous)
            trail_.erase(trail_.begin(),std::prev(trail_.end()));
        }
        const ssne::BotActor* enemy=nullptr;
        const float range=std::isfinite(target_range)?(target_range<0.f?(autonomous?60.f:30.f):std::clamp(target_range,0.f,250.f)):0.f;
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
        // Slow scraping against a wall is a stall too. Count only actual
        // movement requests; planning and standing near the human are idle.
        // A native follow waypoint has its own terrain search and short-edge
        // recovery. A large legacy sidestep can suppress it in a narrow hall.
        const bool legacy_escape=!navigation_waypoint || autonomous;
        if(!legacy_escape)escape_=0;
        if (legacy_escape && moving_ && !escape_ && std::hypot(p[0]-previous_[0],p[2]-previous_[2])<.06f) stalled_+=decision_ticks;
        else stalled_=0;
        if (stalled_>32) { escape_=60; stalled_=0; }
        previous_=p;
        const bool detour=!navigation_waypoint && !autonomous && trail_.size()>1 && follow_distance>=2.f;
        // Navigation can return fine-grid points less than .3 m away.
        // Keep walking until its tighter arrival test can consume the point.
        moving_=follow_distance>(autonomous || navigation_waypoint ? .1f : 3.5f) || std::abs(follow_position[1]-p[1])>1.2f || detour;
        if(!moving_){stalled_=0;escape_=0;}
        if (moving_) {
            auto destination=navigation_waypoint || autonomous || trail_.empty()?follow_position:trail_.front();
            float route_distance=std::hypot(destination[0]-p[0],destination[2]-p[2]);
            if (route_distance<.1f) { destination=follow_position; route_distance=follow_distance; }
            const float route_x=(destination[0]-p[0])/std::max(route_distance,.1f);
            const float route_z=(destination[2]-p[2])/std::max(route_distance,.1f);
            bool safe_escape=true;
            if(escape_ && route_clear){
                safe_escape=false;
                const float side=world.client.id%2?1.f:-1.f;
                const float angle=side*(escape_>30?.78539816f:1.57079633f);
                for(float turn:{angle,-angle}){
                    auto candidate=p;
                    candidate[0]+=(route_x*std::cos(turn)-route_z*std::sin(turn))*2.5f;
                    candidate[2]+=(route_x*std::sin(turn)+route_z*std::cos(turn))*2.5f;
                    if(route_clear(candidate)){destination=candidate;route_distance=2.5f;safe_escape=true;break;}
                }
            }
            const float x=(destination[0]-p[0])/std::max(route_distance,.1f);
            const float z=(destination[2]-p[2])/std::max(route_distance,.1f);
            const float right=x*std::cos(world.yaw)-z*std::sin(world.yaw);
            const float forward=x*std::sin(world.yaw)+z*std::cos(world.yaw);
            const float speed=follow_distance>7?105.f:75.f;
            if(safe_escape){
                input.pad.stick_x=uint8_t(std::clamp(128+int(std::round(right*speed)),0,255));
                input.pad.stick_y=uint8_t(std::clamp(128+int(std::round(forward*speed)),0,255));
                // The native integration still checks headroom before jumping.
                if((autonomous || leader_grounded) && ((escape_==60) || (follow_position[1]-p[1]>.8f && stalled_>20 && stalled_<=20+decision_ticks))){
                    input.pad.buttons|=ssne::PAD_L;input.pad.trigger_l=255;
                }
            }
        }
        if(escape_)escape_=escape_>decision_ticks?escape_-decision_ticks:0;
        return input;
    }
};
}
