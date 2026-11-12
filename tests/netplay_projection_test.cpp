#include "ssne/runtime.h"
#include "ssne/game_addresses.h"
using ssne::game_address;
#include "ssne/recomp_hooks.h"
#include <array>
#include <cstdio>
#include <cstdlib>
static void check(bool value,const char* why){if(!value){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
int main(int argc,char** argv){
 check(argc==2,"Pass game_data");
 ssne::set_netplay_step([](uint64_t,uint64_t,bool,bool&){return true;});
 PPCContext base;std::string error;check(ssne::boot(base,argv[1],error)!=0,error.c_str());
 base.r[13]=game_address(0x802E1BA0u);
 auto matrix=[&](float fov,float wide,uint32_t function,uint32_t caller){
  ssne::set_fov(fov);ssne::set_widescreen(wide);
  PPCContext c=base;c.r[3]=0x81000000;c.lr=0x8014CDF0;
  c.ps0[1]=63;c.ps0[2]=1.428571;c.ps0[3]=.1;c.ps0[4]=100;
  ssne_find_func(game_address(0x801BA874))->func(c); // mark a real gameplay projection
  c=base;c.r[3]=0x81000000;c.lr=mem_read32(0x80000000u)==0x47334250u?caller-0x48u:caller;
  c.ps0[1]=63;c.ps0[2]=1.428571;c.ps0[3]=.1;c.ps0[4]=100;c.ps0[5]=.5;c.ps0[6]=.5;
  ssne_find_func(function)->func(c);
  std::array<uint32_t,16> result{};
  for(int i=0;i<(function==game_address(0x801BA874)?16:12);++i)result[i]=mem_read32(0x81000000+i*4);
  return result;
 };
 for(auto function:{game_address(0x801BA874u),game_address(0x801BA5ECu)})
  for(auto caller:{0x8014C690u,0x8014C6B8u,0x8014C6D8u,0x8014C6FCu}){
   auto canonical=matrix(63,1,function,caller);check(u2f(canonical[0])>0.f,"Projection test has no SDK constants/SDA");
   for(auto fov:{45.f,90.f,110.f})for(auto wide:{1.f,4.f/3.f,7.f/4.f})
    check(matrix(fov,wide,function,caller)==canonical,"Local FOV/aspect changed shared guest projection");
  }
 // Native screen anchors must produce a nonzero full camera height.
 // Anchor 1 is upper-right; using it as lower-right silently erases 3D.
 auto coordinates=ssne_find_func(0x8011CD84u);check(coordinates,"PAL native viewport helper missing");
 for(unsigned aspect:{0u,1u,2u})for(unsigned corner:{0u,3u}) {
  mem_write32(0x81000010,0);mem_write32(0x81000014,0);
  PPCContext c=base;c.r[3]=0x81000000;c.r[4]=aspect;c.r[5]=corner;c.r[6]=0x81000010;
  coordinates->func(c);
  float expected=corner==3?1.f:0.f;
  check(u2f(mem_read32(0x81000000))==expected && u2f(mem_read32(0x81000004))==expected,
        "Native fullscreen camera coordinates differ from 0,0..1,1");
 }
 std::puts("Canonical netplay projection and native fullscreen viewport anchors passed");
}
