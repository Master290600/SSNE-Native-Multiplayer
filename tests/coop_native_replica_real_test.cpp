#include "native_replica.h"
#include "ssne/native_entity.h"
#include "ssne/runtime.h"
#include "ssne/replica_simulation.h"
#include "ssne/recomp_hooks.h"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <limits>
using namespace port::coop;
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
uint64_t digest(unsigned size){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<size;++i){h^=ssne::g_ram[i];h*=1099511628211ull;}return h;}
int main(int argc,char** argv) {
    require(argc==3,"Pass game_data and captured native player directory");
    PPCContext context;std::string error;require(ssne::boot(context,argv[1],error)!=0,error.c_str());
    std::ifstream input(std::string(argv[2])+"/native-capture.ram",std::ios::binary);std::vector<uint8_t> ram((std::istreambuf_iterator<char>(input)),{});
    require(ram.size()==0x01800000,"Expected real two-player PAL fixture");memcpy(ssne::g_ram,ram.data(),ram.size());
    NativeWorld native;for(unsigned slot=0;slot<8;++slot)native.set_player_owner(slot,{slot,1});
    std::ifstream objects(std::string(argv[2])+"/native-capture-objects.csv");std::string row;
    while(std::getline(objects,row)) {
        unsigned long long id;unsigned address,kind,definition;require(std::sscanf(row.c_str(),"%llu,%u,%u,%u",&id,&address,&kind,&definition)==4,"Capture metadata");
        if(kind==unsigned(EntityKind::Enemy))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedActor,address,definition});
        if(kind==unsigned(EntityKind::Pickup))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,address,definition});
        ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,address});
    }
    auto local=native.capture(1);std::map<EntityId,EntityState> host;
    EntityId player=0;uint32_t native_player=0;
    for(const auto& [id,e]:local.entities){auto copy=e;copy.id=10000+id;host[copy.id]=copy;if(e.kind==EntityKind::Player && !player){player=copy.id;native_player=native.local_address(id);}}
    require(player!=0,"No real player to reconcile");NativeReplica replica(native);
    auto before=digest(unsigned(ram.size()));
    ssne::set_replica_simulation(true);
    context.r[3]=0x802924B8;context.ps0[1]=1./60.;ssne_call(context,0x80166724);
    context.r[3]=mem_read32(0x802ABAAC);ssne_call(context,0x801508B4);
    context.r[3]=mem_read32(native_player+0x368);ssne_call(context,0x800A5EDC);
    require(digest(unsigned(ram.size()))==before,"Client camera update overwrote authoritative presentation state");
    auto suppressed=ssne::replica_simulation_stats();
    require(suppressed.game_updates==1 && suppressed.world_updates==1,"Replica still executes native gameplay");
    require(digest(unsigned(ram.size()))==before,"Replica executed AI, scripts or world physics");
    ssne::set_replica_simulation(false);
    auto result=replica.apply(1,host);require(result.applied,result.error.c_str());
    require(result.objects==102 && replica.local_address(player)==native_player,"Pointer-independent replica identity");
    auto moved=host;auto& entity=moved.at(player);entity.properties["position"]=std::array<float,3>{4,1,82};entity.properties["health"]=73.f;entity.properties["yaw"]=.5f;entity.properties["pitch"]=-.1f;
    auto ammo=std::get<std::vector<int64_t>>(entity.properties.at("ammo"));ammo[0]=7;entity.properties["ammo"]=ammo;
    auto invalid=moved;invalid.rbegin()->second.properties["unknown_critical_field"]=int64_t(1);
    before=digest(unsigned(ram.size()));result=replica.apply(1,invalid);require(!result.applied,"Unknown native field silently accepted");require(digest(unsigned(ram.size()))==before,"Rejected batch partially changed native RAM");
    invalid=moved;invalid.at(player).properties["health"]=std::numeric_limits<float>::quiet_NaN();result=replica.apply(1,invalid);require(!result.applied && digest(unsigned(ram.size()))==before,"NaN partially committed");
    result=replica.apply(1,moved);require(result.applied,result.error.c_str());
    require(u2f(mem_read32(native_player+0x20))==4 && u2f(mem_read32(native_player+0x74))==4,"Position/previous-position reconciliation");
    require(u2f(mem_read32(native_player+0xCC))==73 && u2f(mem_read32(native_player+0x290))==.5f,"Health/look reconciliation");
    require(mem_read16(mem_read32(native_player+0x5E8))==7,"Ammunition reconciliation");
    auto after=digest(unsigned(ram.size()));result=replica.apply(1,moved);require(result.applied && digest(unsigned(ram.size()))==after,"Repeated authoritative state changed native data");
    auto invalid_control=moved;
    auto old_camera=std::get<int64_t>(invalid_control.at(player).properties.at("camera_frozen"));
    invalid_control.at(player).properties["camera_frozen"]=int64_t(!old_camera);
    result=replica.apply(1,invalid_control);require(!result.applied && digest(unsigned(ram.size()))==after,"Camera lifecycle command committed without guest context");
    invalid_control=moved;invalid_control.at(player).properties["movement_mode"]=int64_t(9);
    result=replica.apply(1,invalid_control);require(!result.applied && digest(unsigned(ram.size()))==after,"Invalid movement mode modified guest state");
    require(moved.at(player).properties.contains("camera_row0"),"Real player camera was not captured");
    auto invalid_camera=moved;invalid_camera.at(player).properties.erase("camera_row1");
    result=replica.apply(1,invalid_camera);require(!result.applied && digest(unsigned(ram.size()))==after,"Incomplete camera matrix partially committed");
    invalid_camera=moved;auto bad_row=std::get<std::array<float,4>>(invalid_camera.at(player).properties.at("camera_row0"));bad_row[0]=std::numeric_limits<float>::quiet_NaN();invalid_camera.at(player).properties["camera_row0"]=bad_row;
    result=replica.apply(1,invalid_camera);require(!result.applied && digest(unsigned(ram.size()))==after,"NaN camera matrix partially committed");
    auto matrix=std::get<std::array<float,4>>(moved.at(player).properties.at("camera_row0"));matrix[3]=4.f;moved.at(player).properties["camera_row0"]=matrix;
    result=replica.apply(1,moved);require(result.applied,result.error.c_str());
    const auto camera=mem_read32(native_player+0x368);
    require(u2f(mem_read32(camera+0x30))==4.f && u2f(mem_read32(camera+0x60))==4.f,"Authoritative camera cache was not applied to both native matrices");
    require(u2f(mem_read32(native_player+0xCC))==73.f && mem_read16(mem_read32(native_player+0x5E8))==7,"Camera reconciliation changed health or ammunition");
    after=digest(unsigned(ram.size()));
    auto invalid_rule=moved;invalid_rule.at(player).properties["weapons_hidden"]=int64_t(2);
    result=replica.apply(context,1,invalid_rule);
    require(!result.applied && digest(unsigned(ram.size()))==after,"Invalid visibility rule partially committed");
    auto no_model=moved;auto model_visibility=std::get<std::vector<int64_t>>(no_model.at(player).properties.at("weapon_model_visibility"));model_visibility[0]=-1;no_model.at(player).properties["weapon_model_visibility"]=model_visibility;
    for(unsigned row=0;row<3;++row)no_model.at(player).properties.erase("weapon_model0_row"+std::to_string(row));
    result=replica.apply(1,no_model);require(result.applied,result.error.c_str());
    auto pistol_instance=mem_read32(native_player+0x4BC+24),pistol_model=mem_read32(pistol_instance);
    require(!mem_read32(pistol_model+0x24),"Absent authoritative model remained visible locally");
    result=replica.apply(1,moved);require(result.applied,result.error.c_str());after=digest(unsigned(ram.size()));
    auto incomplete_pose=moved;incomplete_pose.at(player).properties.erase("weapon_model0_row1");
    result=replica.apply(1,incomplete_pose);require(!result.applied && digest(unsigned(ram.size()))==after,"Incomplete weapon pose partially committed");
    auto selection_only=moved;
    for(auto it=selection_only.at(player).properties.begin();it!=selection_only.at(player).properties.end();)if(it->first.starts_with("weapon_model"))it=selection_only.at(player).properties.erase(it);else ++it;
    auto invalid_weapon=selection_only;invalid_weapon.at(player).properties["selected_weapon"]=int64_t(12);
    result=replica.apply(1,invalid_weapon);require(!result.applied && digest(unsigned(ram.size()))==after,"Invalid weapon index partially committed");
    auto weapon_context=context;weapon_context.r[13]=0x802E22A0;weapon_context.r[2]=0x802F22A0;weapon_context.r[1]=0x817FF000;
    invalid_weapon=selection_only;invalid_weapon.at(player).properties["selected_weapon"]=int64_t(0);
    result=replica.apply(weapon_context,1,invalid_weapon);require(!result.applied && digest(unsigned(ram.size()))==after,"Unowned weapon change modified native state");
    auto equipped=selection_only;auto ownership=std::get<std::vector<int64_t>>(equipped.at(player).properties.at("weapon_owned_counts"));ownership[0]=1;
    equipped.at(player).properties["weapon_owned_counts"]=ownership;equipped.at(player).properties["selected_weapon"]=int64_t(0);
    auto available=std::get<std::vector<int64_t>>(equipped.at(player).properties.at("weapon_available"));available[0]=1;equipped.at(player).properties["weapon_available"]=available;
    result=replica.apply(1,equipped);require(!result.applied && digest(unsigned(ram.size()))==after,"Weapon change executed without native lifecycle context");
    result=replica.apply(weapon_context,1,equipped);require(result.applied,result.error.c_str());
    require(mem_read32(native_player+0x5DC)==0,"Native weapon equip did not select the authoritative slot");
    require(u2f(mem_read32(native_player+0xCC))==73.f && mem_read16(mem_read32(native_player+0x5E8))==7,"Native weapon equip changed health or ammunition");
    auto equip_digest=digest(unsigned(ram.size()));result=replica.apply(weapon_context,1,equipped);require(result.applied && digest(unsigned(ram.size()))==equip_digest,"Repeated native equip has side effects");
    result=replica.apply(weapon_context,1,moved);require(result.applied,result.error.c_str());require(mem_read32(native_player+0x5DC)==1,"Native weapon equip did not restore the pistol");
    after=digest(unsigned(ram.size()));
    auto debug_available=selection_only;auto debug_flags=std::get<std::vector<int64_t>>(debug_available.at(player).properties.at("weapon_available"));debug_flags[0]=1;
    debug_available.at(player).properties["weapon_available"]=debug_flags;debug_available.at(player).properties["selected_weapon"]=int64_t(0);
    result=replica.apply(weapon_context,1,debug_available);require(result.applied,result.error.c_str());
    require(mem_read32(native_player+0x5DC)==0 && mem_read32(native_player+0x4CC)==0,"Debug availability did not select the weapon without granting inventory ownership");
    require(u2f(mem_read32(native_player+0xCC))==73.f && mem_read16(mem_read32(native_player+0x5E8))==7,"Debug selection granted ammo or changed health");
    result=replica.apply(weapon_context,1,moved);require(result.applied,result.error.c_str());
    after=digest(unsigned(ram.size()));
    auto missing_model=moved;auto model_counts=std::get<std::vector<int64_t>>(missing_model.at(player).properties.at("weapon_active_counts"));model_counts[0]=2;missing_model.at(player).properties["weapon_active_counts"]=model_counts;
    result=replica.apply(1,missing_model);require(!result.applied && digest(unsigned(ram.size()))==after,"Absent second weapon model partially committed");
    auto absent=moved;absent.erase(absent.begin());result=replica.apply(1,absent);require(!result.applied && digest(unsigned(ram.size()))==after,"Missing lifecycle removal was hidden");
    auto extra=moved;auto duplicate=extra.at(player);duplicate.id=999999;extra[duplicate.id]=duplicate;result=replica.apply(1,extra);require(!result.applied && digest(unsigned(ram.size()))==after,"Two server players aliased one native object");
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Removing,native_player});result=replica.apply(1,moved);require(!result.applied,"Removed replica binding reused stale memory");
    std::cout<<"Real native replica fixture: 102 mapped objects; position/history, health, look and ammo applied; invalid batches atomic; duplicate IDs, missing lifecycle and stale bindings rejected.\n";
    std::cout.flush();std::_Exit(0);
}
