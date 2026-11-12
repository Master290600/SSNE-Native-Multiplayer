// Compile the production generator in this TU to exercise its private helpers.
// The runtime archive supplies the other modules, not another gx_gpu.cpp object.
#include "../runtime/src/gx/gx_gpu.cpp"
#include <SDL.h>
#include <array>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <SDL_syswm.h>
#include "ssne/d3d11_device.h"
#include "ssne/d3d11_gx.h"
#endif

namespace {
using namespace ssne::gl;
void require(bool ok, const std::string& message) { if(!ok)throw std::runtime_error(message); }
ssne::gx::DrawCfg config() {
    ssne::gx::DrawCfg c{};c.stages=1;c.atest_always=true;c.ntex=1;
    for(auto& s:c.st) {
        s.tex_enabled=true;s.texmap=s.texcoord=0;s.ras_chan=0;
        for(int i=0;i<4;++i)s.swap_tex[i]=s.swap_ras[i]=i;
        // D=texture, A=B=C=zero, clamped: sample texture unchanged.
        s.cenv=(15u<<12)|(15u<<8)|(15u<<4)|8u|(1u<<19);
        s.aenv=(7u<<13)|(7u<<10)|(7u<<7)|(4u<<4)|(1u<<19);
    }
    return c;
}
struct Case { const char* name;ssne::gx::DrawCfg cfg;std::array<uint8_t,4> expected; };
const std::string quad_vs=R"(#version 330 core
out vec4 v_col0,v_col1;out vec3 v_tex0;
void main(){
vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
gl_Position=vec4(p*2.0-1.0,0,1);
v_col0=v_col1=vec4(255);v_tex0=vec3(0.25,0.25,1);
})";
}
int main(int argc,char** argv) {
    const bool dx11=argc>1 && std::string(argv[1])=="--dx11";
#ifdef _WIN32
    ssne::d3d11::Device device;
#endif
    SDL_Window* window=nullptr;SDL_GLContext context=nullptr;
    int result=0;
    try {
        require(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
        window=SDL_CreateWindow("Shader regression",0,0,32,32,(dx11?0:SDL_WINDOW_OPENGL)|SDL_WINDOW_HIDDEN);
        require(window!=nullptr,SDL_GetError());
        auto loader=SDL_GL_GetProcAddress;
#ifdef _WIN32
        if(dx11){SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);require(SDL_GetWindowWMInfo(window,&wm),SDL_GetError());std::string error;
            require(device.initialize(wm.info.win.window,32,32,false,error),error);require(ssne::d3d11::initialize_gx(device,error),error);loader=ssne::d3d11::gx_proc;
        }else
#endif
        {context=SDL_GL_CreateContext(window);require(context!=nullptr,SDL_GetError());}
        const char* missing=nullptr;require(ssne::gl::load(loader,&missing),missing?missing:"GL loader");
        std::cout<<"Renderer: "<<GetString(0x1F01)<<"\n";
        GLuint vao=0,fbo=0,target=0,sample=0;
        GenVertexArrays(1,&vao);BindVertexArray(vao);
        GenTextures(1,&target);BindTexture(TEXTURE_2D,target);
        TexImage2D(TEXTURE_2D,0,RGBA8,16,16,0,RGBA,UNSIGNED_BYTE,nullptr);
        GenFramebuffers(1,&fbo);BindFramebuffer(FRAMEBUFFER,fbo);
        FramebufferTexture2D(FRAMEBUFFER,COLOR_ATTACHMENT0,TEXTURE_2D,target,0);
        require(CheckFramebufferStatus(FRAMEBUFFER)==FRAMEBUFFER_COMPLETE,"Framebuffer incomplete");
        const uint8_t texels[]={51,102,153,255, 7,8,9,255, 11,12,13,255, 204,102,51,255};
        GenTextures(1,&sample);BindTexture(TEXTURE_2D,sample);
        TexImage2D(TEXTURE_2D,0,RGBA8,2,2,0,RGBA,UNSIGNED_BYTE,texels);
        TexParameteri(TEXTURE_2D,TEXTURE_MIN_FILTER,NEAREST);TexParameteri(TEXTURE_2D,TEXTURE_MAG_FILTER,NEAREST);
        Viewport(0,0,16,16);Disable(BLEND);Disable(DEPTH_TEST);if(!dx11)Disable(0x0BD0); // no dithering
        std::vector<Case> cases;
        auto direct=config();cases.push_back({"direct",direct,{51,102,153,255}});
        auto no_lookup=direct;no_lookup.st[0].ind=1;
        cases.push_back({"indirect_without_lookup",no_lookup,{51,102,153,255}});
        auto accumulated=direct;accumulated.stages=2;accumulated.st[1].ind=1u<<20;
        cases.push_back({"direct_then_indirect_addprev",accumulated,{204,102,51,255}});
        for(int channel: {2,3}) {
            auto bump=direct;bump.st[0].ras_chan=channel;
            bump.st[0].cenv=(bump.st[0].cenv&~15u)|10u;
            bump.st[0].aenv=(bump.st[0].aenv&~(7u<<4))|(5u<<4);
            cases.push_back({channel==2?"alpha_bump_without_lookup":"normalized_bump_without_lookup",bump,{0,0,0,0}});
        }
        auto regular=direct;regular.nind=1;regular.st[0].ind=1u<<7;
        cases.push_back({"regular_indirect_lookup",regular,{51,102,153,255}});
        // D3D reflection exposes an array as u_samp[0], u_samp[1], ... .
        // Verify a nonzero element maps to its requested unit, not unit zero.
        auto array_sample=direct;array_sample.st[0].texmap=3;
        cases.push_back({"texture_array_element",array_sample,{17,220,45,255}});
        GLuint alternate=0;GenTextures(1,&alternate);ActiveTexture(TEXTURE0+3);BindTexture(TEXTURE_2D,alternate);
        const uint8_t alternate_pixel[]={17,220,45,255};TexImage2D(TEXTURE_2D,0,RGBA8,1,1,0,RGBA,UNSIGNED_BYTE,alternate_pixel);
        TexParameteri(TEXTURE_2D,TEXTURE_MIN_FILTER,NEAREST);TexParameteri(TEXTURE_2D,TEXTURE_MAG_FILTER,NEAREST);ActiveTexture(TEXTURE0);
        unsigned passed=0;
        for(const auto& test:cases)for(bool snap: {false,true}) {
            const auto fs=ssne::gen_fs(test.cfg,1,snap);
            // Also link the real vertex generator, not only the synthetic draw fixture.
            const auto real=ssne::link(ssne::gen_vs(1),fs);
            if(!real){std::cerr<<test.name<<": production shader link failed\n";continue;}
            DeleteProgram(real);
            const auto program=ssne::link(quad_vs,fs);
            require(program!=0,std::string(test.name)+": fixture link failed");UseProgram(program);
            const GLint units[8]={0,0,0,3,0,0,0,0};Uniform1iv(GetUniformLocation(program,"u_samp"),8,units);
            const GLfloat scale[32]={1,1,1,1};Uniform2fv(GetUniformLocation(program,"u_stscale"),test.cfg.stages,scale);
            const GLfloat dimensions[16]={2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
            Uniform2fv(GetUniformLocation(program,"u_su"),8,dimensions);
            Uniform2fv(GetUniformLocation(program,"u_texsz"),8,dimensions);
            ClearColor(.9f,.2f,.1f,1);Clear(COLOR_BUFFER_BIT);DrawArrays(TRIANGLES,0,3);
            std::array<uint8_t,16*16*4> pixels{};ReadPixels(0,0,16,16,RGBA,UNSIGNED_BYTE,pixels.data());
            require(GetError()==0,std::string(test.name)+": OpenGL error");
            for(size_t i=0;i<pixels.size();++i)
                require(std::abs(int(pixels[i])-test.expected[i%4])<=1,std::string(test.name)+": unexpected rendered pixel at "+std::to_string(i));
            DeleteProgram(program);++passed;
            std::cout<<"PASS "<<test.name<<" depth_snap="<<snap<<" pixels=256\n";
        }
        // EFB clears obey both scissor and channel masks. D3D's native ClearRTV
        // ignores these, so this tests the backend's explicit clear draw.
        Disable(SCISSOR_TEST);ColorMask(1,1,1,1);ClearColor(10/255.f,20/255.f,30/255.f,40/255.f);Clear(COLOR_BUFFER_BIT);
        Enable(SCISSOR_TEST);Scissor(2,3,4,5);ColorMask(1,0,0,1);ClearColor(100/255.f,100/255.f,100/255.f,200/255.f);Clear(COLOR_BUFFER_BIT);
        std::array<uint8_t,16*16*4> clear_pixels{};ReadPixels(0,0,16,16,RGBA,UNSIGNED_BYTE,clear_pixels.data());
        for(int y=0;y<16;++y)for(int x=0;x<16;++x){bool inside=x>=2&&x<6&&y>=3&&y<8;const int expected[]={inside?100:10,20,30,inside?200:40};
            for(int c=0;c<4;++c)require(std::abs(int(clear_pixels[(y*16+x)*4+c])-expected[c])<=1,"Scissored/channel-masked clear changed the wrong pixel");}
        Disable(SCISSOR_TEST);ColorMask(1,1,1,1);
        std::cout<<"PASS partial EFB clear: exact rectangle and independent channel masks\n";
        BindTexture(TEXTURE_2D,sample);TexParameteri(TEXTURE_2D,TEXTURE_MAX_LEVEL,0);
        GLuint mip_sampler=0;GenSamplers(1,&mip_sampler);SamplerParameteri(mip_sampler,TEXTURE_MIN_FILTER,NEAREST_MIPMAP_NEAREST);SamplerParameteri(mip_sampler,TEXTURE_MAG_FILTER,NEAREST);BindSampler(0,mip_sampler);
        auto mip_program=ssne::link(quad_vs,"#version 330 core\nuniform sampler2D u_src;out vec4 o;void main(){o=textureLod(u_src,vec2(.25),8.);}");
        require(mip_program!=0,"Mip clamp shader");UseProgram(mip_program);Uniform1i(GetUniformLocation(mip_program,"u_src"),0);DrawArrays(TRIANGLES,0,3);
        ReadPixels(0,0,16,16,RGBA,UNSIGNED_BYTE,clear_pixels.data());const int mip_expected[]={51,102,153,255};
        for(size_t i=0;i<clear_pixels.size();++i)require(abs(int(clear_pixels[i])-mip_expected[i%4])<=1,"Sampler read beyond texture MAX_LEVEL");
        BindSampler(0,0);DeleteProgram(mip_program);std::cout<<"PASS texture mip limit with separate sampler and forced LOD\n";
        if(dx11){
            GLuint depth=0;GenTextures(1,&depth);
            for(int samples:{1,4}){
                BindTexture(samples==1?TEXTURE_2D:TEXTURE_2D_MULTISAMPLE,target);
                if(samples==1)TexImage2D(TEXTURE_2D,0,RGBA8,16,16,0,RGBA,UNSIGNED_BYTE,nullptr);else TexImage2DMultisample(TEXTURE_2D_MULTISAMPLE,samples,RGBA8,16,16,1);
                BindTexture(samples==1?TEXTURE_2D:TEXTURE_2D_MULTISAMPLE,depth);
                if(samples==1)TexImage2D(TEXTURE_2D,0,DEPTH24_STENCIL8,16,16,0,DEPTH_STENCIL,UNSIGNED_INT_24_8,nullptr);else TexImage2DMultisample(TEXTURE_2D_MULTISAMPLE,samples,DEPTH24_STENCIL8,16,16,1);
                BindFramebuffer(FRAMEBUFFER,fbo);FramebufferTexture2D(FRAMEBUFFER,DEPTH_STENCIL_ATTACHMENT,samples==1?TEXTURE_2D:TEXTURE_2D_MULTISAMPLE,depth,0);
                Disable(SCISSOR_TEST);DepthMask(1);ClearDepth(.25);Clear(DEPTH_BUFFER_BIT);
                Enable(SCISSOR_TEST);Scissor(2,3,4,5);ClearDepth(.75);Clear(DEPTH_BUFFER_BIT);
                std::array<float,256> depths{};ReadPixels(0,0,16,16,DEPTH_COMPONENT,ssne::gl::FLOAT,depths.data());
                for(int y=0;y<16;++y)for(int x=0;x<16;++x){const float expected=x>=2&&x<6&&y>=3&&y<8?.75f:.25f;if(std::abs(depths[y*16+x]-expected)>=1e-5f)throw std::runtime_error("Depth clear/readback mismatch samples="+std::to_string(samples)+" x="+std::to_string(x)+" y="+std::to_string(y)+" actual="+std::to_string(depths[y*16+x])+" expected="+std::to_string(expected));}
                std::cout<<"PASS scissored depth clear/readback samples="<<samples<<"\n";
            }
            DeleteTextures(1,&depth);
        }
        DeleteTextures(1,&alternate);DeleteTextures(1,&sample);DeleteTextures(1,&target);DeleteFramebuffers(1,&fbo);
        // The context owns the VAO; destroying the context below releases it.
        require(passed==cases.size()*2,"Not all shader variants compiled and rendered");
        std::cout<<passed<<" production shader variants compiled, linked and rendered correctly.\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
#ifdef _WIN32
    if(dx11)ssne::d3d11::shutdown_gx();
#endif
    if(context)SDL_GL_DeleteContext(context);if(window)SDL_DestroyWindow(window);SDL_Quit();return result;
}
