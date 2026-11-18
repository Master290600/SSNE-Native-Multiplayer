#pragma once
#include "map_document.h"
#include "map_scene.h"
#include <filesystem>
#include <string>

namespace mapedit {
struct VisualVertex {
  V3 position, normal;
  float u = 0, v = 0, u1 = 0, v1 = 0, u2 = 0, v2 = 0;
  std::array<float, 4> color{1, 1, 1, 1};
};
struct VisualTexture {
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> rgba;
  bool translucent = false;
};
enum class PreviewPlacement { Scene, EnemyTemplate, EnemySpawner, Pickup, Arrow };
struct VisualBatch {
  PreviewPlacement placement = PreviewPlacement::Scene;
  // Native material base: 0=vertex, 1=texture, 2=texture * vertex.
  // Native blend option: 0=opaque, 1=alpha test, 2=premultiplied,
  // 3=source alpha, 4=additive. Mask alpha does not imply transparency.
  uint8_t base_mode = 2, blend_option = 0;
  uint32_t source_material = 0;
  uint32_t first = 0, count = 0;
  int texture = -1, object = -1;
  uint32_t matrix_offset = 0;
  Matrix matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  Matrix reference{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  uint8_t wrap_u = 1, wrap_v = 1;
  bool sky = false;
  bool enemy_preview = false;
  // Immutable geometry bounds, computed once at load. Model picking transforms
  // its ray into this local space; it never walks every triangle per frame.
  Bounds local_bounds{};
  bool pick_bounds_valid = false;
  std::array<int, 2> layers{-1, -1};
  std::array<unsigned, 2> operations{};
  std::array<uint8_t, 2> layer_u{1, 1}, layer_v{1, 1};
};
struct Appearance {
  struct Preview {
    int object = -1;
    std::string model, pose, role, error;
    size_t bones = 0;
  };
  std::vector<VisualVertex> vertices;
  std::vector<VisualTexture> textures;
  std::vector<VisualBatch> batches;
  std::vector<std::string> warnings;
  std::vector<Preview> previews;
  size_t instances = 0, pickups = 0;
  size_t enemies = 0, extra_objects = 0, arrows = 0;
  static Appearance load(const Document &, const std::filesystem::path &data);
  void prepare_pick_bounds();
  Matrix model_matrix(const VisualBatch &, const Document &,
                      int preview_object = -1, V3 preview = {},
                      const Matrix *transform_preview = nullptr) const;
};
} // namespace mapedit
