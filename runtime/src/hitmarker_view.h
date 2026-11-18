#pragma once
#include "ssne/hitmarker_view.h"
#include "ssne/ppc_context.h"
#include <algorithm>
#include <cmath>

namespace ssne {
// Read the same anchors and offsets as the PAL camera draw (801179BC).
// Calculate in host memory so a damage callback never changes renderer state.
inline void offline_hitmarker_view(PPCContext&, unsigned slot) {
    if (slot>=detail::hitmarker_views.size()) return;
    detail::HitmarkerEdges edges{};
    const uint32_t engine=mem_read32(0x802ABAA0u);
    if (engine>=0x80000000u && engine<=guest_ram_end()-0x210u && !(engine&3u) &&
        mem_read32(engine)==0x8021C600u) {
        const auto view=engine+0xDCu+slot*76u;
        const auto mode=mem_read32(engine+0x84u);
        const auto tl=mem_read32(view+0x14u), br=mem_read32(view+0x18u);
        if (mem_read32(view) && mem_read32(view+0x30u) && mode<=2 && tl<9 && br<9) {
            static constexpr float anchors[9][2]={{0,0},{1,0},{0,1},{1,1},{.5f,.5f},
                                                   {.5f,0},{0,.5f},{1,.5f},{.5f,1}};
            const float width=2.f*u2f(mem_read32(mode==0?0x8021CA5Cu:0x8021CA64u));
            const float height=2.f*u2f(mem_read32(0x8021CA60u));
            if (width>0.f && height>0.f) {
                const float left=anchors[tl][0]+u2f(mem_read32(view+4))/width;
                const float top=anchors[tl][1]+u2f(mem_read32(view+8))/height;
                const float right=anchors[br][0]+u2f(mem_read32(view+12))/width;
                const float bottom=anchors[br][1]+u2f(mem_read32(view+16))/height;
                if (std::isfinite(left) && std::isfinite(right) && std::isfinite(top) && std::isfinite(bottom) &&
                    left<right && top<bottom) {
                    auto edge=[](float n){return static_cast<uint16_t>(std::clamp(n,0.f,1.f)*65535.f+.5f);};
                    edges={edge(left),edge(top),edge(right),edge(bottom)};
                }
            }
        }
    }
    detail::hitmarker_views[slot].store(edges,std::memory_order_relaxed);
}
}
