#include "gx_view_overlay.h"
#include <cstdio>
#include <filesystem>
using namespace ssne::gx;
int main(int argc,char** argv) {
    unsigned flashes=0;
    for(int i=1;i<argc;++i) for(const auto& entry:std::filesystem::directory_iterator(argv[i])) {
        if(entry.path().extension()!=".bin")continue;
        std::string error;auto frame=gpu_load_captured_frame(entry.path().string(),error);
        if(!frame){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        for(const auto& draw:frame->cmds) {
            if(draw.type!=GpuCmd::DRAW || !frame->states[draw.state].local_ui || draw.vcount!=4)continue;
            const auto* color=frame->vtx.data()+draw.vtx_off+16;
            if(!full_view_overlay(*frame,draw))continue;
            if(!color[0] || color[1] || color[2] || !color[3])continue;
            float p[4];std::memcpy(p,frame->vtx.data()+draw.vtx_off,16);
            std::printf("%s: damage quad rgba=%u,%u,%u,%u clip=%g,%g,%g,%g full=%d\n",
                entry.path().string().c_str(),color[0],color[1],color[2],color[3],p[0],p[1],p[2],p[3],full_view_overlay(*frame,draw));
            if(full_view_overlay(*frame,draw))++flashes;
        }
    }
    std::printf("Actual native fullscreen damage draws: %u\n",flashes);
    return flashes?0:2;
}
