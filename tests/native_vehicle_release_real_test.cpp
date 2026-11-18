// Private owned-PAL vehicle release test; requires the matching current party component.
#include "native_vehicle.h"
#include "native_lifecycle.h"
#include "native_world.h"
#include "ssne/native_entity.h"
#include "ssne/native_party.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
using namespace port::coop;
namespace {
void require(bool ok,const std::string& why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
std::vector<uint8_t> read(const std::string& path){std::ifstream in(path,std::ios::binary);require(bool(in),"Missing fixture: "+path);return {(std::istreambuf_iterator<char>(in)),{}};}
uint64_t digest(){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<ssne::guest_ram_size();++i){h^=ssne::g_ram[i];h*=1099511628211ull;}return h;}
std::vector<uint8_t> region(uint32_t p,unsigned n){require(p>=0x80000000u&&p<=ssne::guest_ram_end()-n,"Invalid protected native region");const auto* b=ssne::host_ptr(p);return {b,b+n};}
void native_call(PPCContext& context,uint32_t address,uint32_t object,uint32_t argument=0){
    const auto low=context.r[1]-0x2000;auto saved=region(low,0x2000);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=object;call.r[4]=argument;
    ssne_call(call,address);std::memcpy(ssne::host_ptr(low),saved.data(),saved.size());
}
}
int main(int argc,char** argv){
    require(argc==3||argc==4,"Pass owned game_data, current vehicle fixture and optional expected type154/155/158");
    const unsigned expected=argc==4?unsigned(std::strtoul(argv[3],nullptr,10)):0;
    require(!expected||expected==154||expected==155||expected==158,"Invalid expected vehicle type");
    ssne::native_party_extended_memory(true);PPCContext context;std::string error;require(ssne::boot(context,argv[1],error)!=0,error);
    const std::string folder=argv[2];auto ram=read(folder+"/native-capture.ram");require(ram.size()==ssne::guest_ram_size(),"Extended PAL RAM fixture required");std::memcpy(ssne::g_ram,ram.data(),ram.size());ram.clear();ram.shrink_to_fit();
    auto cpu=read(folder+"/native-capture-context.bin");require(cpu.size()==sizeof(context),"CPU fixture layout differs");std::memcpy(&context,cpu.data(),sizeof(context));context.msr&=~0x8000u;context.r[1]=ssne::guest_ram_end()-0x40000;mem_write32(context.r[1],0);
    auto cache=read(folder+"/native-capture-locked-cache.bin");require(cache.size()==ssne::kLockedCacheSize,"Locked cache layout differs");std::memcpy(ssne::g_locked_cache,cache.data(),cache.size());
    require(ssne::native_party_load_state(read(folder+"/native-capture-party.bin"),error),error);require(ssne::native_party_capacity()==8,"Use current eight-reservation fixture");
    const auto host_actor=mem_read32(ssne::native_party_registry());uint32_t actor=host_actor;require(actor>=0x80000000u&&actor<=ssne::guest_ram_end()-0x790&&mem_read32(actor+0x48)==0x802121E8u&&mem_read32(actor+0x17C)==0,"Live canonical player0 unavailable");
    ssne::netplay_set_local_player(0);
    NativeWorld world;for(unsigned slot=0;slot<8;++slot)world.set_player_owner(slot,{});world.set_player_owner(0,{0,1});world.set_player_owner(1,{1,2});
    std::ifstream objects(folder+"/native-capture-objects.csv");require(bool(objects),"Native fixture object inventory missing");std::string line;
    while(std::getline(objects,line)){unsigned long long id;unsigned address,kind,definition;require(std::sscanf(line.c_str(),"%llu,%u,%u,%u",&id,&address,&kind,&definition)==4,"Malformed native fixture object inventory");if(kind==unsigned(EntityKind::Enemy))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedActor,address,definition});if(kind==unsigned(EntityKind::Pickup))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,address,definition});ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,address});}
    for(unsigned slot=0;slot<8;++slot)ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,mem_read32(ssne::native_party_registry()+slot*4)});
    // Selecting existing authored vehicles avoids inventing factory resources.
    Properties snapshot;require(capture_native_vehicles(snapshot,error),error);
    const auto& tags=std::get<std::vector<int64_t>>(snapshot.at("Session.Vehicles.Tags"));require(tags.size()%20==0,"Malformed vehicle fixture tags");
    int64_t selected_reference=0;
    for(size_t n=0;n<tags.size();n+=20)if((!expected||unsigned(tags[n+7])==expected)&&tags[n+4]==1){require(!selected_reference,"Fixture has multiple Client1 vehicles");selected_reference=tags[n];}
    if(!selected_reference)for(size_t n=0;n<tags.size();n+=20)if((!expected||unsigned(tags[n+7])==expected)&&tags[n+4]<0){selected_reference=tags[n];break;}
    require(selected_reference>0,"Fixture has no Client1-driven or unoccupied requested vehicle");
    unsigned tested=0,private_entries=0;std::vector<unsigned> types;unsigned removed_players=0;
    const auto observer=ssne::observe_native_entities([&](const ssne::NativeEntityNotice& notice){if(notice.event==ssne::NativeEntityNotice::Event::Removing&&notice.object==actor)++removed_players;});
    for(size_t n=0;n<tags.size();n+=20){
        const unsigned type=unsigned(tags[n+7]);if(tags[n]!=selected_reference)continue;
        const auto wrapper=native_script_reference_address(tags[n]);require(wrapper.has_value(),"Authored vehicle reference vanished");
        // A fresh live fixture can already have the departing client driving.
        // Keep that exact actor, or enter an empty authored vehicle originally.
        if(!mem_read32(*wrapper+0x7C))native_call(context,0x80086064u,*wrapper);
        const auto car=mem_read32(*wrapper+0x60),class_id=type==154?0x80225568u:type==155?0x80224D30u:0x80224320u;
        require(car>=0x80000000u&&car<=ssne::guest_ram_end()-0x458&&mem_read32(car+0x48)==class_id&&mem_read32(car+0x1D4)==*wrapper,"Original vehicle Show produced wrong class/owner");
        actor=mem_read32(car+0x160);require(!actor||mem_read32(actor+0x17C)==1,"Selected car belongs to another participant");
        actor=mem_read32(ssne::native_party_registry()+4);world.set_player_owner(1,{1,uint64_t(2+tested)});
        require(actor>=0x80000000u&&actor<=ssne::guest_ram_end()-0x790&&mem_read32(actor+0x48)==0x802121E8u,"Reserved client1 actor unavailable");
        if(mem_read32(car+0x160)!=actor){require(release_native_vehicle_driver(context,actor,error),error);native_call(context,0x800B54D8u,actor,car);++private_entries;}
        const auto slot=mem_read32(actor+0x17C);require(slot==1&&mem_read32(ssne::native_party_registry()+slot*4)==actor,"Fixture driver registry mismatch");
        require(mem_read32(actor+0x6C4)==car&&mem_read32(car+0x160)==actor&&mem_read32(actor+0x340)==3,"Original vehicle Enter did not establish both links");
        const auto other=mem_read32(ssne::native_party_registry()+(slot?0:4));const auto other_before=region(other,0x790);
        const auto other_car=mem_read32(other+0x6C4);std::vector<uint8_t> other_car_before;
        if(other_car){const auto vt=mem_read32(other_car+0x48);require(vt==0x80225568u||vt==0x80224D30u||vt==0x80224320u,"Other driver's car class invalid");other_car_before=region(other_car,vt==0x80224D30u?0x384:vt==0x80224320u?0x448:0x458);require(mem_read32(other_car+0x160)==other&&mem_read32(other+0x340)==3,"Other driver links invalid before release");}
        auto reject=[&](const PPCContext& ctx,uint32_t who,const char* why){auto copy=ctx;const auto before=digest();std::string reason;require(!release_native_vehicle_driver(copy,who,reason)&&!reason.empty(),why);require(digest()==before,"Rejected vehicle release changed native RAM");};
        auto bad_context=context;bad_context.r[13]=0;reject(bad_context,actor,"Invalid context was accepted");reject(context,actor+4,"Invalid actor class was accepted");
        const auto registry=ssne::native_party_registry()+slot*4;mem_write32(registry,0);reject(context,actor,"Missing registry owner was accepted");mem_write32(registry,actor);
        mem_write32(car+0x160,other);reject(context,actor,"Mismatched car/actor links were accepted");mem_write32(car+0x160,actor);
        mem_write32(car+0x1D4,0);reject(context,actor,"Unverified level wrapper was accepted");mem_write32(car+0x1D4,*wrapper);
        const auto movement=mem_read32(actor+0x340);mem_write32(actor+0x340,1);reject(context,actor,"Inconsistent movement mode was accepted");mem_write32(actor+0x340,movement);
        require(release_native_vehicle_driver(context,actor,error),error);
        require(!mem_read32(actor+0x6C4)&&!mem_read32(car+0x160)&&mem_read32(actor+0x340)==1,"Original exit left stale driver links/mode");
        require(!removed_players&&mem_read32(registry)==actor&&mem_read32(actor+0x48)==0x802121E8u&&mem_read32(*wrapper+0x60)==car&&mem_read32(car+0x48)==class_id,"Release destroyed player/car reservation");
        require(region(other,0x790)==other_before,"Release modified another participant's actor");
        if(other_car)require(region(other_car,unsigned(other_car_before.size()))==other_car_before&&mem_read32(other+0x6C4)==other_car&&mem_read32(other_car+0x160)==other&&mem_read32(other+0x340)==3,"Release modified another participant's driven car");
        // Regression boundary: connection-only publication omits this player,
        // so the original vehicle must no longer advertise it as its driver.
        world.set_player_owner(slot,{});const auto before_capture=digest();auto published=world.capture_for_publication(10+tested);require(published.error.empty(),published.error);
        bool host_present=false;for(const auto& [id,entity]:published.entities)if(entity.kind==EntityKind::Player){const auto player_slot=std::get<int64_t>(entity.properties.at("player_slot"));require(player_slot!=slot,"Disconnected vehicle driver survived publication filter");if(player_slot==0){host_present=true;require(entity.owner==PlayerId{0,1},"Connected host identity changed");}}
        require(host_present,"Connected host disappeared with departed driver");Properties released;require(capture_native_vehicles(released,error),error);require(validate_native_vehicles(released,published.entities,error),"Filtered publication/vehicle boundary failed: "+error);require(digest()==before_capture,"Filtered capture/vehicle validation changed guest RAM");
        const auto after=digest();require(release_native_vehicle_driver(context,actor,error),error);require(digest()==after,"Driver-free release was not an exact no-op");
        types.push_back(type);++tested;
    }
    require(tested>0,"Fixture contains no requested authored vehicle");ssne::stop_observing_native_entities(observer);
    std::cout<<"{\"passed\":true,\"native_reserved_players\":8,\"vehicles_tested\":"<<tested<<",\"private_original_entries\":"<<private_entries<<",\"raw_publication_unmodified\":true,\"types\":[";for(size_t i=0;i<types.size();++i)std::cout<<(i?",":"")<<types[i];
    std::cout<<"],\"original_enter_exit\":true,\"fresh_bidirectional_validation\":true,\"disconnected_driver_filtered_vehicle_validation_passed\":true,\"invalid_releases_atomic\":true,\"other_player_preserved\":true,\"other_driven_car_preserved\":true,\"player_car_lifetimes_preserved\":true,\"idempotent_noop\":true}\n";std::cout.flush();std::_Exit(0);
}
