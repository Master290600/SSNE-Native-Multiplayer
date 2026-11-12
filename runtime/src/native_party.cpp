#include "ssne/native_party.h"
#include "ssne/weapon_groups.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include <algorithm>
#include <array>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <cmath>
#include <chrono>
#include "ssne/replica_simulation.h"
#include "ssne/native_entity.h"
#include "ssne/game_addresses.h"
namespace ssne {
namespace {
unsigned roster=2;
bool configured=false,profiles_prepared=false,extended_memory=false;
bool occupancy_enabled=false;
bool leaving_network=false;
uint8_t occupied_slots=255;
uint8_t connected_slots=255;
struct ReservedBodyMask {uint32_t proxy,mask;};
std::map<uint32_t,ReservedBodyMask> reserved_body_masks;
std::array<uint32_t,8> profiles{~0u,~0u,~0u,~0u,~0u,~0u,~0u,~0u};
uint32_t arena=0, original_bank=0, second_bank=0, second_status=0, registry=0, views=0, engine_base=0, pickup_next=0;
uint32_t arena_heap=0,arena_size=0;
// Persist the four additional campaign records across level arena lifetimes.
// Layout comes from 800B73BC: weapon mask, selected slot, eighteen u16 ammo.
std::array<std::array<uint32_t,11>,4> extra_loadouts{};
bool extra_loadouts_initialized=false;
std::map<uint32_t,uint32_t> pickup_flags;
std::map<uint32_t,unsigned> view_owners;
std::array<PadState,8> pads;
std::array<uint32_t,8> known_player_actors{};
struct PendingSpawn {unsigned request=0,flags=0,attempts=0;bool local=true,force_safe=false;std::chrono::steady_clock::time_point next{};};
std::map<uint32_t,PendingSpawn> pending_spawns;
std::map<uint32_t,std::array<float,3>> prepared_spawns;
std::map<uint32_t,uint64_t> completed_spawns;
uint64_t next_spawn_revision=0;
thread_local uint32_t last_segment_source=~0u;
thread_local bool probing_spawn=false,probe_success=false;
thread_local bool force_safe_spawn=false;
thread_local uint32_t probing_actor=0;
thread_local std::string probe_error;
uint64_t pending_subscription=0;std::optional<uint32_t> test_collider_exit_restore;
void ensure_pending_observer() {
    if(pending_subscription)return;
    pending_subscription=observe_native_entities([](const NativeEntityNotice& notice){
        if(notice.event==NativeEntityNotice::Event::LevelExit)completed_spawns.clear();
        else if(notice.event==NativeEntityNotice::Event::Removing)completed_spawns.erase(notice.object);
        if(notice.event==NativeEntityNotice::Event::LevelExit)reserved_body_masks.clear();
        else if(notice.event==NativeEntityNotice::Event::Removing)reserved_body_masks.erase(notice.object);
        if(notice.event==NativeEntityNotice::Event::LevelExit){if(test_collider_exit_restore){mem_write32(0x8020E6C4,*test_collider_exit_restore);test_collider_exit_restore.reset();log_message("[coop-test-collider-level-exit-restored]");}for(const auto& [actor,pending]:pending_spawns)log_message("[coop-spawn-cancelled-level] slot="+std::to_string(mem_read32(actor+0x17C))+" attempts="+std::to_string(pending.attempts));pending_spawns.clear();prepared_spawns.clear();known_player_actors.fill(0);}
        else if(notice.event==NativeEntityNotice::Event::Removing){if(pending_spawns.contains(notice.object))log_message("[coop-spawn-lifetime-remove] actor="+std::to_string(notice.object)+" slot="+std::to_string(mem_read32(notice.object+0x17C)));pending_spawns.erase(notice.object);prepared_spawns.erase(notice.object);for(auto& actor:known_player_actors)if(actor==notice.object)actor=0;}
    });
}


thread_local int owner=-1;
bool valid(uint32_t p,unsigned size){return p>=0x80000000 && p<=guest_ram_end()-size;}
int actor_slot(uint32_t actor) {
    if(!valid(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8)return -1;
    const auto slot=mem_read32(actor+0x17C);return slot<roster?int(slot):-1;
}
}
namespace {
PPCFunc sphere_trace_original=nullptr;
void native_sphere_trace(PPCContext& ctx) {
    static unsigned samples=0;const bool trace=samples++<24;
    const auto output=ctx.r[6];
    if(trace) {
        std::string line="[coop-native-sphere-call] caller="+std::to_string(ctx.lr)+" radius="+std::to_string(ctx.ps0[1]);
        for(unsigned i=3;i<=10;++i)line+=" r"+std::to_string(i)+"="+std::to_string(ctx.r[i]);
        for(unsigned reg:{4u,5u})if(valid(ctx.r[reg],12))line+=" p"+std::to_string(reg)+"="+std::to_string(u2f(mem_read32(ctx.r[reg])))+","+std::to_string(u2f(mem_read32(ctx.r[reg]+4)))+","+std::to_string(u2f(mem_read32(ctx.r[reg]+8)));
        log_message(line);
    }
    ssne_call_original(ctx,0x80140620,sphere_trace_original);
    if(trace && valid(output,12))log_message("[coop-native-sphere-result] contacts="+std::to_string(ctx.r[3])+" output="+std::to_string(u2f(mem_read32(output)))+","+std::to_string(u2f(mem_read32(output+4)))+","+std::to_string(u2f(mem_read32(output+8))));
}
}
void native_party_install_geometry_trace() {
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_COOP_TEST_SPAWN_PROBE") || sphere_trace_original)return;
    const auto* query=ssne_find_func(0x80140620);
    if(query && mem_read32(query->address)==0x9421FDF8u){sphere_trace_original=query->func;ssne_install_hook(query->address,native_sphere_trace);}
}
bool native_world_segment(PPCContext& context,const std::array<float,3>& from,const std::array<float,3>& to,std::optional<NativeSegmentHit>& hit,std::string& error) {
    hit.reset();error.clear();
    if(mem_read32(0x80000000u)!=0x47334250u || context.r[13]!=0x802E22A0u || context.r[2]!=0x802F22A0u || context.r[1]<0x80002000u || context.r[1]>guest_ram_end()-4){error="Native geometry query context unavailable";return false;}
    for(auto point:{from,to})for(float value:point)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native geometry segment";return false;}
    const auto manager=mem_read32(0x802ABAA4);const auto* query=ssne_find_func(0x80140214);
    if(!valid(manager,0x18) || mem_read32(manager)!=0x8021EB70u || !query || mem_read32(query->address)!=0x9421FE60u){error="Native geometry query revision unavailable";return false;}
    std::vector<uint8_t> stack(0x2000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x2000+i);
    auto restore=[&](){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);};
    const auto a=context.r[1]-0x100,b=a+16,result=a+32;
    for(unsigned i=0;i<3;++i){mem_write32(a+i*4,f2u(from[i]));mem_write32(b+i*4,f2u(to[i]));}
    for(unsigned i=0;i<48;i+=4)mem_write32(result+i,0);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=manager;call.r[4]=a;call.r[5]=b;call.r[6]=result;call.r[7]=1;
    try{ssne_call(call,query->address);}catch(...){restore();throw;}
    if(call.r[3]) {
        NativeSegmentHit found;
        for(unsigned i=0;i<3;++i){found.position[i]=u2f(mem_read32(result+i*4));found.normal[i]=u2f(mem_read32(result+12+i*4));}
        found.flags=mem_read32(result+32);last_segment_source=mem_read32(result+28);hit=found;
    }
    restore();
    if(hit)for(auto point:{hit->position,hit->normal})for(float value:point)if(!std::isfinite(value)){hit.reset();error="Invalid native geometry result";return false;}
    return true;
}
// Static friction for the native solver's sub-millimetre gravity correction
// on almost level ground. Never latch world position or discard real input,
// falling/jumping, knockback, vehicles, or moving geometry.
void native_party_stabilize_idle(PPCContext& ctx,uint32_t actor,const std::array<float,3>& before){
    if(replica_simulation() || mem_read32(0x802DB8CC)<2 || mem_read32(0x802DB8CC)>4 || !valid(actor,0x790) || mem_read32(actor+0x340)!=1 || mem_read32(actor+0x6c4) || mem_read32(actor+0x370) || mem_read32(actor+0x328) || u2f(mem_read32(actor+0xcc))<=0)return;
    const auto slot=mem_read32(actor+0x17c);if(slot>=roster)return;const auto& pad=pads[slot];
    if(pad.stick_x!=128 || pad.stick_y!=128 || pad.trigger_l || pad.trigger_r || (pad.buttons&(PAD_L|PAD_R)))return;
    std::array<float,3> after;for(unsigned i=0;i<3;++i){after[i]=u2f(mem_read32(actor+0x20+i*4));if(!std::isfinite(before[i]) || !std::isfinite(after[i]))return;}
    const float elapsed=u2f(mem_read32(ctx.r[13]-0x7a08));if(!std::isfinite(elapsed) || elapsed<=0)return;
    const float seconds=std::clamp(elapsed,1.f/240.f,.1f),limit=.1f*seconds;
    const float horizontal=std::hypot(after[0]-before[0],after[2]-before[2]);
    if(horizontal==0 || horizontal>limit || std::abs(after[1]-before[1])>limit*.2f)return;
    const auto radius=native_player_collision_radius(actor);if(!radius)return;
    auto to=after;to[1]-=*radius+.05f;std::optional<NativeSegmentHit> floor;std::string why;
    if(!native_world_segment(ctx,after,to,floor,why) || !floor)return;
    if(getenv("SSNE_TEST_HIDDEN")){static unsigned samples=0;if(++samples%120==1)log_message("[native-idle-support] normal_y="+std::to_string(floor->normal[1])+" height="+std::to_string(after[1]-floor->position[1])+" radius="+std::to_string(*radius)+" source="+std::to_string(last_segment_source));}
    if(floor->normal[1]<.98f || std::abs(after[1]-floor->position[1]-*radius)>.03f)return;
    // Contact+1C is the original brush-resource index (80084618). A
    // registered geometry owner can carry the player even without input.
    uint32_t owner=0;
    if(last_segment_source!=~0u){if(last_segment_source>=512)return;owner=mem_read32(0x802D9E0Cu+last_segment_source*4);if(owner)return;}
    for(unsigned i=0;i<3;++i)mem_write32(actor+0x20+i*4,f2u(before[i]));
    if(getenv("SSNE_TEST_HIDDEN")){static unsigned corrections=0;if(++corrections%120==1)log_message("[native-idle-friction] slot="+std::to_string(slot)+" delta="+std::to_string(horizontal)+" normal_y="+std::to_string(floor->normal[1])+" source="+std::to_string(last_segment_source));}
}
std::optional<float> native_player_collision_radius(uint32_t actor) {
    // Original player getters: one sphere at actor+20; radius getter8009C404
    // loads the PAL constant8020E6C4. Reject any changed collision schema.
    if(mem_read32(0x80000000u)!=0x47334250u || !valid(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(0x802121E8+0x194)!=0x8009C404u || mem_read32(0x802121E8+0x19C)!=0x8009C410u || mem_read32(0x802121E8+0x1AC)!=0x8009C44Cu || mem_read32(0x8009C410)!=0x38600001u)return std::nullopt;
    const float radius=u2f(mem_read32(0x8020E6C4));
    return std::isfinite(radius) && radius>.05f && radius<100.f?std::optional<float>(radius):std::nullopt;
}
bool native_world_sphere(PPCContext& context,const std::array<float,3>& from,const std::array<float,3>& to,float radius,NativeSphereResult& result,std::string& error) {
    result={};error.clear();
    if(mem_read32(0x80000000u)!=0x47334250u || context.r[13]!=0x802E22A0u || context.r[2]!=0x802F22A0u || context.r[1]<0x80002000u || context.r[1]>guest_ram_end()-4){error="Native sphere query context unavailable";return false;}
    if(!std::isfinite(radius) || radius<=0 || radius>100.f){error="Invalid native sphere radius";return false;}
    for(auto point:{from,to})for(float value:point)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native sphere segment";return false;}
    const auto manager=mem_read32(0x802ABAA4);const auto* query=ssne_find_func(0x80140620);
    if(!valid(manager,0x18) || mem_read32(manager)!=0x8021EB70u || !query || mem_read32(query->address)!=0x9421FDF8u){error="Native sphere query revision unavailable";return false;}
    std::vector<uint8_t> stack(0x2000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x2000+i);
    auto restore=[&](){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);};
    const auto a=context.r[1]-0x700,b=a+16,corrected=a+32,contacts=a+48;
    for(unsigned i=0;i<3;++i){mem_write32(a+i*4,f2u(from[i]));mem_write32(b+i*4,f2u(to[i]));mem_write32(corrected+i*4,f2u(to[i]));}
    for(unsigned i=0;i<20*48;i+=4)mem_write32(contacts+i,0);
    // Captured original movement call800C065C:20 contacts, correction enabled,
    // normal player collision mode1. Output position is resolved by native code.
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x800;mem_write32(call.r[1],context.r[1]);call.r[3]=manager;call.r[4]=a;call.r[5]=b;call.r[6]=corrected;call.r[7]=contacts;call.r[8]=20;call.r[9]=1;call.r[10]=1;call.ps0[1]=call.ps1[1]=radius;
    try{ssne_call(call,query->address);}catch(...){restore();throw;}
    if(call.r[3]>20){restore();error="Native sphere contact count exceeded capacity";return false;}
    for(unsigned i=0;i<3;++i)result.position[i]=u2f(mem_read32(corrected+i*4));
    for(unsigned n=0;n<call.r[3];++n){NativeSegmentHit hit;for(unsigned i=0;i<3;++i){hit.position[i]=u2f(mem_read32(contacts+n*48+i*4));hit.normal[i]=u2f(mem_read32(contacts+n*48+12+i*4));}hit.flags=mem_read32(contacts+n*48+32);result.contacts.push_back(hit);}
    restore();
    for(float value:result.position)if(!std::isfinite(value)){error="Invalid native sphere correction";return false;}
    for(const auto& hit:result.contacts)for(auto point:{hit.position,hit.normal})for(float value:point)if(!std::isfinite(value)){error="Invalid native sphere contact";return false;}
    return true;
}
namespace {
bool spawn_actor_overlap(PPCContext& context,uint32_t actor,const std::array<float,3>& point,float radius,bool& occupied,std::string& error) {
    occupied=false;
    // Include constructors currently placing a player before the original
    // global registry has received that pointer.
    std::set<uint32_t> others;
    for(unsigned slot=0;slot<roster;++slot){others.insert(known_player_actors[slot]);others.insert(mem_read32(native_party_registry()+slot*4));}
    std::set<uint32_t> visited;auto node=mem_read32(0x802DA8E8u);
    while(node) {
        if(!valid(node,12) || !visited.insert(node).second || visited.size()>8192){error="Invalid native spawn actor chain";return false;}
        const auto other=mem_read32(node+4);node=mem_read32(node+8);
        if(!valid(other,0xD4) || other==actor || !(mem_read32(other+0x1C)&1))continue;
        const auto table=mem_read32(other+0x48);if(!valid(table,0x1B0))continue;
        // This is the native COLLIDABLE ACTOR chain. Vtable+34 returns an
        // update-group index (Rocketeers use8), not player/enemy type0/1.
        bool shape=true;for(auto offset:{0x194u,0x19Cu,0x1A4u,0x1ACu})shape&=ssne_find_func(mem_read32(table+offset))!=nullptr;
        if(shape)others.insert(other);
    }
    others.erase(0);others.erase(actor);
    for(auto other:others) {
        if(!valid(other,0xD4) || pending_spawns.contains(other))continue;const float health=u2f(mem_read32(other+0xCC));if(!std::isfinite(health) || health<=0)continue;
        if(auto other_radius=native_player_collision_radius(other)) {
            const auto other_slot=mem_read32(other+0x17C);
            // Reserved disconnected bodies must not occupy spawn locations on
            // initial-placement retries either, once membership is known.
            if(occupancy_enabled && other_slot<roster && mem_read32(native_party_registry()+other_slot*4)==other && !(connected_slots&(1u<<other_slot)))continue;
            float distance=0;for(unsigned i=0;i<3;++i){const float d=point[i]-u2f(mem_read32(other+0x20+i*4));distance+=d*d;}
            const float separation=radius+*other_radius+.05f;if(distance<separation*separation){occupied=true;return true;}
        }
        const auto table=mem_read32(other+0x48);if(!valid(table,0x1B0)) {error="Native spawn collision class unavailable";return false;}
        for(auto offset:{0x194u,0x19Cu,0x1A4u,0x1ACu})if(!ssne_find_func(mem_read32(table+offset))){error="Native spawn collision getter unavailable";return false;}
        const auto* check=ssne_find_func(0x800A3408);if(!check || mem_read32(check->address)!=0x9421FF40u){error="Native actor overlap query unavailable";return false;}
        std::vector<uint8_t> stack(0x2000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x2000+i);
        const auto flags=mem_read32(actor+0x1C),location=context.r[1]-0x60;
        auto restore=[&](){mem_write32(actor+0x1C,flags);for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);};
        for(unsigned i=0;i<3;++i)mem_write32(location+i*4,f2u(point[i]));
        // The original read-only overlap function gates its result on active
        // bit0. A constructor has not necessarily activated its actor yet.
        mem_write32(actor+0x1C,flags|1u);
        PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=location;call.r[5]=other;
        try{ssne_call(call,check->address);}catch(...){restore();throw;}restore();
        if(call.r[3]){occupied=true;return true;}
    }
    return true;
}
bool choose_coop_spawn(PPCContext& context,uint32_t actor,const std::array<float,3>& base,std::array<float,3>& chosen,std::string& error) {
    const auto radius=native_player_collision_radius(actor);if(!radius){error="Native player spawn sphere unavailable";return false;}
    auto from=base,to=base;from[1]+=1.f;to[1]-=3.f;std::optional<NativeSegmentHit> original_floor;
    if(!native_world_segment(context,from,to,original_floor,error))return false;
    if(original_floor && original_floor->normal[1]<.7f){error="Native spawn anchor has steep supporting geometry";return false;}
    // Original campaign starts can be in water or above a drop. Keep that
    // native height when there is no nearby floor; sphere sweeps and actor
    // separation still validate every placement, including the original point.
    auto anchor=base;if(original_floor)anchor[1]=original_floor->position[1]+*radius+.02f;
    std::vector<std::array<float,2>> offsets;
    const float spacing=*radius*2+.5f;
    for(int x=-2;x<=2;++x)for(int z=-2;z<=2;++z)if((x*x+z*z)*spacing*spacing<=36.f)offsets.push_back({x*spacing,z*spacing});
    std::stable_sort(offsets.begin(),offsets.end(),[](auto a,auto b){return a[0]*a[0]+a[1]*a[1]<b[0]*b[0]+b[1]*b[1];});
    // Preserve the existing near-anchor placements, then search between the
    // coarse samples if a narrow entrance cannot fit the whole party there.
    // The same six-metre bound, swept sphere, floor and actor checks apply.
    std::vector<std::array<float,2>> finer;
    const float half_spacing=spacing*.5f;
    for(int x=-4;x<=4;++x)for(int z=-4;z<=4;++z) {
        std::array<float,2> offset{x*half_spacing,z*half_spacing};
        if(offset[0]*offset[0]+offset[1]*offset[1]<=36.f && std::find(offsets.begin(),offsets.end(),offset)==offsets.end())finer.push_back(offset);
    }
    std::stable_sort(finer.begin(),finer.end(),[](auto a,auto b){return a[0]*a[0]+a[1]*a[1]<b[0]*b[0]+b[1]*b[1];});
    offsets.insert(offsets.end(),finer.begin(),finer.end());
    unsigned rejected=0;
    for(auto offset:offsets) {
        auto candidate=anchor;candidate[0]+=offset[0];candidate[2]+=offset[1];from=candidate;to=candidate;from[1]+=1.f;to[1]-=3.f;
        std::optional<NativeSegmentHit> floor;
        if(!native_world_segment(context,from,to,floor,error))return false;
        if(original_floor){
            if(!floor || floor->normal[1]<.7f || floor->flags!=original_floor->flags || std::abs(floor->position[1]-original_floor->position[1])>.75f){++rejected;continue;}
            candidate[1]=floor->position[1]+*radius+.02f;
        }
        NativeSphereResult sweep;if(!native_world_sphere(context,anchor,candidate,*radius,sweep,error))return false;
        float difference=0;for(unsigned i=0;i<3;++i){const float d=sweep.position[i]-candidate[i];difference+=d*d;}
        if(difference>.06f*.06f){++rejected;continue;}
        // A nonzero downward probe detects penetration even for the anchor,
        // where a zero-length sweep does not invoke the native collision solver.
        auto down=candidate;down[1]-=.02f;NativeSphereResult volume;
        if(!native_world_sphere(context,candidate,down,*radius,volume,error))return false;
        difference=0;for(unsigned i=0;i<3;++i){const float d=volume.position[i]-candidate[i];difference+=d*d;}
        if(difference>.06f*.06f){++rejected;continue;}
        bool occupied=false;if(!spawn_actor_overlap(context,actor,candidate,*radius,occupied,error))return false;
        if(occupied){++rejected;continue;}
        chosen=candidate;
        log_message("[coop-spawn-placement] slot="+std::to_string(mem_read32(actor+0x17C))+" rejected="+std::to_string(rejected)+" x="+std::to_string(chosen[0])+" y="+std::to_string(chosen[1])+" z="+std::to_string(chosen[2])+" radius="+std::to_string(*radius));return true;
    }
    error="No free supported native spawn location near the original anchor";return false;
}
}
void native_party_restore_test_collider_at_exit(uint32_t original) {
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_BLOCK_SPAWN")){ensure_pending_observer();test_collider_exit_restore=original;}
}
bool native_party_spawn_waiting(uint32_t actor){return pending_spawns.contains(actor);}
bool native_party_prepare_spawn(PPCContext& context,uint32_t actor,unsigned request) {
    if((!native_party_extended() && !force_safe_spawn) || mem_read32(0x802DB8CC)!=1)return true;
    const int slot=actor_slot(actor);if(slot<0)return true;
    known_player_actors[slot]=actor;
    if(prepared_spawns.contains(actor) && !probing_spawn)return true;
    if(auto pending=pending_spawns.find(actor);pending!=pending_spawns.end()){
        pending->second.force_safe|=force_safe_spawn;
        if(std::chrono::steady_clock::now()<pending->second.next)return false;
    }
    std::vector<uint8_t> stack(0x3000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x3000+i);
    const auto point=context.r[1]-0x100,rotation=point+16,metadata=point+32;
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x400;mem_write32(call.r[1],context.r[1]);call.r[3]=mem_read32(0x802DACC0);call.r[4]=unsigned(slot);call.r[5]=point;call.r[6]=rotation;call.r[7]=0;call.r[8]=metadata;call.r[9]=metadata+4;
    const bool before=probing_spawn;const auto before_actor=probing_actor;
    probing_spawn=true;probing_actor=actor;probe_success=false;probe_error.clear();prepared_spawns.erase(actor);
    const auto level=mem_read32(0x802DACC0);const auto primary=valid(level,0x2B0)?mem_read32(level+0x2A8):0;
    if(!valid(primary,0x88))probe_error="Native primary spawn is not available";
    try{if(valid(primary,0x88))ssne_call(call,0x8010B0AC);}catch(...){probing_spawn=before;probing_actor=before_actor;for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x3000+i,stack[i]);throw;}
    const bool ok=probe_success;probing_spawn=before;probing_actor=before_actor;
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x3000+i,stack[i]);
    if(ok){if(auto pending=pending_spawns.find(actor);pending!=pending_spawns.end())mem_write32(actor+0x1C,pending->second.flags);return true;}
    ensure_pending_observer();auto [it,first]=pending_spawns.try_emplace(actor);auto& pending=it->second;
    if(first){pending.flags=mem_read32(actor+0x1C);pending.request=request;pending.force_safe=force_safe_spawn;}
    ++pending.attempts;pending.next=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);mem_write32(actor+0x1C,pending.flags);
    if(first || pending.attempts%20==0)log_message("[coop-spawn-wait] slot="+std::to_string(slot)+" attempts="+std::to_string(pending.attempts)+" reason="+probe_error);
    return false;
}
void native_party_spawn_completed(uint32_t actor) {
    if(native_player_collision_radius(actor)){
        ensure_pending_observer();completed_spawns[actor]=++next_spawn_revision;
        const auto slot=mem_read32(actor+0x17C);if(slot<roster)known_player_actors[slot]=actor;
        log_message("[coop-native-spawn-completed] slot="+std::to_string(mem_read32(actor+0x17C))+" actor="+std::to_string(actor)+" revision="+std::to_string(next_spawn_revision));
    }
    if(auto pending=pending_spawns.find(actor);pending!=pending_spawns.end()){log_message("[coop-spawn-resumed] slot="+std::to_string(mem_read32(actor+0x17C))+" attempts="+std::to_string(pending->second.attempts));pending_spawns.erase(pending);}
    prepared_spawns.erase(actor);
}
uint64_t native_party_spawn_revision(uint32_t actor){const auto found=completed_spawns.find(actor);return found==completed_spawns.end()?0:found->second;}
bool native_party_request_join_spawn(PPCContext& context,uint32_t actor,std::string& error){
    error.clear();const auto* start=ssne_find_func(0x800B2E5C);
    if(!native_player_collision_radius(actor) || !start || mem_read32(start->address)!=0x9421FFB0u || context.r[13]!=0x802E22A0u || context.r[1]<0x80003000u || context.r[1]>guest_ram_end()-4){error="Native join placement context unavailable";return false;}
    const auto before=native_party_spawn_revision(actor);std::vector<uint8_t> stack(0x3000);
    for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x3000+i);
    const auto previous_force=force_safe_spawn;force_safe_spawn=true;
    auto restore=[&]{force_safe_spawn=previous_force;for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x3000+i,stack[i]);};
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=0;
    try{ssne_call(call,start->address);}catch(...){restore();throw;}restore();
    if(native_party_spawn_revision(actor)>before)return true;
    error=native_party_spawn_waiting(actor)?"Native join is waiting for a free spawn point":"Native join spawn did not complete";return false;
}
void native_party_cancel_join_spawn(uint32_t actor){
    const auto found=pending_spawns.find(actor);if(found!=pending_spawns.end() && found->second.force_safe){pending_spawns.erase(found);prepared_spawns.erase(actor);log_message("[coop-join-spawn-cancelled] actor="+std::to_string(actor));}
}
bool native_party_retry_spawn(PPCContext& context,uint32_t actor) {
    auto pending=pending_spawns.find(actor);if(pending==pending_spawns.end())return false;
    if(replica_simulation() || !pending->second.local || std::chrono::steady_clock::now()<pending->second.next)return true;
    // Capacity reserves storage; disconnected slots wait until membership changes.
    // Preserve constructor/unknown-registry behavior and pending state for later joins.
    static const bool defer=[] {const auto value=std::getenv("SSNE_DEFER_DISCONNECTED_SPAWNS");return !value || std::strcmp(value,"0");}();
    if(defer && occupancy_enabled){const auto slot=actor_slot(actor);if(slot>=0 && mem_read32(native_party_registry()+unsigned(slot)*4)==actor && !(connected_slots&(1u<<slot)))return true;}
    const auto request=pending->second.request;
    std::vector<uint8_t> stack(0x3000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x3000+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=request;
    const auto previous_force=force_safe_spawn;force_safe_spawn=pending->second.force_safe;
    try{ssne_call(call,0x800B2E5C);}catch(...){force_safe_spawn=previous_force;for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x3000+i,stack[i]);throw;}
    force_safe_spawn=previous_force;
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x3000+i,stack[i]);return native_party_spawn_waiting(actor);
}
void native_party_retry_waiting(PPCContext& context) {
    static unsigned visits=0;if(++visits%120==0 && !pending_spawns.empty()){for(const auto& [actor,p]:pending_spawns)log_message("[coop-spawn-retry-state] actor="+std::to_string(actor)+" local="+std::to_string(p.local)+" replica="+std::to_string(replica_simulation())+" class="+std::to_string(mem_read32(actor+0x48))+" slot="+std::to_string(mem_read32(actor+0x17C))+" registry="+std::to_string(mem_read32(native_party_registry()+mem_read32(actor+0x17C)*4)));}
    std::vector<uint32_t> actors;for(const auto& [actor,pending]:pending_spawns)if(pending.local)actors.push_back(actor);
    for(auto actor:actors)if(native_player_collision_radius(actor))native_party_retry_spawn(context,actor);
}
bool native_party_replica_spawn_state(PPCContext& context,uint32_t actor,bool waiting,const std::array<float,3>& position) {
    if(!replica_simulation() || !native_player_collision_radius(actor))return false;
    if(waiting){ensure_pending_observer();auto [it,first]=pending_spawns.try_emplace(actor);if(first){it->second.flags=mem_read32(actor+0x1C);it->second.local=false;log_message("[coop-replica-spawn-wait] slot="+std::to_string(mem_read32(actor+0x17C)));}return true;}
    auto pending=pending_spawns.find(actor);if(pending==pending_spawns.end())return true;
    if(!pending->second.local){mem_write32(actor+0x1C,pending->second.flags);pending_spawns.erase(pending);log_message("[coop-replica-spawn-resumed] slot="+std::to_string(mem_read32(actor+0x17C)));return true;}
    prepared_spawns[actor]=position;mem_write32(actor+0x1C,pending->second.flags);
    std::vector<uint8_t> stack(0x3000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x3000+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=0;
    try{ssne_call(call,0x800B2E5C);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x3000+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x3000+i,stack[i]);return !native_party_spawn_waiting(actor);
}
void native_party_configure(unsigned count){roster=std::clamp(count,1u,8u);configured=true;leaving_network=false;}
void native_party_leave_network(){configured=false;occupancy_enabled=false;profiles_prepared=false;leaving_network=true;pads={};owner=-1;if(!arena){roster=2;leaving_network=false;}}
uint32_t native_party_profile(unsigned slot){return slot<8?profiles[slot]:~0u;}
bool native_party_joined(unsigned slot){return !configured || slot<roster;}
void native_party_set_occupancy(uint8_t mask,uint8_t connected_mask){
    if(!occupancy_enabled || mask!=occupied_slots)log_message("[coop-native-occupancy] mask="+std::to_string(mask));
    occupied_slots=mask;connected_slots=connected_mask;occupancy_enabled=true;
}
bool native_party_actor_occupied(uint32_t actor){
    // Capacity reserves initialized storage; only ready participants enter
    // native gameplay. Do not alter health, inventory or the lifetime bit.
    if(!occupancy_enabled || mem_read32(game_address(0x802DB0E0))!=4 || (mem_read32(0x802DB8CC)<1 || mem_read32(0x802DB8CC)>4) || !valid(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)return true;
    const auto slot=mem_read32(actor+0x17C);
    return slot>=roster || mem_read32(native_party_registry()+slot*4)!=actor || (occupied_slots&(1u<<slot));
}
void native_party_refresh_occupancy(PPCContext& context){
    if(!occupancy_enabled || mem_read32(game_address(0x802DB0E0))!=4 || (mem_read32(0x802DB8CC)<1 || mem_read32(0x802DB8CC)>4))return;
    const auto* setter=ssne_find_func(0x801527C0);
    if(!setter || mem_read32(setter->address)!=0x9421FFF0u || context.r[1]<0x80002000u || context.r[1]>guest_ram_end()-4)fatal("Native player body visibility setter unavailable.");
    for(unsigned slot=0;slot<roster;++slot){
        const auto actor=mem_read32(native_party_registry()+slot*4);if(!native_player_collision_radius(actor))continue;
        // Original player initializer stores the animated model instance at
        // +148;801527C0 propagates its view mask to all three render handles.
        const auto proxy=mem_read32(actor+0x148),model=valid(proxy,0x20)?mem_read32(proxy+0x10):0;
        if(!valid(model,0xB0) || mem_read32(model)!=0x8021D668u)continue;
        const bool occupied=native_party_actor_occupied(actor);auto found=reserved_body_masks.find(actor);
        if(found!=reserved_body_masks.end() && found->second.proxy!=proxy){reserved_body_masks.erase(found);found=reserved_body_masks.end();}
        if(!occupied && found==reserved_body_masks.end()){ensure_pending_observer();found=reserved_body_masks.emplace(actor,ReservedBodyMask{proxy,mem_read32(proxy+0x18)}).first;}
        if(occupied && found==reserved_body_masks.end())continue;
        const auto mask=occupied?found->second.mask:0u;
        if(mem_read32(proxy+0x18)!=mask){
            std::vector<uint8_t> stack(0x2000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x2000+i);
            auto restore=[&]{for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);};
            PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=proxy;call.r[4]=mask;
            try{ssne_call(call,setter->address);}catch(...){restore();throw;}restore();
            log_message("[coop-native-body-mask] slot="+std::to_string(slot)+" occupied="+std::to_string(occupied)+" mask="+std::to_string(mem_read32(proxy+0x18)));
        }
        if(occupied)reserved_body_masks.erase(actor);
    }
}
namespace {
PPCFunc occupied_spheres_original=nullptr,occupied_health_original=nullptr,occupied_render_original=nullptr,occupied_contact_original=nullptr,occupied_active_original=nullptr;
PPCFunc diagnostic_effect_original=nullptr,diagnostic_rocket_original=nullptr;
void diagnostic_effect(PPCContext& ctx){const auto type=ctx.r[3],caller=ctx.lr;ssne_call_original(ctx,0x8009DD48,diagnostic_effect_original);const auto object=ctx.r[3];if(valid(object,0x4C))log_message("[coop-effect-created] type="+std::to_string(type)+" caller="+std::to_string(caller)+" object="+std::to_string(object)+" table="+std::to_string(mem_read32(object+0x48)));}
void diagnostic_rocket(PPCContext& ctx){const auto weapon=ctx.r[3];if(valid(weapon,0x228)){static std::map<uint32_t,uint32_t> states;const auto state=mem_read32(weapon+0x1B4);if(!states.contains(weapon) || states[weapon]!=state){states[weapon]=state;const auto actor=mem_read32(weapon+0x1D4);std::string detail;if(valid(actor,0x790))detail=" slot="+std::to_string(mem_read32(actor+0x17C))+" owned="+std::to_string(mem_read32(actor+0x4CC+6*24))+" active="+std::to_string(mem_read32(actor+0x4C8+6*24));log_message("[coop-rocket-fire-state] weapon="+std::to_string(weapon)+" state="+std::to_string(state)+" mode="+std::to_string(mem_read32(weapon+0x1E0))+detail);}}ssne_call_original(ctx,0x8005CDEC,diagnostic_rocket_original);}
// This native movement response reads bounding radii directly, bypassing the
// sphere-count getter. Reserved actors must not push either side of the pair.
void occupied_contact(PPCContext& ctx){
    if(!native_party_actor_occupied(ctx.r[4]) || !native_party_actor_occupied(ctx.r[5]))return;
    ssne_call_original(ctx,0x800A7AB0,occupied_contact_original);
}
void occupied_spheres(PPCContext& ctx){if(!native_party_actor_occupied(ctx.r[3])){ctx.r[3]=0;return;}ssne_call_original(ctx,0x8009C410,occupied_spheres_original);}
// Enemy target selector 80095EE8 calls player vtable +BC (800B7618),
// which reads +340 directly instead of the health getter. Capacity alone
// must not make a reserved player eligible. Preserve all original state.
// The original selector multiplies each candidate's distance score by the
// enemy model's per-viewport render visibility. Authoritative coop renders
// only the local viewport: remote caches stay0 (also all original slots>=4).
// Neutralize that presentation-only bonus equally for EVERY candidate during
// the original selector. Retain native distance, target-load distribution,
// occupancy and activity rules; do not alter the model's rendering cache.
PPCFunc occupied_target_original=nullptr,occupied_target_visibility_original=nullptr;
thread_local unsigned authoritative_target_depth=0;
void occupied_target_visibility(PPCContext& ctx){
    if(authoritative_target_depth && ctx.lr==0x80096068u){ctx.ps0[1]=ctx.ps1[1]=0.;return;}
    ssne_call_original(ctx,0x80129E00,occupied_target_visibility_original);
}
void occupied_target(PPCContext& ctx){
    const bool shared=occupancy_enabled && !replica_simulation() && mem_read32(game_address(0x802DB0E0))==4 && mem_read32(0x802DB8CC)==1;
    struct Scope{bool active;explicit Scope(bool value):active(value){if(active)++authoritative_target_depth;}~Scope(){if(active)--authoritative_target_depth;}} scope(shared);
    ssne_call_original(ctx,0x80095EE8,occupied_target_original);
}
PPCFunc occupied_bomb_original=nullptr;
void occupied_bomb(PPCContext& ctx){if(ctx.r[4] && !native_party_actor_occupied(ctx.r[3]))return;ssne_call_original(ctx,0x800B4B28,occupied_bomb_original);}
void occupied_active(PPCContext& ctx){if(!native_party_actor_occupied(ctx.r[3])){ctx.r[3]=0;return;}ssne_call_original(ctx,0x800B7618,occupied_active_original);}
void occupied_health(PPCContext& ctx){if(!native_party_actor_occupied(ctx.r[3])){ctx.ps0[1]=ctx.ps1[1]=0;return;}ssne_call_original(ctx,0x8009DBCC,occupied_health_original);}
void occupied_render(PPCContext& ctx){if(!native_party_actor_occupied(ctx.r[3]))return;ssne_call_original(ctx,0x800B2568,occupied_render_original);}
}
void native_party_install_occupancy_hooks(){
    if(occupied_spheres_original)return;
    const auto* spheres=ssne_find_func(0x8009C410);const auto* health=ssne_find_func(0x8009DBCC);const auto* render=ssne_find_func(0x800B2568);
    if(!spheres || !health || !render || mem_read32(spheres->address)!=0x38600001u || mem_read32(health->address)!=0xC02300CCu || mem_read32(render->address)!=0x9421FFF8u)fatal("Native player participation hooks unavailable for this revision.");
    const auto* contact=ssne_find_func(0x800A7AB0);
    if(!contact || mem_read32(contact->address)!=0x9421FEF8u)fatal("Native player contact hook unavailable for this revision.");
    occupied_contact_original=contact->func;ssne_install_hook(contact->address,occupied_contact);
    const auto* active=ssne_find_func(0x800B7618);
    if(!active || mem_read32(active->address)!=0x80030340u)fatal("Native player activity hook unavailable for this revision.");
    occupied_active_original=active->func;ssne_install_hook(active->address,occupied_active);
    const auto* target=ssne_find_func(0x80095EE8);const auto* visibility=ssne_find_func(0x80129E00);
    if(!target || !visibility || mem_read32(target->address)!=0x9421FF90u || mem_read32(visibility->address)!=0x28040003u)fatal("Native enemy target perception unavailable for this revision.");
    occupied_target_original=target->func;occupied_target_visibility_original=visibility->func;
    ssne_install_hook(target->address,occupied_target);ssne_install_hook(visibility->address,occupied_target_visibility);
    const auto* bomb=ssne_find_func(0x800B4B28);
    if(!bomb || mem_read32(bomb->address)!=0x9421FFF0u)fatal("Native versus bomb assignment unavailable.");
    occupied_bomb_original=bomb->func;ssne_install_hook(bomb->address,occupied_bomb);
    occupied_spheres_original=spheres->func;occupied_health_original=health->func;occupied_render_original=render->func;
    ssne_install_hook(spheres->address,occupied_spheres);ssne_install_hook(health->address,occupied_health);ssne_install_hook(render->address,occupied_render);
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){
        const auto* effect=ssne_find_func(0x8009DD48);const auto* rocket=ssne_find_func(0x8005CDEC);
        if(!effect || !rocket || mem_read32(effect->address)!=0x9421FFF0u || mem_read32(rocket->address)!=0x9421FF80u)fatal("Native projectile diagnostic signatures unavailable.");
        diagnostic_effect_original=effect->func;diagnostic_rocket_original=rocket->func;ssne_install_hook(effect->address,diagnostic_effect);ssne_install_hook(rocket->address,diagnostic_rocket);
    }
}
uint32_t native_party_memory_size(){return extended_memory || (configured && roster>4)?0x04000000:0x01800000;}
void native_party_extended_memory(bool enabled){extended_memory=enabled;}
void native_party_prepare_lobby(PPCContext& context) {
    if(roster<=2 || profiles_prepared)return;
    profiles_prepared=true;
    const auto capacity=mem_read32(context.r[13]-0x78BC);
    log_message("[native-party-profiles] capacity="+std::to_string(capacity));
    auto invoke=[&](uint32_t function,uint32_t a,uint32_t b=0,uint32_t c=0) {
        PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;
        mem_write32(call.r[1],context.r[1]);call.r[3]=a;call.r[4]=b;call.r[5]=c;
        ssne_call(call,function);return call.r[3];
    };
    std::set<uint32_t> assigned;
    for(unsigned slot=0;slot<roster;++slot) {
        uint32_t id=slot<2?invoke(0x800C25A4,slot):~0u;
        if(slot<2 && id==~0u)id=slot;
        if(slot>=2) {
            const auto name=std::string("Netplay Player ")+std::to_string(slot+1);
            uint32_t node=0;
            // Reuse an available native profile before allocating another one.
            // Default installations already contain separate character profiles.
            for(unsigned candidate=0;candidate<capacity;++candidate) {
                if(assigned.count(candidate))continue;
                node=invoke(0x800C23A8,candidate);
                if(node)break;
            }
            for(unsigned index=0;!node && index<capacity;++index) {
                const auto candidate=invoke(0x800C226C,index);
                const auto variable=invoke(0x801137B8,candidate,0x802134A0);
                const auto pointer=invoke(0x80159F70,variable+0x24);
                std::string value;
                if(valid(pointer,128))for(unsigned i=0;i<127 && mem_read8(pointer+i);++i)value+=char(mem_read8(pointer+i));
                if(value==name){node=candidate;break;}
            }
            if(!node) {
                if(!invoke(0x800C26BC,0))fatal("No free native player profile for additional netplay participants.");
                const auto name_pointer=context.r[1]-0xC0;
                for(unsigned i=0;i<=name.size();++i)mem_write8(name_pointer+i,i<name.size()?uint8_t(name[i]):0);
                node=invoke(0x800C2414,name_pointer,0,slot%2);
            }
            const auto variable=invoke(0x80113728,node,0x802134FC);
            id=mem_read32(variable+0x24);
        }
        profiles[slot]=id;
        assigned.insert(id);
        if(slot<4)invoke(0x800C253C,slot,id,slot);
        log_message("[native-party-profile] slot="+std::to_string(slot)+" profile="+std::to_string(id));
    }
    for(unsigned slot=roster;slot<4;++slot)invoke(0x800C2650,slot);
}
bool native_party_extended(){return roster>4 && arena;}
unsigned native_party_capacity(){return native_party_extended()?roster:4;}
uint32_t native_party_registry(){return registry && native_party_extended()?registry:0x802D9048;}
uint32_t native_party_loadouts(){return native_party_extended()?arena+0x2C00:0x802D6918;}
void native_party_reset_extra_loadouts() {
    if(!configured || roster<=4)return;
    for(auto& record:extra_loadouts){record.fill(0);record[0]=2;record[1]=1;}
    extra_loadouts_initialized=true;
}
uint32_t native_party_ranking(){return arena && native_party_extended()?arena+0x2080:0x802D906C;}
unsigned native_party_view_last(){return arena && native_party_extended()?31:15;}
uint32_t native_party_controller_offset(uint32_t slot) {
    if(slot<4 || slot>=0x80000000u || !native_party_extended())return slot*0x70C;
    if(slot>=roster || !second_bank)fatal("Additional native controller bank is unavailable.");
    return second_bank-original_bank+(slot-4)*0x70C;
}
void native_party_controller_channel(PPCContext& ctx) {
    if(!native_party_extended() || ctx.r[4]<4 || ctx.r[4]>=roster)return;
    if(ctx.r[3]!=original_bank)fatal("Unexpected native party rumble-bank receiver.");
    ctx.r[3]=second_bank;ctx.r[4]-=4;
}
uint32_t native_party_view_engine(uint32_t engine) {
    if(native_party_extended() && views && engine!=engine_base && getenv("SSNE_NATIVE_PARTY_TRACE")) {
        static std::set<uint32_t> seen;
        if(seen.insert(engine).second)log_message("[native-party-view-engine] actual="+std::to_string(engine)+" expected="+std::to_string(engine_base)+" owner="+std::to_string(owner));
    }
    return native_party_extended() && views && engine==engine_base?views-0xDC:engine;
}
int native_party_owner(){return owner;}
void native_party_owner_set(int value){owner=value;}
void native_party_bind_view(uint32_t view,unsigned slot){if(native_party_extended() && slot<roster)view_owners[view]=slot;}
uint32_t native_party_frame_view_mask(uint32_t mask) {
    if(!native_party_extended())return mask;
    const auto players=(1u<<roster)-1;
    auto group=[&](unsigned shift){const auto bits=(mask>>shift)&15u;return bits==15u?players:bits;};
    return group(0)|(group(6)<<8)|(group(10)<<16)|((mask&0x30u)<<20)|((mask&0xC000u)<<12);
}
void native_party_initialize_player_masks(PPCContext& context,uint32_t engine) {
    if(!native_party_extended())return;
    const auto* setter=ssne_find_func(0x8011A2BC);
    if(engine!=engine_base || !setter || mem_read32(setter->address)!=0x1C84004Cu)fatal("Native player model-mask setter unavailable.");
    // 80166D38 explicitly configures only player views 0-3. The original
    // mask includes world models (bit 31) and that player's first-person
    // models. Apply the same native setter to each additional player view.
    const auto previous_owner=owner;
    for(unsigned slot=4;slot<roster;++slot) {
        PPCContext call=context;call.msr&=~0x8000u;
        owner=int(slot);call.r[3]=engine;call.r[4]=slot;call.r[5]=0x80000000u|(1u<<slot);
        ssne_call(call,setter->address);
    }
    owner=previous_owner;
}
uint32_t native_party_frame_camera(uint32_t engine,unsigned slot) {
    if(!native_party_extended() || slot>native_party_view_last() || engine!=engine_base)fatal("Invalid physical native camera lookup.");
    const auto view=views+slot*76;
    return mem_read32(view+0x30)?mem_read32(view):0;
}
uint32_t native_party_view_slot(uint32_t slot) {
    if(!native_party_extended() || !views)return slot;
    if(owner>=0) {
        if(slot==unsigned(owner))return slot;
        if(slot==unsigned(owner)+6)return unsigned(owner)+8;
        if(slot==unsigned(owner)+10)return unsigned(owner)+16;
    }
    if(slot==4 || slot==5)return slot+20;
    if(slot>=6 && slot<=9)return slot+2;
    if(slot>=10 && slot<=13)return slot+6;
    if(slot==14 || slot==15)return slot+12;
    return slot;
}
uint32_t native_party_pickup_flags(uint32_t pickup) {
    if(!native_party_extended() || !arena)return pickup+0x170;
    auto [it,new_entry]=pickup_flags.try_emplace(pickup,0);
    if(new_entry) {
        if(pickup_next+32>arena+0x10000)fatal("Native party pickup storage exhausted.");
        it->second=pickup_next;pickup_next+=32;
        for(unsigned i=0;i<8;++i)mem_write32(it->second+4*i,i<4?mem_read32(pickup+0x170+4*i):0);
    }
    return it->second;
}
uint32_t native_party_pickup_value(uint32_t pickup,unsigned slot) {
    if(slot>=native_party_capacity())return 0;
    if(auto it=pickup_flags.find(pickup);native_party_extended() && it!=pickup_flags.end())return mem_read32(it->second+slot*4);
    return slot<4?mem_read32(pickup+0x170+slot*4):0;
}
bool native_party_can_prepare_pickups(const std::vector<uint32_t>& pickups) {
    if(!native_party_extended())return true;
    std::set<uint32_t> missing;for(auto object:pickups)if(!pickup_flags.count(object))missing.insert(object);
    return missing.size()<=size_t(arena+0x10000-pickup_next)/32;
}
void native_party_reset_pickup(uint32_t pickup) {
    if(auto it=pickup_flags.find(pickup);it!=pickup_flags.end()) {
        for(unsigned slot=0;slot<8;++slot)mem_write32(it->second+slot*4,slot<4?mem_read32(pickup+0x170+slot*4):0);
    }
}
uint64_t native_party_checksum(uint64_t hash) {
    for(const auto& [object,flags]:pickup_flags) {
        bool collected=false;for(unsigned slot=0;slot<8;++slot)collected|=mem_read32(flags+slot*4)!=0;
        if(!collected)continue; // local ESP visits to untouched pickups do not affect simulation checksums
        hash^=object;hash*=1099511628211ull;
        for(unsigned slot=0;slot<8;++slot){hash^=mem_read32(flags+slot*4);hash*=1099511628211ull;}
    }
    return hash;
}
void native_party_set_input(unsigned slot,const PadState& pad){if(slot<8)pads[slot]=pad;}
uint32_t native_party_second_bank(){return second_bank;}
uint32_t native_party_second_status(){return second_status;}
void native_party_sample_second() {
    for(unsigned slot=0;slot<4;++slot) {
        const auto output=second_status+12*slot;
        for(unsigned i=0;i<12;++i)mem_write8(output+i,0);
        if(slot+4>=roster){mem_write8(output+10,0xff);continue;}
        auto pad=pads[slot+4];pad.buttons|=injected_buttons(int(slot+4));mem_write16(output,pad.buttons&0x1F7F);
        mem_write8(output+2,uint8_t(int(pad.stick_x)-128));mem_write8(output+3,uint8_t(int(pad.stick_y)-128));
        mem_write8(output+4,uint8_t(int(pad.cstick_x)-128));mem_write8(output+5,uint8_t(int(pad.cstick_y)-128));
        mem_write8(output+6,uint8_t(std::max(0,int(pad.trigger_l)-31)));mem_write8(output+7,uint8_t(std::max(0,int(pad.trigger_r)-31)));
    }
}
void native_party_begin(PPCContext& context) {
    if(roster<=4)return;
    PPCContext call=context;call.msr&=~0x8000u;
    arena_heap=mem_read32(context.r[13]-0x6DE0);
    call.r[3]=arena_heap;call.r[4]=0x10000;
    ssne_call(call,0x80116084);arena=call.r[3];
    if(!valid(arena,0x10000))fatal("Cannot allocate native party state.");
    arena_size=mem_read32(arena-0x18);
    mem_write32(context.r[13]-0x6DF0,mem_read32(context.r[13]-0x6DF0)-arena_size);
    for(unsigned i=0;i<0x10000;i+=4)mem_write32(arena+i,0);
    original_bank=mem_read32(0x802ABA9C);engine_base=mem_read32(0x802ABAA0);
    second_bank=arena;second_status=arena+0x2000;registry=arena+0x2040;views=arena+0x2200;pickup_next=arena+0x2D80;
    if(!extra_loadouts_initialized)native_party_reset_extra_loadouts();
    for(unsigned slot=0;slot<8;++slot)for(unsigned word=0;word<11;++word)
        mem_write32(native_party_loadouts()+slot*44+word*4,slot<4?mem_read32(0x802D6918+slot*44+word*4):extra_loadouts[slot-4][word]);
    pickup_flags.clear();
    view_owners.clear();known_player_actors.fill(0);pending_spawns.clear();prepared_spawns.clear();
    call=context;call.msr&=~0x8000u;call.r[3]=second_bank;ssne_call(call,0x8014F750);
    for(unsigned slot=0;slot<4;++slot)mem_write32(second_bank+slot*0x70C,1);
    owner=-1;
    for(unsigned slot=0;slot<16;++slot) {
        const auto destination=native_party_view_slot(slot);
        for(unsigned off=0;off<76;off+=4)mem_write32(views+destination*76+off,mem_read32(engine_base+0xDC+slot*76+off));
    }
    char line[160];std::snprintf(line,sizeof(line),"[native-party] roster=%u arena=%08X controllers=%08X,%08X registry=%08X views=%08X",roster,arena,original_bank,second_bank,registry,views);log_message(line);
}
void native_party_end(PPCContext& context) {
    if(!arena)return;
    for(unsigned slot=0;slot<8;++slot)for(unsigned word=0;word<11;++word) {
        const auto value=mem_read32(native_party_loadouts()+slot*44+word*4);
        if(slot<4)mem_write32(0x802D6918+slot*44+word*4,value);
        else extra_loadouts[slot-4][word]=value;
    }
    owner=-1;
    // Weapon/HUD views belong to the level's eight players. Their canonical
    // reservation bits must not survive into the original main-menu camera
    // allocator (6..15), which otherwise returns an uninitialized camera ID.
    mem_write32(0x802ABAD0u,mem_read32(0x802ABAD0u)&~0x3FC0u);
    for(unsigned slot=8;slot<24;++slot)mem_write32(views+slot*76+0x30,0);
    for(unsigned slot=0;slot<16;++slot) {
        const auto source=native_party_view_slot(slot);
        for(unsigned off=0;off<76;off+=4)mem_write32(engine_base+0xDC+slot*76+off,mem_read32(views+source*76+off));
    }
    if(mem_read32(arena-0x14)) {
        PPCContext call=context;call.msr&=~0x8000u;call.r[3]=arena_heap;call.r[4]=arena;
        ssne_call(call,0x801161EC);
        mem_write32(context.r[13]-0x6DF0,mem_read32(context.r[13]-0x6DF0)+arena_size);
    }
    arena=second_bank=second_status=registry=views=pickup_next=0;
    if(leaving_network){roster=2;leaving_network=false;}
    pickup_flags.clear();view_owners.clear();pending_spawns.clear();prepared_spawns.clear();
}
void native_party_trace(const char* phase,unsigned slot,uint32_t actor) {
    if(!getenv("SSNE_NATIVE_PARTY_TRACE"))return;
    const auto manager=mem_read32(0x802DB780);
    char line[180];std::snprintf(line,sizeof(line),"[native-party-factory] phase=%s slot=%u actor=%08X files=%08X/%08X",phase,slot,actor,manager,valid(manager,4)?mem_read32(manager):0);log_message(line);
}
NativePartyOwnerScope::NativePartyOwnerScope(PPCContext& ctx):previous(owner) {
    if(!native_party_extended())return;
    int slot=actor_slot(ctx.r[3]);if(slot<0)slot=actor_slot(ctx.r[4]);
    if(slot<0)if(auto it=view_owners.find(ctx.r[3]);it!=view_owners.end())slot=int(it->second);
    // Destructor calls can still carry the player vtable after Removing has
    // invalidated its lifetime. A temporary owner scope must not resurrect it.
    // Actor bindings are recorded by spawn preparation/completion instead.
    if(slot>=0)owner=slot;
}
NativePartyOwnerScope::~NativePartyOwnerScope(){owner=previous;}
NativePartySpawnScope::NativePartySpawnScope(PPCContext& ctx) {
    if((!native_party_extended() && !force_safe_spawn) || mem_read32(ctx.r[13]-0x69D4)!=1)return;
    context=&ctx;count_address=ctx.r[13]-0x69D0;previous_count=mem_read32(count_address);previous_msr=ctx.msr;
    slot=ctx.r[4];position=ctx.r[5];rotation=ctx.r[6];ctx.r[4]=0;ctx.msr&=~0x8000u;mem_write32(count_address,1);
}
bool NativePartySpawnScope::prepare() {
    if(prepared)return successful;prepared=true;
    if(!context)return true;
    const auto actor=slot<roster?known_player_actors[slot]:0;
    if(slot>=roster || !valid(position,12) || !valid(rotation,12) || !native_player_collision_radius(actor)){probe_error="Native spawn output unavailable";successful=false;return false;}
    std::array<float,3> base{},chosen{};for(unsigned i=0;i<3;++i)base[i]=u2f(mem_read32(position+i*4));
    if(auto found=prepared_spawns.find(actor);found!=prepared_spawns.end() && !probing_spawn)chosen=found->second;
    else {
        std::string error;successful=choose_coop_spawn(*context,actor,base,chosen,error);
        if(!successful){probe_error=error;return false;}
        prepared_spawns[actor]=chosen;
    }
    if(probing_spawn && actor==probing_actor)probe_success=true;
    for(unsigned i=0;i<3;++i)mem_write32(position+i*4,f2u(chosen[i]));return true;
}
NativePartySpawnScope::~NativePartySpawnScope() {
    if(!context)return;mem_write32(count_address,previous_count);context->msr=previous_msr;
}

}
#include "native_party_state.inc"
