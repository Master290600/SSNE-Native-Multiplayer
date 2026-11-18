#pragma once
#include "native_configuration.h"
#include "native_main_online.h"
#include <array>
#include <map>
#include <string>

namespace port::coop::survival_setup_native {
inline constexpr unsigned root_id=0x64,difficulty_label=0x16a,difficulty_enum=0x16b;
struct Entry {uint32_t root=0,label=0,choice=0;bool active=false;std::array<uint32_t,4> flags{};};
inline std::map<uint32_t,Entry> entries;
inline uint32_t call(PPCContext& base,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t d=0,uint32_t e=0,uint32_t f=0){
    auto c=base;c.r[1]-=0x100;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);c.r[3]=a;c.r[4]=b;c.r[5]=d;c.r[6]=e;c.r[7]=f;ssne_call(c,fn);return c.r[3];
}
inline uint32_t item(PPCContext& c,uint32_t gui,unsigned id){return call(c,0x800f6030,gui,id);}
inline void visibility(PPCContext& c,uint32_t node,bool shown){
    if(!node)return;const auto desc=mem_read32(node+0x1c);const auto flags=mem_read32(desc+0xc);
    mem_write32(desc+0xc,shown?flags&~0x30u:flags|0x30u);
    if(shown && (flags&0x20u))call(c,0x800fa3cc,node);
}
inline void set_text(PPCContext& c,uint32_t node,uint32_t scratch,const std::string& text){
    if(!node)return;for(unsigned i=0;i<=text.size() && i<64;++i)mem_write8(scratch+i,i<text.size()?uint8_t(text[i]):0);
    const auto vt=mem_read32(node+0x5c);call(c,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),scratch);
}
// The owning GUI's existing Extra allocation reserves bytes3328..4095 here:
// label0..87, enum96..191, child IDs192..319, title scratch512..575.
inline bool prepare(PPCContext& c,uint32_t gui,uint32_t scratch,bool enabled,const std::string& title){
    using main_exit_native::readable;
    const auto root=item(c,gui,root_id);if(!readable(root,0x60) || !readable(scratch,768))return false;
    auto found=entries.find(gui);if(found!=entries.end() && found->second.root!=root)entries.erase(found);
    if(!enabled){
        found=entries.find(gui);if(found==entries.end())return true;auto& entry=found->second;
        if(entry.active){
            for(unsigned i=0;i<4;++i)if(const auto node=item(c,gui,0x6c+i)){const auto desc=mem_read32(node+0x1c);mem_write32(desc+0xc,(mem_read32(desc+0xc)&~0x30u)|entry.flags[i]);}
            if(const auto name=item(c,gui,0x69)){const auto desc=mem_read32(name+0x1c),vt=mem_read32(name+0x5c);call(c,mem_read32(vt+0x7c),name+int16_t(mem_read16(vt+0x78)),mem_read32(desc+0x4c));}
        }
        visibility(c,entry.label,false);visibility(c,entry.choice,false);entry.active=false;return true;
    }
    auto& entry=entries[gui];if(!entry.root)entry.root=root;
    if(!entry.active)for(unsigned i=0;i<4;++i)if(const auto node=item(c,gui,0x6c+i))entry.flags[i]=mem_read32(mem_read32(node+0x1c)+0xc)&0x30u;
    const auto definitions=mem_read32(c.r[13]-0x542c);
    for(unsigned i=0;i<2;++i){
        const auto id=i?difficulty_enum:difficulty_label,original=i?0x5fu:0x5eu;auto node=item(c,gui,id);
        if(!node){
            const auto source=mem_read32(definitions+original*4),desc=scratch+i*96;
            if(!readable(source,i?96:88))return false;
            for(unsigned k=0;k<(i?96u:88u);++k)mem_write8(desc+k,mem_read8(source+k));
            mem_write16(desc+2,id);mem_write16(desc+4,root_id);mem_write16(desc+6,0);mem_write16(desc+8,0);mem_write32(desc+0x44,0);mem_write32(desc+0x48,0);mem_write32(desc+0x30,f2u(40.f));
            node=call(c,0x800f5940,desc,gui,root);if(!node)return false;
            call(c,0x800fa3cc,node);
            if(i)call(c,0x800f6c14,gui,gui,id,14,0x800ff15c);
        }
        if(i)entry.choice=node;else entry.label=node;
        visibility(c,node,true);if(!i){const auto desc=mem_read32(node+0x1c);mem_write32(desc+0xc,mem_read32(desc+0xc)|0x10u);}
    }
    const auto desc=mem_read32(root+0x1c),children=mem_read32(desc+0x44);const auto count=mem_read16(desc+6);
    if(count>30 || !readable(children,count*4))return false;
    std::array<uint32_t,32> ids{};unsigned used=0;
    for(unsigned i=0;i<count;++i){const auto id=mem_read32(children+i*4);if(id!=difficulty_label && id!=difficulty_enum){ids[used++]=id;if(id==0x6b){ids[used++]=difficulty_label;ids[used++]=difficulty_enum;}}}
    if(!main_online_native::place_after(root,entry.label,item(c,gui,0x6b)) || !main_online_native::place_after(root,entry.choice,entry.label))return false;
    for(unsigned i=0;i<used;++i)mem_write32(scratch+192+i*4,ids[i]);mem_write32(desc+0x44,scratch+192);mem_write16(desc+6,uint16_t(used));
    for(unsigned i=0;i<4;++i)visibility(c,item(c,gui,0x6c+i),false);
    set_text(c,item(c,gui,0x69),scratch+512,title);
    const auto config=read_native_configuration(native_global_configuration(),"GameSetup.RulesCoop.Difficulty");
    if(const auto field=config.values.find("GameSetup.RulesCoop.Difficulty");field!=config.values.end())if(const auto value=std::get_if<int64_t>(&field->second);value && *value>=0 && *value<=3)call(c,0x800f15d0,entry.choice,unsigned(*value));
    entry.active=true;return true;
}
inline bool changed(PPCContext& c,uint32_t gui){
    const auto found=entries.find(gui);if(found==entries.end() || !found->second.active)return false;
    const auto node=item(c,gui,difficulty_enum);if(node!=found->second.choice || !node)return false;
    const auto value=mem_read32(node+0xd0);if(value>3)return false;
    std::string why;return write_native_configuration(c,native_global_configuration(),{{"GameSetup.RulesCoop.Difficulty",int64_t(value)}},"GameSetup",why);
}
inline void forget(uint32_t gui){entries.erase(gui);}
}
