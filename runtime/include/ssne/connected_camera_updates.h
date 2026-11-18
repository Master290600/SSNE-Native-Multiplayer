#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace ssne::camera_updates {
// Fresh guest-thread facts only; no retained native pointers or RAM writes.
struct Binding {
    uint32_t actor=0,actor_class=0,slot=0,actor_camera=0;
    uint32_t camera=0,camera_class=0,camera_owner=0,mode=0,shake=0;
    uint32_t movement=0,vehicle=0,frozen=0,dead=0;
    uint32_t fov=0,fov_class=0,fov_method=0;
    int16_t fov_adjust=0;
    float health=0;
};
inline bool range(uint32_t p,uint32_t size,uint32_t end){return p>=0x80000000u && uint64_t(p)+size<=end;}
inline bool overlaps(uint32_t a,uint32_t as,uint32_t b,uint32_t bs){return uint64_t(a)<uint64_t(b)+bs && uint64_t(b)<uint64_t(a)+as;}
// A failed or ambiguous fact preserves the complete original expanded mask.
inline uint32_t select(uint32_t canonical,uint32_t legacy,unsigned capacity,
                       bool eligible,uint8_t occupied,uint8_t connected,uint32_t ram_end,
                       const std::array<Binding,8>& bindings){
    if(!eligible || canonical!=0xFu || capacity<5 || capacity>8)return legacy;
    const uint32_t players=(1u<<capacity)-1;
    if(!(occupied&connected&1u) || (connected&~players) || (occupied&~connected))return legacy;
    if((connected&players)==players)return legacy;
    for(unsigned i=0;i<capacity;++i){
        const auto& b=bindings[i];
        if(!range(b.actor,0x790,ram_end) || !range(b.camera,0xE0,ram_end) ||
           b.actor_class!=0x802121E8u || b.slot!=i || b.camera_class!=0x80210880u ||
           b.actor_camera!=b.camera || b.camera_owner!=b.actor)return legacy;
        if(!(connected&(1u<<i)) &&
           (b.mode || b.shake || b.movement!=1 || b.vehicle || b.frozen || b.dead ||
            !std::isfinite(b.health) || b.health<=0 || !range(b.fov,0x28,ram_end) ||
            b.fov_class!=0x80210E80u || b.fov_adjust || b.fov_method!=0x800A981Cu))return legacy;
        for(unsigned j=0;j<capacity;++j){
            const auto& other=bindings[j];
            if(overlaps(b.actor,0x790,other.camera,0xE0) ||
               (i!=j && (overlaps(b.actor,0x790,other.actor,0x790) || overlaps(b.camera,0xE0,other.camera,0xE0))))return legacy;
        }
    }
    return (legacy&~players)|connected;
}
} // namespace ssne::camera_updates
