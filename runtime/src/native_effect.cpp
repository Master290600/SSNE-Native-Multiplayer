#include "ssne/native_effect.h"
#include "ssne/native_entity.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/audio.h"
#include "ssne/replica_simulation.h"
#include "ssne/native_party.h"
#include "ssne/native_spatial_audio_pan.h"
#include <cmath>
#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <chrono>
#include <map>
#include <optional>
#include <algorithm>
#include <fstream>
#include <type_traits>
#include <cstring>
namespace ssne {
namespace {
PPCFunc original=nullptr,original_damage=nullptr,original_shot=nullptr;std::atomic<bool> authoritative{false};uint64_t sequence=0,shot_sequence=0,subscription=0;
thread_local bool presenting=false;thread_local uint64_t presentation_damage_calls=0;
thread_local uint32_t energy_transition_object=0;
thread_local unsigned energy_transition_skipped=0;
std::deque<NativeRocketEffect> history;std::mutex effect_mutex;
uint64_t producer_epoch=1;
NativeProducerCursors producer_accepted{};
template<class T>void retain_native_event(std::deque<T>& queue,const T& event){
    if(queue.size()>=32768)fatal("Native event producer exceeded unpublished capacity");
    queue.push_back(event);
}
std::deque<NativeWeaponShot> shots;
std::atomic<bool> test_fire_request{false};
PPCFunc original_loop_start=nullptr,original_loop_stop=nullptr;
struct HostLoop {NativeWeaponLoop event;std::chrono::steady_clock::time_point began;};
struct ReplicaLoop {uint32_t handle=0;uint64_t origin=0;std::chrono::steady_clock::time_point began,stop_at{};unsigned weapon=2;};
std::map<uint32_t,HostLoop> host_loops;std::map<unsigned,ReplicaLoop> replica_loops;
// Weapon phase, charge and idle cues have explicit native stop lifetimes. They are
// started through the one-shot API, but must not be treated as fire-and-forget.
std::map<uint32_t,uint64_t> host_cue_voices;
std::map<uint64_t,uint32_t> replica_cue_voices;
std::deque<NativeWeaponLoop> loop_history;uint64_t loop_sequence=0,audio_order=0;
PPCFunc original_voice=nullptr;
PPCFunc original_subtract=nullptr;
std::function<NativeHitIdentity(uint32_t)> hit_resolver;std::deque<NativeConfirmedHit> confirmed_hits;uint64_t hit_sequence=0,automatic_ray_sequence=0;
std::map<uint32_t,uint64_t> weapon_shot_ids;
struct AttackTrace {uint32_t actor=0;unsigned slot=0,weapon=0,pellet=0;uint64_t shot=0;};thread_local AttackTrace attack_trace;
PPCFunc original_source_setter=nullptr;
PPCFunc original_area_receiver=nullptr;
PPCFunc original_common_damage=nullptr;
PPCFunc original_area_queue=nullptr;
struct RocketSource {AttackTrace attack;NativeHitIdentity identity;};
std::map<uint32_t,RocketSource> rocket_sources;
thread_local std::optional<RocketSource> rocket_damage_trace;
thread_local std::optional<RocketSource> area_damage_trace;
struct QueuedRocket {RocketSource source;uint32_t victim=0;uint64_t target=0;std::array<uint32_t,10> words{};};
std::map<uint32_t,QueuedRocket> queued_rockets;
// Original queue slots are recycled. Compare immutable event fields, excluding
// its own class word, visibility cache, timer and linked-list pointers.
std::array<uint32_t,10> queued_words(uint32_t p){std::array<uint32_t,10> out;const unsigned offsets[]={0,8,12,16,20,24,28,32,36,44};for(unsigned i=0;i<out.size();++i)out[i]=mem_read32(p+offsets[i]);return out;}
PPCFunc original_query=nullptr;unsigned query_probes=0,impact_probes=0;
PPCFunc original_feedback_sound=nullptr,original_feedback_score=nullptr;
std::deque<NativeFeedbackEvent> feedback_events;uint64_t feedback_sequence=0;
PPCFunc original_impact=nullptr;std::deque<NativeBulletImpact> impacts;uint64_t impact_sequence=0;
PPCFunc original_trail=nullptr;
std::deque<NativeBulletTrail> trails,replica_trails;
uint64_t trail_sequence=0;
std::optional<uint32_t> replica_trail_frame;
struct TrailView {uint32_t matrix;std::array<uint32_t,12> root;std::array<float,12> camera;unsigned owner=0;};
std::map<uint32_t,TrailView> replica_trail_views;
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;}
#include "native_spatial_audio.inc"
void rocket_source_hook(PPCContext& c) {
    const auto projectile=c.r[3],actor=c.r[4],instance=c.r[31],caller=c.lr;
    RocketSource provenance;bool capture=authoritative && caller==0x8005CFA4u &&
        readable(projectile,0x164) && mem_read32(projectile+0x48)==0x802022E0u &&
        readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u &&
        readable(instance,0x1D8) && mem_read32(instance+0xC)==0x801FBB38u && mem_read32(instance+0x1D4)==actor;
    if(capture){const auto slot=mem_read32(actor+0x17C);capture=slot<native_party_capacity() && mem_read32(native_party_registry()+slot*4)==actor &&
        (mem_read32(actor+0x4BC+6*24)==instance || mem_read32(actor+0x4C0+6*24)==instance);
        std::function<NativeHitIdentity(uint32_t)> resolver;
        {std::lock_guard lock(effect_mutex);resolver=hit_resolver;const auto shot=weapon_shot_ids.find(instance);if(shot==weapon_shot_ids.end())capture=false;else provenance.attack={actor,slot,6,0,shot->second};}
        if(capture && resolver){provenance.identity=resolver(actor);capture=provenance.identity.entity && provenance.identity.generation;}else capture=false;
    }
    ssne_call_original(c,0x8009EB94,original_source_setter);
    if(capture && mem_read32(projectile+0xC8)==actor){std::lock_guard lock(effect_mutex);rocket_sources[projectile]=provenance;
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-rocket-source-bound] projectile="+std::to_string(projectile)+" shot="+std::to_string(provenance.attack.shot)+" source="+std::to_string(actor)+" stable_source="+std::to_string(provenance.identity.entity)+" slot="+std::to_string(provenance.attack.slot)+" generation="+std::to_string(provenance.identity.generation));}
}
bool context_ok(const PPCContext& c){return mem_read32(0x80000000u)==0x47334250u && c.r[13]==0x802E22A0u && c.r[1]>=0x80004000u && readable(c.r[1]-0x4000,0x4000);}
struct Scratch {
    uint32_t start;std::array<uint8_t,0x4000> saved;
    explicit Scratch(const PPCContext& c):start(c.r[1]-0x4000){std::memcpy(saved.data(),g_ram+(start-0x80000000u),saved.size());}
    ~Scratch(){std::memcpy(g_ram+(start-0x80000000u),saved.data(),saved.size());}
};
struct PresentationScope {bool previous=presenting;PresentationScope(){presenting=true;}~PresentationScope(){presenting=previous;}};
// The original allocator still owns its 48 voice records. Disable only its
// per-resource four-voice gate while playing a verified party weapon sound.
// Restoring the exact previous gate also covers failed voice allocation.
struct PartySoundGate {
    PPCContext context;uint32_t previous=0;bool active=false;
    PartySoundGate(const PPCContext& c,bool enable):context(c),active(enable) {
        if(active){previous=mem_read32(context.r[13]-0x5BB0);auto gate=context;gate.r[4]=0;ssne_call(gate,0x80155E7C);}
    }
    void restore(){if(active){auto gate=context;gate.r[4]=previous;ssne_call(gate,0x80155E7C);active=false;if(mem_read32(context.r[13]-0x5BB0)!=previous)fatal("Native party sound gate was not restored");}}
    ~PartySoundGate(){restore();}
};
#include "native_weapon_emitter.inc"
#include "native_weapon_casing.inc"
#include "native_weapon_surface.inc"
#include "native_weapon_beam.inc"
#include "native_weapon_bomb.inc"
#include "native_gore.inc"
#include "native_hit_feedback.inc"
struct NativeExplosionClass {uint32_t table,size,initialize,update;};
constexpr NativeExplosionClass explosion_classes[]={{0x802174C8u,0x4FC,0x800EC4ACu,0x800EB8C0u},{0x802170C8u,0x58,0x800E91C4u,0x800E82A4u},{0x80216110u,0x6C,0x800E0A80u,0x800E01A4u},{0x80215F78u,0x60,0x800DEEC0u,0x800DE984u},{0x802159E0u,0x60,0x800DEEC0u,0x800DE984u}};
bool explosion_class_valid(const NativeExplosionClass& type){return mem_read32(type.table+0x24)==type.initialize && mem_read32(type.table+0x34)==type.update && mem_read32(type.table+0x44)==0x800CD740u && ssne_find_func(type.initialize) && ssne_find_func(type.update);}
uint32_t signature(const std::array<float,10>& values){uint32_t hash=2166136261u;for(auto value:values){const auto bits=f2u(value);for(unsigned byte=0;byte<4;++byte){hash^=(bits>>(byte*8))&255u;hash*=16777619u;}}return hash;}
void damage_hook(PPCContext& c){
    // 8007223C combines original model retirement/construction with one area
    // damage message. The replica reuses that lifecycle but drops exactly this
    // object's verified message; every other presentation dispatch still fails.
    if(presenting && energy_transition_object && !energy_transition_skipped &&
       c.lr==0x80072380u && c.r[31]==energy_transition_object &&
       readable(c.r[31],0x17C) && mem_read32(c.r[31]+0x48)==0x80200598u &&
       readable(c.r[4],0x28) && mem_read32(c.r[4])==2 &&
       mem_read32(c.r[4]+0xC)==mem_read32(c.r[31]+0xC8)){
        ++energy_transition_skipped;return;
    }
    if(!authoritative && replica_simulation() && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){const auto type=readable(c.r[4],4)?mem_read32(c.r[4]):~0u;log_message(std::string(type==2?"[coop-client-gameplay-damage-dispatch] caller=":"[coop-client-native-message-dispatch] caller=")+std::to_string(c.lr)+" type="+std::to_string(type));}
    if(presenting){++presentation_damage_calls;fatal("Replica presentation attempted native gameplay damage");}
    struct Restore {std::optional<RocketSource> previous;~Restore(){rocket_damage_trace=previous;}} restore{rocket_damage_trace};
    if(rocket_damage_trace && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){
        static unsigned probes=0;if(probes++<32)log_message("[coop-rocket-nested-dispatch] shot="+std::to_string(rocket_damage_trace->attack.shot)+" caller="+std::to_string(c.lr)+" type="+std::to_string(readable(c.r[4],4)?mem_read32(c.r[4]):~0u));
    }
    rocket_damage_trace.reset();
    if(authoritative && c.lr==0x80076620u && readable(c.r[31],0x164) && mem_read32(c.r[31]+0x48)==0x802022E0u && readable(c.r[4],0x28) && mem_read32(c.r[4])==2){
        std::lock_guard lock(effect_mutex);auto source=rocket_sources.find(c.r[31]);
        if(source!=rocket_sources.end() && mem_read32(c.r[4]+0xC)==source->second.attack.actor && mem_read32(c.r[31]+0xC8)==source->second.attack.actor){rocket_damage_trace=source->second;
            if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-rocket-damage-dispatch] projectile="+std::to_string(c.r[31])+" shot="+std::to_string(source->second.attack.shot)+" source="+std::to_string(source->second.attack.actor)+" slot="+std::to_string(source->second.attack.slot)+" generation="+std::to_string(source->second.identity.generation));}
    }
    ssne_call_original(c,0x8000DF58,original_damage);
}
std::array<uint32_t,3> bullet_counts(const PPCContext& c){return {mem_read32(c.r[13]-0x5FD0),mem_read32(c.r[13]-0x5F58),mem_read32(0x8028B8C8)};}
bool validate_bullet_lists(const PPCContext& c,std::string& error){
    struct List {uint32_t head,count,size,next,previous;};
    const List lists[]={{mem_read32(c.r[13]-0x5FD4),mem_read32(c.r[13]-0x5FD0),0x14,0xC,0x10},{mem_read32(c.r[13]-0x5F5C),mem_read32(c.r[13]-0x5F58),0x4C,0x44,0x48},{mem_read32(0x8028B8C4),mem_read32(0x8028B8C8),0x37C,0x374,0x378}};
    for(const auto& list:lists){std::set<uint32_t> seen;uint32_t previous=0,node=list.head;while(node){if(!readable(node,list.size) || !seen.insert(node).second || seen.size()>32768 || mem_read32(node+list.previous)!=previous){error="Invalid native bullet visual lifetime list";return false;}previous=node;node=mem_read32(node+list.next);}if(seen.size()!=list.count){error="Native bullet visual count differs from list";return false;}}
    return true;
}
#include "native_prop_fragments.inc"
uint32_t impact_signature(const NativeBulletImpact& event){uint32_t hash=2166136261u;auto word=[&](uint32_t value){for(unsigned byte=0;byte<4;++byte){hash^=(value>>(byte*8))&255u;hash*=16777619u;}};for(auto tag:event.tags)word(tag);for(auto value:event.vectors)word(f2u(value));return hash;}
uint32_t trail_signature(const NativeBulletTrail& event){uint32_t hash=2166136261u;auto word=[&](uint32_t value){for(unsigned byte=0;byte<4;++byte){hash^=(value>>(byte*8))&255u;hash*=16777619u;}};word(event.mask);for(auto value:event.vectors)word(f2u(value));for(auto value:event.view)word(f2u(value));return hash;}
void trail_hook(PPCContext& c){
    const auto caller=c.lr;
    // Verified player and enemy callers. Preserve only effects which the
    // original four-slot allocator actually accepted on the host.
    bool capture=authoritative && !presenting && (caller==0x8005650Cu || caller==0x8006143Cu || caller==0x80057AA8u || caller==0x80057AC4u || caller==0x80027010u || caller==0x8017B738u);
    NativeBulletTrail event;event.mask=c.r[5];
    capture=capture && readable(c.r[3],12) && readable(c.r[4],12);
    if(capture)for(unsigned slot=0;slot<native_party_capacity();++slot){
        if(!(event.mask&(1u<<slot)))continue;
        const auto actor=mem_read32(native_party_registry()+slot*4);if(!readable(actor,0x790))continue;
        const auto weapon=mem_read32(actor+0x5DC);if(weapon!=1 && weapon!=2 && weapon!=4)continue;
        for(unsigned hand=0;hand<2;++hand){const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
            if(!readable(instance,0x228) || c.r[3]!=instance+0x180)continue;
            if(const auto camera=native_weapon_camera_reference(instance)){event.view[0]=float(slot+1);std::copy(camera->begin(),camera->end(),event.view.begin()+1);}
        }
    }
    if(capture){for(unsigned point=0;point<2;++point)for(unsigned axis=0;axis<3;++axis)event.vectors[point*3+axis]=u2f(mem_read32(c.r[3+point]+axis*4));std::string why;capture=validate_native_bullet_trail(event,why);}
    const auto before=capture?mem_read32(c.r[13]-0x5490):0;
    ssne_call_original(c,0x800EE020,original_trail);
    if(capture && before<4 && mem_read32(c.r[13]-0x5490)==before+1){
        std::lock_guard lock(effect_mutex);event.sequence=++trail_sequence;retain_native_event(trails,event);
        if(getenv("SSNE_TEST_HIDDEN") && (getenv("SSNE_COOP_TRACE_LIFETIMES") || getenv("SSNE_DEV_CYCLE_LOOK")))log_message("[coop-bullet-trail-host] sequence="+std::to_string(event.sequence)+" payload="+std::to_string(trail_signature(event))+" mask="+std::to_string(event.mask)+" owner_view="+std::to_string(event.view[0]!=0)+" owner_slot="+std::to_string(int(event.view[0])-1)+" caller="+std::to_string(caller));
    }
}
bool update_replica_bullet_trails(PPCContext& context,std::string& error){
    // The stamp advances in the renderer (80117444), not at a network poll.
    // Multiple snapshots/ticks during one rendered frame must not remove a
    // just-created streak before the native renderer can submit its geometry.
    const auto frame=mem_read32(context.r[13]-0x6D80);
    if(replica_trail_frame && *replica_trail_frame==frame)return true;
    const auto count=mem_read32(context.r[13]-0x5490);
    if(count>4){error="Invalid native bullet trail slot count";return false;}
    for(unsigned i=0;i<4;++i){const auto proxy=mem_read32(0x802D90CCu+i*4);if(proxy && !readable(proxy,0x38)){error="Invalid native bullet trail proxy";return false;}}
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    PresentationScope presentation;ssne_call(call,0x800EDFBC);
    if(mem_read32(context.r[13]-0x5490)){error="Native bullet trail retirement failed";return false;}
    if(count && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-bullet-trail-retired] count="+std::to_string(count)+" frame="+std::to_string(frame)+" created_frame="+std::to_string(replica_trail_frame.value_or(frame)));
    replica_trail_frame=frame;
    replica_trail_views.clear();
    for(unsigned budget=0;budget<4 && !replica_trails.empty();++budget){
        const auto& event=replica_trails.front();
        for(unsigned i=0;i<6;++i)mem_write32(call.r[1]+0x80+i*4,f2u(event.vectors[i]));
        call.r[3]=call.r[1]+0x80;call.r[4]=call.r[1]+0x8C;call.r[5]=event.mask;
        const auto before=mem_read32(context.r[13]-0x5490),geometry=mem_read32(0x802D90BCu+before*4);
        if(before>=4 || !readable(geometry,4)){error="Native bullet trail geometry unavailable";return false;}
        ssne_call(call,0x800EE020);
        if(mem_read32(context.r[13]-0x5490)!=before+1 || !readable(mem_read32(0x802D90CCu+before*4),0x38)){error="Native bullet trail was not created";return false;}
        if(event.view[0]!=0){const auto proxy=mem_read32(0x802D90CCu+before*4),matrix=mem_read32(proxy+4);if(!readable(matrix,48)){error="Native first-person trail matrix unavailable";return false;}
            TrailView view{matrix};view.owner=unsigned(event.view[0])-1;std::copy_n(event.view.begin()+1,12,view.camera.begin());for(unsigned i=0;i<12;++i)view.root[i]=mem_read32(matrix+i*4);replica_trail_views[proxy]=view;}
        if(getenv("SSNE_TEST_HIDDEN") && (getenv("SSNE_COOP_TRACE_LIFETIMES") || getenv("SSNE_DEV_CYCLE_LOOK")))log_message("[coop-bullet-trail-client] sequence="+std::to_string(event.sequence)+" payload="+std::to_string(trail_signature(event))+" mask="+std::to_string(event.mask)+" owner_view="+std::to_string(event.view[0]!=0)+" owner_slot="+std::to_string(int(event.view[0])-1)+" frame="+std::to_string(frame)+" active="+std::to_string(before+1)+" pending="+std::to_string(replica_trails.size()-1)+" damage_calls="+std::to_string(presentation_damage_calls));
        replica_trails.pop_front();
    }
    return true;
}
void voice_probe(PPCContext& c){const bool trace=presenting && c.lr==0x8010EB80u;const auto manager=c.r[3],wave=c.r[4];ssne_call_original(c,0x80157840,original_voice);if(trace && int32_t(c.r[3])<0){unsigned free=0;for(unsigned i=0;i<48;++i)free+=mem_read32(0x802D69C8u+i*192)==0;unsigned loaded=0;if(readable(manager,0x60) && wave<65536){const auto table=mem_read32(manager+0x58);if(readable(table+wave*16,16))loaded=mem_read8(table+wave*16);}log_message("[coop-native-voice-rejected] wave="+std::to_string(wave)+" free="+std::to_string(free)+" loaded="+std::to_string(loaded)+" resource_count="+std::to_string(wave<65536?mem_read8(0x80292620u+wave):0)+" gate="+std::to_string(mem_read32(c.r[13]-0x5BB0)));}}
void subtract_probe(PPCContext& c){const auto victim=c.r[3],payload=c.r[4],caller=c.lr;const bool trace=authoritative && !rocket_damage_trace && attack_trace.actor && caller==0x80093C2Cu && readable(victim,0xD4) && readable(payload,0x10) && mem_read32(payload+8)==attack_trace.actor;const auto before=trace?u2f(mem_read32(victim+0xCC)):0.f;const auto amount=float(c.ps0[1]);const auto actor=attack_trace;const auto table=trace?mem_read32(victim+0x48):0;std::function<NativeHitIdentity(uint32_t)> resolver;if(trace){std::lock_guard lock(effect_mutex);resolver=hit_resolver;}const auto source_id=resolver?resolver(actor.actor):NativeHitIdentity{},target_id=resolver?resolver(victim):NativeHitIdentity{};ssne_call_original(c,0x8009D014,original_subtract);if(trace && readable(victim,0xD4) && mem_read32(victim+0x48)==table){const auto after=u2f(mem_read32(victim+0xCC));if(std::isfinite(before) && std::isfinite(after) && std::isfinite(amount) && amount>0 && after<before){if(source_id.entity && source_id.generation && target_id.entity && actor.shot){NativeConfirmedHit hit;hit.shot=actor.shot;hit.source=source_id.entity;hit.target=target_id.entity;hit.generation=source_id.generation;hit.slot=actor.slot;hit.weapon=actor.weapon;hit.pellet=actor.pellet;hit.before=before;hit.after=after;hit.amount=amount;std::lock_guard lock(effect_mutex);hit.sequence=hit_sequence+1;std::string why;if(!validate_native_confirmed_hit(hit,why))fatal(why.c_str());hit_sequence=hit.sequence;retain_native_event(confirmed_hits,hit);log_message("[coop-confirmed-hit-host] sequence="+std::to_string(hit.sequence)+" shot="+std::to_string(hit.shot)+" source="+std::to_string(hit.source)+" target="+std::to_string(hit.target)+" slot="+std::to_string(hit.slot)+" generation="+std::to_string(hit.generation)+" before="+std::to_string(hit.before)+" after="+std::to_string(hit.after)+" amount="+std::to_string(hit.amount)+" weapon="+std::to_string(hit.weapon)+" pellet="+std::to_string(hit.pellet));}log_message("[coop-host-attributed-hit] slot="+std::to_string(actor.slot)+" weapon="+std::to_string(actor.weapon)+" source="+std::to_string(actor.actor)+" shot="+std::to_string(actor.shot)+" victim="+std::to_string(victim)+" before="+std::to_string(before)+" after="+std::to_string(after)+" amount="+std::to_string(amount));}}}
void subtract_diagnostic(PPCContext& c) {
    const auto victim=c.r[3],payload=c.r[4],caller=c.lr;
    const auto provenance=rocket_damage_trace;
    if(authoritative && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){
        static unsigned probes=0;if(probes++<32)log_message("[coop-native-subtract-entry] caller="+std::to_string(caller)+" victim="+std::to_string(victim)+" rocket_scope="+std::to_string(bool(provenance))+" amount="+std::to_string(float(c.ps0[1])));
    }
    if(provenance && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){
        static unsigned probes=0;if(probes++<32)log_message("[coop-rocket-subtract-probe] caller="+std::to_string(caller)+" victim="+std::to_string(victim)+" payload_source="+std::to_string(readable(payload,12)?mem_read32(payload+8):0)+" source="+std::to_string(provenance->attack.actor));
    }
    const bool trace=authoritative && provenance && caller==0x80093C2Cu && readable(victim,0xD4) && readable(payload,0x10) && mem_read32(payload+8)==provenance->attack.actor;
    const auto before=trace?u2f(mem_read32(victim+0xCC)):0.f;
    const auto amount=float(c.ps0[1]);const auto table=trace?mem_read32(victim+0x48):0;
    std::function<NativeHitIdentity(uint32_t)> resolver;
    if(trace){std::lock_guard lock(effect_mutex);resolver=hit_resolver;}
    const auto target=resolver?resolver(victim):NativeHitIdentity{};
    subtract_probe(c);
    if(trace && readable(victim,0xD4) && mem_read32(victim+0x48)==table){const auto after=u2f(mem_read32(victim+0xCC));
        if(target.entity && target.entity!=provenance->identity.entity && std::isfinite(before) && std::isfinite(after) && std::isfinite(amount) && amount>0 && after<before){
            NativeConfirmedHit hit;hit.shot=provenance->attack.shot;hit.source=provenance->identity.entity;hit.target=target.entity;
            hit.generation=provenance->identity.generation;hit.slot=provenance->attack.slot;hit.weapon=6;hit.before=before;hit.after=after;hit.amount=amount;
            {std::lock_guard lock(effect_mutex);hit.sequence=hit_sequence+1;std::string why;if(!validate_native_confirmed_hit(hit,why))fatal(why.c_str());
                hit_sequence=hit.sequence;retain_native_event(confirmed_hits,hit);}
            log_message("[coop-confirmed-hit-host] sequence="+std::to_string(hit.sequence)+" shot="+std::to_string(hit.shot)+" source="+std::to_string(hit.source)+" target="+std::to_string(hit.target)+" slot="+std::to_string(hit.slot)+" generation="+std::to_string(hit.generation)+" before="+std::to_string(hit.before)+" after="+std::to_string(hit.after)+" amount="+std::to_string(hit.amount)+" weapon=6 pellet=0");
            log_message("[coop-host-attributed-hit] slot="+std::to_string(hit.slot)+" weapon=6 source="+std::to_string(provenance->attack.actor)+" shot="+std::to_string(hit.shot)+" victim="+std::to_string(victim)+" before="+std::to_string(before)+" after="+std::to_string(after)+" amount="+std::to_string(amount));
        }
        if(std::isfinite(before) && std::isfinite(after) && std::isfinite(amount) && amount>0 && after<before && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))
            log_message("[coop-rocket-damage-observed] shot="+std::to_string(provenance->attack.shot)+" source="+std::to_string(provenance->attack.actor)+" stable_source="+std::to_string(provenance->identity.entity)+" slot="+std::to_string(provenance->attack.slot)+" generation="+std::to_string(provenance->identity.generation)+" victim="+std::to_string(victim)+" target="+std::to_string(target.entity)+" before="+std::to_string(before)+" after="+std::to_string(after)+" amount="+std::to_string(amount));
    }
}
void area_receiver_diagnostic(PPCContext& c) {
    const auto victim=c.r[3],payload=c.r[4];
    struct Restore {std::optional<RocketSource> previous;~Restore(){area_damage_trace=previous;}} restore{area_damage_trace};
    area_damage_trace=rocket_damage_trace;
    if(rocket_damage_trace && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES") && readable(victim,0x178) && readable(payload,0x30)){
        static unsigned probes=0;if(probes++<48)log_message("[coop-rocket-area-receiver] shot="+std::to_string(rocket_damage_trace->attack.shot)+" victim="+std::to_string(victim)+" table="+std::to_string(mem_read32(victim+0x48))+" flags="+std::to_string(mem_read32(victim+0x1C))+" filter="+std::to_string(mem_read32(payload+0x2C))+" amount="+std::to_string(u2f(mem_read32(payload+0x10)))+" radius="+std::to_string(u2f(mem_read32(payload+0x14))));
    }
    ssne_call_original(c,0x80094C84,original_area_receiver);
}
void area_queue_hook(PPCContext& c) {
    const auto manager=c.r[3],victim=c.r[4],payload=c.r[5],caller=c.lr;
    std::optional<QueuedRocket> captured;
    if(authoritative && area_damage_trace && caller==0x80094D28u && readable(payload,0x30) && mem_read32(payload)==2 && mem_read32(payload+12)==area_damage_trace->attack.actor){
        std::function<NativeHitIdentity(uint32_t)> resolver;{std::lock_guard lock(effect_mutex);resolver=hit_resolver;}
        const auto target=resolver?resolver(victim):NativeHitIdentity{};
        if(target.entity)captured=QueuedRocket{*area_damage_trace,victim,target.entity,queued_words(payload)};
    }
    ssne_call_original(c,0x8008C4CC,original_area_queue);
    if(authoritative && c.r[3]==1 && readable(manager,0x5C)){
        const auto node=mem_read32(manager+0x54);
        if(readable(node,0x44) && mem_read32(node+0x30)==victim){std::lock_guard lock(effect_mutex);queued_rockets.erase(node);
            if(captured && captured->words==queued_words(node)){queued_rockets[node]=*captured;
                if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-rocket-damage-queued] node="+std::to_string(node)+" victim="+std::to_string(victim)+" target="+std::to_string(captured->target)+" shot="+std::to_string(captured->source.attack.shot));}
        }
    }
}
void common_damage_hook(PPCContext& c) {
    if(authoritative && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){
        static unsigned probes=0;if(probes++<24)log_message("[coop-area-common-entry] caller="+std::to_string(c.lr)+" area_scope="+std::to_string(bool(area_damage_trace))+" source="+std::to_string(readable(c.r[4],12)?mem_read32(c.r[4]+8):0)+" flags="+std::to_string(readable(c.r[4],8)?mem_read32(c.r[4]+4):0));
    }
    struct Restore {std::optional<RocketSource> previous;~Restore(){rocket_damage_trace=previous;}} restore{rocket_damage_trace};
    rocket_damage_trace.reset();
    // Bind only the actual damage payload constructed by the original area
    // receiver. Unrelated nested messages/interrupts must not inherit a shot.
    if(authoritative && area_damage_trace && c.lr==0x80094DB8u && readable(c.r[4],0x10) &&
       mem_read32(c.r[4]+8)==area_damage_trace->attack.actor && mem_read32(c.r[4]+4)==0x400000u)
        rocket_damage_trace=area_damage_trace;
    if(authoritative && c.lr==0x8008C818u && readable(c.r[31],0x44) && readable(c.r[4],0x10)){
        std::optional<QueuedRocket> queued;std::function<NativeHitIdentity(uint32_t)> resolver;
        {std::lock_guard lock(effect_mutex);resolver=hit_resolver;auto found=queued_rockets.find(c.r[31]);if(found!=queued_rockets.end()){queued=found->second;queued_rockets.erase(found);}}
        const auto target=resolver?resolver(c.r[3]):NativeHitIdentity{};
        if(queued && queued->victim==c.r[3] && queued->target==target.entity && queued->words==queued_words(c.r[31]) &&
           mem_read32(c.r[4]+8)==queued->source.attack.actor && mem_read32(c.r[4]+4)==0x400000u){rocket_damage_trace=queued->source;
            if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-rocket-queued-consumed] node="+std::to_string(c.r[31])+" target="+std::to_string(target.entity)+" shot="+std::to_string(queued->source.attack.shot));}
    }
    ssne_call_original(c,0x80093AFC,original_common_damage);
}
void query_hook(PPCContext& c) {
    const auto caller=c.lr,stack=c.r[1];
    struct RestoreAttack {AttackTrace previous;~RestoreAttack(){attack_trace=previous;}} restore{attack_trace};
    attack_trace={};
    const bool pistol=caller==0x800564FCu,shotgun=caller==0x8005EEBCu,uzi=caller==0x8006142Cu,minigun=caller==0x80057A78u;
    const auto instance=c.r[shotgun?28:31];
    const unsigned weapon=minigun?4:uzi?2:shotgun?3:1,pellet=shotgun?c.r[23]:0;
    // Original shotgun 8005E9D0 performs fourteen separate queries, r23=0..13.
    // Several pellets can legitimately damage the same target in one shot.
    if(authoritative && (pistol || shotgun || uzi || minigun) && pellet<=13 && c.r[3]==instance &&
       readable(instance,0x1D8) && mem_read32(instance+0xC)==(minigun?0x801FB598u:uzi?0x801FC858u:shotgun?0x801FC2D0u:0x801FB280u)) {
        const auto actor=mem_read32(instance+0x1D4);
        if(readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u) {
            const auto slot=mem_read32(actor+0x17C);
            const bool bound=mem_read32(actor+0x4BC+weapon*24)==instance || mem_read32(actor+0x4C0+weapon*24)==instance;
            if(bound && slot<native_party_capacity() && mem_read32(native_party_registry()+slot*4)==actor) {
                std::lock_guard lock(effect_mutex);
                if(uzi || minigun) {
                    const unsigned lane=mem_read32(actor+0x4BC+weapon*24)==instance?0:1;
                    if(automatic_ray_sequence>=native_automatic_ray_tag/2-1)fatal("Native automatic ray identity exhausted");
                    const auto ray=native_automatic_ray_tag|(++automatic_ray_sequence<<1)|lane;
                    attack_trace={actor,slot,weapon,0,ray};
                    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))
                        log_message(std::string(minigun?"[coop-minigun-ray-host] shot=":"[coop-uzi-ray-host] shot=")+std::to_string(ray)+" slot="+std::to_string(slot)+" lane="+std::to_string(lane)+" instance="+std::to_string(instance)+" source="+std::to_string(actor)+" caller="+std::to_string(caller));
                } else if(auto shot=weapon_shot_ids.find(instance);shot!=weapon_shot_ids.end())
                    attack_trace={actor,slot,weapon,pellet,shot->second};
            }
        }
    }
    const bool trace=authoritative && (pistol || shotgun || uzi || minigun) && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES") && query_probes<64;
    const auto attack=attack_trace;
    ssne_call_original(c,0x8009FDA0,original_query);
    if(trace && readable(stack+0x30,0x48)) {
        ++query_probes;
        log_message("[coop-bullet-query] result="+std::to_string(mem_read32(stack+0x40))+" kind="+std::to_string(mem_read32(stack+0x54))+" x="+std::to_string(u2f(mem_read32(stack+0x30)))+" y="+std::to_string(u2f(mem_read32(stack+0x34)))+" z="+std::to_string(u2f(mem_read32(stack+0x38)))+" weapon="+std::to_string(weapon)+" pellet="+std::to_string(pellet)+" shot="+std::to_string(attack.shot)+" source="+std::to_string(attack.actor));
    }
}
void impact_hook(PPCContext& c){if(authoritative && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES") && impact_probes++<32)log_message("[coop-bullet-impact-entry] caller="+std::to_string(c.lr)+" kind="+std::to_string(c.r[3])+" ammo="+std::to_string(c.r[4])+" material="+std::to_string(c.r[10]));NativeBulletImpact event;bool capture=authoritative && (c.lr==0x800565B4u || c.lr==0x80061488u || c.lr==0x8005EF5Cu || c.lr==0x80057B08u || c.lr==0x80060620u || (c.lr==0x800539A8u && readable(c.r[31],0x1D8) && mem_read32(c.r[31]+0xC)==0x801FAB40u) || (c.lr==0x8006F2B4u && readable(c.r[31],0x140) && mem_read32(c.r[31]+0x48)==0x801FF860u));if(capture){event.tags={c.r[3],c.r[4],unsigned(c.r[8]!=0),unsigned(c.r[9]!=0),c.r[10]};for(unsigned vector=0;vector<3;++vector){capture&=readable(c.r[5+vector],12);if(capture)for(unsigned axis=0;axis<3;++axis)event.vectors[vector*3+axis]=u2f(mem_read32(c.r[5+vector]+axis*4));}std::string why;capture=capture && validate_native_bullet_impact(event,why);if(!capture)log_message("[coop-bullet-impact-rejected] "+why+" kind="+std::to_string(c.r[3])+" ammo="+std::to_string(c.r[4])+" material="+std::to_string(c.r[10]));}ssne_call_original(c,0x800ED8E8,original_impact);if(capture){std::lock_guard lock(effect_mutex);event.sequence=++impact_sequence;retain_native_event(impacts,event);log_message("[coop-bullet-impact-host] sequence="+std::to_string(event.sequence)+" payload="+std::to_string(impact_signature(event))+" kind="+std::to_string(event.tags[0])+" ammo="+std::to_string(event.tags[1])+" material="+std::to_string(event.tags[4]));}}
void track_shot_voice(uint32_t handle,uint64_t event) {
    const auto index=handle&63u;if(index>=48)return;const auto record=0x802D69C8u+index*192u;const auto voice=mem_read32(record);
    if(mem_read32(record+0xAC)!=handle || !readable(voice,0x148))return;const auto slot=mem_read32(voice+0x18);if(slot<64)audio_track_weapon_voice(0x802C18A0u+slot*236,record,handle,event);
}
void loop_start_hook(PPCContext& c) {
    NativeWeaponLoop event;const bool uzi=(c.lr==0x80061378u || c.lr==0x800616D8u) && c.r[3]==419 && c.r[4]==0;const bool minigun=(c.lr==0x800578D0u || c.lr==0x80057E78u) && c.r[3]==406 && c.r[4]==1;event.weapon=minigun?4:2;bool capture=authoritative && (uzi || minigun) && readable(c.r[31],minigun?0x294:0x248) && mem_read32(c.r[31]+0xC)==(minigun?0x801FB598u:0x801FC858u);
    if(capture){const auto actor=mem_read32(c.r[31]+0x1D4);capture=readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u;if(capture){event.slot=mem_read32(actor+0x17C);event.active=true;event.lane=mem_read32(actor+0x4BC+event.weapon*24)==c.r[31]?0:1;capture=mem_read32(actor+0x4BC+event.weapon*24+event.lane*4)==c.r[31];for(unsigned i=0;i<3;++i)event.position[i]=u2f(mem_read32(actor+0x20+i*4));capture=capture && event.slot<8 && event.lane<2 && std::all_of(event.position.begin(),event.position.end(),[](float value){return std::isfinite(value) && std::abs(value)<=1000000.f;});}}
    PartySoundGate gate(c,capture && minigun);ssne_call_original(c,0x8010EB9C,original_loop_start);gate.restore();
    if(capture && int32_t(c.r[3])>=0){std::lock_guard lock(effect_mutex);event.sequence=loop_sequence+1;event.origin=event.sequence;std::string why;if(!validate_native_weapon_loop(event,why)){log_message("[coop-weapon-loop-rejected] "+why);return;}loop_sequence=event.sequence;event.order=++audio_order;host_loops[c.r[3]]={event,std::chrono::steady_clock::now()};retain_native_event(loop_history,event);log_message(std::string(event.weapon==2?"[coop-uzi-loop-host] sequence=":"[coop-minigun-loop-host] sequence=")+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" lane="+std::to_string(event.lane)+" active=1 duration_ms=0 origin="+std::to_string(event.origin)+" order="+std::to_string(event.order));}
}
void publish_loop_stop(const HostLoop& previous) {
    auto event=previous.event;event.active=false;event.duration_ms=uint32_t(std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-previous.began).count(),0,60000));
    {std::lock_guard lock(effect_mutex);event.sequence=++loop_sequence;event.order=++audio_order;retain_native_event(loop_history,event);}
    log_message(std::string(event.weapon==2?"[coop-uzi-loop-host] sequence=":"[coop-minigun-loop-host] sequence=")+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" lane="+std::to_string(event.lane)+" active=0 duration_ms="+std::to_string(event.duration_ms)+" origin="+std::to_string(event.origin)+" order="+std::to_string(event.order));
}
bool weapon_loop_handle_alive(uint32_t handle) {
    const auto index=handle&63u;if(int32_t(handle)<0 || index>=48)return false;
    const auto record=0x802D69C8u+index*192u;
    return mem_read32(record+0xAC)==handle && readable(mem_read32(record),0x148);
}
void loop_stop_hook(PPCContext& c) {
    const auto combo_slot=combo_feedback_slot(c.r[31]);
    if(authoritative && !presenting && combo_slot && c.lr>=0x800B5B74u && c.lr<=0x800B617Cu && c.r[3]==mem_read32(c.r[31]+0x43C)){
        NativeFeedbackEvent e;e.kind=8;e.variant=*combo_slot;std::lock_guard lock(effect_mutex);e.sequence=++feedback_sequence;retain_native_event(feedback_events,e);
    }
    const auto handle=c.r[3];std::optional<HostLoop> previous;
    if(authoritative)host_cue_voices.erase(handle);
    if(authoritative){std::lock_guard lock(effect_mutex);if(auto found=host_loops.find(handle);found!=host_loops.end()){previous=found->second;host_loops.erase(found);}}
    ssne_call_original(c,0x8010EDFC,original_loop_stop);
    if(previous)publish_loop_stop(*previous);
}
#include "native_weapon_audio_calls.inc"
#include "native_player_notices.inc"
void shot_hook(PPCContext& c) {
    NativeFeedbackEvent combo;combo.kind=8;combo.code=c.r[3];combo.values[0]=float(c.r[4]);const auto combo_slot=combo_feedback_slot(c.r[31]);
    const bool capture_combo=authoritative && !presenting && combo_slot &&
        ((c.lr==0x800B5F78u && combo.code==0x180) || (c.lr==0x800B5FE4u && combo.code==0x181) || (c.lr==0x800B6030u && combo.code==0x182) || (c.lr==0x800B6158u && combo.code==0x183));
    if(capture_combo)combo.variant=*combo_slot;
    NativeFeedbackEvent pickup;pickup.kind=5;pickup.code=c.r[3];
    const bool pickup_call=c.lr==0x800C17C4u || c.lr==0x800C1F88u || c.lr==0x800C1FA0u;
    const auto pickup_actor=c.r[c.lr==0x800C17C4u?31:30];
    bool capture_pickup=authoritative && !presenting && pickup_call && c.r[4]==0 && readable(pickup_actor,0x790) && mem_read32(pickup_actor+0x48)==0x802121E8u;
    if(capture_pickup){pickup.variant=mem_read32(pickup_actor+0x17C);for(unsigned i=0;i<3;++i)pickup.values[i]=u2f(mem_read32(pickup_actor+0x20+i*4));std::string why;capture_pickup=validate_native_feedback(pickup,why);}
    NativeFeedbackEvent step;step.kind=10;step.code=c.r[3];
    const auto step_owner=authoritative && !presenting && (c.lr==0x800B7CCCu || c.lr==0x800B7D2Cu)?native_notice_owner(step_controller,0x174):std::nullopt;
    bool capture_step=step_owner && c.r[4]==0;
    if(capture_step){step.variant=*step_owner;const auto actor=mem_read32(native_party_registry()+*step_owner*4);for(unsigned i=0;i<3;++i)step.values[i]=u2f(mem_read32(actor+0x20+i*4));std::string why;capture_step=validate_native_feedback(step,why);}
    NativeWeaponShot shot;uint32_t table=0;bool known=false;unsigned instance_register=31;
    if((c.lr==0x8005CE90u || c.lr==0x8005D0ECu || c.lr==0x8005D33Cu) && c.r[3]==415 && c.r[4]==1){shot.weapon=6;table=0x801FBB38u;known=true;}
    else if(c.lr==0x8005643Cu && c.r[3]==417 && c.r[4]==1){shot.weapon=1;table=0x801FB280u;known=true;}
    else if(c.lr==0x8005EC44u && c.r[3]==414 && c.r[4]==1){shot.weapon=3;table=0x801FC2D0u;known=true;}
    else if((c.lr==0x800557D8u || c.lr==0x80055A14u || c.lr==0x80055C54u) && c.r[3]==405 && c.r[4]==0){shot.weapon=5;table=0x801FAFF0u;known=true;}
    else if(c.r[4]==1 && ((c.r[3]==409 && (c.lr==0x80057784u || c.lr==0x80057D2Cu)) || (c.r[3]==407 && (c.lr==0x800577F0u || c.lr==0x80057960u || c.lr==0x80057D98u || c.lr==0x80057F08u)) || (c.r[3]==408 && (c.lr==0x800578A0u || c.lr==0x80057E48u)))){shot.weapon=4;shot.cue_sound=c.r[3];table=0x801FB598u;known=true;}
    instance_register=shot.weapon==3?28:31;
    if(!known)for(const auto& cue:weapon_audio_calls)if(cue.caller==c.lr && cue.sound==c.r[3] && cue.flags==c.r[4]){
        shot.weapon=cue.weapon;shot.cue_sound=cue.sound;shot.cue_flags=cue.flags;instance_register=cue.instance_register;known=true;break;
    }
    const auto instance=c.r[instance_register];
    bool capture=authoritative && known && readable(instance,0x1D8) && (!table || mem_read32(instance+0xC)==table);
    if(capture){const auto actor=mem_read32(instance+0x1D4);capture=readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u;if(capture){capture=mem_read32(actor+0x4BC+shot.weapon*24)==instance || mem_read32(actor+0x4C0+shot.weapon*24)==instance;shot.slot=mem_read32(actor+0x17C);for(unsigned i=0;i<3;++i)shot.position[i]=u2f(mem_read32(actor+0x20+i*4));std::string why;capture=capture && validate_native_weapon_shot(shot,why);}}
    PartySoundGate gate(c,capture && shot.weapon==4);ssne_call_original(c,0x8010E7B0,original_shot);gate.restore();
    if(capture_combo && int32_t(c.r[3])>=0){std::lock_guard lock(effect_mutex);combo.sequence=++feedback_sequence;retain_native_event(feedback_events,combo);}
    if(capture_pickup){std::lock_guard lock(effect_mutex);pickup.sequence=++feedback_sequence;retain_native_event(feedback_events,pickup);log_message("[coop-pickup-sound-host] sequence="+std::to_string(pickup.sequence)+" slot="+std::to_string(pickup.variant)+" code="+std::to_string(pickup.code));}
    if(capture_step){std::lock_guard lock(effect_mutex);step.sequence=++feedback_sequence;retain_native_event(feedback_events,step);}
    if(capture){std::lock_guard lock(effect_mutex);shot.sequence=++shot_sequence;shot.order=++audio_order;if(shot.cue_sound && int32_t(c.r[3])>=0)host_cue_voices[c.r[3]]=shot.sequence;if(!shot.cue_sound && (shot.weapon==1 || shot.weapon==3 || shot.weapon==6))weapon_shot_ids[instance]=shot.sequence;retain_native_event(shots,shot);track_shot_voice(c.r[3],shot.sequence);log_message(std::string(shot.cue_sound?"[coop-weapon-cue-host] sequence=":"[coop-weapon-shot-host] sequence=")+std::to_string(shot.sequence)+" slot="+std::to_string(shot.slot)+" voice="+std::to_string(int32_t(c.r[3]))+" x="+std::to_string(shot.position[0])+" y="+std::to_string(shot.position[1])+" z="+std::to_string(shot.position[2])+" weapon="+std::to_string(shot.weapon)+" cue_sound="+std::to_string(shot.cue_sound)+" order="+std::to_string(shot.order));}
}
void feedback_sound_hook(PPCContext& c) {
    if(replica_simulation() && !presenting && mem_read32(0x802DB1BC)==1){c.r[3]=UINT32_MAX;return;}
    NativeFeedbackEvent event;event.code=c.r[3];
    bool capture=authoritative && !presenting && readable(c.r[4],12);
    if(capture)for(unsigned i=0;i<3;++i)event.values[i]=u2f(mem_read32(c.r[4]+i*4));
    std::string why;capture=capture && validate_native_feedback(event,why);
    ssne_call_original(c,0x8010E86C,original_feedback_sound);
    if(capture && int32_t(c.r[3])>=0){
        // 8010E86C selects a sample from a group before allocating its voice.
        // Transmit that selection, rather than running a second random draw on
        // the replica. The generation-bearing handle must still own this slot.
        const auto handle=c.r[3],voice=0x802D69C8u+(handle&63)*192;
        const auto group=0x8023AD70u+event.code*24,count=mem_read32(group+8),samples=mem_read32(group+12);
        if((handle&63)<48 && mem_read32(voice) && mem_read32(voice+0xAC)==handle && count && count<=256 && readable(samples,count*4))
            for(unsigned i=0;i<count;++i)if(mem_read32(samples+i*4)==mem_read32(voice+0xB0)){event.variant=i+1;break;}
        if(!event.variant){log_message("[coop-feedback-selection-rejected] code="+std::to_string(event.code)+" voice="+std::to_string(handle));return;}
        std::lock_guard lock(effect_mutex);event.sequence=++feedback_sequence;retain_native_event(feedback_events,event);log_message("[coop-feedback-host] sequence="+std::to_string(event.sequence)+" kind=0 code="+std::to_string(event.code)+" variant="+std::to_string(event.variant)+" voice="+std::to_string(handle));
    }
}
void feedback_score_hook(PPCContext& c) {
    NativeFeedbackEvent event;event.kind=1;event.code=c.r[4];event.variant=c.r[5];
    bool capture=authoritative && !presenting && readable(c.r[6],12) && readable(c.r[7],12);
    if(capture){for(unsigned i=0;i<3;++i){event.values[i]=u2f(mem_read32(c.r[6]+i*4));event.values[i+3]=u2f(mem_read32(c.r[7]+i*4));}event.values[6]=float(c.ps0[1]);event.values[7]=float(c.ps0[2]);}
    std::string why;capture=capture && validate_native_feedback(event,why);
    if(authoritative && !presenting && !capture)log_message("[coop-feedback-score-rejected] "+why+" points="+std::to_string(event.code)+" variant="+std::to_string(event.variant)+" scale="+std::to_string(event.values[6])+" height="+std::to_string(event.values[7]));
    ssne_call_original(c,0x800DB980,original_feedback_score);
    if(capture){std::lock_guard lock(effect_mutex);event.sequence=++feedback_sequence;retain_native_event(feedback_events,event);log_message("[coop-feedback-host] sequence="+std::to_string(event.sequence)+" kind=1 code="+std::to_string(event.code));}
}
void effect_hook(PPCContext& c) {
    // The same original factory also serves Kamikaze, enemy and scenery
    // explosions. Capture its validated visual ABI rather than only callers
    // belonging to player rockets/grenades.
    NativeRocketEffect effect;effect.style=c.r[6];bool capture=authoritative && !presenting && c.r[6]<=4;
    if(capture) {
        for(unsigned vector=0;vector<3;++vector){capture&=readable(c.r[3+vector],12);if(capture)for(unsigned axis=0;axis<3;++axis)effect.values[vector*3+axis]=u2f(mem_read32(c.r[3+vector]+axis*4));}
        effect.values[9]=float(c.ps0[1]);std::string why;capture&=validate_native_rocket_effect(effect,why);
    }
    const auto projectile_kind=(c.lr==0x80074B6Cu || c.lr==0x80074BECu)?2u:(c.lr==0x8006EEF0u || c.lr==0x8006EF70u)?1u:c.lr==0x8006DB80u?3u:c.lr==0x80075878u?5u:c.lr==0x80070D8Cu?4u:c.lr==0x80065B28u?12u:0u;
    ssne_call_original(c,0x800D653C,original);
    if(capture && c.r[3]) {
        std::lock_guard lock(effect_mutex);effect.sequence=++sequence;retain_native_event(history,effect);
        log_message("[coop-rocket-effect-host] sequence="+std::to_string(effect.sequence)+" x="+std::to_string(effect.values[0])+" y="+std::to_string(effect.values[1])+" z="+std::to_string(effect.values[2])+" payload="+std::to_string(signature(effect.values))+" projectile="+std::to_string(projectile_kind)+" style="+std::to_string(effect.style));
    }
}
}
NativePickupNotice native_pickup_notice(uint32_t actor){
    if(!readable(actor,0x790))return {};const auto slot=mem_read32(actor+0x17C);if(slot>=8 || pickup_notices[slot].actor!=actor)return {};
    auto notice=pickup_notices[slot].notice;const auto hud=mem_read32(actor+0x374),popup=readable(hud,0x38)?mem_read32(hud+0x34):0,widget=readable(popup,0x10)?mem_read32(popup+0xC):0;
    // 800AF24C returns the scrolling text's active word at +0. It is not
    // the GUI component pointer (which lives at +0x30 in this class).
    if(!readable(widget,0xC8) || !mem_read32(widget))notice.text.clear();return notice;
}
void set_native_hit_identity_resolver(std::function<NativeHitIdentity(uint32_t)> resolver){std::lock_guard lock(effect_mutex);hit_resolver=std::move(resolver);}
void rebind_native_effect_sources(const std::array<uint64_t,8>& saved,const std::array<uint64_t,8>& live){
    std::lock_guard lock(effect_mutex);
    auto rebind=[&](RocketSource& source){const auto slot=source.attack.slot;
        if(slot>=8 || !live[slot] || source.identity.generation!=saved[slot])return false;
        source.identity.generation=live[slot];return true;};
    for(auto it=rocket_sources.begin();it!=rocket_sources.end();)if(rebind(it->second))++it;else it=rocket_sources.erase(it);
    for(auto it=queued_rockets.begin();it!=queued_rockets.end();)if(rebind(it->second.source))++it;else it=queued_rockets.erase(it);
}
std::vector<NativeConfirmedHit> native_confirmed_hit_history(){std::lock_guard lock(effect_mutex);return {confirmed_hits.begin(),confirmed_hits.end()};}
NativeEventProducerSnapshot native_event_producer_snapshot(){
    std::lock_guard lock(effect_mutex);NativeEventProducerSnapshot snapshot;snapshot.epoch=producer_epoch;
    snapshot.cursors={hit_sequence,sequence,shot_sequence,loop_sequence,impact_sequence,feedback_sequence,trail_sequence,emitter_sequence,casing_sequence,surface_sequence,beam_sequence};
    auto copy=[](const auto& queue,auto& output,uint64_t cursor){const auto first=std::find_if(queue.begin(),queue.end(),[&](const auto& event){return event.sequence>cursor;});output.assign(first,queue.end());};
    copy(confirmed_hits,snapshot.hits,producer_accepted[0]);copy(history,snapshot.rockets,producer_accepted[1]);
    copy(shots,snapshot.shots,producer_accepted[2]);copy(loop_history,snapshot.loops,producer_accepted[3]);copy(impacts,snapshot.impacts,producer_accepted[4]);copy(feedback_events,snapshot.feedback,producer_accepted[5]);copy(trails,snapshot.trails,producer_accepted[6]);copy(emitters,snapshot.emitters,producer_accepted[7]);copy(casings,snapshot.casings,producer_accepted[8]);copy(surfaces,snapshot.surfaces,producer_accepted[9]);copy(beams,snapshot.beams,producer_accepted[10]);return snapshot;
}
bool acknowledge_native_event_publication(uint64_t epoch,const NativeProducerCursors& accepted){
    std::lock_guard lock(effect_mutex);if(epoch!=producer_epoch)return false;
    const NativeProducerCursors tails={hit_sequence,sequence,shot_sequence,loop_sequence,impact_sequence,feedback_sequence,trail_sequence,emitter_sequence,casing_sequence,surface_sequence,beam_sequence};
    for(unsigned i=0;i<tails.size();++i)if(accepted[i]>tails[i])return false;
    for(unsigned i=0;i<tails.size();++i)producer_accepted[i]=std::max(producer_accepted[i],accepted[i]);
    auto prune=[](auto& queue,uint64_t cursor){while(queue.size()>128 && queue.front().sequence<=cursor)queue.pop_front();};
    prune(confirmed_hits,accepted[0]);prune(history,accepted[1]);prune(shots,accepted[2]);prune(loop_history,accepted[3]);prune(impacts,accepted[4]);prune(feedback_events,accepted[5]);prune(trails,accepted[6]);prune(emitters,accepted[7]);prune(casings,accepted[8]);prune(surfaces,accepted[9]);prune(beams,accepted[10]);return true;
}
bool validate_native_confirmed_hit(const NativeConfirmedHit& hit,std::string& error){if(!hit.sequence || !hit.shot || !hit.source || !hit.target || !hit.generation || hit.slot>=8 || (hit.weapon!=1 && hit.weapon!=2 && hit.weapon!=3 && hit.weapon!=4 && hit.weapon!=6) || ((hit.weapon==1 || hit.weapon==2 || hit.weapon==4 || hit.weapon==6) && hit.pellet!=0) || ((hit.weapon==2 || hit.weapon==4) ? hit.shot<=native_automatic_ray_tag || hit.shot>=(native_automatic_ray_tag<<1) : hit.shot>=native_automatic_ray_tag) || hit.pellet>13 || hit.source==hit.target){error="Invalid native confirmed hit identity";return false;}for(auto value:{hit.before,hit.after,hit.amount})if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native confirmed hit coefficient";return false;}if(hit.before<=0 || hit.after>=hit.before || hit.amount<=0){error="Invalid native confirmed hit reduction";return false;}return true;}
bool validate_native_rocket_effect(const NativeRocketEffect& e,std::string& error) {
    for(unsigned i=0;i<e.values.size();++i)if(!std::isfinite(e.values[i]) || std::abs(e.values[i])>(i<3?1000000.f:i<9?10000.f:100.f)){error="Invalid native rocket effect coefficient";return false;}
    if(e.style>=std::size(explosion_classes)){error="Unsupported native explosion style";return false;}if(e.values[9]<=0){error="Invalid native rocket effect scale";return false;}return true;
}
bool install_native_rocket_effect_capture(bool host,std::string& error) {
    if(!install_spatial_audio(error))return false;
    if(!install_native_player_notices(error) || !install_native_emitter_capture(error) || !install_native_casing_capture(error) || !install_native_surface_capture(error) || !install_native_beam_capture(error) || !install_native_bomb_feedback_capture(error) || !install_native_hit_feedback(error))return false;
    for(const auto [address,signature]:{std::pair{0x800EE020u,0x9421FF68u},std::pair{0x800EDFBCu,0x9421FFE8u}})if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native bullet trail lifecycle unavailable";return false;}
    if(!original_trail){original_trail=ssne_find_func(0x800EE020)->func;ssne_install_hook(0x800EE020,trail_hook);}
    if(mem_read32(0x80000000u)!=0x47334250u || !ssne_find_func(0x800D653C) || mem_read32(0x800D653C)!=0x9421FF20u || !ssne_find_func(0x800D6EFC) || mem_read32(0x800D6EFC)!=0x9421FFE0u || !ssne_find_func(0x800EEA90) || mem_read32(0x800EEA90)!=0x9421FFC8u || !ssne_find_func(0x8000DF58) || mem_read32(0x8000DF58)!=0x9421FFF8u){error="Native rocket effect lifecycle unavailable";return false;}
    if(!ssne_find_func(0x8010E7B0) || mem_read32(0x8010E7B0)!=0x9421FFE0u || !ssne_find_func(0x8010EAE0) || mem_read32(0x8010EAE0)!=0x9421FFE0u){error="Native weapon-shot audio lifecycle unavailable";return false;}
    for(const auto [address,signature]:{std::pair{0x8010E86Cu,0x9421FFE0u},std::pair{0x800DB980u,0x9421FFB8u},std::pair{0x800DB850u,0x9421FFE0u}})if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native feedback lifecycle unavailable";return false;}
    if(!ssne_find_func(0x800DC310) || mem_read32(0x800DC310)!=0x9421FFC0u || !ssne_find_func(0x800DC694) || mem_read32(0x800DC694)!=0x9421FFE8u){error="Native gib presentation lifecycle unavailable";return false;}
    if(!ssne_find_func(0x800CBAC4u) || mem_read32(0x800CBAC4u)!=0x9421FFB8u){error="Native scenery fragment lifecycle unavailable";return false;}
    if(!original_prop_fragments){original_prop_fragments=ssne_find_func(0x800CBAC4u)->func;ssne_install_hook(0x800CBAC4u,prop_fragment_hook);}
    if(!original_gib){original_gib=ssne_find_func(0x800DC310)->func;ssne_install_hook(0x800DC310,gib_hook);}
    if(!original_feedback_sound){original_feedback_sound=ssne_find_func(0x8010E86C)->func;ssne_install_hook(0x8010E86C,feedback_sound_hook);}
    if(!original_feedback_score){original_feedback_score=ssne_find_func(0x800DB980)->func;ssne_install_hook(0x800DB980,feedback_score_hook);}
    authoritative=host;
    for(auto address:{0x800ED8E8u,0x800CA44Cu,0x800E7264u,0x800CB894u,0x800CCF38u})if(!ssne_find_func(address)){error="Native bullet visual lifecycle unavailable";return false;}
    if(!original_voice && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){auto entry=ssne_find_func(0x80157840);if(!entry){error="Native voice probe unavailable";return false;}original_voice=entry->func;ssne_install_hook(0x80157840,voice_probe);}
    if(!original_subtract){auto entry=ssne_find_func(0x8009D014);if(!entry){error="Native damage attribution probe unavailable";return false;}original_subtract=entry->func;ssne_install_hook(0x8009D014,subtract_diagnostic);}
    if(!original_source_setter){auto entry=ssne_find_func(0x8009EB94);if(!entry || mem_read32(0x8009EB94)!=0x9421FFF0u){error="Native projectile source setter unavailable";return false;}original_source_setter=entry->func;ssne_install_hook(0x8009EB94,rocket_source_hook);}
    if(!original_area_receiver){auto entry=ssne_find_func(0x80094C84);if(!entry){error="Native area receiver unavailable";return false;}original_area_receiver=entry->func;ssne_install_hook(0x80094C84,area_receiver_diagnostic);}
    if(!original_common_damage){auto entry=ssne_find_func(0x80093AFC);if(!entry){error="Native common damage receiver unavailable";return false;}original_common_damage=entry->func;ssne_install_hook(0x80093AFC,common_damage_hook);}
    if(!original_area_queue){auto entry=ssne_find_func(0x8008C4CC);if(!entry){error="Native queued area damage unavailable";return false;}original_area_queue=entry->func;ssne_install_hook(0x8008C4CC,area_queue_hook);}
    if(!original_query){auto entry=ssne_find_func(0x8009FDA0);if(!entry){error="Native bullet query unavailable";return false;}original_query=entry->func;ssne_install_hook(0x8009FDA0,query_hook);}
    if(!original_impact){original_impact=ssne_find_func(0x800ED8E8)->func;ssne_install_hook(0x800ED8E8,impact_hook);}
    if(!original){original=ssne_find_func(0x800D653C)->func;ssne_install_hook(0x800D653C,effect_hook);}
    if(!original_damage){original_damage=ssne_find_func(0x8000DF58)->func;ssne_install_hook(0x8000DF58,damage_hook);}
    if(!original_shot){original_shot=ssne_find_func(0x8010E7B0)->func;ssne_install_hook(0x8010E7B0,shot_hook);}
    for(auto address:{0x8010EB9Cu,0x8010ED14u,0x8010EDFCu})if(!ssne_find_func(address)){error="Native Uzi loop lifecycle unavailable";return false;}
    if(!original_loop_start){original_loop_start=ssne_find_func(0x8010EB9C)->func;ssne_install_hook(0x8010EB9C,loop_start_hook);original_loop_stop=ssne_find_func(0x8010EDFC)->func;ssne_install_hook(0x8010EDFC,loop_stop_hook);}
    if(!subscription)subscription=observe_native_entities([](const NativeEntityNotice& n){if(n.event==NativeEntityNotice::Event::Removing){std::lock_guard lock(effect_mutex);rocket_sources.erase(n.object);}if(n.event==NativeEntityNotice::Event::LevelExit){std::lock_guard lock(effect_mutex);rocket_sources.clear();queued_rockets.clear();pickup_notices={};pickup_notice_serial=0;++producer_epoch;producer_accepted={};history.clear();shots.clear();loop_history.clear();impacts.clear();feedback_events.clear();pending_splatters.clear();feedback_sequence=0;trails.clear();replica_trails.clear();replica_trail_views.clear();trail_sequence=0;replica_trail_frame.reset();emitters.clear();emitter_sequence=0;casings.clear();replica_casings.clear();casing_sequence=0;surfaces.clear();replica_surfaces.clear();surface_sequence=0;beams.clear();replica_beams.clear();beam_sequence=0;replica_beam_frames.clear();replica_bomb_frame.reset();confirmed_hits.clear();hit_sequence=0;automatic_ray_sequence=0;host_loops.clear();host_cue_voices.clear();replica_cue_voices.clear();replica_loops.clear();sequence=shot_sequence=loop_sequence=impact_sequence=audio_order=0;weapon_shot_ids.clear();}});
    return true;
}
std::vector<NativeRocketEffect> native_rocket_effect_history(){std::lock_guard lock(effect_mutex);return {history.begin(),history.end()};}
std::vector<NativeWeaponShot> native_weapon_shot_history(){std::lock_guard lock(effect_mutex);return {shots.begin(),shots.end()};}
std::vector<NativeBulletImpact> native_bullet_impact_history(){std::lock_guard lock(effect_mutex);return {impacts.begin(),impacts.end()};}
std::vector<NativeBulletTrail> native_bullet_trail_history(){std::lock_guard lock(effect_mutex);return {trails.begin(),trails.end()};}
std::vector<NativeWeaponEmitter> native_weapon_emitter_history(){std::lock_guard lock(effect_mutex);return {emitters.begin(),emitters.end()};}
bool validate_native_weapon_emitter(const NativeWeaponEmitter& e,std::string& error){return emitter_event_valid(e,error);}
bool play_native_weapon_emitter(PPCContext& c,const NativeWeaponEmitter& e,std::string& error){return replay_native_emitter(c,e,error);}
std::vector<NativeWeaponCasing> native_weapon_casing_history(){std::lock_guard lock(effect_mutex);return {casings.begin(),casings.end()};}
bool validate_native_weapon_casing(const NativeWeaponCasing& e,std::string& error){return casing_event_valid(e,error);}
bool play_native_weapon_casing(PPCContext& c,const NativeWeaponCasing& e,std::string& error){return queue_native_casing(c,e,error);}
std::vector<NativeWeaponSurface> native_weapon_surface_history(){std::lock_guard lock(effect_mutex);return {surfaces.begin(),surfaces.end()};}
bool validate_native_weapon_surface(const NativeWeaponSurface& e,std::string& error){return surface_event_valid(e,error);}
bool play_native_weapon_surface(PPCContext& c,const NativeWeaponSurface& e,std::string& error){return queue_native_surface(c,e,error);}
std::vector<NativeWeaponBeam> native_weapon_beam_history(){std::lock_guard lock(effect_mutex);return {beams.begin(),beams.end()};}
bool validate_native_weapon_beam(const NativeWeaponBeam& e,std::string& error){return beam_event_valid(e,error);}
bool play_native_weapon_beam(PPCContext& c,const NativeWeaponBeam& e,std::string& error){return queue_native_beam(c,e,error);}
bool capture_native_bomb_visual(uint32_t actor,unsigned hand,std::vector<float>& v,std::array<uint32_t,2>& masks,std::string& error){return capture_bomb_visual(actor,hand,v,masks,error);}
bool validate_native_bomb_visual(const std::vector<float>& v,const std::array<uint32_t,2>& masks,std::string& error){return bomb_visual_valid(v,masks,error);}
bool apply_native_bomb_visual(PPCContext& c,uint32_t actor,unsigned hand,const std::vector<float>& v,const std::array<uint32_t,2>& masks,std::string& error){return apply_bomb_visual(c,actor,hand,v,masks,error);}
bool validate_native_bullet_trail(const NativeBulletTrail& event,std::string& error){
    if(event.view[0]<0 || event.view[0]>8 || std::floor(event.view[0])!=event.view[0]){error="Invalid native bullet trail view owner";return false;}
    for(auto value:event.view)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native bullet trail camera";return false;}
    if(event.view[0]!=0){if(!(event.mask&(1u<<(unsigned(event.view[0])-1)))){error="Bullet trail excludes its owner view";return false;}
        for(unsigned r=0;r<3;++r)for(unsigned s=0;s<3;++s){float dot=0;for(unsigned c=0;c<3;++c)dot+=event.view[1+r*4+c]*event.view[1+s*4+c];if(std::abs(dot-float(r==s))>.01f){error="Invalid native bullet trail camera basis";return false;}}
    }else for(unsigned i=1;i<13;++i)if(event.view[i]!=0){error="World bullet trail carries a camera reference";return false;}
    for(auto value:event.vectors)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native bullet trail coordinate";return false;}
    double distance=0;for(unsigned axis=0;axis<3;++axis){const auto delta=double(event.vectors[axis+3])-event.vectors[axis];distance+=delta*delta;}
    if(distance<=0){error="Empty native bullet trail";return false;}return true;
}
bool play_native_bullet_trail(PPCContext& context,const NativeBulletTrail& event,std::string& error){
    if(!context_ok(context) || !validate_native_bullet_trail(event,error)){if(error.empty())error="Native bullet trail context unavailable";return false;}
    if(replica_trails.size()>=32768){error="Native bullet trail presentation queue overflow";return false;}
    replica_trails.push_back(event);return true;
}
std::optional<std::array<float,12>> replica_bullet_trail_view(uint32_t proxy){
    const auto it=replica_trail_views.find(proxy);if(it==replica_trail_views.end() || it->second.owner!=unsigned(netplay_local_player()) || !readable(proxy,0x38) || mem_read32(proxy+4)!=it->second.matrix || !readable(it->second.matrix,48))return {};
    bool active=false;const auto count=std::min(4u,mem_read32(0x802DCE10u)); // r13(802E22A0)-5490
    for(unsigned i=0;i<count;++i)active|=mem_read32(0x802D90CCu+i*4)==proxy;if(!active)return {};
    for(unsigned i=0;i<12;++i)if(mem_read32(it->second.matrix+i*4)!=it->second.root[i])return {};
    return it->second.camera;
}
bool validate_native_bullet_impact(const NativeBulletImpact& event,std::string& error){if(event.tags[0]>36 || event.tags[0]%4 || event.tags[1]>17 || event.tags[2]>1 || event.tags[3]>1 || event.tags[4]>255){error="Unsupported native bullet impact tags";return false;}for(auto value:event.vectors)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native bullet impact vector";return false;}return true;}
bool play_native_bullet_impact(PPCContext& context,const NativeBulletImpact& event,std::string& error){if(!context_ok(context) || !validate_native_bullet_impact(event,error)){if(error.empty())error="Native bullet impact context unavailable";return false;}Scratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);for(unsigned i=0;i<9;++i)mem_write32(call.r[1]+0x80+i*4,f2u(event.vectors[i]));call.r[3]=event.tags[0];call.r[4]=event.tags[1];for(unsigned i=0;i<3;++i)call.r[5+i]=call.r[1]+0x80+i*12;call.r[8]=event.tags[2];call.r[9]=event.tags[3];call.r[10]=event.tags[4];NativeBulletImpact arguments;arguments.sequence=event.sequence;arguments.tags={call.r[3],call.r[4],call.r[8],call.r[9],call.r[10]};for(unsigned vector=0;vector<3;++vector)for(unsigned axis=0;axis<3;++axis)arguments.vectors[vector*3+axis]=u2f(mem_read32(call.r[5+vector]+axis*4));if(arguments.tags!=event.tags || arguments.vectors!=event.vectors){error="Native bullet impact arguments differ from authoritative event";return false;}PresentationScope presentation;if(!validate_bullet_lists(call,error))return false;ssne_call(call,0x800ED8E8);if(!validate_bullet_lists(call,error))return false;const auto counts=bullet_counts(call);log_message("[coop-bullet-impact-created] sequence="+std::to_string(event.sequence)+" tracers="+std::to_string(counts[0])+" sparks="+std::to_string(counts[1])+" debris="+std::to_string(counts[2]));log_message("[coop-bullet-impact-client] sequence="+std::to_string(event.sequence)+" payload="+std::to_string(impact_signature(arguments))+" damage_calls="+std::to_string(presentation_damage_calls));return true;}
void native_test_fire_request(bool value){test_fire_request=value;}
bool native_test_fire_requested(){return test_fire_request.load();}
std::vector<NativeWeaponLoop> native_weapon_loop_history(){std::lock_guard lock(effect_mutex);return {loop_history.begin(),loop_history.end()};}
std::vector<NativeWeaponLoop> native_weapon_active_loops(){std::lock_guard lock(effect_mutex);std::vector<NativeWeaponLoop> values;for(const auto& [handle,loop]:host_loops){auto event=loop.event;event.elapsed_ms=uint32_t(std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-loop.began).count(),0,60000));values.push_back(event);}return values;}
bool validate_native_weapon_loop(const NativeWeaponLoop& event,std::string& error){if(event.slot>=8 || event.lane>1 || (event.weapon!=2 && event.weapon!=4) || !event.origin || event.origin>event.sequence || (event.active && event.origin!=event.sequence) || event.duration_ms>60000 || event.elapsed_ms>60000 || (!event.active && event.elapsed_ms) || (event.active && event.duration_ms)){error="Invalid native Uzi loop metadata";return false;}for(auto value:event.position)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native Uzi loop position";return false;}return true;}
// Direct native selection deactivates the old weapon without ticking its fire
// state. Stop only its recorded sustained voices before that update is skipped.
// Generation-bearing handles and the instance field must both still agree.
bool stop_native_weapon_loops_for_actor(PPCContext& context,uint32_t actor,unsigned weapon,std::string& error){
    if(weapon!=2 && weapon!=4)return true;
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || !ssne_find_func(0x8010EDFC)){error="Native weapon-switch loop context unavailable";return false;}
    const auto slot=mem_read32(actor+0x17C);
    if(slot>=native_party_capacity() || mem_read32(native_party_registry()+slot*4)!=actor){error="Native weapon-switch loop owner unavailable";return false;}
    const auto offset=weapon==2?0x244u:0x274u,type=weapon==2?0x801FC858u:0x801FB598u;
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.lr=0;call.r[31]=0;
    for(unsigned lane=0;lane<2;++lane){
        const auto instance=mem_read32(actor+0x4BC+weapon*24+lane*4);
        if(!readable(instance,offset+4) || mem_read32(instance+0xC)!=type || mem_read32(instance+0x1D4)!=actor)continue;
        const auto handle=mem_read32(instance+offset);std::optional<HostLoop> previous;
        {std::lock_guard lock(effect_mutex);const auto found=host_loops.find(handle);
            if(found!=host_loops.end() && found->second.event.slot==slot && found->second.event.weapon==weapon && found->second.event.lane==lane){previous=found->second;host_loops.erase(found);}}
        // Clear the old instance even when its handle has already expired. A
        // reused record or another actor's handle must never be sent to Stop.
        if(mem_read32(instance+offset)==handle)mem_write32(instance+offset,UINT32_MAX);
        if(previous){const bool alive=weapon_loop_handle_alive(handle);if(alive){call.r[3]=handle;ssne_call(call,0x8010EDFC);}publish_loop_stop(*previous);
            log_message("[coop-weapon-switch-loop-stop] slot="+std::to_string(slot)+" weapon="+std::to_string(weapon)+" lane="+std::to_string(lane)+" origin="+std::to_string(previous->event.origin)+" active_handle="+std::to_string(alive));}
    }
    for(auto it=replica_loops.begin();it!=replica_loops.end();)if(it->first/2==slot && it->second.weapon==weapon){
        const auto origin=it->second.origin;const bool alive=weapon_loop_handle_alive(it->second.handle);
        if(alive){call.r[3]=it->second.handle;ssne_call(call,0x8010EDFC);}
        log_message("[coop-weapon-loop-retired] key="+std::to_string(it->first)+" origin="+std::to_string(origin)+" weapon_switch=1 active_handle="+std::to_string(alive));it=replica_loops.erase(it);
    }else ++it;
    return true;
}
bool stop_native_weapon_loops(PPCContext& context,std::string& error){if(!context_ok(context)){error="Native Uzi loop stop context unavailable";return false;}Scratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;for(const auto& [key,loop]:replica_loops){call.r[3]=loop.handle;ssne_call(call,0x8010EDFC);}replica_loops.clear();for(auto [sequence,handle]:replica_cue_voices){call.r[3]=handle;ssne_call(call,0x8010EDFC);}replica_cue_voices.clear();return true;}
bool update_native_weapon_loops(PPCContext& context,std::string& error){update_replica_loop_positions();if(!context_ok(context)){error="Native Uzi loop update context unavailable";return false;}Scratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;const auto now=std::chrono::steady_clock::now();for(auto it=replica_loops.begin();it!=replica_loops.end();)if(it->second.stop_at.time_since_epoch().count() && now>=it->second.stop_at){call.r[3]=it->second.handle;ssne_call(call,0x8010EDFC);log_message("[coop-uzi-loop-retired] key="+std::to_string(it->first));log_message("[coop-weapon-loop-retired] key="+std::to_string(it->first)+" origin="+std::to_string(it->second.origin));it=replica_loops.erase(it);}else ++it;return true;}
bool apply_native_weapon_loop(PPCContext& context,const NativeWeaponLoop& event,bool local_player,std::string& error){
    if(!context_ok(context) || !validate_native_weapon_loop(event,error)){if(error.empty())error="Native Uzi loop context unavailable";return false;}
    const auto key=event.slot*2+event.lane;auto found=replica_loops.find(key);
    if(!event.active){if(found!=replica_loops.end() && found->second.origin==event.origin){found->second.stop_at=found->second.began+std::chrono::milliseconds(event.duration_ms);if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-loop-stop-due] origin="+std::to_string(event.origin)+" remaining_ms="+std::to_string(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::milliseconds>(found->second.stop_at-std::chrono::steady_clock::now()).count())));}log_message(std::string(event.weapon==2?"[coop-uzi-loop-client] sequence=":"[coop-minigun-loop-client] sequence=")+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" lane="+std::to_string(event.lane)+" active=0 duration_ms="+std::to_string(event.duration_ms)+" origin="+std::to_string(event.origin)+" order="+std::to_string(event.order));return true;}
    const auto entry=0x8023AD70u+(event.weapon==2?419u:406u)*24u;const auto variants=mem_read32(entry+8);if(!variants || variants>256 || !readable(mem_read32(entry+12),variants*4)){error="Native Uzi loop resource unavailable";return false;}
    Scratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;
    if(found!=replica_loops.end()){call.r[3]=found->second.handle;ssne_call(call,0x8010EDFC);replica_loops.erase(found);}
    call.r[3]=event.weapon==2?419:406;if(local_player)call.r[4]=event.weapon==2?0:1;else{for(unsigned i=0;i<3;++i)mem_write32(call.r[1]+0x80+i*4,f2u(event.position[i]));call.r[4]=call.r[1]+0x80;}
    PartySoundGate gate(call,event.weapon==4);ssne_call(call,local_player?0x8010EB9Cu:0x8010EC58u);gate.restore();if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES") && event.weapon==4)log_message("[coop-minigun-audio-gate] order="+std::to_string(event.order)+" previous="+std::to_string(gate.previous)+" restored="+std::to_string(mem_read32(call.r[13]-0x5BB0)));if(int32_t(call.r[3])<0){error="Native Uzi loop voice not started";return false;}replica_loops[key]={call.r[3],event.origin,std::chrono::steady_clock::now()-std::chrono::milliseconds(event.elapsed_ms),{},event.weapon};track_shot_voice(call.r[3],0x100000000ull+event.sequence);
    log_message(std::string(event.weapon==2?"[coop-uzi-loop-client] sequence=":"[coop-minigun-loop-client] sequence=")+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" lane="+std::to_string(event.lane)+" active=1 duration_ms=0 voice="+std::to_string(call.r[3])+" local="+std::to_string(local_player)+" origin="+std::to_string(event.origin)+" damage_calls="+std::to_string(presentation_damage_calls)+" order="+std::to_string(event.order)+" elapsed_ms="+std::to_string(event.elapsed_ms));return true;
}
void disable_native_rocket_effect_capture(){authoritative=false;test_fire_request=false;std::lock_guard lock(effect_mutex);rocket_sources.clear();queued_rockets.clear();pickup_notices={};pickup_notice_serial=0;++producer_epoch;producer_accepted={};history.clear();shots.clear();confirmed_hits.clear();loop_history.clear();impacts.clear();feedback_events.clear();pending_splatters.clear();feedback_sequence=0;trails.clear();replica_trails.clear();replica_trail_views.clear();trail_sequence=0;replica_trail_frame.reset();emitters.clear();emitter_sequence=0;casings.clear();replica_casings.clear();casing_sequence=0;surfaces.clear();replica_surfaces.clear();surface_sequence=0;beams.clear();replica_beams.clear();beam_sequence=0;replica_beam_frames.clear();replica_bomb_frame.reset();host_loops.clear();host_cue_voices.clear();replica_cue_voices.clear();replica_loops.clear();hit_resolver={};weapon_shot_ids.clear();hit_sequence=0;automatic_ray_sequence=0;sequence=shot_sequence=loop_sequence=impact_sequence=audio_order=0;}
// Native stop API retires records by generation-tagged handle; stale handles
// must never stop a newly reused sound record.
std::vector<int64_t> native_weapon_cue_state(){
    std::lock_guard lock(effect_mutex);std::vector<int64_t> result{int64_t(shot_sequence)};
    for(auto it=host_cue_voices.begin();it!=host_cue_voices.end();){
        const auto handle=it->first,index=handle&63u;
        if(index>=48 || mem_read32(0x802D69C8u+index*192+0xAC)!=handle)it=host_cue_voices.erase(it);
        else{result.push_back(int64_t(it->second));++it;}
    }
    std::sort(result.begin()+1,result.end());return result;
}
bool reconcile_native_weapon_cues(PPCContext& context,const std::vector<int64_t>& state,std::string& error){
    if(!context_ok(context) || state.empty() || state.size()>49 || state[0]<0){error="Invalid native weapon cue state";return false;}
    for(unsigned i=1;i<state.size();++i)if(state[i]<=0 || state[i]>state[0] || (i>1 && state[i]<=state[i-1])){error="Invalid native weapon cue identity";return false;}
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;
    for(auto it=replica_cue_voices.begin();it!=replica_cue_voices.end();){
        const auto handle=it->second,index=handle&63u;
        const bool alive=index<48 && mem_read32(0x802D69C8u+index*192+0xAC)==handle;
        const bool retired=it->first<=uint64_t(state[0]) && !std::binary_search(state.begin()+1,state.end(),int64_t(it->first));
        if(!alive || retired){if(alive){call.r[3]=handle;ssne_call(call,0x8010EDFC);}log_message("[coop-weapon-cue-retired] sequence="+std::to_string(it->first)+" active_handle="+std::to_string(alive));it=replica_cue_voices.erase(it);}else ++it;
    }
    return true;
}
bool validate_native_weapon_shot(const NativeWeaponShot& shot,std::string& error){
    const bool cue=shot.cue_sound && std::any_of(weapon_audio_calls.begin(),weapon_audio_calls.end(),[&](const auto& call){return call.weapon==shot.weapon && call.sound==shot.cue_sound && call.flags==shot.cue_flags;});
    if(shot.slot>=8 || shot.cue_flags>1 || (shot.cue_sound?!cue:(shot.weapon!=1 && shot.weapon!=3 && shot.weapon!=5 && shot.weapon!=6))){error="Invalid native weapon-shot slot/type";return false;}
    for(auto f:shot.position)if(!std::isfinite(f) || std::abs(f)>1000000.f){error="Invalid native weapon-shot position";return false;}return true;
}
bool play_native_weapon_shot(PPCContext& context,const NativeWeaponShot& shot,bool local_player,std::string& error) {
    if(!context_ok(context) || !validate_native_weapon_shot(shot,error)){if(error.empty())error="Native weapon-shot context unavailable";return false;}
    if((shot.weapon==1 || shot.weapon==4) && !ssne_find_func(0x80155E7C)){error="Native pistol audio gate unavailable";return false;}
    const auto sound=shot.cue_sound?shot.cue_sound:shot.weapon==1?417u:shot.weapon==3?414u:shot.weapon==5?405u:415u;
    const auto entry=0x8023AD70u+sound*24u;const auto variants=mem_read32(entry+8);
    if(!variants || variants>256 || !readable(mem_read32(entry+12),variants*4)){error="Native weapon-shot sound resource unavailable";return false;}
    Scratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=sound;
    if(local_player)call.r[4]=shot.cue_sound?shot.cue_flags:shot.weapon==5?0:1;
    else {for(unsigned i=0;i<3;++i)mem_write32(call.r[1]+0x80+i*4,f2u(shot.position[i]));call.r[4]=call.r[1]+0x80;}
    PresentationScope presentation;
    const auto prior_gate=mem_read32(call.r[13]-0x5BB0);
    PartySoundGate party_gate(call,shot.weapon==1 || shot.weapon==4);
    ssne_call(call,local_player?0x8010E7B0u:0x8010E86Cu);
    party_gate.restore();if(shot.weapon==4 && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-minigun-audio-gate] order="+std::to_string(shot.order)+" previous="+std::to_string(prior_gate)+" restored="+std::to_string(mem_read32(call.r[13]-0x5BB0)));if(shot.weapon==1){PPCContext gate=call;gate.r[4]=prior_gate;ssne_call(gate,0x80155E7C);if(mem_read32(call.r[13]-0x5BB0)!=prior_gate){error="Native pistol audio gate was not restored";return false;}if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-pistol-audio-gate] order="+std::to_string(shot.order)+" previous="+std::to_string(prior_gate)+" restored="+std::to_string(mem_read32(call.r[13]-0x5BB0)));}
    if(int32_t(call.r[3])<0){error="Native weapon-shot voice was not started";return false;}
    if(shot.cue_sound)replica_cue_voices[shot.sequence]=call.r[3];
    track_shot_voice(call.r[3],shot.sequence);
    log_message(std::string(shot.cue_sound?"[coop-weapon-cue-client] sequence=":"[coop-weapon-shot-client] sequence=")+std::to_string(shot.sequence)+" slot="+std::to_string(shot.slot)+" local="+std::to_string(local_player)+" voice="+std::to_string(call.r[3])+" damage_calls="+std::to_string(presentation_damage_calls)+" x="+std::to_string(shot.position[0])+" y="+std::to_string(shot.position[1])+" z="+std::to_string(shot.position[2])+" weapon="+std::to_string(shot.weapon)+" cue_sound="+std::to_string(shot.cue_sound)+" order="+std::to_string(shot.order));return true;
}
bool play_native_rocket_effect(PPCContext& context,const NativeRocketEffect& e,std::string& error) {
    if(!context_ok(context) || !validate_native_rocket_effect(e,error)){if(error.empty())error="Native rocket effect context unavailable";return false;}
    const auto& type=explosion_classes[e.style];
    if(!explosion_class_valid(type)){error="Native explosion presentation class differs";return false;}
    Scratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    for(unsigned i=0;i<9;++i)mem_write32(call.r[1]+0x80+i*4,f2u(e.values[i]));
    call.r[3]=call.r[1]+0x80;call.r[4]=call.r[1]+0x8C;call.r[5]=call.r[1]+0x98;call.r[6]=e.style;call.ps0[1]=call.ps1[1]=e.values[9];
    // This creates original visual/audio resources only. The preceding native
    // damage dispatch 8000DF58 in Rocket::Impact is never invoked here.
    std::array<float,10> arguments;for(unsigned i=0;i<9;++i)arguments[i]=u2f(mem_read32(call.r[1]+0x80+i*4));arguments[9]=float(call.ps0[1]);
    if(arguments!=e.values){error="Native rocket effect arguments differ from authoritative event";return false;}
    PresentationScope presentation;ssne_call(call,0x800D653C);
    if(!call.r[3]){error="Native explosion presentation pool is full";return false;}
    log_message("[coop-rocket-effect-client] sequence="+std::to_string(e.sequence)+" x="+std::to_string(e.values[0])+" y="+std::to_string(e.values[1])+" z="+std::to_string(e.values[2])+" payload="+std::to_string(signature(arguments))+" damage_calls="+std::to_string(presentation_damage_calls)+" active="+std::to_string(mem_read32(context.r[13]-0x5FB8))+" style="+std::to_string(e.style));
    return true;
}
bool play_native_energy_transition(PPCContext& context,uint32_t object,std::string& error){
    if(!context_ok(context) || authoritative || !original_damage || presenting ||
       !readable(object,0x17C) || mem_read32(object+0x48)!=0x80200598u ||
       mem_read32(0x8007223C)!=0x9421FFA0u || !ssne_find_func(0x8007223C)){
        error="Native energy transition context/class unavailable";return false;
    }
    const auto phase=mem_read32(object+0x148);
    if(phase>1){error="Native energy transition requires charge or flight phase";return false;}
    const auto charge_proxy=mem_read32(object+0x134);
    uint32_t charge_table=0,charge_retire=0;
    if(charge_proxy){
        if(!readable(charge_proxy,4) || !readable(charge_table=mem_read32(charge_proxy),0x18) ||
           !ssne_find_func(charge_retire=mem_read32(charge_table+0x14))){
            error="Native energy charge proxy retirement unavailable";return false;
        }
    }
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;
    mem_write32(call.r[1],context.r[1]);call.r[3]=object;
    struct TransitionScope {
        TransitionScope(uint32_t object){energy_transition_object=object;energy_transition_skipped=0;}
        ~TransitionScope(){energy_transition_object=0;energy_transition_skipped=0;}
    } transition(object);
    PresentationScope presentation;
    // Original 800719A0 retires the old charge circle before 8007223C.
    if(charge_proxy){call.r[3]=charge_proxy+int16_t(mem_read16(charge_table+0x10));call.r[4]=3;ssne_call(call,charge_retire);}
    mem_write32(object+0x134,0);call.r[3]=object;ssne_call(call,0x8007223C);
    if(energy_transition_skipped!=1 || mem_read32(object+0x148)!=2 ||
       !mem_read32(object+0x164) || !mem_read32(object+0x168)){
        error="Native energy transition did not create original explosion models";return false;
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))
        log_message("[coop-energy-transition-client] object="+std::to_string(object)+" phase=2 skipped_host_damage=1 damage_calls="+std::to_string(presentation_damage_calls));
    return true;
}
bool validate_native_feedback(const NativeFeedbackEvent& e,std::string& error) {
    for(auto v:e.values)if(!std::isfinite(v) || std::abs(v)>1000000.f){error="Invalid native feedback coordinates";return false;}
    if(e.kind==8 || e.kind==9){
        if(e.variant>=8 || (e.kind==8 && ((e.code && (e.code<0x180 || e.code>0x183)) || e.values[0]!=0)) || (e.kind==9 && (e.code!=0x89 || e.values[0]!=0)) || std::any_of(e.values.begin()+1,e.values.end(),[](float v){return v!=0;})){error="Invalid native combo audio";return false;}return true;
    }
    if((e.kind>7 && e.kind!=10) || (e.kind==6 && (e.code<1 || e.code>6 || e.variant || std::any_of(e.values.begin()+9,e.values.end(),[](float v){return v!=0;}))) || (e.kind==7 && (e.code>=8 || e.variant || e.values[0]<0 || e.values[0]>100 || std::any_of(e.values.begin()+1,e.values.end(),[](float v){return v!=0;}))) || ((e.kind==5 || e.kind==10) && (e.code>=600 || e.variant>=8 || std::any_of(e.values.begin()+3,e.values.end(),[](float v){return v!=0;}))) || (e.kind==0 && (e.code>=600 || e.variant>256)) || (e.kind==1 && (!e.code || e.code>1000000000 || e.variant>1000 || e.values[6]<0 || e.values[6]>100 || e.values[7]<0)) ||
       (e.kind==2 && (e.code>=8 || e.variant || e.values[0]<=0 || e.values[0]>60 || e.values[1]<=0 || e.values[1]>100 || std::any_of(e.values.begin()+2,e.values.end(),[](float v){return v!=0;})))){error="Invalid native feedback type/value";return false;}if(e.kind==3 && !validate_gib_payload(e,error))return false;if(e.kind==4 && !validate_prop_fragment_payload(e,error))return false;return true;
}
bool play_native_feedback(PPCContext& context,const NativeFeedbackEvent& e,std::string& error) {
    if(!context_ok(context) || !validate_native_feedback(e,error))return false;
    if(e.kind==3)return play_gib(context,e,error);if(e.kind==4)return play_prop_fragments(context,e,error);
    if(e.kind>=6 && e.kind!=10)return play_native_hit_feedback(context,e,error);
    uint32_t bomb_hud=0,bomb_flash=0;if(e.kind==2 && !bomb_feedback_binding(e.code,bomb_hud,bomb_flash)){error="Native bomb HUD target unavailable";return false;}
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    for(unsigned i=0;i<6;++i)mem_write32(call.r[1]+0x80+i*4,f2u(e.values[i]));
    PresentationScope presentation;
    if(!e.kind || e.kind==5 || e.kind==10){
        const auto resource=0x8023AD70u+e.code*24;const auto variants=mem_read32(resource+8);
        if(!variants || variants>256 || !readable(mem_read32(resource+12),variants*4)){error="Native feedback sound resource unavailable";return false;}
        const bool own_pickup=(e.kind==5 || e.kind==10) && int(e.variant)==netplay_local_player();
        if(!e.kind && e.variant){
            const auto manager=mem_read32(0x802ABAB0u);
            if(e.variant>variants || !readable(manager,0x4C) || !ssne_find_func(0x801597B4) || !ssne_find_func(0x80157124)){error="Authoritative sound selection unavailable";return false;}
            const auto sample=mem_read32(mem_read32(resource+12)+(e.variant-1)*4);
            call.r[3]=manager;call.r[4]=uint32_t(int32_t(int8_t(mem_read8(resource+0x14))));ssne_call(call,0x801597B4);
            call.r[3]=manager;call.r[4]=sample;call.r[5]=mem_read32(resource+0x10);call.r[6]=call.r[1]+0x80;
            call.ps0[1]=call.ps1[1]=u2f(mem_read32(0x8021AF3Cu));ssne_call(call,0x80157124);
        }else{ // variant0 remains readable for saved legacy event journals.
            call.r[3]=e.code;call.r[4]=own_pickup?0:call.r[1]+0x80;ssne_call(call,own_pickup?0x8010E7B0:0x8010E86C);
        }
        // Opt-in diagnostics record the actual decoded and panned samples.
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))track_shot_voice(call.r[3],0x200000000ull+e.sequence);
    }else if(e.kind==2){
        // The existing replica HUD refresh calls original 800BB280/800ADF68
        // after event application. It owns fading and hiding the flash.
        call.r[3]=bomb_hud;call.ps0[1]=call.ps1[1]=e.values[0];call.ps0[2]=call.ps1[2]=e.values[1];ssne_call(call,0x800BE700u);
        if(mem_read32(bomb_flash+0x10)!=1){error="Native bomb HUD flash did not start";return false;}
    }else{
        const auto pool=mem_read32(context.r[13]-0x5404);const auto count=mem_read32(context.r[13]-0x5408);
        if(count>30 || !readable(pool,30*0x44)){error="Native score presentation pool invalid";return false;}
        // A full visual pool drops the same excess effect as the original game.
        if(count==30)return true;
        unsigned index=0;while(index<30 && mem_read32(pool+index*0x44+0x34))++index;
        if(index==30){error="Native score presentation pool count differs";return false;}
        call.r[3]=pool+index*0x44;call.r[4]=e.code;call.r[5]=e.variant;call.r[6]=call.r[1]+0x80;call.r[7]=call.r[1]+0x8C;call.ps0[1]=call.ps1[1]=e.values[6];call.ps0[2]=call.ps1[2]=e.values[7];ssne_call(call,0x800DB980);
    }
    log_message("[coop-feedback-client] sequence="+std::to_string(e.sequence)+" kind="+std::to_string(e.kind)+" code="+std::to_string(e.code)+(e.kind==2?" duration="+std::to_string(e.values[0])+" strength="+std::to_string(e.values[1]):"")+" result="+std::to_string(int32_t(call.r[3]))+" damage_calls="+std::to_string(presentation_damage_calls));return true;
}
// Presentation subsections of PAL800CE920. Do not execute its arbitrary effect
// callbacks or actor/world update: only the two shared flame texture matrices,
// known glow meshes and permanent sparkle emitters are advanced here.
bool update_native_pickup_ambient(PPCContext& context,float seconds,std::string& error){
    if(!context_ok(context) || !std::isfinite(seconds) || seconds<0 || seconds>.1f){error="Invalid ambient presentation timestep";return false;}
    const auto manager=mem_read32(0x802ABAB8u);if(!manager)return true;
    if(!readable(manager,0x20)){error="Invalid ambient effect manager";return false;}
    std::vector<uint32_t> sparkles;std::set<uint32_t> nodes;auto node=mem_read32(manager);uint32_t previous=0;
    while(node){
        if(!readable(node,12) || !nodes.insert(node).second || nodes.size()>32768 || mem_read32(node+8)!=previous){error="Invalid ambient effect list";return false;}
        const auto object=mem_read32(node);
        if(!readable(object,0x20)){error="Invalid ambient effect object";return false;}
        if(mem_read32(object+0x1C)==0x802156A8u && mem_read32(object+0x18)==0){
            if(!readable(object,0x2D8) || mem_read32(0x802156A8u+0x34)!=0x800D20E8u){error="Invalid sparkle presentation binding";return false;}
            const auto resource=mem_read32(object+0x2C);
            const auto clock=u2f(mem_read32(object+0x20));
            if(!readable(resource,0x64) || mem_read32(resource)!=0x8021DEC8u || !mem_read32(resource+0x3C) || !readable(mem_read32(resource+0x60),0x280) || !std::isfinite(clock) || clock<0 || clock>10.f){error="Invalid sparkle mesh/clock";return false;}
            sparkles.push_back(object);
        }
        previous=node;node=mem_read32(node+4);
    }
    if(nodes.size()!=mem_read32(manager+4)){error="Ambient effect count differs";return false;}
    struct FlameTexture {uint32_t matrix,clock;float next;};
    std::vector<FlameTexture> flames;
    // PAL800CE920's first two branches animate SmallFlame and its distortion
    // material. The full manager also owns arbitrary gameplay callbacks and
    // effect removal, so execute only these verified texture-matrix operations.
    if(mem_read32(0x800CE920u)!=0x9421FF08u || !ssne_find_func(0x801BA380u) || !ssne_find_func(0x801BA334u) ||
       mem_read32(0x80214EF8u)!=0x3F4CCCCDu || mem_read32(0x80214F0Cu)!=0x3F19999Au ||
       mem_read32(0x80214EFCu)!=0x3F800000u || mem_read32(0x80214F00u)!=0x3F000000u ||
       mem_read32(0x80214F04u)!=0x3CF5C28Fu || mem_read32(0x80214F08u)!=0){error="Original flame texture updater unavailable";return false;}
    for(unsigned i=0;i<2;++i){
        const auto matrix=mem_read32(manager+0x18+i*4),clock=context.r[13]-0x7878+i*4;
        if(!matrix)continue;
        const auto phase=u2f(mem_read32(clock));
        const auto next=float(double(seconds)*double(u2f(mem_read32(i?0x80214F0Cu:0x80214EF8u)))+double(phase));
        if(!readable(matrix,48) || !std::isfinite(phase) || !std::isfinite(next) || std::abs(next)>1000000.f){error="Invalid flame texture matrix/clock";return false;}
        flames.push_back({matrix,clock,next});
    }
    const bool glows=mem_read32(context.r[13]-0x7778)!=0;
    if(glows)for(unsigned i=0;i<6;++i){
        const auto resource=mem_read32(0x80238EA0u+i*4);
        if(!readable(resource,0x64) || mem_read32(resource)!=0x8021DEC8u || !mem_read32(resource+0x3C) || !readable(mem_read32(resource+0x60),0x500)){error="Invalid shared pickup glow mesh";return false;}
    }
    Scratch scratch(context);PresentationScope presentation;
    auto run=[&](uint32_t function,uint32_t object){auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=object;call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,function);};
    for(const auto& flame:flames){
        mem_write32(flame.clock,f2u(flame.next));
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
        call.r[3]=flame.matrix;call.ps0[1]=call.ps1[1]=1.;call.ps0[2]=call.ps1[2]=.5;call.ps0[3]=call.ps1[3]=1.;ssne_call(call,0x801BA380u);
        call.r[3]=call.r[4]=flame.matrix;call.ps0[1]=call.ps1[1]=float(double(flame.next)*double(u2f(mem_read32(0x80214F04u))));
        call.ps0[2]=call.ps1[2]=flame.next;call.ps0[3]=call.ps1[3]=0.;ssne_call(call,0x801BA334u);
    }
    if(glows)for(auto function:{0x800E3070u,0x800E3238u,0x800E357Cu,0x800E376Cu,0x800E3968u,0x800E3B50u})run(function,0);
    for(auto object:sparkles)run(0x800D20E8u,object);
    return true;
}
bool update_native_replica_effects(PPCContext& context,float seconds,std::string& error) {
    if(!context_ok(context) || !std::isfinite(seconds) || seconds<0 || seconds>.1f){error="Invalid native effect presentation timestep";return false;}
    if(!update_native_pickup_ambient(context,seconds,error) || !update_replica_bullet_trails(context,error))return false;
    if(!update_native_emitters(context,seconds,error) || !update_native_casings(context,seconds,error) || !update_native_surfaces(context,error) || !update_native_beams(context,seconds,error) || !update_native_bombs(context,error) || !update_native_splatters(context,seconds,error))return false;
    // Validate this presentation list before invoking its original updater.
    std::set<uint32_t> nodes;auto node=mem_read32(context.r[13]-0x5FBC);
    while(node){if(!readable(node,12) || !nodes.insert(node).second || nodes.size()>20){error="Invalid native effect presentation list";return false;}const auto object=mem_read32(node);const auto table=readable(object,8)?mem_read32(object+4):0;const auto type=std::find_if(std::begin(explosion_classes),std::end(explosion_classes),[&](const auto& t){return t.table==table;});if(type==std::end(explosion_classes) || !readable(object,type->size) || !explosion_class_valid(*type)){error="Unverified replica effect presentation class";return false;}node=mem_read32(node+4);}
    for(const auto offsets:{std::array<unsigned,4>{0x5F3C,0x18,0x10,0x5F38},std::array<unsigned,4>{0x5F34,0x48,0x40,0x5F30}}) {
        std::set<uint32_t> particles;auto current=mem_read32(context.r[13]-offsets[0]);uint32_t previous=0;
        while(current){if(!readable(current,offsets[1]) || !particles.insert(current).second || particles.size()>32768 || mem_read32(current+offsets[2]+4)!=previous){error="Invalid native replica particle lifetime list";return false;}previous=current;current=mem_read32(current+offsets[2]);}
        if(particles.size()!=mem_read32(context.r[13]-offsets[3])){error="Native replica particle count differs from list";return false;}
    }
    if(!validate_bullet_lists(context,error) || !validate_gib_list(context,error))return false;
    Scratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.ps0[1]=call.ps1[1]=seconds;
    const auto score_pool=mem_read32(context.r[13]-0x5404),score_count=mem_read32(context.r[13]-0x5408);
    if(score_count){if(score_count>30 || !readable(score_pool,30*0x44)){error="Invalid floating-score pool";return false;}unsigned active=0;for(unsigned i=0;i<30;++i)active+=bool(mem_read32(score_pool+i*0x44+0x34));if(active!=score_count){error="Floating-score pool count differs";return false;}}
    PresentationScope presentation;if(score_count){ssne_call(call,0x800DB850);call.ps0[1]=call.ps1[1]=seconds;}if(!nodes.empty())ssne_call(call,0x800D6EFC);
    // Original visual-only particle emitter manager, normally reached from
    // world update801507E0. Retires Rocket's +128/+12C after its destructor
    // marks them fading. Never run the encompassing world update on a client.
    call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x800EEA90);
    // Original gib/blood-fragment physics/fade has no actor damage callback.
    call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x800DC694);if(!validate_gib_list(context,error))return false;
    // Original presentation subsections of801507E0, not its gameplay update.
    for(auto address:{0x800CA44Cu,0x800E7264u,0x800CB894u,0x800CCF38u}){call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,address);}if(!validate_bullet_lists(call,error))return false;
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){static std::array<uint32_t,3> previous={~0u,~0u,~0u};const auto counts=bullet_counts(call);if(counts!=previous){previous=counts;log_message("[coop-bullet-impact-pool] tracers="+std::to_string(counts[0])+" sparks="+std::to_string(counts[1])+" debris="+std::to_string(counts[2]));}}return true;
}
#include "native_effect_state.inc"
}
