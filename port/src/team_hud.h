#pragma once
#include "coop/world_state.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace port {
struct TeamHudSample {
    coop::PlayerId player{};
    coop::EntityId character=0;
    uint64_t epoch=0;
    float health=0,armor=0;
    std::array<float,3> position{};
    bool health_known=false,armor_known=false,position_known=false;
};
enum class TeamHudStatus {Loading,Playing,Dead,Afk,Waiting};
struct TeamHudRow {
    coop::PlayerId player{};
    std::string name;
    TeamHudStatus status=TeamHudStatus::Loading;
    bool bot=false,health_known=false,armor_known=false,distance_known=false;
    float health=0,armor=0,distance=0;
};
namespace team_hud_detail {
inline bool health_valid(float value){return std::isfinite(value) && std::abs(value)<=1000000.f;}
inline bool armor_valid(float value){return std::isfinite(value) && value>=0 && value<=1000000.f;}
inline bool position_valid(const std::array<float,3>& point){
    return std::all_of(point.begin(),point.end(),[](float value){return std::isfinite(value) && std::abs(value)<=1000000.f;});
}
inline bool participant_valid(const coop::Participant& person,unsigned slot){
    return person.connected && person.id.slot==slot && person.id.generation;
}
inline bool matches(const TeamHudSample& sample,const coop::Participant& person,uint64_t epoch){
    return epoch && sample.epoch==epoch && sample.player==person.id && person.character && sample.character==person.character;
}
}
// Missing/unknown mode must never show opponents as teammates. Survival uses
// native Coop mode1 and adds its own round policy to the same published world.
inline bool team_hud_cooperative(const coop::WorldState& world){
    const auto mode=world.mission.find("Session.GameMode"),phase=world.mission.find("Session.NativePhase");
    if(mode==world.mission.end() || phase==world.mission.end())return false;
    const auto* number=std::get_if<int64_t>(&mode->second);
    const auto* text=std::get_if<std::string>(&phase->second);
    return world.epoch && number && *number==1 && text && *text=="gameplay";
}
// At most eight map lookups; consumes an immutable published snapshot only.
// The host can supply equivalent samples from its existing native player cache.
inline std::array<TeamHudSample,coop::max_players> team_hud_samples(const coop::WorldState& world){
    std::array<TeamHudSample,coop::max_players> samples{};
    if(!world.epoch)return samples;
    for(unsigned slot=0;slot<samples.size();++slot){
        const auto& person=world.players[slot];
        if(!team_hud_detail::participant_valid(person,slot) || !person.character)continue;
        const auto found=world.entities.find(person.character);
        if(found==world.entities.end())continue;
        const auto& entity=found->second;
        if(entity.id!=person.character || entity.kind!=coop::EntityKind::Player || entity.owner!=person.id)continue;
        auto& sample=samples[slot];sample.player=person.id;sample.character=person.character;sample.epoch=world.epoch;
        const auto number=[&](const char* key)->const float*{
            const auto value=entity.properties.find(key);
            return value==entity.properties.end()?nullptr:std::get_if<float>(&value->second);
        };
        if(const auto* value=number("health");value && team_hud_detail::health_valid(*value)){sample.health=*value;sample.health_known=true;}
        if(const auto* value=number("armor");value && team_hud_detail::armor_valid(*value)){sample.armor=*value;sample.armor_known=true;}
        if(const auto value=entity.properties.find("position");value!=entity.properties.end())
            if(const auto* point=std::get_if<std::array<float,3>>(&value->second);point && team_hud_detail::position_valid(*point)){sample.position=*point;sample.position_known=true;}
    }
    return samples;
}
// Stateless: an epoch, connection generation, or character change immediately
// invalidates old measurements. Unknown values are never presented as zero.
inline std::vector<TeamHudRow> team_hud_rows(uint64_t epoch,bool cooperative,coop::PlayerId own,
    const std::array<coop::Participant,coop::max_players>& participants,
    const std::array<TeamHudSample,coop::max_players>& samples,uint8_t bots,
    uint8_t waiting_mask=0,uint8_t eliminated_mask=0){
    std::vector<TeamHudRow> rows;
    if(!cooperative || !epoch || own.slot>=participants.size() || !own.generation)return rows;
    const auto& local=participants[own.slot];
    if(!team_hud_detail::participant_valid(local,own.slot) || local.id!=own)return rows;
    const auto& origin=samples[own.slot];
    const bool origin_known=local.ready && !local.spectator && !(waiting_mask&(1u<<own.slot)) && !(eliminated_mask&(1u<<own.slot)) &&
        team_hud_detail::matches(origin,local,epoch) && origin.health_known && team_hud_detail::health_valid(origin.health) && origin.health>0 &&
        origin.position_known && team_hud_detail::position_valid(origin.position);
    rows.reserve(coop::max_players-1);
    for(unsigned slot=0;slot<participants.size();++slot){
        const auto& person=participants[slot];
        if(slot==own.slot || !team_hud_detail::participant_valid(person,slot) || person.spectator)continue;
        TeamHudRow row;row.player=person.id;row.name=person.name;row.bot=(bots&(1u<<slot))!=0;
        const auto& sample=samples[slot];
        const bool waiting=(waiting_mask&(1u<<slot))!=0,eliminated=(eliminated_mask&(1u<<slot))!=0;
        const bool current=person.ready && !waiting && !eliminated && team_hud_detail::matches(sample,person,epoch);
        row.health_known=current && sample.health_known && team_hud_detail::health_valid(sample.health);
        row.health=row.health_known?std::max(0.f,sample.health):0;
        row.status=!person.ready?TeamHudStatus::Loading:eliminated?TeamHudStatus::Dead:waiting?TeamHudStatus::Waiting:
            row.health_known && row.health<=0?TeamHudStatus::Dead:person.afk?TeamHudStatus::Afk:TeamHudStatus::Playing;
        row.armor_known=current && row.status!=TeamHudStatus::Dead && sample.armor_known && team_hud_detail::armor_valid(sample.armor);
        row.armor=row.armor_known?sample.armor:0;
        if(current && origin_known && row.health_known && row.health>0 && sample.position_known && team_hud_detail::position_valid(sample.position)){
            row.distance=std::hypot(sample.position[0]-origin.position[0],sample.position[1]-origin.position[1],sample.position[2]-origin.position[2]);
            row.distance_known=std::isfinite(row.distance);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
}
