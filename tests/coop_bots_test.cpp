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
    check(bot_weapon_pickup(43)==8 && bot_weapon_pickup(55)==11,"weapon mapping");
    check(bot_pickup_value(37,100,100,false,false)>bot_pickup_value(37,100,100,true,false),"unowned weapon priority");
    check(bot_pickup_value(2,30,100,false,false)>0 && bot_pickup_value(2,100,100,false,false)==0,"health need");
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
    std::array<bool,12> ranged{};ranged[3]=ranged[7]=true;
    check(bot_acquisition_range(ranged)==15.f && choose_afk_weapon(ranged,3,80,1,true)==255,"close weapons used on an eighty-metre target");
    check(choose_afk_weapon(ranged,3,5,1,true)==7,"near weapon priority lost");
    ranged[8]=true;
    check(bot_acquisition_range(ranged)==100.f && choose_afk_weapon(ranged,3,80,1,true)==8,"AFK bot failed to select owned long-range rifle");
    check(choose_game_bot_weapon(ranged,3,80,1,true)==8,"autonomous bot failed long-range selection");
    ranged[8]=false;ranged[4]=true;
    check(choose_afk_weapon(ranged,3,80,1,true)==4,"minigun ignored at long range");
    ranged={};ranged[6]=true;
    check(choose_afk_weapon(ranged,6,80,3,true)==255 && choose_afk_weapon(ranged,6,20,3,true)==6,"grenade range not bounded");
    ranged={};ranged[5]=true;
    check(choose_afk_weapon(ranged,5,80,1,false)==255 && choose_afk_weapon(ranged,5,80,1,true)==5,"far explosives ignored teammate safety");
    ranged={};ranged[1]=true;
    check(choose_afk_weapon(ranged,1,100,1,true)==1 && choose_afk_weapon(ranged,1,100.01f,1,true)==255,"long-range upper boundary");
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
    std::cout<<"Bot admission, identity, human priority, navigation, pickups and weapon-aware near/far combat passed\n";
}


