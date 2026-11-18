#pragma once
#include "native_main_exit.h"
#include <array>

namespace port::coop::main_online_native {
inline constexpr unsigned online_id=0x15c;
inline bool place_after(uint32_t root,uint32_t online,uint32_t after){
    using namespace main_exit_native;
    if(!readable(root,0x3c) || !readable(online,0x28) || !readable(after,0x28) || online==after)return false;
    // Native D-pad navigation uses next/previous siblings, including the
    // nonselectable footer. Validate the whole chain before changing links.
    std::array<uint32_t,64> seen{};unsigned count=0;uint32_t previous=0;
    bool has_online=false,has_after=false;
    for(auto node=mem_read32(root+0x34);node;node=mem_read32(node+8)){
        if(count==seen.size() || !readable(node,0x28) || mem_read32(node+0x24)!=root || mem_read32(node+0xc)!=previous)return false;
        for(unsigned i=0;i<count;++i)if(seen[i]==node)return false;
        seen[count++]=node;has_online|=node==online;has_after|=node==after;previous=node;
    }
    if(!has_online || !has_after || mem_read32(root+0x38)!=previous)return false;
    if(mem_read32(online+0xc)==after)return true;
    const auto old_previous=mem_read32(online+0xc),old_next=mem_read32(online+8),following=mem_read32(after+8);
    if(old_previous)mem_write32(old_previous+8,old_next);else mem_write32(root+0x34,old_next);
    if(old_next)mem_write32(old_next+0xc,old_previous);else mem_write32(root+0x38,old_previous);
    mem_write32(online+0xc,after);mem_write32(online+8,following);mem_write32(after+8,online);
    if(following)mem_write32(following+0xc,online);else mem_write32(root+0x38,online);
    return true;
}
inline bool ensure(PPCContext& ctx,uint32_t gui,uint32_t scratch,const std::string& label){
    using namespace main_exit_native;
    if(!readable(scratch,384))return false;
    // The PS2 main menu inserts Online after MultiPlayer, using the same native
    // type1 font, anchor and 35-unit pitch. All buffers belong to this GUI.
    const auto result=main_exit_native::ensure(ctx,gui,scratch,scratch+128,scratch+256,label,16,online_id,0x3f);
    if(!result.node)return false;
    const auto root=call(ctx,0x800f6030,gui,root_id),single=call(ctx,0x800f6030,gui,0x3e);
    if(!readable(root,0x60) || !readable(single,0x60))return false;
    if(!place_after(root,result.node,call(ctx,0x800f6030,gui,0x3f)))return false;
    const auto definition=mem_read32(root+0x1c),first=mem_read32(single+0x1c);
    if(!readable(definition,0x48) || !readable(first,0x34))return false;
    const float top=u2f(mem_read32(first+0x30));if(!std::isfinite(top))return false;
    constexpr std::array<unsigned,7> rows{0x3e,0x3f,online_id,0x40,0x41,0x42,exit_id};
    for(unsigned i=0;i<rows.size();++i){
        const auto node=call(ctx,0x800f6030,gui,rows[i]);if(!readable(node,0x60))return false;
        const auto d=mem_read32(node+0x1c);if(!readable(d,0x34))return false;
        const float x=u2f(mem_read32(d+0x2c)),y=top+35.f*float(i);
        mem_write32(d+0x30,f2u(y));
        const auto params=mem_read32(node+0x14);
        if(readable(params,4))if(const auto position=mem_read32(params);readable(position,16))
            for(unsigned axis=0;axis<4;++axis)mem_write32(position+axis*4,f2u(axis%2?y:x));
    }
    // Preserve the original footer and any unrelated native children. The
    // explicit list keeps preferred-child lookup in the visible row order.
    const auto count=mem_read16(definition+6);const auto children=mem_read32(definition+0x44);
    if(count>16 || !readable(children,count*4))return false;
    std::array<uint32_t,16> ids{};unsigned used=0;
    for(auto id:rows)ids[used++]=id;
    for(unsigned i=0;i<count;++i){
        const auto id=mem_read32(children+i*4);bool known=false;for(auto row:rows)known|=row==id;
        if(!known){if(used==ids.size())return false;ids[used++]=id;}
    }
    for(unsigned i=0;i<used;++i)mem_write32(scratch+256+i*4,ids[i]);
    mem_write32(definition+0x44,scratch+256);mem_write16(definition+6,uint16_t(used));
    return true;
}
}
