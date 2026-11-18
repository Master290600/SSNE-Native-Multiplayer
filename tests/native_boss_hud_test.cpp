#include "../runtime/src/native_boss_hud.cpp"
#include "../port/src/coop/native_boss_hud_mission.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace fixture {
using namespace ssne;using namespace ssne::native_boss_hud_detail;
constexpr uint32_t ram_size=24*1024*1024,scene=0x80400000,root=0x80401000,widget=0x80402000,draw=0x80403000,heap=0x80404000;
std::vector<uint8_t> ram(ram_size);std::vector<uint32_t> calls;unsigned checks=0;uint32_t throw_at=0;
void require(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
void hidden(bool value){mem_write16(draw+0x1E,value?1:0);}
void service(PPCContext& c,uint32_t address){
    calls.push_back(address);require(!(c.msr&0x8000u),"Original HUD callback received interrupts enabled");
    mem_write32(c.r[1]-0x100,0xAABBCCDDu);
    if(throw_at==address)throw std::runtime_error("controlled original callback exception");
    if(address==active_setter){
        mem_write32(active_global,c.r[3]);hidden(!c.r[3] || mem_read32(suppressed_global));
        if(c.r[3])mem_write32(widget+0x40,f2u(0));
    }else if(address==suppressed_setter){mem_write32(suppressed_global,c.r[3]);hidden(!mem_read32(active_global) || c.r[3]);}
    else if(address==ratio_setter){mem_write32(widget+0x50,f2u(float(c.ps0[1])));mem_write32(widget+0x40,f2u(.5f));}
    else if(address==presentation_tick){
        require(c.r[3]==scene,"Original boss HUD frame updater received the wrong scene");
        require(c.ps0[1]>0 && c.ps0[1]<=.100001,"Original widget received unbounded frame time");
        if(mem_read32(active_global))mem_write32(widget+0x40,f2u(std::max(0.f,u2f(mem_read32(widget+0x40))-float(c.ps0[1]))));
    }
    c.r[3]=0xBADu;c.ps0[1]=123.5;c.ps1[1]=-100;c.lr=0;
}
template<uint32_t Address>void original(PPCContext& c){service(c,Address);}
PPCContext context(){PPCContext c{};c.r[1]=0x817FF000;c.r[2]=0x802F22A0;c.r[13]=0x802E22A0;c.msr=0xA032;c.lr=0x80008000;return c;}
void initialize(){
    std::fill(ram.begin(),ram.end(),0);ssne::g_ram=ram.data();calls.clear();throw_at=0;forget_native_boss_hud();
    mem_write32(0x80000000,0x47334250);mem_write32(0x80000004,0x39470000);mem_write32(0x802DB7E8,4);mem_write32(0x802DB8CC,1);
    mem_write32(widget_vtable+0x1C,0x800AD6B4);mem_write32(widget_vtable+0x14,0x800AD510);mem_write32(drawable_vtable+0x44,0x8011D458);
    for(auto [a,op]:{std::pair{ratio_setter,0x9421FFF8u},std::pair{active_setter,0x9421FFF0u},std::pair{suppressed_setter,0x9421FFF0u},std::pair{0x800AD6B4u,0x9421FFE8u},std::pair{presentation_tick,0x9421FFF8u},std::pair{0x800AD510u,0x9421FFC8u}})mem_write32(a,op);
    mem_write32(scene+0x78,root);mem_write32(root,drawable_vtable);mem_write32(widget_global,widget);
    mem_write32(widget-24,0x60);mem_write32(widget-20,heap);mem_write32(widget+0x3C,widget_vtable);mem_write32(widget,draw);
    mem_write32(draw,drawable_vtable);mem_write32(draw+0x14,root);mem_write32(widget+0x50,f2u(1));hidden(true);
}
template<class Action>void preserved(PPCContext& c,Action action){
    const auto cpu=c;const auto begin=host_ptr(c.r[1]-stack_bytes);const std::vector<uint8_t> bytes(begin,begin+stack_bytes);
    try{action();}catch(...){require(!std::memcmp(&cpu,&c,sizeof(c)),"Exception changed caller CPU");require(std::equal(bytes.begin(),bytes.end(),begin),"Exception changed caller guest stack");throw;}
    require(!std::memcmp(&cpu,&c,sizeof(c)),"HUD application changed caller CPU");require(std::equal(bytes.begin(),bytes.end(),begin),"HUD application changed caller guest stack");
}
}
namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return fixture::ram_size;}uint32_t guest_ram_end(){return 0x80000000u+fixture::ram_size;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("unexpected MMIO");}void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("unexpected MMIO");}
uint32_t native_game_scene(){return fixture::scene;}
}
const SSNEFuncEntry g_ssne_funcs[]={
    {0x80168804,fixture::original<0x80168804>,"ratio"},{0x801687AC,fixture::original<0x801687AC>,"active"},
    {0x8016883C,fixture::original<0x8016883C>,"suppressed"},{0x800AD6B4,fixture::original<0x800AD6B4>,"ratio_impl"},
    {0x80168A0C,fixture::original<0x80168A0C>,"visual_frame"},{0x800AD510,fixture::original<0x800AD510>,"visual_frame_impl"}
};
const size_t g_ssne_func_count=std::size(g_ssne_funcs);
const SSNEFuncEntry* ssne_find_func(uint32_t a){for(const auto& fn:g_ssne_funcs)if(fn.address==a)return &fn;return nullptr;}
void ssne_call(PPCContext& c,uint32_t a){const auto fn=ssne_find_func(a);if(!fn)throw std::runtime_error("unknown callback");fn->func(c);}
int main()try{
    using namespace fixture;using namespace ssne;using namespace ssne::native_boss_hud_detail;
    initialize();auto c=context();std::string error;NativeBossHudState desired{true,true,false,.5f},captured;
    const auto before=ram;require(capture_native_boss_hud(captured,error) && captured.present && !captured.active && captured.ratio==1,"Initial native HUD capture failed");require(ram==before && calls.empty(),"Read-only capture mutated native state");
    preserved(c,[&]{require(apply_native_boss_hud(c,1,desired,error),error.c_str());});require(calls==std::vector<uint32_t>{active_setter,ratio_setter},"Initial bind did not use original visibility/ratio setters");
    NativeBossHudObservation observed;require(observe_native_boss_hud(observed,error) && observed.state.ratio==.5f && !observed.hidden,"Applied target ratio/visibility was not readable");
    mem_write32(widget+0x40,f2u(.123f));calls.clear();const auto stable=ram;
    for(unsigned i=0;i<20;++i)preserved(c,[&]{require(apply_native_boss_hud(c,1,desired,error),error.c_str());});
    require(calls.empty() && ram==stable,"Unchanged snapshots retriggered flash or modified native state");
    desired.ratio=.25f;preserved(c,[&]{require(apply_native_boss_hud(c,1,desired,error),error.c_str());});require(calls==std::vector<uint32_t>{ratio_setter},"Changed ratio did not use exactly one native setter");
    calls.clear();desired.suppressed=true;preserved(c,[&]{require(apply_native_boss_hud(c,1,desired,error),error.c_str());});require(calls==std::vector<uint32_t>{suppressed_setter} && (mem_read16(draw+0x1E)&1),"Suppression reset ratio flash or left bar visible");
    calls.clear();desired.suppressed=false;desired.active=false;preserved(c,[&]{require(apply_native_boss_hud(c,1,desired,error),error.c_str());});require(mem_read16(draw+0x1E)&1,"Boss death did not hide native bar");
    desired.active=true;calls.clear();preserved(c,[&]{require(apply_native_boss_hud(c,2,desired,error),error.c_str());});require(calls.back()==ratio_setter,"Fresh epoch/late join did not initialize current target ratio");
    calls.clear();preserved(c,[&]{require(clear_native_boss_hud(c,error),error.c_str());});require(calls==std::vector<uint32_t>{active_setter} && !mem_read32(active_global) && (mem_read16(draw+0x1E)&1),"Transition cleanup did not hide");
    // Freed/reparented widgets must not be invoked via the previous cache.
    mem_write32(draw+0x14,root+4);calls.clear();const auto stale=ram;preserved(c,[&]{require(clear_native_boss_hud(c,error),error.c_str());});require(calls.empty() && ram==stale,"Cleanup touched a replaced native widget");
    initialize();c=context();for(unsigned kind=0;kind<7;++kind){
        auto bad=desired;auto cpu=c;uint32_t address=0,saved=0;
        switch(kind){case 0:bad.ratio=std::numeric_limits<float>::quiet_NaN();break;case 1:bad.ratio=-.1f;break;case 2:bad.ratio=1.1f;break;case 3:cpu.r[13]+=4;break;case 4:address=draw+0x14;break;case 5:address=widget+0x3C;break;case 6:address=0x80000004;break;}
        if(address){saved=mem_read32(address);mem_write32(address,0);}const auto untouched=ram;calls.clear();require(!apply_native_boss_hud(cpu,1,bad,error),"Invalid state/context/binding accepted");require(calls.empty() && ram==untouched,"Invalid state/context/binding mutated native RAM");if(address)mem_write32(address,saved);
    }
    initialize();throw_at=ratio_setter;bool threw=false;preserved(c,[&]{try{apply_native_boss_hud(c,1,desired,error);}catch(const std::runtime_error&){threw=true;}});require(threw,"Original callback exception was not propagated");
    using namespace port::coop;Properties mission;publish_native_boss_hud_mission(mission,desired);NativeBossHudState decoded;
    calls.clear();const auto old_host_ram=ram;require(apply_native_boss_hud_mission(c,1,Properties{},error),"Old protocol67 host without additive HUD properties was rejected");require(calls.empty() && ram==old_host_ram,"Absent optional HUD properties caused native calls/writes");
    require(read_native_boss_hud_mission(mission,decoded,error) && decoded.ratio==desired.ratio,"Mission HUD roundtrip failed");
    for(const auto& key:{"Session.BossHud.Present","Session.BossHud.Active","Session.BossHud.Suppressed","Session.BossHud.Ratio"}){auto broken=mission;broken.erase(key);require(!read_native_boss_hud_mission(broken,decoded,error),"Partial HUD schema accepted");}
    auto broken=mission;broken["Session.BossHud.Active"]=int64_t(2);require(!read_native_boss_hud_mission(broken,decoded,error),"Nonboolean HUD flag accepted");
    broken=mission;broken["Session.BossHud.Ratio"]=int64_t(1);require(!read_native_boss_hud_mission(broken,decoded,error),"Wrong ratio property type accepted");
    broken=mission;broken["Session.BossHud.Unknown"]=int64_t(1);require(!read_native_boss_hud_mission(broken,decoded,error),"Unknown HUD property accepted");
    // Original flash duration must be wall-time based at low and high FPS,
    // independently of snapshot delivery. Repeated snapshots do not reset it.
    const auto origin=std::chrono::steady_clock::time_point{}+std::chrono::seconds(100);
    auto at=[&](double seconds){return origin+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));};
    for(unsigned fps:{30u,60u,240u}){
        initialize();c=context();desired={true,true,false,.5f};require(apply_native_boss_hud(c,1,desired,error),error.c_str());
        require(advance_native_boss_hud(c,0,at(0),error),error.c_str());
        for(unsigned frame=1;frame<=fps/2+1;++frame){
            const auto when=at(double(frame)/fps);
            preserved(c,[&]{require(advance_native_boss_hud(c,frame,when,error),error.c_str());});
            if(frame==fps*2/5)require(u2f(mem_read32(widget+0x40))>.06f && u2f(mem_read32(widget+0x40))<.13f,"Flash aged by frame count instead of wall time");
            const auto unchanged=ram;const auto count=calls.size();
            require(advance_native_boss_hud(c,frame,when+std::chrono::milliseconds(1),error),error.c_str());
            require(calls.size()==count && ram==unchanged,"Same rendered frame ticked original boss widget twice");
            require(apply_native_boss_hud(c,1,desired,error),error.c_str());
        }
        require(u2f(mem_read32(widget+0x40))<=.00001f,"Flash did not expire after half a second at tested FPS");
        require(std::count(calls.begin(),calls.end(),ratio_setter)==1,"Stable snapshots restarted flash while frame clock advanced");
    }
    // A long render stall consumes bounded small native updates and leaves no
    // backlog that could instantly expire a subsequently received new flash.
    initialize();c=context();desired={true,true,false,.4f};require(apply_native_boss_hud(c,1,desired,error),error.c_str());
    require(advance_native_boss_hud(c,1,at(0),error),error.c_str());calls.clear();
    preserved(c,[&]{require(advance_native_boss_hud(c,2,at(120),error),error.c_str());});
    require(u2f(mem_read32(widget+0x40))==0 && calls.size()<=600,"Long frame left flash alive or exceeded native catch-up bound");
    desired.ratio=.3f;require(apply_native_boss_hud(c,1,desired,error),error.c_str());calls.clear();
    require(advance_native_boss_hud(c,3,at(240),error),error.c_str());require(u2f(mem_read32(widget+0x40))==.5f && calls.empty(),"Pre-arrival backlog aged a new flash");
    require(advance_native_boss_hud(c,4,at(240.6),error),error.c_str());require(u2f(mem_read32(widget+0x40))==0,"New flash did not expire with stalled snapshots");
    // Epoch bind and cleanup reset the local presentation clock and never use
    // cached old widgets after native ownership changes.
    require(apply_native_boss_hud(c,2,desired,error),error.c_str());calls.clear();
    require(advance_native_boss_hud(c,5,at(1000),error),error.c_str());require(calls.empty() && u2f(mem_read32(widget+0x40))==.5f,"Epoch clock reused pre-bind time");
    const auto unchanged=ram;auto bad_context=c;bad_context.r[13]+=4;require(!advance_native_boss_hud(bad_context,6,at(1000.1),error) && ram==unchanged,"Invalid clock context mutated native state");
    require(!advance_native_boss_hud(c,6,at(999),error) && ram==unchanged,"Backwards clock mutated native state");
    require(clear_native_boss_hud(c,error),error.c_str());calls.clear();const auto cleared=ram;
    require(advance_native_boss_hud(c,7,at(2000),error) && calls.empty() && ram==cleared,"Cleaned-up HUD clock invoked an old widget");
    desired.active=false;require(apply_native_boss_hud(c,3,desired,error),error.c_str());
    require(advance_native_boss_hud(c,8,at(2100),error),error.c_str());calls.clear();const auto inactive=ram;
    require(advance_native_boss_hud(c,9,at(2220),error) && calls.empty() && ram==inactive,"Inactive boss bar did unnecessary native frame work");
    std::cout<<checks<<" native boss HUD assertions passed: original setters, 30/60/240FPS wall-clock flash expiry, duplicate-frame guard, long stalls/new arrivals, epochs, ownership, cleanup, CPU/stack and schema\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
