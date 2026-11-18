// Private owned-PAL regression: a real eight-slot reservation and party component.
#include "native_replica.h"
#include "client_presentation.h"
#include "ssne/native_entity.h"
#include "ssne/native_party.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <cstring>
#include <cstdlib>
using namespace port::coop;
namespace {
void require(bool ok,const std::string& message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
std::vector<uint8_t> bytes(const std::string& path){std::ifstream in(path,std::ios::binary);require(bool(in),"Missing private fixture: "+path);return {(std::istreambuf_iterator<char>(in)),{}};}
uint64_t digest(){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<ssne::guest_ram_size();++i){h^=ssne::g_ram[i];h*=1099511628211ull;}return h;}
unsigned players(const NativeWorldSample& sample){unsigned count=0;for(const auto& [id,e]:sample.entities)count+=e.kind==EntityKind::Player;return count;}
std::map<EntityId,EntityState> authority(const NativeWorldSample& sample){
    std::map<EntityId,EntityState> result;
    for(const auto& [id,e]:sample.entities){auto copy=e;copy.id=id+10000;for(auto& [name,value]:copy.properties)if(auto* ref=std::get_if<EntityReference>(&value);ref&&ref->id)ref->id+=10000;result.emplace(copy.id,std::move(copy));}
    return result;
}
EntityId player_id(const std::map<EntityId,EntityState>& source,unsigned slot){for(const auto& [id,e]:source)if(e.kind==EntityKind::Player&&std::get<int64_t>(e.properties.at("player_slot"))==slot)return id;return 0;}
void other_entities_equal(const NativeWorldSample& full,const NativeWorldSample& published){
    for(const auto& [id,original]:full.entities)if(original.kind!=EntityKind::Player){
        auto expected=original;
        if(expected.kind==EntityKind::Projectile)if(auto value=expected.properties.find("projectile_source");value!=expected.properties.end())if(auto* ref=std::get_if<EntityReference>(&value->second);ref&&ref->id&&!published.entities.contains(ref->id))ref->id=0;
        require(published.entities.contains(id)&&published.entities.at(id)==expected,"Publication changed a non-player entity beyond absent-source normalization");
    }
}
std::vector<uint8_t> region(uint32_t address,unsigned size){require(address>=0x80000000u&&address<=ssne::guest_ram_end()-size,"Invalid protected native region");const auto* p=ssne::host_ptr(address);return {p,p+size};}
}
int main(int argc,char** argv){
    require(argc==3,"Pass owned game_data and private eight-player native fixture directory");
    ssne::native_party_extended_memory(true);PPCContext context;std::string error;
    require(ssne::boot(context,argv[1],error)!=0,error);
    const std::string fixture=argv[2];auto ram=bytes(fixture+"/native-capture.ram");require(ram.size()==ssne::guest_ram_size(),"Fixture must contain the active extended PAL RAM");std::memcpy(ssne::g_ram,ram.data(),ram.size());ram.clear();ram.shrink_to_fit();
    auto cpu=bytes(fixture+"/native-capture-context.bin");require(cpu.size()==sizeof(context),"Fixture CPU layout differs");std::memcpy(&context,cpu.data(),sizeof(context));context.msr&=~0x8000u;context.r[1]=ssne::guest_ram_end()-0x40000;mem_write32(context.r[1],0);
    auto cache=bytes(fixture+"/native-capture-locked-cache.bin");require(cache.size()==ssne::kLockedCacheSize,"Fixture locked-cache layout differs");std::memcpy(ssne::g_locked_cache,cache.data(),cache.size());
    require(ssne::native_party_load_state(bytes(fixture+"/native-capture-party.bin"),error),error);
    require(ssne::native_party_capacity()==8,"Fixture does not reserve eight native players");
    NativeWorld local;for(unsigned slot=0;slot<8;++slot)local.set_player_owner(slot,{});
    std::ifstream objects(fixture+"/native-capture-objects.csv");require(bool(objects),"Fixture object inventory missing");std::string line;
    while(std::getline(objects,line)){unsigned long long id;unsigned address,kind,definition;require(std::sscanf(line.c_str(),"%llu,%u,%u,%u",&id,&address,&kind,&definition)==4,"Malformed object fixture");if(kind==unsigned(EntityKind::Enemy))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedActor,address,definition});if(kind==unsigned(EntityKind::Pickup))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,address,definition});ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,address});}
    // A publication fixture may index only connected players. Re-register the
    // actual reserved actors from the matching loaded party registry as well.
    for(unsigned slot=0;slot<8;++slot){const auto actor=mem_read32(ssne::native_party_registry()+slot*4);require(actor>=0x80000000u&&actor<=ssne::guest_ram_end()-0x790&&mem_read32(actor+0x48)==0x802121E8u&&mem_read32(actor+0x17C)==slot,"Eight-slot fixture registry is invalid");ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,actor});}
    ssne::netplay_set_local_player(0);local.set_player_owner(0,{0,1});
    auto full=local.capture(1);require(full.error.empty(),full.error);require(players(full)==8&&local.player_bindings().size()==8,"Full capture/bindings omitted reserved actors");
    const auto read_only=digest();auto one=local.capture_for_publication(1);require(one.error.empty(),one.error);require(players(one)==1,"Publication included disconnected reservations");other_entities_equal(full,one);require(digest()==read_only,"Publication capture modified guest RAM");
    // No ready/occupancy change is made: generation alone represents this
    // newly connected participant, whose first bootstrap still needs its data.
    local.set_player_owner(7,{7,7});auto two=local.capture_for_publication(2);require(two.error.empty(),two.error);require(players(two)==2,"Connected but unready player was filtered");full=local.capture(2);other_entities_equal(full,two);require(players(full)==8&&local.player_bindings().size()==8,"Publication changed full capture/bindings");
    auto source=authority(two);const auto host=player_id(source,0),guest=player_id(source,7);require(host&&guest,"Publication lacks host/new join");
    NativeReplica replica(local);auto result=replica.apply(context,1,source);require(result.applied,result.error);
    const auto actor=replica.local_address(guest),hud=mem_read32(actor+0x374);const auto native_id=local.local_entity_id(actor);require(actor&&hud&&native_id,"Joined player has no verified native reservation");
    const auto protected_actor=region(actor,0x790),protected_hud=region(hud,0x534);unsigned destroyed=0;
    const auto subscription=ssne::observe_native_entities([&](const ssne::NativeEntityNotice& notice){if(notice.event==ssne::NativeEntityNotice::Event::Removing&&notice.object==actor)++destroyed;});
    auto detached=source;detached.erase(guest);local.set_player_owner(7,{});
    auto invalid=detached;invalid.at(host).properties["health"]=std::numeric_limits<float>::quiet_NaN();auto before=digest();result=replica.apply(context,1,invalid);require(!result.applied&&digest()==before&&replica.local_address(guest)==actor,"Late invalid field committed a logical detach or RAM writes");
    // Keep a real camera/model lease active while a disconnect sample arrives.
    auto visual=presentation_fields(source.at(host));auto camera=std::get<std::array<float,4>>(visual.properties.at("camera_row0"));camera[3]+=.125f;visual.properties["camera_row0"]=camera;
    std::map<EntityId,EntityState> visuals{{host,std::move(visual)}};require(replica.begin_presentation(visuals,error),error);const auto leased=digest();result=replica.apply(context,1,detached);require(!result.applied&&digest()==leased&&replica.local_address(guest)==actor,"Disconnect invalidated active presentation lease");replica.end_presentation();require(digest()==before,"Rejected disconnect lease did not restore exact RAM");
    result=replica.apply(context,1,detached);require(result.applied,result.error);require(!replica.local_address(guest),"Disconnected logical binding survived");
    require(local.local_entity_id(actor)==native_id&&mem_read32(ssne::native_party_registry()+7*4)==actor&&!destroyed,"Logical detach destroyed/replaced reserved native actor");
    require(region(actor,0x790)==protected_actor&&region(hud,0x534)==protected_hud,"Logical detach changed reserved actor/HUD storage");
    // Same native slot, new authenticated generation, fresh logical attachment.
    local.set_player_owner(7,{7,8});two=local.capture_for_publication(3);require(two.error.empty(),two.error);source=authority(two);require(player_id(source,7)==guest,"Reserved native lifetime changed across disconnect");result=replica.apply(context,1,source);require(result.applied&&replica.local_address(guest)==actor,result.error);
    auto stale=source;stale.at(guest).owner={7,7};before=digest();result=replica.apply(context,1,stale);require(!result.applied&&digest()==before,"Stale connection generation was applied");
    auto missing=source;missing.erase(guest);result=replica.apply(context,1,missing);require(!result.applied&&digest()==before&&replica.local_address(guest)==actor,"Connected unready player was detached");result=replica.apply(context,2,missing);require(!result.applied&&digest()==before,"Fresh epoch accepted an omitted connected player");
    missing=source;missing.erase(host);result=replica.apply(context,1,missing);require(!result.applied&&digest()==before,"Active host was detached");
    result=replica.apply(context,2,source);require(result.applied&&replica.local_address(guest)==actor,result.error);
    // A disconnected own slot may detach; a connected local/host never may.
    ssne::netplay_set_local_player(7);local.set_player_owner(7,{});detached=source;detached.erase(guest);result=replica.apply(context,2,detached);require(result.applied&&!replica.local_address(guest)&&!destroyed,result.error);ssne::netplay_set_local_player(0);
    const auto final_full=local.capture(4),final_public=local.capture_for_publication(4);require(final_full.error.empty()&&final_public.error.empty()&&players(final_full)==8&&players(final_public)==1&&local.player_bindings().size()==8,"Disconnect modified full/native binding inventory");other_entities_equal(final_full,final_public);
    ssne::stop_observing_native_entities(subscription);
    std::cout<<"{\"passed\":true,\"native_reserved_players\":8,\"full_capture_players\":8,\"publication_players\":[1,2,1],\"new_unready_included\":true,\"logical_detach_only\":true,\"native_actor_hud_preserved\":true,\"rejoin_new_generation\":true,\"epoch_rebind\":true,\"stale_owner_rejected\":true,\"connected_omission_rejected\":true,\"nan_atomic\":true,\"lease_atomic\":true,\"other_entities_unchanged\":true}\n";std::cout.flush();std::_Exit(0);
}
