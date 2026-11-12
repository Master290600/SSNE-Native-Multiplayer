#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include "../port/src/dx11_backend.h"
#include "../runtime/src/gx/gx_gl.h"
#include <array>
#include <iostream>
#include <stdexcept>
namespace {void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}}
int main(int,char**){
    SDL_Window* window=nullptr;port::Dx11Backend backend;bool ui=false;int result=0;
    try{
        require(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        window=SDL_CreateWindow("DX11 UI lifecycle",0,0,320,240,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
        require(window,SDL_GetError());std::string error;require(backend.initialize(window,error),error.c_str());
        ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;require(backend.initialize_ui(window),"DX11 ImGui init");ui=true;
        const char* missing=nullptr;require(ssne::gl::load(port::Dx11Backend::getproc,&missing),"GX API load");
        using namespace ssne::gl;
        GLuint texture=0;GenTextures(1,&texture);BindTexture(TEXTURE_2D,texture);
        const std::array<uint8_t,4> pixel{17,119,221,255};TexImage2D(TEXTURE_2D,0,RGBA8,1,1,0,RGBA,UNSIGNED_BYTE,pixel.data());
        for(auto size:{std::array<int,2>{320,240},{257,129},{640,360}}){
            SDL_SetWindowSize(window,size[0],size[1]);SDL_PumpEvents();
            for(int frame=0;frame<5;++frame){
                auto start=SDL_GetTicks();while(!backend.ready()){SDL_PumpEvents();SDL_Delay(1);require(SDL_GetTicks()-start<5000,"DX11 queue stalled");}
                backend.new_frame();ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();
                ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)backend.image(texture),ImVec2(0,0),ImVec2(float(size[0]),float(size[1])));
                ImGui::Render();backend.render(window);
                std::vector<uint8_t> pixels;backend.readback(pixels);require(pixels.size()==size_t(size[0])*size[1]*4,"UI readback size");
                const size_t at=(size_t(size[1]/2)*size[0]+size[0]/2)*4;
                for(int c=0;c<4;++c)require(abs(int(pixels[at+c])-pixel[c])<=1,"DX11 UI sampled the wrong GX texture");
                // Includes the minimized/background path which skips Present:
                // it must still fence submitted GX/UI work.
                backend.present(false,frame%2==0);require(backend.pending()>0&&backend.pending()<=2,"DX11 unfenced/background submission");
            }
        }
        DeleteTextures(1,&texture);
        std::cout<<"PASS DX11 UI texture, odd-size resize/readback, 15 presents/background submissions, bounded queue\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    backend.shutdown();if(ui)ImGui_ImplSDL2_Shutdown();if(ImGui::GetCurrentContext())ImGui::DestroyContext();if(window)SDL_DestroyWindow(window);SDL_Quit();return result;
}
