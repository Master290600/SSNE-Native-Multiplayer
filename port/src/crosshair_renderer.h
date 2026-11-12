#pragma once
#include <string>

namespace port {
struct Settings;
struct MenuInfo;

// UI/display-thread only, with the application's OpenGL context current.
// Apply before ImGui::NewFrame, never while draw data still references an old
// preview texture. Only the core renderer substitutes the actual game reticle.
void initialize_crosshair_renderer(const std::string& executable_directory);
void update_crosshair_renderer(Settings&,bool reload=false);
void fill_crosshair_menu_info(MenuInfo&);
void shutdown_crosshair_renderer();
}
