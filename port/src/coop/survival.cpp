#include "survival.h"
#include <algorithm>
#include <bit>
#include <limits>
namespace port::coop {namespace {
struct Enemy {unsigned definition,from_wave,weight;};
constexpr Enemy enemies[]={{0,1,5},{12,1,6},{18,2,5},{13,2,3},{14,3,3},{5,4,2},{7,4,2},{10,5,2},{8,6,1},{11,8,1}};
uint32_t mix(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
uint64_t add(uint64_t a,uint64_t b){return a>std::numeric_limits<uint64_t>::max()-b?std::numeric_limits<uint64_t>::max():a+b;}
uint32_t add_count(uint32_t a,unsigned b){return uint32_t(std::min<uint64_t>(uint64_t(a)+b,std::numeric_limits<uint32_t>::max()));}
uint64_t remaining(uint64_t now,uint64_t start,uint64_t duration){const auto elapsed=now>=start?now-start:0;return elapsed>=duration?0:duration-elapsed;}
uint8_t eligible(const SurvivalObservation& o){uint8_t mask=o.connected&o.ready&o.alive&uint8_t(~o.voluntary_spectators);for(unsigned i=0;i<8;++i)if(!o.generations[i])mask&=uint8_t(~(1u<<i));return mask;}
}
const std::vector<unsigned>& survival_enemy_roster(){static const std::vector<unsigned> roster=[](){std::vector<unsigned> out;for(const auto& e:enemies)out.push_back(e.definition);return out;}();return roster;}
unsigned survival_enemy_for(uint32_t seed,uint32_t wave,uint32_t index){
    wave=std::max(1u,wave);unsigned total=0;for(const auto& e:enemies)if(wave>=e.from_wave)total+=e.weight;
    auto choice=mix(seed^mix(wave)^mix(index+0x9e3779b9u))%total;
    for(const auto& e:enemies)if(wave>=e.from_wave){if(choice<e.weight)return e.definition;choice-=e.weight;}
    return 12;
}
void SurvivalController::reset(SurvivalConfig c){
    c.countdown_ms=std::min(c.countdown_ms,30000u);c.wave_ms=std::clamp(c.wave_ms,1000u,300000u);
    c.restart_ms=std::clamp(c.restart_ms,1000u,60000u);c.spawn_ms=std::clamp(c.spawn_ms,50u,10000u);
    c.alive_cap=std::clamp(c.alive_cap,1u,48u);c.enemy_multiplier=std::clamp(c.enemy_multiplier,1u,5u);
    config_=c;state_={};last_ms_=phase_started_ms_=round_started_ms_=next_spawn_ms_=next_ticket_=0;spawned_wave_=failed_wave_=0;clock_set_=false;pending_.clear();
}
void SurvivalController::set_enemy_multiplier(unsigned factor){
    config_.enemy_multiplier=std::clamp(factor,1u,5u);
    if(state_.phase==SurvivalPhase::Running){const auto quota=wave_quota();state_.quota_remaining=quota-std::min(quota,spawned_wave_);}
}
unsigned SurvivalController::wave_quota()const{
    const auto players=std::max(1,std::popcount(state_.members));
    return unsigned(std::min<uint64_t>(192,(8ull+4ull*std::min(state_.wave,40u)+4ull*unsigned(players-1))*config_.enemy_multiplier));
}
bool SurvivalController::playing(unsigned slot,uint64_t generation)const{
    return slot<8 && generation && state_.phase==SurvivalPhase::Running && (state_.playing_mask&(1u<<slot)) && state_.member_generations[slot]==generation;
}
SurvivalStep SurvivalController::advance(const SurvivalObservation& o){
    if(clock_set_ && o.now_ms<last_ms_)return {state_,{},false};
    clock_set_=true;last_ms_=o.now_ms;pending_.clear();state_.active_enemies=std::min(o.live_enemies,1000000u);
    if(state_.phase==SurvivalPhase::Waiting){
        state_.waiting_mask=o.connected;state_.playing_mask=0;state_.phase_remaining_ms=0;
        if(eligible(o) && !(o.connected&uint8_t(~o.voluntary_spectators)&uint8_t(~o.ready))){state_.phase=SurvivalPhase::Countdown;phase_started_ms_=o.now_ms;}
    }
    if(state_.phase==SurvivalPhase::Countdown){
        state_.waiting_mask=o.connected;state_.playing_mask=0;
        if(!eligible(o)){state_.phase=SurvivalPhase::Waiting;state_.phase_remaining_ms=0;return {state_,{},false};}
        // A connected prestart player may still be loading the same map.
        // Hold a full countdown until everyone who intends to play is ready;
        // late joiners during Running still wait for the following round.
        if(o.connected&uint8_t(~o.voluntary_spectators)&uint8_t(~o.ready)){state_.phase=SurvivalPhase::Waiting;state_.phase_remaining_ms=0;return {state_,{},false};}
        state_.phase_remaining_ms=remaining(o.now_ms,phase_started_ms_,config_.countdown_ms);
        if(state_.phase_remaining_ms)return {state_,{},false};
        state_.phase=SurvivalPhase::Running;state_.members=eligible(o);state_.member_generations=o.generations;
        state_.eliminated_mask=0;round_started_ms_=add(phase_started_ms_,config_.countdown_ms);next_spawn_ms_=round_started_ms_;spawned_wave_=0;
    }
    if(state_.phase==SurvivalPhase::Running){
        for(unsigned i=0;i<8;++i)if(state_.members&(1u<<i)){
            const auto bit=uint8_t(1u<<i);
            if(!(o.connected&bit) || !(o.alive&bit) || (o.voluntary_spectators&bit) || o.generations[i]!=state_.member_generations[i])state_.eliminated_mask|=bit;
        }
        state_.playing_mask=state_.members&uint8_t(~state_.eliminated_mask);state_.waiting_mask=o.connected&uint8_t(~state_.playing_mask);
        state_.survival_ms=o.now_ms>=round_started_ms_?o.now_ms-round_started_ms_:0;state_.best_ms=std::max(state_.best_ms,state_.survival_ms);
        state_.kills=add_count(state_.kills,o.confirmed_kills);
        if(!state_.playing_mask){state_.phase=SurvivalPhase::Defeat;phase_started_ms_=o.now_ms;state_.quota_remaining=0;state_.wave_remaining_ms=0;}
        else {
            const uint32_t wave=uint32_t(std::min<uint64_t>(1+state_.survival_ms/config_.wave_ms,std::numeric_limits<uint32_t>::max()));
            if(wave!=state_.wave){state_.wave=wave;spawned_wave_=failed_wave_=0;}
            state_.phase_remaining_ms=0;state_.wave_remaining_ms=config_.wave_ms-state_.survival_ms%config_.wave_ms;
            const auto quota=wave_quota();state_.quota_remaining=quota-std::min(quota,spawned_wave_);
            SurvivalStep step;step.state=state_;
            if(o.now_ms>=next_spawn_ms_ && state_.quota_remaining && o.live_enemies<config_.alive_cap){
                const auto count=std::min({2u,state_.quota_remaining,config_.alive_cap-o.live_enemies});
                const auto acceleration=std::min(state_.wave-1,12u)*40u;const auto cadence=std::max(50u,config_.spawn_ms>acceleration?config_.spawn_ms-acceleration:50u);
                next_spawn_ms_=add(next_spawn_ms_,cadence);
                if(next_spawn_ms_<=o.now_ms)next_spawn_ms_=add(o.now_ms,cadence); // no unbounded lag catch-up
                for(unsigned n=0;n<count;++n){
                    if(next_ticket_==std::numeric_limits<uint64_t>::max())break;
                    const auto ticket=++next_ticket_;pending_.push_back({ticket,state_.wave});
                    step.spawns.push_back({ticket,survival_enemy_for(config_.seed,state_.wave,spawned_wave_+n+failed_wave_*17u)});
                }
            }
            return step;
        }
    }
    if(state_.phase==SurvivalPhase::Defeat){
        state_.playing_mask=0;state_.waiting_mask=o.connected;state_.phase_remaining_ms=remaining(o.now_ms,phase_started_ms_,config_.restart_ms);
        return {state_,{},state_.phase_remaining_ms==0};
    }
    return {state_,{},false};
}
bool SurvivalController::finish_spawn(uint64_t ticket,bool success){
    const auto found=std::find_if(pending_.begin(),pending_.end(),[&](const auto& p){return p.ticket==ticket && p.wave==state_.wave;});
    if(!ticket || found==pending_.end() || state_.phase!=SurvivalPhase::Running)return false;
    pending_.erase(found);
    if(success){++spawned_wave_;state_.total_spawned=add_count(state_.total_spawned,1);state_.active_enemies=add_count(state_.active_enemies,1);state_.quota_remaining=wave_quota()-std::min(wave_quota(),spawned_wave_);}else ++failed_wave_;
    return true;
}
void SurvivalController::restart(uint64_t now_ms){
    if(clock_set_ && now_ms<last_ms_)return;
    const auto best=state_.best_ms;const auto round=state_.round==std::numeric_limits<uint32_t>::max()?state_.round:state_.round+1;
    state_={};state_.round=round;state_.best_ms=best;last_ms_=phase_started_ms_=round_started_ms_=next_spawn_ms_=now_ms;clock_set_=true;spawned_wave_=failed_wave_=0;pending_.clear();
}
}
