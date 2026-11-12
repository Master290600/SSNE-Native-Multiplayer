#include "native_lifecycle.h"
#include "native_world.h"
#include "ssne/native_entity.h"
#include "ssne/native_effect.h"
#include "ssne/native_party.h"
#include "ssne/recomp_hooks.h"
#include <algorithm>
#include <set>
#include "ssne/runtime.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
using namespace port::coop;
void require(bool ok,const std::string& error){if(!ok){std::cerr<<error<<std::endl;std::_Exit(1);}}
// Optional real-RAM/native-allocator regression. Needs two booted party actors;
// synthetic audio hooks cannot prove voice retirement or selection lifetimes.
bool voice_live(uint32_t handle){const auto index=handle&63u;return int32_t(handle)>=0 && index<48 && mem_read32(0x802D69C8u+index*192u) && mem_read32(0x802D69C8u+index*192u+0xAC)==handle;}
std::set<uint32_t> live_voices(){std::set<uint32_t> voices;for(unsigned i=0;i<48;++i){const auto handle=mem_read32(0x802D69C8u+i*192u+0xAC);if(voice_live(handle))voices.insert(handle);}return voices;}
uint32_t added_voice(const std::set<uint32_t>& before){auto after=live_voices();for(auto handle:after)if(!before.contains(handle))return handle;require(false,"Native loop allocator did not add a generation-tagged voice");return UINT32_MAX;}
void weapon_loop_switch_regression(PPCContext& context){
    std::string error;require(ssne::install_native_rocket_effect_capture(true,error),error);
    const auto registry=ssne::native_party_registry();const uint32_t actors[]={mem_read32(registry),mem_read32(registry+4)};
    for(unsigned slot=0;slot<2;++slot)require(actors[slot]>=0x80000000u && actors[slot]<ssne::guest_ram_end()-0x790 && mem_read32(actors[slot]+0x48)==0x802121E8u && mem_read32(actors[slot]+0x17C)==slot,"Weapon-loop regression needs two real party actors");
    auto call_native=[&](uint32_t function,uint32_t a,uint32_t b=0,uint32_t instance=0,uint32_t caller=0){auto c=context;c.r[1]-=0x100;mem_write32(c.r[1],context.r[1]);c.r[3]=a;c.r[4]=b;c.r[31]=instance;c.lr=caller;ssne_call(c,function);return c.r[3];};
    for(unsigned weapon:{2u,4u}){
        const auto offset=weapon==2?0x244u:0x274u;uint32_t instances[2],handles[2];ssne::NativeWeaponLoop starts[2];
        for(unsigned slot=0;slot<2;++slot){
            require(apply_native_weapon_selection(context,actors[slot],weapon,true,error),error);
            instances[slot]=mem_read32(actors[slot]+0x4BC+weapon*24);
            handles[slot]=call_native(0x8010EB9Cu,weapon==2?419:406,weapon==2?0:1,instances[slot],weapon==2?0x80061378u:0x800578D0u);
            require(voice_live(handles[slot]),"Native sustained voice did not start");mem_write32(instances[slot]+offset,handles[slot]);
            auto loops=ssne::native_weapon_active_loops();auto found=std::find_if(loops.begin(),loops.end(),[&](const auto& e){return e.slot==slot && e.weapon==weapon && e.lane==0;});require(found!=loops.end(),"Native start was not journaled");starts[slot]=*found;
        }
        // A normal one-shot has no loop ownership and must survive the switch.
        const auto one_shot=call_native(0x8010E7B0u,417,1);require(voice_live(one_shot),"Protected one-shot did not start");
        const auto cpu=context;require(apply_native_weapon_selection(context,actors[0],1,true,error),error);require(!std::memcmp(&cpu,&context,sizeof(context)),"Weapon-loop stop changed caller CPU state");
        require(mem_read32(instances[0]+offset)==UINT32_MAX && !voice_live(handles[0]),"Old sustained native voice survived weapon selection");
        require(voice_live(handles[1]) && voice_live(one_shot),"Weapon switch cut another player or a one-shot");
        auto loops=ssne::native_weapon_active_loops();require(std::none_of(loops.begin(),loops.end(),[&](const auto& e){return e.origin==starts[0].origin;}),"Switched loop remained in active host snapshot");
        auto events=ssne::native_weapon_loop_history();require(std::any_of(events.begin(),events.end(),[&](const auto& e){return !e.active && e.origin==starts[0].origin && e.slot==0 && e.weapon==weapon;}),"Weapon switch did not publish matching native stop");
        // Even a still-live foreign handle in the old instance cannot confer
        // ownership. A retired generation must not stop its reused record.
        const auto event_count=events.size();
        for(auto invalid:{handles[1],handles[0]}){mem_write32(instances[0]+offset,invalid);require(ssne::stop_native_weapon_loops_for_actor(context,actors[0],weapon,error),error);require(mem_read32(instances[0]+offset)==UINT32_MAX && voice_live(handles[1]) && voice_live(one_shot),"Stale/foreign handle stopped an unrelated voice");}
        require(ssne::native_weapon_loop_history().size()==event_count,"Stale/foreign handle fabricated a host stop");
        require(apply_native_weapon_selection(context,actors[1],1,true,error),error);require(!voice_live(handles[1]),"Second actor's own loop was not retired");
        call_native(0x8010EDFCu,one_shot);
        // Replica presentation starts real native voices but owns no host damage.
        require(ssne::install_native_rocket_effect_capture(false,error),error);uint32_t replica_handles[2];
        for(unsigned slot=0;slot<2;++slot){const auto before=live_voices();require(ssne::apply_native_weapon_loop(context,starts[slot],false,error),error);replica_handles[slot]=added_voice(before);}
        require(ssne::stop_native_weapon_loops_for_actor(context,actors[0],weapon==2?4:2,error),error);require(voice_live(replica_handles[0]),"Replica stopped a different weapon's loop");
        require(ssne::stop_native_weapon_loops_for_actor(context,actors[0],weapon,error),error);require(!voice_live(replica_handles[0]) && voice_live(replica_handles[1]),"Replica switch did not isolate player/weapon");
        auto newer=starts[0];newer.origin+=100000;newer.sequence=newer.origin;const auto before=live_voices();require(ssne::apply_native_weapon_loop(context,newer,false,error),error);const auto newer_handle=added_voice(before);
        auto stale=starts[0];stale.active=false;stale.elapsed_ms=0;stale.sequence=newer.sequence+1;require(ssne::apply_native_weapon_loop(context,stale,false,error),error);require(ssne::update_native_weapon_loops(context,error),error);require(voice_live(newer_handle),"Old origin retired a newer replica loop");
        require(ssne::stop_native_weapon_loops_for_actor(context,actors[0],weapon,error),error);require(ssne::stop_native_weapon_loops_for_actor(context,actors[1],weapon,error),error);require(!voice_live(newer_handle) && !voice_live(replica_handles[1]),"Replica cleanup failed");
        require(ssne::install_native_rocket_effect_capture(true,error),error);
    }
    std::cout<<"Real native Uzi/Minigun selection: matching host stop, scoped replica retirement, stale/origin/foreign safety and one-shot preservation passed.\n"<<std::flush;
}
int main(int argc,char** argv) {
    require(argc==3 || (argc==5 && std::string(argv[3])=="weapon-loop-switch"),"Pass game_data and the captured 64-MiB native level directory [weapon-loop-switch matching-party-checkpoint.bin]");
    // This fixture always contains 64 MiB; allocate that before boot/copy.
    ssne::native_party_extended_memory(true);
    PPCContext context;std::string error;require(ssne::boot(context,argv[1],error)!=0,error);
    std::ifstream input(std::string(argv[2])+"/native-capture.ram",std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(input)),{});require(ram.size()==0x04000000,"Expected real 64-MiB native level fixture");
    memcpy(ssne::g_ram,ram.data(),ram.size());context.r[2]=0x802F22A0;context.r[13]=0x802E22A0;context.r[1]=ssne::guest_ram_end()-0x40000;context.msr&=~0x8000u;
    if(argc==5){
        // RAM alone cannot restore relocated eight-player registry/arena state.
        // Require the matching safe-point component instead of guessing it from
        // heap contents or using the ordinary four-player registry.
        std::ifstream party_file(argv[4],std::ios::binary);std::vector<uint8_t> party((std::istreambuf_iterator<char>(party_file)),{});
        require(!party.empty(),"Pass the native-party checkpoint captured with this RAM fixture");
        require(ssne::native_party_load_state(party,error),error);
        // Geometry/animation/voice callbacks use the captured quantization and
        // floating-point state. RAM and party ownership alone are insufficient.
        std::ifstream cpu_file(std::string(argv[2])+"/native-capture-context.bin",std::ios::binary);
        std::vector<uint8_t> cpu((std::istreambuf_iterator<char>(cpu_file)),{});
        require(cpu.size()==sizeof(context),"Expected the matching captured PPC context");
        std::memcpy(&context,cpu.data(),sizeof(context));
        context.r[1]=ssne::guest_ram_end()-0x40000;context.r[2]=0x802F22A0;context.r[13]=0x802E22A0;context.msr&=~0x8000u;
        std::ifstream cache_file(std::string(argv[2])+"/native-capture-locked-cache.bin",std::ios::binary);
        if(cache_file){std::vector<uint8_t> cache((std::istreambuf_iterator<char>(cache_file)),{});require(cache.size()==ssne::kLockedCacheSize,"Invalid captured locked cache");std::memcpy(ssne::g_locked_cache,cache.data(),cache.size());}
        weapon_loop_switch_regression(context);std::_Exit(0);
    }
    NativeWorld native;std::ifstream objects(std::string(argv[2])+"/native-capture-objects.csv");std::string row;
    while(std::getline(objects,row)) {
        unsigned long long id;unsigned address,kind,definition;require(std::sscanf(row.c_str(),"%llu,%u,%u,%u",&id,&address,&kind,&definition)==4,"Capture metadata");
        if(kind==unsigned(EntityKind::Enemy))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedActor,address,definition});
        if(kind==unsigned(EntityKind::Pickup))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,address,definition});
        ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,address});
    }
    auto sample=native.capture(1);EntityState enemy;
    for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Enemy){enemy=e;break;}
    require(enemy.id!=0,"Fixture has no native enemy");enemy.id=100000;
    auto spawn=std::get<std::array<float,3>>(enemy.properties.at("spawn_position"));spawn[0]+=3;
    enemy.properties["spawn_position"]=spawn;enemy.properties["position"]=spawn;
    auto invalid=enemy;invalid.properties["health"]=std::numeric_limits<float>::quiet_NaN();
    require(!validate_native_enemy_creation(invalid,error),"Nonfinite enemy creation accepted");error.clear();
    require(validate_native_enemy_creation(enemy,error),error);
    const auto before=native.capture(1).entities.size();auto object=create_native_enemy(context,enemy,error);require(object!=0,error);
    sample=native.capture(2);require(sample.entities.size()==before+1,"Factory did not register exactly one enemy");EntityId born=0;
    for(const auto& [id,e]:sample.entities)if(native.local_address(id)==object){born=id;require(e.kind==EntityKind::Enemy && e.definition==enemy.definition,"Created native class differs");require(std::get<std::array<float,3>>(e.properties.at("spawn_position"))==spawn,"Factory placed enemy at the wrong transform");}
    require(born!=0,"Created actor lacks a stable local lifetime");require(validate_native_removal(context,object,error),error);
    require(remove_native_entity(context,object,error),error);
    require(!native.local_address(born) && native.capture(3).entities.size()==before,"Native removal left a registered/stale actor");
    std::cout<<"Real native lifecycle: enemy created by the level factory, registered with a new lifetime, positioned correctly and destroyed through the native update list.\n"<<std::flush;std::_Exit(0);
}
