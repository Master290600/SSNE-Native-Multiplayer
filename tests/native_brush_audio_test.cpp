// Contract checks execute the production audio-only brush implementation with
// strict native mixer callbacks. Audible samples require the separate PAL QA.
#include "native_lifecycle.h"
#include "native_campaign_verification.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "ssne/native_entity.h"
#include "ssne/native_results.h"
#include "ssne/replica_simulation.h"
#include "ssne/audio.h"
#include "ssne/game_addresses.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks=0,oneshots=0,loops=0,stops=0,generation=1;
bool replica=false,results=false,full=false;
std::map<uint32_t,PPCFunc> hooks;
std::vector<ssne::NativeEntityObserver> observers;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
bool live(uint32_t handle){return int32_t(handle)>=0 && (handle&63)<48 && mem_read32(0x802D69C8+(handle&63)*192) && mem_read32(0x802D69C8+(handle&63)*192+0xAC)==handle;}
void play(PPCContext& c){
    const auto sound=c.r[3];check(sound<479 && c.r[4]>=0x80000000,"Invalid mixer source");
    mem_write32(c.r[1]-32,0xABCDEF01);
    if(full){c.r[3]=UINT32_MAX;return;}
    for(unsigned i=0;i<48;++i){const auto record=0x802D69C8+i*192;if(!mem_read32(record)){
        const auto voice=0x80600000+i*0x200,handle=(++generation)*64+i;
        mem_write32(record,voice);mem_write32(record+0xAC,handle);mem_write32(record+0xB0,mem_read32(mem_read32(0x8023AD70+sound*24+12)+4));
        mem_write32(voice+0x18,i);c.r[3]=handle;return;
    }}
    c.r[3]=UINT32_MAX;
}
void one(PPCContext& c){++oneshots;play(c);}
void loop(PPCContext& c){++loops;play(c);}
void retire(PPCContext& c){++stops;const auto handle=c.r[4];if(live(handle))mem_write32(0x802D69C8+(handle&63)*192,0);}
void stop(PPCContext& c){
    const auto sp=c.r[1],lr=c.lr;c.r[1]-=8;mem_write32(c.r[1],sp);mem_write32(c.r[1]+12,lr);
    c.r[4]=c.r[3];c.lr=0x8010EE18;ssne_call(c,0x8015841C);c.r[1]=sp;c.lr=lr;
}
SSNEFuncEntry entries[]={{0x8010EAE0,one,"one"},{0x8010ED14,loop,"loop"},{0x8010EDFC,stop,"stop"},{0x8015841C,retire,"retire"}};
}
namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return 0x01800000;}uint32_t guest_ram_end(){return 0x81800000;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO");}
void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO");}
bool replica_simulation(){return replica;}bool native_replica_results_presenting(){return results;}
void log_message(const std::string&){}
uint64_t observe_native_entities(NativeEntityObserver observer){observers.push_back(std::move(observer));return observers.size();}
void audio_track_weapon_voice(uint32_t,uint32_t,uint32_t,uint64_t){throw std::runtime_error("Unexpected test-only PCM capture");}
[[noreturn]] void fatal(const char* message){throw std::runtime_error(message);}
}
const SSNEFuncEntry* ssne_find_func(uint32_t address){for(const auto& e:entries)if(e.address==address)return &e;return nullptr;}
void ssne_install_hook(uint32_t address,PPCFunc callback){hooks[address]=callback;}
void ssne_call(PPCContext& c,uint32_t address){if(auto it=hooks.find(address);it!=hooks.end())it->second(c);else if(auto entry=ssne_find_func(address))entry->func(c);else throw std::runtime_error("Unexpected mission/geometry call");}
namespace port::coop {
namespace {
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;}
bool context_ok(const PPCContext& c){return mem_read32(0x80000000)==0x47334250u && c.r[13]==0x802E22A0 && c.r[1]>=0x80000800 && readable(c.r[1]-0x800,0x800);}
struct StackScratch {uint32_t address;std::array<uint8_t,0x800> saved;explicit StackScratch(const PPCContext& c):address(c.r[1]-0x800){memcpy(saved.data(),ssne::host_ptr(address),saved.size());}~StackScratch(){memcpy(ssne::host_ptr(address),saved.data(),saved.size());}};
}
#include "native_brush_audio.inc"
}
using namespace port::coop;
int main(){try{
    for(const auto key:{"brush_audio_events","brush_audio_serial","brush_audio_loop"})
        for(unsigned kind=0;kind<=unsigned(EntityKind::Mission);++kind)
            check(replica_detail::campaign_brush_audio_command(EntityKind(kind),key)==(EntityKind(kind)==EntityKind::Door),"Campaign audio command exception escaped its exact entity class");
    for(const auto key:{"brush_audio_position","brush_audio_events_extra","brush_audio","brush_progress","brush_segment","brush_index","brush_resource","brush_active","brush_completed","position"})
        check(!replica_detail::campaign_brush_audio_command(EntityKind::Door,key),"Campaign excluded a native geometry/spatial or unknown field");
    auto ram=std::make_unique<uint8_t[]>(ssne::guest_ram_size());ssne::g_ram=ram.get();
    mem_write32(0x80000000,0x47334250);mem_write32(ssne::game_address(0x802DB0E0),4);
    for(auto [address,signature]:{std::pair{0x8010EAE0u,0x9421FFE0u},std::pair{0x8010ED14u,0x9421FFE0u},std::pair{0x8010EDFCu,0x9421FFF8u},std::pair{0x8015841Cu,0x9421FFE8u}})mem_write32(address,signature);
    for(unsigned sound=100;sound<=103;++sound){const auto resource=0x8023AD70+sound*24,samples=0x80500000+sound*8;mem_write32(resource+8,2);mem_write32(resource+12,samples);mem_write32(samples,sound*10);mem_write32(samples+4,sound*10+1);}
    constexpr uint32_t host=0x80400000,client=0x80401000,late=0x80402000;
    for(auto object:{host,client,late}){mem_write32(object+0x48,0x80206868);mem_write32(object+0x50,7);mem_write32(object+0x68,UINT32_MAX);mem_write32(object+0x6C,UINT32_MAX);mem_write32(object+0x94,100);mem_write32(object+0x98,101);mem_write32(object+0x9C,102);for(unsigned i=0;i<3;++i)mem_write32(object+0x70+i*4,f2u(float(i+1)));}
    PPCContext context;context.r[13]=0x802E22A0;context.r[1]=0x817E0000;context.r[3]=123;context.ps0[1]=4.25;
    std::string error;EntityState state;state.kind=EntityKind::Door;state.definition=1;
    auto capture=[&]{replica=false;error.clear();check(capture_native_brush_audio(host,state,error),error.c_str());};
    auto apply=[&](uint32_t object=client,uint64_t epoch=1,bool fresh_world=false){replica=true;const auto cpu=context;std::array<uint8_t,0x800> stack;memcpy(stack.data(),ssne::host_ptr(context.r[1]-0x800),stack.size());error.clear();check(apply_native_brush_audio(context,epoch,object,state,error,fresh_world),error.c_str());check(!memcmp(&cpu,&context,sizeof(context)),"Audio changed caller CPU");check(!memcmp(stack.data(),ssne::host_ptr(context.r[1]-0x800),stack.size()),"Audio changed borrowed stack");};
    auto native=[&](uint32_t address,uint32_t sound,uint32_t caller){replica=false;auto c=context;c.r[31]=host;c.r[3]=sound;c.r[4]=host+0x70;c.lr=caller;ssne_call(c,address);return c.r[3];};
    capture();apply();check(!oneshots && !loops,"Baseline replayed old audio");
    const auto host_start=native(0x8010EAE0,100,0x8007F178);mem_write32(host+0x68,host_start);capture();
    const auto original_ones=oneshots;apply();check(oneshots==original_ones+1,"Start cue missing");const auto client_start=brush_replica_audio.at(client).start_handle;check(live(client_start),"Client opening cue has no voice");apply();check(oneshots==original_ones+1,"Duplicate snapshot replayed start");
    const auto host_loop=native(0x8010ED14,101,0);mem_write32(host+0x6C,host_loop);capture();const auto original_loops=loops;apply();check(loops==original_loops+1,"Moving loop missing");const auto client_loop=brush_replica_audio.at(client).handle;check(live(client_loop),"Loop with multiple sample variants not recognized");apply();check(loops==original_loops+1,"Duplicate loop voice");
    native(0x8010EDFC,host_start,0x8007F544);native(0x8010EDFC,host_loop,0x8007F56C);native(0x8010EAE0,102,0x8007F5A0);capture();
    const auto end_count=oneshots;apply();check(!live(client_start) && !live(client_loop),"End did not retire opening cue and loop");check(oneshots==end_count+1,"Authored end cue missing");apply();check(oneshots==end_count+1,"End cue duplicated");
    mem_write32(host+0x68,native(0x8010EAE0,100,0x8007F270));capture();apply();check(live(brush_replica_audio.at(client).start_handle),"Closing/repeated start missing");
    const auto late_count=oneshots;apply(late);check(oneshots==late_count,"Late join replayed completed start/end history");
    mem_write32(host+0x9C,350);mem_write32(client+0x9C,350);native(0x8010EDFC,mem_read32(host+0x68),0x8007F544);capture();const auto no_end_count=oneshots;const auto no_end_handle=brush_replica_audio.at(client).start_handle;apply();check(!live(no_end_handle) && oneshots==no_end_count,"Door without end sample failed to retire start");
    mem_write32(host+0x6C,native(0x8010ED14,101,0));capture();const auto before_late_loop=loops;apply(late);check(loops==before_late_loop+1,"Late join did not resume existing moving loop");
    results=true;const auto before_result_loop=loops;apply(client);check(loops==before_result_loop,"Result UI restarted moving loop");check(stop_native_brush_audio(context,error),"Results cleanup failed");
    results=false;full=true;const auto before_retry=loops;apply(client);check(loops==before_retry+1 && !live(brush_replica_audio.at(client).handle),"Full pool did not leave loop retryable");full=false;apply(client);const auto reuse=brush_replica_audio.at(client).handle;check(live(reuse),"Pool recovery did not start loop");
    auto c=context;c.r[3]=reuse;ssne_call(c,0x8010EDFC);c=context;c.r[3]=103;c.r[4]=client+0x70;ssne_call(c,0x8010ED14);const auto unrelated=c.r[3];check((reuse&63)==(unrelated&63),"Reuse test did not reuse native record");check(stop_native_brush_audio(context,error) && live(unrelated),"Stale brush handle cut unrelated generation/sample");
    auto invalid=state;invalid.properties.erase("brush_audio_loop");const auto before_bad=oneshots+loops+stops;error.clear();check(!apply_native_brush_audio(context,1,client,invalid,error),"Incomplete audio state accepted");check(oneshots+loops+stops==before_bad,"Invalid state touched mixer");
    invalid=state;invalid.properties["brush_audio_events"]=std::vector<int64_t>{1,9};check(!validate_native_brush_audio(client,invalid,error),"Invalid cue stage accepted");
    invalid=state;invalid.properties["brush_audio_position"]=std::array<float,3>{NAN,0,0};check(!validate_native_brush_audio(client,invalid,error),"Nonfinite audio source accepted");
    const auto host_still=mem_read32(host+0x6C);check(live(host_still),"Replica cleanup stopped original host voice");
    // Another session can reuse both the guest object and wire epoch1. Its
    // first snapshot seeds silently even when its serial is below the old host.
    state.properties["brush_audio_serial"]=int64_t(1);state.properties["brush_audio_events"]=std::vector<int64_t>{1,0};state.properties["brush_audio_loop"]=int64_t(0);
    const auto stream_ones=oneshots,stream_loops=loops;apply(client,1,true);check(oneshots==stream_ones && loops==stream_loops,"Fresh same-epoch stream replayed old history");
    check(brush_replica_audio.at(client).serial==1,"New stream kept previous host cursor");
    state.properties["brush_audio_serial"]=int64_t(2);state.properties["brush_audio_events"]=std::vector<int64_t>{1,0,2,0};apply(client);check(oneshots==stream_ones+1,"New stream did not play its next original cue");
    for(auto& observer:observers)observer({ssne::NativeEntityNotice::Event::LevelExit,0,0,-2,{},&context});check(brush_replica_audio.empty() && brush_host_audio.empty(),"Level exit kept stale audio state");check(live(host_still) && live(unrelated),"Level cleanup cut voices owned by another source");
    std::cout<<checks<<" native brush audio contract checks passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
