#pragma once
#include "map_document.h"
#include "map_scene.h"
#include <string>

namespace mapedit {
enum class TransformMode { Move, Rotate, Scale };
enum class GizmoHandle : int {
  None = -1,
  X = 0,
  Y = 1,
  Z = 2,
  XY = 3,
  XZ = 4,
  YZ = 5,
  Uniform = 6
};
const char *transform_mode_name(TransformMode);
bool transform_mode_available(uint32_t type, TransformMode);
bool transform_handle_available(uint32_t type, TransformMode, GizmoHandle);
const char *transform_limit_reason(uint32_t type);
V3 matrix_position(const Matrix &);
std::array<V3, 3> gizmo_axes(const Matrix &, bool local);
float gizmo_world_size(const Camera &, V3 pivot, float pixels = 92);
struct GizmoPath {
  GizmoHandle handle = GizmoHandle::None;
  std::vector<Projected> points;
  bool filled = false;
};
std::vector<GizmoPath> gizmo_paths(const Camera &, const Matrix &,
                                   TransformMode, bool local, uint32_t type);
GizmoHandle gizmo_pick(const std::vector<GizmoPath> &, float x, float y,
                       float tolerance = 8);
std::optional<V3> plane_point(Ray, V3 pivot, V3 normal);
Matrix transformed_rotation(const Matrix &, V3 axis, float radians);
Matrix transformed_scale(const Matrix &, V3 factors);
// A complete drag is computed from a frozen original matrix and camera. No
// document mutation is needed until mouse release, so one drag is one Undo.
struct GizmoDrag {
  bool active = false;
  TransformMode mode = TransformMode::Move;
  GizmoHandle handle = GizmoHandle::None;
  Matrix start{}, preview{};
  Camera camera;
  std::array<V3, 3> axes{};
  V3 pivot{}, anchor{}, plane_normal{}, start_vector{};
  float start_parameter = 0, world_size = 1, start_x = 0, start_y = 0,
        value = 0;
  float last_angle = 0, accumulated_angle = 0;
  bool axis_parameter_valid = false;
  bool begin(TransformMode, GizmoHandle, const Camera &, const Matrix &,
             bool local, float x, float y);
  bool update(float x, float y, bool snap, float move_grid = .25f,
              float angle_degrees = 15, float scale_step = .1f);
};
} // namespace mapedit
