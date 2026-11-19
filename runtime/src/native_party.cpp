#include "ssne/native_party.h"
#include "ssne/connected_camera_updates.h"
#include "ssne/native_results.h"
#include "ssne/native_cutscene.h"
#include <cstring>
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
const void* camera_update_gate_owner=nullptr;
std::function<bool()> camera_update_gate;
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
// PAL's wheeled-vehicle camera has two four-sample histories and one 3x4
// output matrix. The console stores all three in one global block.
constexpr uint32_t wheeled_camera_history=0x802ACD80u;
struct DriverCamera {
    uint32_t car=0;
    std::array<uint8_t,0x90> history{};
};
std::array<DriverCamera,8> driver_cameras{};
std::array<uint32_t,8> known_player_actors{};
struct PendingSpawn {unsigned request=0,flags=0,attempts=0;bool local=true,force_safe=false,full_respawn=false;std::chrono::steady_clock::time_point next{};};
std::map<uint32_t,PendingSpawn> pending_spawns;
std::map<uint32_t,std::array<float,3>> prepared_spawns;
std::map<uint32_t,uint64_t> completed_spawns;
uint64_t next_spawn_revision=0;
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
PPCFunc sphere_move_original = nullptr;
PPCFunc ground_collision_original = nullptr;
bool trace_sphere_queries = false;
struct RestingSweep {
    PPCContext* context = nullptr;
    uint32_t start = 0, destination = 0;
};
thread_local RestingSweep resting_sweep;

bool static_contacts(uint32_t hits, uint32_t count) {
    if (count > 20 || !valid(hits, count * 48)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto hit = hits + i * 48;
        const auto kind = mem_read32(hit + 36);
        if (kind == 3) continue; // Touch fields have no solid response.
        // World geometry and placed level objects. Animated brushes, bounce
        // pads, actors and contacts with their own motion keep the solver result.
        if ((kind != 0 && kind != 4) || (mem_read32(hit + 32) & 0x80000000u)) return false;
    }
    return true;
}

void native_sphere_move(PPCContext& ctx) {
    static unsigned samples = 0;
    const bool trace = trace_sphere_queries && samples++ < 24;
    const auto from = ctx.r[4], output = ctx.r[6], hits = ctx.r[7];
    // Only the first world sweep inside this particular player's actorMoveSphere.
    const bool at_rest = resting_sweep.context == &ctx && ctx.lr == 0x800C065Cu &&
                         from == resting_sweep.start && output == resting_sweep.destination;
    if (trace) {
        std::string line="[coop-native-sphere-call] caller="+std::to_string(ctx.lr)+" radius="+std::to_string(ctx.ps0[1]);
        for(unsigned i=3;i<=10;++i)line+=" r"+std::to_string(i)+"="+std::to_string(ctx.r[i]);
        for(unsigned reg:{4u,5u})if(valid(ctx.r[reg],12))line+=" p"+std::to_string(reg)+"="+std::to_string(u2f(mem_read32(ctx.r[reg])))+","+std::to_string(u2f(mem_read32(ctx.r[reg]+4)))+","+std::to_string(u2f(mem_read32(ctx.r[reg]+8)));
        log_message(line);
    }
    ssne_call_original(ctx, 0x80140620u, sphere_move_original);
    if (at_rest && static_contacts(hits, ctx.r[3])) {
        // The only motion was our ground probe. Its separation bias, including
        // any lateral part on an incline, is not locomotion. Leave the contacts
        // intact. PAL now does its normal support sweep here to choose the height.
        for (unsigned axis = 0; axis < 12; axis += 4)
            mem_write32(output + axis, mem_read32(from + axis));
    }
    if(trace && valid(output,12))log_message("[coop-native-sphere-result] contacts="+std::to_string(ctx.r[3])+" output="+std::to_string(u2f(mem_read32(output)))+","+std::to_string(u2f(mem_read32(output+4)))+","+std::to_string(u2f(mem_read32(output+8))));
}

