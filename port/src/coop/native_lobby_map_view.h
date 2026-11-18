#pragma once
// Original PAL map metadata/image contracts. The owning lobby allocates all
// guest descriptors and scratch; native GUI destruction owns each image.
#include "custom_levels_runtime.h"
#include "custom_level_menu.h"
#include "native_frontend.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace port::coop::lobby_map_native {
inline bool readable(uint32_t p,unsigned bytes){
    return p>=0x80000000u && bytes<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-bytes;
}
inline uint32_t call(PPCContext& base,uint32_t address,uint32_t a=0,uint32_t b=0,uint32_t d=0){
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);
    c.r[3]=a;c.r[4]=b;c.r[5]=d;ssne_call(c,address);return c.r[3];
}
inline std::string text(uint32_t p,unsigned maximum=256){
    std::string result;
    for(unsigned i=0;i<maximum && readable(p+i,1);++i){const auto c=mem_read8(p+i);if(!c)return result;result+=char(c);}
    return {};
}
inline bool alias_text(uint32_t scratch,unsigned capacity,const std::string& alias){
    if(alias.empty() || alias.size()>63 || alias.size()+1>capacity || !readable(scratch,capacity))return false;
    for(unsigned i=0;i<alias.size();++i)mem_write8(scratch+i,uint8_t(alias[i]));
    mem_write8(scratch+unsigned(alias.size()),0);return true;
}
inline std::string pal_text(const std::string& utf8){
    // Same conversion as the existing Custom Levels carousel: native PAL fonts
    // consume Latin1, while package titles are UTF8. Unsupported codepoints use ?.
    std::string result;
    for(size_t i=0;i<utf8.size() && result.size()<100;){const auto c=uint8_t(utf8[i++]);
        if(c<128)result+=char(c);
        else if(c>=0xc2 && c<=0xc3 && i<utf8.size()){const auto next=uint8_t(utf8[i++]);result+=next>=0x80 && next<=0xbf?char((unsigned(c&31)<<6)|(next&63)):'?';}
        else{while(i<utf8.size() && (uint8_t(utf8[i])&0xc0)==0x80)++i;result+='?';}}
    return result;
}
inline std::string localized(PPCContext& ctx,uint32_t id){
    // Native80159AE4 uses this same count;80159AB4 itself has no bounds check.
    const auto count_address=ctx.r[13]-0x543cu;
    if(!readable(count_address,4))return {};
    const auto count=mem_read32(count_address);if(!count || count>65536 || id>=count)return {};
    return text(call(ctx,0x80159ab4,id));
}
struct Metadata {bool valid=false,custom=false;uint32_t record=0,region_id=0,title_id=0;std::string region,title;};
inline Metadata metadata(PPCContext& ctx,const std::string& alias,unsigned level_set,uint32_t scratch,unsigned capacity=96){
    Metadata out;if(level_set>2 || !alias_text(scratch,capacity,alias))return out;
    // Use the installed hook chain. Custom aliases resolve their immutable base
    // record even when the carousel has already closed/restored its tables.
    out.record=call(ctx,0x8010c858,scratch,level_set);
    if(!readable(out.record,12))return out;
    out.region_id=mem_read32(out.record);out.title_id=mem_read32(out.record+4);
    out.region=localized(ctx,out.region_id);out.title=localized(ctx,out.title_id);
    if(const auto* custom=find_custom_level(alias)){out.custom=true;out.title=pal_text(custom->title);out.region=custom_level_menu_category(*custom,native_menu_language());}
    out.valid=!out.title.empty();return out;
}
inline uint32_t create_thumbnail(PPCContext& ctx,uint32_t gui,uint32_t descriptor,unsigned id,
                                 float left=178.f,float top=-122.f,float width=64.f,float height=64.f){
    if(!readable(gui,0x88) || !readable(descriptor,84) || id>0xffff || !(width>0) || !(height>0))return 0;
    if(const auto old=call(ctx,0x800f6030,gui,id))return old;
    const auto root=call(ctx,0x800f6030,gui,0x9a);if(!root)return 0;
    const auto descriptors_address=ctx.r[13]-0x542cu;if(!readable(descriptors_address,4))return 0;
    const auto descriptors=mem_read32(descriptors_address);if(!readable(descriptors,4*(0xad+1)))return 0;
    // Original AD supports an initially empty image. The controller images
    // 9F/A6 default to PadGfx and would enter controller-sprite suppression.
    const auto source=mem_read32(descriptors+0xad*4);
    if(!readable(source,84) || mem_read16(source)!=6 || mem_read16(source+2)!=0xad)return 0;
    for(unsigned i=0;i<84;++i)mem_write8(descriptor+i,mem_read8(source+i));
    mem_write16(descriptor+2,uint16_t(id));mem_write16(descriptor+4,0x9a);mem_write16(descriptor+6,0);
    mem_write32(descriptor+0xc,(mem_read32(descriptor+0xc)|0x10u)&~0x20u);
    mem_write32(descriptor+0x28,4);mem_write32(descriptor+0x2c,f2u(left));mem_write32(descriptor+0x30,f2u(top));
    mem_write32(descriptor+0x34,f2u(width));mem_write32(descriptor+0x38,f2u(height));
    mem_write32(descriptor+0x44,0);mem_write32(descriptor+0x48,0);
    // Keep original AD image-resource descriptor+4c/+50 intact. Factory copies
    // all84bytes; native opening constructs node+B4, and GUI destruction owns it.
    const auto node=call(ctx,0x800f5940,descriptor,gui,root);
    if(node)call(ctx,0x800fa3cc,node);return node;
}
inline bool show_thumbnail(PPCContext& ctx,uint32_t node,const std::string& alias,uint32_t scratch,unsigned capacity=96){
    if(!readable(node,0xb8) || !alias_text(scratch,capacity,alias))return false;
    const auto sprite=mem_read32(node+0xb4),atlas_address=ctx.r[13]-0x7658u;
    if(!readable(sprite,24) || !readable(atlas_address,4))return false;
    const auto atlas=mem_read32(atlas_address);if(!readable(atlas,4))return false;
    // Exact original800FF16C image update. Never bypass custom image_hook:
    // r5 remains the alias, allowing package preview PNG and base-map fallback.
    call(ctx,0x800f306c,sprite,atlas,scratch);return true;
}
} // namespace port::coop::lobby_map_native
