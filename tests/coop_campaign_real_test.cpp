#include "../runtime/src/native_party.cpp"
#include "ssne/runtime.h"
#include "ssne/native_party.h"
#include "../port/src/coop/native_lifecycle.h"
#include "../port/src/coop/native_vehicle.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv)try{
    require(argc==3,"Pass owned PAL game_data and captured fixture directory");
    ssne::native_party_extended_memory(true);PPCContext ctx;std::string error;
    require(ssne::boot(ctx,argv[1],error)!=0,error.c_str());
    const auto root=std::string(argv[2]);std::ifstream ram(root+"/native-capture.ram",std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ram)),{});
    require(bytes.size()==ssne::guest_ram_size(),"Extended PAL fixture required");std::memcpy(ssne::g_ram,bytes.data(),bytes.size());
    std::ifstream cpu(root+"/native-capture-context.bin",std::ios::binary);cpu.read(reinterpret_cast<char*>(&ctx),sizeof(ctx));require(bool(cpu),"CPU fixture required");ctx.msr&=~0x8000u;
    require(mem_read32(0x80000000)==0x47334250u,"PAL fixture required");
    const auto level=mem_read32(0x802DACC0),manager=mem_read32(level+0x298),count=mem_read32(manager+0x1C),entries=mem_read32(manager+0x18);
    std::vector<int64_t> states;
    require(port::coop::capture_native_model_states(states,error),error.c_str());
    require(states.size()==count,"Captured catalog length");
    require(port::coop::apply_native_model_states(ctx,states,error),error.c_str());
    auto invalid=states;invalid.push_back(0);require(!port::coop::validate_native_model_states(invalid,error),"Mismatched catalog must fail");
    mem_write32(manager+0x1C,4097);require(!port::coop::capture_native_model_states(invalid,error),"Oversized table must fail");
    mem_write32(manager+0x1C,1);mem_write32(manager+0x18,0);require(!port::coop::capture_native_model_states(invalid,error),"Nonempty null table must fail");
    mem_write32(manager+0x1C,0);require(port::coop::capture_native_model_states(invalid,error) && invalid.empty(),"Zero native models allow null storage");
    require(port::coop::apply_native_model_states(ctx,invalid,error),"Apply zero native models");
    mem_write32(manager+0x1C,count);mem_write32(manager+0x18,entries);
    require(port::coop::capture_native_model_states(invalid,error) && invalid==states,"Original model state preserved");
    for(uint32_t a=0x81000000u;a<ssne::guest_ram_end()-0xA0u;a+=4)if(mem_read32(a+0x48)==0x80206348u && mem_read32(a+0x50)==0){
        const auto config=mem_read32(a+0x54),resource=mem_read32(config);const auto geometry=int32_t(mem_read32(entries+resource*20+8));
        port::coop::EntityState e;e.id=3;e.kind=port::coop::EntityKind::Destructible;e.definition=2;
        e.properties={{"destruct_index",int64_t(0)},{"destruct_resource",int64_t(resource)},{"destruct_state",int64_t(geometry)},{"destruct_active",int64_t(mem_read32(a+0x84))},{"destruct_stage",int64_t(mem_read32(a+0x88))},{"destruct_destroyed",int64_t(geometry<0)}};
        require(port::coop::validate_native_destructible(a,e,error),error.c_str());
        const auto original=e;e.properties["destruct_state"]=int64_t(-1);e.properties["destruct_destroyed"]=int64_t(1);e.properties["destruct_active"]=int64_t(0);
        require(port::coop::apply_native_destructible(ctx,a,e,error),error.c_str());
        require(int32_t(mem_read32(entries+resource*20+8))==-1 && mem_read32(a+0x84)==std::get<int64_t>(original.properties.at("destruct_active")),"Unindexed geometry setter readback without collateral scalar changes");
        require(port::coop::apply_native_destructible(ctx,a,original,error),error.c_str());e=original;
        require(int32_t(mem_read32(entries+resource*20+8))==geometry,"Unindexed destruction restoration");
        e.properties["destruct_resource"]=int64_t(resource+1);require(!port::coop::validate_native_destructible(a,e,error),"Unindexed geometry resource mismatch rejected");
        e.properties["destruct_resource"]=int64_t(resource);e.properties["destruct_index"]=int64_t(-1);require(!port::coop::validate_native_destructible(a,e,error),"Negative destructible index rejected");
        std::cout<<"PAL unindexed authored destructible accepted; mismatched resource and negative index rejected.\n";break;
    }
    port::coop::Properties vehicles;
    require(port::coop::capture_native_vehicles(vehicles,error),error.c_str());
    require(port::coop::apply_native_vehicle_poses(vehicles,error),error.c_str());
    port::coop::Properties vehicle_readback;require(port::coop::capture_native_vehicles(vehicle_readback,error) && vehicle_readback==vehicles,"Native vehicle pose roundtrip");
    std::cout<<"PAL native vehicle class/model/fuel/pose roundtrip passed.\n";
    uint32_t actor=0;for(uint32_t a=0x80000100u;a<ssne::guest_ram_end()-0x790u;a+=4)if(mem_read32(a+0x48)==0x802121E8u && mem_read32(a+0x17C)==0 && u2f(mem_read32(a+0xCC))>0){actor=a;break;}require(ssne::native_player_collision_radius(actor).has_value(),"PAL player fixture required");
    port::coop::EntityState notice;notice.id=1;notice.kind=port::coop::EntityKind::Player;
    require(port::coop::capture_native_player_notices(actor,notice,error),error.c_str());
    require(port::coop::validate_native_player_notices(actor,notice,error),error.c_str());
    std::cout<<"PAL captured Netricsa catalog validation passed.\n";
    std::array<float,3> above{},placed{};for(unsigned axis=0;axis<3;++axis)above[axis]=u2f(mem_read32(actor+0x20+axis*4));above[1]+=10;
    require(ssne::choose_coop_spawn(ctx,actor,above,placed,error),error.c_str());
    require(std::abs(above[1]-placed[1])<.001f,"Native unsupported spawn keeps original height");
    bool overlap=false;require(ssne::spawn_actor_overlap(ctx,actor,placed,*ssne::native_player_collision_radius(actor),overlap,error) && !overlap,"Free anchor still separates collidable actors");
    std::cout<<"PAL native free-anchor spawn passed: original height and collision separation.\n";
    uint32_t boss=0;for(uint32_t a=0x81000000u;a<ssne::guest_ram_end()-0x240u;a+=4)if(mem_read32(a+0x48)==0x801EA4A8u){boss=a;break;}
    if(boss){
        port::coop::EntityState e;e.id=2;e.kind=port::coop::EntityKind::Enemy;e.definition=42;
        require(port::coop::capture_native_boss_parts(boss,e,error),error.c_str());
        const auto original=e;auto& v=std::get<std::vector<float>>(e.properties.at("boss_weakpoints"));require(v.size()==112 && v[0]==1,"Live Dark Lord fixture required");
        const auto first=mem_read32(boss+0x1FC);v[3]=std::max(0.f,v[3]-1);
        require(port::coop::apply_native_boss_parts(boss,e,error),error.c_str());
        port::coop::EntityState read=e;require(port::coop::capture_native_boss_parts(boss,read,error) && read.properties==e.properties,"Weakpoint native readback");
        for(unsigned k=0;k<7;++k)v[k]=0;
        require(port::coop::apply_native_boss_parts(boss,e,error),error.c_str());
        require(mem_read32(boss+0x1FC)==first && mem_read32(first+0x1BC)==2,"Retired local weakpoint retains ownership for cleanup");
        require(port::coop::capture_native_boss_parts(boss,read,error) && read.properties==e.properties,"Retired weakpoint readback");
        v[0]=.5f;require(!port::coop::validate_native_boss_parts(e,error),"Fractional weakpoint presence rejected");
        require(port::coop::apply_native_boss_parts(boss,original,error),error.c_str());
        require(port::coop::capture_native_boss_parts(boss,read,error) && read.properties==original.properties,"Weakpoint restoration");
        std::cout<<"PAL Dark Lord sixteen weakpoints passed: native readback, health, retirement, ownership, validation and restoration.\n";
    }
    std::cout<<"PAL campaign geometry passed: count="<<count<<", empty catalog accepted, nonempty-null/oversize/mismatched catalogs rejected, original state preserved.\n";std::cout.flush();std::_Exit(0);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
