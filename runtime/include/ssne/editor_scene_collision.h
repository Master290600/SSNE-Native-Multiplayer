#pragma once
#include <string>

namespace ssne {
// Corrects newly constructed PAL SceneModel collision bounds for nonuniform
// editor transforms. Runs once per native scene load, before world insertion.
bool install_editor_scene_collision(std::string& error);
}
