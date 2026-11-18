#pragma once
// Original PS2 Online/Player Setup geometry, built from original PAL widgets.
// The frontend owns the GUI and at least4096 retained guest scratch bytes.
// Call prepare for every page transition/frame, home_icon separately, then
// after_update AFTER the native GUI animation to gate our independent views.
// Call forget before GUI destruction. No native profile is created here.
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

namespace port::coop::online_setup_native {
inline constexpr uint16_t root_id=0x148,first_id=0x15d;
inline constexpr uint16_t character_id=0x162,name_id=0x163,host_name_id=0x164,
    details_id=0x165,preview_id=0x167,home_icon_id=0x168;
inline constexpr std::array<uint16_t,4> editable_ids{character_id,name_id,host_name_id,details_id};
inline constexpr unsigned scratch_bytes=4096,child_capacity=64,node_count=12;
// Descriptors0..1151; text slots1152..2687; child IDs3072..3327;
// Model/Name keys3328..3343; retained fallback icon alias3360..3391.
struct Nodes {
    std::array<uint32_t,node_count> all{};
    uint32_t character=0,name=0,host_name=0,details=0,preview=0,icon=0;
    uint32_t profile_id=~0u;
    std::string profile_name;
    explicit operator bool()const{return character && name && host_name && details && preview;}
};
struct Entry {uint32_t root=0,scratch=0,model=~0u,profile=~0u;Nodes nodes;
    std::array<uint32_t,node_count> bound{};bool busy=false,shown=false,icon_shown=false;};
inline std::map<uint32_t,Entry> entries;
inline bool readable(uint32_t p,unsigned bytes){return p>=0x80000000u && bytes<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-bytes;}
inline uint32_t invoke(PPCContext& base,uint32_t address,uint32_t a=0,uint32_t b=0,uint32_t d=0,uint32_t e=0,uint32_t f=0){
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);
    c.r[3]=a;c.r[4]=b;c.r[5]=d;c.r[6]=e;c.r[7]=f;ssne_call(c,address);return c.r[3];
}
struct Busy {Entry& entry;explicit Busy(Entry& e):entry(e){entry.busy=true;}~Busy(){entry.busy=false;}};
inline std::string guest_text(uint32_t p,unsigned maximum=128){
    std::string out;for(unsigned i=0;i<maximum && readable(p+i,1);++i){const auto c=mem_read8(p+i);if(!c)return out;out+=char(c);}return {};
}
inline void store_text(uint32_t p,const std::string& text,unsigned capacity=128){
    if(!capacity || !readable(p,capacity))return;
    const auto count=unsigned(text.size()<capacity?text.size():capacity-1);
    for(unsigned i=0;i<count;++i)mem_write8(p+i,uint8_t(text[i]));mem_write8(p+count,0);
}
inline void position(uint32_t node,float x,float y){
    if(!readable(node,0x20))return;
    const auto d=mem_read32(node+0x1c),params=mem_read32(node+0x14);
    if(!readable(d,0x34) || !readable(params,4))return;
    mem_write32(d+0x2c,f2u(x));mem_write32(d+0x30,f2u(y));
    const auto vector=mem_read32(params);
    // Native800FB728 allocates16bytes: current x/y followed by target x/y.
    if(readable(vector,16))for(unsigned i=0;i<4;++i)mem_write32(vector+i*4,f2u(i%2?y:x));
}
inline void set_text(PPCContext& c,uint32_t node,const std::string& text,uint32_t scratch){
    if(!readable(node,0xbc))return;
    const auto helper=mem_read32(node+0xb8);const auto desired=text.substr(0,127);
    if(readable(helper,12) && guest_text(mem_read32(helper+8))==desired)return;
    const auto vt=mem_read32(node+0x5c);if(!readable(vt,0x78))return;
    store_text(scratch,desired);invoke(c,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),scratch);
}
inline void text_scale(PPCContext& c,uint32_t node,float scale){
    if(!readable(node,0xbc))return;
    const auto helper=mem_read32(node+0xb8);if(!readable(helper,0x48))return;
    const auto fields=mem_read32(helper);if(!readable(fields,8) || mem_read16(fields)>1)return;
    const float actual=u2f(mem_read32(fields+4)),ascent=u2f(mem_read32(helper+0x18)),height=u2f(mem_read32(helper+0x1c));
    if(actual==scale || !std::isfinite(actual) || actual<=0 || !std::isfinite(ascent) || !std::isfinite(height))return;
    // The original Font1 ctor clamps sizes below .6; use the proven lobby
    // metric adjustment so the original PS2 .5 labels really render at .5.
    mem_write32(fields+4,f2u(scale));mem_write32(helper+0x18,f2u(ascent*scale/actual));mem_write32(helper+0x1c,f2u(height*scale/actual));
    const auto text=mem_read32(helper+8),vt=mem_read32(node+0x5c);
    if(readable(text,1) && readable(vt,0x78))invoke(c,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),text);
}
inline void visibility(PPCContext& c,uint32_t node,bool shown,bool selectable=false){
    if(!readable(node,0x20))return;const auto d=mem_read32(node+0x1c);if(!readable(d,16))return;
    const auto flags=mem_read32(d+0xc);const bool reopen=shown && (flags&0x20u);
    mem_write32(d+0xc,shown?(selectable?flags&~0x30u:(flags|0x10u)&~0x20u):flags|0x30u);
    if(reopen)invoke(c,0x800fa3cc,node);
}
inline uint32_t original(PPCContext& c,unsigned id,unsigned bytes,unsigned type){
    const auto tablep=c.r[13]-0x542cu;if(!readable(tablep,4))return 0;
    const auto table=mem_read32(tablep);if(!readable(table,(id+1)*4))return 0;
    const auto d=mem_read32(table+id*4);return readable(d,bytes) && mem_read16(d)==type && mem_read16(d+2)==id?d:0;
}
inline bool list_child(uint32_t root,uint32_t scratch,unsigned id){
    const auto d=mem_read32(root+0x1c);if(!readable(d,0x48))return false;
    const unsigned count=mem_read16(d+6);const auto ids=mem_read32(d+0x44);
    if(count>child_capacity || (count && !readable(ids,count*4)))return false;
    for(unsigned i=0;i<count;++i)if(mem_read32(ids+i*4)==id)return true;
    if(count==child_capacity)return false;
    for(unsigned i=0;i<count;++i)mem_write32(scratch+3072+i*4,mem_read32(ids+i*4));
    mem_write32(scratch+3072+count*4,id);mem_write32(d+0x44,scratch+3072);mem_write16(d+6,uint16_t(count+1));return true;
}
inline bool owns(uint32_t node,uint32_t gui,uint32_t root,unsigned id,unsigned type){
    if(!readable(node,0x60) || mem_read32(node+0x20)!=gui || mem_read32(node+0x24)!=root)return false;
    const auto d=mem_read32(node+0x1c);return readable(d,6) && mem_read16(d)==type && mem_read16(d+2)==id && mem_read16(d+4)==root_id;
}
inline uint32_t create(PPCContext& c,uint32_t gui,Entry& e,unsigned index,unsigned template_id,unsigned type,
                       float x,float y,bool selectable,float scale=1.f){
    const unsigned id=first_id+index,bytes=type==3?96:type==7?92:type==6?84:88;
    auto node=invoke(c,0x800f6030,gui,id);
    if(node)return owns(node,gui,e.root,id,type)?node:0;
    const auto source=original(c,template_id,bytes,type);if(!source)return 0;
    const auto d=e.scratch+index*96;
    for(unsigned i=0;i<bytes;++i)mem_write8(d+i,mem_read8(source+i));
    mem_write16(d+2,uint16_t(id));mem_write16(d+4,root_id);mem_write16(d+6,0);mem_write16(d+8,0);mem_write16(d+0xa,0xffff);
    mem_write32(d+0xc,(type==3?mem_read32(d+0xc)&0xa000u:0u)|(selectable?0u:0x10u));
    for(unsigned i=0;i<6;++i)mem_write32(d+0x10+i*4,selectable && i<2?0x5du+i:0u);
    mem_write32(d+0x28,4);mem_write32(d+0x2c,f2u(x));mem_write32(d+0x30,f2u(y));
    mem_write32(d+0x44,0);mem_write32(d+0x48,0);
    if(type==1){mem_write32(d+0x4c,0);mem_write32(d+0x50,index<5?0x10001u:1u);mem_write32(d+0x54,f2u(scale));}
    if(type==3){mem_write32(d+0x54,1);mem_write32(d+0x58,f2u(scale));}
    if(type==7){mem_write32(d+0x34,f2u(196));mem_write32(d+0x38,f2u(288));}
    // GC has no Online frame. The frontend may bind the validated original
    // PS2 image separately; this always-existing native frame is its fallback.
    if(type==6){store_text(e.scratch+3360,"FrontEndA:GTCooperative",32);mem_write32(d+0x4c,e.scratch+3360);mem_write32(d+0x34,f2u(128));mem_write32(d+0x38,f2u(128));}
    if(!list_child(e.root,e.scratch,id))return 0;
    node=invoke(c,0x800f5940,d,gui,e.root);if(!owns(node,gui,e.root,id,type))return 0;
    invoke(c,0x800fa3cc,node);position(node,x,y);return node;
}
inline Entry* entry(uint32_t gui,uint32_t root,uint32_t scratch){
    if(!readable(gui,0x88) || !readable(root,0x60) || !readable(scratch,scratch_bytes))return nullptr;
    const auto d=mem_read32(root+0x1c);if(!readable(d,6) || mem_read16(d+2)!=root_id)return nullptr;
    auto& e=entries[gui];if(e.root!=root || e.scratch!=scratch)e=Entry{};
    e.root=root;e.scratch=scratch;return &e;
}
struct Profile {uint32_t id=~0u,object=0,variable=0,model=~0u;std::string name;};
inline Profile profile(PPCContext& c,Entry& e){
    Profile out;out.id=invoke(c,0x800c25a4,0);if(out.id==~0u)out.id=0;
    out.object=invoke(c,0x800c23a8,out.id);if(!readable(out.object,4))return out;
    store_text(e.scratch+3328,"Model",8);store_text(e.scratch+3336,"Name",8);
    out.variable=invoke(c,0x80113728,out.object,e.scratch+3328,0);
    if(readable(out.variable,0x28))out.model=mem_read32(out.variable+0x24);else out.variable=0;
    const auto name=invoke(c,0x80113728,out.object,e.scratch+3336,0);
    if(readable(name,0x28))out.name=guest_text(invoke(c,0x80159f70,name+0x24));return out;
}
inline void preview(PPCContext& c,Entry& e,const Profile& p){
    const auto node=e.nodes.preview;if(!readable(node,0xc4) || !p.object)return;
    // Type7 construction deliberately leaves+C0 null. Original801081E4
    // creates that state lazily through800F1DF8(preview, model-enum ID).
    // The getter retains the native GUI/enum/preview ownership and callbacks.
    const auto state=invoke(c,0x800f1df8,node,character_id);if(!readable(state,20))return;
    const auto alias=invoke(c,0x800c2314,p.object);const auto desired=guest_text(alias,128);if(desired.empty())return;
    const auto current=mem_read32(state);
    if(readable(current,0x40) && guest_text(mem_read32(current+0x3c),128)==desired)return;
    invoke(c,0x8010a32c,state,alias);
}
inline void bind(PPCContext& c,uint32_t gui,Entry& e,unsigned index,uint32_t node,uint32_t callback){
    if(!node || e.bound[index]==node)return;
    const auto id=first_id+index;
    for(unsigned event:{12u,22u})invoke(c,0x800f6c14,gui,gui,id,event,callback);
    if(id==character_id)invoke(c,0x800f6c14,gui,gui,id,14,callback);
    e.bound[index]=node;
}
inline uint32_t owned_icon_sprite(uint32_t gui,const Entry& e){
    const auto node=e.nodes.icon;
    if(!owns(node,gui,e.root,home_icon_id,6) || !readable(node,0xb8))return 0;
    const auto sprite=mem_read32(node+0xb4);
    // CcSprite retains its owner's animation parameters. Its pool address
    // alone may already identify a new widget while the old root fades out.
    return readable(sprite,12) && mem_read32(sprite)==mem_read32(node+0x14)?sprite:0;
}
inline void after_update(PPCContext& c,uint32_t gui){
    const auto found=entries.find(gui);if(found==entries.end())return;auto& e=found->second;
    // Type7 owns an independent native camera; a hidden GUI label alone does
    // not disable its renderer. Touch only this verified widget's own view.
    if(owns(e.nodes.preview,gui,e.root,preview_id,7) && readable(e.nodes.preview,0xc0)){
        const auto draw=mem_read32(e.nodes.preview+0xbc),engine=mem_read32(0x802abaa0u);
        if(readable(draw,12) && readable(engine,4)){const auto view=mem_read32(draw+8);if(view<16)invoke(c,0x8011a240,engine,view,e.shown);}
    }
    // Likewise collapse only the Online icon's own quad when leaving Home.
    // Do not change its shared atlas alpha (the map/Ready images share it).
    if(!e.icon_shown){
        const auto sprite=owned_icon_sprite(gui,e),zero=e.scratch+3400;
        if(sprite){const auto quad=mem_read32(sprite+8);if(readable(quad,4)){
            const auto vt=mem_read32(quad);if(readable(vt,0x38)){mem_write32(zero,0);mem_write32(zero+4,0);invoke(c,mem_read32(vt+0x34),quad+int16_t(mem_read16(vt+0x30)),zero,zero);}
        }}
    }
}
inline void after_gui_update(PPCContext& c,uint32_t gui){
    const auto found=entries.find(gui);if(found==entries.end())return;
    // ClosePanel frees this widget tree while its CcGUI survives. Stop all
    // camera/quad updates the moment the original detaches the Online root.
    for(auto root=mem_read32(gui+0x6c);readable(root,0x60);root=mem_read32(root+8)){
        if(root==found->second.root){after_update(c,gui);return;}
    }
    entries.erase(found);
}
inline Nodes prepare(PPCContext& c,uint32_t gui,uint32_t root,uint32_t scratch,
                     const std::array<std::string,3>& labels,const std::array<std::string,3>& values,
                     bool shown,uint32_t callback=0x800ff15c){
    auto* found=entry(gui,root,scratch);if(!found || found->busy)return {};auto& e=*found;Busy guard(e);e.shown=shown;
    if(!shown){for(unsigned i=0;i<node_count-1;++i)visibility(c,e.nodes.all[i],false);after_update(c,gui);return e.nodes;}
    constexpr std::array<float,3> label_y{-110,-40,30};
    for(unsigned i=0;i<label_y.size();++i){const auto node=create(c,gui,e,i,0x6b,1,80,label_y[i],false,.5f);e.nodes.all[i]=node;
        visibility(c,node,true);position(node,80,label_y[i]);text_scale(c,node,.5f);set_text(c,node,labels[i],scratch+1152+i*128);}
    const auto old_character=e.nodes.character;
    e.nodes.character=e.nodes.all[5]=create(c,gui,e,5,0x102,3,80,-90,true,.75f);
    constexpr std::array<float,3> value_y{-18,52,120};
    for(unsigned i=0;i<3;++i){const auto node=create(c,gui,e,6+i,0x6b,1,80,value_y[i],true,.75f);e.nodes.all[6+i]=node;bind(c,gui,e,6+i,node,callback);
        visibility(c,node,true,true);position(node,80,value_y[i]);text_scale(c,node,.75f);}
    e.nodes.name=e.nodes.all[6];e.nodes.host_name=e.nodes.all[7];e.nodes.details=e.nodes.all[8];

    e.nodes.preview=e.nodes.all[10]=create(c,gui,e,10,0x105,7,-290,-96,false);
    if(!e.nodes)return e.nodes;
    const auto selected=profile(c,e);e.nodes.profile_id=selected.id;e.nodes.profile_name=selected.name;
    bind(c,gui,e,5,e.nodes.character,callback);
    visibility(c,e.nodes.character,true,true);position(e.nodes.character,80,-90);

    visibility(c,e.nodes.preview,true);position(e.nodes.preview,-290,-96);
    if(old_character!=e.nodes.character || !mem_read32(e.nodes.character+0xd4)){
        // Original8010151C accepts selector0: populate original catalog only.
        // Its final event14 is guarded against writing a transient index.
        invoke(c,0x8010151c,gui,character_id,0);e.model=~0u;
    }
    if(selected.variable && (e.profile!=selected.id || e.model!=selected.model)){
        const auto count=mem_read32(e.nodes.character+0xd4);
        if(selected.model<count){invoke(c,0x800f15d0,e.nodes.character,selected.model);e.model=selected.model;e.profile=selected.id;}
    }
    set_text(c,e.nodes.name,values[0].empty()?selected.name:values[0],scratch+1152+6*128);
    set_text(c,e.nodes.host_name,values[1],scratch+1152+7*128);set_text(c,e.nodes.details,values[2],scratch+1152+8*128);
    preview(c,e,selected);after_update(c,gui);return e.nodes;
}
inline bool changed_model(PPCContext& c,uint32_t gui){
    const auto found=entries.find(gui);if(found==entries.end() || found->second.busy || !found->second.shown)return false;
    auto& e=found->second;Busy guard(e);if(!owns(e.nodes.character,gui,e.root,character_id,3) || !readable(e.nodes.character,0xd8))return false;
    const auto count=mem_read32(e.nodes.character+0xd4),model=mem_read32(e.nodes.character+0xd0);if(!count || model>=count)return false;
    auto selected=profile(c,e);if(!selected.variable)return false;
    if(selected.model!=model)invoke(c,0x801155f0,selected.variable,model);
    selected.model=model;e.model=model;e.profile=selected.id;preview(c,e,selected);return true;
}
inline bool hidden_icon_sprite(uint32_t sprite){
    // For the existing native sprite-update hook: return before its original
    // alpha update when true, then after_update collapses only our own quad.
    // Hidden widgets otherwise write alpha zero into the shared native atlas.
    for(const auto& [gui,e]:entries)if(!e.icon_shown && sprite && owned_icon_sprite(gui,e)==sprite)return true;
    return false;
}
inline uint32_t home_icon(PPCContext& c,uint32_t gui,uint32_t root,uint32_t scratch,bool shown){
    auto* found=entry(gui,root,scratch);if(!found || found->busy)return 0;auto& e=*found;e.icon_shown=shown;
    if(shown)e.nodes.icon=e.nodes.all[11]=create(c,gui,e,11,0xad,6,-260,-50,false);
    visibility(c,e.nodes.icon,shown);if(shown)position(e.nodes.icon,-260,-50);after_update(c,gui);return e.nodes.icon;
}
inline void forget(uint32_t gui){entries.erase(gui);}
} // namespace port::coop::online_setup_native
