#include "authoritative_session.h"
#include "bot_navigation.h"
#include "afk_combat.h"
#include "../port/src/test_bot.h"
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
using namespace port::coop;
void check(bool b,const char* s){if(!b){std::cerr<<s<<'\n';std::exit(1);}}
int main(){
    AuthoritativeServer host(8);check(host.start(0,"123456",1234),"host start");
    check(!host.add_bot(0) && !host.add_bot(4),"invalid difficulty");
    for(unsigned i=1;i<8;++i){auto bot=host.add_bot(i%3+1);check(bot && bot->slot==i,"unique bot slot");}
    check(host.bot_mask()==254 && !host.add_bot(),"capacity");
    for(unsigned i=1;i<8;++i)check(host.participants()[i].afk,"bots must be automated");
    check(host.set_match_player_limit(4) && host.bot_mask()==14,"mode change trims bots");
    AuthoritativeClient bad;check(bad.start("127.0.0.1",host.status().port,"654321",1234,"bad"),"bad start");
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(bad.status().message.find("differs")==std::string::npos && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(host.bot_mask()==14,"unauthenticated peer evicted a bot");bad.stop();
    AuthoritativeClient human;check(human.start("127.0.0.1",host.status().port,"123456",1234,"Human"),"human start");
    end=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(!human.status().connected && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(human.status().connected && host.bot_mask()==6 && human.status().player.slot==3,"human must replace bot");
    check(!host.set_match_player_limit(2),"mode change discarded human");
    check(!host.remove_bot(human.status().player),"remove bot discarded human");
    const auto id=host.participants()[1].id;check(host.remove_bot(id) && !host.remove_bot(id),"bot identity generation");
    human.stop();host.stop();
    BotNavigation nav;BotPoint start{0,1,0},goal{15,1,0};unsigned probes=0;bool crossed=false;
    auto probe=[&](const BotPoint& a,const BotPoint& b)->std::optional<BotPoint>{++probes;
        // A wall at x=5 with its only gap beyond z=7.5. Check whole edge.
        if((a[0]<5 && b[0]>=5)||(b[0]<5 && a[0]>=5)){const auto t=(5-a[0])/(b[0]-a[0]);if(a[2]+t*(b[2]-a[2])<7.5f)return {};crossed=true;}
        if(std::abs(b[0])>25 || std::abs(b[2])>25)return {};return b;
    };
    for(unsigned i=0;i<200 && bot_distance(start,goal)>2;++i){const auto before=probes;const auto next=nav.step(start,goal,probe,12);check(probes-before<=12,"navigation exceeded per-tick budget");
        if(bot_distance(start,next)>.01f){check(bool(probe(start,next)),"navigation returned blocked edge");const auto d=bot_distance(start,next);for(unsigned j=0;j<3;++j)start[j]+=(next[j]-start[j])*std::min(1.f,1.f/d);}
    }
    check(crossed && bot_distance(start,goal)<=2,"failed wall detour");
    BotNavigation cliff;start={0,0,0};for(unsigned i=0;i<20;++i){const auto next=cliff.step(start,goal,[](auto&,auto&)->std::optional<BotPoint>{return {};});check(next==start,"bot walked into unsupported terrain");}
    BotNavigation overshoot;const BotPoint origin{0,0,0},short_goal{5,0,0};
    overshoot.step(origin,short_goal,[](auto&,auto&)->std::optional<BotPoint>{return {};},100);
    auto open=[](const BotPoint&,const BotPoint& b)->std::optional<BotPoint>{return b;};
    overshoot.step(origin,short_goal,open,100);
    check(overshoot.step(BotPoint{2.1f,0,0},short_goal,open,100)[0]>2.1f,"bot backtracked after crossing a fine waypoint between decisions");
    // The leader keeps walking while the bot searches around a wall. Reusing
    // bounded search work must make progress before the human stops moving.
    BotNavigation moving;BotPoint follower{0,0,0};unsigned moving_queries=0;
    auto moving_probe=[&](const BotPoint& a,const BotPoint& b)->std::optional<BotPoint>{
        ++moving_queries;
        if((a[0]<5 && b[0]>=5)||(b[0]<5 && a[0]>=5)){
            const float t=(5-a[0])/(b[0]-a[0]);if(a[2]+t*(b[2]-a[2])<7.5f)return {};
        }
        if(std::abs(b[0])>30 || std::abs(b[2])>30)return {};return b;
    };
    for(unsigned i=0;i<360 && follower[0]<10;++i){
        const BotPoint human{18,0,12+4*std::sin(float(i)*.22f)};
        const auto before=moving_queries;const auto next=moving.step(follower,human,moving_probe,24);
        check(moving_queries-before<=24 && moving.explored()<=2048,"moving-leader search exceeded its bounds");
        if(bot_distance(follower,next)>.01f){check(bool(moving_probe(follower,next)),"moving leader returned a blocked edge");const float scale=std::min(1.f,.65f/bot_distance(follower,next));for(unsigned j=0;j<3;++j)follower[j]+=(next[j]-follower[j])*scale;}
    }
    check(follower[0]>=10,"moving human repeatedly invalidated the obstacle detour");
    BotNavigation reversal;const BotPoint old_leader{20,0,0},returning_leader{-10,0,0};
    check(reversal.step(origin,old_leader,open,512)[0]>0,"leader reversal setup did not produce a route");
    check(reversal.step(origin,returning_leader,open,512)[0]<0,"bot followed an obsolete solved route after the human reversed");
    // A .5 m centre corridor lies between coarse grid rows. The collision
    // callback represents the character's swept radius, not a zero-width ray.
    BotNavigation doorway;BotPoint door_start{0,0,0},door_goal{8,0,.625f};unsigned door_queries=0,door_ticks=0;
    auto door_probe=[&](const BotPoint& a,const BotPoint& b)->std::optional<BotPoint>{
        ++door_queries;
        if((a[0]<4 && b[0]>=4)||(b[0]<4 && a[0]>=4)){
            const float t=(4-a[0])/(b[0]-a[0]);const float z=a[2]+t*(b[2]-a[2]);if(z<.375f || z>.875f)return {};
        }
        if(b[0]<-2 || b[0]>10 || std::abs(b[2])>5)return {};return b;
    };
    for(;door_ticks<160 && bot_distance(door_start,door_goal)>.6f;++door_ticks){
        const auto before=door_queries;const auto next=doorway.step(door_start,door_goal,door_probe,32);
        check(door_queries-before<=32,"narrow-door search exceeded its probe budget");
        if(bot_distance(door_start,next)>.01f){check(bool(door_probe(door_start,next)),"doorway path crossed the wall");const float scale=std::min(1.f,.4f/bot_distance(door_start,next));for(unsigned j=0;j<3;++j)door_start[j]+=(next[j]-door_start[j])*scale;}
    }
    check(bot_distance(door_start,door_goal)<=.6f && door_ticks<160,"fine doorway alignment was not found promptly");
    // A door closes after planning. Every returned edge must be queried again.
    BotNavigation changing;BotPoint changed_start{0,0,0};bool closed=false;unsigned change_queries=0;
    auto changing_probe=[&](const BotPoint& a,const BotPoint& b)->std::optional<BotPoint>{
        ++change_queries;
        for(unsigned n=0;n<=40;++n){const float t=float(n)/40;const float x=a[0]+t*(b[0]-a[0]),z=a[2]+t*(b[2]-a[2]);if(closed && x>1 && x<2 && std::abs(z)<1)return {};}
        if(std::abs(b[0])>10 || std::abs(b[2])>5)return {};return b;
    };
    changing.step(changed_start,BotPoint{6,0,0},changing_probe,128);closed=true;
    for(unsigned i=0;i<140 && bot_distance(changed_start,BotPoint{6,0,0})>.6f;++i){
        const auto before=change_queries;const auto next=changing.step(changed_start,BotPoint{6,0,0},changing_probe,24);
        check(change_queries-before<=24,"dynamic obstacle exceeded probe budget");
        if(bot_distance(changed_start,next)>.01f){check(bool(changing_probe(changed_start,next)),"closed door reused an invalid old edge");const float scale=std::min(1.f,.4f/bot_distance(changed_start,next));for(unsigned j=0;j<3;++j)changed_start[j]+=(next[j]-changed_start[j])*scale;}
    }
    check(bot_distance(changed_start,BotPoint{6,0,0})<=.6f,"closed-door detour did not recover");
    // Stairs retain native support heights; no path can hop an unsupported gap.
    BotNavigation stairs;BotPoint stair_start{0,0,0};const BotPoint stair_goal{6,1.2f,0};
    auto stair_probe=[](const BotPoint& a,const BotPoint& b)->std::optional<BotPoint>{
        if(b[0]<0 || b[0]>7 || std::abs(b[2])>.2f)return {};
        auto q=b;q[1]=std::floor(std::max(0.f,b[0])/.625f)*.125f;
        if(std::abs(q[1]-a[1])>.6f)return {};return q;
    };
    for(unsigned i=0;i<100 && bot_distance(stair_start,stair_goal)>.6f;++i){const auto next=stairs.step(stair_start,stair_goal,stair_probe,24);if(next!=stair_start){check(bool(stair_probe(stair_start,next)),"navigation bypassed native stair support");stair_start=next;}}
    check(bot_distance(stair_start,stair_goal)<=.6f,"supported native stairs were not followed");
    // Sideways sliding may have plenty of displacement while making no
    // progress down the route. The search must eventually restart at the bot.
    BotNavigation sliding;
    const BotPoint slide_goal{9,0,0};
    check(sliding.step(origin,slide_goal,open,128)[0]>0,"slide recovery setup has no waypoint");
    const auto initial_search=sliding.state().restarts;
    for(unsigned i=0;i<14;++i)sliding.step(BotPoint{0,0,.2f*(i+1)},slide_goal,open,24);
    check(sliding.state().restarts>initial_search,"sideways wall scrape suppressed route recovery");

    BotNavigation invalid;unsigned invalid_queries=0;const BotPoint invalid_goal{NAN,0,0};
    check(invalid.step(origin,invalid_goal,[&](auto&,auto&)->std::optional<BotPoint>{++invalid_queries;return {};})==origin && !invalid_queries,"invalid navigation coordinates reached native queries");
    check(bot_weapon_pickup(43)==8 && bot_weapon_pickup(55)==11,"weapon mapping");
    check(bot_pickup_value(37,100,100,false,false)>bot_pickup_value(37,100,100,true,false),"unowned weapon priority");
    check(bot_pickup_value(2,30,100,false,false)>0 && bot_pickup_value(2,100,100,false,false)==0,"health need");
    check(bot_coop_pickup_allowed(6,2,5,false) && bot_coop_pickup_allowed(10,8,6,false),"nearby useful pickup or route boundary rejected");
    check(!bot_coop_pickup_allowed(10.01f,1,9,false) && !bot_coop_pickup_allowed(5,8.01f,4,false),"nonurgent pickup abandoned a distant human");
    check(!bot_coop_pickup_allowed(9,3,12.01f,false) && !bot_coop_pickup_allowed(6,5,6,false),"nonurgent pickup exceeded its human leash or route excess");
    check(bot_coop_pickup_allowed(18,4,18,true) && bot_coop_pickup_allowed(18,8,18,true),"bounded critical supply detour rejected while regrouping");
    check(!bot_coop_pickup_allowed(18,8.01f,18,true) && !bot_coop_pickup_allowed(10,6,13,true),"urgent pickup bypassed its detour bounds");
    check(bot_pickup_value(37,100,100,false,false)>0 && bot_coop_pickup_allowed(4,2,3,true),"nearby unowned weapon could not be collected");
    check(bot_pickup_value(17,100,100,true,false)>0 && bot_coop_pickup_allowed(3,1,3,false) && !bot_coop_pickup_allowed(15,1,15,false),"optional ammo pickup displaced regrouping priority");
    check(!bot_coop_pickup_allowed(NAN,1,1,false) && !bot_coop_pickup_allowed(1,INFINITY,1,true) && !bot_coop_pickup_allowed(1,1,NAN,false) && !bot_coop_pickup_allowed(-1,1,1,true) && !bot_coop_pickup_allowed(1,-1,1,true) && !bot_coop_pickup_allowed(1,1,-1,true),"invalid pickup distances reached goal selection");
    std::array<bool,12> usable{};usable[1]=usable[3]=usable[8]=true;
    check(choose_game_bot_weapon(usable,1,5,1,true)==3 && choose_game_bot_weapon(usable,1,35,1,true)==8,"range weapon choice");
    check(choose_game_bot_weapon(usable,3,10,1,true)==3,"weapon boundary hysteresis");
    usable[4]=true;
    check(choose_game_bot_weapon(usable,4,8,1,true)==4,"minigun spin-up interrupted by shotgun boundary");
    check(choose_game_bot_weapon(usable,4,36,1,true)==4,"minigun range jitter");
    usable[4]=false;usable[7]=true;
    check(choose_game_bot_weapon(usable,1,7,1,true)==7,"flamethrower close priority");
    usable[3]=false;
    check(choose_game_bot_weapon(usable,1,7,1,true)==7,"flamethrower ignored in favour of pistol");
    for(unsigned weapon:{2u,4u,7u}){
        check(!bot_fire_aligned(weapon,.993f,false),"automatic trigger started before aim acquired");
        check(bot_fire_aligned(weapon,.993f,true),"automatic trigger dropped during small tracking error");
        check(!bot_fire_aligned(weapon,.98f,true),"automatic trigger held outside tracking cone");
    }
    check(!bot_fire_aligned(8,.993f,true),"sniper firing cone widened");
    check(!bot_fire_aligned(4,NAN,true),"invalid aim fired");
    check(bot_firing_stance(4,8) && bot_firing_stance(4,15) && !bot_firing_stance(4,16),"minigun stance must cover spin-up and firing");
    check(!bot_firing_stance(2,8),"other weapons lost strafe cadence");
    usable[7]=false;
    usable[1]=usable[3]=usable[8]=false;usable[5]=true;
    check(choose_game_bot_weapon(usable,5,3,3,false)==255,"unsafe rocket choice");
    port::netplay::CompanionBot controller;ssne::BotWorld near;near.client={1,{0,0,0},100,1};near.host={2,{0,0,.5f},100,1};
    const auto input=controller.update(near,8,nullptr,{},true,true);
    check(input.pad.stick_y!=128,"bot stopped before reaching a fine navigation waypoint");
    // Human breadcrumbs wrap around a tight corner. They must be reached
    // before trimming them, even if the human is already beyond the wall.
    port::netplay::CompanionBot corner;ssne::BotWorld corner_world;
    corner_world.client={1,{0,0,0},100,1.6f};corner_world.host={2,{2,0,0},100,1.6f};
    auto corner_clear=[&](const BotPoint& q){
        const auto& from=corner_world.client.position;
        for(unsigned n=0;n<=100;++n){const float t=float(n)/100;const float x=from[0]+t*(q[0]-from[0]),z=from[2]+t*(q[2]-from[2]);if(x>3 && x<7 && z<3.5f && z>-2)return false;}return true;
    };
    for(const BotPoint& q:std::array<BotPoint,8>{{{2,0,0},{2,0,2},{2,0,4},{4,0,4},{6,0,4},{8,0,4},{8,0,2},{8,0,0}}}){corner_world.host.position=q;corner.update(corner_world,8,nullptr,corner_clear,true);}
    auto corner_state=corner.checkpoint();corner_state.stalled=corner_state.escape=0;corner_state.moving=false;check(corner.restore_checkpoint(corner_state),"corner checkpoint setup");
    for(unsigned i=0;i<140 && bot_distance(corner_world.client.position,corner_world.host.position)>3.5f;++i){
        const auto pad=corner.update(corner_world,8,nullptr,corner_clear,true).pad;auto next=corner_world.client.position;
        next[0]+=(int(pad.stick_x)-128)/75.f*.35f;next[2]+=(int(pad.stick_y)-128)/75.f*.35f;
        check(corner_clear(next),"companion cut a blocked corner before reaching its breadcrumb");corner_world.client.position=next;
    }
    check(bot_distance(corner_world.client.position,corner_world.host.position)<=3.5f,"companion failed to follow the moving human around a corner");
    // A nearby team member can still be around a corner. Use a confirmed
    // turn and preserve it while the leader is airborne. Retire it after
    // the follower rounds the same corner and sees the leader again.
    port::netplay::CompanionBot follower_controller;
    const BotPoint ground_leader{0,0,4}, airborne_leader{2,3,2}, landed_leader{2,0,2};
    auto blocked_route=[](const BotPoint&){return false;};
    check(follower_controller.follow_goal(origin,ground_leader,true,blocked_route).position==ground_leader,
          "first grounded breadcrumb was not acquired");
    check(follower_controller.follow_goal(origin,airborne_leader,false,blocked_route).position==ground_leader,
          "airborne leader overwrote last grounded breadcrumb");
    const auto near_but_blocked=follower_controller.follow_goal(origin,landed_leader,true,blocked_route);
    check(!near_but_blocked.directly_reachable && near_but_blocked.position==ground_leader,
          "nearby leader behind a wall displaced the corner");
    ssne::BotWorld at_wall;
    at_wall.client={1,origin,100,1.6f};at_wall.host={2,landed_leader,100,1.6f};
    const BotPoint checked_waypoint{0,0,.625f};
    const auto checked_pad=follower_controller.update(at_wall,8,nullptr,blocked_route,true,false,120.f,true,&checked_waypoint).pad;
    check(checked_pad.stick_x==128 && checked_pad.stick_y>128,"AFK ignored a checked waypoint inside its normal following distance");
    const auto around_wall=follower_controller.follow_goal(ground_leader,landed_leader,true,[](const BotPoint&){return true;});
    check(around_wall.directly_reachable && around_wall.position==landed_leader,"reachable leader did not resume direct follow");
    const BotPoint new_area{100,0,4};
    check(follower_controller.follow_goal(origin,new_area,true,blocked_route).position==new_area &&
          follower_controller.checkpoint().trail.size()==1,"obsolete breadcrumbs survived a leader teleport");

    // Scraping a wall by two centimetres per decision must not suppress stall
    // recovery. Recovery is a checked world-space sidestep, with a neutral pad
    // when neither candidate has collision/support clearance.
    port::netplay::CompanionBot scraping;ssne::BotWorld scrape;
    scrape.client={1,{0,0,0},100,1.6f};scrape.host={2,{0,0,12},100,1.6f};bool recovered=false;unsigned recovery_queries=0;
    auto side_clear=[&](const BotPoint& q){++recovery_queries;return q[0]-scrape.client.position[0]>1.2f && q[2]-scrape.client.position[2]>1.2f;};
    for(unsigned i=0;i<9;++i){scrape.client.position[0]+=.02f;const auto pad=scraping.update(scrape,8,nullptr,side_clear,true,true).pad;if(pad.stick_x>128 && pad.stick_y>128 && pad.trigger_l)recovered=true;}
    check(recovered && recovery_queries,"slow wall scraping never attempted a checked sidestep");
    auto escape_state=scraping.checkpoint();escape_state.escape=60;escape_state.moving=true;check(scraping.restore_checkpoint(escape_state),"escape checkpoint setup");
    const auto stopped=scraping.update(scrape,8,nullptr,[](const BotPoint&){return false;},true,true).pad;
    check(stopped.stick_x==128 && stopped.stick_y==128 && !stopped.trigger_l,"unsafe stall escape issued movement or jump");
    BotNavigation fine_route;
    fine_route.step(origin,short_goal,[](auto&,auto&)->std::optional<BotPoint>{return {};},100);
    fine_route.step(origin,short_goal,open,100);
    const BotPoint fine_position{.38f,0,0};const auto fine_waypoint=fine_route.step(fine_position,short_goal,open,100);
    check(bot_distance(fine_position,fine_waypoint)>.22f && bot_distance(fine_position,fine_waypoint)<.3f,"fine-grid arrival regression setup changed");
    port::netplay::CompanionBot fine_controller;ssne::BotWorld fine_world;
    fine_world.client={1,fine_position,100,1.6f};fine_world.host={1,fine_waypoint,100,1.6f};
    const auto fine_pad=fine_controller.update(fine_world,8,nullptr,{},true,true).pad;
    check(fine_pad.stick_x!=128 || fine_pad.stick_y!=128,"autonomous controller stopped before navigation could consume a fine-grid waypoint");
    std::array<bool,12> ranged{};ranged[3]=ranged[7]=true;
    check(bot_acquisition_range(ranged)==15.f && choose_afk_weapon(ranged,3,80,1,true)==255,"close weapons used on an eighty-metre target");
    check(choose_afk_weapon(ranged,3,5,1,true)==7,"near weapon priority lost");
    ranged[8]=true;
    check(bot_acquisition_range(ranged)==250.f && choose_afk_weapon(ranged,3,80,1,true)==8,"AFK bot failed to select owned long-range rifle");
    check(choose_game_bot_weapon(ranged,3,80,1,true)==8,"autonomous bot failed long-range selection");
    ranged[8]=false;ranged[4]=true;
    check(choose_afk_weapon(ranged,3,80,1,true)==4,"minigun ignored at long range");
    ranged={};ranged[6]=true;
    check(choose_afk_weapon(ranged,6,80,3,true)==255 && choose_afk_weapon(ranged,6,20,3,true)==6,"grenade range not bounded");
    ranged={};ranged[5]=true;
    check(choose_afk_weapon(ranged,5,80,1,false)==255 && choose_afk_weapon(ranged,5,80,1,true)==5,"far explosives ignored teammate safety");
    ranged={};ranged[1]=true;
    check(choose_afk_weapon(ranged,1,120,1,true)==1 && choose_afk_weapon(ranged,1,120.01f,1,true)==255,"long-range upper boundary");
    unsigned visibility_queries=0;
    auto blocked=[&]{++visibility_queries;return false;};auto clear=[&]{++visibility_queries;return true;};
    check(!bot_visible_target(80,bot_acquisition_range(ranged),blocked) && visibility_queries==1,"distant occluded target bypassed visibility query");
    check(bot_visible_target(80,100,clear) && visibility_queries==2,"distant clear target was not queried");
    check(!bot_visible_target(101,100,clear) && !bot_visible_target(NAN,100,clear) && visibility_queries==2,"invalid or out-of-range target queried");
    check(!bot_fire_aligned(8,.999f,false,80) && bot_fire_aligned(8,.99999f,false,80),"distant muzzle alignment was not tightened");
    check(!bot_fire_aligned(4,.999f,true,80),"automatic latch bypassed distant muzzle alignment");
    ssne::BotWorld far;far.client={1,{0,0,0},100,1.6f};far.host={2,{0,0,2},100,1.6f};far.enemies={{3,{0,0,80},100,1.6f}};const std::array<float,3> eye{0,.8f,0};
    port::netplay::CompanionBot distant;
    auto far_input=distant.update(far,8,&eye,{},true,false,100);
    check(distant.target()==3 && far_input.pad.trigger_r==255,"companion retained the old thirty-metre acquisition cap");
    far.enemies.clear();far_input=distant.update(far,8,&eye,{},true,false,100);
    check(!distant.target() && !far_input.pad.trigger_r,"occluded/retired far target retained trigger");
    far.enemies={{3,{0,0,80},100,1.6f}};far.host.position={0,0,25};far_input=distant.update(far,8,&eye,{},true,false,100);
    check(!distant.target() && far_input.pad.stick_y>128,"far target distracted companion from active human follow");
    far.enemies.push_back({4,{0,0,6},100,1.6f});far_input=distant.update(far,8,&eye,{},true,false,100);
    check(distant.target()==4,"catching-up companion lost immediate self-defence");
    far.host.position={0,0,2};far.enemies={{3,{0,0,80},100,1.6f}};far_input=distant.update(far,8,&eye,{},true,false,15);
    check(!distant.target() && !far_input.pad.trigger_r,"short inventory ignored explicit acquisition range");
    std::cout<<"Bot admission, moving-human follow, narrow doors, dynamic obstacles, stairs, safe recovery and weapon-aware combat passed\n";
}


