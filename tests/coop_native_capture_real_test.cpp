#include "native_world.h"
#include "native_configuration.h"
#include "ssne/native_entity.h"
#include "ssne/runtime.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
using namespace port::coop;
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
uint64_t digest(unsigned size){uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<size;++i){hash^=ssne::g_ram[i];hash*=1099511628211ull;}return hash;}
int main(int argc,char** argv) {
    require(argc==3,"Pass PAL game_data and real native fixture player directory");
    PPCContext context;std::string error;require(ssne::boot(context,argv[1],error)!=0,error.c_str());
    std::ifstream input(std::string(argv[2])+"/native-capture.ram",std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(input)),{});
    require(ram.size()==0x01800000 || ram.size()==0x04000000,"Real capture RAM size");
    memcpy(ssne::g_ram,ram.data(),ram.size());require(ssne::guest_ram_size()==ram.size(),"Capture active RAM differs");
    auto before=digest(unsigned(ram.size()));
    auto config=read_native_configuration(native_global_configuration(),"GameSetup");
    require(config.error.empty() && config.unsupported==0,"Native config contains unreadable variables");
    require(config.values.size()==34,"Native GameSetup variable count");
    require(std::get<int64_t>(config.values.at("GameSetup.RulesCoop.Difficulty"))==1,"Native integer decoded as float");
    require(std::get<int64_t>(config.values.at("GameSetup.Player3.Profile"))==-1,"Native negative profile value");
    require(std::get<std::string>(config.values.at("GameSetup.RulesCoop.Level"))=="Rlevel1_1","Native level string");
    NativeWorld world;for(unsigned slot=0;slot<8;++slot)world.set_player_owner(slot,{slot,1});
    std::ifstream objects(std::string(argv[2])+"/native-capture-objects.csv");std::string row;size_t expected=0;
    uint32_t pickup=0;EntityId pickup_id=0;
    while(std::getline(objects,row)) {
        unsigned long long id;unsigned address,kind,definition;
        require(std::sscanf(row.c_str(),"%llu,%u,%u,%u",&id,&address,&kind,&definition)==4,"Capture metadata malformed");
        (void)id;
        if(kind==unsigned(EntityKind::Enemy))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedActor,address,definition});
        if(kind==unsigned(EntityKind::Pickup)){ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,address,definition});pickup=address;}
        ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,address});++expected;
    }
    auto sample=world.capture(1);require(sample.entities.size()==expected && expected>50,"Real native actors omitted");
    unsigned players=0;
    for(const auto& [id,entity]:sample.entities) {
        auto address=world.local_address(id);require(address!=0,"Network ID has no local binding");
        auto xyz=std::get<std::array<float,3>>(entity.properties.at("position"));
        for(unsigned axis=0;axis<3;++axis)require(xyz[axis]==u2f(mem_read32(address+0x20+4*axis)),"Native transform capture differs");
        if(address==pickup)pickup_id=id;
        if(entity.kind!=EntityKind::Player)continue;
        ++players;require(entity.properties.at("respawn_timer")==Property(0.f),"Idle timer leaks uninitialized guest memory");
        const auto& ammo=std::get<std::vector<int64_t>>(entity.properties.at("ammo"));require(ammo.size()==18,"Native ammo slots truncated");
        auto inventory=mem_read32(address+0x5E8);for(unsigned slot=0;slot<18;++slot)require(ammo[slot]==mem_read16(inventory+slot*2),"Native ammo value differs");
    }
    require(players==2,"Real two-player identities");
    require(digest(unsigned(ram.size()))==before,"Read-only adapter altered guest RAM");
    require(pickup_id!=0,"No pickup available to exercise lifetime");
    auto descriptor=std::get<std::array<float,3>>(sample.entities.at(pickup_id).properties.at("position"));
    descriptor[1]-=.008f; // the measured registration/animation timing difference
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Spawned,pickup,0,-2,descriptor});
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,pickup});
    auto identity=world.capture(2).entities.at(pickup_id);
    require(std::get<std::array<float,3>>(identity.properties.at("spawn_position"))==descriptor,"Registration overwrote the immutable factory spawn descriptor");
    require(std::get<std::array<float,3>>(identity.properties.at("position"))!=descriptor,"Spawn metadata overwrote the live transform");
    require(digest(unsigned(ram.size()))==before,"Spawn identity recording altered native RAM");
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Removing,pickup});require(!world.local_address(pickup_id),"Removed address still resolves");
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,pickup,mem_read32(pickup+0x180)});
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,pickup});
    sample=world.capture(2);bool fresh=false;for(const auto& [id,entity]:sample.entities)if(world.local_address(id)==pickup){require(id!=pickup_id,"Reused pointer retained old network identity");fresh=true;}
    require(fresh,"New lifetime not indexed");
    ssne::notify_native_entity({ssne::NativeEntityNotice::Event::LevelExit});require(world.capture(3).entities.empty(),"Old level bindings retained");
    std::cout<<"Real engine fixture: "<<expected<<" typed actors, two players, eighteen ammo slots, 34 GameSetup variables, read-only RAM, removal/pointer reuse and level cleanup passed.\n";
    std::cout.flush();std::_Exit(0);
}
