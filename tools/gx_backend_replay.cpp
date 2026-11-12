// Replay immutable native GX captures through either real backend. No game,
// networking or save data is executed/modified by this comparison tool.
#include <SDL.h>
#include <SDL_syswm.h>
#include "ssne/d3d11_device.h"
#include "ssne/d3d11_gx.h"
#include "ssne/gx_gpu.h"
#include "gx_gpu_frame.h"
#include "png_write.h"
#include <iostream>
#include <stdexcept>
#include <filesystem>
int main(int argc,char** argv){
    SDL_Window* win=nullptr;SDL_GLContext gl=nullptr;ssne::d3d11::Device device;
    bool dx11=true;std::string out="replay.png",cache;std::vector<std::string> frames;int scale=1,msaa=1;bool fxaa=false;
    int result=0;
    try{
        for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--opengl")dx11=false;else if(a=="--fxaa")fxaa=true;else if(a=="--cache"&&i+1<argc)cache=argv[++i];else if(a=="--scale"&&i+1<argc)scale=atoi(argv[++i]);else if(a=="--msaa"&&i+1<argc)msaa=atoi(argv[++i]);else if(a=="--output"&&i+1<argc)out=argv[++i];else frames.push_back(a);}
        if(frames.empty())throw std::runtime_error("Supply capture frame files, --opengl optional, --output image.png");
        if(SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
        win=SDL_CreateWindow("GX comparison",0,0,640,480,SDL_WINDOW_HIDDEN|(dx11?0:SDL_WINDOW_OPENGL));if(!win)throw std::runtime_error(SDL_GetError());
        std::string error;auto loader=SDL_GL_GetProcAddress;
        if(dx11){SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);if(!SDL_GetWindowWMInfo(win,&wm))throw std::runtime_error(SDL_GetError());
            if(!device.initialize(wm.info.win.window,640,480,false,error)||!ssne::d3d11::initialize_gx(device,error))throw std::runtime_error(error);loader=ssne::d3d11::gx_proc;
        }else{gl=SDL_GL_CreateContext(win);if(!gl)throw std::runtime_error(SDL_GetError());}
        ssne::gpu_renderer_set_antialiasing(msaa,fxaa,false);
        if(!cache.empty()){std::filesystem::create_directories(cache);ssne::gpu_renderer_set_shader_cache(cache);}
        if(!ssne::gpu_renderer_init(loader,scale,error))throw std::runtime_error(error);
        ssne::gpu_renderer_set_player_views(true);
        for(auto& path:frames){auto frame=ssne::gx::gpu_load_captured_frame(path,error);if(!frame)throw std::runtime_error(error);
            std::cout<<"Rendering "<<path<<" draws="<<frame->cmds.size()<<std::endl;ssne::gx::gpu_push_frame(std::move(frame));ssne::gpu_renderer_run(1);}
        std::vector<uint32_t> pixels;int w=0,h=0;if(!ssne::gpu_renderer_read_output(pixels,w,h))throw std::runtime_error("No display output");
        if(!port::write_png(out,pixels.data(),w,h))throw std::runtime_error("Cannot write image");
        std::cout<<"PASS "<<ssne::gpu_renderer_info()<<" output="<<w<<"x"<<h<<" programs="<<ssne::gpu_renderer_stats().programs<<std::endl;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<std::endl;result=1;}
    if(dx11)ssne::d3d11::shutdown_gx();if(gl)SDL_GL_DeleteContext(gl);if(win)SDL_DestroyWindow(win);SDL_Quit();return result;
}
