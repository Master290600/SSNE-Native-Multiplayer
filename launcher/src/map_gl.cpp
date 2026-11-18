#include "map_gl.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace mapedit {
namespace {
template <class T> T proc(const char *name) {
  auto p = SDL_GL_GetProcAddress(name);
  if (!p)
    throw std::runtime_error(std::string("3D editor needs OpenGL 3: ") + name);
  return reinterpret_cast<T>(p);
}
struct GL {
  PFNGLGENERATEMIPMAPPROC GenerateMipmap =
      proc<PFNGLGENERATEMIPMAPPROC>("glGenerateMipmap");
  PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate =
      proc<PFNGLBLENDFUNCSEPARATEPROC>("glBlendFuncSeparate");
  PFNGLUNIFORM1IPROC Uniform1i = proc<PFNGLUNIFORM1IPROC>("glUniform1i");
  PFNGLGENVERTEXARRAYSPROC GenVertexArrays =
      proc<PFNGLGENVERTEXARRAYSPROC>("glGenVertexArrays");
  PFNGLBINDVERTEXARRAYPROC BindVertexArray =
      proc<PFNGLBINDVERTEXARRAYPROC>("glBindVertexArray");
  PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays =
      proc<PFNGLDELETEVERTEXARRAYSPROC>("glDeleteVertexArrays");
  PFNGLGENBUFFERSPROC GenBuffers = proc<PFNGLGENBUFFERSPROC>("glGenBuffers");
  PFNGLBINDBUFFERPROC BindBuffer = proc<PFNGLBINDBUFFERPROC>("glBindBuffer");
  PFNGLBUFFERDATAPROC BufferData = proc<PFNGLBUFFERDATAPROC>("glBufferData");
  PFNGLDELETEBUFFERSPROC DeleteBuffers =
      proc<PFNGLDELETEBUFFERSPROC>("glDeleteBuffers");
  PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray =
      proc<PFNGLENABLEVERTEXATTRIBARRAYPROC>("glEnableVertexAttribArray");
  PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer =
      proc<PFNGLVERTEXATTRIBPOINTERPROC>("glVertexAttribPointer");
  PFNGLCREATESHADERPROC CreateShader =
      proc<PFNGLCREATESHADERPROC>("glCreateShader");
  PFNGLSHADERSOURCEPROC ShaderSource =
      proc<PFNGLSHADERSOURCEPROC>("glShaderSource");
  PFNGLCOMPILESHADERPROC CompileShader =
      proc<PFNGLCOMPILESHADERPROC>("glCompileShader");
  PFNGLGETSHADERIVPROC GetShaderiv =
      proc<PFNGLGETSHADERIVPROC>("glGetShaderiv");
  PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog =
      proc<PFNGLGETSHADERINFOLOGPROC>("glGetShaderInfoLog");
  PFNGLDELETESHADERPROC DeleteShader =
      proc<PFNGLDELETESHADERPROC>("glDeleteShader");
  PFNGLCREATEPROGRAMPROC CreateProgram =
      proc<PFNGLCREATEPROGRAMPROC>("glCreateProgram");
  PFNGLATTACHSHADERPROC AttachShader =
      proc<PFNGLATTACHSHADERPROC>("glAttachShader");
  PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation =
      proc<PFNGLBINDATTRIBLOCATIONPROC>("glBindAttribLocation");
  PFNGLLINKPROGRAMPROC LinkProgram =
      proc<PFNGLLINKPROGRAMPROC>("glLinkProgram");
  PFNGLGETPROGRAMIVPROC GetProgramiv =
      proc<PFNGLGETPROGRAMIVPROC>("glGetProgramiv");
  PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog =
      proc<PFNGLGETPROGRAMINFOLOGPROC>("glGetProgramInfoLog");
  PFNGLDELETEPROGRAMPROC DeleteProgram =
      proc<PFNGLDELETEPROGRAMPROC>("glDeleteProgram");
  PFNGLUSEPROGRAMPROC UseProgram = proc<PFNGLUSEPROGRAMPROC>("glUseProgram");
  PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation =
      proc<PFNGLGETUNIFORMLOCATIONPROC>("glGetUniformLocation");
  PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv =
      proc<PFNGLUNIFORMMATRIX4FVPROC>("glUniformMatrix4fv");
  PFNGLUNIFORM1FPROC Uniform1f = proc<PFNGLUNIFORM1FPROC>("glUniform1f");
  PFNGLGENFRAMEBUFFERSPROC GenFramebuffers =
      proc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers");
  PFNGLBINDFRAMEBUFFERPROC BindFramebuffer =
      proc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
  PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers =
      proc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers");
  PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D =
      proc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D");
  PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus =
      proc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus");
  PFNGLGENRENDERBUFFERSPROC GenRenderbuffers =
      proc<PFNGLGENRENDERBUFFERSPROC>("glGenRenderbuffers");
  PFNGLBINDRENDERBUFFERPROC BindRenderbuffer =
      proc<PFNGLBINDRENDERBUFFERPROC>("glBindRenderbuffer");
  PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage =
      proc<PFNGLRENDERBUFFERSTORAGEPROC>("glRenderbufferStorage");
  PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer =
      proc<PFNGLFRAMEBUFFERRENDERBUFFERPROC>("glFramebufferRenderbuffer");
  PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers =
      proc<PFNGLDELETERENDERBUFFERSPROC>("glDeleteRenderbuffers");
  PFNGLACTIVETEXTUREPROC ActiveTexture =
      proc<PFNGLACTIVETEXTUREPROC>("glActiveTexture");
};
struct State {
  GL &g;
  GLint blend_src_rgb = 0, blend_dst_rgb = 0, blend_src_a = 0, blend_dst_a = 0;
  GLint draw = 0, read = 0, viewport[4]{}, program = 0, vao = 0, buffer = 0,
        renderbuffer = 0, active = 0, texture0 = 0, depth_func = 0,
        polygon[2]{};
  GLint other_textures[2]{};
  GLboolean depth, blend, cull, scissor, depth_mask, color_mask[4]{};
  GLfloat clear[4]{};
  explicit State(GL &gl) : g(gl) {
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    g.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
    for (unsigned i = 0; i < 2; ++i) {
      g.ActiveTexture(GL_TEXTURE1 + i);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &other_textures[i]);
    }
    g.ActiveTexture(GL_TEXTURE0);
    depth = glIsEnabled(GL_DEPTH_TEST);
    blend = glIsEnabled(GL_BLEND);
    cull = glIsEnabled(GL_CULL_FACE);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glGetIntegerv(GL_POLYGON_MODE, polygon);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
  }
  ~State() {
    g.BlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    g.UseProgram(program);
    g.BindVertexArray(vao);
    g.BindBuffer(GL_ARRAY_BUFFER, buffer);
    g.BindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    for (unsigned i = 0; i < 2; ++i) {
      g.ActiveTexture(GL_TEXTURE1 + i);
      glBindTexture(GL_TEXTURE_2D, other_textures[i]);
    }
    g.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture0);
    g.ActiveTexture(active);
    auto enabled = [](GLenum key, bool yes) {
      if (yes)
        glEnable(key);
      else
        glDisable(key);
    };
    enabled(GL_DEPTH_TEST, depth);
    enabled(GL_BLEND, blend);
    enabled(GL_CULL_FACE, cull);
    enabled(GL_SCISSOR_TEST, scissor);
    glDepthMask(depth_mask);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthFunc(depth_func);
    glPolygonMode(GL_FRONT_AND_BACK, polygon[0]);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
  }
};
} // namespace
struct SceneRenderer::Impl {
  GL gl;
  GLuint program = 0, vao = 0, vbo = 0, fbo = 0, color = 0, depth = 0;
  int width = 0, height = 0;
  GLsizei count = 0;
  uint64_t revision = ~uint64_t(0);
  GLint matrix = -1, cut = -1, model = -1, normal_matrix = -1, style = -1,
        sampler = -1;
  GLint layer_sampler[2]{}, layer_op[2]{};
  GLint base_mode = -1, blend_option = -1;
  const Appearance *visual_source = nullptr;
  std::vector<GLuint> textures;
  ~Impl() {
    if (!textures.empty())
      glDeleteTextures(GLsizei(textures.size()), textures.data());
    if (vbo)
      gl.DeleteBuffers(1, &vbo);
    if (vao)
      gl.DeleteVertexArrays(1, &vao);
    if (program)
      gl.DeleteProgram(program);
    if (depth)
      gl.DeleteRenderbuffers(1, &depth);
    if (color)
      glDeleteTextures(1, &color);
    if (fbo)
      gl.DeleteFramebuffers(1, &fbo);
  }
  GLuint shader(GLenum type, const char *source) {
    GLuint id = gl.CreateShader(type);
    gl.ShaderSource(id, 1, &source, nullptr);
    gl.CompileShader(id);
    GLint ok = 0;
    gl.GetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048]{};
      gl.GetShaderInfoLog(id, sizeof(log), nullptr, log);
      gl.DeleteShader(id);
      throw std::runtime_error(std::string("3D editor shader: ") + log);
    }
    return id;
  }
  void initialize() {
    if (program)
      return;
    const char *vs = R"(#version 130
in vec3 position;in vec3 normal;in vec2 texcoord;in vec2 texcoord1;in vec2 texcoord2;in vec4 tint;
uniform mat4 viewProjection;uniform mat4 model;uniform mat4 normalMatrix;
out vec3 n;out float worldY;out float fogDepth;out vec2 uv;out vec2 uv1;out vec2 uv2;out vec4 vertexColor;
void main(){vec4 world=model*vec4(position,1.0);gl_Position=viewProjection*world;n=mat3(normalMatrix)*normal;worldY=world.y;fogDepth=gl_Position.w;uv=texcoord;uv1=texcoord1;uv2=texcoord2;vertexColor=tint;}
)";
    const char *fs = R"(#version 130
