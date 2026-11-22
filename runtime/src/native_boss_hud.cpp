#include "ssne/native_boss_hud.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
namespace ssne { namespace native_boss_hud_detail {
constexpr uint32_t widget_global=0x802DB8D4u,active_global=0x802DB8D8u,suppressed_global=0x802DB8DCu;
constexpr uint32_t widget_vtable=0x80211368u,drawable_vtable=0x8021CAB0u;
constexpr uint32_t ratio_setter=0x80168804u,active_setter=0x801687ACu,suppressed_setter=0x8016883Cu;
constexpr uint32_t presentation_tick=0x80168A0Cu;
constexpr unsigned stack_bytes=0x4000;
struct Binding {
    uint32_t scene=0,root=0,widget=0,drawable=0,heap=0;
    bool operator==(const Binding&)const=default;
};
struct Applied {Binding binding{};uint64_t epoch=0;bool valid=false;};
Applied applied;
struct PresentationClock {Binding binding{};uint64_t epoch=0,frame=0;std::chrono::steady_clock::time_point time{};bool initialized=false;};
PresentationClock boss_clock;
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;}
bool revision(std::string* error=nullptr){
    if(mem_read32(0x80000000u)!=0x47334250u || mem_read32(0x80000004u)!=0x39470000u){
        if(error)*error="Native boss HUD requires original G3BP9G revision0";
        return false;
    }
    if(mem_read32(widget_vtable+0x1C)!=0x800AD6B4u || mem_read32(widget_vtable+0x14)!=0x800AD510u ||
       mem_read32(drawable_vtable+0x44)!=0x8011D458u){
        if(error)*error="Native boss HUD vtable layout differs";
        return false;
    }
    for(const auto [address,signature]:{std::pair{ratio_setter,0x9421FFF8u},std::pair{active_setter,0x9421FFF0u},
            std::pair{suppressed_setter,0x9421FFF0u},std::pair{0x800AD6B4u,0x9421FFE8u},std::pair{presentation_tick,0x9421FFF8u},std::pair{0x800AD510u,0x9421FFC8u}}){
        if(!ssne_find_func(address) || mem_read32(address)!=signature){
            if(error)*error="Native boss HUD callback unavailable at "+std::to_string(address);
            return false;
        }
    }
    return true;
}
bool gameplay(){return mem_read32(0x802DB7E8u)==4u && mem_read32(0x802DB8CCu)<=1u;}
bool binding(Binding& out){
    if(!revision())return false;
    out.scene=native_game_scene();
    if(!readable(out.scene,0xA8))return false;
    out.root=mem_read32(out.scene+0x78);out.widget=mem_read32(widget_global);
    if(!readable(out.root,0x44) || mem_read32(out.root)!=drawable_vtable ||
       !readable(out.widget-24,0x54+24) || mem_read32(out.widget+0x3C)!=widget_vtable)return false;
    const auto size=mem_read32(out.widget-24);out.heap=mem_read32(out.widget-20);
    if(size<0x54 || size>4096 || !readable(out.heap,24))return false;
    out.drawable=mem_read32(out.widget);
    return readable(out.drawable,0x44) && mem_read32(out.drawable)==drawable_vtable &&
        mem_read32(out.drawable+0x14)==out.root;
}
bool context_ok(const PPCContext& c){return c.r[13]==0x802E22A0u && c.r[2]==0x802F22A0u &&
    !(c.r[1]&7u) && c.r[1]>=0x80005000u && readable(c.r[1]-stack_bytes,stack_bytes);}
struct Scratch {
    PPCContext base;uint32_t low;std::array<uint8_t,stack_bytes> saved;
    explicit Scratch(const PPCContext& c):base(c),low(c.r[1]-stack_bytes){std::memcpy(saved.data(),host_ptr(low),saved.size());}
    ~Scratch(){std::memcpy(host_ptr(low),saved.data(),saved.size());}
    void invoke(uint32_t fn,uint32_t value=0,float ratio=0){
        auto c=base;c.r[1]-=0x200;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);c.r[3]=value;
        c.ps0[1]=c.ps1[1]=ratio;ssne_call(c,fn);
    }
};
}
bool validate_native_boss_hud(const NativeBossHudState& s,std::string& error){
    error.clear();
    if(!std::isfinite(s.ratio) || s.ratio<0.f || s.ratio>1.f ||
       (!s.present && (s.active || s.suppressed || s.ratio!=1.f))){error="Invalid native boss HUD presentation state";return false;}
    return true;
}
bool observe_native_boss_hud(NativeBossHudObservation& out,std::string& error){
    using namespace native_boss_hud_detail;out={};error.clear();
    if(!revision(&error))return false;
    if(!gameplay())return true;
    Binding b;if(!binding(b)){error="Native boss HUD widget ownership differs";return false;}
    const auto active=mem_read32(active_global),suppressed=mem_read32(suppressed_global);
    if(active>1 || suppressed>1){error="Native boss HUD visibility flags differ";return false;}
    out.state={true,bool(active),bool(suppressed),u2f(mem_read32(b.widget+0x50))};
    out.widget=b.widget;out.drawable=b.drawable;out.hidden=(mem_read16(b.drawable+0x1E)&1)!=0;
    out.flash_seconds=u2f(mem_read32(b.widget+0x40));
    if(!std::isfinite(out.flash_seconds)){error="Native boss HUD flash timer differs";return false;}
    return validate_native_boss_hud(out.state,error);
}
bool capture_native_boss_hud(NativeBossHudState& out,std::string& error){
    NativeBossHudObservation observed;if(!observe_native_boss_hud(observed,error))return false;out=observed.state;return true;
}
bool apply_native_boss_hud(PPCContext& context,uint64_t epoch,const NativeBossHudState& desired,std::string& error){
    using namespace native_boss_hud_detail;
    if(!validate_native_boss_hud(desired,error))return false;
    if(!desired.present)return clear_native_boss_hud(context,error);
    if(!epoch || !context_ok(context)){error="Native boss HUD call context differs";return false;}
    NativeBossHudObservation before;if(!observe_native_boss_hud(before,error))return false;
    Binding b;if(!before.state.present || !binding(b)){error="Native boss HUD is not initialized for gameplay";return false;}
    const bool rebound=!applied.valid || applied.epoch!=epoch || !(applied.binding==b);
    const bool active_changed=before.state.active!=desired.active;
    const bool suppressed_changed=before.state.suppressed!=desired.suppressed;
    const bool ratio_changed=before.state.ratio!=desired.ratio;
    if(active_changed || suppressed_changed || ratio_changed || rebound){
        Scratch call(context);
        if(suppressed_changed)call.invoke(suppressed_setter,unsigned(desired.suppressed));
        if(active_changed)call.invoke(active_setter,unsigned(desired.active));
        // Original setter starts a short flash. Never restart it on unchanged
        // snapshots; force once only for a newly bound native HUD/epoch.
        if(ratio_changed || rebound || (active_changed && desired.active)){
            call.invoke(ratio_setter,0,desired.ratio);
            // Arrival of a new native flash starts its own clock. Time before
            // this setter must never be charged to the newly received flash.
            boss_clock={};
        }
    }
    NativeBossHudObservation after;if(!observe_native_boss_hud(after,error))return false;
    if(after.widget!=b.widget || after.drawable!=b.drawable || after.state.active!=desired.active ||
       after.state.suppressed!=desired.suppressed || after.state.ratio!=desired.ratio ||
       after.hidden!=(!desired.active || desired.suppressed)){error="Original boss HUD setter readback differs";return false;}
    applied={b,epoch,true};return true;
}
bool advance_native_boss_hud(PPCContext& context,uint64_t frame,std::chrono::steady_clock::time_point now,std::string& error){
    using namespace native_boss_hud_detail;error.clear();
    if(!applied.valid || !gameplay()){boss_clock={};return true;}
    if(!context_ok(context)){error="Native boss HUD presentation context differs";return false;}
    Binding b;if(!binding(b)){error="Native boss HUD presentation binding differs";return false;}
    // Revalidate ownership every frame. A new local widget must first receive
    // an authoritative snapshot; never tick a retained/recycled old pointer.
    if(!(b==applied.binding)){boss_clock={};return true;}
    if(!boss_clock.initialized || !(boss_clock.binding==b) || boss_clock.epoch!=applied.epoch || frame<boss_clock.frame){
        boss_clock={b,applied.epoch,frame,now,true};return true;
    }
    if(frame==boss_clock.frame)return true;
    if(now<boss_clock.time){error="Native boss HUD presentation clock moved backwards";return false;}
    // The original wrapper immediately returns while no boss bar is active.
    if(!mem_read32(active_global)){boss_clock={b,applied.epoch,frame,now,true};return true;}
    double remaining=std::min(60.,std::chrono::duration<double>(now-boss_clock.time).count());
    // The original game update80166724 called80168A0C, which dispatches the
    // widget's800AD510 update. Replica world suppression skips that call.
    // Replay only this visual callback, in <=.1s steps. Consume up to60s
    // after a long stall, then drop backlog: a <=.5s flash has already expired,
    // and the looping texture interpolator never receives an unsafe huge dt.
    if(remaining>0.){
        Scratch call(context);
        for(unsigned steps=0;steps<600 && remaining>0.;++steps){
            const double step=std::min(.1,remaining);call.invoke(presentation_tick,b.scene,float(step));remaining-=step;
        }
    }
    Binding after;if(!binding(after) || !(after==b)){error="Native boss HUD ownership changed during presentation";return false;}
    boss_clock={b,applied.epoch,frame,now,true};return true;
}
bool clear_native_boss_hud(PPCContext& context,std::string& error){
    using namespace native_boss_hud_detail;error.clear();applied={};boss_clock={};
    // A scene may already have freed/replaced its widget. No stale pointer is
    // dereferenced or native callback invoked unless the full ownership binds.
    Binding b;if(!binding(b))return true;
    if(!context_ok(context)){error="Native boss HUD cleanup context differs";return false;}
    if(mem_read32(active_global) || !(mem_read16(b.drawable+0x1E)&1)){
        Scratch call(context);call.invoke(active_setter,0);
    }
    if(mem_read32(active_global) || !(mem_read16(b.drawable+0x1E)&1)){error="Native boss HUD cleanup did not hide its drawable";return false;}
    return true;
}
void forget_native_boss_hud(){native_boss_hud_detail::applied={};native_boss_hud_detail::boss_clock={};}
}
