#pragma once
#include <SDL.h>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace port {
// The loader and its companion DLL stay resident while SDL owns a context.
// Loading by full path keeps the launcher and the default system driver isolated.
class ZinkRuntime {
public:
    bool load(const std::filesystem::path& runtime, std::string& error);
    bool restore_system_gl(std::string& error);
    bool present(std::string& error) const;
private:
#ifdef _WIN32
    HDC (WINAPI *current_dc_)() = nullptr;
    BOOL (WINAPI *swap_)(HDC) = nullptr;
    bool loaded_ = false;
#endif
};

inline bool ZinkRuntime::load(const std::filesystem::path& runtime, std::string& error) {
#ifdef _WIN32
    const auto driver = runtime / "libgallium_wgl.dll";
    const auto loader = runtime / "opengl32.dll";
    constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    if (SDL_setenv("GALLIUM_DRIVER", "zink", 1) != 0 ||
        !SetEnvironmentVariableW(L"GALLIUM_DRIVER", L"zink")) {
        error = "Could not select the Mesa Zink driver.";
        return false;
    }
    HMODULE mesa = nullptr;
    for (const auto& path : {driver, loader}) {
        mesa = LoadLibraryExW(path.c_str(), nullptr, flags);
        if (!mesa) {
            error = "Could not load " + path.filename().string() + " from the Vulkan directory (Windows error " +
                    std::to_string(GetLastError()) + ").";
            return false;
        }
    }
    current_dc_ = reinterpret_cast<decltype(current_dc_)>(GetProcAddress(mesa, "wglGetCurrentDC"));
    swap_ = reinterpret_cast<decltype(swap_)>(GetProcAddress(mesa, "wglSwapBuffers"));
    if (!current_dc_ || !swap_) {
        error = "The private Mesa runtime is missing its WGL presentation functions.";
        return false;
    }
    const auto utf8 = loader.u8string();
    if (SDL_GL_LoadLibrary(reinterpret_cast<const char*>(utf8.c_str())) != 0) {
        error = SDL_GetError();
        return false;
    }
    loaded_ = true;
    return true;
#else
    (void)runtime;
    error = "The bundled Mesa Zink renderer is available on Windows.";
    return false;
#endif
}

inline bool ZinkRuntime::restore_system_gl(std::string& error) {
#ifdef _WIN32
    if (loaded_) {
        SDL_GL_UnloadLibrary();
        loaded_ = false;
    }
    // Mesa and the system driver share the filename opengl32.dll. Explicitly
    // select System32 when retrying in a process that has already loaded both.
    wchar_t directory[MAX_PATH];
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = "The Windows system OpenGL directory is unavailable.";
        return false;
    }
    const auto path = (std::filesystem::path(directory) / L"opengl32.dll").u8string();
    if (SDL_GL_LoadLibrary(reinterpret_cast<const char*>(path.c_str())) != 0) {
        error = SDL_GetError();
        return false;
    }
#else
    (void)error;
#endif
    return true;
}

inline bool ZinkRuntime::present(std::string& error) const {
#ifdef _WIN32
    // SDL 2.30 uses the system GDI SwapBuffers on Windows. The private WGL
    // driver owns this drawable, so it must also perform its Vulkan present.
    const HDC dc = current_dc_();
    if (!dc) {
        error = "Mesa has no current window device context.";
        return false;
    }
    if (swap_(dc)) return true;
    error = "Mesa could not present the frame (Windows error " + std::to_string(GetLastError()) + ").";
#else
    error = "The bundled Mesa Zink renderer is available on Windows.";
#endif
    return false;
}
}
