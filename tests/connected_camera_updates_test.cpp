#include "ssne/connected_camera_updates.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <cstdlib>
using namespace ssne::camera_updates;
namespace {
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value){std::cerr<<why<<'\n';std::exit(1);}}
std::array<Binding,8> fixture(){
    std::array<Binding,8> out{};
    for(unsigned i=0;i<8;++i){auto& b=out[i];b.actor=0x80800000u+i*0x1000;b.actor_class=0x802121E8u;b.slot=i;
        b.camera=0x81000000u+i*0x1000;b.actor_camera=b.camera;b.camera_class=0x80210880u;b.camera_owner=b.actor;
        b.movement=1;b.fov=0x81800000u+i*0x1000;b.fov_class=0x80210E80u;b.fov_method=0x800A981Cu;b.health=100.f;}
    return out;
}
uint32_t run(const std::array<Binding,8>& b,uint8_t connected=1,uint8_t occupied=1,uint32_t canonical=15,
             uint32_t legacy=255,unsigned capacity=8,bool eligible=true){
    std::array<std::byte,sizeof(b)> before{};std::memcpy(before.data(),b.data(),before.size());
    const auto result=select(canonical,legacy,capacity,eligible,occupied,connected,0x84000000u,b);
    require(!std::memcmp(before.data(),b.data(),before.size()),"Policy mutated borrowed native facts");return result;
}
}
int main(){
    const auto original=fixture();
    require(run(original)==1,"Solo host retained disconnected world-camera callbacks");
    require(run(original,3,1)==3,"Connected unready player lost camera updates");
    require(run(original,255,255)==255,"Full party changed camera updates");
    for(unsigned connected=1;connected<256;connected+=2){
        require(run(original,uint8_t(connected),1)==connected,"Connected roster mask differs");
        require(run(original,uint8_t(connected),uint8_t(connected))==connected,"Ready roster changed connected selection");
    }
    require(run(original,1,1,15,0xA50000FF)==0xA5000001,"Nonworld mask bits changed");
    for(uint32_t mask=0;mask<0x10000;++mask)if(mask!=15)
        require(run(original,1,1,mask,0x12345678)==0x12345678,"A noncanonical/UI mask was filtered");
    require(run(original,1,1,15,255,8,false)==255,"Ineligible phase changed legacy mask");
    require(run(original,1,0)==255,"Unready host enabled camera filtering");
    require(run(original,0,1)==255,"Disconnected host enabled camera filtering");
    require(run(original,1,3)==255,"Inconsistent ready/connected roster was accepted");
    require(run(original,1,1,15,255,4)==255,"Nonextended party was filtered");
    require(run(original,1,1,15,255,9)==255,"Invalid capacity was filtered");
    require(run(original,129,1,15,127,7)==127,"Out-of-capacity connected bit was accepted");
    for(auto member:{&Binding::actor_class,&Binding::slot,&Binding::actor_camera,&Binding::camera_class,&Binding::camera_owner,
                     &Binding::mode,&Binding::shake,&Binding::movement,&Binding::vehicle,&Binding::frozen,&Binding::dead,
                     &Binding::fov_class,&Binding::fov_method}){
        auto b=original;b[2].*member^=1;require(run(b)==255,"Malformed fresh identity/side-effect guard was accepted");
    }
    for(auto member:{&Binding::actor,&Binding::camera,&Binding::fov}){
        auto b=original;b[2].*member=0;require(run(b)==255,"Null native range was accepted");
        b=original;b[2].*member=0xFFFFFFF0u;require(run(b)==255,"Wrapped/outside native range was accepted");
    }
    {auto b=original;b[2].fov_adjust=1;require(run(b)==255,"Unknown indirect FOV this-adjust was accepted");}
    for(float health:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        auto b=original;b[2].health=health;require(run(b)==255,"Dead/invalid reserved player was filtered");
    }
    {auto b=original;b[1].mode=2;b[1].shake=1;b[1].movement=3;b[1].vehicle=0x81A00000;b[1].fov=0;b[1].fov_class=0;b[1].fov_method=0;
        require(run(b,3,1)==3,"Connected unready vehicle/other camera mode was filtered");}
    for(uint32_t delta:{0u,1u,0x78Fu}){
        auto b=original;b[2].actor=b[1].actor+delta;b[2].camera_owner=b[2].actor;
        require(run(b)==255,"Shared/partially aliased actor storage was accepted");
    }
    for(uint32_t delta:{0u,1u,0xDFu}){
        auto b=original;b[2].camera=b[1].camera+delta;b[2].actor_camera=b[2].camera;
        require(run(b)==255,"Shared/partially aliased camera storage was accepted");
    }
    {auto b=original;b[2].camera=b[1].actor+4;b[2].actor_camera=b[2].camera;require(run(b)==255,"Actor/camera cross-alias was accepted");}
    {auto b=original;b[2].actor=b[1].actor+0x790;b[2].camera_owner=b[2].actor;require(run(b)==1,"Adjacent actor ranges were rejected");}
    {auto b=original;b[2].camera=b[1].camera+0xE0;b[2].actor_camera=b[2].camera;require(run(b)==1,"Adjacent camera ranges were rejected");}
    std::cout<<"connected camera policy: "<<checks<<" checks passed\n";
}
