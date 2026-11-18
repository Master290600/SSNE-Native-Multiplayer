// Private owned-PAL auto-aim binding, restoration and real firing-getter checks.
// Link only against the owner's existing generated game objects; never ship fixtures.
#include "native_lifecycle.h"
#include "ssne/native_party.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <limits>
using namespace port::coop;
static void require(bool ok,const std::string& why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
static std::vector<uint8_t> read(const std::string& p){std::ifstream f(p,std::ios::binary);require(bool(f),"Missing private fixture: "+p);return {(std::istreambuf_iterator<char>(f)),{}};}
static uint64_t digest(){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<ssne::guest_ram_size();++i){h^=ssne::g_ram[i];h*=1099511628211ull;}return h;}
int main(int argc,char** argv){
 require(argc==3,"Pass owned PAL game_data and extended fixture");ssne::native_party_extended_memory(true);PPCContext ctx;std::string error;require(ssne::boot(ctx,argv[1],error)!=0,error);
 const std::string dir=argv[2];auto ram=read(dir+"/native-capture.ram");require(ram.size()==ssne::guest_ram_size(),"Wrong private RAM size");std::memcpy(ssne::g_ram,ram.data(),ram.size());
 const auto cpu=read(dir+"/native-capture-context.bin");require(cpu.size()==sizeof(ctx),"Wrong context size");std::memcpy(&ctx,cpu.data(),sizeof(ctx));ctx.msr&=~0x8000u;ctx.r[1]=ssne::guest_ram_end()-0x40000;mem_write32(ctx.r[1],0);
 const auto cache=read(dir+"/native-capture-locked-cache.bin");require(cache.size()==ssne::kLockedCacheSize,"Wrong cache size");std::memcpy(ssne::g_locked_cache,cache.data(),cache.size());require(ssne::native_party_load_state(read(dir+"/native-capture-party.bin"),error),error);
 unsigned cases=0;
 for(unsigned slot=0;slot<2;++slot){
  const auto actor=mem_read32(ssne::native_party_registry()+slot*4);NativeAutoAimLease info;require(read_native_auto_aim_binding(actor,info,error),error);require(info.slot==slot,"Wrong actor ownership");
  for(unsigned previous=0;previous<=2;++previous){
   std::array<uint8_t,8> preferences{255,255,255,255,255,255,255,255};preferences[0]=uint8_t(previous);require(apply_native_preferences(ctx,actor,preferences,error),error);
   const auto before_preferences=capture_native_preferences(actor);NativeAutoAimLease lease;
   require(begin_native_auto_aim_override(ctx,actor,lease,error),error);require(lease.saved_mode==previous&&native_auto_aim_lease_matches(lease),"Lease lost saved mode/binding");
   require(mem_read32(info.variable+0x24)==1&&mem_read32(info.aim)==1,"AFK did not enable option and controller");
   require(begin_native_auto_aim_override(ctx,actor,lease,error)&&lease.saved_mode==previous,"Repeated AFK recaptured temporary On");
   require(end_native_auto_aim_override(lease),"Live AFK lease failed to restore");require(!lease.actor&&mem_read32(info.variable+0x24)==previous&&mem_read32(info.aim)==previous,"Human option/controller not restored");require(capture_native_preferences(actor)==before_preferences,"AFK changed unrelated human preferences");++cases;
  }
  // A replaced option/controller/registry must not receive writes from an old lease.
  for(unsigned field:{0x378u,0x37Cu}){
   NativeAutoAimLease lease;require(begin_native_auto_aim_override(ctx,actor,lease,error),error);const auto original=mem_read32(actor+field);mem_write32(actor+field,0);const auto unchanged=digest();
   require(!end_native_auto_aim_override(lease)&&!lease.actor&&digest()==unchanged,"Stale option/controller lease wrote RAM");mem_write32(actor+field,original);++cases;
  }
  NativeAutoAimLease lease;require(begin_native_auto_aim_override(ctx,actor,lease,error),error);const auto registry=ssne::native_party_registry()+slot*4;mem_write32(registry,0);const auto unchanged=digest();require(!end_native_auto_aim_override(lease)&&digest()==unchanged,"Retired actor lease wrote RAM");mem_write32(registry,actor);++cases;
  require(set_native_afk_manual_aim(ctx,actor,true,error),error);
  mem_write32(info.aim,0);mem_write32(info.aim+0x44,0);std::array<float,3> manual;require(native_player_fire_direction(ctx,actor,manual,error),error);
  const std::array<float,3> acquired{.25f,.5f,.75f};for(unsigned i=0;i<3;++i)mem_write32(info.aim+0x34+i*4,f2u(acquired[i]));mem_write32(info.aim,1);mem_write32(info.aim+0x48,1);mem_write32(info.aim+0x44,1);
  std::array<float,3> actual;require(native_player_fire_direction(ctx,actor,actual,error)&&actual==acquired,"AFK ignored valid original auto-aim firing vector");++cases;
  mem_write32(info.aim+0x48,0);require(native_player_fire_direction(ctx,actor,actual,error)&&actual==acquired,"Native available vector incorrectly depended on cone/marker flag");++cases;
  mem_write32(info.aim,0);require(native_player_fire_direction(ctx,actor,actual,error)&&actual==manual,"Off used stale acquired vector");++cases;
  mem_write32(info.aim,1);mem_write32(info.aim+0x34,f2u(std::numeric_limits<float>::quiet_NaN()));require(native_player_fire_direction(ctx,actor,actual,error)&&actual==manual,"Invalid acquired vector had no manual fallback");++cases;
  require(set_native_afk_manual_aim(ctx,actor,false,error),error);
 }
 const auto before=digest();NativeAutoAimLease invalid;require(!begin_native_auto_aim_override(ctx,0,invalid,error)&&digest()==before,"Invalid actor changed RAM");++cases;
 clear_native_afk_manual_aim();std::cout<<"{\"passed\":true,\"cases\":"<<cases<<",\"real_player_slots\":2,\"off_on_internal_mode_restored\":true,\"repeated_afk_keeps_human_preference\":true,\"unrelated_preferences_preserved\":true,\"stale_bindings_no_writes\":true,\"valid_native_aim_direction_used\":true,\"off_and_nan_manual_fallback\":true}\n";std::cout.flush();std::_Exit(0);
}
