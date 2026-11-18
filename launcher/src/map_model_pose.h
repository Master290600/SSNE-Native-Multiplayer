#pragma once
#include "map_scene.h"
#include <string>
namespace mapedit {
struct ModelPose {
  std::vector<V3> positions, normals;
  std::string name;
  size_t bones = 0;
};
// Decode one authored idle/stand frame and its original weighted bone
// influences. This is a read-only editor preview, not a replacement
// animation/game update.
ModelPose preview_model_pose(const std::vector<uint8_t> &image, uint32_t model);
} // namespace mapedit