in vec3 n;in float worldY;in float fogDepth;in vec2 uv;in vec2 uv1;in vec2 uv2;in vec4 vertexColor;
uniform float cutHeight;uniform float style;uniform sampler2D image;uniform sampler2D layer1;uniform sampler2D layer2;uniform int operation1;uniform int operation2;uniform int baseMode;uniform int blendOption;
out vec4 color;
vec4 combine(vec4 previous,vec4 t,int operation){
 if(operation==1)return vec4(clamp(previous.rgb*t.rgb*2.0,0.0,1.0),previous.a*t.a);
 if(operation==2)return vec4(mix(previous.rgb,t.rgb,t.a),t.a);
 if(operation==3||operation==5)return vec4(clamp(previous.rgb+0.5*t.rgb,0.0,1.0),previous.a);
 if(operation==4)return vec4(mix(previous.rgb,t.rgb,t.a*vertexColor.a),previous.a);
 if(operation==6)return vec4(clamp(previous.rgb+t.rgb,0.0,1.0),previous.a);
 if(operation==7)return vec4(previous.rgb*(1.0-t.a)+t.rgb,t.a);
 return previous;
}
void main(){if(style<1.5&&worldY>cutHeight)discard;
 if(style>0.5){vec4 sampleColor=baseMode==0?vertexColor:baseMode==1?texture(image,uv):texture(image,uv)*vertexColor;if(operation1!=0)sampleColor=combine(sampleColor,texture(layer1,uv1),operation1);if(operation2!=0)sampleColor=combine(sampleColor,texture(layer2,uv2),operation2);if(blendOption==1 && sampleColor.a<=128.0/255.0)discard;if(blendOption<=1)sampleColor.a=1.0;color=sampleColor;}
 else {vec3 norm=normalize(n);float light=0.38+0.62*abs(dot(norm,normalize(vec3(0.35,0.8,0.4))));vec3 base=abs(norm.y)>0.6?vec3(0.49,0.52,0.58):vec3(0.31,0.40,0.53);color=vec4(base*light,1.0);}}
)";
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = 0;
    try {
      f = shader(GL_FRAGMENT_SHADER, fs);
      program = gl.CreateProgram();
      gl.AttachShader(program, v);
      gl.AttachShader(program, f);
      gl.BindAttribLocation(program, 0, "position");
      gl.BindAttribLocation(program, 1, "normal");
      gl.BindAttribLocation(program, 2, "texcoord");
      gl.BindAttribLocation(program, 3, "tint");
      gl.BindAttribLocation(program, 4, "texcoord1");
      gl.BindAttribLocation(program, 5, "texcoord2");
      gl.LinkProgram(program);
      GLint ok = 0;
      gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
      if (!ok) {
        char log[2048]{};
        gl.GetProgramInfoLog(program, sizeof(log), nullptr, log);
        throw std::runtime_error(std::string("3D editor program: ") + log);
      }
      gl.DeleteShader(v);
      gl.DeleteShader(f);
    } catch (...) {
      gl.DeleteShader(v);
      if (f)
        gl.DeleteShader(f);
      if (program) {
        gl.DeleteProgram(program);
        program = 0;
      }
      throw;
    }
    matrix = gl.GetUniformLocation(program, "viewProjection");
    model = gl.GetUniformLocation(program, "model");
    normal_matrix = gl.GetUniformLocation(program, "normalMatrix");
    style = gl.GetUniformLocation(program, "style");
    sampler = gl.GetUniformLocation(program, "image");
    base_mode = gl.GetUniformLocation(program, "baseMode");
    blend_option = gl.GetUniformLocation(program, "blendOption");
    layer_sampler[0] = gl.GetUniformLocation(program, "layer1");
    layer_sampler[1] = gl.GetUniformLocation(program, "layer2");
    layer_op[0] = gl.GetUniformLocation(program, "operation1");
    layer_op[1] = gl.GetUniformLocation(program, "operation2");
    cut = gl.GetUniformLocation(program, "cutHeight");
    gl.GenVertexArrays(1, &vao);
    gl.GenBuffers(1, &vbo);
    gl.GenFramebuffers(1, &fbo);
    gl.GenRenderbuffers(1, &depth);
    glGenTextures(1, &color);
  }
  void resize(int w, int h) {
    if (width == w && height == h)
      return;
    glBindTexture(GL_TEXTURE_2D, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    gl.BindRenderbuffer(GL_RENDERBUFFER, depth);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            color, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_RENDERBUFFER, depth);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      throw std::runtime_error("Cannot allocate the 3D editor framebuffer");
    width = w;
    height = h;
  }
  void upload(const Scene &s, const Appearance *appearance, uint64_t tag) {
    if (revision == tag && visual_source == appearance)
      return;
    if (!textures.empty())
      glDeleteTextures(GLsizei(textures.size()), textures.data());
    textures.clear();
    std::vector<VisualVertex> fallback;
    if (!appearance) {
      for (auto t : s.triangles()) {
        auto a = s.vertices()[t.a], b = s.vertices()[t.b],
             c = s.vertices()[t.c], n = normalized(cross(b - a, c - a));
        for (auto p : {a, b, c}) {
          VisualVertex v;
          v.position = p;
          v.normal = n;
          fallback.push_back(v);
        }
      }
    }
    const auto &vertices = appearance ? appearance->vertices : fallback;
    gl.BindVertexArray(vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER,
                  GLsizeiptr(vertices.size() * sizeof(VisualVertex)),
                  vertices.data(), GL_STATIC_DRAW);
    for (unsigned i = 0; i < 6; ++i)
      gl.EnableVertexAttribArray(i);
    gl.VertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, sizeof(VisualVertex),
        reinterpret_cast<void *>(offsetof(VisualVertex, position)));
    gl.VertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, sizeof(VisualVertex),
        reinterpret_cast<void *>(offsetof(VisualVertex, normal)));
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VisualVertex),
                           reinterpret_cast<void *>(offsetof(VisualVertex, u)));
    gl.VertexAttribPointer(
        3, 4, GL_FLOAT, GL_FALSE, sizeof(VisualVertex),
        reinterpret_cast<void *>(offsetof(VisualVertex, color)));
    gl.VertexAttribPointer(
        4, 2, GL_FLOAT, GL_FALSE, sizeof(VisualVertex),
        reinterpret_cast<void *>(offsetof(VisualVertex, u1)));
    gl.VertexAttribPointer(
        5, 2, GL_FLOAT, GL_FALSE, sizeof(VisualVertex),
        reinterpret_cast<void *>(offsetof(VisualVertex, u2)));
    if (appearance) {
      textures.resize(appearance->textures.size() + 1);
      glGenTextures(GLsizei(textures.size()), textures.data());
      for (size_t i = 0; i < textures.size(); ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (i < appearance->textures.size()) {
          auto &t = appearance->textures[i];
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
          gl.GenerateMipmap(GL_TEXTURE_2D);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                          GL_LINEAR_MIPMAP_LINEAR);
          if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic")) {
            GLfloat limit = 1;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &limit);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                            std::min(limit, 4.f));
          }
        } else {
          uint32_t white = 0xffffffff;
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, &white);
        }
      }
    }
    count = GLsizei(vertices.size());
    revision = tag;
    visual_source = appearance;
  }
};
SceneRenderer::SceneRenderer() = default;
SceneRenderer::~SceneRenderer() = default;
bool SceneRenderer::ready() const {
  return impl_ && impl_->program && impl_->count > 0;
}
uint32_t SceneRenderer::draw(const Scene &s, uint64_t revision,
                             const Camera &camera, int w, int h,
                             float cut_height, bool wireframe,
                             const Appearance *appearance, const Document &doc,
                             int preview_object, V3 preview, bool show_enemies,
                             const Matrix *preview_matrix) {
  if (s.triangles().empty())
    return 0;
  if (!impl_)
    impl_ = std::make_unique<Impl>();
  auto &g = impl_->gl;
  State state(g);
  impl_->initialize();
  impl_->resize(std::clamp(w, 32, 4096), std::clamp(h, 32, 4096));
  impl_->upload(s, appearance, revision);
  g.BindFramebuffer(GL_FRAMEBUFFER, impl_->fbo);
  glViewport(0, 0, impl_->width, impl_->height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(.025f, .04f, .075f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
  g.UseProgram(impl_->program);
  auto matrix = camera.matrix();
  g.UniformMatrix4fv(impl_->matrix, 1, GL_FALSE, matrix.data());
  g.Uniform1f(impl_->cut, std::isfinite(cut_height) ? cut_height : 1000001.f);
  g.BindVertexArray(impl_->vao);
  auto model = [&](const Matrix &m) {
    std::array<float, 16> value{m[0], m[4], m[8],  0, m[1], m[5], m[9],  0,
                                m[2], m[6], m[10], 0, m[3], m[7], m[11], 1};
    g.UniformMatrix4fv(impl_->model, 1, GL_FALSE, value.data());
    const float d = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                    m[1] * (m[4] * m[10] - m[6] * m[8]) +
                    m[2] * (m[4] * m[9] - m[5] * m[8]);
    if (std::abs(d) > 1e-12f) {
      // Inverse transpose once per draw, rather than an inverse per vertex.
      const std::array<float, 16> normal{(m[5] * m[10] - m[6] * m[9]) / d,
                                         (m[2] * m[9] - m[1] * m[10]) / d,
                                         (m[1] * m[6] - m[2] * m[5]) / d,
                                         0,
                                         (m[6] * m[8] - m[4] * m[10]) / d,
                                         (m[0] * m[10] - m[2] * m[8]) / d,
                                         (m[2] * m[4] - m[0] * m[6]) / d,
                                         0,
                                         (m[4] * m[9] - m[5] * m[8]) / d,
                                         (m[1] * m[8] - m[0] * m[9]) / d,
                                         (m[0] * m[5] - m[1] * m[4]) / d,
                                         0,
                                         0,
                                         0,
                                         0,
                                         1};
      g.UniformMatrix4fv(impl_->normal_matrix, 1, GL_FALSE, normal.data());
    }
  };
  if (!appearance) {
    g.Uniform1f(impl_->style, 0);
    model({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    glDrawArrays(GL_TRIANGLES, 0, impl_->count);
  } else {
    g.Uniform1i(impl_->sampler, 0);
    g.Uniform1i(impl_->layer_sampler[0], 1);
    g.Uniform1i(impl_->layer_sampler[1], 2);
    std::vector<const VisualBatch *> transparent;
    auto translucent = [](const VisualBatch &b) { return b.blend_option >= 2; };
    auto draw = [&](const VisualBatch &b) {
      auto m = appearance->model_matrix(b, doc, preview_object, preview,
                                        preview_matrix);
      if (b.sky) {
        auto eye = camera.eye();
        m[3] += eye.x;
        m[7] += eye.y;
        m[11] += eye.z;
      }
      model(m);
      g.Uniform1i(impl_->base_mode, b.base_mode);
      g.Uniform1i(impl_->blend_option, b.blend_option);
      g.Uniform1f(impl_->style, b.sky ? 2.f : 1.f);
      g.Uniform1f(impl_->cut,
                  (b.sky || b.enemy_preview)
                      ? 1000001.f
                      : (std::isfinite(cut_height) ? cut_height : 1000001.f));
      size_t ti =
          b.texture >= 0 ? size_t(b.texture) : impl_->textures.size() - 1;
      glBindTexture(GL_TEXTURE_2D, impl_->textures.at(ti));
      auto wrap = [](unsigned w) {
        return w == 1   ? GL_CLAMP_TO_EDGE
               : w == 2 ? GL_MIRRORED_REPEAT
                        : GL_REPEAT;
      };
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap(b.wrap_u));
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap(b.wrap_v));
      for (unsigned i = 0; i < 2; ++i) {
        g.Uniform1i(impl_->layer_op[i], int(b.operations[i]));
        g.ActiveTexture(GL_TEXTURE1 + i);
        glBindTexture(GL_TEXTURE_2D,
                      impl_->textures.at(b.layers[i] >= 0
                                             ? size_t(b.layers[i])
                                             : impl_->textures.size() - 1));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap(b.layer_u[i]));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap(b.layer_v[i]));
      }
      g.ActiveTexture(GL_TEXTURE0);
      glDrawArrays(GL_TRIANGLES, b.first, b.count);
    };
    glDepthMask(GL_FALSE);
    for (auto &b : appearance->batches)
      if (b.sky)
        draw(b);
    glDepthMask(GL_TRUE);
    for (auto &b : appearance->batches)
      if (!b.sky && (show_enemies || !b.enemy_preview)) {
        if (translucent(b))
          transparent.push_back(&b);
        else
          draw(b);
      }
    auto distance = [&](const VisualBatch &b) {
      auto m = appearance->model_matrix(b, doc, preview_object, preview,
                                        preview_matrix);
      return length(V3{m[3], m[7], m[11]} - camera.eye());
    };
    std::stable_sort(
        transparent.begin(), transparent.end(),
        [&](auto a, auto b) { return distance(*a) > distance(*b); });
    glEnable(GL_BLEND);
    g.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                        GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (auto b : transparent) {
      if (b->blend_option == 2)
        g.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
      else if (b->blend_option == 4)
        g.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
      else
        g.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
      draw(*b);
    }
    glDepthMask(GL_TRUE);
  }
  if (glGetError() != GL_NO_ERROR)
    throw std::runtime_error("OpenGL error drawing the 3D editor view");
  return impl_->color;
}
} // namespace mapedit