void ground_player_collision(PPCContext& ctx) {
    const auto actor = ctx.r[4], down = ctx.r[6], start = ctx.r[7];
    const auto motion = ctx.r[8], additional = ctx.r[9], destination = ctx.r[10];
    const auto original = [&] { ssne_call_original(ctx, 0x800C04ACu, ground_collision_original); };
    if (ctx.lr != 0x800A92A8u || actor_slot(actor) < 0 || !valid(down, 12) ||
        !valid(start, 12) || !valid(motion, 12) || !valid(additional, 12) || !valid(destination, 12)) {
        original();
        return;
    }
    std::array<uint32_t, 3> saved;
    std::array<float, 3> gravity;
    double length2 = 0;
    bool zero_request = true;
    for (unsigned i = 0; i < 3; ++i) {
        saved[i] = mem_read32(motion + 4*i);
        const float request = u2f(saved[i]), carry = u2f(mem_read32(additional + 4*i));
        const float pos = u2f(mem_read32(start + 4*i));
        gravity[i] = u2f(mem_read32(down + 4*i));
        if (!std::isfinite(pos) || pos + (request + carry) != pos) {
            original();
            return;
        }
        zero_request &= request == 0.f && carry == 0.f;
        length2 += double(gravity[i]) * gravity[i];
    }
    if (!(length2 > .5 && length2 < 1.5)) {
        original();
        return;
    }
    // PAL zeroes the movement vector itself after braking. Leave actual moves,
    // pending steps, deliberate slides, attached platforms and jump pads alone.
    const bool resting = zero_request && mem_read32(ctx.r[1] + 8) && mem_read32(actor + 0x6D0) &&
        !mem_read32(actor + 0x6E4) && !mem_read32(actor + 0x330) && !mem_read32(actor + 0x704) &&
        !mem_read32(actor + 0x6FC) && u2f(mem_read32(actor + 0x708)) == 0.f;

    struct Restore {
        RestingSweep previous;
        uint32_t motion;
        std::array<uint32_t, 3> saved;
        ~Restore() {
            resting_sweep = previous;
            for (unsigned i = 0; i < 3; ++i) mem_write32(motion + 4*i, saved[i]);
        }
    } restore{resting_sweep, motion, saved};
    resting_sweep = resting ? RestingSweep{&ctx, start, destination} : RestingSweep{};
    // Keep PAL's contact callbacks and avoid a degenerate zero-length sweep.
    const float distance = .001f / float(std::sqrt(length2));
    for (unsigned i = 0; i < 3; ++i)
        mem_write32(motion + 4*i, f2u(u2f(saved[i]) + gravity[i]*distance));
    original();
}

