#include "ui_gl_profile.h"
#include "ssne/performance_detail.h"
#include "imgui_impl_opengl3_loader.h"
#include <cstdio>
#include <cstring>

namespace {
// Use ImGui's own function table, not the game's GL dispatch. Forward every
// argument/result unchanged. Never modify the external dependency's sources.
decltype(imgl3wProcs.gl) original;
using namespace ssne::perf::detail;
#define PROFILE_VOID(Name, Args, Values, Stage) \
    void APIENTRY timed_##Name Args { Scope timer(Stage); original.Name Values; }
PROFILE_VOID(GetIntegerv, (GLenum n, GLint* p), (n,p), UiQuery)
PROFILE_VOID(BufferData, (GLenum t, GLsizeiptr n, const void* p, GLenum u), (t,n,p,u), UiUpload)
PROFILE_VOID(BufferSubData, (GLenum t, GLintptr o, GLsizeiptr n, const void* p), (t,o,n,p), UiUpload)
PROFILE_VOID(DrawElements, (GLenum m, GLsizei n, GLenum t, const void* p), (m,n,t,p), UiDraw)
PROFILE_VOID(DrawElementsBaseVertex, (GLenum m, GLsizei n, GLenum t, const void* p, GLint b), (m,n,t,p,b), UiDraw)
PROFILE_VOID(GenVertexArrays, (GLsizei n, GLuint* p), (n,p), UiVao)
PROFILE_VOID(DeleteVertexArrays, (GLsizei n, const GLuint* p), (n,p), UiVao)
PROFILE_VOID(BindVertexArray, (GLuint v), (v), UiVao)
#undef PROFILE_VOID
GLboolean APIENTRY timed_IsEnabled(GLenum name){Scope timer(UiQuery);return original.IsEnabled(name);}
GLboolean APIENTRY timed_IsProgram(GLuint name){Scope timer(UiProgramCheck);return original.IsProgram(name);}
}
namespace port {
void profile_ui_gl_calls(){
    static bool installed=false;
    const auto mode=std::getenv("SSNE_PROFILE_UI_GL");
    if(installed || !mode || std::strcmp(mode,"1") || !imgl3wProcs.gl.GetIntegerv)return;
    original=imgl3wProcs.gl;
#define INSTALL(Name) if(original.Name)imgl3wProcs.gl.Name=timed_##Name
    INSTALL(GetIntegerv);INSTALL(IsEnabled);INSTALL(IsProgram);
    INSTALL(BufferData);INSTALL(BufferSubData);INSTALL(DrawElements);INSTALL(DrawElementsBaseVertex);
    INSTALL(GenVertexArrays);INSTALL(DeleteVertexArrays);INSTALL(BindVertexArray);
#undef INSTALL
    installed=true;std::fputs("[renderer-profile] UI GL call sampling enabled\n",stdout);
}
}
