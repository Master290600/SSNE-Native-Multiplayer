#pragma once
#include "world_state.h"
#include "ssne/runtime.h"
#include <optional>
#include <cmath>
namespace port::coop {
struct NativeRenderBinding {uint32_t model=0,matrix=0;};
// Embedded original flash owners, never foreign process pointers. Original PAL
// helpers: pistol5672C, Uzi61A48, shotgun5F1A4, Minigun584F4, sniper60684.
// Minigun has a second radial flash; Sirian's beam has three radial owners.
inline unsigned native_weapon_flash_offset(unsigned weapon,unsigned effect=0){
    if(weapon==9)return effect<3?0x24Cu+effect*0x14u:0u;
    if(effect)return effect==1 && weapon==4?0x250u:0u;
    return weapon==1?0x22Cu:weapon==2?0x228u:weapon==3?0x238u:weapon==4?0x23Cu:weapon==8?0x234u:0u;
}
inline bool native_weapon_flash_radial(unsigned weapon,unsigned effect=0){return weapon==3 || weapon==9 || (weapon==4 && effect==1);}
// Sirian's three continuous flashes are explicitly removed by59F8C/ACFC.
// Their owner+8 is never initialized by9A728/9A8CC or ticked by9ACF8.
inline bool native_weapon_flash_timed(unsigned weapon){return weapon!=9;}
inline std::string native_weapon_flash_key(unsigned hand,unsigned effect){return "weapon_model"+std::to_string(hand)+(effect==2?"_flash_aux2":effect==1?"_flash_aux":"_flash");}
inline bool presentation_readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;}
// Radial sprites store their size/angle on the native shared resource, outside
// the proxy matrix. PAL8009A8CC calls these setters on every flash creation.
inline uint32_t native_radial_flash_resource(unsigned index){
    if(index>=2)return 0;const auto resource=mem_read32(0x802D8ED4u+index*4);
    if(!presentation_readable(resource,0x24))return 0;const auto table=mem_read32(resource);
    if(!presentation_readable(table,0x50) || mem_read32(table+0x44)!=0x8012BB30u || mem_read32(table+0x4C)!=0x8012BB48u)return 0;
    return resource;
}
// The renderer consumes this proxy matrix, not the gameplay position at +20.
// PAL model matrix getter 8013E954 returns proxy+4; proxy+2D is visibility (setter 8013E7E8); +24 is the render category.
inline std::optional<NativeRenderBinding> native_render_binding(uint32_t object,EntityKind kind,unsigned index=0){
    const bool corpse=kind==EntityKind::Physics && presentation_readable(object,0x188) && mem_read32(object+0x48)==0x8020C6B8u;
    const bool piece=kind==EntityKind::Physics && presentation_readable(object,0x130) && mem_read32(object+0x48)==0x8020B990u;
    const bool rotor=kind==EntityKind::Physics && presentation_readable(object,0x118) && mem_read32(object+0x48)==0x8020B228u;
    const unsigned offset=kind==EntityKind::Player?0x148:kind==EntityKind::Pickup || corpse || rotor || piece?0xCC:0xFC;
    if((kind!=EntityKind::Pickup && kind!=EntityKind::Enemy && kind!=EntityKind::Player && !corpse && !rotor && !piece) || !presentation_readable(object,offset+4))return {};
    auto model=mem_read32(object+offset);
    if(kind==EntityKind::Player){
        if(index>=4)return {};
        // The original vehicle controller deliberately releases the walking
        // body. This is an absent renderer, not a corrupt player snapshot.
        if(!model && presentation_readable(object,0x6C8) && mem_read32(object+0x340)==3 && mem_read32(object+0x6C4))return NativeRenderBinding{};
        if(!presentation_readable(model,0x20))return {};model=mem_read32(model+index*4);
    }
    else if(index)return {};
    if(!model)return NativeRenderBinding{};
    if(!presentation_readable(model,0x38))return {};
    const auto table=mem_read32(model),matrix=mem_read32(model+4);
    if(!presentation_readable(table,0x30) || mem_read32(table+0x2C)!=0x8013E954u || !presentation_readable(matrix,48) || mem_read8(model+0x2D)>1)return {};
    return NativeRenderBinding{model,matrix};
}
// Pickup glow is a separate native effect/proxy. PAL 800E2DD8 and
// 800E2EA8 forward its view mask and visibility to effect+20.
inline std::optional<uint32_t> native_pickup_glow_proxy(uint32_t object){
    if(!presentation_readable(object,0x18C))return {};
    const auto effect=mem_read32(object+0x188);if(!effect)return 0u;
    if(!presentation_readable(effect,0x24))return {};
    const auto table=mem_read32(effect+0x1C);
    if(!presentation_readable(table,0x88) || mem_read32(table+0x7C)!=0x800E2DD8u || mem_read32(table+0x84)!=0x800E2EA8u)return {};
    const auto proxy=mem_read32(effect+0x20);if(!proxy)return 0u;
    if(!presentation_readable(proxy,0x38))return {};
    const auto vt=mem_read32(proxy);
    if(!presentation_readable(vt,0xD8) || mem_read32(vt+0xD4)!=0x8013E814u || mem_read8(proxy+0x2D)>1)return {};
    return proxy;
}
// The same affine getter as a model proxy (PAL800E2E18/800E301C).
inline std::optional<NativeRenderBinding> native_pickup_glow_binding(uint32_t object){
    const auto proxy=native_pickup_glow_proxy(object);if(!proxy)return {};if(!*proxy)return NativeRenderBinding{};
    const auto table=mem_read32(*proxy),matrix=mem_read32(*proxy+4);
    if(mem_read32(table+0x2C)!=0x8013E954u || !presentation_readable(matrix,48))return {};
    return NativeRenderBinding{*proxy,matrix};
}
inline bool validate_pickup_glow_metadata(const EntityState& e,std::string& error){
    const auto found=e.properties.find("pickup_glow_pose");if(found==e.properties.end())return true;
    const auto* pose=std::get_if<std::vector<float>>(&found->second);
    if(e.kind!=EntityKind::Pickup || !pose || (pose->size()!=0 && pose->size()!=12)){error="Invalid pickup glow pose schema";return false;}
    for(auto v:*pose)if(!std::isfinite(v) || std::abs(v)>1000000.f){error="Invalid pickup glow coefficient";return false;}
    if(pose->empty()){
        const auto state=e.properties.find("pickup_glow");const auto* flags=state==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&state->second);
        // Original global treasure collection retires the host glow, while its
        // level-owned pickup can remain in the actor list for another frame.
        // A retained client proxy is equivalent only when completely hidden.
        if(!flags || *flags!=std::vector<int64_t>{0,0}){error="Absent pickup glow is still visible or has a view mask";return false;}
    }return true;
}
inline std::optional<NativeRenderBinding> native_minigun_barrel(uint32_t actor,unsigned hand){
    if(hand>=2 || !presentation_readable(actor,0x790))return {};
    const auto instance=mem_read32(actor+0x4BC+4*24+hand*4);
    if(!instance)return NativeRenderBinding{};
    if(!presentation_readable(instance,0x294) || mem_read32(instance+0xC)!=0x801FB598u)return {};
    const auto proxy=mem_read32(instance+0x268);if(!proxy)return NativeRenderBinding{};
    if(!presentation_readable(proxy,0x38))return {};
    const auto table=mem_read32(proxy),matrix=mem_read32(proxy+4);
    if(!presentation_readable(table,0x30) || mem_read32(table+0x2C)!=0x8013E954u || !presentation_readable(matrix,48) || mem_read8(proxy+0x2D)>1)return {};
    return NativeRenderBinding{proxy,matrix};
}
inline bool native_marker_class(uint32_t table){return table==0x802054A0u || table==0x802027C8u;}
inline bool render_field(const std::string& key){return key=="render_visible" || key=="render_row0" || key=="render_row1" || key=="render_row2";}
inline bool validate_render_metadata(const EntityState& e,std::string& error){
    const auto field=e.properties.find("render_visible");unsigned rows=0;
    for(unsigned row=0;row<3;++row)if(auto it=e.properties.find("render_row"+std::to_string(row));it!=e.properties.end()){
        ++rows;auto* values=std::get_if<std::array<float,4>>(&it->second);if(!values){error="Invalid render matrix type";return false;}
        for(auto f:*values)if(!std::isfinite(f) || std::abs(f)>1000000.f){error="Invalid render matrix coefficient";return false;}
    }
    if(field==e.properties.end()){if(rows){error="Render pose has no visibility state";return false;}return true;}
    auto* visible=std::get_if<int64_t>(&field->second);
    if(!visible || *visible<0 || *visible>1 || (rows!=0 && rows!=3) || (*visible && rows!=3)){error="Invalid/incomplete native render pose";return false;}return true;
}
}
