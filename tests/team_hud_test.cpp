#include "../port/src/team_hud.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace port;
using namespace port::coop;
namespace {
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
WorldState fixture(){
    WorldState world;world.epoch=42;world.level="Rlevel1_1";
    world.mission["Session.GameMode"]=int64_t(1);world.mission["Session.NativePhase"]=std::string("gameplay");
    for(unsigned slot=0;slot<max_players;++slot){
        auto& person=world.players[slot];person={{slot,10+slot},true,true,false,100+slot,"Player "+std::to_string(slot)};
        EntityState entity;entity.id=person.character;entity.kind=EntityKind::Player;entity.owner=person.id;
        entity.properties["health"]=100.f;entity.properties["armor"]=25.f;entity.properties["position"]=std::array<float,3>{float(slot*2),0,0};
        world.entities.emplace(entity.id,std::move(entity));
    }
    return world;
}
std::vector<TeamHudRow> rows(const WorldState& world,uint8_t bots=0,uint8_t waiting=0,uint8_t eliminated=0){
    return team_hud_rows(world.epoch,team_hud_cooperative(world),world.players[0].id,world.players,team_hud_samples(world),bots,waiting,eliminated);
}
const TeamHudRow& player(const std::vector<TeamHudRow>& values,unsigned slot){
    const auto found=std::find_if(values.begin(),values.end(),[&](const auto& row){return row.player.slot==slot;});
    if(found==values.end())throw std::runtime_error("Expected teammate is absent");return *found;
}
void normal_team_and_round_states(){
    auto world=fixture();world.players[1].afk=true;world.players[4].spectator=true;world.players[5].ready=false;
    world.entities.at(101).properties["health"]=85.25f;
    world.entities.at(101).properties["armor"]=0.f;
    world.entities.at(101).properties["position"]=std::array<float,3>{3,4,12};
    world.entities.at(103).properties["health"]=-3.f;
    const auto actual=rows(world,1u<<2,1u<<6,1u<<7);
    require(actual.size()==6,"Self or voluntary spectators leaked into compact team rows");
    const auto& afk=player(actual,1);
    require(afk.status==TeamHudStatus::Afk && !afk.bot && afk.health_known && afk.health==85.25f,"AFK teammate lost authoritative health");
    require(afk.armor_known && afk.armor==0 && afk.distance_known && std::abs(afk.distance-13.f)<.001f,"Zero armor or three-dimensional distance became unknown");
    require(player(actual,2).bot && player(actual,2).status==TeamHudStatus::Playing,"Bot flag changed the teammate's playing state");
    const auto& dead=player(actual,3);
    require(dead.status==TeamHudStatus::Dead && dead.health_known && dead.health==0 && !dead.armor_known && !dead.distance_known,"Dead teammate exposed stale armor/distance");
    const auto& loading=player(actual,5);
    require(loading.status==TeamHudStatus::Loading && !loading.health_known && !loading.armor_known && !loading.distance_known,"Loading teammate inherited old native measurements");
    const auto& waiting=player(actual,6);const auto& eliminated=player(actual,7);
    require(waiting.status==TeamHudStatus::Waiting && !waiting.health_known && !waiting.armor_known && !waiting.distance_known,"Next-round wait exposed a reserved actor's vitals");
    require(eliminated.status==TeamHudStatus::Dead && !eliminated.health_known && !eliminated.armor_known && !eliminated.distance_known,"Survival elimination exposed a stale live actor");
    world.mission["Session.Survival.Active"]=int64_t(1);world.mission["Session.CustomMode"]=int64_t(5);
    require(team_hud_cooperative(world),"Native Coop-backed Survival was excluded");
}
void identity_and_epoch_changes(){
    auto world=fixture();const auto old=team_hud_samples(world);
    ++world.players[1].id.generation;
    auto actual=team_hud_rows(world.epoch,true,world.players[0].id,world.players,old,0);
    require(!player(actual,1).health_known && !player(actual,1).armor_known && !player(actual,1).distance_known,"Reused slot inherited old generation measurements");
    require(!team_hud_samples(world)[1].health_known,"World extraction accepted an old entity owner");
    world=fixture();++world.players[1].character;
    actual=team_hud_rows(world.epoch,true,world.players[0].id,world.players,old,0);
    require(!player(actual,1).health_known && !player(actual,1).armor_known && !player(actual,1).distance_known,"Respawn/rebound character inherited the previous actor");
    world=fixture();++world.epoch;
    actual=team_hud_rows(world.epoch,true,world.players[0].id,world.players,old,0);
    require(std::all_of(actual.begin(),actual.end(),[](const auto& row){return !row.health_known && !row.armor_known && !row.distance_known;}),"Level epoch retained previous-world measurements");
    world=fixture();world.entities.at(101).id=999;
    require(!team_hud_samples(world)[1].health_known,"Entity map key and entity identity disagree");
    world=fixture();world.entities.at(101).kind=EntityKind::Enemy;
    require(!team_hud_samples(world)[1].health_known,"A reused entity identity accepted a nonplayer");
    world=fixture();world.entities.erase(101);
    require(!player(rows(world),1).health_known,"Missing character displayed a false zero health");
}
void finite_and_independent_unknowns(){
    auto world=fixture();auto& fields=world.entities.at(101).properties;
    fields["health"]=std::numeric_limits<float>::quiet_NaN();
    auto actual=rows(world);
    require(!player(actual,1).health_known && player(actual,1).armor_known && !player(actual,1).distance_known,"NaN health leaked or erased independent known armor");
    fields["health"]=100.f;fields["armor"]=-1.f;
    actual=rows(world);
    require(player(actual,1).health_known && !player(actual,1).armor_known && player(actual,1).distance_known,"Invalid armor contaminated valid health/distance");
    fields["position"]=std::array<float,3>{std::numeric_limits<float>::infinity(),0,0};
    require(!player(rows(world),1).distance_known,"Infinite position produced a distance");
    fields["position"]=std::array<float,3>{1000001.f,0,0};
    require(!team_hud_samples(world)[1].position_known,"Out-of-world coordinate was presented as known");
    fields["health"]=int64_t(100);fields["armor"]=1000001.f;
    actual=rows(world);
    require(!player(actual,1).health_known && !player(actual,1).armor_known,"Wrong property type or out-of-range armor became known");
    // Host cache adapters cannot bypass the same finite checks with true flags.
    auto direct=team_hud_samples(fixture());direct[1].health=std::numeric_limits<float>::infinity();
    world=fixture();actual=team_hud_rows(world.epoch,true,world.players[0].id,world.players,direct,0);
    require(!player(actual,1).health_known && !player(actual,1).distance_known,"Direct native cache bypassed numeric validation");
}
void modes_and_local_view(){
    auto world=fixture();require(rows(world).size()==7,"Eight-player team must produce exactly seven teammates");
    for(const auto mode:{0,2,3,4,5}){
        world.mission["Session.GameMode"]=int64_t(mode);
        world.mission["Session.Survival.Active"]=int64_t(1);
        require(rows(world).empty(),"Unknown or competitive mode exposed opponents as teammates");
    }
    world=fixture();world.mission.erase("Session.GameMode");require(rows(world).empty(),"Missing mode defaulted to Coop");
    world=fixture();world.mission["Session.NativePhase"]=std::string("loading");require(rows(world).empty(),"Loading world displayed a live team HUD");
    world=fixture();auto samples=team_hud_samples(world);auto own=world.players[0].id;++own.generation;
    require(team_hud_rows(world.epoch,true,own,world.players,samples,0).empty(),"Stale local connection received a team view");
    world.players[0].spectator=true;
    auto actual=rows(world);
    require(actual.size()==7 && std::all_of(actual.begin(),actual.end(),[](const auto& row){return row.health_known && !row.distance_known;}),"Spectator measured distance from a reserved player actor");
    world.players[0].spectator=false;world.entities.at(100).properties["health"]=0.f;
    actual=rows(world);
    require(std::none_of(actual.begin(),actual.end(),[](const auto& row){return row.distance_known;}),"Dead local player supplied a live distance origin");
    world=fixture();world.players[1].connected=false;world.players[2].id.slot=7;world.players[3].id.generation=0;
    require(rows(world).size()==4,"Disconnected or malformed participant appeared in team rows");
}
}
int main(){try{
    normal_team_and_round_states();identity_and_epoch_changes();finite_and_independent_unknowns();modes_and_local_view();
    std::cout<<"Team HUD modes, teammate identity, epochs, round states, vitals and distance passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
