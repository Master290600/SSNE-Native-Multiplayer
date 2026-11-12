#include "native_crosshair.h"
#include "ssne/custom_crosshair.h"
#include "ssne/native_party.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <vector>

namespace port { namespace {
constexpr uint32_t reticle_update_address=0x800bdc1c;
constexpr uint32_t material_address=0x8012680c;
constexpr uint32_t group_address=0x8011e5ac;
constexpr uint32_t material_return=0x8011e90c;
constexpr uint32_t player_class=0x802121e8;
constexpr uint32_t quad_class=0x8021cb58;
constexpr uint32_t group_class=0x8021cab0;
constexpr uint32_t material_class=0x8021d2a8;
PPCFunc original_update=nullptr,original_material=nullptr,original_group=nullptr;
std::atomic<bool> supported{false};
std::atomic<unsigned> language{0};
thread_local bool scope_on=false;
thread_local unsigned group_depth=0;

struct Reticle {
    uint32_t actor=0,hud=0,quad=0,material=0,parent=0;
    unsigned slot=0;
};
std::vector<Reticle> reticles; // guest thread only; no live guest pointers reach UI.
struct Trace {
    uint64_t updates=0,materials=0,quad_binds=0,matches=0,groups=0,hud_lists=0,frames=0;
    Reticle last_update;
    uint32_t last_quad=0,last_material=0;
} trace;

bool readable(uint32_t address,unsigned bytes) {
    return address>=0x80000000u&&bytes<=ssne::guest_ram_size()&&address<=ssne::guest_ram_end()-bytes;
}
uint32_t word(uint32_t address) { return readable(address,4)?mem_read32(address):0; }
void set_scope(bool on) {
    if(scope_on==on)return;
    ssne::gx_custom_crosshair_scope(on);
    scope_on=on;
}
void clear_scope() { set_scope(false); }
void render_scope(bool entering) {
    clear_scope();
    if(entering)++group_depth;
    else if(group_depth)--group_depth;
}
void observe_hud_list(bool entering) {
    // Network authority installs its own original group wrapper after boot.
    // Its narrow observer brackets only lists surviving the local-view filter;
    // offline retains render_group below. Neither hook chain is reinstalled.
    if(entering)++trace.hud_lists;
    render_scope(entering);
}

bool owned(const Reticle& r) {
    const auto capacity=ssne::native_party_capacity(),registry=ssne::native_party_registry();
    return capacity<=8&&r.slot<capacity&&readable(registry,capacity*4)&&
        word(registry+r.slot*4)==r.actor&&readable(r.actor,0x790)&&
        word(r.actor+0x48)==player_class&&word(r.actor+0x374)==r.hud;
}
bool live(const Reticle& r) {
    if(!owned(r)||!readable(r.hud,0x534)||word(r.hud+0x4ec)!=r.quad||
       word(r.hud+0x4e8)!=r.material||word(r.hud+0x14)!=r.parent||
       !readable(r.quad,0x58)||word(r.quad)!=quad_class||
       word(r.quad+8)!=r.material||word(r.quad+0x10)!=r.parent||
       !readable(r.material,0x10)||word(r.material)!=material_class||
       !readable(r.parent,0x44)||word(r.parent)!=group_class)return false;
    // A removed quad may retain its old type while freed storage is reused.
    // Require current ownership AND actual membership in this HUD's draw list.
    auto item=word(r.parent+0xc);
    for(unsigned n=0;item&&n<4096;++n) {
        if(item==r.quad)return true;
        if(!readable(item,0x14)||word(item+0x10)!=r.parent)return false;
        const auto next=word(item+0xc);if(next==item)return false;item=next;
    }
    return false;
}
void prune() { std::erase_if(reticles,[](const Reticle& r){return !live(r);}); }
void remember(uint32_t hud) {
    std::erase_if(reticles,[&](const Reticle& r){return r.hud==hud||!live(r);});
    if(!readable(hud,0x534))return;
    Reticle found;found.hud=hud;found.quad=word(hud+0x4ec);
    found.material=word(hud+0x4e8);found.parent=word(hud+0x14);trace.last_update=found;
    if(!found.quad)return;
    const auto capacity=ssne::native_party_capacity(),registry=ssne::native_party_registry();
    if(capacity>8||!readable(registry,capacity*4))return;
    for(unsigned slot=0;slot<capacity;++slot) {
        found.slot=slot;found.actor=word(registry+slot*4);
        if(word(found.actor+0x374)==hud)trace.last_update=found;
        if(live(found)){reticles.push_back(found);return;}
    }
}
void reticle_update(PPCContext& c) {
    ++trace.updates;const auto hud=c.r[3];ssne_call_original(c,reticle_update_address,original_update);
    remember(hud);
}
void material_setup(PPCContext& c) {
    ++trace.materials;
    if(group_depth&&c.lr==material_return) {
        ++trace.quad_binds;trace.last_quad=c.r[28];trace.last_material=c.r[3];
    }
    bool replace=false;
    // This is the original per-quad material bind, before GXBegin. The same
    // material is used by many HUD pieces; only the exact live reticle may tag.
    if(group_depth&&c.lr==material_return&&ssne::custom_crosshair_active()) {
        for(const auto& r:reticles)
            if(r.quad==c.r[28]&&r.material==c.r[3]&&live(r)){replace=true;break;}
    }
    if(replace)++trace.matches;
    set_scope(replace);
    ssne_call_original(c,material_address,original_material);
}
void render_group(PPCContext& c) {
    ++trace.groups;
    struct Scope {
        Scope(){render_scope(true);}
        ~Scope(){render_scope(false);}
    } scope;
    ssne_call_original(c,group_address,original_group);
}
uint64_t hash_code(uint32_t address,unsigned size) {
    uint64_t hash=14695981039346656037ull;
    for(unsigned i=0;i<size;++i)hash=(hash^mem_read8(address+i))*1099511628211ull;
    return hash;
}
void write_trace() {
    static const bool enabled=[] {
        const auto hidden=std::getenv("SSNE_TEST_HIDDEN");
        return hidden&&std::strcmp(hidden,"1")==0&&std::getenv("SSNE_TEST_CROSSHAIR");
    }();
    if(!enabled)return;
    ++trace.frames;
    static auto next=std::chrono::steady_clock::time_point{};
    const auto now=std::chrono::steady_clock::now();if(now<next)return;
    next=now+std::chrono::milliseconds(250);
    const auto& r=trace.last_update;
    std::ostringstream out;out<<std::boolalpha;
    out<<"{\"supported\":"<<native_crosshair_supported()<<",\"frame\":"<<trace.frames
       <<",\"updates\":"<<trace.updates<<",\"material_calls\":"<<trace.materials
       <<",\"quad_binds\":"<<trace.quad_binds<<",\"matches\":"<<trace.matches
       <<",\"groups\":"<<trace.groups<<",\"hud_lists\":"<<trace.hud_lists<<",\"tracked_count\":"<<reticles.size()
       <<",\"registry\":"<<ssne::native_party_registry()<<",\"capacity\":"<<ssne::native_party_capacity()
       <<",\"last_bind\":{\"quad\":"<<trace.last_quad<<",\"material\":"<<trace.last_material<<"}"
       <<",\"last_update\":{\"actor\":"<<r.actor<<",\"hud\":"<<r.hud
       <<",\"quad\":"<<r.quad<<",\"material\":"<<r.material<<",\"parent\":"<<r.parent
       <<",\"slot\":"<<r.slot<<",\"owner_valid\":"<<owned(r)
       <<",\"hud_quad_valid\":"<<(r.quad&&word(r.hud+0x4ec)==r.quad)
       <<",\"hud_material_valid\":"<<(r.material&&word(r.hud+0x4e8)==r.material)
       <<",\"hud_parent_valid\":"<<(r.parent&&word(r.hud+0x14)==r.parent)
       <<",\"quad_type_valid\":"<<(word(r.quad)==quad_class)
       <<",\"quad_material_valid\":"<<(r.material&&word(r.quad+8)==r.material)
       <<",\"quad_parent_valid\":"<<(r.parent&&word(r.quad+0x10)==r.parent)
       <<",\"material_type_valid\":"<<(word(r.material)==material_class)
       <<",\"parent_type_valid\":"<<(word(r.parent)==group_class)
       <<",\"valid\":"<<live(r)<<"},\"tracked\":[";
    bool first=true;
    for(const auto& item:reticles) {
        if(!first)out<<',';first=false;
        out<<"{\"actor\":"<<item.actor<<",\"hud\":"<<item.hud<<",\"quad\":"<<item.quad
           <<",\"material\":"<<item.material<<",\"parent\":"<<item.parent<<",\"slot\":"<<item.slot<<'}';
    }
    out<<"]}";
    std::ofstream file("native-crosshair-state.json.pending",std::ios::binary|std::ios::trunc);file<<out.str();file.close();
    if(file){std::remove("native-crosshair-state.json");std::rename("native-crosshair-state.json.pending","native-crosshair-state.json");}
}
struct Hook {uint32_t address;unsigned bytes;uint64_t hash;PPCFunc callback;PPCFunc* original;};
} // namespace

bool install_native_crosshair() {
    if(supported.load(std::memory_order_acquire))return true;
    if(word(0x80000000u)!=0x47334250u)return false;
    const std::array hooks={
        Hook{reticle_update_address,0x20c,0x8cf1cf42d35f525full,reticle_update,&original_update},
        Hook{material_address,0x700,0xbc9108d7d5adddb0ull,material_setup,&original_material},
        Hook{group_address,0x1f54,0xbe04d740e89e1ddeull,render_group,&original_group}};
    for(const auto& hook:hooks)
        if(!ssne_find_func(hook.address)||!readable(hook.address,hook.bytes)||
           hash_code(hook.address,hook.bytes)!=hook.hash)return false;
    // Keep pre-existing functional hooks: our original call bypasses only our
    // own first entry, preserving the normal nested/recursive call behavior.
    for(const auto& hook:hooks) {
        auto previous=*ssne_hook_slot(hook.address);
        *hook.original=previous?previous:ssne_find_func(hook.address)->func;
    }
    for(const auto& hook:hooks)ssne_install_hook(hook.address,hook.callback);
    reticles.clear();clear_scope();ssne::set_native_hud_list_observer(observe_hud_list);
    supported.store(true,std::memory_order_release);
    native_crosshair_frame();return true;
}
bool native_crosshair_supported() { return supported.load(std::memory_order_acquire); }
unsigned native_crosshair_language() { return language.load(std::memory_order_relaxed); }
void native_crosshair_frame() {
    const auto value=word(0x802db838u);language.store(value<3?value:0,std::memory_order_relaxed);
    if(!native_crosshair_supported()){reticles.clear();clear_scope();write_trace();return;}
    prune();if(reticles.empty())clear_scope();write_trace();
}
} // namespace port
