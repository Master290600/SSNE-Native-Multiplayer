#include "ssne/editor_logic_use.h"
#include "gc/editor_button.h"
#include "ssne/native_party.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>
#include <string_view>

namespace ssne {
namespace {
constexpr uint32_t post_use = 0x800B3068, post_use_return = 0x800BF2D4;
constexpr uint32_t native_eligibility = 0x800B3F6C;
constexpr uint32_t native_ray = 0x80140EC8, eligibility_ray_return = 0x800B42DC;
constexpr uint32_t switch_action = 0x80083A7C;
constexpr uint32_t switch_table = 0x80208B60, model_table = 0x80206348;
constexpr uint32_t player_table = 0x802121E8, relay_table = 0x80209758;
PPCFunc original_post_use = nullptr, original_ray = nullptr;
std::atomic<bool> authority{true};
void trace(const std::string& message) {
  static const bool enabled=[] {
    const auto* hidden=std::getenv("SSNE_TEST_HIDDEN");
    const auto* requested=std::getenv("SSNE_EDITOR_USE_TRACE");
    return hidden && std::strcmp(hidden,"0") && requested && std::strcmp(requested,"0");
  }();
  static unsigned count=0;
  if(enabled && count++<512)log_message("[editor-use] "+message);
}
bool readable(uint32_t p, uint64_t n) {
  return g_ram && p >= 0x80000000u && n <= guest_ram_size() &&
         uint64_t(p) + n <= guest_ram_end();
}
bool text_prefix(uint32_t p, std::string_view prefix) {
  if (!readable(p, prefix.size() + 1)) return false;
  for (size_t i=0;i<prefix.size();++i)
    if (mem_read8(p+uint32_t(i)) != uint8_t(prefix[i])) return false;
  return true;
}
bool scalar(uint32_t p, float& out) {
  if (!readable(p,4)) return false;
  out=u2f(mem_read32(p));
  return std::isfinite(out) && std::abs(out)<=1000000.f;
}
struct Actor {uint32_t node=0,type=0,config=0,matrix=0;};
using Actors=std::map<uint32_t,Actor>;
bool source(uint32_t& resource, Actors& actors) {
  const auto owner=mem_read32(0x802DACC0);
  if (!readable(owner,0x29C)) return false;
  const auto manager=mem_read32(owner+0x298);
  if (!readable(manager,8)) return false;
  resource=mem_read32(manager+4);
  if (!readable(resource,0x80) || mem_read32(resource)!=1 ||
      mem_read32(resource+4)!=100) return false;
  std::set<uint32_t> seen;
  for(auto p=mem_read32(resource+0x40);p;p=mem_read32(p)) {
    if(!readable(p,44) || !seen.insert(p).second || seen.size()>32768)
      return false;
    // Original non-spatial descriptors are not registered logic actors and
    // may reuse placeholder IDs. Match Document's authored-actor contract.
    if(!mem_read32(p+4))continue;
    const auto id=mem_read32(p+20);
    if(!actors.emplace(id,Actor{p,mem_read32(p+12),mem_read32(p+16),
                               mem_read32(p+4)}).second) return false;
  }
  return true;
}
uint32_t live(uint32_t id,uint32_t vt,uint32_t config,unsigned size) {
  if(id>32767) return 0;
  const auto table=mem_read32(0x802DB7C4),count=mem_read32(0x802DB7C8),
             capacity=mem_read32(0x802DB7CC);
  if(!count || count>capacity || count>32768 || !readable(table,uint64_t(count)*8))
    return 0;
  // The original script registry is sorted by signed16 authored ID.
  uint32_t lo=0,hi=count;
  while(lo<hi){const auto mid=lo+(hi-lo)/2;
    if(int16_t(mem_read16(table+mid*8))<int(id))lo=mid+1;else hi=mid;}
  if(lo==count || mem_read16(table+lo*8)!=id) return 0;
  const auto p=mem_read32(table+lo*8+4);
  if(!readable(p,size) || mem_read32(p+0x48)!=vt ||
     mem_read32(p+0x50)!=id || mem_read32(p+0x54)!=config) return 0;
  return p;
}
struct Pair {uint32_t button=0,visual=0,scene_index=0;std::array<float,3> point{};};
bool pair(uint32_t resource,const Actors& actors,uint32_t id,const Actor& a,Pair& out) {
  const auto c=a.config;
  if(a.type!=4180 || !readable(c,gc::editor::button_config_bytes) ||
     mem_read32(c+gc::editor::button_magic_offset)!=gc::editor::button_magic ||
     mem_read32(c+gc::editor::button_version_offset)!=gc::editor::button_version ||
     !readable(a.matrix,48)) return false;
  const auto vi=actors.find(mem_read32(c+12)),ri=actors.find(mem_read32(c+4));
  if(vi==actors.end() || ri==actors.end() || vi->second.type!=4200 ||
     vi->second.matrix!=a.matrix || ri->second.type!=4080 ||
     !text_prefix(mem_read32(ri->second.node+8),"__SSNE_LOGIC_HELPER_ROUTER_") ||
     !readable(vi->second.config,16)) return false;
  out.button=live(id,switch_table,c,0x68);
  out.visual=live(vi->first,model_table,vi->second.config,0x9C);
  const auto router=live(ri->first,relay_table,ri->second.config,0xA8);
  if(!out.button || !out.visual || !router ||
     mem_read32(out.button+0x58)!=router ||
     mem_read32(out.button+0x60)!=1 || mem_read32(out.button+0x64)!=1 ||
     mem_read32(out.visual+0x84)!=1) return false;
  out.scene_index=mem_read32(vi->second.config);
  std::set<uint32_t> visited;
  auto model=mem_read32(resource+0x58);
  for(uint32_t i=0;i<out.scene_index && model;++i){
    if(!readable(model,48) || !visited.insert(model).second || i>=32768)return false;
    model=mem_read32(model);
  }
  if(!readable(model,48) || mem_read32(model+4)!=a.matrix) return false;
  const auto frame=mem_read32(model+44);
  if(!readable(frame,20) ||
     !text_prefix(mem_read32(frame+4),"models/ssne/editor_button_"))return false;
  const auto bounds=mem_read32(frame+16);
  if(!readable(bounds,20))return false;
  for(unsigned k=0;k<3;++k)if(!scalar(bounds+4+k*4,out.point[k]))return false;
  return true;
}
// Observe the exact original eligibility query without changing its arguments,
// hit record or return. Original800B3F6C supplies Eye, range and look-cone gates.
struct Probe {uint32_t index=0;bool matched=false;};
thread_local Probe* probe=nullptr;
void ray_hook(PPCContext& c) {
  auto* active=probe;
  const bool observe=active && c.lr==eligibility_ray_return;
  const auto result=c.r[6];
  ssne_call_original(c,native_ray,original_ray);
  if(observe && c.r[3] && readable(result,48)) {
    // PAL80137488/80137AD8 tags source root58 model node N as 0x04000000|N.
    // 80140EC8 returns its high byte and low24 index separately.
    active->matched=mem_read32(result+0x24)==4 &&
                    mem_read32(result+0x28)==active->index;
    trace("ray kind="+std::to_string(mem_read32(result+0x24))+
          " index="+std::to_string(mem_read32(result+0x28))+
          " expected="+std::to_string(active->index)+
          " matched="+std::to_string(active->matched));
  }
}
struct Scratch {
  uint32_t base;std::array<uint8_t,0x4000> saved;PPCContext call;
  explicit Scratch(const PPCContext& c):base(c.r[1]-0x4000),call(c) {
    std::memcpy(saved.data(),host_ptr(base),saved.size());
    call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],c.r[1]);
  }
  ~Scratch(){std::memcpy(host_ptr(base),saved.data(),saved.size());}
};
bool eligible(PPCContext& c,uint32_t player,const Pair& p) {
  float radius=0;
  float value=0;
  const auto engine=mem_read32(0x802ABAA4),look=mem_read32(player+0x27C);
  if(!scalar(p.button+0x5C,radius) || radius<=0 || radius>1000 ||
     !readable(engine,4) || mem_read32(engine)!=0x8021EB70 ||
     !readable(look,0x28) || !scalar(look+0x24,value) ||
     !scalar(player+0x290,value) || !scalar(player+0x294,value))return false;
  Scratch temp(c);
  const auto point=c.r[1]-0x80;
  for(unsigned k=0;k<3;++k)mem_write32(point+k*4,f2u(p.point[k]));
  Probe current{p.scene_index};
  struct Scope {Probe* old;Scope(Probe* p):old(probe){probe=p;}~Scope(){probe=old;}} scope(&current);
  temp.call.r[3]=player;temp.call.r[4]=p.visual;temp.call.r[5]=point;
  temp.call.ps0[1]=temp.call.ps1[1]=radius;
  ssne_call(temp.call,native_eligibility);
  return current.matched;
}
void use(PPCContext& c) {
  const auto player=c.r[3];
  if(c.lr!=post_use_return || !authority.load(std::memory_order_relaxed) ||
     !g_ram || replica_simulation() ||
     mem_read32(0x80000000)!=0x47334250 || c.r[13]!=0x802E22A0 ||
     c.r[1]<0x80008000 || !readable(c.r[1]-0x4000,0x4000) ||
     mem_read32(0x802DB7E8)!=4 || !readable(player,0x790) ||
     mem_read32(player+0x48)!=player_table ||
     mem_read32(player+0x6BC) || mem_read32(player+0x6C4) ||
     mem_read32(player+0x370) || mem_read32(player+0x328) ||
     !native_party_actor_occupied(player)) return;
  float health=0;
  if(!scalar(player+0xCC,health) || health<=0)return;
  const auto hud=mem_read32(player+0x374),bank=mem_read32(0x802ABA9C),
             slot=mem_read32(player+0x178);
  if(!readable(hud,0x3E4) || mem_read32(hud+0x3E0) ||
     slot>=native_party_capacity()) return;
  const auto offset=native_party_controller_offset(slot);
  if(uint64_t(bank)+offset>0xffffffffu || !readable(bank+offset,0x22C) ||
     mem_read32(bank+offset+0x224)!=1) return;
  trace("pressed slot="+std::to_string(slot));
  uint32_t resource=0;Actors actors;
  if(!source(resource,actors)){trace("level source unavailable");return;}
  for(const auto& [id,a]:actors) {
    Pair p;
    if(!pair(resource,actors,id,a,p))continue;
    trace("validated pair id="+std::to_string(id));
    if(!eligible(c,player,p)){trace("native eligibility rejected id="+std::to_string(id));continue;}
    // The same original Switch action called by MovingBrush Use. The native
    // real player's identity is the activator; scripts execute only on authority.
    Scratch temp(c);temp.call.r[3]=p.button;temp.call.r[4]=player;
    ssne_call(temp.call,switch_action);
    trace("activated id="+std::to_string(id)+" slot="+std::to_string(slot));
    return; // One rising edge performs at most one authored interaction.
  }
}
void post_use_hook(PPCContext& c) {
  use(c);
  ssne_call_original(c,post_use,original_post_use);
}
}
bool install_editor_logic_use(std::string& error) {
  if(original_post_use){error.clear();return true;}
  constexpr std::pair<uint32_t,uint32_t> signatures[]={
    {post_use,0x9421FFE0},{native_ray,0x9421FE60},
    {native_eligibility,0x9421FE60},{switch_action,0x9421FFD8}};
  if(!g_ram || mem_read32(0x80000000)!=0x47334250 ||
     mem_read32(switch_table+0x44)!=0x80083944 ||
     mem_read32(model_table+0x4C)!=0x8007D948 ||
     mem_read32(player_table+0x280)!=0 ||
     mem_read32(player_table+0x284)!=0x8008B474 ||
     mem_read32(0x8021EB70+0x5C)!=native_ray ||
     mem_read32(post_use_return-4)!=0x4BFF3D99) {
    error="Editor interaction requires the verified PAL native classes";return false;
  }
  for(auto [address,word]:signatures)
    if(!ssne_find_func(address) || mem_read32(address)!=word){
      error="Editor interaction native function signature is incompatible";return false;}
  auto chain=[](uint32_t address,PPCFunc hook,PPCFunc& previous){
    previous=*ssne_hook_slot(address);
    if(!previous)previous=ssne_find_func(address)->func;
    ssne_install_hook(address,hook);
  };
  chain(native_ray,ray_hook,original_ray);
  chain(post_use,post_use_hook,original_post_use);
  error.clear();return true;
}
void set_editor_logic_use_authoritative(bool enabled) {
  authority.store(enabled,std::memory_order_relaxed);
}
}
