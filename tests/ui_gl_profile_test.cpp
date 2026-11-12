#include "../port/src/ui_gl_profile.h"
#include "ssne/performance_detail.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void profiling(bool enabled){
#ifdef _WIN32
    require(_putenv_s("SSNE_PROFILE_UI_GL",enabled?"1":"0")==0,"Cannot configure profiler");
#else
    require(setenv("SSNE_PROFILE_UI_GL",enabled?"1":"0",1)==0,"Cannot configure profiler");
#endif
    port::profile_ui_gl_calls();
}
std::vector<unsigned char> render(ImDrawData* data){
    glViewport(0,0,320,240);glClearColor(.05f,.1f,.2f,1);glClear(GL_COLOR_BUFFER_BIT);
    {
        ssne::perf::detail::Frame sample(ssne::perf::detail::UiSubmission);
        ImGui_ImplOpenGL3_RenderDrawData(data);
    }
    std::vector<unsigned char> out(320*240*4);glReadPixels(0,0,320,240,GL_RGBA,GL_UNSIGNED_BYTE,out.data());
    require(glGetError()==GL_NO_ERROR,"GL error in UI rendering");return out;
}
int main(int,char**){
    SDL_Window* window=nullptr;SDL_GLContext gl=nullptr;bool backends=false;int result=0;
    try{
        require(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
        window=SDL_CreateWindow("UI profiling regression",0,0,320,240,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
        require(window!=nullptr,SDL_GetError());gl=SDL_GL_CreateContext(window);require(gl!=nullptr,SDL_GetError());
        ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;
        require(ImGui_ImplSDL2_InitForOpenGL(window,gl),"SDL backend failed");
        require(ImGui_ImplOpenGL3_Init("#version 330 core"),"GL backend failed");backends=true;
        profiling(false);
        // Let ImGui settle the newly created window before capturing its draw data.
        ImGui_ImplOpenGL3_NewFrame();ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(30,35));ImGui::SetNextWindowSize(ImVec2(250,160));
        ImGui::Begin("Settings fixture",nullptr,ImGuiWindowFlags_NoSavedSettings);ImGui::End();
        ImGui::Render();
        ImGui_ImplOpenGL3_NewFrame();ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();
        auto* background=ImGui::GetBackgroundDrawList();
        background->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
        background->AddCallback([](const ImDrawList*,const ImDrawCmd*){glDisable(GL_BLEND);},nullptr);
        background->AddRectFilled(ImVec2(5,5),ImVec2(315,235),IM_COL32(20,120,180,80));
        background->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
        ImGui::SetNextWindowPos(ImVec2(30,35));ImGui::SetNextWindowSize(ImVec2(250,160));
        ImGui::Begin("Settings fixture",nullptr,ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("UI state / font / alpha");
        bool checked=true;ImGui::Checkbox("Graphics",&checked);ImGui::Button("Save");
        ImGui::End();ImGui::Render();
        auto expected=render(ImGui::GetDrawData());
        std::set<unsigned> colors;
        for(size_t i=0;i<expected.size();i+=4)colors.insert(unsigned(expected[i])|(unsigned(expected[i+1])<<8)|(unsigned(expected[i+2])<<16));
        // The default pixel font has few colors; verify visible text and two
        // populated draw lists instead of assuming a photographic histogram.
        size_t text_pixels=0;
        for(size_t i=0;i<expected.size();i+=4)
            if(expected[i]>200 && expected[i+1]>200 && expected[i+2]>200)++text_pixels;
        require(colors.size()>8 && text_pixels>50 && ImGui::GetDrawData()->CmdListsCount>=2
                && ImGui::GetDrawData()->TotalVtxCount>200,"Empty UI fixture");
        profiling(true);
        ssne::perf::detail::period.store(1);
        for(int i=0;i<3;++i){
            require(render(ImGui::GetDrawData())==expected,"UI diagnostic wrappers changed pixels");
            port::profile_ui_gl_calls(); // installation is idempotent, never recurse
        }
        const auto& totals=ssne::perf::detail::totals[ssne::perf::detail::UiSubmission];
        require(totals.samples==3,"Did not sample all three UI submissions");
        for(auto stage:{ssne::perf::detail::UiQuery,ssne::perf::detail::UiUpload,
                        ssne::perf::detail::UiDraw,ssne::perf::detail::UiVao})
            require(totals.batch.spans[stage].calls>0,"Expected GL call group not exercised");
        std::cout<<"PASS 3 exact UI framebuffer comparisons; real GL query/upload/draw/VAO calls, font, alpha and reset callbacks\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    if(backends){ImGui_ImplOpenGL3_Shutdown();ImGui_ImplSDL2_Shutdown();}
    if(ImGui::GetCurrentContext())ImGui::DestroyContext();
    if(gl)SDL_GL_DeleteContext(gl);if(window)SDL_DestroyWindow(window);SDL_Quit();return result;
}