void install_sphere_move() {
    if (sphere_move_original) return;
    const auto* entry = ssne_find_func(0x80140620u);
    if (!entry || mem_read32(entry->address) != 0x9421FDF8u) fatal("Unsupported PAL sphere move");
    sphere_move_original = entry->func;
    ssne_install_hook(entry->address, native_sphere_move);
}
}
void native_party_install_geometry_trace() {
    if (getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_SPAWN_PROBE")) {
        trace_sphere_queries = true;
        install_sphere_move();
    }
}
void native_party_install_ground_collision() {
    install_sphere_move();
    const auto* entry = ssne_find_func(0x800C04ACu);
    if (!entry || mem_read32(entry->address) != 0x9421FE98u) fatal("Unsupported native player collision routine");
    ground_collision_original = entry->func;
    ssne_install_hook(entry->address, ground_player_collision);
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
        found.flags=mem_read32(result+32);hit=found;
    }
    restore();
    if(hit)for(auto point:{hit->position,hit->normal})for(float value:point)if(!std::isfinite(value)){hit.reset();error="Invalid native geometry result";return false;}
    return true;
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
    // Original PlayerStart may destroy/recreate the body at the same proxy
    // address. Its original self-hidden mask supersedes any inactive/death
    // mask saved before the completed spawn, even when that address is reused.
    reserved_body_masks.erase(actor);
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
namespace {
bool full_respawn_code(){
    if(mem_read32(0x80000000u)!=0x47334250u)return false;
    uint64_t hash=14695981039346656037ull;
    for(uint32_t i=0;i<0x4C4u;++i){hash^=mem_read8(0x800B2950u+i);hash*=1099511628211ull;}
    return hash==0x408BC930E2A95916ull;
}
}
bool native_party_request_respawn(PPCContext& context,uint32_t actor,std::string& error){
    error.clear();const auto* reset=ssne_find_func(0x800B2950u);
    const auto slot=actor_slot(actor);
    if(replica_simulation() || !native_player_collision_radius(actor) || slot<0 ||
       mem_read32(native_party_registry()+unsigned(slot)*4)!=actor ||
       (occupancy_enabled && !(connected_slots&(1u<<unsigned(slot)))) ||
       mem_read32(0x802DB8CCu)!=1 || mem_read32(0x802DB7E8u)!=4 || !reset || !full_respawn_code() ||
       context.r[2]!=0x802F22A0u || context.r[13]!=0x802E22A0u ||
       context.r[1]<0x80004000u || context.r[1]>guest_ram_end()-4){
        error="Native full respawn context, owner or PAL revision unavailable";return false;
    }
    const auto camera=mem_read32(actor+0x368);
    if(!valid(camera,0xD8) || mem_read32(camera)!=0x80210880u || mem_read32(camera+4)!=actor){
        error="Native full respawn camera binding unavailable";return false;
    }
    const auto before=native_party_spawn_revision(actor);
    std::vector<uint8_t> stack(0x4000);std::memcpy(stack.data(),host_ptr(context.r[1]-0x4000),stack.size());
    const auto previous_force=force_safe_spawn;force_safe_spawn=true;
    auto restore=[&]{force_safe_spawn=previous_force;std::memcpy(host_ptr(context.r[1]-0x4000),stack.data(),stack.size());};
    try{
        // Validate placement before resetting health, model/camera or inventory.
        // A blocked attempt keeps its original dead/inactive state until the
        // pending retry can run this full lifecycle at a safe native point.
        if(!native_party_prepare_spawn(context,actor,1)){
            if(auto pending=pending_spawns.find(actor);pending!=pending_spawns.end())pending->second.full_respawn=true;
            restore();error="Native full respawn is waiting for a free spawn point";return false;
        }
        PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=0;
        ssne_call(call,reset->address);
    }catch(...){prepared_spawns.erase(actor);restore();throw;}
    restore();
    const auto health=u2f(mem_read32(actor+0xCC));
    if(native_party_spawn_revision(actor)>before && !native_party_spawn_waiting(actor) &&
       std::isfinite(health) && health>0.f && !mem_read32(actor+0x328) && !mem_read32(actor+0x370) &&
       mem_read32(actor+0x368)==camera && !mem_read32(camera+0xAC)){
        log_message("[coop-native-full-respawn-completed] slot="+std::to_string(slot)+" actor="+std::to_string(actor)+" revision="+std::to_string(native_party_spawn_revision(actor)));return true;
    }
    error="Native full respawn did not restore a living player and first-person camera";return false;
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
    if(pending->second.full_respawn){
        std::string error;
        if(!native_party_request_respawn(context,actor,error) && !native_party_spawn_waiting(actor))log_message("[coop-native-full-respawn-error] "+error);
        return native_party_spawn_waiting(actor);
    }
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
void native_party_leave_network(){driver_cameras={};camera_update_gate={};camera_update_gate_owner=nullptr;configured=false;occupancy_enabled=false;profiles_prepared=false;leaving_network=true;pads={};owner=-1;if(!arena){roster=2;leaving_network=false;}}
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

constexpr uint32_t secondary_player_query=0x800B6630u;
constexpr std::array<uint32_t,12> secondary_player_signature{
    0x80030340u,0x2C000000u,0x41820018u,0x2C000003u,
    0x41820010u,0x80030370u,0x2C000000u,0x4182000Cu,
    0x38600000u,0x4E800020u,0x38600001u,0x4E800020u};
PPCFunc secondary_player_previous=nullptr;
bool secondary_player_filter_enabled(){
    static const bool enabled=[] {const auto* value=std::getenv("SSNE_SECONDARY_PLAYER_FILTER");return !value || std::strcmp(value,"0");}();
    return enabled;
}
bool secondary_player_client_filter_enabled(){
    static const bool enabled=[] {const auto* value=std::getenv("SSNE_SECONDARY_PLAYER_CLIENT_FILTER");return !value || std::strcmp(value,"0");}();
    return enabled;
}
// Only the scene manager's extra weapon/aim update is filtered. In particular,
// leave the original predicate intact for scripts, transitions and replicas.
bool secondary_player_reserved(uint32_t actor){
    if(!configured || !occupancy_enabled || replica_simulation() || netplay_local_player()!=0 ||
       mem_read32(0x80000000u)!=0x47334250u ||
       mem_read32(game_address(0x802DB0E0u))!=4 || mem_read32(0x802DB8CCu)!=1)return false;
    const auto slot=actor_slot(actor);const auto reg=native_party_registry();
    if(slot<0 || !valid(reg,roster*4u) || mem_read32(reg+unsigned(slot)*4u)!=actor)return false;
    // Other native actor states retain their exact predicate behavior.
    if(mem_read32(actor+0x340u)!=1 || mem_read32(actor+0x370u)!=0)return false;
    return !native_party_actor_occupied(actor);
}
// Replica reservations receive no authoritative weapon state. Filter only a
// fully ready local world, keeping every transition and uncertain actor native.
bool secondary_player_client_reserved(uint32_t actor){
    if(!secondary_player_client_filter_enabled() || !configured || !occupancy_enabled || !replica_simulation())return false;
    const auto local=netplay_local_player();
    if(local<=0 || unsigned(local)>=roster || !netplay_world_ready() ||
       mem_read32(0x80000000u)!=0x47334250u ||
       mem_read32(game_address(0x802DB0E0u))!=4 || mem_read32(game_address(0x802DB0E4u))!=0 ||
       mem_read32(game_address(0x802DB1BCu))!=1 || !prepared_spawns.empty() || pending_spawns.size()>roster ||
       native_results_presenting() || native_cutscene_presenting() || native_movie_presenting() ||
       local_information_open() || local_pause_menu_open())return false;
    const auto players=(1u<<roster)-1u;
    if((occupied_slots&~players) || (connected_slots&~players) || occupied_slots!=connected_slots ||
       !(connected_slots&1u) || !(connected_slots&(1u<<unsigned(local))))return false;
    const auto reg=native_party_registry();
    if(!valid(reg,roster*4u))return false;
    const auto self=mem_read32(reg+unsigned(local)*4u);
    if(actor_slot(self)!=local || mem_read32(self+0x340u)!=1 || mem_read32(self+0x370u)!=0)return false;
    // Disconnected reservations can remain pending indefinitely. Keep their
    // pending state; only a fresh canonical unconnected reservation is safe.
    for(const auto& entry:pending_spawns){
        const auto pending=entry.first;const auto slot=actor_slot(pending);
        if(slot<0 || mem_read32(reg+unsigned(slot)*4u)!=pending ||
           ((connected_slots|occupied_slots)&(1u<<unsigned(slot))))return false;
    }
    const auto slot=actor_slot(actor);
    if(slot<0 || mem_read32(reg+unsigned(slot)*4u)!=actor ||
       mem_read32(actor+0x340u)!=1 || mem_read32(actor+0x370u)!=0)return false;
    const auto bit=1u<<unsigned(slot);
    return !(occupied_slots&bit) && !(connected_slots&bit);
}
void secondary_player_trace(const PPCContext& ctx,uint32_t actor,bool skipped){
    if(ctx.lr!=0x801511B8u || !configured || !occupancy_enabled ||
       mem_read32(game_address(0x802DB0E0u))!=4 || ctx.r[13]!=0x802E22A0u)return;
    // Cold replica Info/pause transitions must not exhaust the bounded trace
    // before the original local view is ready. This never changes the filter.
    if(replica_simulation() && (!netplay_world_ready() || local_information_open() || local_pause_menu_open() ||
       native_cutscene_presenting() || native_movie_presenting() || native_results_presenting()))return;
    struct Frame {uint32_t id=0;unsigned forwarded=0,skipped=0,forwarded_mask=0,skipped_mask=0,lines=0;bool started=false;};
    static Frame frame;
    if(frame.lines>=32)return;
    const auto id=mem_read32(ctx.r[13]-0x6D80u);
    if(frame.started && id!=frame.id){
        char line[240];std::snprintf(line,sizeof(line),"[secondary-player-filter] frame=%u forwarded=%u skipped=%u forwarded_mask=%02X skipped_mask=%02X occupied=%02X connected=%02X enabled=%u",frame.id,frame.forwarded,frame.skipped,frame.forwarded_mask,frame.skipped_mask,unsigned(occupied_slots),unsigned(connected_slots),unsigned(secondary_player_filter_enabled()));
        log_message(line);const auto lines=frame.lines+1;frame={};frame.lines=lines;
        if(frame.lines>=32)return;
    }
    frame.started=true;frame.id=id;
    const auto slot=actor_slot(actor);const auto reg=native_party_registry();
    const auto mask=slot>=0 && valid(reg,roster*4u) && mem_read32(reg+unsigned(slot)*4u)==actor ? 1u<<slot:0u;
    if(skipped){++frame.skipped;frame.skipped_mask|=mask;}
    else{++frame.forwarded;frame.forwarded_mask|=mask;}
}
void secondary_player_predicate(PPCContext& ctx){
    const auto actor=ctx.r[3];
    const bool skip=ctx.lr==0x801511B8u && secondary_player_filter_enabled() &&
        (secondary_player_reserved(actor) || secondary_player_client_reserved(actor));
    static const bool trace=[] {const auto* hidden=std::getenv("SSNE_TEST_HIDDEN");const auto* value=std::getenv("SSNE_TEST_SECONDARY_PLAYER_TRACE");return hidden && std::strcmp(hidden,"0") && value && !std::strcmp(value,"1");}();
    if(trace)secondary_player_trace(ctx,actor,skip);
    if(skip){ctx.r[3]=0;return;}
    ssne_call_original(ctx,secondary_player_query,secondary_player_previous);
}
void install_secondary_player_filter(){
    if(secondary_player_previous)return;
    const auto* query=ssne_find_func(secondary_player_query);
    if(!query || !query->func || mem_read32(0x80000000u)!=0x47334250u)return;
    for(unsigned i=0;i<secondary_player_signature.size();++i)
        if(mem_read32(secondary_player_query+i*4u)!=secondary_player_signature[i])return;
    auto* hook=ssne_hook_slot(secondary_player_query);
    if(!hook || *hook==secondary_player_predicate)return;
    secondary_player_previous=*hook?*hook:query->func;
    ssne_install_hook(secondary_player_query,secondary_player_predicate);
}
}
namespace {
PPCFunc vehicle_reference_table_original=nullptr;
PPCFunc vehicle_config_read_original=nullptr;
PPCFunc vehicle_model_view_original=nullptr;
PPCFunc wheeled_enter_original=nullptr,wheeled_follow_original=nullptr,wheeled_view_original=nullptr;
DriverCamera* driving_camera(uint32_t car){
    if(!configured || !valid(car,0x164))return nullptr;
    const auto driver=mem_read32(car+0x160);
    const int slot=actor_slot(driver);
    if(slot<0 || driver_cameras[slot].car!=car || mem_read32(driver+0x6C4)!=car)return nullptr;
    return &driver_cameras[slot];
}
void enter_wheeled_vehicle(PPCContext& c){
    const auto car=c.r[3];
    const int slot=configured?actor_slot(c.r[4]):-1;
    ssne_call_original(c,0x801728B4u,wheeled_enter_original);
    // Enter seeds the native history at this vehicle's current position.
    // An unsuccessful Use must keep the existing driver's saved history.
    if(slot>=0 && c.r[3]){
        auto& camera=driver_cameras[slot];
        camera.car=car;
        std::memcpy(camera.history.data(),host_ptr(wheeled_camera_history),camera.history.size());
    }
}
void follow_wheeled_vehicle(PPCContext& c){
    auto* camera=driving_camera(c.r[3]);
    if(camera)std::memcpy(host_ptr(wheeled_camera_history),camera->history.data(),camera->history.size());
    ssne_call_original(c,0x80173AD0u,wheeled_follow_original);
    if(camera)std::memcpy(camera->history.data(),host_ptr(wheeled_camera_history),camera->history.size());
}
void read_wheeled_vehicle_view(PPCContext& c){
    // The original getter ignores its vehicle and reads only the final
    // matrix. Restore that driver's result before the player's camera reads it.
    if(auto* camera=driving_camera(c.r[3]))
        std::memcpy(host_ptr(wheeled_camera_history+0x60),camera->history.data()+0x60,0x30);
    ssne_call_original(c,0x80173AA8u,wheeled_view_original);
}
std::map<uint32_t,std::vector<uint8_t>> vehicle_configs;
void select_party_vehicle_model(PPCContext& c){
    const auto model=c.r[3],view=c.r[4],car=model-0xCC;
    const auto vt=configured && view && valid(car,0x1D8)?mem_read32(car+0x48):0;
    if(vt==0x80224D30u || vt==0x80224320u || vt==0x80225568u){
        const auto owner=mem_read32(car+0x1D4);
        if(valid(owner,0x94) && mem_read32(owner+0x60)==car && valid(mem_read32(model+0x10),0xB0) &&
           (view>=4 || !valid(mem_read32(model+0x10+view*4),0xB0)))c.r[4]=0;
    }
    ssne_call_original(c,0x80005850u,vehicle_model_view_original);
}
void read_party_vehicle_config(PPCContext& c){
    const auto dest=c.r[3],len=c.r[4],car=c.r[31];
    const auto type=c.lr==0x80172574u && valid(car,0x1F8)?mem_read32(car+0x48):0;
    if(!configured || (type!=0x80224D30u && type!=0x80224320u && type!=0x80225568u) ||
       !len || len>0x2800 || !valid(dest,len)){
        ssne_call_original(c,0x8014E4F0u,vehicle_config_read_original);return;
    }
    // The vehicle parser opens an immutable level-disc file, reads it once
    // and immediately closes it. A late join can instantiate a second car
    // from the guest-frame callback, where waiting for another DVD interrupt
    // would deadlock. Reuse the original bytes, then run the original parser.
    auto& cache=vehicle_configs[type];
    if(cache.size()==len){std::memcpy(host_ptr(dest),cache.data(),len);c.r[3]=len;return;}
    ssne_call_original(c,0x8014E4F0u,vehicle_config_read_original);
    if(c.r[3]==len){
        const auto* data=host_ptr(dest);cache.assign(data,data+len);
    }
}
void reserve_party_vehicle_references(PPCContext& c){
    // The level loader sizes the script table before instantiating any actors.
    if(configured && c.lr==0x8010C998u){
        vehicle_configs.clear();
        if(c.r[3] && c.r[3]<=32760)c.r[3]+=7;
    }
    ssne_call_original(c,0x80151400u,vehicle_reference_table_original);
}
}
void native_party_install_occupancy_hooks(){
    install_secondary_player_filter();
    if(occupied_spheres_original)return;
    const auto* enter=ssne_find_func(0x801728B4u);
    const auto* follow=ssne_find_func(0x80173AD0u);
    const auto* view=ssne_find_func(0x80173AA8u);
    if(enter && follow && view && mem_read32(0x80172918u)==0x396BCD80u &&
       mem_read32(0x80173AF0u)==0x3AA9CD80u && mem_read32(0x80173AB8u)==0x3863CDE0u){
        wheeled_enter_original=enter->func;ssne_install_hook(enter->address,enter_wheeled_vehicle);
        wheeled_follow_original=follow->func;ssne_install_hook(follow->address,follow_wheeled_vehicle);
        wheeled_view_original=view->func;ssne_install_hook(view->address,read_wheeled_vehicle_view);
    }
    const auto* select_model=ssne_find_func(0x80005850u);
    if(select_model && mem_read32(select_model->address)==0x90830050u){
        vehicle_model_view_original=select_model->func;
        ssne_install_hook(select_model->address,select_party_vehicle_model);
    }
    const auto* vehicle_read=ssne_find_func(0x8014E4F0u);
    if(vehicle_read && mem_read32(vehicle_read->address)==0x9421FFE0u){
        vehicle_config_read_original=vehicle_read->func;
        ssne_install_hook(vehicle_read->address,read_party_vehicle_config);
    }
    const auto* reference_table=ssne_find_func(0x80151400u);
    if(reference_table && mem_read32(reference_table->address)==0x9421FFF0u){
        vehicle_reference_table_original=reference_table->func;
        ssne_install_hook(reference_table->address,reserve_party_vehicle_references);
    }
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
bool native_party_render_layout_safe(uint32_t engine){
    return configured && occupancy_enabled && native_party_extended() && arena && valid(arena,0x10000u) &&
        views==arena+0x2200u && engine==engine_base && valid(engine,0x708u) &&
        mem_read32(engine)==0x8021C600u && valid(views,32u*76u);
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
void native_party_set_camera_update_gate(const void* owner,std::function<bool()> gate){
    camera_update_gate_owner=owner;camera_update_gate=owner?std::move(gate):std::function<bool()>{};
}
void native_party_clear_camera_update_gate(const void* owner){
    if(owner==camera_update_gate_owner){camera_update_gate={};camera_update_gate_owner=nullptr;}
}
uint32_t native_party_frame_view_mask(uint32_t mask) {
    int gate_status=-1,inspected_slot=-1;
    auto finish=[&](uint32_t selected,const char* reason){
        static const bool trace=[] {const auto* hidden=std::getenv("SSNE_TEST_HIDDEN");const auto* value=std::getenv("SSNE_TEST_CONNECTED_CAMERA_TRACE");return hidden && std::strcmp(hidden,"0") && value && std::strcmp(value,"0");}();
        if(trace && mask==0xFu){
            struct Last {uint32_t selected=0;uint8_t connected=0,occupied=0;int gate=-1,slot=-1;const char* reason=nullptr;unsigned count=0;};
            static Last last;
            if(last.count<32 && (last.reason!=reason || last.selected!=selected || last.connected!=connected_slots || last.occupied!=occupied_slots || last.gate!=gate_status || last.slot!=inspected_slot)){
                char line[256];std::snprintf(line,sizeof(line),"[connected-camera-updates] selected=%08X connected=%02X occupied=%02X gate=%d slot=%d reason=%s pending=%zu prepared=%zu",selected,unsigned(connected_slots),unsigned(occupied_slots),gate_status,inspected_slot,reason,pending_spawns.size(),prepared_spawns.size());log_message(line);
                last={selected,connected_slots,occupied_slots,gate_status,inspected_slot,reason,last.count+1};
            }
        }
        return selected;
    };
    if(!native_party_extended())return finish(mask,"not_extended");
    const auto players=(1u<<roster)-1;
    auto group=[&](unsigned shift){const auto bits=(mask>>shift)&15u;return bits==15u?players:bits;};
    const uint32_t legacy=group(0)|(group(6)<<8)|(group(10)<<16)|((mask&0x30u)<<20)|((mask&0xC000u)<<12);
    static const bool enabled=[] {const auto* value=std::getenv("SSNE_CONNECTED_CAMERA_UPDATES");return !value || std::strcmp(value,"0");}();
    if(!enabled)return finish(legacy,"reference_disabled");
    if(mask!=0xFu)return legacy;
    if(!configured || !occupancy_enabled)return finish(legacy,"party_or_occupancy_unknown");
    if(replica_simulation())return finish(legacy,"replica");
    if(!(occupied_slots&connected_slots&1u))return finish(legacy,"host_not_connected_ready");
    if((connected_slots&players)==players)return finish(legacy,"all_connected");
    if(!camera_update_gate)return finish(legacy,"session_gate_missing");
    if(!pending_spawns.empty())return finish(legacy,"native_spawn_pending");
    if(!prepared_spawns.empty())return finish(legacy,"native_spawn_prepared");
    try {gate_status=camera_update_gate()?1:0;if(!gate_status)return finish(legacy,"session_transition");}
    catch(...){gate_status=2;return finish(legacy,"session_gate_exception");}
    // This helper is called only by the original guest Frame camera loop.
    // Preserve every transition, unknown class and shared/aliased camera path.
    if(mem_read32(0x80000000u)!=0x47334250u || mem_read16(0x80000004u)!=0x3947u)return finish(legacy,"not_supported_pal");
    if(!netplay_world_ready() || netplay_local_player()!=0)return finish(legacy,"runtime_not_host_ready");
    if(mem_read32(game_address(0x802DB0E0u))!=4 || mem_read32(game_address(0x802DB0E4u))!=0 ||
       mem_read32(game_address(0x802DB1BCu))!=1)return finish(legacy,"native_scene_or_mode_transition");
    if(native_results_presenting() || native_cutscene_presenting() || native_movie_presenting() ||
       local_information_open() || local_pause_menu_open())return finish(legacy,"native_presentation");
    if(!valid(registry,roster*4) || !valid(views,32*76) || !valid(engine_base,0x768) ||
       mem_read32(0x802ABAA0u)!=engine_base || mem_read32(engine_base)!=0x8021C600u)return finish(legacy,"party_storage_or_engine_identity");
    if(mem_read32(0x80210880u+0x14)!=0x800A5EDCu || mem_read16(0x80210880u+0x10)!=0 ||
       mem_read32(0x800A5EDCu)!=0x9421FE30u)return finish(legacy,"camera_callback_identity");
    std::array<camera_updates::Binding,8> bindings{};
    for(unsigned slot=0;slot<roster;++slot){
        inspected_slot=int(slot);
        auto& b=bindings[slot];b.actor=mem_read32(registry+slot*4);b.camera=mem_read32(views+slot*76);
        if(!valid(b.actor,0x790) || !valid(b.camera,0xE0) || mem_read32(views+slot*76+0x30)!=1)return finish(legacy,"actor_camera_range_or_inactive_view");
        b.actor_class=mem_read32(b.actor+0x48);b.slot=mem_read32(b.actor+0x17C);b.actor_camera=mem_read32(b.actor+0x368);
        b.camera_class=mem_read32(b.camera);b.camera_owner=mem_read32(b.camera+4);
        if(!(connected_slots&(1u<<slot))){
            b.mode=mem_read32(b.camera+0xAC);b.shake=mem_read32(b.camera+0x9C);
            b.movement=mem_read32(b.actor+0x340);b.vehicle=mem_read32(b.actor+0x6C4);
            b.frozen=mem_read32(b.actor+0x370);b.dead=mem_read32(b.actor+0x328);b.health=u2f(mem_read32(b.actor+0xCC));
            b.fov=mem_read32(b.actor+0x344);if(!valid(b.fov,0x28))return finish(legacy,"reserved_fov_range");
            b.fov_class=mem_read32(b.fov);if(b.fov_class!=0x80210E80u)return finish(legacy,"reserved_fov_class");
            b.fov_adjust=int16_t(mem_read16(b.fov_class+0x48));b.fov_method=mem_read32(b.fov_class+0x4C);
        }
    }
    inspected_slot=-1;
    const auto selected=camera_updates::select(mask,legacy,roster,true,occupied_slots,connected_slots,guest_ram_end(),bindings);
    return finish(selected,selected==legacy?"binding_or_alias_fallback":"connected_world_cameras");
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
    driver_cameras={};
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
