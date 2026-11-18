#pragma once
#include "session_authority.h"
#include "property_index.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace port::coop {
using EntityId = uint64_t;
struct EntityReference { EntityId id=0; bool operator==(const EntityReference&)const=default; };
using Property = std::variant<int64_t,float,std::array<float,3>,std::array<float,4>,
                              EntityReference,std::string,std::vector<int64_t>,std::vector<float>>;
using Properties = IndexedPropertyMap<Property>;
enum class EntityKind : uint8_t { Player,Enemy,Projectile,Pickup,Door,Switch,Destructible,Physics,Mission };
struct EntityState {
    EntityId id=0;
    EntityKind kind=EntityKind::Mission;
    uint32_t definition=0;
    std::string resource;
    PlayerId owner;
    Properties properties;
    bool operator==(const EntityState&)const=default;
};
struct WorldState {
    uint64_t epoch=1, revision=0, tick=0;
    std::string level;
    Properties mission;
    std::map<EntityId,EntityState> entities;
    std::array<Participant,max_players> players{};
    std::array<uint64_t,max_players> input_ack{};
    SkipVote vote;
};
// Borrowed read-only graph. Owners must retain both the immutable body and the
// selected mission overlay throughout validation/differencing. Payloads are
// referenced, never cloned; metadata and entity validation remain identical.
struct WorldReadView {
    const uint64_t epoch,revision,tick;
    const std::string& level;
    const Properties& mission;
    const std::map<EntityId,EntityState>& entities;
    const std::array<Participant,max_players>& players;
    const std::array<uint64_t,max_players>& input_ack;
    const SkipVote& vote;
    explicit WorldReadView(const WorldState& body):WorldReadView(body,body.mission){}
    WorldReadView(const WorldState& body,const Properties& overlay)
        :epoch(body.epoch),revision(body.revision),tick(body.tick),level(body.level),
         mission(overlay),entities(body.entities),players(body.players),input_ack(body.input_ack),vote(body.vote){}
    WorldReadView(WorldState&&)=delete;
    WorldReadView(const WorldState&&)=delete;
    WorldReadView(const WorldState&,Properties&&)=delete;
    WorldReadView(const WorldState&,const Properties&&)=delete;
    WorldReadView(WorldState&&,const Properties&)=delete;
    WorldReadView(const WorldState&&,const Properties&)=delete;
};
struct PropertyPatch { Properties set; std::vector<std::string> remove; };
struct EntityPatch {
    EntityId id=0;
    bool create=false;
    EntityKind kind=EntityKind::Mission;
    uint32_t definition=0;
    std::string resource;
    PlayerId owner;
    PropertyPatch properties;
};
struct WorldDelta {
    uint64_t epoch=0,base_revision=0,revision=0,tick=0;
    bool full=false;
    std::string level;
    PropertyPatch mission;
    std::vector<EntityPatch> entities;
    std::vector<EntityId> remove;
    std::array<Participant,max_players> players{};
    std::array<uint64_t,max_players> input_ack{};
    SkipVote vote;
};
bool validate_world(WorldReadView,std::string& error);
bool validate_world(const WorldState&,std::string& error);
WorldDelta difference(const WorldReadView* baseline,WorldReadView current);
WorldDelta difference(const WorldState* baseline,const WorldState& current);
// Applies atomically after validation. A failed delta leaves the old world intact.
bool apply_delta(WorldState& world,const WorldDelta&,std::string& error);
// Prepare a validated independent snapshot with a single baseline copy.
std::optional<WorldState> apply_delta_copy(const WorldState&,const WorldDelta&,std::string& error);
std::vector<uint8_t> encode_delta(const WorldDelta&);
bool decode_delta(const std::vector<uint8_t>&,WorldDelta&,std::string& error);
inline constexpr size_t max_frame_bytes=8*1024*1024;
}
