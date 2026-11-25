#include "../port/src/coop/bot_navigation.h"
#include "../port/src/test_bot.h"
#include <iostream>
#include <stdexcept>
using namespace port::coop;
using port::netplay::CompanionBot;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
ssne::BotWorld world(BotPoint leader={0,0,1.5f}){
    ssne::BotWorld out;out.client={1,{0,0,0},100,1.6f};out.host={2,leader,100,1.6f};return out;
}
auto follow(CompanionBot& bot,const ssne::BotWorld& w,bool grounded,
            const std::function<bool(const BotPoint&)>& clear={}){
    return bot.update(w,8,nullptr,clear,true,false,0.f,grounded);
}
void airborne_samples_are_not_routes(){
    CompanionBot bot;auto w=world();
    follow(bot,w,true);
    const auto anchor=bot.checkpoint().trail.back();
    for(float height:{.4f,1.2f,2.4f,3.2f,2.1f,1.1f,.4f}){
        w.host.position[1]=height;
        const auto input=follow(bot,w,false);
        require(input.pad.stick_x==128 && input.pad.stick_y==128 && !input.pad.trigger_l,"Nearby human jump caused chase movement or a guessed jump");
        const auto state=bot.checkpoint();
        require(state.trail.size()==1 && state.trail.back()==anchor,"Airborne height contaminated the grounded trail");
    }
    // No supported sample has been observed yet. Stay with the current native
    // position rather than manufacturing a landing point under an aerial goal.
    CompanionBot joining;auto airborne=world({10,4,0});unsigned queries=0;
    const auto idle=follow(joining,airborne,false,[&](const BotPoint&){++queries;return true;});
    require(joining.checkpoint().trail.empty() && !queries && idle.pad.stick_x==128 && idle.pad.stick_y==128 && !idle.pad.trigger_l,"New airborne follow invented a route or queried a guessed landing");
    w.host.position=anchor;follow(bot,w,true);
    require(bot.checkpoint().trail.back()==anchor,"Landing failed to retain the grounded anchor");
}
void repeated_jump_corner_follow(){
    CompanionBot bot;auto w=world({2,0,0});unsigned queries=0;
    auto clear=[&](const BotPoint& q){
        ++queries;const auto& from=w.client.position;
        for(unsigned n=0;n<=100;++n){const float t=float(n)/100;
            const float x=from[0]+t*(q[0]-from[0]),z=from[2]+t*(q[2]-from[2]);
            if(x>3 && x<7 && z<3.5f && z>-2)return false;
        }
        return std::abs(q[1])<.01f;
    };
    // The human walks a supported doorway/corner route, then repeatedly jumps
    // at its far end while the AFK follower is still catching up.
    for(const BotPoint& point:std::array<BotPoint,8>{{{2,0,0},{2,0,2},{2,0,4},{4,0,4},{6,0,4},{8,0,4},{8,0,2},{8,0,0}}}){
        w.host.position=point;follow(bot,w,true,clear);
    }
    auto setup=bot.checkpoint();setup.stalled=setup.escape=0;setup.moving=false;
    require(bot.restore_checkpoint(setup),"Grounded corner checkpoint setup failed");
    const BotPoint landing{8,0,0};bool moved_during_jump=false;
    for(unsigned tick=0;tick<180 && bot_distance(w.client.position,landing)>3.5f;++tick){
        const bool grounded=tick%9==0;
        w.host.position={8.f+.2f*std::sin(float(tick)),grounded?0.f:2.5f,0};
        const auto before=queries;const auto input=follow(bot,w,grounded,clear);
        require(queries-before<=4,"Jump-follow added unbounded shortcut/recovery queries");
        if(!grounded)require(!input.pad.trigger_l,"Following an airborne human guessed a jump");
        for(const auto& point:bot.checkpoint().trail)require(point[1]==0.f,"Repeated jumping retained an unreachable aerial breadcrumb");
        auto next=w.client.position;
        next[0]+=(int(input.pad.stick_x)-128)/75.f*.35f;
        next[2]+=(int(input.pad.stick_y)-128)/75.f*.35f;
        require(clear(next),"Follower cut the blocked corner while its human jumped");
        moved_during_jump|=!grounded && bot_distance(next,w.client.position)>.01f;
        w.client.position=next;
    }
    require(moved_during_jump && bot_distance(w.client.position,landing)<=3.5f,"Repeated human jumps prevented grounded corner-follow progress");
    // The retained anchor is already represented by the existing checkpoint
    // trail. No new checkpoint field or native save format is required.
    CompanionBot restored;require(restored.restore_checkpoint(bot.checkpoint()),"Jump-follow checkpoint was rejected");
    w.host.position={8,3,0};
    const auto a=follow(bot,w,false,clear),b=follow(restored,w,false,clear);
    require(a.pad.stick_x==b.pad.stick_x && a.pad.stick_y==b.pad.stick_y && a.pad.trigger_l==b.pad.trigger_l && bot.checkpoint().trail==restored.checkpoint().trail,"Checkpoint restore lost the supported follow route");
}
void supported_detour_survives_airborne_goal(){
    BotNavigation nav;BotPoint follower{0,0,0};const BotPoint landing{10,0,0};
    unsigned queries=0;bool crossed_bridge=false,advanced_in_air=false;
    auto probe=[&](const BotPoint& from,const BotPoint& desired)->std::optional<BotPoint>{
        ++queries;
        if(desired[0]<-2 || desired[0]>12 || desired[2]<-4 || desired[2]>6)return {};
        for(unsigned i=0;i<=80;++i){const float t=float(i)/80;
            const float x=from[0]+t*(desired[0]-from[0]),z=from[2]+t*(desired[2]-from[2]);
            // Unsupported gap has a real walkable crossing above z=2.5.
            if(x>3 && x<6 && z<2.5f)return {};
        }
        auto supported=desired;supported[1]=0;return supported;
    };
    const auto before=queries;nav.step(follower,landing,probe,24,true);
    require(queries-before<=24,"Initial supported search exceeded its query budget");
    for(unsigned tick=0;tick<320 && bot_distance(follower,landing)>.6f;++tick){
        const bool grounded=tick%11==0;
        const BotPoint requested{10,grounded?0.f:8.f+std::sin(float(tick)),0};
        const auto used=queries;const auto next=nav.step(follower,requested,probe,24,grounded);
        require(queries-used<=24 && nav.explored()<=2048,"Airborne-goal search exceeded its existing bounds");
        require(nav.state().goal==landing,"Aerial human Y replaced the last supported navigation target");
        if(bot_distance(follower,next)>.01f){
            require(bool(probe(follower,next)),"Retained route crossed an unsupported gap");
            const float scale=std::min(1.f,.45f/std::max(.01f,bot_distance(follower,next)));
            auto moved=follower;for(unsigned axis=0;axis<3;++axis)moved[axis]+=(next[axis]-follower[axis])*scale;
            require(bool(probe(follower,moved)),"Movement shortened a checked edge into an invalid step");
            advanced_in_air|=!grounded;
            crossed_bridge|=follower[0]<6 && moved[0]>=6 && moved[2]>=2.5f;
            follower=moved;
        }
    }
    require(advanced_in_air && crossed_bridge && bot_distance(follower,landing)<=.6f,"Jumping across a gap discarded the walkable bridge detour");
    require(nav.state().restarts<20,"Repeated aerial targets restarted grounded search work");
    BotNavigation waiting;unsigned guesses=0;
    const auto idle=waiting.step(BotPoint{0,0,0},BotPoint{9,7,0},[&](auto&,auto&)->std::optional<BotPoint>{++guesses;return {};},24,false);
    require(idle==BotPoint({0,0,0}) && guesses==0 && waiting.explored()==0,"Unsupported initial goal launched a guessed search");
}
void perception_does_not_override_follow(){
    const std::array<float,3> eye{0,.8f,0};auto w=world({0,0,2});w.enemies={{3,{0,0,180},100,1.6f}};
    CompanionBot watching;watching.update(w,8,&eye,{},true,false,250.f,true);
    require(watching.target()==3,"Explicit new perception range was still clamped to100m");
    w.host.position={0,0,25};
    const auto regroup=watching.update(w,8,&eye,{},true,false,250.f,true);
    require(!watching.target() && !regroup.pad.trigger_r && regroup.pad.stick_y>128,"Distant threat displaced the human catch-up priority");
}
void use_short_native_waypoint_in_narrow_passage(){
    CompanionBot bot;auto w=world({0,0,9});
    auto checkpoint=bot.checkpoint();
    checkpoint.moving=true;
    checkpoint.stalled=32;
    checkpoint.previous=w.client.position;
    require(bot.restore_checkpoint(checkpoint),"Narrow follow checkpoint setup failed");
    unsigned escape_checks=0;
    const BotPoint waypoint{0,0,.3f};
    const auto blocked_side=[&](const BotPoint&){++escape_checks;return false;};
    const auto input=bot.update(w,8,nullptr,blocked_side,true,false,0.f,true,&waypoint);
    require(input.pad.stick_y>128 && !input.pad.trigger_l && !escape_checks,
            "Long stall escape blocked the short checked movement through a tight passage");
}
void skip_a_stale_corner_to_the_reachable_tread(){
    CompanionBot bot;
    const BotPoint follower{0,0,0},next_step{4,.65f,3.5f},landing{4,2.8f,5.8f};
    const auto blocked=[](const BotPoint&){return false;};
    for(const auto& step:std::array<BotPoint,6>{{{4,0,0},{4,0,2},next_step,{4,1.4f,4.1f},{4,2.1f,5},landing}})
        bot.follow_goal(follower,step,true,blocked);
    unsigned queries=0;
    const auto result=bot.follow_goal(follower,landing,true,[&](const BotPoint& target){++queries;return target==next_step;});
    require(result.position==next_step && !result.directly_reachable && queries<=3,
            "A high breadcrumb hid the next accessible stair tread behind a stale corner");
}
void keep_the_first_step_around_a_tight_bend(){
    BotNavigation nav;
    const BotPoint start{0,0,0},first{.625f,0,0},second{.625f,0,.625f},goal{1.25f,0,1.25f};
    auto probe=[&](const BotPoint& from,const BotPoint& to)->std::optional<BotPoint>{
        if((from==start && to==first) || (from==first && to==second))return to;
        return {};
    };
    const auto next=nav.step(start,goal,probe,54);
    require(next==first,"A short corner waypoint was discarded before the follower took its first step");
}
}
int main(){
    try{
        airborne_samples_are_not_routes();
        repeated_jump_corner_follow();
        supported_detour_survives_airborne_goal();
        perception_does_not_override_follow();
        use_short_native_waypoint_in_narrow_passage();
        skip_a_stale_corner_to_the_reachable_tread();
        keep_the_first_step_around_a_tight_bend();
        std::cout<<"Grounded AFK follow, repeated jumps, corner/bridge detours, checkpoint retention and query bounds passed\n";
        return 0;
    }catch(const std::exception& error){
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
