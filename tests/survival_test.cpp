#include "survival.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
using namespace port::coop;
namespace {
unsigned assertions=0;
void require(bool ok,const char* why){++assertions;if(!ok)throw std::runtime_error(why);}
SurvivalObservation party(uint64_t now=0){SurvivalObservation o;o.now_ms=now;o.connected=o.ready=o.alive=3;o.generations[0]=11;o.generations[1]=42;return o;}
void accept(SurvivalController& c,const SurvivalStep& s){for(auto request:s.spawns)require(c.finish_spawn(request.ticket,true),"Native creation confirmation rejected");}
SurvivalState run(unsigned fps){
    SurvivalConfig config;config.countdown_ms=0;SurvivalController c(config);
    for(uint64_t frame=0;frame<=uint64_t(fps)*70;++frame){auto o=party(frame*1000/fps);auto s=c.advance(o);accept(c,s);}
    return c.state();
}
}
int main(){try{
    SurvivalController c;SurvivalObservation none;auto s=c.advance(none);require(s.state.phase==SurvivalPhase::Waiting && s.spawns.empty(),"Empty server started Survival");
    auto o=party(1000);s=c.advance(o);require(s.state.phase==SurvivalPhase::Countdown && s.state.phase_remaining_ms==5000,"Round countdown did not start on readiness");
    o.now_ms=5999;s=c.advance(o);require(s.spawns.empty() && s.state.phase_remaining_ms==1,"Round spawned before countdown elapsed");
    o.now_ms=6500;s=c.advance(o);require(s.state.phase==SurvivalPhase::Running && s.state.survival_ms==500 && s.state.wave==1,"Countdown overshoot changed survival clock");
    require(s.state.members==3 && s.state.playing_mask==3 && s.spawns.size()==2,"Round membership or bounded first spawn burst failed");
    require(c.playing(0,11) && !c.playing(0,12) && !c.playing(8,11),"Generation/slot check lost ownership");
    const auto ticket=s.spawns[0].ticket;const auto quota=s.state.quota_remaining;
    require(c.finish_spawn(ticket,false) && c.state().quota_remaining==quota && c.state().total_spawned==0,"Failed native spawn consumed quota");
    require(!c.finish_spawn(ticket,true) && !c.finish_spawn(0,true),"Duplicate/unknown spawn confirmation accepted");
    require(c.finish_spawn(s.spawns[1].ticket,true) && c.state().total_spawned==1,"Successful native spawn not committed");
    o.now_ms=7000;o.connected=o.ready=o.alive=7;o.generations[2]=7;s=c.advance(o);
    require(s.state.members==3 && s.state.playing_mask==3 && s.state.waiting_mask==4,"Late joiner received an extra life in an active round");
    o.now_ms=7100;o.alive=6;s=c.advance(o);require(s.state.playing_mask==2 && s.state.eliminated_mask==1,"A dead player was not eliminated");
    o.now_ms=7200;o.alive=7;s=c.advance(o);require(s.state.playing_mask==2,"Native automatic respawn bypassed one-life rule");
    o.now_ms=7300;o.generations[1]=43;s=c.advance(o);require(s.state.phase==SurvivalPhase::Defeat && s.state.playing_mask==0 && s.state.waiting_mask==7,"Reconnected slot inherited an old generation's life");
    require(s.state.phase_remaining_ms==15000 && !s.restart_due,"Defeat restart countdown not automatic");
    const auto best=s.state.best_ms;o.now_ms=22299;s=c.advance(o);require(!s.restart_due && s.state.phase_remaining_ms==1,"Restart issued early");
    o.now_ms=22300;s=c.advance(o);require(s.restart_due && s.spawns.empty(),"AFK-safe round restart signal missing");
    c.restart(o.now_ms);require(c.state().round==2 && c.state().best_ms==best && c.state().total_spawned==0,"Round restart lost best time or retained spawns");
    s=c.advance(o);require(s.state.phase==SurvivalPhase::Countdown,"Late joiners did not become eligible for the next round");

    SurvivalConfig immediate;immediate.countdown_ms=0;immediate.alive_cap=4;SurvivalController bounded(immediate);o=party();o.live_enemies=3;s=bounded.advance(o);
    require(s.spawns.size()==1,"Spawn burst exceeded active-enemy budget");accept(bounded,s);require(bounded.state().active_enemies==4,"Successful spawn missing from immediate budget");
    o.now_ms=1000;o.live_enemies=4;s=bounded.advance(o);require(s.spawns.empty(),"Active-enemy cap exceeded");
    o.now_ms=30001;s=bounded.advance(o);require(s.state.wave==2 && s.spawns.empty() && s.state.quota_remaining>0,"Living/stuck enemies prevented timed wave progression");
    o.now_ms=60001;s=bounded.advance(o);require(s.state.wave==3,"Wave clock depended on clearing an encounter");
    auto frozen=bounded.state();o.now_ms=50;o.alive=0;s=bounded.advance(o);require(s.state.survival_ms==frozen.survival_ms && s.state.playing_mask==frozen.playing_mask,"Regressed clock changed round state");
    o=party(60000000);o.live_enemies=0;s=bounded.advance(o);require(s.spawns.size()<=2,"A long stall created an unbounded spawn catch-up burst");
    const auto stale=s.spawns.empty()?0:s.spawns.front().ticket;o.now_ms+=1000;bounded.advance(o);require(!bounded.finish_spawn(stale,true),"Stale factory confirmation affected a later tick");

    SurvivalController retry(immediate);o=party();std::set<unsigned> retry_types;unsigned failures=0;
    for(unsigned n=0;n<40;++n){o.now_ms=n*1000;auto r=retry.advance(o);for(auto request:r.spawns){retry_types.insert(request.definition);require(retry.finish_spawn(request.ticket,false),"Spawn retry acknowledgement failed");++failures;}}
    require(failures>20 && retry.state().total_spawned==0 && retry.state().quota_remaining>0 && retry_types.size()>1,"An unavailable enemy type permanently starved the spawn queue");
    SurvivalController live_rules(immediate);o=party();auto pending=live_rules.advance(o);
    const auto original_quota=live_rules.state().quota_remaining;const auto original_clock=live_rules.state().survival_ms;
    live_rules.set_enemy_multiplier(3);require(live_rules.config().enemy_multiplier==3 && live_rules.state().quota_remaining==original_quota*3,"Increasing live enemy factor did not add future quota");
    require(live_rules.finish_spawn(pending.spawns.front().ticket,true),"Live rule change invalidated an authorized factory ticket");
    live_rules.set_enemy_multiplier(1);require(live_rules.state().quota_remaining==original_quota-1 && live_rules.state().active_enemies==1,"Decreasing live enemy factor removed a live actor or retained excess quota");
    live_rules.set_enemy_multiplier(999);require(live_rules.config().enemy_multiplier==5 && live_rules.state().quota_remaining==original_quota*5-1,"Live enemy factor bypassed x5 bound");
    live_rules.set_enemy_multiplier(0);require(live_rules.config().enemy_multiplier==1 && live_rules.state().survival_ms==original_clock && live_rules.state().members==3 && live_rules.state().round==1,"Live rule change reset round, time or membership");
    o.now_ms=120000;live_rules.advance(o);live_rules.set_enemy_multiplier(5);require(live_rules.state().quota_remaining<=192,"Live factor removed maximum wave quota bound");
    const auto slow=run(30),fast=run(240);require(slow.wave==fast.wave && slow.survival_ms==fast.survival_ms && slow.total_spawned==fast.total_spawned,"Wave/round policy changes with display sampling rate");
    require(slow.wave==3 && slow.survival_ms==70000,"Elapsed survival time or wave count incorrect");
    std::set<unsigned> allowed(survival_enemy_roster().begin(),survival_enemy_roster().end());
    require(allowed.size()==10 && !allowed.contains(15) && !allowed.contains(2) && !allowed.contains(41) && !allowed.contains(42),"Survival roster contains story actors or bosses");
    bool seed_changes=false;
    for(uint32_t wave:{1u,2u,8u,100000u})for(uint32_t i=0;i<200;++i){const auto enemy=survival_enemy_for(123,wave,i);require(allowed.contains(enemy),"Unknown enemy escaped finite survival roster");require(enemy==survival_enemy_for(123,wave,i),"Survival enemy sequence is nondeterministic");if(wave==1)require(enemy==0 || enemy==12,"Heavy enemies appeared in the first wave");seed_changes|=enemy!=survival_enemy_for(321,wave,i);}
    require(seed_changes,"Distinct round seeds do not change encounters");
    SurvivalController wide(immediate);o=party();o.connected=o.ready=o.alive=1;o.generations[0]=(uint64_t(1)<<40)|11;
    s=wide.advance(o);require(wide.playing(0,o.generations[0]) && !wide.playing(0,11),"64-bit player generation was truncated");
    o.now_ms=100;o.generations[0]=11;s=wide.advance(o);require(s.state.phase==SurvivalPhase::Defeat,"High-generation slot reused a low-generation life");
    SurvivalController loading;auto prestart=party();prestart.ready=1;
    s=loading.advance(prestart);require(s.state.phase==SurvivalPhase::Waiting && s.spawns.empty(),"A slow connected prestart player was excluded by the countdown");
    prestart.now_ms=9000;s=loading.advance(prestart);require(s.state.phase==SurvivalPhase::Waiting,"Prestart loading timeout incorrectly started a round");
    prestart.ready=3;s=loading.advance(prestart);require(s.state.phase==SurvivalPhase::Countdown && s.state.phase_remaining_ms==5000,"Ready players did not receive a complete countdown");
    prestart.now_ms=12000;prestart.ready=1;s=loading.advance(prestart);require(s.state.phase==SurvivalPhase::Waiting && s.spawns.empty(),"A renewed load did not hold a complete countdown");
    prestart.now_ms=13000;prestart.ready=3;s=loading.advance(prestart);require(s.state.phase==SurvivalPhase::Countdown && s.state.phase_remaining_ms==5000,"Returning ready players did not receive a full countdown");
    prestart.now_ms=18000;s=loading.advance(prestart);require(s.state.phase==SurvivalPhase::Running && s.state.members==3,"A prestart participant was forced to spectate after loading");
    SurvivalController placement;
    auto blocked=party(200);
    blocked.alive=1;
    s=placement.advance(blocked);
    require(s.state.phase==SurvivalPhase::Waiting && s.spawns.empty(),"Network readiness overrode pending native placement");
    blocked.now_ms=5500;
    blocked.alive=3;
    s=placement.advance(blocked);
    require(s.state.phase==SurvivalPhase::Countdown && s.state.phase_remaining_ms==5000,"Late native placement skipped the complete countdown");
    blocked.now_ms=9500;
    blocked.alive=1;
    s=placement.advance(blocked);
    require(s.state.phase==SurvivalPhase::Waiting && s.state.playing_mask==0,"Lost native life did not stop the countdown");
    blocked.now_ms=13000;
    blocked.alive=3;
    s=placement.advance(blocked);
    require(s.state.phase==SurvivalPhase::Countdown && s.state.phase_remaining_ms==5000,"Restored native placement inherited a stale countdown");
    blocked.now_ms=18000;
    s=placement.advance(blocked);
    require(s.state.phase==SurvivalPhase::Running && s.state.playing_mask==3,"Placed participant did not receive a life");
    SurvivalController watching;prestart=party();prestart.ready=1;prestart.voluntary_spectators=2;s=watching.advance(prestart);require(s.state.phase==SurvivalPhase::Countdown,"An intentional spectator blocked a round");
    prestart.now_ms=5000;s=watching.advance(prestart);require(s.state.phase==SurvivalPhase::Running && s.state.members==1,"An intentional spectator received a survival life");
    SurvivalController departed;prestart=party();prestart.ready=1;departed.advance(prestart);prestart.now_ms=1000;prestart.connected=1;s=departed.advance(prestart);require(s.state.phase==SurvivalPhase::Countdown,"A disconnected prestart player blocked a round forever");
    SurvivalConfig malformed;malformed.wave_ms=0;malformed.spawn_ms=0;malformed.alive_cap=999;malformed.enemy_multiplier=99;SurvivalController safe(malformed);
    require(safe.config().wave_ms>=1000 && safe.config().spawn_ms>=50 && safe.config().alive_cap<=48 && safe.config().enemy_multiplier==5,"Malformed session rules removed bounded-work limits");
    o=party();o.generations={};s=safe.advance(o);require(s.state.phase==SurvivalPhase::Waiting,"Unowned players started a round");
    o=party();o.voluntary_spectators=3;s=safe.advance(o);require(s.state.phase==SurvivalPhase::Waiting,"Spectators counted as playing survivors");
    std::cout<<"Survival policy passed "<<assertions<<" assertions: one life, generations, late join, timed pressure, native confirmations, bounded spawning and frame-independent time.\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
