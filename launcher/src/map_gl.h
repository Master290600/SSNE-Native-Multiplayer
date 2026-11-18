#pragma once
#include "map_appearance.h"
#include "map_scene.h"
#include <cstdint>
#include <memory>

namespace mapedit {
// Owns only editor GL objects. All state touched by draw is restored before
// returning to the normal ImGui/launcher renderer.
class SceneRenderer {
public:
  SceneRenderer();
  ~SceneRenderer();
  SceneRenderer(const SceneRenderer &) = delete;
  SceneRenderer &operator=(const SceneRenderer &) = delete;
  uint32_t draw(const Scene &, uint64_t revision, const Camera &, int width,
                int height, float cut_height, bool wireframe,
                const Appearance *appearance, const Document &,
                int preview_object = -1, V3 preview = {},
                bool show_enemies = true,
                const Matrix *preview_matrix = nullptr);
  bool ready() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace mapedit
