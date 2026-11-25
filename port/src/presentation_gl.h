#pragma once
#include <SDL.h>
#include <SDL_opengl.h>

// Windows imports its default OpenGL DLL before main(). A Mesa context from a
// private directory needs every presentation call resolved against that context.
namespace port::gl {
#define PORT_GL_FUNCTIONS(X) \
    X(BindTexture) X(Clear) X(ClearColor) X(DeleteTextures) X(Disable) \
    X(GenTextures) X(GetError) X(GetIntegerv) X(GetString) X(PixelStorei) \
    X(ReadPixels) X(TexImage2D) X(TexParameteri) X(TexSubImage2D) X(Viewport)

#define PORT_GL_DECLARE(name) inline decltype(&::gl##name) name = &::gl##name;
PORT_GL_FUNCTIONS(PORT_GL_DECLARE)
#undef PORT_GL_DECLARE

inline bool use_current_context() {
#define PORT_GL_RESOLVE(name) \
    name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress("gl" #name)); \
    if (!name) return false;
    PORT_GL_FUNCTIONS(PORT_GL_RESOLVE)
#undef PORT_GL_RESOLVE
    return true;
}
#undef PORT_GL_FUNCTIONS
}
