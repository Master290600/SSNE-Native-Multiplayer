// Exercise the production command executor, VAO bindings, shaders and copies.
#include "../runtime/src/gx/gx_gpu.cpp"
#include <SDL.h>
#include <iostream>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <SDL_syswm.h>
#include "ssne/d3d11_device.h"
#include "ssne/d3d11_gx.h"
#endif

using namespace ssne;
using namespace ssne::gx;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
GpuFrame fixture(uint64_t sequence,bool doubled=false) {
    GpuFrame f;f.seq=sequence;f.presentation_generation=1;
    GpuCmd clear{};clear.type=GpuCmd::CLEAR;clear.w=640;clear.h=528;
    clear.cu=clear.au=clear.zu=true;clear.color=0xff162331;clear.z=0xffffff;
    f.cmds.push_back(clear);
    GpuXf xf{};xf.model[0]=xf.model[5]=xf.model[10]=1;
    xf.normal[0]=xf.normal[5]=xf.normal[10]=1;
    for(auto& control:xf.control)control[0]=control[1]=control[2]=1;
    xf.tex_control[0][0]=5<<7;f.xf_states.push_back(xf);
    auto tex=std::make_shared<Texture>();tex->id=123;tex->w=tex->h=2;
    tex->rgba={0xff3355aa,0xffaa5533,0xff44aa66,0xffaa44aa};
    f.textures.push_back(tex);
    for(unsigned d=0;d<4;++d){
        GpuState st{};st.xf=d>=2?1:0;st.perspective=true;
        st.camera_locked=d==2;st.local_ui=d==3;st.world_camera=d<2;
        st.vp[0]=320;st.vp[1]=264;st.vp[2]=16777216;
        st.vp[3]=662;st.vp[4]=606;st.vp[5]=16777216;
        st.projection[0]=st.projection[2]=1;st.projection[4]=.5f;
        auto& c=st.cfg;c.stages=1;c.atest_always=true;c.blend=8|16;
        c.dst_has_alpha=true;c.scissor_x1=639;c.scissor_y1=527;
        c.ntex=d%2;c.tex[0]=tex.get();c.su_s[0]=c.su_t[0]=2;
        auto& s=c.st[0];s.tex_enabled=d%2;s.ras_chan=0;
        for(int k=0;k<4;++k)s.swap_tex[k]=s.swap_ras[k]=k;
        s.cenv=(15u<<12)|(15u<<8)|(15u<<4)|(d%2?8u:10u)|(1u<<19);
        s.aenv=(7u<<13)|(7u<<10)|(7u<<7)|((d%2?4u:5u)<<4)|(1u<<19);
        f.states.push_back(st);
        GpuCmd draw{};draw.type=GpuCmd::DRAW;draw.state=d;draw.ntex=d%2;
        draw.stride=(st.xf?52:24)+draw.ntex*12;draw.vcount=doubled?6:3;
        draw.vtx_off=uint32_t(f.vtx.size());draw.idx_first=uint32_t(f.idx.size());draw.idx_count=draw.vcount;
        // Padding makes per-draw byte offsets different from a packed vertex index.
        f.vtx.resize(f.vtx.size()+draw.vcount*draw.stride+16);
        const float x=float(d%2)*.9f-.85f,y=float(d/2)*.9f-.85f;
        const float p[3][4]={{x,y,st.xf?-1.f:-.5f,1},{x+.7f,y,st.xf?-1.f:-.5f,1},{x,y+.7f,st.xf?-1.f:-.5f,1}};
        for(unsigned index=0;index<draw.vcount;++index){
            const unsigned v=index%3;
            auto* dst=f.vtx.data()+draw.vtx_off+index*draw.stride;
            std::memcpy(dst,p[v],16);
            const uint8_t colors[8]={uint8_t(40+d*40),uint8_t(220-v*55),uint8_t(50+v*80),255,10,20,30,255};
            std::memcpy(dst+16,colors,8);
            if(draw.ntex){float uv[3]={v==1?1.f:0.f,v==2?1.f:0.f,st.xf?0.f:1.f};std::memcpy(dst+24,uv,12);}
            if(st.xf){const unsigned at=24+draw.ntex*12;float normal[3]={0,0,1},raw[4]={p[v][0],p[v][1],p[v][2],0};
                std::memcpy(dst+at,normal,12);std::memcpy(dst+at+12,raw,16);}
        }
        for(unsigned index=0;index<draw.vcount;++index)f.idx.push_back(index);f.cmds.push_back(draw);
        if(d==1){GpuCmd copy{};copy.type=GpuCmd::COPY_TEX;copy.w=copy.out_w=32;copy.h=copy.out_h=32;copy.copy_id=sequence+500;f.cmds.push_back(copy);f.live_copies.push_back(copy.copy_id);}
    }
    GpuCmd display{};display.type=GpuCmd::COPY_XFB;display.w=640;display.h=528;f.cmds.push_back(display);
    return f;
}
std::vector<uint32_t> pixels(){
    std::vector<uint32_t> p(size_t(g_out_w)*g_out_h);
    BindFramebuffer(FRAMEBUFFER,g_out_fbo);ReadPixels(0,0,g_out_w,g_out_h,RGBA,UNSIGNED_BYTE,p.data());
    require(GetError()==0,"GL error during production rendering");return p;
}
void shift(GpuFrame& f,float amount){
    materialize_positions(f);
    for(const auto& c:f.cmds)if(c.type==GpuCmd::DRAW && !f.states[c.state].camera_locked && !f.states[c.state].local_ui)
        for(unsigned v=0;v<c.vcount;++v){float x;auto* p=f.vtx.data()+c.vtx_off+v*c.stride;std::memcpy(&x,p,4);x+=amount;std::memcpy(p,&x,4);}
}
}
int main(int argc,char** argv){
    bool dx11=argc>1&&std::string(argv[1])=="--dx11";
#ifdef _WIN32
    ssne::d3d11::Device device;
#endif
    SDL_Window* window=nullptr;SDL_GLContext context=nullptr;int result=0;
    try{
        require(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
        window=SDL_CreateWindow("Position upload regression",0,0,32,32,(dx11?0:SDL_WINDOW_OPENGL)|SDL_WINDOW_HIDDEN);
        require(window!=nullptr,SDL_GetError());std::string error;auto loader=SDL_GL_GetProcAddress;
#ifdef _WIN32
        if(dx11){SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);require(SDL_GetWindowWMInfo(window,&wm),SDL_GetError());
            require(device.initialize(wm.info.win.window,32,32,false,error),error.c_str());require(ssne::d3d11::initialize_gx(device,error),error.c_str());loader=ssne::d3d11::gx_proc;
        }else
#endif
        {context=SDL_GL_CreateContext(window);require(context!=nullptr,SDL_GetError());}
        require(gpu_renderer_init(loader,1,error),error.c_str());
        unsigned comparisons=0;
        for(int scale:{1,2}){
            gpu_renderer_set_scale(scale);
            auto f=fixture(scale*10);g_compact_upload=false;execute(f);const auto canonical=pixels();
            require(std::set<uint32_t>(canonical.begin(),canonical.end()).size()>100,"Fixture rendered no varied geometry");
            const auto copy=g_copies.at(f.seq+500).rt.tex;
            shift(f,.06f);
            for(int repeat=0;repeat<3;++repeat){
                g_compact_upload=false;execute(f,true,true);const auto full=pixels();
                require(full!=canonical,"Position change was invisible");
                g_compact_upload=true;execute(f,true,true);require(g_using_position_buffer,"Resident prediction did not use compact upload");
                require(pixels()==full,"Compact upload changed pixels");++comparisons;
                require(g_copies.at(f.seq+500).rt.tex==copy,"Prediction replaced a canonical texture copy");
                shift(f,.015f);
            }
            // A different frame/prerequisite overwrites both canonical buffers.
            auto other=fixture(100+scale);execute(other,false);
            execute(f,true,true);require(!g_using_position_buffer,"Stale residency reused another frame's attributes");
            const auto fallback=pixels();execute(f,true,true);require(g_using_position_buffer,"Fallback did not establish residency");
            require(pixels()==fallback,"Resident replay differs after fallback");++comparisons;
            ++f.presentation_generation;execute(f,true,true);require(!g_using_position_buffer,"New generation reused stale residency");
            ++f.seq;execute(f,true,true);require(!g_using_position_buffer,"New sequence reused stale residency");
            // Resizing the EFB must not lose the resident canonical attributes.
            gpu_renderer_set_scale(scale==1?2:1);
            g_compact_upload=false;execute(f,true,true);const auto resized=pixels();
            g_compact_upload=true;execute(f,true,true);require(g_using_position_buffer,"Resize discarded buffer residency");
            require(pixels()==resized,"Compact replay differs after target resize");++comparisons;
            // Canonical uploads remain full even after compact prediction.
            auto fresh=fixture(200+scale);execute(fresh);require(!g_using_position_buffer,"Canonical upload was compacted");

            auto prior=fixture(400+scale,true),current=fixture(401+scale,true);
            for(auto* frame:{&prior,&current}){frame->local_camera=frame->camera_valid=true;frame->camera={1,0,0,0,0,1,0,0,0,0,1,0};}
            current.camera[3]=.04f;
            for(unsigned d=0;d<current.states.size();++d){
                prior.states[d].render_proxy=current.states[d].render_proxy=d+1;
                prior.states[d].cfg.tex[0]=current.states[d].cfg.tex[0];
            }
            materialize_positions(prior);materialize_positions(current);
            FrameInterpolation scalar,packed;packed.batch_reprojection=true;
            scalar.prepare(&prior,current);packed.prepare(&prior,current);
            const auto native=current.vtx;const auto look=prior.camera;
            scalar.apply(current.vtx,1.5f,&look);const auto reference=current.vtx;
            require(reference!=native,"Reprojection fixture did not move");
            g_compact_upload=false;execute(current,true,true);const auto expected=pixels();
            current.vtx=native;packed.apply(current.vtx,1.5f,&look);
            require(current.vtx==reference,"Batched projection changed vertex payload");
            g_compact_upload=true;execute(current,true,true);
            require(pixels()==expected,"Batched projection changed framebuffer pixels");++comparisons;
            current.vtx=native;
            PackedPositionLayout layout;require(layout.prepare(current),"Invalid compact fixture layout");
            VertexBytes reduced;layout.gather(current,reduced);
            FrameInterpolation direct;direct.batch_reprojection=true;direct.prepare(&prior,current,&layout);
            direct.apply(reduced,1.5f,&look);
            auto expanded=native;layout.scatter(current,reduced,expanded);
            require(expanded==reference,"Packed interpolation changed full payload");
            execute(current,true,true,&layout,&reduced);
            require(g_using_position_buffer && pixels()==expected,"Direct positions changed pixels");++comparisons;
            auto interloper=fixture(800+scale);execute(interloper,false);
            execute(current,true,true,&layout,&reduced);
            require(g_using_position_buffer && pixels()==expected,"Direct positions lost attributes after another frame");++comparisons;
            ++current.seq;++current.presentation_generation;
            execute(current,true,true,&layout,&reduced);
            require(pixels()==expected,"Direct positions failed to refresh generation/sequence");++comparisons;
            g_compact_upload=false;execute(current,true,true,&layout,&reduced);
            require(!g_using_position_buffer && pixels()==expected,"Direct positions full-upload fallback differs");++comparisons;
            g_compact_upload=true;execute(current,true,true,&layout,&reduced);
            require(g_using_position_buffer && pixels()==expected,"Direct positions failed after full fallback");++comparisons;
            require(current.vtx==native,"Direct prediction corrupted canonical history");
            gpu_renderer_set_scale(scale==1?2:1);
            g_compact_upload=false;execute(current,true,true,&layout,&reduced);const auto alternate=pixels();
            g_compact_upload=true;execute(current,true,true,&layout,&reduced);
            require(pixels()==alternate,"Direct positions changed pixels after resize");++comparisons;
        }
        // Inject a producer arrival DURING execute via the real look callback.
        // A snapshot batch must leave it for the next presentation. Canonical
        // copy commands and an already queued backlog still execute in order.
        g_snapshot_batch=true;gpu_renderer_set_interpolation(true);
        unsigned arrivals=0;
        gpu_renderer_set_look_provider([&](const RenderLookSample&,std::array<float,12>&){
            if(!arrivals++){auto next=std::make_unique<GpuFrame>(fixture(1002));shift(*next,.1f);gpu_push_frame(std::move(next));}
            return false;
        });
        auto first=std::make_unique<GpuFrame>(fixture(1001));first->local_camera=true;
        gpu_push_frame(std::move(first));auto before=g_out_seq;
        require(gpu_renderer_run(2) && g_out_seq==before+1,"Consumed a frame that arrived during rendering");
        require(gpu_frames_queued()==1,"Mid-batch arrival was not retained");
        require(g_copies.contains(1501),"Snapshot batch lost the first frame texture copy");
        const auto first_pixels=pixels();
        require(!gpu_renderer_run(0) && gpu_frames_queued()==1,"Zero batch consumed a queued image");
        gpu_renderer_set_look_provider({});
        require(gpu_renderer_run(2) && gpu_frames_queued()==0,"Following presentation lost the arrival");
        require(pixels()!=first_pixels && g_copies.contains(1502),"Following image or its texture copy was lost");
        require(!gpu_renderer_run(2),"Empty queue reported a new image");
        gpu_push_frame(std::make_unique<GpuFrame>(fixture(1003)));
        gpu_push_frame(std::make_unique<GpuFrame>(fixture(1004)));before=g_out_seq;
        require(gpu_renderer_run(2) && g_out_seq==before+2 && gpu_frames_queued()==0,"Initial backlog was not executed");
        require(g_copies.contains(1504),"Backlog lost final texture copy");
        gpu_renderer_set_interpolation(false);
        std::cout<<"PASS queue snapshot: mid-render arrival, zero/empty batch, backlog and canonical copies\n";
        std::cout<<"PASS "<<comparisons<<" exact framebuffer comparisons; CPU/XF positions, colors, UVs, locked draws, copies, scales and residency\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
#ifdef _WIN32
    if(dx11)ssne::d3d11::shutdown_gx();
#endif
    if(context)SDL_GL_DeleteContext(context);if(window)SDL_DestroyWindow(window);SDL_Quit();return result;
}
