#include "gx_view_overlay.h"
#include <cassert>
#include <cstdio>
using namespace ssne::gx;
int main(){
 GpuFrame f;GpuState st{};st.local_ui=true;f.states.push_back(st);
 GpuCmd c{};c.type=GpuCmd::DRAW;c.vcount=4;c.idx_count=6;c.stride=24;
 f.vtx.resize(96);
 float corners[4][4]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
 for(unsigned i=0;i<4;++i)std::memcpy(f.vtx.data()+i*24,corners[i],16);
 assert(full_view_overlay(f,c));
 f.states[0].perspective=true;assert(!full_view_overlay(f,c));f.states[0].perspective=false;
 f.states[0].local_ui=false;assert(!full_view_overlay(f,c));f.states[0].local_ui=true;
 for(unsigned i=0;i<4;++i){corners[i][1]*=.1f;std::memcpy(f.vtx.data()+i*24,corners[i],16);}
 assert(!full_view_overlay(f,c)); // health bars/banner strips keep HUD proportions
 puts("Fullscreen damage/fade overlays distinguished from HUD strips, world draws and shared menus");
}
