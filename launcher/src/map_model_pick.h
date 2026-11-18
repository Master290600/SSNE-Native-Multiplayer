#pragma once
#include "map_appearance.h"
#include <functional>
#include <limits>
#include <optional>

namespace mapedit {
struct ModelHit {
  size_t object = 0, visual_object = 0, batch = 0;
  uint32_t triangle = 0; // triangle index relative to the visual batch
  float distance = 0;    // world distance, independent of input ray magnitude
  V3 point{}, normal{};
};
struct ModelPickOptions {
  float maximum = std::numeric_limits<float>::infinity();
  float cut_height = std::numeric_limits<float>::infinity();
  bool show_enemies = true, alpha_test = true;
  int preview_object = -1;
  V3 preview_position{};
  const Matrix *transform_preview = nullptr;
  // Receives the logical owner (4180 for a proven paired button visual). A
  // caller can keep category/search/visibility filters consistent with icons.
  std::function<bool(size_t)> accept_object;
};
struct ModelPickStats {
  size_t batches = 0, bounds_tested = 0, bounds_hit = 0, triangles_tested = 0;
};
// Call only when selecting, not each rendered frame. Appearance::load caches
// bounds; synthetic/custom geometry must call prepare_pick_bounds after edits.
// For visible-surface picking pass the nearest Scene terrain distance (+ a
// small contact tolerance) as maximum; X-ray mode can leave maximum infinite.
std::optional<ModelHit> pick_model(const Appearance &, const Document &, Ray,
                                   const ModelPickOptions & = {},
                                   ModelPickStats * = nullptr);
} // namespace mapedit
