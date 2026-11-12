#include "gx_frame_interpolation.h"
#include <cassert>
#include <cstdio>
using namespace ssne::gx;
GpuFrame frame(uint64_t seq,float x) {
    GpuFrame f;f.seq=seq;f.local_camera=true;f.wide=1.333333f;
    GpuState s{};s.perspective=true;s.vp[0]=320;s.vp[1]=-224;f.states.push_back(s);
    GpuCmd c{};c.type=GpuCmd::DRAW;c.vcount=3;c.stride=24;c.idx_count=3;
    f.cmds.push_back(c);f.idx={0,1,2};f.vtx.resize(72);
    for(unsigned i=0;i<3;++i){float pos[4]={x+float(i),float(i),.1f,10};std::memcpy(f.vtx.data()+24*i,pos,16);}
    return f;
}
int main() {
    auto a=frame(1,0),b=frame(2,1);auto original=b.vtx;
    FrameInterpolation lerp;lerp.prepare(&a,b);assert(lerp.positions.size()==3);
    lerp.apply(b.vtx,.5f);float x;std::memcpy(&x,b.vtx.data(),4);assert(x==.5f);
    lerp.apply(b.vtx,1);assert(b.vtx==original); lerp.apply(b.vtx,2);std::memcpy(&x,b.vtx.data(),4);assert(x==2);b.vtx=original;
    b.seq=4;lerp.prepare(&a,b);assert(lerp.positions.empty());
    b.seq=2;b.states[0].local_ui=true;lerp.prepare(&a,b);assert(lerp.positions.empty());
    b.states[0].local_ui=false;b.vtx[16]=1;lerp.prepare(&a,b);assert(lerp.positions.empty());
    b.vtx=original;b.idx[0]=2;lerp.prepare(&a,b);assert(lerp.positions.empty());
    b.idx[0]=0;GpuCmd copy{};copy.type=GpuCmd::COPY_TEX;b.cmds.push_back(copy);
    copy.copy_id=71;b.cmds.back()=copy;lerp.prepare(&a,b);lerp.apply(b.vtx,1.5f);assert(b.cmds.back().copy_id==71); // copy ownership stays canonical
    b=frame(2,100);lerp.prepare(&a,b);assert(lerp.positions.empty()); // teleport/camera cut
    puts("Render motion prediction: endpoints, topology, HUD, copy IDs, dropped-frame and teleport guards passed");
}
