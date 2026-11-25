// CPU contract tests use the production port validator AND the production
// native_effect presentation wrappers. Native calls are strict fake callbacks;
// actual PAL rendering/replication additionally requires the private game QA.
#include "native_particles.h"
#include "native_scalar_projection.h"
#include "native_world.h"
#include "ssne/native_effect.h"
#include "ssne/mods.h"
#include "ssne/native_entity.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace port::coop;
namespace {
unsigned checks=0,calls=0,starts=0,stops=0,ticks=0,sparkle_ticks=0;
bool replica=true;
std::vector<SSNEFuncEntry> entries;
std::map<int64_t,uint32_t> references;
void check(bool v,const char*why){++checks;if(!v)throw std::runtime_error(why);}
void unused(PPCContext&){}
void signature(uint32_t address,uint32_t value){
  mem_write32(address,value);
  if(!ssne_find_func(address))entries.push_back({address,unused,"fake verified PAL"});
}
PPCContext context(){
  PPCContext c;c.r[13]=0x802E22A0;c.r[1]=0x817E0000;
  c.r[3]=123;c.ps0[1]=7.25;return c;
}
std::pair<uint32_t,EntityState> make(unsigned kind){
  const auto*t=native_particle_class(kind);
  const uint32_t object=0x80400000+kind*0x100,config=0x80500000+kind*0x40,
                 emitter=0x80600000+kind*0x400;
  mem_write32(object+0x48,native_particles_vtable);
  mem_write32(object+0x4C,1);mem_write32(object+0x50,100+kind);
  mem_write32(object+0x54,config);mem_write32(object+0x58,0);
  mem_write32(object+0x60,emitter);mem_write16(config+2,uint16_t(kind));
  mem_write32(emitter+0x1C,t->vtable);
  mem_write32(t->vtable+0x34,t->update);mem_write32(t->vtable+0x74,t->enable);
  signature(t->update,t->update_prologue);signature(t->enable,t->enable_prologue);
  references[100+kind]=object;
  EntityState state;state.id=kind+1;state.kind=EntityKind::Mission;
  state.definition=native_particles_definition;
  state.properties["script_index"]=int64_t(100+kind);
  state.properties["script_enabled"]=int64_t(1);
  state.properties["position"]=std::array<float,3>{};
  std::string error;
  check(capture_native_particles(object,state,error),"Particle capture failed");
  return {object,state};
}
} // namespace
namespace ssne {
uint8_t*g_ram=nullptr;
uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return 0x01800000;}
uint32_t guest_ram_end(){return 0x81800000;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO");}
void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO");}
bool replica_simulation(){return replica;}
void log_message(const std::string&){}
uint64_t observe_native_entities(NativeEntityObserver){
  throw std::runtime_error("Unexpected entity-observer installation");
}
unsigned native_party_capacity(){throw std::runtime_error("Unexpected party query");}
uint32_t native_party_registry(){throw std::runtime_error("Unexpected party registry query");}
std::optional<std::array<float,12>> native_weapon_camera_reference(uint32_t){
  throw std::runtime_error("Unexpected weapon camera query");
}
[[noreturn]] void fatal(const char*message){throw std::runtime_error(message);}
int netplay_local_player(){throw std::runtime_error("Unexpected local-player query");}
bool mod_is_registered_enemy(uint32_t){throw std::runtime_error("Unexpected enemy registry query");}
bool netplay_world_ready(){throw std::runtime_error("Unexpected world-ready query");}
void audio_track_weapon_voice(uint32_t,uint32_t,uint32_t,uint64_t){
  throw std::runtime_error("Unexpected weapon audio ownership");
}
}
namespace port::coop {
std::optional<uint32_t> native_script_reference_address(int64_t ref){
  auto it=references.find(ref);return it==references.end()?std::nullopt:std::optional<uint32_t>(it->second);
}
}
const SSNEFuncEntry*ssne_find_func(uint32_t address){
  for(const auto&e:entries)if(e.address==address)return&e;return nullptr;
}
void ssne_install_hook(uint32_t,PPCFunc){
  throw std::runtime_error("Unexpected native hook installation");
}
void ssne_call(PPCContext&c,uint32_t address){
  ++calls;
  // Deliberately write the borrowed stack. The wrappers must restore it.
  mem_write32(c.r[1]-32,0xDEADBEEF);
  if(address==0x80081578 || address==0x800815D4){
    const bool active=address==0x80081578;
    active?++starts:++stops;
    mem_write32(c.r[3]+0x58,active);
    mem_write32(mem_read32(c.r[3]+0x60)+0x20,active);
    return;
  }
  if(address==0x800D20E8){
    ++sparkle_ticks;
    check(mem_read32(c.r[3]+0x2D4)==1 && u2f(mem_read32(c.r[3]+0x20))==11.f,
          "Sparkle regression did not update a disabled emitter older than ten seconds");
    check(c.ps0[1]==c.ps1[1]&&c.ps0[1]==double(.02f),"Wrong ambient sparkle dt");
    return;
  }
  for(unsigned kind=1;kind<=5;++kind)
    if(address==native_particle_class(kind)->update){
      ++ticks;
      check(c.ps0[1]==c.ps1[1]&&c.ps0[1]>=0&&c.ps0[1]<=.1,"Wrong native particle dt");
      return;
    }
  throw std::runtime_error("Unexpected native call, mission dispatch or damage");
}
static void disabled_sparkle_clock(PPCContext& c,uint32_t emitter){
  constexpr uint32_t manager=0x80700000,node=0x80700100,mesh=0x80700200,vertices=0x80700300;
  mem_write32(0x802ABAB8,manager);mem_write32(manager,node);mem_write32(manager+4,1);
  mem_write32(node,emitter);mem_write32(node+4,0);mem_write32(node+8,0);
  mem_write32(emitter+0x2C,mesh);mem_write32(emitter+0x2D4,1);
  mem_write32(mesh,0x8021DEC8);mem_write32(mesh+0x3C,1);mem_write32(mesh+0x60,vertices);
  signature(0x800CE920,0x9421FF08);
  signature(0x801BA380,0);signature(0x801BA334,0);
  for(auto [address,value]:{std::pair{0x80214EF8u,0x3F4CCCCDu},{0x80214F0Cu,0x3F19999Au},
                          {0x80214EFCu,0x3F800000u},{0x80214F00u,0x3F000000u},
                          {0x80214F04u,0x3CF5C28Fu},{0x80214F08u,0u}})
    mem_write32(address,value);
  std::string error;
  mem_write32(emitter+0x20,f2u(11.f));
  const auto before=calls;
  check(ssne::update_native_pickup_ambient(c,.02f,error),"Legal disabled sparkle clock above ten seconds rejected");
  check(calls==before+1&&sparkle_ticks==1,"Ambient update did not tick exactly one disabled sparkle");
  for(float invalid:{std::numeric_limits<float>::quiet_NaN(),-1.f}){
    mem_write32(emitter+0x20,f2u(invalid));
    check(!ssne::update_native_pickup_ambient(c,.02f,error)&&calls==before+1,
          "Invalid sparkle clock reached a partial native update");
  }
  mem_write32(emitter+0x20,f2u(11.f));mem_write32(mesh+0x60,0);
  check(!ssne::update_native_pickup_ambient(c,.02f,error)&&calls==before+1,
        "Invalid sparkle mesh reached a partial native update");
  mem_write32(0x802ABAB8,0);
}
int main(){
  try{
    auto ram=std::make_unique<uint8_t[]>(ssne::kRamSize);ssne::g_ram=ram.get();
    mem_write32(0x80000000,0x47334250);
    mem_write32(native_particles_vtable+0x44,0x800814C4);
    mem_write32(native_particles_vtable+0xAC,0x80081578);
    mem_write32(native_particles_vtable+0xB4,0x800815D4);
    mem_write32(native_particles_vtable+0x54,0x8008147C);
    signature(0x8008147C,0x9421FFF0);
    signature(0x80081578,0x9421FFF0);signature(0x800815D4,0x9421FFF0);
    auto c=context();std::string error;
    std::vector<uint32_t> objects;
    for(unsigned kind=0;kind<7;++kind){
      auto[object,state]=make(kind);objects.push_back(object);
      check(validate_native_particles(object,state,false,error),"Captured state invalid");
      const auto before=calls;
      check(apply_native_particles(c,object,state,error)&&calls==before,
            "Unchanged sample restarted native particles");
      state.properties["particles_enabled"]=int64_t(1);
      check(!validate_native_particles(object,state,false,error),
            "Scalar-only change bypassed native lifecycle");
      check(validate_native_particles(object,state,true,error),"Lifecycle preflight rejected change");
      const auto cpu=c;
      std::array<uint8_t,0x4000> stack{};
      std::memcpy(stack.data(),ssne::g_ram+c.r[1]-0x4000-0x80000000,stack.size());
      check(apply_native_particles(c,object,state,error),"Native Enable failed");
      check(!std::memcmp(&cpu,&c,sizeof(c))&&
            !std::memcmp(stack.data(),ssne::g_ram+c.r[1]-0x4000-0x80000000,stack.size()),
            "Particle activation changed caller CPU/borrowed stack");
      check(apply_native_particles(c,object,state,error),"Repeated enabled sample failed");
      check(calls==before+1,"Repeated sample restarted emission/sound");
      state.properties["particles_enabled"]=int64_t(0);
      check(apply_native_particles(c,object,state,error),"Native Disable failed");
      auto invalid=state;invalid.properties["particles_enabled"]=int64_t(2);
      check(!validate_native_particles(object,invalid,true,error),"Nonboolean emission accepted");
      invalid=state;invalid.properties["particles_kind"]=int64_t((kind+1)%7);
      check(!validate_native_particles(object,invalid,true,error),"Mismatched emitter kind accepted");
      invalid=state;invalid.properties["script_index"]=int64_t(9999);
      check(!validate_native_particles(object,invalid,true,error),"Foreign level identity accepted");
      invalid=state;invalid.properties.erase("particles_enabled");
      check(!validate_native_particles(object,invalid,true,error),"Incomplete native command accepted");
      auto projected=replica_detail::project_lifecycle_scalars(state);
      check(projected&&!projected->properties.contains("particles_enabled")&&
            validate_native_particles(object,*projected,false,error),
            "Post-lifecycle projection loses identity or writes emission directly");
    }
    const auto before=ticks;
    objects.push_back(objects[1]);
    check(update_native_particles(c,objects,.02f,error),"Whitelisted particle frame failed");
    check(ticks==before+5,"Frame double-ticked Sparkles/no-op/shared emitters");
    const auto immutable_calls=calls;
    check(!update_native_particles(c,objects,std::numeric_limits<float>::quiet_NaN(),error),
          "Nonfinite particle dt accepted");
    auto invalid=objects;invalid.push_back(0x817FFFFF);
    check(!update_native_particles(c,invalid,.02f,error)&&calls==immutable_calls,
          "Failed complete preflight made partial native calls");
    auto bad=context();bad.r[13]=0;
    check(!update_native_particles(bad,objects,.02f,error),"Invalid guest context accepted");
    replica=false;
    check(!update_native_particles(c,objects,.02f,error),"Host received duplicate visual ticks");
    replica=true;
    const auto emitter=mem_read32(objects[1]+0x60),table=mem_read32(emitter+0x1C);
    mem_write32(emitter+0x1C,0x80200000);
    check(!update_native_particles(c,objects,.02f,error),"Unverified emitter class accepted");
    mem_write32(emitter+0x1C,table);
    ssne::g_ram=nullptr;
    const auto null_before=calls;
    check(!ssne::set_native_scene_particle_enabled(c,objects[1],1,true,error),
          "Null guest RAM reached native particle activation");
    check(!ssne::update_native_scene_particle(c,emitter,1,.02f,error)&&calls==null_before,
          "Null guest RAM reached native particle update");
    ssne::g_ram=ram.get();
    check(starts==7&&stops==7,"Enable/Disable count differs from flag changes");
    disabled_sparkle_clock(c,mem_read32(objects[0]+0x60));
    std::cout<<"PASS checks="<<checks<<" starts="<<starts<<" stops="<<stops
             <<" visual_ticks="<<ticks<<" no_mission_or_damage_calls=true\n";
    return 0;
  }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<"\n";return 1;}
}
