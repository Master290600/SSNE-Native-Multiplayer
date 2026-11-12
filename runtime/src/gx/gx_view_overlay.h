// Player HUD keeps its 4:3 proportions; viewport-sized overlays cover widescreen.
#pragma once
#include "gx_gpu_frame.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace ssne::gx {
inline bool full_view_overlay(const GpuFrame& frame,const GpuCmd& draw) {
    // The combo border batches many textured quads into one draw. Restricting
    // this to a single quad squeezed that border back into the 4:3 HUD canvas.
    if(draw.type!=GpuCmd::DRAW || draw.vcount<4 || draw.idx_count<6 || draw.stride<16 || draw.state>=frame.states.size() ||
        uint64_t(draw.vtx_off)+uint64_t(draw.vcount)*draw.stride>frame.vtx.size())return false;
    const auto& state=frame.states[draw.state];
    if(state.perspective || !state.local_ui)return false;
    float min_x=2,max_x=-2,min_y=2,max_y=-2;
    for(unsigned i=0;i<draw.vcount;++i) {
        float pos[4];std::memcpy(pos,frame.vtx.data()+draw.vtx_off+i*draw.stride,16);
        if(!std::isfinite(pos[3]) || std::abs(pos[3])<1e-6f)return false;
        const float x=pos[0]/pos[3],y=pos[1]/pos[3];
        if(!std::isfinite(x) || !std::isfinite(y))return false;
        min_x=std::min(min_x,x);max_x=std::max(max_x,x);
        min_y=std::min(min_y,y);max_y=std::max(max_y,y);
    }
    return min_x<=-.99f && max_x>=.99f && min_y<=-.99f && max_y>=.99f;
}
}
