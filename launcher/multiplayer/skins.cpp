#include "skins.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#include <array>
#include <vector>
#include <fstream>
#include <cmath>
#include <stdexcept>
#include <algorithm>
namespace skins {
namespace {
using Matrix=std::array<float,16>;
Matrix identity(){Matrix r{};r[0]=r[5]=r[10]=r[15]=1;return r;}
Matrix multiply(const Matrix& a,const Matrix& b){Matrix r{};for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)for(unsigned k=0;k<4;++k)r[col*4+row]+=a[k*4+row]*b[col*4+k];return r;}
Matrix translation(float x,float y,float z){auto r=identity();r[12]=x;r[13]=y;r[14]=z;return r;}
Matrix rotation(float angle,bool x){auto r=identity();const auto c=std::cos(angle*.01745329252f),s=std::sin(angle*.01745329252f);if(x){r[5]=c;r[6]=s;r[9]=-s;r[10]=c;}else{r[0]=c;r[2]=-s;r[8]=s;r[10]=c;}return r;}
Matrix perspective(float aspect){Matrix r{};const float f=1.f/std::tan(33.f*.00872664626f);r[0]=f/aspect;r[5]=f;r[10]=-1.002f;r[11]=-1;r[14]=-.02002f;return r;}
}
struct Preview::Impl {
#define FN(n,t) t n=nullptr;
 FN(glGenVertexArrays,PFNGLGENVERTEXARRAYSPROC) FN(glBindVertexArray,PFNGLBINDVERTEXARRAYPROC) FN(glDeleteVertexArrays,PFNGLDELETEVERTEXARRAYSPROC)
 FN(glGenBuffers,PFNGLGENBUFFERSPROC) FN(glBindBuffer,PFNGLBINDBUFFERPROC) FN(glBufferData,PFNGLBUFFERDATAPROC) FN(glDeleteBuffers,PFNGLDELETEBUFFERSPROC)
 FN(glCreateShader,PFNGLCREATESHADERPROC) FN(glShaderSource,PFNGLSHADERSOURCEPROC) FN(glCompileShader,PFNGLCOMPILESHADERPROC) FN(glGetShaderiv,PFNGLGETSHADERIVPROC) FN(glGetShaderInfoLog,PFNGLGETSHADERINFOLOGPROC) FN(glDeleteShader,PFNGLDELETESHADERPROC)
 FN(glCreateProgram,PFNGLCREATEPROGRAMPROC) FN(glAttachShader,PFNGLATTACHSHADERPROC) FN(glLinkProgram,PFNGLLINKPROGRAMPROC) FN(glGetProgramiv,PFNGLGETPROGRAMIVPROC) FN(glDeleteProgram,PFNGLDELETEPROGRAMPROC) FN(glUseProgram,PFNGLUSEPROGRAMPROC)
 FN(glEnableVertexAttribArray,PFNGLENABLEVERTEXATTRIBARRAYPROC) FN(glVertexAttribPointer,PFNGLVERTEXATTRIBPOINTERPROC)
 FN(glGetUniformLocation,PFNGLGETUNIFORMLOCATIONPROC) FN(glUniformMatrix4fv,PFNGLUNIFORMMATRIX4FVPROC) FN(glUniform1i,PFNGLUNIFORM1IPROC) FN(glActiveTexture,PFNGLACTIVETEXTUREPROC)
 FN(glGenFramebuffers,PFNGLGENFRAMEBUFFERSPROC) FN(glBindFramebuffer,PFNGLBINDFRAMEBUFFERPROC) FN(glFramebufferTexture2D,PFNGLFRAMEBUFFERTEXTURE2DPROC) FN(glCheckFramebufferStatus,PFNGLCHECKFRAMEBUFFERSTATUSPROC) FN(glDeleteFramebuffers,PFNGLDELETEFRAMEBUFFERSPROC)
 FN(glGenRenderbuffers,PFNGLGENRENDERBUFFERSPROC) FN(glBindRenderbuffer,PFNGLBINDRENDERBUFFERPROC) FN(glRenderbufferStorage,PFNGLRENDERBUFFERSTORAGEPROC) FN(glFramebufferRenderbuffer,PFNGLFRAMEBUFFERRENDERBUFFERPROC) FN(glDeleteRenderbuffers,PFNGLDELETERENDERBUFFERSPROC)
#undef FN
 struct Model {GLuint vao=0,vbo=0,texture=0,thumbnail=0;unsigned count=0;};
 std::vector<Model> models;std::string error;GLuint program=0,framebuffer=0,depth=0,output=0;unsigned width=0,height=0;
 GLint mvp_location=-1,model_location=-1;
 void initialize(){
#define LOAD(n) n=reinterpret_cast<decltype(n)>(SDL_GL_GetProcAddress(#n));if(!n)throw std::runtime_error("3D-Vorschau benötigt OpenGL 3.3.");
 LOAD(glGenVertexArrays) LOAD(glBindVertexArray) LOAD(glDeleteVertexArrays) LOAD(glGenBuffers) LOAD(glBindBuffer) LOAD(glBufferData) LOAD(glDeleteBuffers)
 LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv) LOAD(glGetShaderInfoLog) LOAD(glDeleteShader) LOAD(glCreateProgram) LOAD(glAttachShader) LOAD(glLinkProgram) LOAD(glGetProgramiv) LOAD(glDeleteProgram) LOAD(glUseProgram)
 LOAD(glEnableVertexAttribArray) LOAD(glVertexAttribPointer) LOAD(glGetUniformLocation) LOAD(glUniformMatrix4fv) LOAD(glUniform1i) LOAD(glActiveTexture)
 LOAD(glGenFramebuffers) LOAD(glBindFramebuffer) LOAD(glFramebufferTexture2D) LOAD(glCheckFramebufferStatus) LOAD(glDeleteFramebuffers)
 LOAD(glGenRenderbuffers) LOAD(glBindRenderbuffer) LOAD(glRenderbufferStorage) LOAD(glFramebufferRenderbuffer) LOAD(glDeleteRenderbuffers)
#undef LOAD
  const char* vertex="#version 330 core\nlayout(location=0)in vec3 position;layout(location=1)in vec3 normal;layout(location=2)in vec2 uv;uniform mat4 mvp;uniform mat4 model;out vec2 texcoord;out vec3 n;void main(){gl_Position=mvp*vec4(position,1);texcoord=uv;n=mat3(model)*normal;}";
  const char* fragment="#version 330 core\nin vec2 texcoord;in vec3 n;uniform sampler2D diffuse;out vec4 color;void main(){vec4 c=texture(diffuse,texcoord);if(c.a<0.15)discard;float light=0.76+0.30*abs(dot(normalize(n),normalize(vec3(-0.4,0.7,1))));color=vec4(c.rgb*light,c.a);}";
  auto shader=[&](GLenum type,const char* source){auto s=glCreateShader(type);glShaderSource(s,1,&source,nullptr);glCompileShader(s);GLint okay;glGetShaderiv(s,GL_COMPILE_STATUS,&okay);if(!okay){glDeleteShader(s);throw std::runtime_error("3D-Vorschau: Shader konnte nicht erstellt werden.");}return s;};
  auto vs=shader(GL_VERTEX_SHADER,vertex),fs=shader(GL_FRAGMENT_SHADER,fragment);program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);glDeleteShader(vs);glDeleteShader(fs);GLint okay;glGetProgramiv(program,GL_LINK_STATUS,&okay);if(!okay)throw std::runtime_error("3D-Vorschau: Shader konnte nicht verbunden werden.");
  mvp_location=glGetUniformLocation(program,"mvp");model_location=glGetUniformLocation(program,"model");glUseProgram(program);glUniform1i(glGetUniformLocation(program,"diffuse"),0);glUseProgram(0);glGenFramebuffers(1,&framebuffer);glGenRenderbuffers(1,&depth);
 }
 GLuint texture(unsigned w,unsigned h,const void* pixels){GLuint result;glGenTextures(1,&result);glBindTexture(GL_TEXTURE_2D,result);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);return result;}
 GLuint draw(unsigned skin,unsigned w,unsigned h,float yaw,float pitch,float zoom,bool thumb=false){
  if(skin>=models.size() || !program)return 0;
  w=std::clamp(w,32u,1600u);h=std::clamp(h,32u,1600u);
  GLuint target;
  if(thumb)target=texture(w,h,nullptr);else{if(w!=width || h!=height){if(output)glDeleteTextures(1,&output);output=texture(w,h,nullptr);width=w;height=h;}target=output;}
  glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);glBindRenderbuffer(GL_RENDERBUFFER,depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,w,h);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glBindFramebuffer(GL_FRAMEBUFFER,0);throw std::runtime_error("3D-Vorschau: Renderziel nicht verfügbar.");}
  glViewport(0,0,w,h);glDisable(GL_SCISSOR_TEST);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  auto model=multiply(rotation(pitch,true),multiply(rotation(yaw,false),translation(0,thumb?-.79f:-.5f,0)));
  // Centering is applied before the orbit; yaw keeps the body on its pedestal.
  auto mvp=multiply(perspective(float(w)/h),multiply(translation(0,0,-(thumb?.72f:1.8f)/zoom),model));
  glUseProgram(program);glUniformMatrix4fv(mvp_location,1,GL_FALSE,mvp.data());glUniformMatrix4fv(model_location,1,GL_FALSE,model.data());glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,models[skin].texture);glBindVertexArray(models[skin].vao);glDrawArrays(GL_TRIANGLES,0,models[skin].count);
  glBindVertexArray(0);glBindBuffer(GL_ARRAY_BUFFER,0);glUseProgram(0);glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_DEPTH_TEST);glEnable(GL_BLEND);return target;
 }
 ~Impl(){if(!glDeleteBuffers)return;for(auto& m:models){if(m.vao)glDeleteVertexArrays(1,&m.vao);if(m.vbo)glDeleteBuffers(1,&m.vbo);if(m.texture)glDeleteTextures(1,&m.texture);if(m.thumbnail)glDeleteTextures(1,&m.thumbnail);}if(output)glDeleteTextures(1,&output);if(depth)glDeleteRenderbuffers(1,&depth);if(framebuffer)glDeleteFramebuffers(1,&framebuffer);if(program)glDeleteProgram(program);}
};
Preview::Preview():impl(std::make_unique<Impl>()){}Preview::~Preview()=default;
bool Preview::load(const std::filesystem::path& path){try{
 std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Skin-Vorschaudaten fehlen (skins.bin).");
 auto read=[&](void* bytes,size_t size){if(!input.read(static_cast<char*>(bytes),size))throw std::runtime_error("Skin-Vorschaudaten sind unvollständig.");};auto u32=[&](){uint32_t value;read(&value,4);return value;};char magic[8];read(magic,8);if(std::string(magic,8)!="SSNESKN1" || u32()!=11)throw std::runtime_error("Skin-Vorschaudaten sind ungültig.");
 impl->initialize();impl->models.resize(11);
 for(unsigned i=0;i<11;++i){if(u32()!=i)throw std::runtime_error("Skin-Reihenfolge ist ungültig.");auto n=u32();if(n>64)throw std::runtime_error("Skin-Name ist ungültig.");std::string name(n,' ');read(name.data(),n);if(name!=names[i])throw std::runtime_error("Skin-Katalog stimmt nicht überein.");auto count=u32(),w=u32(),h=u32();if(!count || count>60000 || count%3 || !w || !h || w>1024 || h>1024)throw std::runtime_error("Skin-Modell ist ungültig.");
  std::vector<unsigned char> pixels(size_t(w)*h*4);read(pixels.data(),pixels.size());std::vector<float> vertices(size_t(count)*8);read(vertices.data(),vertices.size()*4);for(auto v:vertices)if(!std::isfinite(v) || std::abs(v)>10000)throw std::runtime_error("Skin-Geometrie ist ungültig.");
  auto& m=impl->models[i];m.count=count;m.texture=impl->texture(w,h,pixels.data());impl->glGenVertexArrays(1,&m.vao);impl->glBindVertexArray(m.vao);impl->glGenBuffers(1,&m.vbo);impl->glBindBuffer(GL_ARRAY_BUFFER,m.vbo);impl->glBufferData(GL_ARRAY_BUFFER,vertices.size()*4,vertices.data(),GL_STATIC_DRAW);
  for(unsigned a=0;a<3;++a){impl->glEnableVertexAttribArray(a);impl->glVertexAttribPointer(a,a==2?2:3,GL_FLOAT,GL_FALSE,32,reinterpret_cast<void*>(uintptr_t(a==0?0:a==1?12:24)));}
 }
 impl->glBindVertexArray(0);impl->glBindBuffer(GL_ARRAY_BUFFER,0);for(unsigned i=0;i<11;++i)impl->models[i].thumbnail=impl->draw(i,160,160,-10,0,1,true);return true;
 }catch(const std::exception& e){impl->error=e.what();return false;}}
const std::string& Preview::error()const{return impl->error;}
unsigned Preview::thumbnail(unsigned skin){return skin<impl->models.size()?impl->models[skin].thumbnail:0;}
unsigned Preview::render(unsigned skin,unsigned w,unsigned h,float yaw,float pitch,float zoom){if(!impl->error.empty())return 0;try{return impl->draw(skin,w,h,yaw,pitch,zoom);}catch(const std::exception& e){impl->error=e.what();return 0;}}
}
