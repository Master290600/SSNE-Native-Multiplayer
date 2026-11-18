#pragma once
#include "world_state.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace port::coop {
enum class PingKind:uint8_t {Location,Enemy,Pickup};
struct TacticalPingRequest {
    uint64_t epoch=0;
    std::array<float,3> position{};
    EntityId target=0;
    PingKind kind=PingKind::Location;
};
struct TacticalPing {
    uint64_t sequence=0,epoch=0;
    PlayerId sender{};
    std::string name;
    std::array<float,3> position{};
    EntityId target=0;
    PingKind kind=PingKind::Location;
    uint32_t remaining_ms=0;
};
struct PlayerLatency {PlayerId id{};unsigned ping_ms=0;bool known=false;};
inline constexpr uint32_t tactical_ping_lifetime_ms=8000,tactical_ping_cooldown_ms=750;
inline constexpr float tactical_ping_range=250.f;
inline bool tactical_ping_position_valid(const std::array<float,3>& p){
    return std::all_of(p.begin(),p.end(),[](float n){return std::isfinite(n) && std::abs(n)<=1000000.f;});
}
inline bool tactical_ping_request_valid(const TacticalPingRequest& p){
    return p.epoch && tactical_ping_position_valid(p.position) && unsigned(p.kind)<=unsigned(PingKind::Pickup) &&
        ((p.kind==PingKind::Location)==(p.target==0));
}
inline const std::array<float,3>* tactical_ping_position(const EntityState& entity){
    const auto it=entity.properties.find("position");
    return it==entity.properties.end()?nullptr:std::get_if<std::array<float,3>>(&it->second);
}
inline bool tactical_ping_alive(const EntityState& entity){
    const auto it=entity.properties.find("health");
    if(it==entity.properties.end())return entity.kind!=EntityKind::Enemy && entity.kind!=EntityKind::Player;
    const auto* health=std::get_if<float>(&it->second);
    return health && std::isfinite(*health) && *health>0.f;
}
inline bool tactical_ping_pickup_available(const EntityState& entity,unsigned slot){
    if(slot>=max_players)return false;
    if(const auto it=entity.properties.find("pickup_view_mask");it!=entity.properties.end())
        if(const auto* mask=std::get_if<int64_t>(&it->second);mask && !(*mask&(int64_t(1)<<slot)))return false;
    if(const auto it=entity.properties.find("pickup_ownership");it!=entity.properties.end())
        if(const auto* owners=std::get_if<std::vector<int64_t>>(&it->second);owners && slot<owners->size() && (*owners)[slot]!=0)return false;
    return true;
}
inline bool tactical_ping_gameplay(const WorldState& world){
    const auto it=world.mission.find("Session.NativePhase");
    return it!=world.mission.end() && it->second==Property(std::string("gameplay"));
}
// Small transient state, separate from authoritative world/checkpoint graphs.
// No socket or UI clock is trusted: only the server supplies now/identity.
class TacticalPingLog {
    struct Entry {TacticalPing ping;uint64_t expires=0;};
    struct Rate {PlayerId id{};uint64_t previous=0;bool sent=false;};
    std::array<Entry,max_players> entries_{};
    std::array<Rate,max_players> rates_{};
    uint64_t epoch_=0,sequence_=0;
public:
    void clear(uint64_t epoch=0){entries_={};rates_={};epoch_=epoch;}
    void remove(PlayerId id){if(id.slot<max_players && entries_[id.slot].ping.sender==id)entries_[id.slot]={};}
    bool send(const Participant& sender,const TacticalPingRequest& request,const WorldState& world,uint64_t now){
        if(world.epoch!=epoch_)clear(world.epoch);
        if(!tactical_ping_request_valid(request) || request.epoch!=world.epoch || !tactical_ping_gameplay(world) ||
           !sender.connected || !sender.ready || sender.afk || sender.spectator || sender.id.slot>=max_players || !sender.id.generation ||
           sequence_==std::numeric_limits<uint64_t>::max())return false;
        const auto actor=world.entities.find(sender.character);
        if(actor==world.entities.end() || actor->second.kind!=EntityKind::Player || actor->second.owner!=sender.id || !tactical_ping_alive(actor->second))return false;
        const auto* origin=tactical_ping_position(actor->second);if(!origin || !tactical_ping_position_valid(*origin))return false;
        auto point=request.position;
        if(request.target){
            const auto target=world.entities.find(request.target);
            const auto expected=request.kind==PingKind::Enemy?EntityKind::Enemy:EntityKind::Pickup;
            if(target==world.entities.end() || target->second.kind!=expected || !tactical_ping_alive(target->second))return false;
            if(expected==EntityKind::Pickup && !tactical_ping_pickup_available(target->second,sender.id.slot))return false;
            const auto* position=tactical_ping_position(target->second);if(!position || !tactical_ping_position_valid(*position))return false;
            point=*position;
        }
        double distance=0;for(unsigned axis=0;axis<3;++axis){const double d=double(point[axis])-(*origin)[axis];distance+=d*d;}
        if(distance>double(tactical_ping_range)*tactical_ping_range)return false;
        auto& rate=rates_[sender.id.slot];
        if(rate.id==sender.id && rate.sent && (now<rate.previous || now-rate.previous<tactical_ping_cooldown_ms))return false;
        rate={sender.id,now,true};
        entries_[sender.id.slot]={{++sequence_,world.epoch,sender.id,sender.name,point,request.target,request.kind,tactical_ping_lifetime_ms},
            now>std::numeric_limits<uint64_t>::max()-tactical_ping_lifetime_ms?std::numeric_limits<uint64_t>::max():now+tactical_ping_lifetime_ms};
        return true;
    }
    std::vector<TacticalPing> messages(const WorldState& world,const std::array<Participant,max_players>& people,uint64_t now)const{
        std::vector<TacticalPing> out;if(world.epoch!=epoch_ || !tactical_ping_gameplay(world))return out;
        for(const auto& entry:entries_){
            const auto& p=entry.ping;if(!p.sequence || entry.expires<=now || p.sender.slot>=max_players)continue;
            const auto& sender=people[p.sender.slot];
            if(sender.id!=p.sender || !sender.connected || !sender.ready || sender.afk || sender.spectator)continue;
            const auto actor=world.entities.find(sender.character);
            if(actor==world.entities.end() || actor->second.owner!=sender.id || !tactical_ping_alive(actor->second))continue;
            auto shown=p;
            if(p.target){
                const auto target=world.entities.find(p.target);
                if(target==world.entities.end() || target->second.kind!=(p.kind==PingKind::Enemy?EntityKind::Enemy:EntityKind::Pickup) || !tactical_ping_alive(target->second))continue;
                if(p.kind==PingKind::Pickup && !tactical_ping_pickup_available(target->second,p.sender.slot))continue;
                if(const auto* point=tactical_ping_position(target->second);point && tactical_ping_position_valid(*point))shown.position=*point;else continue;
            }
            shown.remaining_ms=uint32_t(std::min<uint64_t>(tactical_ping_lifetime_ms,entry.expires-now));out.push_back(std::move(shown));
        }
        return out;
    }
};
}
