#pragma once
// Direct original-style model enum on AF. Caller supplies at least256 guest
// bytes at Storage.extra+2560 and calls forget(gui) before GUI destruction.
// Descriptor0..95, "Model"96..101, retained AF child-ID list128..191.
// No profile creation, catalog extension, save-capacity change or page switch.
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <cstdint>
#include <map>

namespace port::coop::profile_skin_native {
inline constexpr uint16_t root_id=0xaf,profiles_id=0xb1,name_id=0xb5,enum_id=0x15b;
inline constexpr unsigned scratch_bytes=256,child_capacity=16;
struct Entry {uint32_t node=0,scratch=0,profile=~0u,model=~0u;bool busy=false,bound=false;};
inline std::map<uint32_t,Entry> entries;
inline bool readable(uint32_t p,unsigned bytes){return p>=0x80000000u && bytes<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-bytes;}
inline uint32_t invoke(PPCContext& base,uint32_t address,uint32_t a=0,uint32_t b=0,uint32_t d=0,uint32_t e=0,uint32_t f=0){
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);
    c.r[3]=a;c.r[4]=b;c.r[5]=d;c.r[6]=e;c.r[7]=f;ssne_call(c,address);return c.r[3];
}
// In the AF panel the portrait (B4) follows the highlighted profile, while
// the full-body view (B6) initially has a separate, fixed "Sam" instance.
// The latter belongs to the profile screen, whose tick animates it and whose
// close handler destroys it. Rebind that instance to the portrait's catalog
// alias, using a separate model in the full-body viewport.
inline void sync_body(PPCContext& ctx,uint32_t owner) {
    if(!readable(owner,0x5c))return;
    const auto gui=mem_read32(owner+0x58),current=mem_read32(owner+0x1c);
    if(!readable(gui,0x88) || !readable(current,0x40))return;
    const auto portrait=invoke(ctx,0x800f6030,gui,0xb4);
    if(!readable(portrait,0xc4))return;
    const auto state=mem_read32(portrait+0xc0);
    if(!readable(state,0x14))return;
    const auto instance=mem_read32(state);
    if(!readable(instance,0x40))return;
    // Both wrappers retain the catalog's string pointer for their lifetime.
    const auto alias=mem_read32(instance+0x3c);
    if(alias==mem_read32(current+0x3c) || !readable(alias,1) || !mem_read8(alias))return;
    const auto body=invoke(ctx,0x800f6030,gui,0xb6);
    if(!readable(body,0xc0) || !readable(mem_read32(body+0xbc),12))return;
    const auto next=invoke(ctx,0x800f1dd4,body,alias);
    if(!readable(next,0x40))return;
    // Retain the animated position and rotation, including the original
    // orientation initialized by the screen's Sam constructor.
    for(unsigned offset=4;offset<=0x18;offset+=4)
        mem_write32(next+offset,mem_read32(current+offset));
    mem_write32(owner+0x1c,next);
    invoke(ctx,0x800f373c,current,3);
}
struct Busy {Entry& entry;explicit Busy(Entry& value):entry(value){entry.busy=true;}~Busy(){entry.busy=false;}};
struct Selection {uint32_t id=~0u,variable=0,model=~0u;};
inline Selection selection(PPCContext& ctx,uint32_t gui,uint32_t scratch){
    Selection result;
    const auto profiles=invoke(ctx,0x800f6030,gui,profiles_id);
    if(!readable(profiles,0xd8) || !mem_read32(profiles+0xd4))return result;
    const auto row=invoke(ctx,0x800f1bd0,profiles,~0u);
    if(!readable(row,0x14))return result;
    result.id=mem_read32(row+0x10);
    const auto profile=invoke(ctx,0x800c23a8,result.id);
    if(!readable(profile,4))return {};
    constexpr char key[]="Model";
    for(unsigned i=0;i<sizeof(key);++i)mem_write8(scratch+96+i,uint8_t(key[i]));
    result.variable=invoke(ctx,0x80113728,profile,scratch+96,0);
    if(!readable(result.variable,0x28))return {};
    result.model=mem_read32(result.variable+0x24);return result;
}
inline bool list_child(uint32_t root,uint32_t scratch){
    const auto descriptor=mem_read32(root+0x1c);
    if(!readable(descriptor,0x48))return false;
    const auto count=mem_read16(descriptor+6);const auto children=mem_read32(descriptor+0x44);
    if(!count || count>child_capacity || !readable(children,count*4))return false;
    for(unsigned i=0;i<count;++i)if(mem_read32(children+i*4)==enum_id)return true;
    if(count==child_capacity)return false;
    for(unsigned i=0;i<count;++i)mem_write32(scratch+128+i*4,mem_read32(children+i*4));
    mem_write32(scratch+128+count*4,enum_id);
    mem_write32(descriptor+0x44,scratch+128);mem_write16(descriptor+6,uint16_t(count+1));return true;
}
inline bool prepare(PPCContext& ctx,uint32_t gui,uint32_t scratch){
    // The caller gates this helper to the currently displayed native rootAF.
    if(!readable(gui,0x88) || !readable(scratch,scratch_bytes))return false;
    auto& entry=entries[gui];if(entry.busy)return false;
    Busy guard(entry);
    const auto root=invoke(ctx,0x800f6030,gui,root_id);if(!readable(root,0x60))return false;
    const auto selected=selection(ctx,gui,scratch);if(!selected.variable)return false;
    const auto profiles=invoke(ctx,0x800f6030,gui,profiles_id);
    if(!readable(profiles,0x20))return false;
    const auto profile_definition=mem_read32(profiles+0x1c);
    if(!readable(profile_definition,0x28))return false;
    auto node=invoke(ctx,0x800f6030,gui,enum_id);bool created=false;
    if(!node){
        const auto table_address=ctx.r[13]-0x542cu;if(!readable(table_address,4))return false;
        const auto table=mem_read32(table_address);if(!readable(table,4*(0x102+1)))return false;
        const auto source=mem_read32(table+0x102*4);
        // Game.Gui type3 is96bytes. Its scale is+58, unlike type1's+54.
        if(!readable(source,96) || mem_read16(source)!=3 || mem_read16(source+2)!=0x102)return false;
        for(unsigned i=0;i<96;++i)mem_write8(scratch+i,mem_read8(source+i));
        mem_write16(scratch+2,enum_id);mem_write16(scratch+4,root_id);mem_write16(scratch+6,0);mem_write16(scratch+8,0);
        mem_write32(scratch+0xc,mem_read32(scratch+0xc)&~0x30u);
        for(unsigned i=0;i<6;++i)mem_write32(scratch+0x10+i*4,mem_read32(profile_definition+0x10+i*4));
        mem_write32(scratch+0x2c,f2u(144.f));mem_write32(scratch+0x30,f2u(112.f));
        mem_write32(scratch+0x44,0);mem_write32(scratch+0x48,0);mem_write32(scratch+0x58,f2u(.6f));
        node=invoke(ctx,0x800f5940,scratch,gui,root);created=true;
    }
    if(!readable(node,0xd8))return false;
    const auto live=mem_read32(node+0x1c);
    if(!readable(live,96) || mem_read16(live)!=3 || !list_child(root,scratch))return false;
    if(entry.node!=node || entry.scratch!=scratch){entry.node=node;entry.scratch=scratch;entry.profile=~0u;entry.model=~0u;entry.bound=false;}
    // Preserve the original live profile's Select/Back/Add/Delete prompts,
    // including eligibility changes when another existing profile is selected.
    for(unsigned i=0;i<6;++i)mem_write32(live+0x10+i*4,mem_read32(profile_definition+0x10+i*4));
    if(!entry.bound){
        // Root's existing FF15C hook handles only our15B before its original
        // mode-selection fallback. Model population itself emits event14.
        for(unsigned event:{14u,22u,24u,25u,26u})invoke(ctx,0x800f6c14,gui,gui,enum_id,event,0x800ff15c);
        entry.bound=true;
    }
    const bool hidden=(mem_read32(live+0xc)&0x30u)!=0;
    mem_write32(live+0xc,mem_read32(live+0xc)&~0x30u);
    if(created || hidden)invoke(ctx,0x800fa3cc,node);
    if(entry.profile!=selected.id || entry.model!=selected.model || !mem_read32(node+0xd4)){
        // Original801081E4 uses exactly this catalog/profile selector contract.
        invoke(ctx,0x8010151c,gui,enum_id,profiles_id);
        if(!mem_read32(node+0xd4))return false;
        entry.profile=selected.id;entry.model=selected.model;
    }
    if(const auto name=invoke(ctx,0x800f6030,gui,name_id)){
        const auto d=mem_read32(name+0x1c);if(readable(d,16))mem_write32(d+0xc,mem_read32(d+0xc)|0x20u);
    }
    return true;
}
inline bool changed(PPCContext& ctx,uint32_t gui){
    const auto found=entries.find(gui);if(found==entries.end() || found->second.busy)return false;
    auto& entry=found->second;if(!readable(entry.node,0xd8) || !readable(entry.scratch,scratch_bytes))return false;
    Busy guard(entry);const auto selected=selection(ctx,gui,entry.scratch);if(!selected.variable)return false;
    const auto index=mem_read32(entry.node+0xd0),count=mem_read32(entry.node+0xd4);
    if(!count || index>=count)return false;
    // Original801082B4: only the selected existing profile's Model changes.
    if(index!=selected.model)invoke(ctx,0x801155f0,selected.variable,index);
    entry.profile=selected.id;entry.model=index;
    // Original character confirmation sends B1 event14 to refresh its model.
    // Rebuilding the profile list does not emit that event or change a preview.
    invoke(ctx,0x800f6e3c,gui,profiles_id,14);return true;
}
inline void forget(uint32_t gui){entries.erase(gui);}
} // namespace port::coop::profile_skin_native
