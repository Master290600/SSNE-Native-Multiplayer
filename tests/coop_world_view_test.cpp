#include "world_state.h"
#include "native_event_journal.h"
#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
using namespace port::coop;
namespace port::coop {
bool reference_view_validate_world(const WorldState&,std::string&);
WorldDelta reference_view_difference(const WorldState*,const WorldState&);
std::vector<uint8_t> reference_view_encode_delta(const WorldDelta&);
}
namespace {
void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
std::vector<uint8_t> reference_bytes(const WorldState* before,const WorldState& now){auto wire=reference_view_encode_delta(reference_view_difference(before,now));if(wire.size()<=32)throw std::runtime_error("Frozen reference frame truncated");wire[32]|=2;uint8_t watchers=0;for(unsigned slot=0;slot<max_players;++slot)if(now.players[slot].spectator)watchers|=uint8_t(1u<<slot);wire.push_back(watchers);return wire;}
WorldState materialize(const WorldState& body,const Properties& overlay){auto copy=body;copy.mission=overlay;return copy;}
WorldState scene(unsigned seed){
    WorldState w;w.epoch=7;w.revision=100+seed;w.tick=400+seed;w.level="Rlevel1_4a";
    w.mission={{"fixture",int64_t(seed)},{"remove_me",std::string("earlier metadata")},{"Session.NativePhase",std::string("gameplay")},{"Session.AmmoCapacity",int64_t(3)},{"Session.EnemyMultiplier",int64_t(5)}};
    for(unsigned slot=0;slot<2;++slot){auto& p=w.players[slot];p.id={slot,10+slot};p.connected=true;p.ready=true;p.afk=slot==1;p.character=1+slot;p.name="Player "+std::to_string(slot);w.input_ack[slot]=0x100000000ull+slot+seed;
        EntityState e;e.id=p.character;e.kind=EntityKind::Player;e.definition=2;e.owner=p.id;e.resource="Player/SeriousSam";e.properties={{"player_slot",int64_t(slot)},{"health",100.f},{"position",std::array<float,3>{float(slot),2,3}}};w.entities.emplace(e.id,std::move(e));}
    for(unsigned n=0;n<84;++n){EntityState e;e.id=100+n;e.kind=EntityKind::Enemy;e.definition=n%61;e.resource="Enemies/Enemy";
        std::vector<float> bones(12*(1+(n+seed)%128));for(unsigned k=0;k<bones.size();++k)bones[k]=(int(k%17)-8)*.125f;
        bones[0]=std::bit_cast<float>(0x80000000u);if(bones.size()>1)bones[1]=std::numeric_limits<float>::denorm_min();
        e.properties={{"enemy_bone_matrices",std::move(bones)},{"health",float(100+n)},{"position",std::array<float,3>{float(n),2,-3}},{"rotation",std::array<float,4>{0,0,0,1}},{"target",EntityReference{1}},{"name",std::string("enemy")},{"flags",std::vector<int64_t>{INT64_MIN,int64_t(n),INT64_MAX}}};w.entities.emplace(e.id,std::move(e));}
    return w;
}
void parity(const WorldState* before,const Properties* before_overlay,const WorldState& body,const Properties& overlay){
    const auto expected=materialize(body,overlay);std::optional<WorldState> expected_before;if(before)expected_before=materialize(*before,*before_overlay);
    std::optional<WorldReadView> before_view;if(before)before_view.emplace(*before,*before_overlay);
    const auto actual_view=WorldReadView(body,overlay);std::string a="previous",b="previous";
    require(reference_view_validate_world(expected,a)==validate_world(actual_view,b) && a==b,"View validation differs from independent frozen oracle");
    const auto expected_wire=reference_bytes(expected_before?&*expected_before:nullptr,expected);
    const auto actual_wire=encode_delta(difference(before_view?&*before_view:nullptr,actual_view));
    require(actual_wire==expected_wire,"View full/delta wire differs from independent frozen oracle");
    const auto normal_wire=encode_delta(difference(expected_before?&*expected_before:nullptr,expected));require(normal_wire==expected_wire,"WorldState adapter changed original wire");
    WorldDelta decoded;require(decode_delta(actual_wire,decoded,b),"View wire decode rejected");WorldState applied;if(expected_before)applied=*expected_before;
    require(apply_delta(applied,decoded,b),"View wire application rejected");require(reference_bytes(nullptr,applied)==reference_bytes(nullptr,expected),"View delta did not reconstruct selected body/mission");
    require(&actual_view.entities==&body.entities && &actual_view.mission==&overlay && &actual_view.players==&body.players,"View cloned a borrowed payload");
    if(!body.entities.empty()){auto it=body.entities.find(100);if(it!=body.entities.end())require(std::get<std::vector<float>>(actual_view.entities.at(100).properties.at("enemy_bone_matrices")).data()==std::get<std::vector<float>>(it->second.properties.at("enemy_bone_matrices")).data(),"View cloned bone storage");}
}
void rejected(const WorldState& body,const Properties& overlay){
    const auto expected=materialize(body,overlay);std::string a="previous",b="previous";
    require(!reference_view_validate_world(expected,a) && !validate_world(WorldReadView(body,overlay),b) && a==b,"Malformed view rejection/error differs from frozen validator");
    bool old_failed=false,new_failed=false;std::string old_error,new_error;
    try{(void)reference_view_difference(nullptr,expected);}catch(const std::invalid_argument& e){old_failed=true;old_error=e.what();}
    try{(void)difference(nullptr,WorldReadView(body,overlay));}catch(const std::invalid_argument& e){new_failed=true;new_error=e.what();}
    require(old_failed && new_failed && old_error==new_error,"Malformed overlay bypassed full diff validation");
}
void rocket_window(WorldState& w,unsigned first,unsigned last){
    std::vector<int64_t> ids,styles;std::vector<float> values;
    for(unsigned n=first;n<=last;++n){ids.push_back(n);styles.push_back(n%3);for(unsigned k=0;k<10;++k)values.push_back(float(n+k)*.01f);}
    w.mission["Session.RocketEffects.Ids"]=std::move(ids);w.mission["Session.RocketEffects.Styles"]=std::move(styles);w.mission["Session.RocketEffects.Values"]=std::move(values);
}
}
int main(){
    static_assert(!std::is_constructible_v<WorldReadView,WorldState&&>);
    static_assert(!std::is_constructible_v<WorldReadView,const WorldState&,Properties&&>);
    static_assert(!std::is_constructible_v<WorldReadView,const WorldState&&>);
    static_assert(!std::is_constructible_v<WorldReadView,const WorldState&,const Properties&&>);
    static_assert(!std::is_constructible_v<WorldReadView,WorldState&&,const Properties&>);
    static_assert(!std::is_constructible_v<WorldReadView,const WorldState&&,const Properties&>);
    for(unsigned n=0;n<32;++n){
        auto before=scene(n),now=before;const auto before_overlay=before.mission;auto overlay=now.mission;
        ++now.revision;now.tick+=3;now.input_ack[0]+=17;now.players[0].name="Renamed player";
        overlay.erase("remove_me");overlay["new_overlay"]=std::vector<float>{-0.f,.125f,17.f};overlay["fixture"]=int64_t(999);
        now.entities.at(100).properties["health"]=41.f;now.entities.at(101).properties.erase("flags");
        now.entities.at(102).properties["enemy_bone_matrices"]=std::vector<float>(n%2?4096:0,.25f);
        now.entities.erase(103);auto born=now.entities.at(104);born.id=1000+n;born.resource="Enemies/New";now.entities.emplace(born.id,std::move(born));
        if(n%3==0){++now.epoch;now.revision=1;now.tick=0;now.level="Alevel10_4";}
        const auto before_bytes=reference_bytes(nullptr,before),now_bytes=reference_bytes(nullptr,now);
        parity(nullptr,nullptr,now,overlay);parity(&before,&before_overlay,now,overlay);
        require(reference_bytes(nullptr,before)==before_bytes && reference_bytes(nullptr,now)==now_bytes,"View mutated source graph");
    }
    auto good=scene(1);auto overlay=good.mission;
    auto bad=overlay;bad["invalid\nkey"]=int64_t(1);rejected(good,bad);
    bad=overlay;bad["nonfinite"]=std::numeric_limits<float>::infinity();rejected(good,bad);
    bad=overlay;bad["oversize"]=std::vector<float>(4097,0);rejected(good,bad);
    bad=overlay;bad["Session.AmmoCapacity"]=int64_t(6);rejected(good,bad);
    bad=overlay;bad["Session.Results.Active"]=int64_t(1);rejected(good,bad);
    bad=overlay;bad["Session.Presentation.Scene"]=int64_t(3);rejected(good,bad);
    auto invalid_body=good;invalid_body.entities.erase(1);rejected(invalid_body,overlay);
    invalid_body=good;invalid_body.entities.at(100).properties["health"]=std::numeric_limits<float>::quiet_NaN();rejected(invalid_body,overlay);
    invalid_body=good;invalid_body.players[0].name.clear();rejected(invalid_body,overlay);
    invalid_body=good;invalid_body.vote={17,2,1,false};rejected(invalid_body,overlay);
    // Event cursors select different owning overlays over precisely one body.
    auto events=scene(2);rocket_window(events,1,200);NativeEventJournal journal;journal.ingest(events);
    auto owned_body=std::make_shared<const WorldState>(events);const auto immutable_bytes=reference_bytes(nullptr,*owned_body);
    NativeEventCursors first{},second{};second[1]=170;
    WorldState full_first=*owned_body,full_second=*owned_body;journal.decorate(full_first,first,true);journal.decorate(full_second,second,true);
    WorldState mission_first,mission_second;mission_first.epoch=mission_second.epoch=events.epoch;mission_first.mission=mission_second.mission=owned_body->mission;
    journal.decorate(mission_first,first,true);journal.decorate(mission_second,second,true);
    require(std::get<std::vector<int64_t>>(mission_first.mission.at("Session.RocketEffects.Ids")).front()==1 && std::get<std::vector<int64_t>>(mission_first.mission.at("Session.RocketEffects.Ids")).back()==128,"First consumer batch");
    require(std::get<std::vector<int64_t>>(mission_second.mission.at("Session.RocketEffects.Ids")).front()==171,"Second consumer batch");
    require(reference_bytes(nullptr,full_first)==encode_delta(difference(nullptr,WorldReadView(*owned_body,mission_first.mission))),"First peer overlay wire parity");
    require(reference_bytes(nullptr,full_second)==encode_delta(difference(nullptr,WorldReadView(*owned_body,mission_second.mission))),"Second peer overlay wire parity");
    auto retained=owned_body;const auto saved_overlay=std::move(mission_first.mission);const auto saved_wire=encode_delta(difference(nullptr,WorldReadView(*retained,saved_overlay)));
    owned_body.reset();journal.prune(journal.tails());events.entities.clear();
    require(saved_wire==encode_delta(difference(nullptr,WorldReadView(*retained,saved_overlay))) && reference_bytes(nullptr,*retained)==immutable_bytes,"Retained body/overlay changed after producer prune/release");
    const auto old_view=WorldReadView(*retained,saved_overlay),new_view=WorldReadView(*retained,mission_second.mission);
    require(encode_delta(difference(&old_view,new_view))==reference_bytes(&full_first,full_second),"Different peer overlay baseline delta");
    std::cout<<"WorldReadView: independent frozen validator/diff/wire parity, full/delta/epochs/lifecycles/large bones/metadata, malformed overlays, immutable borrowed arrays and two retained event-cursor overlays passed.\n";
}
