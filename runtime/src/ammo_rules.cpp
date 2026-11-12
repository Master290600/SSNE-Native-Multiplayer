#include "ssne/ammo_rules.h"
#include "ssne/ppc_context.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include <algorithm>
#include <array>
#include <atomic>
namespace ssne {
namespace {
std::atomic<unsigned> rules{0};
std::array<uint16_t,18> defaults{};
bool defaults_ready=false;
thread_local unsigned pickup_depth=0;
PPCFunc init_fn=nullptr,grant_fn=nullptr,pickup_fn=nullptr;
PPCFunc pack_fn=nullptr,all_fn=nullptr;
bool valid(uint32_t p){return p>=0x80000000u && p<=guest_ram_end()-0xDC;}
void remember_defaults(uint32_t inventory){
 for(unsigned i=0;i<18;++i)defaults[i]=std::min<unsigned>(mem_read16(inventory+0xB4+i*2),32767);
 defaults_ready=true;
}
void inventory_init(PPCContext& c){
 const auto inventory=c.r[3];ssne_call_original(c,0x8009F0A8,init_fn);
 if(!replica_simulation() && valid(inventory)){remember_defaults(inventory);apply_ammo_limits(inventory);}
}
void pickup_scope(PPCContext& c){
 struct Scope{Scope(){++pickup_depth;}~Scope(){--pickup_depth;}} scope;
 // Exactly one grant per native collector call. Health, armor, ownership,
 // events and the per-player consumed flags retain the original execution.
 ssne_call_original(c,0x800C10F4,pickup_fn);
}
void ammo_grant(PPCContext& c){
 const auto option=ammo_rules();
 if(replica_simulation() || !valid(c.r[3]) || c.r[4]>=18 ||
    (option.capacity==0 && (option.pickup_multiplier==1 || !pickup_depth))){ssne_call_original(c,0x8009F3B8,grant_fn);return;}
 const auto inventory=c.r[3],index=c.r[4];apply_ammo_limits(inventory);
 const auto maximum=mem_read16(inventory+0xB4+index*2);
 const auto amount=unsigned(mem_read16(inventory+0x48+index*2))*(pickup_depth?option.pickup_multiplier:1);
 unsigned changed=0;
 for(unsigned bank:{0u,0x24u}){
  const auto address=inventory+bank+index*2,current=unsigned(mem_read16(address));
  const auto next=std::min<unsigned>(maximum,current+amount);
  if(next>current){mem_write16(address,uint16_t(next));changed=1;}
 }
 c.r[3]=changed;
}
template<unsigned Address,unsigned Offset> void ammo_pack(PPCContext& c){
 const auto option=ammo_rules();const auto fn=Address==0x8009F288?pack_fn:all_fn;
 if(replica_simulation() || !valid(c.r[3]) || (option.capacity==0 && (option.pickup_multiplier==1 || !pickup_depth))){ssne_call_original(c,Address,fn);return;}
 const auto inventory=c.r[3];apply_ammo_limits(inventory);
 struct Amounts {uint32_t inventory;std::array<uint16_t,18> saved;~Amounts(){for(unsigned i=0;i<18;++i)mem_write16(inventory+Offset+i*2,saved[i]);}} restore{inventory};
 for(unsigned i=0;i<18;++i){
  auto& original=restore.saved[i];original=mem_read16(inventory+Offset+i*2);
  const auto amount=std::min(32767u,unsigned(original)*(pickup_depth?option.pickup_multiplier:1));
  mem_write16(inventory+Offset+i*2,uint16_t(amount));
  // The original routine adds before it clamps. Keep the signed-u16 addition
  // representable even when this pickup reaches the native upper bound.
  for(unsigned bank:{0u,0x24u})if(mem_read16(inventory+bank+i*2)+amount>32767)mem_write16(inventory+bank+i*2,uint16_t(32767-amount));
 }
 ssne_call_original(c,Address,fn);
}
void hook(uint32_t address,uint32_t signature,PPCFunc callback,PPCFunc& original){
 auto entry=ssne_find_func(address);
 if(!entry || mem_read32(address)!=signature)fatal("Unsupported native ammunition routine");
 original=entry->func;ssne_install_hook(address,callback);
}
}
void set_ammo_rules(unsigned capacity,unsigned pickup_multiplier){rules.store((capacity<=5?capacity:0)|((pickup_multiplier>=1 && pickup_multiplier<=5?pickup_multiplier:1)<<3),std::memory_order_relaxed);}
AmmoRules ammo_rules(){const auto value=rules.load(std::memory_order_relaxed);return {value&7u,std::max(1u,value>>3)};}
void apply_ammo_limits(uint32_t inventory){
 if(replica_simulation() || !valid(inventory))return;
 if(!defaults_ready)remember_defaults(inventory);
 const auto option=ammo_rules();
 for(unsigned i=0;i<18;++i){
  const auto base=unsigned(defaults[i]);
  const auto maximum=base==0?0u:option.capacity==2?32767u:std::min(32767u,base*(option.capacity==1?2u:option.capacity>=3?option.capacity:1u));
  const auto address=inventory+0xB4+i*2;
  const bool changed=mem_read16(address)!=maximum;
  if(changed)mem_write16(address,uint16_t(maximum));
  if(changed || option.capacity)for(unsigned bank:{0u,0x24u})if(mem_read16(inventory+bank+i*2)>maximum)mem_write16(inventory+bank+i*2,uint16_t(maximum));
 }
}
void install_ammo_rule_hooks(){
 hook(0x8009F0A8,0x9421FFD8,inventory_init,init_fn);
 hook(0x8009F3B8,0x38000002,ammo_grant,grant_fn);
 hook(0x800C10F4,0x9421FFB8,pickup_scope,pickup_fn);
 hook(0x8009F288,0x39200000,ammo_pack<0x8009F288,0x6C>,pack_fn);
 hook(0x8009F338,0x39200000,ammo_pack<0x8009F338,0x90>,all_fn);
}
}
