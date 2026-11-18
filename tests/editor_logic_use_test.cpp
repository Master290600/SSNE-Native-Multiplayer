// Contract coverage of the production adapter; native geometry is a strict
// mock here. Only real keyboard/pad Use in the private game QA proves playability.
#include "ssne/editor_logic_use.h"
#include "gc/editor_button.h"
#include "ssne/native_party.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
namespace {
unsigned checks=0,actions=0,post_calls=0,ray_calls=0;
bool replica=false,occupied=true,geometry_eligible=true,ray_hit=true;
uint32_t hit_kind=4,hit_index=0,last_activator=0,ray_caller=0x800B42DC;
constexpr uint32_t player=0x80400000,hud=0x80410000,bank=0x80900000;
constexpr uint32_t resource=0x80500000,button=0x80600000,visual=0x80601000,router=0x80602000;
constexpr uint32_t bc=0x80501000,vc=0x80501100,rc=0x80501200,matrix=0x80501300;
constexpr uint32_t bn=0x80502000,vn=0x80502100,rn=0x80502200;
constexpr uint32_t model=0x80503000,frame=0x80503100,bounds=0x80503200;
std::map<uint32_t,PPCFunc> hooks;
void check(bool yes,const char*why){++checks;if(!yes)throw std::runtime_error(why);}
void str(uint32_t p,const char*s){std::memcpy(ssne::host_ptr(p),s,std::strlen(s)+1);}
PPCContext context(){PPCContext c;c.r[1]=0x817E0000;c.r[13]=0x802E22A0;c.r[3]=player;c.lr=0x800BF2D4;c.ps0[1]=7.5;return c;}
void post(PPCContext&){++post_calls;}
void ray(PPCContext&c){++ray_calls;mem_write32(c.r[6]+0x24,hit_kind);mem_write32(c.r[6]+0x28,hit_index);c.r[3]=ray_hit;}
void eligibility(PPCContext&c){
  check(c.r[3]==player && c.r[4]==visual,"Eligibility used a wrong native player/model");
  check(c.ps0[1]==3,"Native Switch radius was replaced");
  if(geometry_eligible){c.r[6]=c.r[1]-0x100;c.lr=ray_caller;ssne_call(c,0x80140EC8);}
  c.r[3]=0; // Retail function correctly rejects non-Brush hit after the query.
  mem_write32(c.r[1]-16,0xdeadbeef);
}
void action(PPCContext&c){
  check(c.r[3]==button,"Wrong native Switch action receiver");
  ++actions;last_activator=c.r[4];
  if(mem_read32(bc+8)==0)mem_write32(button+0x64,0);
  c.r[3]=999;mem_write32(c.r[1]-16,0xfadebabe);
}
const std::array<SSNEFuncEntry,4> entries={{{0x800B3068,post,"postUse"},
 {0x80140EC8,ray,"ray"},{0x800B3F6C,eligibility,"eligibility"},
 {0x80083A7C,action,"SwitchAction"}}};
void base(){
  ssne::set_editor_logic_use_authoritative(true);
  replica=false;occupied=true;geometry_eligible=true;ray_hit=true;hit_kind=4;hit_index=0;ray_caller=0x800B42DC;
  mem_write32(0x80000000,0x47334250);mem_write32(0x802DB7E8,4);
  mem_write32(0x802ABA9C,bank);
  mem_write32(player+0x48,0x802121E8);mem_write32(player+0x374,hud);
  mem_write32(player+0xCC,f2u(100));mem_write32(player+0x178,0);
  mem_write32(0x802ABAA4,0x80800000);mem_write32(0x80800000,0x8021EB70);
  mem_write32(player+0x27C,0x80420000);mem_write32(0x80420024,f2u(.8f));
  for(auto offset:{0x6BC,0x6C4,0x370,0x328})mem_write32(player+offset,0);
  mem_write32(hud+0x3E0,0);mem_write32(bank+0x224,1);
  mem_write32(button+0x60,1);mem_write32(button+0x64,1);mem_write32(bc+8,1);
  mem_write32(visual+0x84,1);
}
void setup(){
  base();
  mem_write32(0x80208B60+0x44,0x80083944);mem_write32(0x80206348+0x4C,0x8007D948);
  mem_write32(0x802121E8+0x284,0x8008B474);
  mem_write32(0x8021EB70+0x5C,0x80140EC8);mem_write32(0x800BF2D0,0x4BFF3D99);
  for(auto [address,word]:std::array<std::pair<uint32_t,uint32_t>,4>{{
      {0x800B3068,0x9421FFE0},{0x80140EC8,0x9421FE60},
      {0x800B3F6C,0x9421FE60},{0x80083A7C,0x9421FFD8}}})mem_write32(address,word);
  mem_write32(0x802DACC0,0x80508000);mem_write32(0x80508000+0x298,0x80508400);
  mem_write32(0x80508404,resource);mem_write32(resource,1);mem_write32(resource+4,100);
  mem_write32(resource+0x40,bn);mem_write32(resource+0x58,model);
  for(auto [node,next,type,id,config]:std::array<std::array<uint32_t,5>,3>{{
      {bn,vn,4180,100,bc},{vn,rn,4200,101,vc},{rn,0,4080,102,rc}}}){
    mem_write32(node,next);mem_write32(node+4,matrix);mem_write32(node+12,type);
    mem_write32(node+16,config);mem_write32(node+20,id);
  }
  mem_write32(rn+8,0x80509000);str(0x80509000,"__SSNE_LOGIC_HELPER_ROUTER_102");
  mem_write32(bc+4,102);mem_write32(bc+12,101);
  mem_write32(bc+32,gc::editor::button_magic);mem_write32(bc+36,gc::editor::button_version);
  mem_write32(0x802DB7C4,0x8050A000);mem_write32(0x802DB7C8,3);mem_write32(0x802DB7CC,3);
  for(auto [p,id,vt,config]:std::array<std::array<uint32_t,4>,3>{{
    {button,100,0x80208B60,bc},{visual,101,0x80206348,vc},{router,102,0x80209758,rc}}}){
    const auto k=id-100;mem_write16(0x8050A000+k*8,uint16_t(id));mem_write32(0x8050A004+k*8,p);
    mem_write32(p+0x48,vt);mem_write32(p+0x50,id);mem_write32(p+0x54,config);
  }
  mem_write32(button+0x58,router);mem_write32(button+0x5C,f2u(3));
  mem_write32(model+4,matrix);mem_write32(model+44,frame);
  mem_write32(frame+4,0x80509100);str(0x80509100,"models/ssne/editor_button_abcd_123.ssm");
  mem_write32(frame+16,bounds);mem_write32(bounds+4,f2u(1));mem_write32(bounds+8,f2u(2));mem_write32(bounds+12,f2u(3));
}
template<class F>void rejects(F f,const char*why){base();f();const auto before=actions;auto c=context();ssne_call(c,0x800B3068);check(actions==before,why);}
}
namespace ssne {
uint8_t*g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return 0x1800000;}uint32_t guest_ram_end(){return 0x81800000;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO");}
void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO");}
bool replica_simulation(){return replica;}
void log_message(const std::string&){}
bool native_party_actor_occupied(uint32_t p){return p==player && occupied;}
unsigned native_party_capacity(){return 8;}
uint32_t native_party_controller_offset(uint32_t slot){return slot*0x70C;}
}
const SSNEFuncEntry*ssne_find_func(uint32_t address){for(const auto&e:entries)if(e.address==address)return&e;return nullptr;}
PPCFunc*ssne_hook_slot(uint32_t address){return &hooks[address];}
void ssne_install_hook(uint32_t address,PPCFunc f){hooks[address]=f;}
void ssne_call(PPCContext&c,uint32_t address){
  const auto*e=ssne_find_func(address);if(!e)throw std::runtime_error("Unexpected native call");
  if(!SSNEOriginalCall::consume(c,address)&&hooks[address])hooks[address](c);else e->func(c);
}
int main(){try{
  std::string error;check(!ssne::install_editor_logic_use(error),"Installed without RAM");
  auto ram=std::make_unique<uint8_t[]>(ssne::guest_ram_size());ssne::g_ram=ram.get();setup();
  check(ssne::install_editor_logic_use(error),error.c_str());
  auto c=context();const auto before=c;std::array<uint8_t,0x4000> stack{};
  std::memcpy(stack.data(),ssne::host_ptr(c.r[1]-0x4000),stack.size());
  ssne_call(c,0x800B3068);
  check(actions==1&&last_activator==player,"Real Use did not preserve native player activator");
  check(std::memcmp(&c,&before,sizeof(c))==0,"Adapter changed caller CPU");
  check(std::memcmp(stack.data(),ssne::host_ptr(c.r[1]-0x4000),stack.size())==0,"Adapter damaged borrowed stack");
  rejects([]{replica=true;},"Client committed a Switch action");
  rejects([]{ssne::set_editor_logic_use_authoritative(false);},
          "Client bootstrap committed before replica simulation was queued");
  rejects([]{occupied=false;},"Unoccupied player activated a button");
  rejects([]{mem_write32(player+0x6BC,0x1234);},"Original Use target lost priority");
  rejects([]{mem_write32(player+0x6C4,1);},"Vehicle player activated a button");
  rejects([]{mem_write32(player+0x370,1);},"Native modal player activated a button");
  rejects([]{mem_write32(player+0x328,1);},"Invalid native player lifecycle activated a button");
  rejects([]{mem_write32(player+0xCC,f2u(0));},"Dead player activated a button");
  rejects([]{mem_write32(hud+0x3E0,1);},"HUD blocked Use activated a button");
  rejects([]{mem_write32(bank+0x224,0);mem_write32(bank+0x228,1);},"Held Use repeated without rising edge");
  rejects([]{geometry_eligible=false;},"Range/cone failure activated a button");
  rejects([]{ray_hit=false;},"Failed ray query activated a button");
  rejects([]{hit_kind=2;},"Unrelated native brush activated a button");
  rejects([]{hit_kind=3;},"Wrong scene collider class activated a button");
  rejects([]{hit_index=1;},"Another model activated a button");
  rejects([]{ray_caller=0x1234;},"Unrelated native ray activated a button");
  rejects([]{mem_write32(button+0x60,0);},"Disabled Switch activated");
  rejects([]{mem_write32(button+0x64,0);},"Consumed one-shot Switch activated");
  rejects([]{mem_write32(visual+0x84,0);},"Hidden native button activated");
  mem_write32(bc+32,0);rejects([]{},"Original Switch was mistaken for editor button");mem_write32(bc+32,gc::editor::button_magic);
  mem_write32(bc+36,2);rejects([]{},"Unknown pair schema activated");mem_write32(bc+36,gc::editor::button_version);
  mem_write32(vn+4,matrix+48);rejects([]{},"Unpaired model transform activated");mem_write32(vn+4,matrix);
  mem_write32(button+0x58,0);rejects([]{},"Unresolved router activated");mem_write32(button+0x58,router);
  mem_write32(visual+0x54,vc+16);rejects([]{},"Stale live actor activated");mem_write32(visual+0x54,vc);
  mem_write32(resource+0x40,0x80502300);mem_write32(0x80502300,bn);mem_write32(0x80502300+20,100);
  base();const auto nonspatial=actions;ssne_call(c,0x800B3068);
  check(actions==nonspatial+1,"Non-spatial descriptor placeholder ID blocked authored actors");
  mem_write32(resource+0x40,bn);
  mem_write32(rn,bn);rejects([]{},"Cyclic source actor chain activated");mem_write32(rn,0);
  mem_write32(0x80508404,0);rejects([]{},"Retired level source activated");mem_write32(0x80508404,resource);
  base();mem_write32(bc+8,0);const auto one=actions;ssne_call(c,0x800B3068);ssne_call(c,0x800B3068);
  check(actions==one+1,"Native one-shot semantics were bypassed");
  base();mem_write32(player+0x178,5);mem_write32(bank+0x224,0);mem_write32(bank+5*0x70C+0x224,1);
  const auto extra=actions;ssne_call(c,0x800B3068);check(actions==extra+1,"Remote/extra player's controller bank was not used");
  base();const auto wrong=actions;c.lr=0x12345678;ssne_call(c,0x800B3068);check(actions==wrong,"Unrelated native caller activated a button");
  check(post_calls>=25,"Original post-Use function was skipped");
  std::cout<<"PASS editor_logic_use checks="<<checks<<" actions="<<actions<<" rays="<<ray_calls<<"\n";
  return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<"\n";return 1;}}
