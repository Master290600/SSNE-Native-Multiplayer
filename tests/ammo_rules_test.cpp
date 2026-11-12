#include "ssne/ammo_rules.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <array>
#include <map>
#include <vector>
#include <stdexcept>
#include <iostream>
namespace {bool replica=false;std::map<unsigned,PPCFunc> hooks;unsigned calls=0;constexpr unsigned inv=0x80300000;}
namespace ssne {
uint8_t* g_ram;uint8_t g_locked_cache[kLockedCacheSize];
uint32_t guest_ram_size(){return kRamSize;}uint32_t guest_ram_end(){return 0x80000000u+kRamSize;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO");}void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO");}
bool replica_simulation(){return replica;}[[noreturn]] void fatal(const char* m){throw std::runtime_error(m);}
}
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
void init(PPCContext& c){for(unsigned i=0;i<18;++i){mem_write16(c.r[3]+0xb4+i*2,i?100:0);mem_write16(c.r[3]+i*2,0);mem_write16(c.r[3]+0x24+i*2,0);mem_write16(c.r[3]+0x48+i*2,20);mem_write16(c.r[3]+0x6c+i*2,30);mem_write16(c.r[3]+0x90+i*2,40);}}
void grant(PPCContext& c){++calls;for(unsigned bank:{0u,0x24u}){auto a=c.r[3]+bank+c.r[4]*2;mem_write16(a,std::min<unsigned>(mem_read16(c.r[3]+0xb4+c.r[4]*2),mem_read16(a)+mem_read16(c.r[3]+0x48+c.r[4]*2)));}}
template<unsigned Offset>void pack(PPCContext& c){++calls;for(unsigned i=0;i<18;++i)for(unsigned bank:{0u,0x24u}){auto a=c.r[3]+bank+i*2;const auto sum=int16_t(mem_read16(a)+mem_read16(c.r[3]+Offset+i*2));mem_write16(a,uint16_t(std::min<int>(mem_read16(c.r[3]+0xb4+i*2),sum)));}}
void pickup(PPCContext& c){const auto action=c.r[5];hooks.at(action?action==1?0x8009f288:0x8009f338:0x8009f3b8)(c);}
const SSNEFuncEntry entries[]={{0x8009f0a8,init,"init"},{0x8009f3b8,grant,"grant"},{0x800c10f4,pickup,"pickup"},{0x8009f288,pack<0x6c>,"pack"},{0x8009f338,pack<0x90>,"all"}};
const SSNEFuncEntry* ssne_find_func(uint32_t p){for(auto& e:entries)if(e.address==p)return &e;return nullptr;}
void ssne_install_hook(uint32_t p,PPCFunc f){hooks[p]=f;}
int main(){try{
 std::vector<uint8_t> ram(ssne::kRamSize);ssne::g_ram=ram.data();
 for(auto [p,signature]:{std::pair{0x8009f0a8u,0x9421ffd8u},std::pair{0x8009f3b8u,0x38000002u},std::pair{0x800c10f4u,0x9421ffb8u},std::pair{0x8009f288u,0x39200000u},std::pair{0x8009f338u,0x39200000u}})mem_write32(p,signature);
 ssne::install_ammo_rule_hooks();
 for(unsigned cap=0;cap<=5;++cap)for(unsigned mult=1;mult<=5;++mult)for(unsigned method=0;method<3;++method){
  ssne::set_ammo_rules(cap,mult);const auto selected=ssne::ammo_rules();check(selected.capacity==cap && selected.pickup_multiplier==mult,"Rule packing failed");
  PPCContext c{};c.r[3]=inv;c.r[4]=1;c.r[5]=method;hooks.at(0x8009f0a8)(c);
  const auto maximum=cap==2?32767u:100*(cap==1?2:cap>=3?cap:1);
  check(mem_read16(inv+0xb4)==0 && mem_read16(inv+0xb6)==maximum,"Native base capacities not respected");
  const auto amount=(20+method*10)*mult;hooks.at(0x800c10f4)(c);
  for(unsigned bank:{0u,0x24u})check(mem_read16(inv+bank+2)==std::min(amount,maximum),"Pickup factor or bank failed");
  check(mem_read16(inv+0x6e)==30 && mem_read16(inv+0x92)==40,"Pickup template was permanently multiplied");
  mem_write16(inv+2,maximum-1);mem_write16(inv+0x26,maximum-1);c.r[3]=inv;hooks.at(0x800c10f4)(c);
  check(mem_read16(inv+2)==maximum && mem_read16(inv+0x26)==maximum,"Grant overflow or saturation failed");
  // A script grant outside collector scope must not receive the pickup factor.
  c.r[3]=inv;c.r[4]=1;mem_write16(inv+2,0);mem_write16(inv+0x26,0);hooks.at(0x8009f3b8)(c);check(mem_read16(inv+2)==20,"Script grant multiplied");
  replica=true;const auto before=calls;c.r[3]=inv;hooks.at(0x8009f3b8)(c);check(calls==before+1,"Replica grant interception changed");replica=false;
 }
 ssne::set_ammo_rules(99,99);check(ssne::ammo_rules().capacity==0 && ssne::ammo_rules().pickup_multiplier==1,"Invalid local rules not normalized");
 std::cout<<"All ammo factors, native capacities, two banks, collector scoping, template restoration, signed saturation and replica bypass passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
