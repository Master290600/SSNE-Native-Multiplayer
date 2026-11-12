#include "d3d11_shader.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_hlsl.hpp>
#include <mutex>
#include <stdexcept>
namespace ssne::d3d11 {
ShaderPair translate_gx(const std::string& vertex,const std::string& fragment) {
    static std::once_flag once;
    std::call_once(once,[]{ if(!glslang::InitializeProcess()) throw std::runtime_error("glslang initialization failed"); });
    glslang::TShader vs(EShLangVertex),ps(EShLangFragment);
    auto configure=[](glslang::TShader& s,EShLanguage stage,const std::string& source){
        // 450 supplies explicit IO locations to SPIR-V; the GX source remains
        // GLSL 330 on OpenGL. No arithmetic/TEV rewrites are performed here.
        std::string text=source;
        auto end=text.find('\n');text.replace(0,end,"#version 450 core");
        const char* p=text.c_str();s.setStrings(&p,1);
        s.setEnvInput(glslang::EShSourceGlsl,stage,glslang::EShClientOpenGL,450);
        s.setEnvClient(glslang::EShClientOpenGL,glslang::EShTargetOpenGL_450);
        s.setEnvTarget(glslang::EShTargetSpv,glslang::EShTargetSpv_1_0);
        s.setAutoMapBindings(true);s.setAutoMapLocations(true);
        if(!s.parse(GetDefaultResources(),450,false,EShMsgSpvRules))
            throw std::runtime_error(std::string("GX GLSL: ")+s.getInfoLog());
    };
    configure(vs,EShLangVertex,vertex);configure(ps,EShLangFragment,fragment);
    glslang::TProgram program;program.addShader(&vs);program.addShader(&ps);
    if(!program.link(EShMsgSpvRules) || !program.mapIO())
        throw std::runtime_error(std::string("GX shader link: ")+program.getInfoLog());
    auto convert=[&](EShLanguage stage){
        std::vector<unsigned> spirv;glslang::SpvOptions opt;opt.disableOptimizer=true;
        glslang::GlslangToSpv(*program.getIntermediate(stage),spirv,&opt);
        spirv_cross::CompilerHLSL hlsl(std::move(spirv));
        auto common=hlsl.get_common_options();
        common.vertex.fixup_clipspace=true;
        // GX framebuffer row zero is its top. The shared GLSL maps it to -Y;
        // flip vertex Y for D3D's top-left framebuffer convention.
        common.vertex.flip_vert_y=true;hlsl.set_common_options(common);
        auto options=hlsl.get_hlsl_options();options.shader_model=50;
        hlsl.set_hlsl_options(options);
        return hlsl.compile();
    };
    return {convert(EShLangVertex),convert(EShLangFragment)};
}
}
