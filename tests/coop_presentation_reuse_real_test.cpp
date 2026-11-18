// Private owned-PAL test. Including the production registry TU gives the test
// direct access to its private cache for self-alias/capacity/metadata checks,
// like gpu_indirect_shader_test; the runtime archive supplies its other TUs.
#include "../runtime/src/replica_simulation.cpp"
#include "native_replica.h"
#include "client_presentation.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <chrono>
static thread_local bool count_allocations=false;
static thread_local size_t allocation_count=0;
void* operator new(std::size_t size){if(void* p=std::malloc(size?size:1)){if(count_allocations)++allocation_count;return p;}throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
using namespace port::coop;
static void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
static uint64_t ram_digest(){uint64_t h=14695981039346656037ull;const auto bytes=ssne::guest_ram_size();for(unsigned i=0;i<bytes;++i){h^=ssne::g_ram[i];h*=1099511628211ull;}return h;}
static std::vector<uint32_t> words(const std::vector<float>& values){std::vector<uint32_t> result;result.reserve(values.size());for(float value:values)result.push_back(f2u(value));return result;}
int main(int argc,char** argv){
    require(argc==3,"Pass owned game_data and private native-capture fixture directory");
    ssne::native_party_extended_memory(true);PPCContext context;std::string error;require(ssne::boot(context,argv[1],error)!=0,error.c_str());
    std::ifstream input(std::string(argv[2])+"/native-capture.ram",std::ios::binary);std::vector<uint8_t> ram((std::istreambuf_iterator<char>(input)),{});require(ram.size()==ssne::guest_ram_size(),"Owned-PAL fixture RAM size differs from active guest memory");std::memcpy(ssne::g_ram,ram.data(),ram.size());
    std::ifstream cpu(std::string(argv[2])+"/native-capture-context.bin",std::ios::binary);cpu.read(reinterpret_cast<char*>(&context),sizeof(context));require(bool(cpu),"PPC fixture context missing");context.msr&=~0x8000u;context.r[1]=ssne::guest_ram_end()-0x40000;mem_write32(context.r[1],0);
    NativeWorld local;for(unsigned slot=0;slot<8;++slot)local.set_player_owner(slot,{slot,1});
    std::ifstream objects(std::string(argv[2])+"/native-capture-objects.csv");std::string line;
    while(std::getline(objects,line)){unsigned long long id;unsigned address,kind,definition;require(std::sscanf(line.c_str(),"%llu,%u,%u,%u",&id,&address,&kind,&definition)==4,"Invalid capture object metadata");if(kind==unsigned(EntityKind::Enemy))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedActor,address,definition});if(kind==unsigned(EntityKind::Pickup))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,address,definition});ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,address});}
    const auto capture=local.capture(1);require(capture.error.empty(),capture.error.c_str());std::map<EntityId,EntityState> authority;
    for(const auto& [id,entity]:capture.entities){auto copy=entity;copy.id=id+10000;authority.emplace(copy.id,std::move(copy));}
    NativeReplica replica(local);const auto applied=replica.apply(context,1,authority);require(applied.applied,applied.error.c_str());
    std::map<EntityId,EntityState> visual;EntityId enemy=0;std::string pose_key;
    for(const auto& [id,entity]:authority){auto copy=presentation_fields(entity);for(const auto& [key,value]:copy.properties)if(key.ends_with("_bone_matrices"))if(const auto* values=std::get_if<std::vector<float>>(&value);values&&!values->empty()){enemy=id;pose_key=key;}if(auto row=copy.properties.find("render_row0");row!=copy.properties.end())if(auto* values=std::get_if<std::array<float,4>>(&row->second))(*values)[3]+=.125f;visual.emplace(id,std::move(copy));}
    require(enemy!=0,"Fixture lacks a valid body/enemy/weapon presentation pose");
    if(pose_key.starts_with("weapon_model"))ssne::netplay_set_local_player(int(mem_read32(replica.local_address(enemy)+0x17C)));
    const auto before=ram_digest();
    auto invalid=visual;std::get<std::vector<float>>(invalid.at(enemy).properties.at(pose_key)).back()=std::numeric_limits<float>::quiet_NaN();
    require(!replica.begin_presentation(invalid,error),"Invalid late bone coefficient was accepted");require(ram_digest()==before,"Failed presentation preflight changed guest RAM");
    require(replica.begin_presentation(visual,error),error.c_str());const auto during=ram_digest();require(during!=before,"Visual fixture did not change a model root");
    require(!replica.begin_presentation(visual,error),"Nested presentation lease accepted");require(ram_digest()==during,"Rejected nested lease changed visual RAM");replica.end_presentation();require(ram_digest()==before,"Presentation lease did not restore exact guest RAM");
    // Lease preflight accepts this finite word; the pose registry rejects it.
    // This exercises end_presentation rollback AFTER the guest writes happened.
    invalid=visual;std::get<std::vector<float>>(invalid.at(enemy).properties.at(pose_key)).front()=10001.f;
    require(!replica.begin_presentation(invalid,error),"Out-of-range pose registry value accepted");require(ram_digest()==before,"Pose registration failure did not roll back exact guest RAM");
    for(unsigned repeat=0;repeat<3;++repeat){require(replica.begin_presentation(visual,error),error.c_str());require(ram_digest()==during,"Repeated visual plan changed alias/value result");replica.end_presentation();require(ram_digest()==before,"Scratch reuse leaked writes across leases");}
    allocation_count=0;const auto lease_started=std::chrono::steady_clock::now();count_allocations=true;
    for(unsigned repeat=0;repeat<3;++repeat){require(replica.begin_presentation(visual,error),error.c_str());replica.end_presentation();}
    count_allocations=false;const auto lease_allocations_per3=allocation_count;
    const auto lease_nanoseconds_per3=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-lease_started).count();
    require(ram_digest()==before,"Measured warmed leases changed authoritative RAM");
    const auto* scratch_env=std::getenv("SSNE_PRESENTATION_SCRATCH_REUSE");const bool scratch_reuse=!scratch_env||std::strcmp(scratch_env,"0")!=0;
    const auto object=replica.local_address(enemy);auto found=ssne::enemy_poses.end();
    for(auto it=ssne::enemy_poses.begin();it!=ssne::enemy_poses.end();++it)if(it->second.object==object&&!it->second.values.empty()){found=it;if(it->second.weapon<0)break;}
    require(found!=ssne::enemy_poses.end(),"Restored enemy registry entry missing");const uint32_t model=found->first,mesh_stamp=mem_read32(model+0x84);auto& registered=found->second;
    const auto saved_values=registered.values;const int expected_weapon=registered.weapon;const unsigned expected_index=registered.index;
    const auto call_register=[&](const std::vector<float>& values){return expected_weapon<0?ssne::replica_enemy_pose(object,model,values):ssne::replica_weapon_pose(object,unsigned(expected_weapon),expected_index,values);};
    const auto exact=words(registered.values);const auto* storage=registered.values.data();
    registered.object=0;registered.object_type=0;registered.resource=0;registered.weapon=11;registered.index=1;
    require(call_register(registered.values),"Self-alias pose registration failed");require(words(registered.values)==exact,"Self-alias pose registration changed float bits");
    require(registered.object==object&&registered.object_type==mem_read32(object+0x48)&&registered.resource==mem_read32(model+0xC)&&registered.weapon==expected_weapon&&registered.index==expected_index,"Reused pose retained stale metadata");
    const auto* reuse_env=std::getenv("SSNE_POSE_REGISTER_REUSE");const bool reuse=!reuse_env||std::strcmp(reuse_env,"0")!=0;
    if(reuse)require(registered.values.data()==storage,"Self-alias invalidated persistent pose storage");
    auto immutable=registered.values;immutable.front()=-0.f;immutable.back()=-1.25f;require(call_register(immutable),"Pose-register warmup failed");
    allocation_count=0;const auto registry_started=std::chrono::steady_clock::now();count_allocations=true;
    for(unsigned i=0;i<100;++i)require(call_register(immutable),"Repeated valid pose failed");
    count_allocations=false;const auto measured_allocations=allocation_count;
    const auto registry_nanoseconds_per100=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-registry_started).count();
    require(words(registered.values)==words(immutable),"Capacity reuse changed registered float bits");if(reuse)require(measured_allocations==0,"Warm pose-register path still allocates");else require(measured_allocations>=100,"Reference registration did not exercise vector allocation path");
    require(call_register(saved_values),"Original registry pose restore failed");mem_write32(model+0x84,mesh_stamp);require(ram_digest()==before,"Pose-register cache test changed authoritative RAM");
    std::cout<<"{\"passed\":true,\"original_ram_digest\":"<<before<<",\"visual_ram_digest\":"<<during<<",\"pose_register_reuse\":"<<(reuse?"true":"false")<<",\"pose_register_allocations_per100\":"<<measured_allocations<<",\"pose_register_nanoseconds_per100\":"<<registry_nanoseconds_per100<<",\"presentation_scratch_reuse\":"<<(scratch_reuse?"true":"false")<<",\"lease_allocations_per3\":"<<lease_allocations_per3<<",\"lease_nanoseconds_per3\":"<<lease_nanoseconds_per3<<",\"alias_order_unit_test_separate\":true,\"preflight_atomic\":true,\"registration_rollback_exact\":true,\"restore_exact\":true,\"self_alias_exact\":true,\"metadata_fresh\":true}\n";std::cout.flush();std::_Exit(0);
}
