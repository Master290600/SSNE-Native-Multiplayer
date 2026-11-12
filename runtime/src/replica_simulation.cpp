#include "ssne/replica_simulation.h"
#include "ssne/performance_detail.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/native_entity.h"
#include "ssne/native_party.h"
#include "ssne/native_gait_motion.h"
#include "ssne/native_render_paths.h"
#include "ssne/native_model_update.h"
#include "ssne/native_animation_pose.h"
#include "ssne/game_addresses.h"
#include <atomic>
#include <map>
#include <cmath>
#include <array>
#include <algorithm>
#include <utility>
#include <chrono>
#include <cstdlib>
#include <cstring>
namespace ssne {
namespace {
std::atomic<bool> replica{false};
std::atomic<uint64_t> game_skips{0},world_skips{0};
PPCFunc original_game=nullptr,original_world=nullptr,original_camera=nullptr;
PPCFunc original_bones=nullptr,original_skinning=nullptr;
PPCFunc original_mesh=nullptr;
PPCFunc original_animation_tick=nullptr;
PPCFunc original_level_request=nullptr,original_game_end=nullptr;
bool suppress();
struct EnemyPose {uint32_t object,object_type,resource;std::vector<float> values;int weapon=-1;unsigned index=0;};
std::map<uint32_t,EnemyPose> enemy_poses;
struct HiddenNpcPart {uint32_t object,resource;};
std::map<uint32_t,HiddenNpcPart> hidden_npc_parts;
struct WeaponCameraReference{uint32_t actor=0,proxy=0,matrix=0;std::array<float,12> camera{};std::array<uint32_t,12> root{};};
std::map<uint32_t,WeaponCameraReference> weapon_camera_references;
uint32_t preparing_weapon=0;
PPCFunc original_camera_getter=nullptr;
// Render clones can share a separate native animation master. That master's
// tick also writes held-weapon attachment matrices outside game/world update.
std::multimap<uint32_t,std::pair<uint32_t,uint32_t>> animation_sources;
struct GaitState {uint32_t actor=0,controller=0,model=0;NativeGaitMotion motion;};
std::map<uint32_t,GaitState> gait_states;
void clear_poses(){enemy_poses.clear();animation_sources.clear();weapon_camera_references.clear();gait_states.clear();}
uint64_t pose_subscription=0;
void ensure_pose_observer() {
    if(pose_subscription)return;
    pose_subscription=observe_native_entities([](const NativeEntityNotice& notice) {
        if(notice.event==NativeEntityNotice::Event::LevelExit){clear_poses();hidden_npc_parts.clear();return;}
        if(notice.event==NativeEntityNotice::Event::Removing){
            for(auto it=gait_states.begin();it!=gait_states.end();) {if(it->second.actor==notice.object)it=gait_states.erase(it);else ++it;}
            for(auto it=hidden_npc_parts.begin();it!=hidden_npc_parts.end();)if(it->second.object==notice.object)it=hidden_npc_parts.erase(it);else ++it;
            for(auto it=enemy_poses.begin();it!=enemy_poses.end();) {if(it->second.object==notice.object)it=enemy_poses.erase(it);else ++it;}
            for(auto it=weapon_camera_references.begin();it!=weapon_camera_references.end();) {if(it->second.actor==notice.object)it=weapon_camera_references.erase(it);else ++it;}
        }
    });
}
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;}
void camera_getter_hook(PPCContext& context){
    const auto instance=preparing_weapon,output=context.r[4],caller=context.lr;
    ssne_call_original(context,0x800A6A8Cu,original_camera_getter);
    if(instance && caller==0x800A03CCu && readable(instance,0x228) && readable(output,48)){
        WeaponCameraReference reference;reference.actor=mem_read32(instance+0x1D4);
        for(unsigned i=0;i<12;++i){reference.camera[i]=u2f(mem_read32(output+i*4));if(!std::isfinite(reference.camera[i]))return;}
        ensure_pose_observer();weapon_camera_references[instance]=reference;
    }
}
void invalidate_received_mesh(uint32_t model){
    // 801286C0 normally throttles skinning for up to four render frames and
    // keys it by local animation state. A network pose bypasses that state.
    // Invalidate the geometry stamp so the next actual draw skins the received
    // bones even if the local clip and interpolation values did not change.
    mem_write32(model+0x84,mem_read32(model+0x80)-8u);
}
bool restore_enemy_pose(uint32_t model,bool write=true) {
    auto found=enemy_poses.find(model);if(found==enemy_poses.end())return false;const auto& pose=found->second;
    uint32_t bound=0;
    if(pose.weapon>=0){
        if(!readable(pose.object,0x790) || mem_read32(pose.object+0x5DC)!=uint32_t(pose.weapon))return false;
        const auto instance=mem_read32(pose.object+0x4BC+unsigned(pose.weapon)*24+pose.index*4);
        if(readable(instance,0x228))bound=mem_read32(instance+0x28);
    }else if(pose.object_type==0x802121E8u){
        const auto proxy=readable(pose.object,0x14C)?mem_read32(pose.object+0x148):0;
        if(readable(proxy,0x20))for(unsigned i=0;i<4;++i){const auto part=mem_read32(proxy+i*4);if(readable(part,0x24) && mem_read32(part+0x20)==model){bound=model;break;}}
    }else if(pose.object_type==0x8020C6B8u){
        if(readable(pose.object,0x188))bound=mem_read32(pose.object+0xF4);
    }else{
        const auto proxy=readable(pose.object,0x100)?mem_read32(pose.object+0xFC):0;
        if(readable(proxy,0x40))bound=mem_read32(proxy+0x20);
    }
    if(bound!=model || mem_read32(pose.object+0x48)!=pose.object_type || !readable(model,0xB0) || mem_read32(model)!=0x8021D668u || mem_read32(model+0xC)!=pose.resource || mem_read32(model+0x7C)*12!=pose.values.size())return false;
    const auto matrices=mem_read32(model+0x78);if(!readable(matrices,unsigned(pose.values.size())*4))return false;
    if(write)for(unsigned i=0;i<pose.values.size();++i)mem_write32(matrices+i*4,f2u(pose.values[i]));return true;
}
void register_animation_source(uint32_t model){
    const auto source=mem_read32(model+0x94);
    if(readable(source,0xB0) && mem_read32(source)==0x8021D668u){
        const auto value=std::pair{model,mem_read32(source+0xC)};
        const auto range=animation_sources.equal_range(source);
        for(auto it=range.first;it!=range.second;++it)if(it->second==value)return;
        animation_sources.emplace(source,value);
    }
}
void bones_hook(PPCContext& context){
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::SkeletonPose);
    const auto model=context.r[3];
    if(suppress() && restore_enemy_pose(model)) {
        // The draw getter (801286C0) requires a successful bone evaluation and
        // a current-frame stamp before it updates skinned vertices. Returning
        // the local idle animation's "unchanged" code leaves stale geometry.
        mem_write32(model+0x80,mem_read32(context.r[13]-0x6D80));
        context.r[3]=0;return;
    }
    native_animation_pose_dispatch(context,original_bones);
}
// Private diagnostics retain native clip clocks instead of judging gait speed
// by rendered frame counts. No extra model hook is installed in ordinary play.
bool trace_animation_clock(){static const bool enabled=std::getenv("SSNE_TEST_HIDDEN") && std::getenv("SSNE_TEST_ANIMATION_TRACE");return enabled;}
struct AnimationOwner{unsigned slot,part;};
std::optional<AnimationOwner> animation_owner(uint32_t model){
    for(unsigned slot=0;slot<native_party_capacity();++slot){
        const auto actor=mem_read32(native_party_registry()+slot*4);
        if(!readable(actor,0x790) || mem_read32(actor+0x48)!=game_address(0x80212268u))continue;
        const auto body=mem_read32(actor+0x148);if(!readable(body,16))continue;
        for(unsigned part=0;part<4;++part){
            const auto proxy=mem_read32(body+part*4);if(!readable(proxy,0x24))continue;
            auto current=mem_read32(proxy+0x20);
            for(unsigned depth=0;depth<4 && readable(current,0x98);++depth){
                if(current==model)return AnimationOwner{slot,part};current=mem_read32(current+0x94);
            }
        }
    }
    return {};
}
void trace_animation_sample(PPCContext& context,uint32_t model,bool skipped){
    if(!trace_animation_clock() || !readable(model,0xB0))return;
    const auto owner=animation_owner(model);if(!owner)return;
    struct Clock{uint64_t calls=0;double input=0,advanced=0,scaled=0;std::chrono::steady_clock::time_point last{};};
    static std::map<uint32_t,Clock> clocks;auto& sample=clocks[model];const double dt=context.ps0[1],speed=u2f(mem_read32(model+0x20));
    ++sample.calls;sample.input+=dt;if(!skipped){sample.advanced+=dt;sample.scaled+=dt*speed;}
    const auto now=std::chrono::steady_clock::now();if(sample.last.time_since_epoch().count() && now-sample.last<std::chrono::milliseconds(200))return;sample.last=now;
    const auto head=mem_read32(model+0x1C);const bool valid_head=readable(head,0x28);
    log_message("[animation-clock] wall="+std::to_string(std::chrono::duration<double>(now.time_since_epoch()).count())+" frame="+std::to_string(mem_read32(context.r[13]-0x6D80))+" slot="+std::to_string(owner->slot)+" part="+std::to_string(owner->part)+" model="+std::to_string(model)+" calls="+std::to_string(sample.calls)+" input_dt_total="+std::to_string(sample.input)+" advance_dt_total="+std::to_string(sample.advanced)+" scaled_dt_total="+std::to_string(sample.scaled)+" dt="+std::to_string(dt)+" physics_dt="+std::to_string(u2f(mem_read32(context.r[13]-0x7A08)))+" render_dt="+std::to_string(u2f(mem_read32(context.r[13]-0x6D98)))+" speed="+std::to_string(speed)+" skipped="+std::to_string(skipped)+" command="+std::to_string(valid_head?mem_read32(head+4):~0u)+" clip="+std::to_string(valid_head?mem_read32(head+8):~0u)+" time="+std::to_string(valid_head?u2f(mem_read32(head+12)):0)+" duration="+std::to_string(valid_head?u2f(mem_read32(head+16)):0)+" selected="+std::to_string(mem_read32(model+0x40))+" selected_frame="+std::to_string(mem_read32(model+0x44)));
}
PPCFunc original_body_motion=nullptr;
void body_motion_hook(PPCContext& context){
    const auto body=context.r[3],actor=context.r[31];
    static const bool enabled=[] {const auto* value=std::getenv("SSNE_NATIVE_GAIT_MOTION");return !value || std::strcmp(value,"0");}();
    // Verified PAL normal player-update call. Startup/preview/dead/vehicle and
    // replica body paths retain their native behavior. The authoritative host's
    // bones, including this gait, are already transmitted to every replica.
    const bool active=enabled && !replica_simulation() && context.lr==0x800BF890u && (frame_limit()==0 || frame_limit()>120) &&
        readable(actor,0x790) && mem_read32(actor+0x48)==game_address(0x80212268u) && mem_read32(actor+0x148)==body &&
        native_party_actor_occupied(actor) && u2f(mem_read32(actor+0xCC))>0 && !mem_read32(actor+0x6C4) && readable(body,0xE8) && !mem_read32(body+0xBC);
    if(!active){gait_states.erase(body);ssne_call_original(context,0x80153354u,original_body_motion);return;}
    const auto config=mem_read32(actor+0x23C),controller=mem_read32(actor+0x344),model=mem_read32(body+0x90);
    if(!readable(config,0x28) || !readable(controller,4) || !readable(model,0xB0)){gait_states.erase(body);ssne_call_original(context,0x80153354u,original_body_motion);return;}
    if(gait_states.size()>64)gait_states.clear();ensure_pose_observer();auto& state=gait_states[body];
    if(state.actor!=actor || state.controller!=controller || state.model!=model){state={actor,controller,model,{}};}
    std::array<float,3> position;for(unsigned i=0;i<3;++i)position[i]=u2f(mem_read32(body+0x3C+i*4));
    const auto sample=state.motion.sample(mem_read32(context.r[13]-0x6D80),position,u2f(mem_read32(actor+0x140)),float(context.ps0[1]),u2f(mem_read32(config+0x24)));
    if(sample.duplicate){context.r[3]=0;return;}
    //80153354 copies current+3C to previous+30 before its remaining native
    // model/flash/orientation work. Give its displacement calculation a smooth
    // physical velocity, then retain that exact current-position history.
    if(sample.reconstructed)for(unsigned i=0;i<3;++i)mem_write32(body+0x30+i*4,f2u(position[i]-sample.displacement[i]));
    ssne_call_original(context,0x80153354u,original_body_motion);
}
PPCFunc original_animation_speed=nullptr;
void animation_speed_hook(PPCContext& context){
    if(trace_animation_clock())if(auto owner=animation_owner(context.r[3]);owner && std::abs(u2f(mem_read32(context.r[3]+0x20))-context.ps0[1])>.00001)
        log_message("[animation-speed] slot="+std::to_string(owner->slot)+" part="+std::to_string(owner->part)+" model="+std::to_string(context.r[3])+" caller="+std::to_string(context.lr)+" speed="+std::to_string(context.ps0[1]));
    ssne_call_original(context,0x80128158u,original_animation_speed);
}
void animation_tick_hook(PPCContext& context){
    const auto model=context.r[3];
    if(suppress()){
        bool received=restore_enemy_pose(model,false);
        const auto range=animation_sources.equal_range(model);
        for(auto it=range.first;!received && it!=range.second;++it){
            const auto [render,resource]=it->second;
            received=restore_enemy_pose(render,false) && mem_read32(render+0x94)==model
                && readable(model,0xB0) && mem_read32(model)==0x8021D668u && mem_read32(model+0xC)==resource;
        }
        // All clip/bone/attachment presentation for these models comes from
        // the host. A local tick would replace it with the client's idle pose.
        // Unreplicated UI/effect models and the host keep their normal tick.
        if(received){trace_animation_sample(context,model,true);context.r[3]=0;return;}
    }
    trace_animation_sample(context,model,false);
    ssne_call_original(context,0x80127718u,original_animation_tick);
}
// Weapon instances are ticked by a separate native manager. Suppressing the
// world and model animation alone does not suppress recoil/bob/holster, barrel
// integration or their fire callbacks. A received actor has one weapon-state
// authority: host poses plus ordered shot/loop events, never these local ticks.
// Ownership is checked through the actor's instance table AND a live received
// body/weapon binding, so loading, unbound actors and host play fall through.
bool received_weapon_instance(uint32_t instance){
    if(!suppress() || !readable(instance,0x228))return false;
    const auto actor=mem_read32(instance+0x1D4);
    if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)return false;
    const auto selected=mem_read32(actor+0x5DC);if(selected>=12)return false;
    bool received=false;
    for(unsigned hand=0;hand<2;++hand){
        const auto current=mem_read32(actor+0x4BC+selected*24+hand*4);
        if(!readable(current,0x228))continue;
        const auto model=mem_read32(current+0x28);const auto pose=enemy_poses.find(model);
        if(pose!=enemy_poses.end() && pose->second.object==actor &&
           pose->second.weapon==int(selected) && restore_enemy_pose(model,false)){received=true;break;}
    }
    // Equip can publish a visible root before the new skeleton is available.
    // The received body still establishes actor authority during that gap.
    const auto body=mem_read32(actor+0x148);
    if(!received && readable(body,16))for(unsigned part=0;part<4;++part){
        const auto proxy=mem_read32(body+part*4);if(!readable(proxy,0x24))continue;
        const auto model=mem_read32(proxy+0x20);const auto pose=enemy_poses.find(model);
        if(pose!=enemy_poses.end() && pose->second.object==actor && restore_enemy_pose(model,false)){received=true;break;}
    }
    if(!received)return false;
    for(unsigned weapon=0;weapon<12;++weapon)for(unsigned hand=0;hand<2;++hand)
        if(mem_read32(actor+0x4BC+weapon*24+hand*4)==instance)return true;
    return false;
}
struct WeaponCallback {uint32_t address,prologue;PPCFunc original=nullptr;};
std::array<WeaponCallback,16> weapon_callbacks{{
    {0x80053634,0x9421FF10},{0x800561AC,0x9421FF00},
    {0x80061198,0x9421FFE0},{0x8005E9D0,0x9421FE10},
    {0x8005746C,0x9421FFE0},{0x80055414,0x9421FFD8},
    {0x8005CA40,0x9421FFD8},{0x80054C00,0x9421FF60},
    {0x8005FF38,0x9421FEE0},{0x800598DC,0x9421FFD0},
    {0x80053064,0x9421FFA8},{0x8005DD3C,0x9421FF38},
    {0x800A032C,0x9421FF30},{0x8005758C,0x9421FF60},
    {0x800550F4,0x9421FF70},{0x80059A48,0x9421FFF0}
}};
template<size_t Index> void weapon_callback(PPCContext& context){
    if(received_weapon_instance(context.r[3]))return;
    const auto& callback=weapon_callbacks[Index];
    if(callback.address==0x800A032Cu){
        const auto instance=context.r[3];
        struct Scope{uint32_t old;~Scope(){preparing_weapon=old;}} scope{preparing_weapon};preparing_weapon=instance;
        weapon_camera_references.erase(instance);
        ssne_call_original(context,callback.address,callback.original);
        if(auto it=weapon_camera_references.find(instance);it!=weapon_camera_references.end()){
            auto& reference=it->second;reference.proxy=readable(instance,4)?mem_read32(instance):0;
            reference.matrix=readable(reference.proxy,8)?mem_read32(reference.proxy+4):0;
            if(readable(reference.matrix,48))for(unsigned i=0;i<12;++i)reference.root[i]=mem_read32(reference.matrix+i*4);
            else weapon_camera_references.erase(it);
        }
        return;
    }
    ssne_call_original(context,callback.address,callback.original);
}
template<size_t... Index> void install_weapon_callbacks(std::index_sequence<Index...>){
    (ssne_install_hook(weapon_callbacks[Index].address,weapon_callback<Index>),...);
}
bool install_weapon_callbacks(){
    // Validate every callback before installing any of them.
    for(auto& callback:weapon_callbacks){
        const auto* function=ssne_find_func(callback.address);
        if(!function || mem_read32(callback.address)!=callback.prologue)return false;
        callback.original=function->func;
    }
    install_weapon_callbacks(std::make_index_sequence<weapon_callbacks.size()>{});return true;
}
// Diagnostic flags are startup environment settings, not in-game options.
// Model draw/skin callbacks can run hundreds of times in one native frame.
bool trace_hidden_models(){static const bool enabled=getenv("SSNE_TEST_HIDDEN")!=nullptr;return enabled;}
bool trace_vertex_audit(){static const bool enabled=getenv("SSNE_DEV_VERTEX_AUDIT")!=nullptr;return enabled;}
void mesh_probe(PPCContext& context){
    const auto model=context.r[3];
    const bool trace=suppress() && trace_hidden_models() && enemy_poses.contains(model);
    native_model_update_dispatch(context,original_mesh);
    if(!trace)return;
    const auto it=enemy_poses.find(model);if(it==enemy_poses.end())return;
    const auto& pose=it->second;const auto matrices=mem_read32(model+0x78);
    if(trace_vertex_audit() && readable(mem_read32(model+0x14),8)){
        auto hash=[&]{uint64_t h=14695981039346656037ull;const auto pair=mem_read32(model+0x14);for(unsigned part=0;part<2;++part){const auto descriptor=mem_read32(pair+part*4);if(!readable(descriptor,8))continue;const auto data=mem_read32(descriptor),size=mem_read32(descriptor+4);if(size>1000000 || !readable(data,size))continue;for(unsigned i=0;i<size;++i){h^=mem_read8(data+i);h*=1099511628211ull;}}return h;};
        const auto before=hash();auto evaluate=context;evaluate.r[3]=model;
        restore_enemy_pose(model);native_skinning_dispatch(evaluate,original_skinning);
        const auto after=hash();
        if(before!=after)log_message("[coop-stale-vertices] slot="+std::to_string(mem_read32(pose.object+0x17C))+" weapon="+std::to_string(pose.weapon)+" model="+std::to_string(model)+" frame="+std::to_string(mem_read32(context.r[13]-0x6D80)));
    }
    unsigned different=0;for(unsigned i=0;i<pose.values.size();++i)different+=mem_read32(matrices+i*4)!=f2u(pose.values[i]);
    static std::map<uint32_t,unsigned> samples;
    if(++samples[model]%30!=1 && !different && context.r[3]==mem_read32(model+0x14))return;
    log_message("[coop-mesh-probe] slot="+std::to_string(mem_read32(pose.object+0x17C))+" weapon="+std::to_string(pose.weapon)+" model="+std::to_string(model)+" own="+std::to_string(mem_read32(model+0x14))+" returned="+std::to_string(context.r[3])+" changed_bones="+std::to_string(different)+" attached="+std::to_string(mem_read32(model+0x90))+" frame="+std::to_string(mem_read32(context.r[13]-0x6D80))+" skinned="+std::to_string(mem_read32(model+0x84)));
}
void skinning_hook(PPCContext& context){
    const auto model=context.r[3];const bool received=suppress() && restore_enemy_pose(model);
    native_skinning_dispatch(context,original_skinning);
    if(received && trace_hidden_models()){
        const auto found=enemy_poses.find(model);
        if(found!=enemy_poses.end() && found->second.weapon>=0){
            static std::array<unsigned,12> samples{};const auto weapon=unsigned(found->second.weapon);
            if(++samples[weapon]==1 || samples[weapon]%60==0){
                const auto pair=mem_read32(model+0x14),descriptor=readable(pair,8)?mem_read32(pair):0;
                const auto data=readable(descriptor,8)?mem_read32(descriptor):0,size=readable(descriptor,8)?mem_read32(descriptor+4):0;
                uint64_t hash=14695981039346656037ull;
                if(size && size<=1000000 && readable(data,size)){for(unsigned i=0;i<size;++i){hash^=mem_read8(data+i);hash*=1099511628211ull;}
                    log_message("[coop-weapon-rendered-vertices] weapon="+std::to_string(weapon)+" samples="+std::to_string(samples[weapon])+" hash="+std::to_string(hash));}
            }
        }
    }
}
#include "native_movie.inc"
uint32_t game_update=0,world_update=0;
bool suppress(){return replica && mem_read32(game_address(0x802DB0E0))==4 && (mem_read32(game_address(0x802DB1BC))>=1 && mem_read32(game_address(0x802DB1BC))<=4);}
void game_hook(PPCContext& context){movie_scene=context.r[3];if(suppress()){++game_skips;return;}ssne_call_original(context,game_update,original_game);}
void world_hook(PPCContext& context){if(suppress()){++world_skips;return;}ssne_call_original(context,world_update,original_world);}
void camera_hook(PPCContext& context){if(suppress())return;ssne_call_original(context,0x800A5EDC,original_camera);}
void level_request_hook(PPCContext& context){if(replica && (mem_read32(game_address(0x802DB1BC))>=1 && mem_read32(game_address(0x802DB1BC))<=4)){log_message("[coop-replica-authority] local level request suppressed");return;}ssne_call_original(context,0x80167B40,original_level_request);}
void game_end_hook(PPCContext& context){if(replica && (mem_read32(game_address(0x802DB1BC))>=1 && mem_read32(game_address(0x802DB1BC))<=4)){log_message("[coop-replica-authority] local campaign-end request suppressed");return;}ssne_call_original(context,0x80167D10,original_game_end);}
}
uint32_t native_game_scene(){return movie_scene;}
void install_replica_simulation_hooks() {
    const bool pal=mem_read32(0x80000000u)==0x47334250u;
    game_update=pal?0x80166724:0x8016677C;world_update=pal?0x801508B4:0x801508FC;
    const auto* game=ssne_find_func(game_update);const auto* world=ssne_find_func(world_update);
    if(!game || !world || mem_read32(game_update)!=0x9421FFB8u || mem_read32(world_update)!=0x9421FFA8u) {
        // Enabling a replica without both verified update hooks would allow local AI/scripts.
        original_game=original_world=nullptr;return;
    }
    original_game=game->func;original_world=world->func;
    ssne_install_hook(game_update,game_hook);ssne_install_hook(world_update,world_hook);
    if(pal) {
        if(!install_weapon_callbacks()){original_game=original_world=nullptr;return;}
        install_movie_hooks();
        const auto* camera=ssne_find_func(0x800A5EDC);
        if(!camera || mem_read32(camera->address)!=0x9421FE30u){original_game=original_world=nullptr;return;}
        original_camera=camera->func;ssne_install_hook(camera->address,camera_hook);
        const auto* getter=ssne_find_func(0x800A6A8Cu);
        if(!getter || mem_read32(getter->address)!=0x9421FFF8u){original_game=original_world=nullptr;return;}
        original_camera_getter=getter->func;ssne_install_hook(getter->address,camera_getter_hook);
        const auto* level=ssne_find_func(0x80167B40);const auto* end=ssne_find_func(0x80167D10);
        if(!level || !end || mem_read32(level->address)!=0x9421FFE0u || mem_read32(end->address)!=0x9421FFE8u){original_game=original_world=nullptr;return;}
        original_level_request=level->func;original_game_end=end->func;
        ssne_install_hook(level->address,level_request_hook);ssne_install_hook(end->address,game_end_hook);
        const auto* bones=ssne_find_func(0x80129154);const auto* skinning=ssne_find_func(0x80129958);
        const auto* tick=ssne_find_func(0x80127718u);
        if(!tick || mem_read32(tick->address)!=0x9421FF10u){original_game=original_world=nullptr;return;}
        original_animation_tick=tick->func;ssne_install_hook(tick->address,animation_tick_hook);
        const auto* gait=ssne_find_func(0x80153354u);
        if(gait && mem_read32(gait->address)==0x9421FDE8u && mem_read32(gait->address+4)==0x7C0802A6u){original_body_motion=gait->func;ssne_install_hook(gait->address,body_motion_hook);}
        if(trace_animation_clock())if(const auto* speed=ssne_find_func(0x80128158u);speed && mem_read32(speed->address)==0xD0230020u){original_animation_speed=speed->func;ssne_install_hook(speed->address,animation_speed_hook);}
        if(const auto* mesh=ssne_find_func(0x801286C0);mesh && mem_read32(mesh->address)==0x9421FFF0u){original_mesh=mesh->func;ssne_install_hook(mesh->address,mesh_probe);}
        if(bones && skinning && mem_read32(bones->address)==0x9421FF40u && mem_read32(skinning->address)==0x9421FFF0u){original_bones=bones->func;original_skinning=skinning->func;ssne_install_hook(bones->address,bones_hook);ssne_install_hook(skinning->address,skinning_hook);}
    }
}
void set_replica_simulation(bool enabled){if(enabled && (!original_game || !original_world))fatal("Native replica simulation hooks are unavailable for this revision.");replica=enabled;if(!enabled){clear_poses();hidden_npc_parts.clear();if(pose_subscription){stop_observing_native_entities(pose_subscription);pose_subscription=0;}}}
bool replica_simulation(){return replica;}
bool replica_hide_npc_part(uint32_t object){
 if(!replica || !readable(object,0x130) || mem_read32(object+0x48)!=0x8020B990u || !mem_read32(object+0xC8))return false;
 const auto proxy=mem_read32(object+0xCC);if(!readable(proxy,0x38))return false;
 ensure_pose_observer();hidden_npc_parts[proxy]={object,mem_read32(proxy+0x1C)};return true;
}
bool replica_suppressed_npc_proxy(uint32_t proxy){
 if(!replica || hidden_npc_parts.empty())return false;const auto it=hidden_npc_parts.find(proxy);if(it==hidden_npc_parts.end())return false;
 const auto& p=it->second;return readable(p.object,0x130) && mem_read32(p.object+0x48)==0x8020B990u && mem_read32(p.object+0xCC)==proxy && mem_read32(p.object+0xC8) && readable(proxy,0x38) && mem_read32(proxy+0x1C)==p.resource;
}
std::optional<std::array<float,12>> native_weapon_camera_reference(uint32_t instance){
    const auto it=weapon_camera_references.find(instance);if(it==weapon_camera_references.end())return {};
    const auto& r=it->second;if(!readable(instance,0x228) || mem_read32(instance+0x1D4)!=r.actor || mem_read32(instance)!=r.proxy || !readable(r.proxy,8) || mem_read32(r.proxy+4)!=r.matrix || !readable(r.matrix,48))return {};
    for(unsigned i=0;i<12;++i)if(mem_read32(r.matrix+i*4)!=r.root[i])return {};
    return r.camera;
}
uint32_t native_gameplay_gui(){return readable(movie_scene,0xA8)?mem_read32(movie_scene+0xA4):0;}
uint32_t prepare_native_gameplay_gui(PPCContext& context,std::string& error){
    if(!replica || !readable(movie_scene,0xA8) || !readable(context.r[1]-0x800,0x800) || context.r[13]!=0x802E22A0u){error="Native gameplay GUI scene unavailable";return 0;}
    if(const auto gui=native_gameplay_gui()){if(readable(gui,0x90))return gui;error="Invalid native gameplay GUI";return 0;}
    const auto asset=mem_read32(movie_scene+0x78);if(!readable(asset,0x10) || !ssne_find_func(0x800F5448)){error="Original results GUI resource unavailable";return 0;}
    MovieScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=asset;call.r[4]=uint32_t(netplay_local_player());ssne_call(call,0x800F5448);
    if(!readable(call.r[3],0x90)){error="Original results GUI allocation failed";return 0;}mem_write32(movie_scene+0xA4,call.r[3]);return call.r[3];
}
void replica_clear_enemy_poses(){clear_poses();}
bool replica_enemy_pose(uint32_t object,uint32_t model,const std::vector<float>& pose) {
    if(!original_bones || !original_skinning || pose.empty() || pose.size()>3072 || pose.size()%12 || !readable(object,0x100) || !readable(model,0xB0) || mem_read32(model)!=0x8021D668u || mem_read32(model+0x7C)*12!=pose.size())return false;
    for(auto value:pose)if(!std::isfinite(value) || std::abs(value)>10000.f)return false;
    ensure_pose_observer();enemy_poses[model]={object,mem_read32(object+0x48),mem_read32(model+0xC),pose};register_animation_source(model);invalidate_received_mesh(model);return true;
}
bool replica_weapon_pose(uint32_t actor,unsigned weapon,unsigned index,const std::vector<float>& pose) {
    if(!original_bones || !original_skinning || weapon>=12 || index>=2 || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(actor+0x5DC)!=weapon)return false;
    const auto instance=mem_read32(actor+0x4BC+weapon*24+index*4);
    const auto model=readable(instance,0x228)?mem_read32(instance+0x28):0;
    if(pose.empty() || pose.size()>3072 || pose.size()%12 || !readable(model,0xB0) || mem_read32(model)!=0x8021D668u || mem_read32(model+0x7C)*12!=pose.size() || !readable(mem_read32(model+0x78),unsigned(pose.size())*4))return false;
    for(auto value:pose)if(!std::isfinite(value) || std::abs(value)>10000.f)return false;
    ensure_pose_observer();enemy_poses[model]={actor,mem_read32(actor+0x48),mem_read32(model+0xC),pose,int(weapon),index};
    register_animation_source(model);
    invalidate_received_mesh(model);
    return restore_enemy_pose(model);
}
NativeMovieState native_movie_state(){
    auto result=movie_state;result.active=native_movie_presenting();
    if(result.active){const auto resource=mem_read32(movie_scene+0x80),bink=mem_read32(resource+0x10);if(readable(bink,0x20))result.frame=mem_read32(bink+0xC);}
    return result;
}
bool native_movie_presenting(){return mem_read32(game_address(0x802DB0E0))==4 && readable(movie_scene,0x94) && readable(mem_read32(movie_scene+0x80),0xC0);}
void set_native_movie_vote_gate(bool enabled){movie_vote_gate.store(enabled);}
bool request_native_movie_skip(PPCContext& context,uint64_t sequence,std::string& error){
    if(!movie_vote_gate.load() || replica || !movie_stop_original || !sequence || sequence!=movie_state.sequence || !native_movie_presenting() || context.r[13]!=0x802E22A0u || !readable(context.r[1]-2048,2048)){
        error="Authoritative native movie skip unavailable";return false;
    }
    movie_skip_pending=sequence;
    log_message("[coop-movie-skip-queued] sequence="+std::to_string(sequence));return true;
}
bool apply_native_movie(PPCContext& context,const NativeMovieState& desired,std::string& error){return apply_movie(context,desired,error);}
ReplicaSimulationStats replica_simulation_stats(){return {game_skips.load(),world_skips.load()};}
}
