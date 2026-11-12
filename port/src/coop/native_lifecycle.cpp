#include "native_lifecycle.h"
#include "native_vehicle.h"
#include "native_bootstrap.h"
#include <cctype>
#include "native_configuration.h"
#include "native_world.h"
#include "native_presentation.h"
#include "ssne/runtime.h"
#include "ssne/game_addresses.h"
#include "ssne/recomp_hooks.h"
#include "ssne/native_entity.h"
#include "ssne/native_pickup.h"
#include "ssne/native_party.h"
#include "ssne/native_effect.h"
#include "ssne/replica_simulation.h"
#include <cmath>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>
namespace port::coop {
namespace {
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;}
int prototype(uint32_t definition) {
    // Factory definition2 is MiniSam (PAL ctor80018E7C, class801E9258).
    // Player snapshots also use definition2, but EntityKind/vtable distinguish
    // them. Rejecting this number aborted the entire replica batch as soon as
    // the scripted MiniSam spawned, including camera and speech application.
    for(unsigned index=0;index<61;++index)if(mem_read32(ssne::game_address(0x8021AB2C)+index*4)==definition)return int(index);
    return -1;
}
bool context_ok(const PPCContext& c){return mem_read32(0x80000000)==0x47334250u && c.r[13]==0x802E22A0 && c.r[1]>=0x80000800 && readable(c.r[1]-0x800,0x800);}
template<uint32_t Address> void weapon_trace_hook(PPCContext& c){
    const auto actor=c.r[3],argument=c.r[4],caller=c.lr;
    const bool valid=readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u;
    const auto before=valid?mem_read32(actor+0x5DC):0,pending=valid?mem_read32(actor+0x5F8):0;
    ssne_call_original(c,Address,ssne_find_func(Address)->func);
    if(valid && (before!=mem_read32(actor+0x5DC) || pending!=mem_read32(actor+0x5F8) || Address==0x800C5F60u)){
        fprintf(stderr,"[coop-native-weapon-call] function=%08X caller=%08X slot=%u argument=%u before=%u after=%u pending_before=%u pending_after=%u state=%u queue=%u\n",Address,caller,mem_read32(actor+0x17C),argument,before,mem_read32(actor+0x5DC),pending,mem_read32(actor+0x5F8),mem_read32(actor+0x5F4),mem_read32(actor+0x4B0));
    }
}
void pickup_trace_hook(PPCContext& c){
    const auto pickup=c.r[3],actor=c.r[4];
    const bool valid=readable(pickup,0x1D4) && readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u;
    const auto slot=valid?mem_read32(actor+0x17C):max_players;
    const auto before=valid && slot<8?ssne::native_party_pickup_value(pickup,slot):0;
    const auto pending=valid?mem_read32(actor+0x5F8):0;
    ssne_call_original(c,0x800C7F78,ssne_find_func(0x800C7F78)->func);
    if(valid && slot<8 && mem_read32(pickup+0x180)>=35 && mem_read32(pickup+0x180)<=45){
        const auto config=mem_read32(pickup+0xDC);
        fprintf(stderr,"[coop-native-pickup-call] object=%u type=%u slot=%u result=%u flag_before=%u flag_after=%u destroy=%u respawn=%u cooldown=%.3f mode=%u pending_before=%u pending_after=%u\n",pickup,mem_read32(pickup+0x180),slot,c.r[3],before,ssne::native_party_pickup_value(pickup,slot),mem_read32(pickup+0xE8),readable(config,8)?mem_read32(config+4):~0u,u2f(mem_read32(pickup+0x118)),mem_read32(c.r[13]-0x69D0),pending,mem_read32(actor+0x5F8));
    }
}
void pickup_destructor_trace_hook(PPCContext& c){
    const auto pickup=c.r[3];
    const auto config=readable(pickup,0x1D4)?mem_read32(pickup+0xDC):0;
    const bool owned=readable(config,0x40) && mem_read32(pickup+0xE0)!=0;
    const auto type=owned?mem_read32(config):0;
    ssne_call_original(c,0x800C6B60,ssne_find_func(0x800C6B60)->func);
    if(owned)fprintf(stderr,"[coop-native-pickup-release] object=%u type=%u config=%u allocated_after=%u\n",pickup,type,config,mem_read32(config-0x14));
}
void install_weapon_trace(){
    static bool installed=false;
    if(installed || !getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_COOP_TRACE_LIFETIMES"))return;
    installed=true;
    ssne_install_hook(0x800C5F60,weapon_trace_hook<0x800C5F60>);
    ssne_install_hook(0x800C5C18,weapon_trace_hook<0x800C5C18>);
    ssne_install_hook(0x800C59CC,weapon_trace_hook<0x800C59CC>);
    ssne_install_hook(0x800C5B18,weapon_trace_hook<0x800C5B18>);
    ssne_install_hook(0x800C7F78,pickup_trace_hook);
    ssne_install_hook(0x800C6B60,pickup_destructor_trace_hook);
}
struct StackScratch {
    // Callers validate this complete, contiguous guest-RAM stack range first.
    // Preserve the same bytes without allocating and translating every byte.
    uint32_t start;std::array<uint8_t,0x800> saved;
    explicit StackScratch(const PPCContext& c):start(c.r[1]-0x800){std::memcpy(saved.data(),ssne::host_ptr(start),saved.size());}
    ~StackScratch(){std::memcpy(ssne::host_ptr(start),saved.data(),saved.size());}
};
PPCFunc original_fire_direction=nullptr;
PPCFunc original_pickup_selection=nullptr;
uint64_t manual_aim_subscription=0;
std::map<uint32_t,uint32_t> afk_manual_aim;
void afk_pickup_selection_hook(PPCContext& context){
    const auto actor=context.r[3],weapon=context.r[4];const auto found=afk_manual_aim.find(actor);
    // The pickup handler has already granted the original item/ammo through
    // 800C6224. Only its automatic equip request competes with AFK weapon policy.
    // Human requests and every other native call retain the original behavior.
    if(context.lr==0x800C16F4u && found!=afk_manual_aim.end() && context_ok(context) && weapon<12 && readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u && mem_read32(actor+0x37C)==found->second){
        const auto slot=mem_read32(actor+0x17C);
        if(slot<ssne::native_party_capacity() && mem_read32(ssne::native_party_registry()+slot*4)==actor && mem_read32(actor+0x4CC+weapon*24)){
            if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))fprintf(stderr,"[coop-afk-pickup-equip-deferred] slot=%u weapon=%u selected=%u owned=%u\n",slot,weapon,mem_read32(actor+0x5DC),mem_read32(actor+0x4CC+weapon*24));
            return;
        }
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))weapon_trace_hook<0x800C5D24>(context);
    else ssne_call_original(context,0x800C5D24,original_pickup_selection);
}
void afk_fire_direction_hook(PPCContext& context){
    const auto actor=context.r[3],output=context.r[4];auto found=afk_manual_aim.find(actor);
    if(found==afk_manual_aim.end() || !context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(actor+0x37C)!=found->second || !readable(output,12)){
        ssne_call_original(context,0x800C43A0,original_fire_direction);return;
    }
    const auto slot=mem_read32(actor+0x17C);
    if(slot>=ssne::native_party_capacity() || mem_read32(ssne::native_party_registry()+slot*4)!=actor){ssne_call_original(context,0x800C43A0,original_fire_direction);return;}
    const auto camera=mem_read32(actor+0x368);
    if(!readable(camera,0x100) || !readable(mem_read32(camera),0x28) || !ssne_find_func(mem_read32(mem_read32(camera)+0x24))){ssne_call_original(context,0x800C43A0,original_fire_direction);return;}
    std::array<float,3> direction;
    {StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=camera;call.r[4]=call.r[1]+0x60;ssne_call(call,mem_read32(mem_read32(camera)+0x24));
        for(unsigned i=0;i<3;++i)direction[i]=u2f(mem_read32(call.r[1]+0x60+8+i*16));}
    float length=0;for(auto component:direction)length+=component*component;
    if(!std::isfinite(length) || length<.01f || length>100.f){ssne_call_original(context,0x800C43A0,original_fire_direction);return;}
    for(unsigned i=0;i<3;++i)mem_write32(output+i*4,f2u(direction[i]));
}
}
bool set_native_script_enabled(PPCContext& context,uint32_t object,bool enabled,std::string& error) {
    const auto vt=readable(object,0x54)?mem_read32(object+0x48):0;
    const auto method=vt==0x80209758u?0x800848C0u:vt==0x80208B60u?0x80083944u:0u;
    if(!context_ok(context) || !method || !ssne_find_func(method) || mem_read32(method)!=(vt==0x80209758u?0x9421FFE0u:0x9421FFD8u) || mem_read32(vt+0x44)!=method){error="Native trigger/switch enable event unavailable";return false;}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    const auto event=context.r[1]-0x500;
    for(unsigned i=0;i<16;i+=4)mem_write32(event+i,0);
    mem_write32(event,enabled?11u:12u);call.r[3]=object;call.r[4]=event;ssne_call(call,method);
    return true;
}
bool activate_native_brush(PPCContext& context,uint32_t object,std::string& error) {
    if(!context_ok(context) || !readable(object,0x13C) || mem_read32(object+0x48)!=0x80206868u || !ssne_find_func(0x8007F9C0) || mem_read32(0x8007F9C0)!=0x9421FFE0u){error="Native moving-brush activation unavailable";return false;}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=object;ssne_call(call,0x8007F9C0);return true;
}
#include "native_model_states.inc"
#include "native_vehicle.inc"
std::array<uint8_t,8> capture_native_preferences(uint32_t actor){
    std::array<uint8_t,8> result{255,255,255,255,255,255,255,255};
    if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)return result;
    const auto sample=read_native_configuration(mem_read32(actor+0x18C));if(!sample.error.empty())return result;
    for(unsigned i=0;i<result.size();++i)if(auto it=sample.values.find(native_preference_keys[i]);it!=sample.values.end())if(auto n=std::get_if<int64_t>(&it->second);n && *n>=0 && *n<=3)result[i]=uint8_t(*n);
    return result;
}
bool apply_native_preferences(PPCContext& context,uint32_t actor,const std::array<uint8_t,8>& preferences,std::string& error){
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u){error="Native preference owner unavailable";return false;}
    const auto root=mem_read32(actor+0x18C);const auto current=capture_native_preferences(actor);
    Properties changed;
    for(unsigned i=0;i<preferences.size();++i)if(preferences[i]!=255 && preferences[i]!=current[i]){
        if(preferences[i]>3 || !validate_native_configuration(context,root,{{native_preference_keys[i],int64_t(preferences[i])}},native_preference_keys[i],error))return false;
        changed[native_preference_keys[i]]=int64_t(preferences[i]);
    }
    for(const auto& [key,value]:changed)if(!write_native_configuration(context,root,{{key,value}},key,error))return false;
    return refresh_native_aim_preferences(context,actor,false,error);
}
bool refresh_native_aim_preferences(PPCContext& context,uint32_t actor,bool presentation,std::string& error){
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)return true;
    const auto variable=mem_read32(actor+0x378),aim=mem_read32(actor+0x37C);
    if(!readable(variable,0x28) || !readable(aim,0x60))return true; // Native spawn has not finished.
    const auto mode=mem_read32(variable+0x24);if(mode>2){error="Invalid native auto-aim mode";return false;}
    // Original800BF6B8 copies the live option into this controller every tick.
    // That actor tick is suppressed on replicas. The original menu sets 0
    // for Off and 1 for On. 2 is an internal no-search mode, not menu Off.
    mem_write32(aim,mode);
    if(mode!=1){mem_write32(aim+0xC,0);mem_write32(aim+0x44,0);}
    if(presentation && ssne::replica_simulation()){
        // Before replication starts the original boot simulation may create
        // another player's first-person target ring. Its actor tick then
        // stops, leaving that separate renderer alive in our local view.
        if(!ssne_find_func(0x800AFD60u)){error="Original aim marker retirement unavailable";return false;}
        StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
        for(unsigned slot=0;slot<ssne::native_party_capacity();++slot){const auto remote=mem_read32(ssne::native_party_registry()+slot*4);if(remote==actor || !readable(remote,0x790) || mem_read32(remote+0x48)!=0x802121E8u)continue;
            const auto controller=mem_read32(remote+0x37C);if(!readable(controller,0x60) || !mem_read32(controller+0x50))continue;
            const auto enabled=mem_read32(controller+0x48);call.r[3]=controller;call.r[4]=0;ssne_call(call,0x800AFD60u);mem_write32(controller+0x48,enabled);
        }
    }
    if(!presentation || !ssne::netplay_world_ready() || ssne::local_pause_menu_open())return true;
    for(auto entry:{0x800C433Cu,0x800C43A0u,0x800C44D4u,0x800AFE08u})if(!ssne_find_func(entry)){error="Original aim presentation unavailable";return false;}
    // Only the original aim controller / marker updater, never the full
    // player firing method (800C4EF8 also advances weapons and ammunition).
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    for(const auto [entry,offset]:{std::pair{0x800C433Cu,0x60u},std::pair{0x800C43A0u,0x70u},std::pair{0x800C44D4u,0x80u}}){call.r[3]=actor;call.r[4]=call.r[1]+offset;ssne_call(call,entry);}
    call.r[3]=aim;call.r[4]=call.r[1]+0x60;call.r[5]=call.r[1]+0x70;call.r[6]=call.r[1]+0x80;ssne_call(call,0x800AFE08);
    if(mode!=1){mem_write32(aim+0xC,0);mem_write32(aim+0x44,0);}return true;
}
bool validate_native_destructible(uint32_t object,const EntityState& e,std::string& error){
    auto fail=[&](const char* why){error=why;return false;};
    const bool model=e.definition==2;
    if(e.kind!=EntityKind::Destructible || (e.definition!=1 && !model) || !readable(object,model?0x9C:0x70) || mem_read32(object+0x48)!=(model?0x80206348u:0x80204190u))return fail("Invalid native destructible class");
    const auto config=mem_read32(object+0x54),level=mem_read32(0x802DACC0);
    if(!readable(config,0x24) || !readable(level,0x29C))return fail("Native destructible configuration unavailable");
    for(const auto key:{"destruct_index","destruct_resource","destruct_destroyed"})if(!e.properties.contains(key) || !std::holds_alternative<int64_t>(e.properties.at(key)))return fail("Incomplete native destructible identity");
    const auto index=std::get<int64_t>(e.properties.at("destruct_index")),resource=std::get<int64_t>(e.properties.at("destruct_resource")),destroyed=std::get<int64_t>(e.properties.at("destruct_destroyed"));
    if(index<0 || index!=mem_read32(object+0x50) || resource<0 || resource!=mem_read32(config) || (destroyed!=0 && destroyed!=1))return fail("Native destructible identity/state differs");
    const auto manager=mem_read32(level+0x298);
    if(model){
        if(!readable(manager,0x28) || mem_read32(manager)!=0x8021E660u || mem_read32(manager+0x1C)>4096 || resource>=mem_read32(manager+0x1C) || !readable(mem_read32(manager+0x18),mem_read32(manager+0x1C)*20u) || !ssne_find_func(0x80133EF8) || mem_read32(0x80133EF8)!=0x81630018u)return fail("Native destructible model table unavailable");
        for(const auto key:{"destruct_state","destruct_active","destruct_stage"})if(!e.properties.contains(key) || !std::holds_alternative<int64_t>(e.properties.at(key)))return fail("Incomplete model destruction state");
        const auto entry=mem_read32(manager+0x18)+unsigned(resource)*20;
        const auto geometry=std::get<int64_t>(e.properties.at("destruct_state")),active=std::get<int64_t>(e.properties.at("destruct_active")),stage=std::get<int64_t>(e.properties.at("destruct_stage"));
        if(geometry< -1 || geometry>=mem_read32(entry+0xC) || (active!=0 && active!=1) || stage<0 || stage>=mem_read32(object+0x7C) || destroyed!=(geometry<0))return fail("Invalid model destruction stage/visibility");
        // Some level entries are hidden placeholders or allocate their render
        // model lazily. The original variant setter explicitly accepts null.
        const auto render_model=mem_read32(entry);if(render_model && !readable(render_model,0xC0))return fail("Native destructible model unavailable");
        return true;
    }
    for(const auto key:{"destruct_health","destruct_damage"}){const auto it=e.properties.find(key);const auto* f=it==e.properties.end()?nullptr:std::get_if<float>(&it->second);if(!f || !std::isfinite(*f) || std::abs(*f)>1000000)return fail("Invalid destructible health/damage");}
    if(!readable(manager,0x18) || mem_read32(manager)!=0x8021E660u || mem_read32(manager+0x14)>200 || resource>=mem_read32(manager+0x14) || !readable(mem_read32(manager+0x10),mem_read32(manager+0x14)*80u) || !ssne_find_func(0x8013368C) || mem_read32(0x8013368C)!=0x9421FFF0u)return fail("Native destructible geometry unavailable");
    const auto entry=mem_read32(manager+0x10)+unsigned(resource)*80;
    if(!readable(mem_read32(entry),0xC0) || mem_read32(entry+0x14)>1)return fail("Native destructible render model unavailable");
    return true;
}
bool apply_native_destructible(PPCContext& context,uint32_t object,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_destructible(object,e,error))return false;
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    call.r[3]=mem_read32(mem_read32(0x802DACC0)+0x298);call.r[4]=unsigned(std::get<int64_t>(e.properties.at("destruct_resource")));call.r[5]=e.definition==2?uint32_t(std::get<int64_t>(e.properties.at("destruct_state"))):std::get<int64_t>(e.properties.at("destruct_destroyed"))?~0u:0u;
    // Original geometry/cache setter only. Destruction, damage, triggers and
    // shard generation remain host-owned and are not run again here.
    ssne_call(call,e.definition==2?0x80133EF8:0x8013368C);
    static const bool trace_destructibles=getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES");
    if(trace_destructibles){static std::map<uint32_t,bool> seen;const bool destroyed=std::get<int64_t>(e.properties.at("destruct_destroyed"));if(!seen.contains(object) || seen[object]!=destroyed){seen[object]=destroyed;
        const auto manager=mem_read32(mem_read32(0x802DACC0)+0x298),node=mem_read32(manager+(e.definition==2?0x18:0x10))+unsigned(std::get<int64_t>(e.properties.at("destruct_resource")))*(e.definition==2?20:80);
        ssne::log_message("[coop-destructible-client] index="+std::to_string(std::get<int64_t>(e.properties.at("destruct_index")))+" destroyed="+std::to_string(destroyed)+" model_visible="+std::to_string(mem_read32(node)?mem_read32(mem_read32(node)+0x30):0));}}
    return true;
}
bool validate_native_brush_geometry(uint32_t object,int64_t resource,int64_t segment,float progress,std::string& error) {
    auto fail=[&](const char* why){error=why;return false;};
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(object,0x13C) || mem_read32(object+0x48)!=0x80206868u || resource<0 || segment<0 || !std::isfinite(progress) || progress<0 || progress>1)return fail("Invalid authoritative moving-brush state");
    const auto config=mem_read32(object+0x5C),level=mem_read32(0x802DACC0);
    if(!readable(config,0x38) || mem_read32(config+0xC)!=resource || !readable(level,0x29C))return fail("Moving-brush resource identity mismatch");
    auto node=mem_read32(config+0x14);std::set<uint32_t> nodes;
    while(node) {
        if(!readable(node,0x18) || !readable(mem_read32(node+0x14),48) || !nodes.insert(node).second || nodes.size()>4096)return fail("Invalid native moving-brush path");
        node=mem_read32(node+0xC);
    }
    if(uint64_t(segment)>=nodes.size())return fail("Authoritative moving-brush segment leaves its path");
    const auto manager=mem_read32(level+0x298),vt=readable(manager,0x18)?mem_read32(manager):0;
    if(vt!=0x8021E660u || !readable(manager,0x18) || mem_read32(manager+0x14)>65536 || resource>=mem_read32(manager+0x14) || !readable(mem_read32(manager+0x10),mem_read32(manager+0x14)*80u))return fail("Native geometry manager unavailable");
    if(mem_read32(vt+0x7C)!=0x8013368Cu || mem_read32(vt+0x84)!=0x801337C8u || !ssne_find_func(0x8013368C) || !ssne_find_func(0x801337C8) || mem_read32(0x8013368C)!=0x9421FFF0u || mem_read32(0x801337C8)!=0x9421FF90u)return fail("Native moving-brush geometry setters unavailable");
    const auto entry=mem_read32(manager+0x10)+uint32_t(resource)*80;
    if(!readable(mem_read32(entry),0xC0))return fail("Native moving-brush model unavailable");
    const auto path=mem_read32(entry+8);
    if(!readable(path,0x24))return fail("Native moving-brush geometry path unavailable");
    const auto first=mem_read32(path+0xC);auto geometry_node=first;std::set<uint32_t> geometry_nodes;
    while(geometry_node && !geometry_nodes.contains(geometry_node)) {
        if(!readable(geometry_node,8) || !readable(mem_read32(geometry_node+4),48) || geometry_nodes.size()>=4096)return fail("Invalid native geometry keyframe storage");
        geometry_nodes.insert(geometry_node);geometry_node=mem_read32(geometry_node);
    }
    // Geometry keyframes form a ring; a cycle elsewhere would select invalid data.
    if(geometry_node!=first || geometry_nodes.empty() || uint64_t(segment)>=geometry_nodes.size())return fail("Invalid native geometry keyframe ring");
    return true;
}
bool apply_native_brush_geometry(PPCContext& context,uint32_t object,int64_t resource,int64_t segment,float progress,std::string& error) {
    if(!context_ok(context) || !validate_native_brush_geometry(object,resource,segment,progress,error))return false;
    const auto manager=mem_read32(mem_read32(0x802DACC0)+0x298);
    // These two native geometry setters change cached geometry/velocity only.
    // Do not invoke MovingBrush::Update: it dispatches mission and audio events.
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    call.r[3]=manager;call.r[4]=uint32_t(resource);call.r[5]=uint32_t(segment);ssne_call(call,0x8013368C);
    call.r[3]=manager;call.r[4]=uint32_t(resource);call.ps0[1]=call.ps1[1]=progress;ssne_call(call,0x801337C8);
    mem_write32(object+0xC8,uint32_t(segment));mem_write32(object+0xC0,f2u(progress));return true;
}
bool validate_native_weapon_selection(uint32_t actor,unsigned weapon,std::string& error) {
    if(weapon>=12 || !readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u) || mem_read32(actor+0x5DC)>=12){error="Invalid native weapon selection";return false;}
    const auto instance=mem_read32(actor+0x4BC+weapon*24);
    const auto* select=ssne_find_func(0x800C5F60);
    const auto* swap=ssne_find_func(0x800C5C18);
    if(!swap || mem_read32(swap->address)!=0x9421FFE0u || !readable(mem_read32(actor+0x374),0x534)){error="Native weapon display swap unavailable";return false;}
    if(!readable(instance,0x228) || !readable(mem_read32(instance+0xC),0x160) || !select || mem_read32(select->address)!=0x9421FFD8u){error="Native weapon-selection lifecycle unavailable";return false;}
    return true;
}
bool capture_native_weapon_modes(uint32_t actor,unsigned weapon,std::vector<int64_t>& modes,std::string& error) {
    if(!validate_native_weapon_selection(actor,weapon,error))return false;
    std::vector<int64_t> next(2,-1);
    for(unsigned hand=0;hand<2;++hand){
        const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
        if(!instance && hand)continue;
        if(!readable(instance,0x228) || mem_read32(instance+0x1D4)!=actor ||
           mem_read32(instance+0x1B0)!=mem_read32(actor+0x5E8)){
            error="Native weapon mode owner/inventory binding unavailable";return false;
        }
        const auto count=mem_read32(instance+0x1BC),mode=mem_read32(instance+0x1E0);
        if(!count || count>18 || mode>=count || !readable(mem_read32(instance+0x1D0),count*60)){
            error="Native weapon mode configuration unavailable";return false;
        }
        next[hand]=mode;
    }
    modes=std::move(next);return true;
}
bool validate_native_weapon_modes(uint32_t actor,unsigned weapon,const std::vector<int64_t>& modes,std::string& error) {
    std::vector<int64_t> current;
    if(modes.size()!=2){error="Authoritative weapon modes require two hand entries";return false;}
    if(!capture_native_weapon_modes(actor,weapon,current,error))return false;
    for(unsigned hand=0;hand<2;++hand){
        if(current[hand]<0){if(modes[hand]==-1)continue;error="Authoritative mode targets an absent weapon hand";return false;}
        const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
        const auto count=mem_read32(instance+0x1BC),table=mem_read32(instance+0xC);
        if(modes[hand]<0 || uint64_t(modes[hand])>=count){error="Authoritative weapon mode is outside native configuration";return false;}
        const auto config=mem_read32(instance+0x1D0)+unsigned(modes[hand])*60;
        const auto ammo=int32_t(mem_read32(config+0x38));
        // These are the PAL virtual mode-commit callbacks, not PS2 offsets.
        const auto callback=weapon==6?0x8005CBACu:weapon==7?0x800547BCu:0x800A1288u;
        const auto prologue=weapon==6?0x9421FFF8u:weapon==7?0x9421FFF0u:0x4E800020u;
        if(ammo< -1 || ammo>=18 || !readable(mem_read32(config+0x34),0x28) ||
           !readable(table,0x150) || mem_read16(table+0x148)!=0 || mem_read32(table+0x14C)!=callback ||
           !ssne_find_func(callback) || mem_read32(callback)!=prologue){error="Native weapon mode consumer unavailable";return false;}
    }
    return true;
}
bool apply_native_weapon_modes(PPCContext& context,uint32_t actor,unsigned weapon,const std::vector<int64_t>& modes,std::string& error) {
    if(!context_ok(context) || mem_read32(actor+0x5DC)!=weapon){error="Native weapon mode lifecycle context unavailable";return false;}
    if(!validate_native_weapon_modes(actor,weapon,modes,error))return false;
    StackScratch scratch(context);
    for(unsigned hand=0;hand<2;++hand){
        if(modes[hand]<0)continue;
        const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4),mode=uint32_t(modes[hand]);
        if(mem_read32(instance+0x1E0)==mode)continue;
        // Same commit as 800A0660/800A06C4. The host already completed the
        // lower/raise decision; re-requesting it on a paused replica can hang.
        // Do not tick/fire the weapon or consume ammunition on this client.
        mem_write32(instance+0x1E0,mode);
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
        call.r[3]=instance;ssne_call(call,mem_read32(mem_read32(instance+0xC)+0x14C));
        mem_write32(instance+0x224,1);
    }
    return true;
}
bool native_player_fire_origin(PPCContext& context,uint32_t actor,std::array<float,3>& origin,std::string& error) {
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(0x802121E8u+0x2AC)!=0x800C433Cu || !ssne_find_func(0x800C433C)){error="Native player shot origin unavailable";return false;}
    const auto camera=mem_read32(actor+0x368);if(!readable(camera,0x100)){error="Native shot camera unavailable";return false;}
    const auto table=mem_read32(camera);if(!readable(table,0x28) || !ssne_find_func(mem_read32(table+0x24))){error="Native shot camera getter unavailable";return false;}
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    call.r[3]=actor;call.r[4]=call.r[1]+0x60;ssne_call(call,0x800C433C);
    for(unsigned i=0;i<3;++i){origin[i]=u2f(mem_read32(call.r[1]+0x60+i*4));if(!std::isfinite(origin[i]) || std::abs(origin[i])>1000000.f){error="Invalid native player shot origin";return false;}}
    return true;
}
bool native_player_fire_direction(PPCContext& context,uint32_t actor,std::array<float,3>& direction,std::string& error) {
    std::array<float,3> origin;if(!native_player_fire_origin(context,actor,origin,error) || mem_read32(0x802121E8u+0x2BC)!=0x800C43A0u || !ssne_find_func(0x800C43A0))return false;
    const auto aim=mem_read32(actor+0x37C);if(!readable(aim,0x48)){error="Native firing direction unavailable";return false;}
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=call.r[1]+0x60;ssne_call(call,0x800C43A0);
    float length=0;for(unsigned i=0;i<3;++i){direction[i]=u2f(mem_read32(call.r[1]+0x60+i*4));length+=direction[i]*direction[i];}
    if(!std::isfinite(length) || length<.01f || length>100.f){error="Invalid native firing direction";return false;}return true;
}
bool set_native_afk_manual_aim(PPCContext& context,uint32_t actor,bool active,std::string& error){
    if(!active){afk_manual_aim.erase(actor);return true;}
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || !readable(mem_read32(actor+0x37C),0x58)){error="Native manual aim actor unavailable";return false;}
    if(!prepare_native_afk_manual_aim(error))return false;
    afk_manual_aim[actor]=mem_read32(actor+0x37C);return true;
}
bool prepare_native_afk_manual_aim(std::string& error){
    if(!original_fire_direction){auto entry=ssne_find_func(0x800C43A0);if(!entry){error="Native firing direction getter unavailable";return false;}original_fire_direction=entry->func;ssne_install_hook(0x800C43A0,afk_fire_direction_hook);}
    if(!original_pickup_selection){auto entry=ssne_find_func(0x800C5D24);if(!entry || mem_read32(entry->address)!=0x9421FFE0u){error="Native pickup weapon request unavailable";return false;}original_pickup_selection=entry->func;ssne_install_hook(0x800C5D24,afk_pickup_selection_hook);}
    if(!manual_aim_subscription)manual_aim_subscription=ssne::observe_native_entities([](const auto& notice){if(notice.event==ssne::NativeEntityNotice::Event::LevelExit)afk_manual_aim.clear();else if(notice.event==ssne::NativeEntityNotice::Event::Removing)afk_manual_aim.erase(notice.object);});
    return true;
}
void clear_native_afk_manual_aim(){afk_manual_aim.clear();}
std::map<uint32_t,uint32_t> native_afk_manual_aim_bindings(){return afk_manual_aim;}
bool validate_native_afk_manual_aim_bindings(const std::map<uint32_t,uint32_t>& bindings,std::string& error){
    if(bindings.size()>8 || (!bindings.empty() && (!original_fire_direction || !original_pickup_selection || !manual_aim_subscription))){error="AFK hooks are not initialized for checkpoint restore";return false;}
    for(const auto& [actor,aim]:bindings){
        if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || !readable(aim,0x58) || mem_read32(actor+0x37C)!=aim){error="AFK checkpoint aim binding differs from RAM";return false;}
        const auto slot=mem_read32(actor+0x17C);if(slot>=ssne::native_party_capacity() || mem_read32(ssne::native_party_registry()+slot*4)!=actor){error="AFK checkpoint actor registry differs";return false;}
    }
    return true;
}
bool restore_native_afk_manual_aim_bindings(std::map<uint32_t,uint32_t> bindings,std::string& error){
    if(!validate_native_afk_manual_aim_bindings(bindings,error))return false;
    afk_manual_aim.swap(bindings);return true;
}
std::optional<int> native_owned_primary_ammo(PPCContext& context,uint32_t actor,unsigned weapon,std::string& error) {
    if(!context_ok(context) || !validate_native_weapon_selection(actor,weapon,error))return {};
    if(!mem_read32(actor+0x4CC+weapon*24))return {}; // actual pickup ownership, not all-weapons cheat
    const auto instance=mem_read32(actor+0x4BC+weapon*24),config=mem_read32(instance+0x1D0),inventory=mem_read32(instance+0x1B0);
    if(mem_read32(instance+0x1D4)!=actor || !readable(config,60) || !readable(inventory,0xDC) || inventory!=mem_read32(actor+0x5E8) || !ssne_find_func(0x8009F444)){error="Native owned weapon ammo binding unavailable";return {};}
    const auto index=int32_t(mem_read32(config+0x38));if(index==-1)return 1;
    if(index<0 || index>=18){error="Native primary ammo index unavailable";return {};}
    auto call=context;call.r[3]=inventory;call.r[4]=uint32_t(index);call.r[5]=0;ssne_call(call,0x8009F444);return int32_t(call.r[3]);
}
bool request_native_primary_ammo(PPCContext& context,uint32_t actor,unsigned weapon,std::string& error) {
    if(!context_ok(context) || !validate_native_weapon_selection(actor,weapon,error) || !ssne_find_func(0x8009FEEC))return false;
    const auto count=mem_read32(actor+0x4C8+weapon*24);if(!count || count>2){error="Native weapon hand count unavailable";return false;}
    bool ready=true;
    for(unsigned hand=0;hand<count;++hand){const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
        if(!readable(instance,0x228) || mem_read32(instance+0x1D4)!=actor){error="Native weapon hand unavailable";return false;}
        if(mem_read32(instance+0x1E0)!=0){ready=false;if(mem_read32(instance+0x1B4)==0 && mem_read32(instance+0x1BC)>1){auto call=context;call.r[3]=instance;call.r[4]=4;call.r[5]=0;ssne_call(call,0x8009FEEC);}}
    }
    return ready;
}
namespace {
bool refresh_body_weapon(PPCContext& context,uint32_t actor,std::string& error){
    const auto body=mem_read32(actor+0x148),weapon=mem_read32(actor+0x5DC);
    if(!body)return true;
    if(!readable(body,0xA8) || weapon>=12 || !ssne_find_func(0x800C6B04u) || mem_read32(0x800C6B04u)!=0x9421FFF8u){error="Native third-person weapon selection unavailable";return false;}
    const auto desired=(weapon==1 && mem_read32(actor+0x4E0)==2)?2u:mem_read32(0x802137ACu+weapon*4);
    uint32_t resource=~0u;for(unsigned i=0;i<13;++i)if(mem_read32(0x80244870u+i*36)==desired)resource=i;
    if(mem_read32(body+0xA4)==resource)return true;
    // First-person equip (800C5F60) does not update the character's held
    // weapon. The normal player state machine calls this separate visual
    // lifecycle; a replica never runs that state machine.
    StackScratch scratch(context);auto call=context;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;ssne_call(call,0x800C6B04u);
    if(mem_read32(body+0xA4)!=resource){error="Native third-person weapon swap did not complete";return false;}
    return true;
}
}
bool apply_native_weapon_selection(PPCContext& context,uint32_t actor,unsigned weapon,bool available,std::string& error) {
    install_weapon_trace();
    if(!context_ok(context) || !validate_native_weapon_selection(actor,weapon,error)){if(error.empty())error="Native weapon-selection context unavailable";return false;}
    if(mem_read32(actor+0x5DC)==weapon)return refresh_body_weapon(context,actor,error);
    if(!available){error="Authoritative weapon is not available";return false;}
    // Private same-binary counterexample: only a hidden test may set
    // SSNE_TEST_WEAPON_SWITCH_LOOP_FIX=0 to retain the old selection route.
    // Ordinary launches always stop the old weapon's sustained voices.
    static const bool stop_old_loops=[] {const auto value=std::getenv("SSNE_TEST_WEAPON_SWITCH_LOOP_FIX");return !std::getenv("SSNE_TEST_HIDDEN") || !value || std::strcmp(value,"0")!=0;}();
    if(stop_old_loops && !ssne::stop_native_weapon_loops_for_actor(context,actor,mem_read32(actor+0x5DC),error))return false;
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=weapon;
    if(mem_read32(actor+0x4CC+weapon*24))ssne_call(call,0x800C5F60);
    else {
        // The normal pending-selection swap supports the host's all-weapons
        // debug rule without marking a weapon as picked up or granting ammo.
        mem_write32(actor+0x5F8,weapon);ssne_call(call,0x800C5C18);
        const auto count=mem_read32(actor+0x4C8+weapon*24);
        for(unsigned index=0;index<count;++index) {
            const auto instance=mem_read32(actor+0x4BC+weapon*24+index*4),vt=mem_read32(instance+0xC);
            call.r[3]=instance+uint32_t(int32_t(int16_t(mem_read16(vt+0xB8))));call.r[4]=1;
            ssne_call(call,mem_read32(vt+0xBC));
        }
        call.r[3]=mem_read32(actor+0x374);call.r[4]=weapon;ssne_call(call,0x800BDA2C);
    }
    if(mem_read32(actor+0x5DC)!=weapon){error="Native weapon selection did not complete";return false;}
    return refresh_body_weapon(context,actor,error);
}
static bool apply_native_flash_owner(PPCContext& context,uint32_t owner,bool body,const std::vector<float>& state,uint32_t mask,std::string& error,bool radial=false,unsigned resource_index=0,unsigned category=5,bool timed=true){
    if(!context_ok(context) || !readable(owner,16) || (state.size()!=14 && state.size()!=17)){error="Native muzzle flash owner unavailable";return false;}
    const auto resource_get=(body || radial)?0x8009B06Cu:0x8009B080u,create=(body || radial)?0x8009A8CCu:0x8009AA2Cu;
    for(auto address:{resource_get,create,0x8009A834u,0x8009AC48u,0x8009AC88u})if(!ssne_find_func(address)){error="Native muzzle flash lifecycle unavailable";return false;}
    if(!state[0] && !mem_read32(owner+12))return true;
    StackScratch scratch(context);auto call=context;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    if(!state[0]){if(mem_read32(owner+12)){call.r[3]=owner;ssne_call(call,0x8009AC88u);}return true;}
    if(!mem_read32(owner+4)){
        call.r[3]=resource_index;ssne_call(call,resource_get);const auto resource=call.r[3];
        if(!resource){error="Native muzzle flash resource unavailable";return false;}
        call.r[3]=owner;call.r[4]=resource;call.r[5]=body?1:category;call.ps0[1]=call.ps1[1]=1.;ssne_call(call,create);
    }
    if(!readable(mem_read32(owner+4),0x38)){error="Native muzzle flash was not created";return false;}
    if((body || radial) && state.size()==17){
        const auto resource=native_radial_flash_resource(resource_index);
        if(!resource){error="Native radial flash shape resource unavailable";return false;}
        mem_write32(call.r[1]+0x60,f2u(state[14]));mem_write32(call.r[1]+0x64,f2u(state[15]));
        call.r[3]=resource;call.r[4]=call.r[1]+0x60;ssne_call(call,0x8012BB30u);
        call.r[3]=resource;call.ps0[1]=call.ps1[1]=state[16];ssne_call(call,0x8012BB48u);
    }
    for(unsigned i=0;i<12;++i)mem_write32(call.r[1]+0x80+i*4,f2u(state[i+2]));
    call.r[3]=owner;call.r[4]=call.r[1]+0x80;ssne_call(call,0x8009A834u);
    call.r[3]=owner;call.r[4]=mask;ssne_call(call,0x8009AC48u);
    if(timed)mem_write32(owner+8,f2u(state[1]));mem_write32(owner+12,1);
    return true;
}
bool apply_native_weapon_flash(PPCContext& context,uint32_t actor,unsigned weapon,unsigned hand,const std::vector<float>& state,uint32_t mask,std::string& error,unsigned effect){
    const auto offset=native_weapon_flash_offset(weapon,effect);
    if(!offset || hand>=2 || (state.size()!=14 && state.size()!=17) || !readable(actor,0x790)){error="Invalid native muzzle flash request";return false;}
    const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
    if(!instance && !state[0])return true;
    if(!readable(instance,offset+16)){error="Native muzzle flash instance unavailable";return false;}
    return apply_native_flash_owner(context,instance+offset,false,state,mask,error,native_weapon_flash_radial(weapon,effect),weapon==9?1:0,weapon==9 && effect?1:5,native_weapon_flash_timed(weapon));
}
bool apply_native_body_flash(PPCContext& context,uint32_t actor,unsigned hand,const std::vector<float>& state,uint32_t mask,std::string& error){
    if(hand>=2 || (state.size()!=14 && state.size()!=17) || !readable(actor,0x790)){error="Invalid native held-weapon flash request";return false;}
    const auto body=mem_read32(actor+0x148);
    if(!body && !state[0])return true;
    if(!readable(body,0xE8)){error="Native held-weapon flash owner unavailable";return false;}
    // Original third-person fire effects 80154848/80154B48 use C0/D4.
    // These carry the world-space muzzle and exclude the owner's own view.
    return apply_native_flash_owner(context,body+0xC0+hand*0x14,true,state,mask,error);
}
bool update_native_muzzle_flashes(PPCContext& context,float seconds,std::string& error){
    if(!context_ok(context) || !ssne_find_func(0x8009ACF8u) || !std::isfinite(seconds) || seconds<0 || seconds>.1f){error="Native muzzle flash clock unavailable";return false;}
    StackScratch scratch(context);
    auto advance=[&](uint32_t owner){if(!readable(owner,16) || !mem_read32(owner+12))return;
        auto call=context;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=owner;call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x8009ACF8u);};
    for(unsigned slot=0;slot<ssne::native_party_capacity();++slot){
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(!readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u))continue;
        for(unsigned weapon:{1u,2u,3u,4u,8u,9u})if(native_weapon_flash_timed(weapon))for(unsigned effect=0;effect<3;++effect){const auto offset=native_weapon_flash_offset(weapon,effect);if(!offset)continue;for(unsigned hand=0;hand<2;++hand){const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);if(readable(instance,offset+16))advance(instance+offset);}}
        const auto body=mem_read32(actor+0x148);if(readable(body,0xE8))for(unsigned hand=0;hand<2;++hand)advance(body+0xC0+hand*0x14);
    }
    return true;
}
bool refresh_native_weapon_poses(PPCContext& context,std::string& error) {
    if(!context_ok(context) || !ssne_find_func(0x80129154u)){error="Native weapon animation context unavailable";return false;}
    StackScratch scratch(context);
    for(unsigned slot=0;slot<ssne::native_party_capacity();++slot){
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(!readable(actor,0x790) || !ssne::native_party_actor_occupied(actor))continue;
        const auto selected=mem_read32(actor+0x5DC);
        for(unsigned index=0;index<2;++index)if(auto binding=native_weapon_pose_binding(actor,selected,index);binding && binding->model){
            // Bone evaluation is presentation only. It does not fire weapons,
            // consume ammunition or traverse another player's world viewport.
            auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
            call.r[3]=binding->model;ssne_call(call,0x80129154u);
        }
    }
    return true;
}
bool refresh_native_actor_pose(PPCContext& context,uint32_t actor,EntityKind kind,std::string& error){
    if(!context_ok(context) || !ssne_find_func(0x80129154u)){error="Native body animation context unavailable";return false;}
    StackScratch scratch(context);
    for(unsigned part=0;part<(kind==EntityKind::Player?4u:1u);++part){
        const auto binding=kind==EntityKind::Player?native_body_pose_binding(actor,part):kind==EntityKind::Physics?native_corpse_pose_binding(actor):native_enemy_pose_binding(actor);
        if(!binding || !binding->model)continue;
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=binding->model;ssne_call(call,0x80129154u);
    }
    return true;
}
bool prepare_native_weapon_model(PPCContext& context,uint32_t actor,unsigned weapon,unsigned index,std::string& error) {
    if(!context_ok(context) || index>1 || !validate_native_weapon_selection(actor,weapon,error))return false;
    const auto instance=mem_read32(actor+0x4BC+weapon*24+index*4);
    if(!readable(instance,0x228)){error="Native weapon instance missing";return false;}
    if(mem_read32(instance))return true;
    const auto vt=mem_read32(instance+0xC);
    if(!readable(vt,0xC0) || !ssne_find_func(mem_read32(vt+0xBC)) || !readable(mem_read32(instance+4),4)){error="Native weapon model resource unavailable";return false;}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    call.r[3]=instance+uint32_t(int32_t(int16_t(mem_read16(vt+0xB8))));call.r[4]=1;ssne_call(call,mem_read32(vt+0xBC));
    if(!readable(mem_read32(instance),0x40)){error="Native weapon model was not recreated";return false;}
    return true;
}
bool validate_native_powerups(uint32_t actor,const std::vector<float>& values,std::string& error){
    if(values.size()!=18 || !readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u) || !readable(mem_read32(actor+0x374),0x208)){error="Invalid power-up HUD binding";return false;}
    for(unsigned i=0;i<6;++i)if((values[i*3]!=0 && values[i*3]!=1) || !std::isfinite(values[i*3+1]) || values[i*3+1]<0 || values[i*3+1]>1000000 || (values[i*3+2]!=0 && values[i*3+2]!=1)){error="Invalid authoritative power-up state";return false;}
    for(auto address:{0x800C1C78u,0x800C1D94u,0x800BD5BCu})if(!ssne_find_func(address)){error="Native power-up lifecycle unavailable";return false;}
    return true;
}
bool apply_native_powerups(PPCContext& context,uint32_t actor,const std::vector<float>& values,std::string& error){
    if(!context_ok(context) || !validate_native_powerups(actor,values,error))return false;
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    // These original setters only maintain the actor's power-up slots, HUD and
    // camera presentation. Do not replay a pickup, damage, or script event.
    // Remove first so a full six-widget HUD has space for new active effects.
    for(unsigned i=0;i<6;++i)if(!values[i*3] && mem_read32(actor+0x61C+i*16)){
        call.r[3]=actor;call.r[4]=i;ssne_call(call,0x800C1D94u);
    }
    for(unsigned i=0;i<6;++i)if(values[i*3]){
        const auto at=actor+0x61C+i*16;
        if(!mem_read32(at)){call.r[3]=actor;call.r[4]=i;ssne_call(call,0x800C1C78u);}
        mem_write32(at+4,f2u(values[i*3+1]));mem_write32(at+8,uint32_t(values[i*3+2]));
        call.r[3]=mem_read32(actor+0x374);call.r[4]=mem_read32(at+12);call.ps0[1]=call.ps1[1]=values[i*3+1];ssne_call(call,0x800BD5BCu);
    }
    return true;
}
bool refresh_native_bomb_ammo_hud(PPCContext& context,uint32_t actor,std::string& error) {
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u)){error="Invalid native bomb HUD context";return false;}
    if(mem_read32(actor+0x5DC)!=11)return true;
    if(!validate_native_weapon_selection(actor,11,error))return false;
    const auto hands=mem_read32(actor+0x4C8+11*24);
    if(!hands)return true; // Equip can be between releasing and activating a model.
    const auto weapon=mem_read32(actor+0x4BC+11*24),inventory=mem_read32(actor+0x5E8),hud=mem_read32(actor+0x374);
    if(hands>2 || !readable(weapon,0x254) || mem_read32(weapon+0xC)!=0x801FBDA0u || mem_read32(weapon+0x1D4)!=actor || mem_read32(weapon+0x1B0)!=inventory || !readable(inventory,72) || !readable(hud,0xA4)){error="Native bomb HUD binding unavailable";return false;}
    const auto channel=mem_read32(weapon+0x1E0),count=mem_read32(weapon+0x1BC),config=mem_read32(weapon+0x1D0);
    if(channel>=count || count>18 || !readable(config,count*60)){error="Native bomb HUD channel invalid";return false;}
    const auto row=config+channel*60;const auto index=int32_t(mem_read32(row+0x38));
    const auto number=mem_read32(hud+0x9C),icon=mem_read32(hud+0xA0);
    if(index<0 || index>=18 || !readable(mem_read32(row+0x34),0x28) || !readable(number,0xC8) || !readable(icon,0x38) || !readable(mem_read32(number+0xC4),0x18) || mem_read32(mem_read32(number+0xC4)+0x14)!=0x800ABB8Cu || !ssne_find_func(0x8009FEA0) || !ssne_find_func(0x800C4104) || mem_read32(0x8009FEA0)!=0x800301E0u || mem_read32(0x800C4104)!=0x9421FFD8u){error="Native bomb ammo HUD unavailable";return false;}
    // 8005DD3C consumes the bomb at 8005E06C but returns -1 and never marks
    // weapon+224 dirty. The original actor therefore misses its HUD refresh.
    // Read the same active channel as 800C4104 and only repair stale UI text.
    auto call=context;call.r[3]=weapon;call.r[4]=inventory;call.r[5]=0;ssne_call(call,0x8009FEA0);
    const auto expected=std::to_string(int32_t(call.r[3]));std::string displayed;
    for(unsigned i=0;i<128 && mem_read8(number+0x1C+i);++i)displayed+=char(mem_read8(number+0x1C+i));
    if(displayed==expected)return true;
    StackScratch scratch(context);call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    call.r[3]=actor;call.r[4]=1;ssne_call(call,0x800C4104);return true;
}
bool capture_native_interaction_hint(uint32_t actor,EntityState& e,std::string& error){
    const auto hud=readable(actor,0x790)?mem_read32(actor+0x374):0;
    if(!readable(hud,0x54) || mem_read32(hud+0x48)>1){error="Invalid native interaction HUD";return false;}
    const bool active=mem_read32(hud+0x48);std::string text;uint32_t flags=0;float timer=0;
    if(active){
        const auto widget=mem_read32(hud+0x44);if(!readable(widget,0xC8)){error="Missing native interaction text";return false;}
        unsigned n=0;for(;n<128 && mem_read8(widget+0x1C+n);++n)text+=char(mem_read8(widget+0x1C+n));
        if(n==128){error="Unterminated native interaction text";return false;}
        flags=mem_read32(hud+0x4C);timer=u2f(mem_read32(hud+0x50));
        if(!std::isfinite(timer) || std::abs(timer)>10000){error="Invalid native interaction lifetime";return false;}
    }
    e.properties["hint_active"]=int64_t(active);e.properties["hint_text"]=std::move(text);e.properties["hint_flags"]=int64_t(flags);e.properties["hint_timer"]=timer;return true;
}
bool validate_native_interaction_hint(uint32_t actor,const EntityState& e,std::string& error){
    auto fail=[&](const char* why){error=why;return false;};
    const auto a=e.properties.find("hint_active"),t=e.properties.find("hint_text"),f=e.properties.find("hint_flags"),d=e.properties.find("hint_timer");
    if(a==e.properties.end() || t==e.properties.end() || f==e.properties.end() || d==e.properties.end())return fail("Incomplete interaction hint");
    const auto active=std::get_if<int64_t>(&a->second),flags=std::get_if<int64_t>(&f->second);const auto text=std::get_if<std::string>(&t->second);const auto timer=std::get_if<float>(&d->second);
    if(e.kind!=EntityKind::Player || !active || (*active!=0 && *active!=1) || !flags || *flags<0 || uint64_t(*flags)>UINT32_MAX || !text || text->size()>=128 || text->find('\0')!=std::string::npos || !timer || !std::isfinite(*timer) || std::abs(*timer)>10000 || (!*active && (!text->empty() || *flags || *timer)))return fail("Invalid interaction hint metadata");
    const auto hud=readable(actor,0x790)?mem_read32(actor+0x374):0,widget=readable(hud,0x54)?mem_read32(hud+0x44):0,vt=readable(widget,0xC8)?mem_read32(widget+0xC4):0;
    if(!readable(vt,0x28) || mem_read16(vt+0x18) || mem_read32(vt+0x1C)!=0x800ABBE4u || mem_read32(0x800ABBE4)!=0x9421FFE8u || !ssne_find_func(0x800ABBE4) || !ssne_find_func(0x800BBD10))return fail("Native interaction text setter unavailable");return true;
}
bool apply_native_interaction_hint(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_interaction_hint(actor,e,error))return false;
    const auto hud=mem_read32(actor+0x374);const auto active=std::get<int64_t>(e.properties.at("hint_active"));
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    if(!active){if(mem_read32(hud+0x48)){call.r[3]=hud;ssne_call(call,0x800BBD10);}return true;}
    const auto widget=mem_read32(hud+0x44);const auto& text=std::get<std::string>(e.properties.at("hint_text"));std::string local;
    for(unsigned i=0;i<128 && mem_read8(widget+0x1C+i);++i)local+=char(mem_read8(widget+0x1C+i));
    if(local!=text || !mem_read32(hud+0x48) || mem_read32(widget+0xB8)){for(unsigned i=0;i<=text.size();++i)mem_write8(call.r[1]+0x80+i,i<text.size()?uint8_t(text[i]):0);call.r[3]=widget;call.r[4]=call.r[1]+0x80;ssne_call(call,0x800ABBE4);}
    mem_write32(hud+0x48,1);mem_write32(hud+0x4C,uint32_t(std::get<int64_t>(e.properties.at("hint_flags"))));mem_write32(hud+0x50,f2u(std::get<float>(e.properties.at("hint_timer"))));return true;
}
#include "native_player_notices.inc"
#include "native_hud_combo.inc"
#include "native_reentry_hud.inc"
#include "native_weapon_bar.inc"
#include "native_weapon_sidebar.inc"
bool refresh_native_player_hud(PPCContext& context,uint32_t actor,std::string& error) {
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u)){error="Invalid native player presentation context";return false;}
    const auto hud=mem_read32(actor+0x374);const auto* update=ssne_find_func(0x800BB280);
    if(!readable(hud,0x3E4) || !update || mem_read32(update->address)!=0x9421FFE8u){error="Native HUD presentation unavailable";return false;}
    const auto number=mem_read32(hud+0x88),bar=mem_read32(hud+0x8C);const auto* health_set=ssne_find_func(0x800BC3E0);
    const float health=u2f(mem_read32(actor+0xCC));
    if(!std::isfinite(health) || std::abs(health)>1000000.f || !health_set || mem_read32(health_set->address)!=0x9421FFD0u || !readable(number,0xC8) || !readable(bar,0x50) || !readable(mem_read32(number+0xC4),0x20) || !readable(mem_read32(bar+0x3C),0x20) || mem_read32(mem_read32(number+0xC4)+0x14)!=0x800ABB8Cu || mem_read32(mem_read32(bar+0x3C)+0x1C)!=0x800ADC30u){error="Native health HUD setters unavailable";return false;}
    // Actor health replication alone does not update the number/bar widgets.
    // In particular, the native movement/respawn lifecycle can recreate them
    // with0 while authoritative health already equals100. Use the game's UI-
    // only health setter when the displayed text differs; no damage/heal event.
    const auto expected=std::to_string(int32_t(health));std::string displayed;
    for(unsigned i=0;i<128 && mem_read8(number+0x1C+i);++i)displayed+=char(mem_read8(number+0x1C+i));
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    if(displayed!=expected){call.r[3]=hud;call.r[4]=uint32_t(int32_t(health));ssne_call(call,health_set->address);}
    const auto armor_number=mem_read32(hud+0x94),armor_bar=mem_read32(hud+0x98),armor_icon=mem_read32(hud+0x90);
    const auto* armor_set=ssne_find_func(0x800BC498);const auto armor=u2f(mem_read32(actor+0x390));
    if(!std::isfinite(armor) || armor<0 || armor>1000000.f || !armor_set || mem_read32(armor_set->address)!=0x9421FFD8u || !readable(armor_number,0xC8) || !readable(armor_bar,0x50) || !readable(armor_icon,4) || !readable(mem_read32(armor_number+0xC4),0x20) || !readable(mem_read32(armor_bar+0x3C),0x20) || mem_read32(mem_read32(armor_number+0xC4)+0x14)!=0x800ABB8Cu || mem_read32(mem_read32(armor_bar+0x3C)+0x1C)!=0x800ADC30u){error="Native armor HUD setters unavailable";return false;}
    // UI-only setter also hides the armor widgets at zero. Replica actors do
    // not execute pickup/damage logic, so their HUD must follow host armor.
    call.r[3]=hud;call.r[4]=uint32_t(int32_t(armor));ssne_call(call,armor_set->address);
    // Score awards do not execute on replicas. Update the original score
    // widget directly, without replaying an award or triggering level scripts.
    if(mem_read32(hud+0x20)!=1 && mem_read32(hud+0x20)!=3){
    const auto score=u2f(mem_read32(actor+(mem_read32(context.r[13]-0x69D4)==3?0x6A0:0x68C)));
    const auto score_number=mem_read32(hud+0xA4);const auto* score_set=ssne_find_func(0x800BC570u);
    if(!std::isfinite(score) || std::abs(score)>1000000000.f || !score_set || mem_read32(score_set->address)!=0x9421FFD8u || !readable(score_number,0xC8) || !readable(mem_read32(score_number+0xC4),0x18) || mem_read32(mem_read32(score_number+0xC4)+0x14)!=0x800ABB8Cu){error="Native score HUD unavailable";return false;}
    std::string score_text;for(unsigned i=0;i<128 && mem_read8(score_number+0x1C+i);++i)score_text+=char(mem_read8(score_number+0x1C+i));
    if(score_text!=std::to_string(int32_t(score))){call.r[3]=hud;call.r[4]=uint32_t(int32_t(score));call.ps0[1]=call.ps1[1]=1.;ssne_call(call,score_set->address);}
    }
    // Original UI-only ammunition refresh: sums the active pistol hands,
    // selects the current ammo channel, and updates its number and icon.
    // It does not tick the weapon, fire it, or consume inventory.
    const auto selected=mem_read32(actor+0x5DC);
    if(!validate_native_weapon_selection(actor,selected,error))return false;
    const auto hands=mem_read32(actor+0x4C8+selected*24);
    if(hands>2 || !readable(mem_read32(actor+0x5E8),72) ||
       !readable(mem_read32(hud+0x9C),0xC8) || !readable(mem_read32(hud+0xA0),0x38) ||
       !ssne_find_func(0x800C4104) || mem_read32(0x800C4104)!=0x9421FFD8u){error="Native ammo HUD unavailable";return false;}
    for(unsigned hand=0;hand<std::max(1u,hands);++hand){
        const auto weapon=mem_read32(actor+0x4BC+selected*24+hand*4);
        if(!readable(weapon,0x1E4) || mem_read32(weapon+0x1D4)!=actor){error="Native ammo HUD weapon binding invalid";return false;}
        const auto channel=mem_read32(weapon+0x1E0),count=mem_read32(weapon+0x1BC),config=mem_read32(weapon+0x1D0);
        if(channel>=count || count>18 || !readable(config,60*count)){error="Native ammo HUD channel invalid";return false;}
        const auto row=config+channel*60;const auto index=int32_t(mem_read32(row+0x38));
        if(index<-1 || index>=18 || !readable(mem_read32(row+0x34),0x28)){error="Native ammo HUD resource invalid";return false;}
    }
    call.r[3]=actor;call.r[4]=0;ssne_call(call,0x800C4104);
    // Applying a snapshot synchronizes text/layout without advancing time.
    // HUD animations run once per rendered client frame below, independently
    // of snapshot arrival frequency.
    call.r[3]=hud;call.ps0[1]=call.ps1[1]=0.;
    ssne_call(call,0x800BB280);return true;
}
bool advance_native_player_huds(PPCContext& context,const std::vector<uint32_t>& actors,
    NativeHudPresentationClock& clock,uint64_t frame,std::chrono::steady_clock::time_point now,std::string& error) {
    if(clock.initialized && clock.frame==frame)return true;
    if(!context_ok(context) || actors.size()>max_players || !ssne_find_func(0x800BB280) || mem_read32(0x800BB280)!=0x9421FFE8u || !ssne_find_func(0x800ADF68) || mem_read32(0x800ADF68)!=0x9421FFB8u){error="Invalid native HUD frame context";return false;}
    std::set<uint32_t> huds;
    for(const auto actor:actors){
        if(!readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u) || !readable(mem_read32(actor+0x374),0x534) || !huds.insert(mem_read32(actor+0x374)).second){error="Invalid native HUD frame binding";return false;}
        const auto hud=mem_read32(actor+0x374);
        if(!readable(mem_read32(hud+0x4B8),0x20) || !readable(mem_read32(hud+0x208),0x20)){error="Invalid native HUD overlay binding";return false;}
    }
    if(clock.initialized && now<clock.time){error="Native HUD presentation clock moved backwards";return false;}
    double remaining=clock.initialized && frame>clock.frame?std::chrono::duration<double>(now-clock.time).count():0.;
    // 800BB280 advances only HUD/widget presentation: in particular its two
    // 800ADF68 calls fade the bomb and hurt overlays. Actor power-up, weapon,
    // camera and damage simulation are not called. Consume the full wall time
    // in small steps so low frame rates and stalled snapshots do not stretch
    // effect lifetimes; never reuse the other effects' clamped 0.1-second dt.
    StackScratch scratch(context);
    for(unsigned steps=0;steps<600 && remaining>0. && !huds.empty();++steps){
        const double step=std::min(remaining,.1);
        for(const auto hud:huds){
            auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
            call.r[3]=hud;call.ps0[1]=call.ps1[1]=double(float(step));ssne_call(call,0x800BB280);
        }
        remaining-=step;
    }
    // After a long suspend, bound full-HUD catch-up to 60 seconds. Its other
    // widgets have data-driven transition times; a huge dt is not safe for all
    // their interpolators. The two original overlay ticks explicitly test
    // elapsed>duration before interpolation, so they alone consume the rest
    // in constant time and hide expired flashes immediately. No backlog can
    // incorrectly age a new flash received after this frame.
    if(remaining>0.)for(const auto hud:huds)if(!mem_read32(hud+0x28))for(const auto offset:{0x4B8u,0x208u}){
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
        call.r[3]=mem_read32(hud+offset);call.ps0[1]=call.ps1[1]=double(float(remaining));ssne_call(call,0x800ADF68);
    }
    clock={now,frame,true};return true;
}
bool apply_native_camera_control(PPCContext& context,uint32_t actor,bool frozen,std::string& error) {
    if(!context_ok(context) || !readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u)){error="Invalid native player camera transition";return false;}
    if(mem_read32(actor+0x370)==unsigned(frozen))return true;
    const auto function=frozen?0x800B31B8u:0x800B3398u;
    const auto* change=ssne_find_func(function);
    if(!change || mem_read32(function)!=(frozen?0x9421FF78u:0x9421FFF0u)){error="Native camera lifecycle unavailable";return false;}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=0;
    ssne_call(call,function);return true;
}
bool validate_native_movement_mode(uint32_t actor,unsigned mode,std::string& error) {
    if(mode>3 || !readable(actor,0x790) || mem_read32(actor+0x48)!=ssne::game_address(0x80212268u)){error="Invalid native player movement mode";return false;}
    const auto driver=mem_read32(actor+0x348+mode*4);
    const auto* change=ssne_find_func(0x800B5A78);
    if(!readable(driver,4) || !readable(mem_read32(driver),0x20) || !change || mem_read32(change->address)!=0x9421FFE8u){error="Native movement-controller lifecycle unavailable";return false;}
    return true;
}
bool prepare_native_player_body(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    if(!context_ok(context) || e.kind!=EntityKind::Player || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u){error="Native body preparation context unavailable";return false;}
    if(mem_read32(actor+0x340)==3 && mem_read32(actor+0x6C4) && !mem_read32(actor+0x148))return true;
    for(auto [address,signature]:{std::pair{0x800B4DCCu,0x9421FED0u},std::pair{0x80129154u,0x9421FF40u}})if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Original body resource preparation unavailable";return false;}
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    if(!mem_read32(actor+0x148)){
        // 800B3398 legitimately drops the body for a local single-view or
        // mode3 release. A replica may still need the host's third-person body.
        // Recreate its original local resources; do not replay spawn/death.
        call.r[3]=actor;call.r[4]=mem_read32(actor+0x170);ssne_call(call,0x800B4DCCu);
    }
    for(unsigned part=0;part<2;++part)if(e.properties.contains("body"+std::to_string(part)+"_bone_matrices") && !native_body_pose_binding(actor,part)){
        const auto binding=native_render_binding(actor,EntityKind::Player,part);const auto model=binding && binding->model?mem_read32(binding->model+0x20):0;
        if(!readable(model,0xB0) || mem_read32(model)!=0x8021D668u){error="Native body has no verified animated model";return false;}
        call.r[3]=model;ssne_call(call,0x80129154u);
        if(!native_body_pose_binding(actor,part)){error="Original body skeleton preparation did not complete";return false;}
    }
    return true;
}
bool apply_native_body_weapon_resource(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    const auto property=e.properties.find("body_weapon_resource");if(property==e.properties.end())return true;
    const auto* resource=std::get_if<int64_t>(&property->second);const auto body=readable(actor,0x790)?mem_read32(actor+0x148):0;
    if(!context_ok(context) || !resource || *resource< -1 || *resource>12 || !readable(body,0xF4) || !ssne_find_func(0x801529D0u) || mem_read32(0x801529D0u)!=0x9421FFD8u){error="Original held weapon resource unavailable";return false;}
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    if(mem_read32(body+0xA4)!=uint32_t(*resource)){call.r[3]=body;call.r[4]=*resource<0?~0u:mem_read32(0x80244870u+unsigned(*resource)*36);ssne_call(call,0x801529D0u);if(mem_read32(body+0xA4)!=uint32_t(*resource)){error="Original held weapon resource did not change";return false;}}
    for(unsigned part=2;part<4;++part)if(e.properties.contains("body"+std::to_string(part)+"_bone_matrices") && !native_body_pose_binding(actor,part)){
        const auto proxy=native_render_binding(actor,EntityKind::Player,part);const auto model=proxy && proxy->model?mem_read32(proxy->model+0x20):0;
        if(!readable(model,0xB0) || mem_read32(model)!=0x8021D668u){error="Held weapon has no verified animated model";return false;}
        call.r[3]=model;ssne_call(call,0x80129154u);if(!native_body_pose_binding(actor,part)){error="Original held weapon skeleton preparation did not complete";return false;}
    }
    return true;
}
bool apply_native_movement_mode(PPCContext& context,uint32_t actor,unsigned mode,std::string& error) {
    if(!context_ok(context) || !validate_native_movement_mode(actor,mode,error))return false;
    if(mem_read32(actor+0x340)==mode)return true;
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=mode;
    ssne_call(call,0x800B5A78);return true;
}
#include "native_boss_parts.inc"
bool validate_native_enemy_creation(const EntityState& e,std::string& error) {
    auto fail=[&](const char* why){error=why;return false;};
    if(!validate_render_metadata(e,error))return false;
    if(e.kind!=EntityKind::Enemy || !e.id || prototype(e.definition)<0)return fail("Enemy has no native level prototype");
    const auto pose_fields=e.properties.contains("enemy_bone_count")+e.properties.contains("enemy_bone_layout")+e.properties.contains("enemy_bone_matrices");
    if(pose_fields && pose_fields!=3)return fail("Incomplete enemy skeleton creation metadata");
    for(const auto& [key,value]:e.properties) {
        if(render_field(key))continue;
        if(key=="position" || key=="spawn_position") {
            auto p=std::get_if<std::array<float,3>>(&value);if(!p)return fail("Enemy transform has the wrong type");
            for(auto f:*p)if(!std::isfinite(f) || std::abs(f)>1000000)return fail("Invalid enemy transform");
        } else if(key=="enemy_rotation" || key=="enemy_scale") {
            const auto* v=std::get_if<std::array<float,3>>(&value);if(!v)return fail("Enemy transform has the wrong type");
            for(auto f:*v)if(!std::isfinite(f) || (key=="enemy_rotation"?std::abs(f)>10000.f:(f<=0 || f>100)))return fail("Invalid enemy transform coefficient");
        } else if(key=="enemy_bone_count") {
            const auto* count=std::get_if<int64_t>(&value);if(!count || *count<1 || *count>256)return fail("Invalid enemy skeleton count");
        } else if(key=="enemy_bone_layout") {
            if(!std::holds_alternative<int64_t>(value))return fail("Invalid enemy skeleton signature");
        } else if(key=="enemy_bone_matrices") {
            const auto* matrices=std::get_if<std::vector<float>>(&value);const auto count=e.properties.find("enemy_bone_count");
            if(!matrices || count==e.properties.end() || !std::holds_alternative<int64_t>(count->second) || std::get<int64_t>(count->second)<1 || std::get<int64_t>(count->second)>256 || matrices->size()!=size_t(std::get<int64_t>(count->second))*12 || !e.properties.contains("enemy_bone_layout"))return fail("Incomplete enemy skeleton pose");
            for(auto f:*matrices)if(!std::isfinite(f) || std::abs(f)>10000.f)return fail("Invalid enemy skeleton coefficient");
        } else if(key=="boss_weakpoints"){
            if(!validate_native_boss_parts(e,error))return false;
        } else if(key=="health") {
            auto health=std::get_if<float>(&value);if(!health || !std::isfinite(*health) || std::abs(*health)>1000000)return fail("Invalid enemy health");
        } else if(key=="rotor_visible" || key=="rotor_mask"){
            const auto* v=std::get_if<int64_t>(&value);if(e.definition!=20 || !v || *v<0 || uint64_t(*v)>(key=="rotor_visible"?1u:UINT32_MAX))return fail("Invalid created propeller visibility");
        } else if(key=="rotor_row0" || key=="rotor_row1" || key=="rotor_row2"){
            const auto* v=std::get_if<std::array<float,4>>(&value);if(e.definition!=20 || !v)return fail("Invalid created propeller matrix");for(float f:*v)if(!std::isfinite(f) || std::abs(f)>1000000.f)return fail("Invalid created propeller coefficient");
        } else return fail("Enemy creation property has no native schema");
    }
    unsigned rotor_fields=0;for(const auto key:{"rotor_visible","rotor_mask","rotor_row0","rotor_row1","rotor_row2"})rotor_fields+=e.properties.contains(key);if(rotor_fields && rotor_fields!=5)return fail("Incomplete created propeller state");
    if(!e.properties.contains("position") || !e.properties.contains("spawn_position"))return fail("Enemy creation needs its authoritative spawn transform");
    const auto level=mem_read32(ssne::game_address(0x802DA5B8));
    const auto* factory=ssne_find_func(ssne::game_address(0x8010D70C));
    if(!readable(level,0x60) || !factory || mem_read32(factory->address)!=0x9421FF90u)return fail("Native enemy factory or active level unavailable");
    return true;
}
uint32_t create_native_enemy(PPCContext& context,const EntityState& e,std::string& error) {
    if(!context_ok(context) || !validate_native_enemy_creation(e,error)){if(error.empty())error="Native creation context unavailable";return 0;}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;
    const auto matrix=call.r[1]+0x80,proto=call.r[1]+0xC0;
    const auto& position=std::get<std::array<float,3>>(e.properties.at("spawn_position"));
    mem_write32(call.r[1],context.r[1]);
    for(unsigned i=0;i<12;++i)mem_write32(matrix+i*4,f2u(i==0 || i==5 || i==10?1.f:0.f));
    for(unsigned axis=0;axis<3;++axis)mem_write32(matrix+12+axis*16,f2u(position[axis]));
    for(unsigned i=0;i<8;++i)mem_write32(proto+4*i,0);mem_write32(proto,uint32_t(prototype(e.definition)));
    call.r[3]=mem_read32(ssne::game_address(0x802DA5B8));call.r[4]=matrix;call.r[5]=proto;
    call.r[6]=0;call.r[7]=1;call.r[8]=call.r[9]=call.r[10]=0;
    ssne_call(call,ssne::game_address(0x8010D70C));
    if(!readable(call.r[3],0xD4)){error="Native enemy factory did not create an actor";return 0;}return call.r[3];
}
bool validate_native_pickup_metadata(const EntityState& e,std::string& error) {
    const auto name=e.properties.find("pickup_model"),score=e.properties.find("pickup_value");
    if(e.kind!=EntityKind::Pickup || e.definition!=54){if(name!=e.properties.end() || score!=e.properties.end()){error="Custom pickup metadata targets another class";return false;}return true;}
    const auto* model=name==e.properties.end()?nullptr:std::get_if<std::string>(&name->second);
    const auto* value=score==e.properties.end()?nullptr:std::get_if<int64_t>(&score->second);
    if(!model || model->empty() || model->size()>255 || model->find("..")!=std::string::npos || !value || *value<INT32_MIN || *value>INT32_MAX){error="Invalid custom pickup model/value metadata";return false;}
    for(const unsigned char c:*model)if(c<32 || c>126 || c==':'){error="Invalid custom pickup model resource name";return false;}
    return true;
}
namespace {
bool retire_native_enemy_auxiliary(PPCContext& context,uint32_t object,std::string& error){
    const auto vt=mem_read32(object+0x48);
    if(vt==0x801F6908u || vt==0x801EF6C0u){
        const unsigned start=vt==0x801F6908u?0x250:0x244,count=vt==0x801F6908u?2:3;
        for(unsigned i=0;i<count;++i){const auto child=mem_read32(object+start+i*4);
            if(child && readable(child,0x130) && mem_read32(child+0x48)==0x8020B990u && mem_read32(child+0xC8)==object){if(!remove_native_entity(context,child,error))return false;mem_write32(object+start+i*4,0);}}
        return true;
    }
    if(vt!=0x801EB9F8u)return true;
    if(!readable(object,0x250)){error="Native rotor owner is truncated";return false;}
    const auto child=mem_read32(object+0x24C);if(!child)return true;
    if(!readable(child,0x118) || mem_read32(child+0x48)!=0x8020B228u){error="Native propeller class differs";return false;}
    // Also used for the temporary enemy that supplies corpse geometry. Its
    // ordinary parent destructor does not own the separate propeller actor.
    if(!remove_native_entity(context,child,error))return false;
    mem_write32(object+0x24C,0);return true;
}
}
#include "native_corpse.inc"
#include "native_npc_piece.inc"
bool validate_native_detached_rotor(const EntityState& e,std::string& error){
    if(e.kind!=EntityKind::Physics || e.definition!=4 || !e.id || !validate_render_metadata(e,error)){error="Invalid detached propeller identity/render state";return false;}
    for(const auto& [key,value]:e.properties){
        if(render_field(key))continue;
        const auto* v=std::get_if<std::array<float,3>>(&value);
        if((key!="position" && key!="spawn_position") || !v){error="Unsupported detached propeller property";return false;}
        for(auto f:*v)if(!std::isfinite(f) || std::abs(f)>1000000.f){error="Invalid detached propeller position";return false;}
    }
    for(auto key:{"position","spawn_position","render_visible","render_row0","render_row1","render_row2"})if(!e.properties.contains(key)){error="Incomplete detached propeller";return false;}
    if(mem_read32(0x80000000)!=0x47334250u || !ssne_find_func(0x80087E04u) || mem_read32(0x80087E04u)!=0x9421FFF0u){error="Original propeller release unavailable";return false;}
    return true;
}
uint32_t create_native_detached_rotor(PPCContext& context,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_detached_rotor(e,error))return 0;
    EntityState seed;seed.id=e.id;seed.kind=EntityKind::Enemy;seed.definition=20;
    seed.properties["position"]=seed.properties["spawn_position"]=e.properties.at("spawn_position");
    // Original enemy factory supplies the propeller resource and renderer.
    // Transfer that child before disposing the temporary geometry source.
    const auto source=create_native_enemy(context,seed,error);if(!source)return 0;
    const auto child=mem_read32(source+0x24C);
    if(!readable(child,0x118) || mem_read32(child+0x48)!=0x8020B228u || !validate_native_removal(context,source,error))return 0;
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    const auto velocity=call.r[1]+0x80;for(unsigned i=0;i<3;++i)mem_write32(velocity+i*4,0);
    call.r[3]=child;call.r[4]=velocity;ssne_call(call,0x80087E04u); // detach and stop only its looping voice
    mem_write32(source+0x24C,0);call.r[3]=source;ssne_call(call,0x8009B288u);
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Spawned,child,0,-2,std::get<std::array<float,3>>(e.properties.at("spawn_position"))});
    return child;
}
bool capture_native_pickup_metadata(uint32_t object,EntityState& e,std::string& error) {
    if(e.kind!=EntityKind::Pickup || e.definition!=54)return true;
    if(!readable(object,0x184) || mem_read32(object+0x48)!=0x80214448u){error="Invalid custom pickup class";return false;}
    const auto config=mem_read32(object+0xDC),meta=readable(config,0x30)?mem_read32(config+0x2C):0;
    const auto name=readable(meta,8)?mem_read32(meta):0;
    if(!readable(meta,8) || !readable(name,1)){error="Native custom pickup metadata unavailable";return false;}
    std::string model;unsigned i=0;
    for(;i<=255 && readable(name+i,1) && mem_read8(name+i);++i)model+=char(mem_read8(name+i));
    if(i>255 || !readable(name+i,1) || mem_read8(name+i)){error="Native custom pickup resource name exceeds schema";return false;}
    e.properties["pickup_model"]=std::move(model);e.properties["pickup_value"]=int64_t(int32_t(mem_read32(meta+4)));
    return validate_native_pickup_metadata(e,error);
}
bool match_native_pickup_metadata(uint32_t object,const EntityState& e,std::string& error) {
    if(!validate_native_pickup_metadata(e,error))return false;
    if(e.kind!=EntityKind::Pickup || e.definition!=54)return true;
    EntityState local;local.kind=e.kind;local.definition=e.definition;
    if(!capture_native_pickup_metadata(object,local,error))return false;
    if(local.properties.at("pickup_model")!=e.properties.at("pickup_model") || local.properties.at("pickup_value")!=e.properties.at("pickup_value")){error="Custom pickup immutable model/value differs locally";return false;}
    return true;
}
bool validate_native_weapon_pickup_creation(const EntityState& e,std::string& error) {
    auto fail=[&](const char* why){error=why;return false;};
    if(!validate_render_metadata(e,error) || !validate_native_pickup_metadata(e,error) || !validate_pickup_glow_metadata(e,error))return false;
    // 8010CEB8 routes ordinary health/armor/ammo/weapons (0..45) and
    // Serious Bomb (55) through the same 80213CC8 pickup class. Mission
    // subclasses 46..53 and random remaps still need their own schema.
    if(e.kind!=EntityKind::Pickup || !e.id || (e.definition>45 && e.definition!=54 && e.definition!=55))return fail("Unsupported native dynamic pickup definition");
    for(const auto& [key,value]:e.properties) {
        if(render_field(key) || key=="pickup_model" || key=="pickup_value")continue;
        if(key=="position" || key=="spawn_position"){const auto* p=std::get_if<std::array<float,3>>(&value);if(!p)return fail("Invalid pickup position type");for(auto f:*p)if(!std::isfinite(f) || std::abs(f)>1000000.f)return fail("Invalid pickup position");}
        else if(key=="pickup_ownership"){const auto* flags=std::get_if<std::vector<int64_t>>(&value);if(!flags || flags->size()!=ssne::native_party_capacity())return fail("Invalid pickup ownership");for(auto flag:*flags)if(flag!=0 && flag!=1)return fail("Invalid pickup ownership flag");}
        else if(key=="pickup_view_mask") {const auto* n=std::get_if<int64_t>(&value);if(!n || *n<0 || *n>UINT32_MAX)return fail("Invalid dynamic pickup view mask");}
        else if(key=="pickup_glow_pose"){const auto* pose=std::get_if<std::vector<float>>(&value);if(!pose || (pose->size()!=0 && pose->size()!=12))return fail("Invalid dynamic pickup glow pose");for(auto v:*pose)if(!std::isfinite(v) || std::abs(v)>1000000.f)return fail("Invalid dynamic pickup glow coefficient");}
        else if(key=="pickup_glow") {const auto* v=std::get_if<std::vector<int64_t>>(&value);if(!v || v->size()!=2 || (*v)[0]<0 || (*v)[0]>1 || (*v)[1]<0 || (*v)[1]>UINT32_MAX)return fail("Invalid dynamic pickup glow state");}
        else return fail("Unsupported dynamic pickup property");
    }
    if(!e.properties.contains("position") || !e.properties.contains("spawn_position") || !e.properties.contains("pickup_ownership"))return fail("Incomplete dynamic pickup state");
    if(mem_read32(0x80000000)!=0x47334250u || !readable(mem_read32(0x802DACC0),0x60) || !ssne_find_func(0x8010CEB8) || mem_read32(0x8010CEB8)!=0x9421FFD0u || !ssne_find_func(0x80116084) || !ssne_find_func(0x801161EC))return fail("Native pickup factory unavailable");
    return true;
}
uint32_t create_native_weapon_pickup(PPCContext& context,const EntityState& e,std::string& error) {
    if(!context_ok(context) || !validate_native_weapon_pickup_creation(e,error))return 0;
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    const auto model=e.definition==54?std::get<std::string>(e.properties.at("pickup_model")):std::string{};
    const auto value=e.definition==54?uint32_t(std::get<int64_t>(e.properties.at("pickup_value"))):0u;
    ssne::NativePickupConfig config(call,e.definition,model,value);const auto proto=config.address();
    if(!proto){error="Native pickup prototype allocation failed";return 0;}
    const auto matrix=call.r[1]+0x80;const auto& position=std::get<std::array<float,3>>(e.properties.at("spawn_position"));
    for(unsigned i=0;i<12;++i)mem_write32(matrix+i*4,f2u(i==0 || i==5 || i==10?1.f:0.f));for(unsigned i=0;i<3;++i)mem_write32(matrix+12+i*16,f2u(position[i]));
    call.r[3]=mem_read32(0x802DACC0);call.r[4]=0;call.r[5]=matrix;call.r[6]=proto;call.r[7]=0;ssne_call(call,0x8010CEB8);const auto object=call.r[3];
    if(readable(object,0x1D4) && mem_read32(object+0xDC)==proto)config.transfer_to(object);
    if(!readable(object,0x1D4) || mem_read32(object+0x48)!=(e.definition==54?0x80214448u:0x80213CC8u) || mem_read32(object+0x180)!=e.definition){error="Native weapon pickup factory failed";return 0;}
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,object,e.definition});
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Spawned,object,0,-2,position});
    return object;
}
bool validate_native_rocket_creation(const EntityState& e,std::string& error) {
    auto fail=[&](const char* why){error=why;return false;};
    if(e.kind!=EntityKind::Projectile || e.definition!=0 || !e.id)return fail("Unsupported native projectile definition");
    for(const auto& [key,value]:e.properties) {
        if(key=="position" || key=="spawn_position" || key=="velocity") {
            const auto* v=std::get_if<std::array<float,3>>(&value);if(!v)return fail("Invalid rocket vector type");
            for(auto f:*v)if(!std::isfinite(f) || std::abs(f)>(key=="velocity"?10000.f:1000000.f))return fail("Invalid rocket vector coefficient");
        } else if(key=="projectile_source") {if(!std::holds_alternative<EntityReference>(value))return fail("Invalid rocket source reference");}
        else return fail("Rocket property has no verified native schema");
    }
    for(const auto* key:{"position","spawn_position","velocity","projectile_source"})if(!e.properties.contains(key))return fail("Incomplete rocket state");
    if(mem_read32(0x80000000)!=0x47334250u || !readable(mem_read32(0x802ABAAC),4))return fail("Native rocket manager unavailable");
    for(auto address:{0x8009DD48u,0x8009EB94u,0x801505F8u,0x80075EA4u})if(!ssne_find_func(address))return fail("Native rocket lifecycle unavailable");
    if(mem_read32(0x802022E0u+0x4C)!=0x80075EA4u)return fail("Native rocket class signature differs");
    return true;
}
uint32_t create_native_rocket(PPCContext& context,const EntityState& e,uint32_t source,std::string& error) {
    if(!context_ok(context) || context.r[1]<0x80004000u || !readable(context.r[1]-0x4000,0x4000) || !validate_native_rocket_creation(e,error)){if(error.empty())error="Native rocket creation context unavailable";return 0;}
    if(source && (!readable(source,0x54) || !readable(mem_read32(source+0x48),0x108))){error="Native rocket source unavailable";return 0;}
    // Preserve the interrupted native frame, including nested resource initialization.
    const auto start=context.r[1]-0x4000;std::vector<uint8_t> saved(0x4000);
    for(unsigned i=0;i<saved.size();++i)saved[i]=mem_read8(start+i);
    struct Restore{uint32_t start;std::vector<uint8_t>& saved;~Restore(){for(unsigned i=0;i<saved.size();++i)mem_write8(start+i,saved[i]);}}restore{start,saved};
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    call.r[3]=0;ssne_call(call,0x8009DD48);const auto object=call.r[3];
    if(!readable(object,0x164) || mem_read32(object+0x48)!=0x802022E0u){error="Native rocket factory returned another class";return 0;}
    const auto& position=std::get<std::array<float,3>>(e.properties.at("position"));
    const auto& velocity=std::get<std::array<float,3>>(e.properties.at("velocity"));
    for(unsigned i=0;i<3;++i){mem_write32(object+0x20+i*4,f2u(position[i]));mem_write32(object+0x104+i*4,f2u(velocity[i]));}
    call.r[3]=object;call.r[4]=source;ssne_call(call,0x8009EB94);
    call.r[3]=mem_read32(0x802ABAAC);call.r[4]=object;ssne_call(call,0x801505F8);
    return object;
}
bool present_native_rocket(PPCContext& context,uint32_t object,std::string& error) {
    if(!context_ok(context) || !readable(object,0x164) || mem_read32(object+0x48)!=0x802022E0u){error="Invalid native rocket presentation";return false;}
    // Only the visual setters from 80076328. Never call its collision/damage
    // update, nor 8009E798 (which also performs collision and removal).
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    auto invoke=[&](uint32_t target,uint32_t address){call.r[3]=target;call.r[4]=object+0x20;ssne_call(call,address);};
    const auto model=mem_read32(object+0x160);
    if(model){const auto vt=readable(model,4)?mem_read32(model):0;if(!readable(vt,0x30) || !ssne_find_func(mem_read32(vt+0x2C))){error="Native rocket model transform unavailable";return false;}call.r[3]=model+uint32_t(int32_t(int16_t(mem_read16(vt+0x28))));ssne_call(call,mem_read32(vt+0x2C));const auto matrix=call.r[3];if(!readable(matrix,48)){error="Invalid native rocket model matrix";return false;}for(unsigned i=0;i<3;++i)mem_write32(matrix+12+i*16,mem_read32(object+0x20+i*4));}
    if(const auto trail=mem_read32(object+0x128))invoke(trail,0x800EEF8C);
    if(const auto particles=mem_read32(object+0x12C))invoke(particles,0x800EF784);
    const auto light=mem_read32(object+0x124);
    if(light){const auto vt=readable(light,4)?mem_read32(light):0;if(!readable(vt,0x18) || !ssne_find_func(mem_read32(vt+0x14))){error="Native rocket light setter unavailable";return false;}invoke(light+uint32_t(int32_t(int16_t(mem_read16(vt+0x10)))),mem_read32(vt+0x14));}
    return true;
}
namespace {
constexpr NativeProjectileClass projectile_classes[]{
    {0,0x802022E0,0x164,0x80075EA4,0x128,0x80076AC0},
    {1,0x801FF400,0x168,0x8006E2C0,0x12C,0x8006F068},
    {2,0x80201650,0x164,0x80074578,0x124,0x80074E64},
    {3,0x801FEB30,0x134,0x8006D60C,0x130,0x8006DD14},
    {4,0x802000C8,0x140,0x800706D8,0x13C,0x80070F90},
    {5,0x80201AF0,0x144,0x80074EAC,0x140,0x80075E34},
    {6,0x80200598,0x17C,0x80071004,0,0x80072B20},
    {8,0x801FCAE8,0x138,0x80062224,0,0x80062FAC},
    {9,0x801FF860,0x140,0x8006F0DC,0x124,0x8006FA48},
    {10,0x801FE6F8,0x18C,0x8006A42C,0x158,0x8006D568},
    {11,0x80200E10,0x17C,0x800737A0,0x160,0x80074010},
    {12,0x801FD0B0,0x144,0x80064F28,0,0x80066334},
    {13,0x80201F50,0x168,0x80075EA4,0x128,0x80076B18},
    {14,0x801FD948,0x188,0x80067854,0x158,0x80068A58},
    {15,0x801FD518,0x194,0x800665D4,0,0x8006768C},
    {16,0x801FEF58,0x130,0x8006DD88,0x12C,0x8006E278},
    {17,0x802009D8,0x174,0x80072B68,0,0x80073758},
    {19,0x801FDE10,0x12C,0x80069698,0,0x80069B5C},
    {20,0x80201208,0x12C,0x8007406C,0,0x80074530}
};
}
const NativeProjectileClass* native_projectile_class(unsigned definition){for(const auto& type:projectile_classes)if(type.definition==definition)return &type;return nullptr;}
const NativeProjectileClass* native_projectile_class_by_vtable(uint32_t vtable){for(const auto& type:projectile_classes)if(type.vtable==vtable)return &type;return nullptr;}
std::vector<uint32_t> native_projectile_extra_offsets(unsigned definition){
    switch(definition){
    case 8:return {0x74,0x78,0x7C}; // native shadow's previous position
    case 10:return {0x164};
    case 14:return {0x160};
    case 15:return {0x140,0x144,0x168,0x170};
    default:return {};
    }
}
bool validate_native_projectile_creation(const EntityState& e,std::string& error){
    if(e.definition==0)return validate_native_rocket_creation(e,error);
    auto fail=[&](const char* text){error=text;return false;};
    const auto* type=native_projectile_class(e.definition);
    if(e.kind!=EntityKind::Projectile || !type || !e.id)return fail("Unsupported native projectile definition");
    for(const auto& [key,value]:e.properties){
        if(key=="position" || key=="spawn_position" || key=="velocity" || key=="projectile_rotation" || key=="projectile_scale"){
            const auto* v=std::get_if<std::array<float,3>>(&value);if(!v)return fail("Invalid native projectile vector type");
            for(auto f:*v)if(!std::isfinite(f) || (key=="projectile_scale" && f<0) || std::abs(f)>(key=="projectile_scale"?100.f:(key=="velocity" || key=="projectile_rotation")?10000.f:1000000.f))return fail("Invalid native projectile vector coefficient");
        }else if(key=="projectile_extra_visual"){
            const auto* v=std::get_if<std::vector<float>>(&value);const auto offsets=native_projectile_extra_offsets(e.definition);
            if(!v || offsets.empty() || v->size()!=offsets.size())return fail("Invalid original monster projectile visual layout");
            for(auto f:*v)if(!std::isfinite(f) || std::abs(f)>1000000.f)return fail("Invalid monster projectile visual coefficient");
        }else if(key=="projectile_model"){
            const auto* model=std::get_if<int64_t>(&value);
            // Original level model cache contains165 entries (8010AB70:
            // zero0x294 bytes); the two generic projectile constructors leave
            // +D4 uninitialized. Their original spawners fill it before +4C.
            if((e.definition!=10 && e.definition!=15) || !model || *model<0 || *model>=165)return fail("Invalid native generic projectile model");
        }else if(key=="projectile_source"){
            if(!std::holds_alternative<EntityReference>(value))return fail("Invalid native projectile source reference");
        }else if(key=="projectile_visual"){
            const auto* v=std::get_if<std::array<float,3>>(&value);if(e.definition!=12 || !v)return fail("Invalid native power projectile presentation");
            for(auto f:*v)if(!std::isfinite(f) || f<0 || f>10000.f)return fail("Invalid native power projectile coefficient");
            if((*v)[1]==20.f || (*v)[2]>1.f)return fail("Invalid native power projectile length/charge");
        }else if(key=="projectile_trail_active"){
            const auto* active=std::get_if<int64_t>(&value);if(e.definition!=4 || !active || *active<0 || *active>1)return fail("Invalid native adhesive projectile trail state");
        }else if(key=="projectile_spin"){
            const auto* elapsed=std::get_if<float>(&value);if(e.definition!=17 || !elapsed || !std::isfinite(*elapsed) || *elapsed<0 || *elapsed>1000000.f)return fail("Invalid native spinning projectile time");
        }else if(key=="projectile_energy_phase"){
            const auto* phase=std::get_if<int64_t>(&value);if(e.definition!=6 || !phase || *phase<0 || *phase>2)return fail("Invalid energy projectile phase");
        }else if(key=="projectile_energy_visual"){
            const auto* values=std::get_if<std::vector<float>>(&value);if(e.definition!=6 || !values || values->size()!=native_energy_visual_offsets.size())return fail("Invalid energy projectile presentation layout");
            for(unsigned i=0;i<values->size();++i)if(!std::isfinite((*values)[i]) || std::abs((*values)[i])>10000.f || (i>=6 && (*values)[i]<0)){error="Invalid energy projectile presentation coefficient index="+std::to_string(i)+" value="+std::to_string((*values)[i]);return false;}
        }else return fail("Projectile property has no verified native schema");
    }
    for(const auto* key:{"position","spawn_position","velocity","projectile_rotation","projectile_scale","projectile_source"})if(!e.properties.contains(key))return fail("Incomplete native grenade state");
    if((e.definition==10 || e.definition==15) && !e.properties.contains("projectile_model"))return fail("Generic projectile model missing before native initialization");
    if(e.definition==12 && !e.properties.contains("projectile_visual"))return fail("Power projectile presentation missing");
    if(e.definition==17 && !e.properties.contains("projectile_spin"))return fail("Spinning projectile presentation missing");
    if(e.definition==4 && !e.properties.contains("projectile_trail_active"))return fail("Adhesive projectile trail state missing");
    if(e.definition==6 && (!e.properties.contains("projectile_energy_phase") || !e.properties.contains("projectile_energy_visual")))return fail("Energy projectile presentation missing");
    if(!native_projectile_extra_offsets(e.definition).empty() && !e.properties.contains("projectile_extra_visual"))return fail("Monster projectile visual state missing");
    if(mem_read32(0x80000000)!=0x47334250u || !readable(mem_read32(0x802ABAAC),4) || mem_read32(type->vtable+0x4C)!=type->initialize)return fail("Native projectile class/manager signature differs");
    for(auto address:{0x8009DD48u,0x8009EB94u,0x801505F8u,type->initialize,0x8009B160u,0x800EEF8Cu})if(!ssne_find_func(address))return fail("Native projectile lifecycle unavailable");
    if(e.definition==12 && (mem_read32(type->vtable+0x15C)!=0x80065E74 || !ssne_find_func(0x80065E74)))return fail("Native power projectile render callback unavailable");
    if(e.definition==6 && (mem_read32(type->vtable+0x15C)!=0x8007277C || !ssne_find_func(0x8007277C) || !ssne_find_func(0x8007223C) || !ssne_find_func(0x80071CD0)))return fail("Native energy projectile render lifecycle unavailable");
    if(e.definition==17 && (mem_read32(type->vtable+0x15C)!=0x800732E4 || mem_read32(type->vtable+0x54)!=0x8007301C || !ssne_find_func(0x800732E4) || !ssne_find_func(0x8007301C)))return fail("Native spinning projectile render lifecycle unavailable");
    return true;
}
uint32_t create_native_projectile(PPCContext& context,const EntityState& e,uint32_t source,std::string& error){
    if(e.definition==0)return create_native_rocket(context,e,source,error);
    if(!context_ok(context) || context.r[1]<0x80004000 || !readable(context.r[1]-0x4000,0x4000) || !validate_native_projectile_creation(e,error))return 0;
    if(source && (!readable(source,0x54) || !readable(mem_read32(source+0x48),0x108))){error="Native grenade source unavailable";return 0;}
    const auto start=context.r[1]-0x4000;std::vector<uint8_t> saved(0x4000);for(unsigned i=0;i<saved.size();++i)saved[i]=mem_read8(start+i);
    struct Restore{uint32_t start;std::vector<uint8_t>& saved;~Restore(){for(unsigned i=0;i<saved.size();++i)mem_write8(start+i,saved[i]);}}restore{start,saved};
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=e.definition;ssne_call(call,0x8009DD48);const auto object=call.r[3];
    const auto* type=native_projectile_class(e.definition);
    if(!readable(object,type->size) || mem_read32(object+0x48)!=type->vtable){error="Native projectile factory returned another class";return 0;}
    for(const auto& [key,offset]:{std::pair{"position",0x20u},std::pair{"velocity",0x104u},std::pair{"projectile_rotation",0x2Cu},std::pair{"projectile_scale",0x68u}}){const auto& v=std::get<std::array<float,3>>(e.properties.at(key));for(unsigned i=0;i<3;++i)mem_write32(object+offset+i*4,f2u(v[i]));}
    if(e.definition==10 || e.definition==15)mem_write16(object+0xD4,uint16_t(std::get<int64_t>(e.properties.at("projectile_model"))));
    call.r[3]=object;call.r[4]=source;ssne_call(call,0x8009EB94);call.r[3]=mem_read32(0x802ABAAC);call.r[4]=object;ssne_call(call,0x801505F8);
    if(e.definition==12){const auto& visual=std::get<std::array<float,3>>(e.properties.at("projectile_visual"));for(unsigned i=0;i<3;++i)mem_write32(object+0x12C+i*4,f2u(visual[i]));}
    if(e.definition==4 && !std::get<int64_t>(e.properties.at("projectile_trail_active"))){
        const auto trail=mem_read32(object+0x13C);if(trail){mem_write32(trail+8,1);mem_write32(trail+0xC,mem_read32(0x80200088));mem_write32(object+0x13C,0);}
    }
    if(e.definition==17){
        // Original registration aims the model from velocity. Retain the
        // authoritative orientation and spin after allocating all nine proxies.
        for(const auto& [key,offset]:{std::pair{"position",0x20u},std::pair{"projectile_rotation",0x2Cu},std::pair{"projectile_scale",0x68u}}){const auto& v=std::get<std::array<float,3>>(e.properties.at(key));for(unsigned i=0;i<3;++i)mem_write32(object+offset+i*4,f2u(v[i]));}
        mem_write32(object+0x16C,f2u(std::get<float>(e.properties.at("projectile_spin"))));
    }
    if(e.definition==6){
        // Original initialization resets the charge scale. Restore the host's
        // visual pose after resources have been allocated by the native class.
        for(const auto& [key,offset]:{std::pair{"position",0x20u},std::pair{"projectile_rotation",0x2Cu},std::pair{"projectile_scale",0x68u}}){const auto& v=std::get<std::array<float,3>>(e.properties.at(key));for(unsigned i=0;i<3;++i)mem_write32(object+offset+i*4,f2u(v[i]));}
        const auto& visual=std::get<std::vector<float>>(e.properties.at("projectile_energy_visual"));for(unsigned i=0;i<visual.size();++i)mem_write32(object+native_energy_visual_offsets[i],f2u(visual[i]));mem_write32(object+0x148,uint32_t(std::get<int64_t>(e.properties.at("projectile_energy_phase"))));
    }
    if(e.properties.contains("projectile_extra_visual")){
        const auto offsets=native_projectile_extra_offsets(e.definition);const auto& v=std::get<std::vector<float>>(e.properties.at("projectile_extra_visual"));
        for(unsigned i=0;i<offsets.size();++i)mem_write32(object+offsets[i],f2u(v[i]));
    }
    return object;
}
bool present_native_projectile(PPCContext& context,uint32_t object,std::string& error){
    if(readable(object,0x4C) && mem_read32(object+0x48)==0x802022E0)return present_native_rocket(context,object,error);
    if(readable(object,0x4C) && mem_read32(object+0x48)==0x802009D8){
        if(!context_ok(context) || !readable(object,0x174)){error="Invalid native spinning projectile presentation";return false;}
        StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);const auto base=call.r[1];
        auto translate=[&](uint32_t proxy,uint32_t vector)->bool{
            const auto vt=readable(proxy,4)?mem_read32(proxy):0;if(!readable(vt,0x30) || !ssne_find_func(mem_read32(vt+0x2C))){error="Native spinning projectile proxy unavailable";return false;}
            call.r[3]=proxy+uint32_t(int32_t(int16_t(mem_read16(vt+0x28))));ssne_call(call,mem_read32(vt+0x2C));
            if(!readable(call.r[3],48)){error="Native spinning projectile matrix unavailable";return false;}
            for(unsigned i=0;i<3;++i)call.ps0[1+i]=u2f(mem_read32(vector+i*4));ssne_call(call,0x801BA300);return true;
        };
        // Exact visual-only suffix of73108: normalize(-velocity), then place
        // the eight original proxies behind the body. No base movement,
        // collision callback73288, impact73648 or damage dispatch is invoked.
        call.r[3]=object+0x104;call.r[4]=base+0x40;call.ps0[1]=u2f(mem_read32(0x80200990));ssne_call(call,0x801BA9C4);
        const float vx=u2f(mem_read32(base+0x40)),vy=u2f(mem_read32(base+0x44)),vz=u2f(mem_read32(base+0x48));
        if(vx*vx+vy*vy+vz*vz>0){call.r[3]=base+0x40;call.r[4]=base+0x40;ssne_call(call,0x801BA9E0);}
        else for(unsigned i=0;i<3;++i)mem_write32(base+0x40+i*4,0); // Degenerate retired motion has no tail direction.
        for(int i=7;i>=0;--i){
            call.r[3]=base+0x40;call.r[4]=base+0x50;call.ps0[1]=float(i);ssne_call(call,0x801BA9C4);
            call.r[3]=base+0x50;call.r[4]=base+0x60;call.ps0[1]=u2f(mem_read32(0x802009A0));ssne_call(call,0x801BA9C4);
            call.r[3]=object+0x20;call.r[4]=base+0x60;call.r[5]=base+0x70;ssne_call(call,0x801BA97C);
            if(!translate(mem_read32(object+0x14C+unsigned(i)*4),base+0x70))return false;
        }
        if(!translate(mem_read32(object+0x128),object+0x20))return false;
        call.r[3]=object;ssne_call(call,0x800732E4);return true;
    }
    if(readable(object,0x4C) && mem_read32(object+0x48)==0x80200598){
        if(!context_ok(context) || !readable(object,0x17C) || mem_read32(object+0x148)>2){error="Invalid native energy projectile presentation";return false;}
        const auto phase=mem_read32(object+0x148);
        if(phase==2 && (!mem_read32(object+0x164) || !mem_read32(object+0x168))){
            if(mem_read32(object+0x164) || mem_read32(object+0x168)){error="Incomplete native energy explosion resources";return false;}
            std::array<uint32_t,native_energy_visual_offsets.size()> visual{};for(unsigned i=0;i<visual.size();++i)visual[i]=mem_read32(object+native_energy_visual_offsets[i]);
            mem_write32(object+0x148,1);
            // Reuse original resource transition with its one verified damage
            // dispatch suppressed by the runtime's scoped presentation guard.
            if(!ssne::play_native_energy_transition(context,object,error)){mem_write32(object+0x148,phase);return false;}
            for(unsigned i=0;i<visual.size();++i)mem_write32(object+native_energy_visual_offsets[i],visual[i]);
        }
        StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
        if(phase==2){
            // 80071CD0 updates explosion matrices and fade at the host's elapsed
            // time. Its final local-expiry branch must remain host-authoritative.
            const auto elapsed=mem_read32(object+0x14C);const auto limit=mem_read32(0x80200504);
            if(u2f(elapsed)>u2f(limit))mem_write32(object+0x14C,limit);
            call.r[3]=object;ssne_call(call,0x80071CD0);mem_write32(object+0x14C,elapsed);return true;
        }
        const auto material=mem_read32(object+0x130),light=mem_read32(object+0x144);
        auto method=[&](uint32_t instance,uint32_t slot)->bool{
            const auto vt=readable(instance,4)?mem_read32(instance):0;
            if(!readable(vt,slot+4) || !ssne_find_func(mem_read32(vt+slot))){error="Native energy visual setter unavailable";return false;}
            call.r[3]=instance+uint32_t(int32_t(int16_t(mem_read16(vt+slot-4))));ssne_call(call,mem_read32(vt+slot));return true;
        };
        if(phase==0){
            const auto scale=f2u(u2f(mem_read32(object+0x68))*u2f(mem_read32(0x802004C4)));
            mem_write32(call.r[1]+0x40,scale);mem_write32(call.r[1]+0x44,scale);call.r[4]=call.r[1]+0x40;if(!method(material,0x44))return false;
            call.ps0[1]=u2f(mem_read32(object+0x34));if(!method(material,0x4C))return false;
        }
        if(light){call.r[4]=object+0x20;if(!method(light,0x14))return false;call.ps0[1]=u2f(mem_read32(object+0x154))+u2f(mem_read32(0x802004E0));if(!method(light,0x2C))return false;}
        // Pure native render callback; charge randomness, motion, collisions,
        // ammunition and damage updates are intentionally not executed here.
        call.r[3]=object;ssne_call(call,0x8007277C);return true;
    }
    if(readable(object,0x4C) && mem_read32(object+0x48)==0x801FD0B0){
        if(!context_ok(context) || !readable(object,0x144) || !readable(mem_read32(object+0x128),4)){error="Invalid native power projectile presentation";return false;}
        // Original render virtual+15C: body orientation and energy-tail matrices,
        // with no collision, movement, weapon consumption or damage dispatch.
        StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=object;ssne_call(call,0x80065E74);return true;
    }
    if(readable(object,0x4C) && (mem_read32(object+0x48)==0x801FF400 || mem_read32(object+0x48)==0x80201650)){
        if(!context_ok(context) || !readable(object,mem_read32(object+0x48)==0x80201650?0x164:0x168)){error="Invalid native special rocket presentation";return false;}
        StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
        for(unsigned i=0;i<6;++i)mem_write32(call.r[1]+0x40+i*4,mem_read32(object+0x20+i*4));mem_write32(call.r[1]+0x58,0);call.r[3]=object;call.r[4]=call.r[1]+0x40;ssne_call(call,0x8009B160);
        if(const auto trail=mem_read32(object+(mem_read32(object+0x48)==0x80201650?0x124:0x12C))){call.r[3]=trail;call.r[4]=object+0x20;ssne_call(call,0x800EEF8C);}
        if(mem_read32(object+0x48)==0x80201650)if(const auto particles=mem_read32(object+0x128)){call.r[3]=particles;call.r[4]=object+0x20;ssne_call(call,0x800EF784);}
        if(const auto model=mem_read32(object+0x160)){const auto vt=readable(model,4)?mem_read32(model):0;if(!readable(vt,0x30) || !ssne_find_func(mem_read32(vt+0x2C))){error="Native special rocket model unavailable";return false;}call.r[3]=model+uint32_t(int32_t(int16_t(mem_read16(vt+0x28))));ssne_call(call,mem_read32(vt+0x2C));if(!readable(call.r[3],48)){error="Invalid special rocket model matrix";return false;}for(unsigned i=0;i<3;++i)mem_write32(call.r[3]+12+i*16,mem_read32(object+0x20+i*4));}
        return true;
    }
    const auto* type=readable(object,0x4C)?native_projectile_class_by_vtable(mem_read32(object+0x48)):nullptr;
    if(!context_ok(context) || !type || !readable(object,type->size)){error="Invalid native projectile presentation";return false;}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    // Original geometric setter copies the authoritative pose. Its optional
    // gameplay virtual branch is disabled by zero pose flags. No grenade
    // physics/update8006D6F8 or explosion/damage8006DAB4 is executed here.
    for(unsigned i=0;i<6;++i)mem_write32(call.r[1]+0x40+i*4,mem_read32(object+0x20+i*4));mem_write32(call.r[1]+0x58,0);
    call.r[3]=object;call.r[4]=call.r[1]+0x40;ssne_call(call,0x8009B160);
    if(type->trail)if(const auto trail=mem_read32(object+type->trail)){call.r[3]=trail;call.r[4]=object+0x20;ssne_call(call,0x800EEF8C);}
    if(type->definition==10)if(const auto particles=mem_read32(object+0x15C)){call.r[3]=particles;call.r[4]=object+0x20;ssne_call(call,0x800EF830);}
    if(type->definition==8 || type->definition==10 || type->definition==13 || type->definition==14 || type->definition==15 || type->definition==16 || type->definition>=18){
        // Original virtual +15C contains only model matrix construction for
        // these classes. Their AI, movement and collision ticks stay disabled.
        call.r[3]=object;ssne_call(call,mem_read32(type->vtable+0x15C));
    }
    return true;
}
bool validate_native_marker(uint32_t object,uint32_t definition,int64_t index,int64_t active,std::string& error){
    const auto resolved=native_script_reference_address(index);
    if((definition!=1 && definition!=2) || (active!=0 && active!=1) || index<=0 || !resolved || *resolved!=object || !readable(object,0x94) || mem_read32(object+0x48)!=(definition==1?0x802054A0u:0x802027C8u)){error="Invalid native level marker binding/state";return false;}
    return true;
}
bool apply_native_marker(PPCContext& context,uint32_t object,bool active,std::string& error){
    if(!context_ok(context) || !readable(object,0x94) || !native_marker_class(mem_read32(object+0x48))){error="Native marker lifecycle context unavailable";return false;}
    if(bool(mem_read32(object+0x8C))==active)return true;
    // Use the original visual enable/disable lifecycle. Never dispatch the
    // script event itself: it can fire additional gameplay actions on clients.
    const bool lockdown=mem_read32(object+0x48)==0x802054A0u;
    const uint32_t function=lockdown?(active?0x8007CFB8u:0x8007D06Cu):(active?0x800775DCu:0x80077698u);
    if(!ssne_find_func(function) || mem_read32(function)!=0x9421FFF0u){error="Native marker lifecycle function is not verified";return false;}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=object;ssne_call(call,function);
    if(bool(mem_read32(object+0x8C))!=active){error="Native marker lifecycle did not reach authoritative state";return false;}return true;
}
bool validate_native_arrow_animation(uint32_t object,const std::vector<float>& animation,std::string& error){
    if(!readable(object,0xB8) || mem_read32(object+0x48)!=0x802027C8u || animation.size()!=18 ||
       !ssne_find_func(0x800773B0u) || mem_read32(0x800773B0u)!=0x9421FEC0u){error="Invalid original arrow animation binding";return false;}
    for(float value:animation)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid arrow animation coefficient";return false;}
    return true;
}
bool apply_native_arrow_animation(PPCContext& context,uint32_t object,const std::vector<float>& animation,std::string& error){
    if(!context_ok(context) || !validate_native_arrow_animation(object,animation,error))return false;
    const auto proxy=mem_read32(object+0x8C),matrix=proxy && readable(proxy,8)?mem_read32(proxy+4):0;
    if(proxy && !readable(matrix,48)){error="Original arrow render matrix is unavailable";return false;}
    unsigned i=0;for(unsigned off:{0x90u,0x94u,0xA0u,0xA4u,0xA8u,0xACu})mem_write32(object+off,f2u(animation[i++]));
    // Transfer the matrix produced by the original host posing routine too.
    // Its optional local parent pointer can differ on a replica; recomputing
    // from that pointer would detach the arrow from its moving target.
    if(matrix)for(unsigned j=0;j<12;++j)mem_write32(matrix+j*4,f2u(animation[6+j]));
    return true;
}
bool validate_native_removal(const PPCContext& context,uint32_t object,std::string& error) {
    auto fail=[&](const char* why){error=why;return false;};
    if(!context_ok(context) || !readable(object,0x54))return fail("Native removal context/object unavailable");
    const auto vt=mem_read32(object+0x48);if(!readable(vt,0x58))return fail("Native removal vtable unavailable");
    const auto* type=native_projectile_class_by_vtable(vt);
    const bool corpse=vt==0x8020C6B8u;
    const bool rotor=vt==0x8020B228u;
    const bool piece=vt==0x8020B990u;
    if(rotor && !native_render_binding(object,EntityKind::Pickup))return fail("Native propeller renderer differs"); // Same +CC model-proxy layout.
    const auto owned_size=piece?0x130u:rotor?0x118u:corpse?0x188u:type?type->size:0u,owned_destructor=piece?0x80087ED0u:rotor?0x80087D9Cu:corpse?0x8008CEDCu:type?type->destructor:0u;
    if(vt==0x801F6908u || vt==0x801EF6C0u){
        const unsigned start=vt==0x801F6908u?0x250:0x244,count=vt==0x801F6908u?2:3;
        if(!readable(object,start+count*4))return fail("NPC part owner is truncated");
        for(unsigned i=0;i<count;++i){const auto child=mem_read32(object+start+i*4);if(child && readable(child,0x130) && mem_read32(child+0x48)==0x8020B990u && mem_read32(child+0xC8)==object && !validate_native_removal(context,child,error))return false;}
    }
    if(vt==0x801EB9F8u){
        // Tweedle Dum Dum owns an independently registered propeller actor.
        // Validate its complete retirement before committing the parent batch.
        if(!readable(object,0x250))return fail("Native rotor owner is truncated");
        const auto child=mem_read32(object+0x24C);
        if(child && (!readable(child,0x118) || mem_read32(child+0x48)!=0x8020B228u || !validate_native_removal(context,child,error)))return fail("Native propeller ownership/lifecycle differs");
    }
    if(type || corpse || rotor || piece){
        // Projectile/corpse factories and the propeller factory8001D418 use
        // native heap allocations. Requested size, owning heap and allocated-
        // list links occupy the preceding32 bytes; level-owned actors do not qualify.
        const auto heap_index=mem_read32(context.r[13]-0x6DE0),heap_count=mem_read32(context.r[13]-0x5CB8),heaps=mem_read32(context.r[13]-0x5CBC);
        if((object&31) || object<mem_read32(context.r[13]-0x6DE8) || !readable(object-0x20,0x20+owned_size) || heap_index>=heap_count || heap_count>64 || !readable(heaps,heap_count*24))return fail("Native projectile has no verified factory allocation");
        const auto block=object-0x20,heap=heaps+heap_index*24,bytes=mem_read32(block+8),previous=mem_read32(block),next=mem_read32(block+4);
        if(mem_read32(block+0xC)!=heap || mem_read32(block+0x10)!=owned_size || bytes<owned_size+0x20 || (bytes&31) || !readable(block,bytes) ||
           (previous?(!readable(previous,8) || mem_read32(previous+4)!=block):mem_read32(heap+8)!=block) || (next && (!readable(next,4) || mem_read32(next)!=block)))return fail("Native projectile heap ownership differs");
        // Full destruction dispatches observer callbacks before the destructor.
        // These replica presentation factories do not install gameplay observers.
        if(mem_read32(object+0x38) || !ssne_find_func(0x8009B288) || mem_read32(0x8009B288)!=0x9421FFF0u || mem_read16(vt+0x18) || mem_read32(vt+0x1C)!=owned_destructor || !ssne_find_func(owned_destructor) || mem_read32(owned_destructor)!=(rotor?0x9421FFF0u:0x9421FFF8u))return fail("Native owned presentation destruction has unverified observers/lifecycle");
    }
    if((vt==0x80213CC8u || vt==0x80214448u) && readable(object,0xE4) && mem_read32(object+0xE0)==1){
        // Replica-created pickups own their configuration. Unregister alone
        // leaves it allocated forever because the native update list no
        // longer reaches the destructor. The full original destruction path
        // is safe here only without gameplay observer callbacks at +38.
        if(mem_read32(object+0x38) || !ssne_find_func(0x8009B288) || mem_read32(0x8009B288)!=0x9421FFF0u || mem_read32(vt+0x1C)!=(vt==0x80214448u?0x800C8CC4u:0x800C6B60u))return fail("Owned native pickup destruction has unverified observers/lifecycle");
    }
    const auto getter=mem_read32(vt+0x34);if(!readable(getter,8) || (mem_read32(getter)&0xFFFF0000u)!=0x38600000u || mem_read32(getter+4)!=0x4E800020u)return fail("Native update group getter is not verified");
    const auto group=int16_t(mem_read32(getter));if(group==-1)return true;
    if(group<0 || group>63)return fail("Native update group is out of range");
    const auto lists=mem_read32(context.r[13]-0x5648);if(!readable(lists,(group+1)*4))return fail("Native update lists unavailable");
    std::set<uint32_t> visited;auto current=mem_read32(lists+group*4);
    while(current) {
        if(current==object)return true;
        if(!readable(current,0x40) || !visited.insert(current).second || visited.size()>32768)return fail("Native removal list is invalid");
        current=mem_read32(current+0x3C);
    }
    return fail("Native object is absent from its removal list");
}
bool remove_native_entity(PPCContext& context,uint32_t object,std::string& error){return remove_native_entity(context,object,error,EntityKind::Mission);}
bool remove_native_entity(PPCContext& context,uint32_t object,std::string& error,EntityKind presentation) {
    if(!validate_native_removal(context,object,error))return false;
    std::vector<uint32_t> proxies;
    if(presentation==EntityKind::Enemy || presentation==EntityKind::Pickup){
        const auto binding=native_render_binding(object,presentation);if(!binding){error="Native removal render binding unavailable";return false;}
        if(binding->model)proxies.push_back(binding->model);
        if(presentation==EntityKind::Pickup){const auto glow=native_pickup_glow_proxy(object);if(!glow){error="Native removal glow binding unavailable";return false;}if(*glow)proxies.push_back(*glow);}
    }
    // The original propeller destructor stops its own looping voice at +114,
    // unregisters its renderer, and frees only its factory allocation.
    if(!retire_native_enemy_auxiliary(context,object,error))return false;
    // The native update list and render-proxy list have separate lifetimes.
    // Level-owned actors cannot be heap-freed here. Unregistering their update
    // alone leaves their model standing in the renderer indefinitely.
    for(auto proxy:proxies)mem_write8(proxy+0x2D,0);
    const auto vt=mem_read32(object+0x48);
    if((vt==0x80213CC8u || vt==0x80214448u) && mem_read32(object+0xE0)==1){
        StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=object;
        ssne_call(call,0x8009B288);return true;
    }
    // Minigun alternate bullet retires its trail in8006F9EC before the base
    // destructor. Snapshot removal bypasses that gameplay update, so perform
    // the exact two visual lifetime stores before invoking normal removal.
    if(mem_read32(object+0x48)==0x801FF860u){const auto trail=mem_read32(object+0x124);if(trail){if(!readable(trail,0x10)){error="Invalid alternate bullet trail lifetime";return false;}mem_write32(trail+8,1);mem_write32(trail+0xC,mem_read32(0x801FF83C));}}
    StackScratch scratch(context);PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;
    if(native_projectile_class_by_vtable(vt) || vt==0x8020C6B8u || vt==0x8020B228u || vt==0x8020B990u){
        // Unregistering alone removes the actor from the only list that later
        // calls its destructor. Retire visual resources and free the actual
        // factory allocation through the original complete lifecycle.
        mem_write32(call.r[1],context.r[1]);call.r[3]=object;ssne_call(call,0x8009B288);return true;
    }
    mem_write32(call.r[1],context.r[1]);call.r[3]=mem_read32(0x802ABA9C);call.r[4]=object;
    ssne_call(call,ssne::game_address(0x8015070C));return true;
}
}
