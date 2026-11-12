// SSNE-Native launcher: asks for the user's GameCube disc, verifies it, installs the game data and
// launches the PC port.
#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <exception>
#include <string>

#include "platform.h"

#include "launcher_app.h"
#include "theme.h"

// Development: SSNE_SCREENSHOT=file.ppm saves the window after ~1 s and quits.
static void save_screenshot(SDL_Window* win, const char* path) {
    int w, h;
    SDL_GL_GetDrawableSize(win, &w, &h);
    std::vector<unsigned char> px(size_t(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
#ifdef _WIN32
    FILE* output = _wfopen(std::filesystem::u8path(path).c_str(), L"wb");
#else
    FILE* output = fopen(path, "wb");
#endif
    if (FILE* f = output) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(&px[size_t(y) * w * 3], 1, size_t(w) * 3, f);
        fclose(f);
    }
}

// Pixels per window point (display scaling on Windows, Wayland): the fonts are rebuilt when it changes, e.g. when the
// window moves to a screen with another scale.
static float g_ui_scale = 0;
static void update_ui_scale(SDL_Window* win) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    float s = ww > 0 ? std::clamp(std::round(float(dw) / ww * 4) / 4, 1.0f, 3.0f) : 1.0f;
    if (s == g_ui_scale) return;
    bool rebuild = g_ui_scale != 0;
    g_ui_scale = s;
    theme::load_fonts(s);
    if (rebuild) { ImGui_ImplOpenGL3_DestroyFontsTexture(); ImGui_ImplOpenGL3_CreateFontsTexture(); }
}

static const char* startup_stage = "initializing";
static int launcher_main(int argc, char** argv) {
    bool updated = false;  // started by the self-update
    for (int i = 1; i < argc; i++) updated |= strcmp(argv[i], "--updated") == 0;
#ifdef _WIN32
    // Windows display scaling: SDL works in scaled points and the window gets all its pixels (unaware of it, the
    // window is rendered at 1/scale of its size and stretched, blurry).
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    const bool hidden = getenv("SSNE_TEST_HIDDEN") && strcmp(getenv("SSNE_TEST_HIDDEN"), "1") == 0;
    const auto command_file = hidden ? platform::environment_utf8("SSNE_LAUNCHER_TEST_COMMAND") : std::string{};
    const char* command_path = command_file.empty() ? nullptr : command_file.c_str();
    SDL_Window* win = SDL_CreateWindow("Serious Sam: Next Encounter Native", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1024, 640,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!win) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 860, 560);
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    bool core = gl != nullptr;
    if (!gl) {  // older drivers: any 3.0 context
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        gl = SDL_GL_CreateContext(win);
    }
    if (!gl) { fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    startup_stage = "loading launcher fonts";
    update_ui_scale(win);
    theme::apply_style();
    const auto screenshot_path = platform::environment_utf8("SSNE_SCREENSHOT");
    const char* shot = screenshot_path.empty() ? nullptr : screenshot_path.c_str();
    uint64_t frames = 0, accepted_sequence = 0;
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init(core ? "#version 330 core" : "#version 130");

    {
        startup_stage = "reading launcher configuration and build kit";
        LauncherApp app(updated);
        bool quit = false;
        while (!quit && !app.wants_quit()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL2_ProcessEvent(&e);
                if (e.type == SDL_QUIT) quit = true;
                if (e.type == SDL_DROPFILE) {
                    app.on_file_dropped(e.drop.file);
                    SDL_free(e.drop.file);
                }
            }
            update_ui_scale(win);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            std::string requested_shot;
            if (command_path) {
                // Feed the normal SDL/ImGui consumers without moving the desktop pointer.
                ImGui::GetIO().AddFocusEvent(true);
                std::ifstream input(std::filesystem::u8path(command_path));
                std::string line;
                while (std::getline(input, line)) {
                    std::istringstream command(line);
                    uint64_t sequence = 0; std::string action;
                    if (!(command >> sequence >> action) || sequence <= accepted_sequence) continue;
                    SDL_Event event{};
                    const Uint32 window_id = SDL_GetWindowID(win);
                    if (action == "move" || action == "down" || action == "up") {
                        float x = 0, y = 0; std::string button;
                        if (action != "move") command >> button;
                        if (!(command >> x >> y) || (action != "move" && button != "left")) continue;
                        if (action == "move") {
                            event.type = SDL_MOUSEMOTION; event.motion.windowID = window_id;
                            event.motion.x = int(x); event.motion.y = int(y);
                        } else {
                            // Position and button are independent events, just as in the backend.
                            ImGui::GetIO().AddMousePosEvent(x, y);
                            event.type = action == "down" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                            event.button.windowID = window_id; event.button.button = SDL_BUTTON_LEFT;
                            event.button.x = int(x); event.button.y = int(y);
                        }
                        ImGui_ImplSDL2_ProcessEvent(&event);
                    } else if (action == "key") {
                        std::string state, key; command >> state >> key;
                        if (key != "Escape" || (state != "down" && state != "up")) continue;
                        event.type = state == "down" ? SDL_KEYDOWN : SDL_KEYUP;
                        event.key.windowID = window_id; event.key.keysym.sym = SDLK_ESCAPE;
                        event.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
                        ImGui_ImplSDL2_ProcessEvent(&event);
                    } else if (action == "drop" || action == "shot") {
                        std::string path; std::getline(command >> std::ws, path);
                        if (!path.empty() && path.back() == '\r') path.pop_back();
                        if (action == "drop") app.on_file_dropped(path);
                        else requested_shot = path;
                    } else if (action == "quit") quit = true;
                    else continue;
                    accepted_sequence = sequence;
                }
            }
            ImGui::NewFrame();
            startup_stage = "drawing launcher";
            app.frame();
            ImGui::Render();
            int w, h;
            SDL_GL_GetDrawableSize(win, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.01f, 0.02f, 0.10f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            ++frames;
            if (!requested_shot.empty()) save_screenshot(win, requested_shot.c_str());
            if (shot && frames == 60) { save_screenshot(win, shot); quit = true; }
            startup_stage = "writing hidden test state";
            if (command_path) app.save_test_state(platform::exe_dir() + "/launcher-test-state.json", accepted_sequence, frames);
            SDL_GL_SwapWindow(win);
        }
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int main(int argc,char** argv) {
    try { return launcher_main(argc,argv); }
    catch(const std::exception& error) {
        fprintf(stderr,"Launcher failed while %s: %s\n",startup_stage,error.what());
        fflush(stderr);
        const char* hidden=getenv("SSNE_TEST_HIDDEN");
        if (!hidden || strcmp(hidden,"1")!=0) {
            const std::string message=std::string("The launcher could not start: ")+error.what();
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"SSNE-Native",message.c_str(),nullptr);
        }
        return 1;
    }
}
