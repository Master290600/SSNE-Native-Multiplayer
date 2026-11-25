// ImGui's default Windows loader opens "opengl32.dll" by name. Resolve its
// procedures through the same SDL context that renders the game, including
// when the game selects an isolated Mesa runtime.
#include <SDL.h>
#include <cstdio>
#define IMGL3W_IMPL
#include <imgui_impl_opengl3_loader.h>
#undef IMGL3W_IMPL
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#define ImGui_ImplOpenGL3_Init ImGui_ImplOpenGL3_InitAfterLoad
#include <imgui_impl_opengl3.cpp>
#undef ImGui_ImplOpenGL3_Init

bool ImGui_ImplOpenGL3_Init(const char* version) {
    auto resolve = [](const char* name) {
        return reinterpret_cast<GL3WglProc>(SDL_GL_GetProcAddress(name));
    };
    return imgl3wInit2(resolve) == GL3W_OK && ImGui_ImplOpenGL3_InitAfterLoad(version);
}
