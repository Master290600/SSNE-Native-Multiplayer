#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "ssne/game_addresses.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
using ssne::game_address;
void identity(PPCContext& c){for(unsigned i=0;i<12;++i)mem_write32(c.r[4]+4*i,f2u(i==0||i==5||i==10?1.f:0.f));}
int main(int argc,char**argv){
 assert(argc==2);ssne::set_netplay_step([](uint64_t,uint64_t,bool,bool&){return true;});
 PPCContext base;std::string error;assert(ssne::boot(base,argv[1],error));
 base.r[13]=0x802E22A0; ssne_install_hook(0x8014C048,identity);
 constexpr uint32_t camera=0x81000000,vtable=0x81001000;
 mem_write32(camera,vtable);mem_write16(vtable+0x20,0);mem_write32(vtable+0x24,0x8014C048);
 for(unsigned i=0;i<12;++i)mem_write32(0x802921B4+4*i,f2u(i==0||i==5||i==10?1.f:0.f));
 mem_write32(0x80292214,0);mem_write32(0x80292218,0);mem_write32(0x8029221C,f2u(1));
 mem_write32(base.r[13]-0x5C04,f2u(448));
 mem_write32(0x802922E0,f2u(1.142));mem_write32(0x802922F4,f2u(1.632));
 std::array<uint32_t,2> projection{mem_read32(0x802922E0),mem_read32(0x802922F4)};
 auto inside=[&](float x,float y,float z){
    for(unsigned i=0;i<4;++i) {
     auto p=0x80292260+16*i;
     float d=u2f(mem_read32(p+12));
     float center=u2f(mem_read32(p+8))*(-10)+d;
     float point=u2f(mem_read32(p))*x+u2f(mem_read32(p+4))*y+u2f(mem_read32(p+8))*z+d;
     if(center*point<0)return false;
    }return true;
 };
 auto call=[&](uint32_t lr){PPCContext c=base;c.r[3]=camera;c.lr=lr;ssne_find_func(0x8014D5F4)->func(c);};
 call(0);assert(!inside(20,0,-10));assert(inside(0,0,-10));
 ssne::netplay_set_visibility(4.083f,2.334f);call(0x80117B54);assert(inside(20,0,-10));assert(inside(0,10,-10));assert(!inside(100,0,-10));
 assert(mem_read32(0x802922E0)==projection[0] && mem_read32(0x802922F4)==projection[1]);
 std::puts("Native frustum: widescreen edge objects retained, outside objects rejected, projection restored");
}
