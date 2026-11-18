#pragma once
// Native main-menu row only. Caller owns guest descriptor/text/child-ID buffers
// until original GUI destruction. This helper has no quit/shutdown side effect.
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <cmath>
#include <cstdint>
#include <string>

namespace port::coop::main_exit_native {
inline constexpr unsigned root_id=0x3d,credits_id=0x42,exit_id=0x15a;
inline bool readable(uint32_t p,unsigned bytes){return p>=0x80000000u && bytes<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-bytes;}
inline uint32_t call(PPCContext& base,uint32_t address,uint32_t a=0,uint32_t b=0,uint32_t d=0,uint32_t e=0,uint32_t f=0){
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);
    c.r[3]=a;c.r[4]=b;c.r[5]=d;c.r[6]=e;c.r[7]=f;ssne_call(c,address);return c.r[3];
}
inline bool current_text_matches(uint32_t node,const std::string& label){
    if(!readable(node,0xbc))return false;
    const auto helper=mem_read32(node+0xb8);if(!readable(helper,12))return false;
    const auto pointer=mem_read32(helper+8);if(!readable(pointer,unsigned(label.size()+1)))return false;
    for(unsigned i=0;i<label.size();++i)if(mem_read8(pointer+i)!=uint8_t(label[i]))return false;
    return mem_read8(pointer+unsigned(label.size()))==0;
}
struct Result {uint32_t node=0;bool created=false;};
inline Result ensure(PPCContext& ctx,uint32_t gui,uint32_t descriptor,uint32_t text_buffer,
                     uint32_t child_ids,const std::string& localized_label,unsigned child_capacity=16,
                     unsigned row_id=exit_id,unsigned after_id=credits_id){
    // Call only while native_root_id(gui)==3D. Do not create/open a148 overlay.
    Result result;
    if(localized_label.empty() || localized_label.size()>95 || !readable(gui,0x88) ||
       !readable(descriptor,88) || !readable(text_buffer,128) || child_capacity<7 || child_capacity>64 ||
       !readable(child_ids,child_capacity*4))return result;
    const auto root=call(ctx,0x800f6030,gui,root_id),credits=call(ctx,0x800f6030,gui,credits_id);
    if(!readable(root,0x60) || !readable(credits,0xbc))return result;
    const auto parent_definition=mem_read32(root+0x1c);if(!readable(parent_definition,0x48))return result;
    const unsigned count=mem_read16(parent_definition+6);const auto children=mem_read32(parent_definition+0x44);
    if(!count || count>child_capacity || !readable(children,count*4))return result;
    bool listed=false;for(unsigned i=0;i<count;++i)listed|=mem_read32(children+i*4)==row_id;
    if(!listed && count>=child_capacity)return result;
    result.node=call(ctx,0x800f6030,gui,row_id);
    if(!result.node){
        // Clone the ORIGINAL retained Credits template, not an active widget's
        // transient Open flags. Original type1 main rows use88-byte descriptors.
        const auto array_address=ctx.r[13]-0x542cu;if(!readable(array_address,4))return {};
        const auto definitions=mem_read32(array_address);if(!readable(definitions,(credits_id+1)*4))return {};
        const auto source=mem_read32(definitions+credits_id*4);
        if(!readable(source,88) || mem_read16(source)!=1 || mem_read16(source+2)!=credits_id || mem_read16(source+4)!=root_id)return {};
        for(unsigned i=0;i<88;++i)mem_write8(descriptor+i,mem_read8(source+i));
        mem_write16(descriptor+2,row_id);mem_write16(descriptor+4,root_id);mem_write16(descriptor+6,0);
        mem_write32(descriptor+0xc,mem_read32(descriptor+0xc)&~0x30u);
        // Native original rows: y=-70,-35,0,35,70. Preserve their35-unit pitch.
        const auto after=call(ctx,0x800f6030,gui,after_id);if(!readable(after,0x60))return {};
        const auto live_credits=mem_read32(after+0x1c);if(!readable(live_credits,88))return {};
        const float y=u2f(mem_read32(live_credits+0x30));float spacing=35.f;
        if(const auto prior=call(ctx,0x800f6030,gui,0x41)){
            const auto d=mem_read32(prior+0x1c);
            if(readable(d,0x34)){const float measured=y-u2f(mem_read32(d+0x30));if(std::isfinite(measured) && measured>=20.f && measured<=64.f)spacing=measured;}
        }
        if(!std::isfinite(y))return {};
        mem_write32(descriptor+0x30,f2u(y+spacing));
        mem_write32(descriptor+0x44,0);mem_write32(descriptor+0x48,0);mem_write32(descriptor+0x4c,0);
        // Keep anchor/x/font/alignment/scale, color and original confirm prompt.
        result.node=call(ctx,0x800f5940,descriptor,gui,root);if(!result.node)return {};
        call(ctx,0x800f6c14,gui,gui,row_id,22,0x800ff15c);
        call(ctx,0x800fa3cc,result.node);result.created=true;
    }
    if(!listed){
        // The ordinary factory links the child into the native sibling chain.
        // Also extend the LIVE root child-ID list, as existing prepare_pause()
        // does, retaining original Main rows and the nonselectable footer43.
        for(unsigned i=0;i<count;++i)mem_write32(child_ids+i*4,mem_read32(children+i*4));
        mem_write32(child_ids+count*4,row_id);mem_write32(parent_definition+0x44,child_ids);mem_write16(parent_definition+6,uint16_t(count+1));
    }
    const auto live=mem_read32(result.node+0x1c);if(!readable(live,16))return result;
    if(mem_read32(live+0xc)&0x20u)call(ctx,0x800fa3cc,result.node);
    mem_write32(live+0xc,mem_read32(live+0xc)&~0x30u);
    // Compare actual retained helper text, not merely scratch bytes. Native
    // reopening can rebuild the helper; a language change also updates here.
    if(!current_text_matches(result.node,localized_label)){
        for(unsigned i=0;i<localized_label.size();++i)mem_write8(text_buffer+i,uint8_t(localized_label[i]));
        mem_write8(text_buffer+unsigned(localized_label.size()),0);
        const auto vtable=mem_read32(result.node+0x5c);
        call(ctx,mem_read32(vtable+0x74),result.node+int16_t(mem_read16(vtable+0x70)),text_buffer);
    }
    return result;
}
} // namespace port::coop::main_exit_native
