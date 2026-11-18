#include "map_gizmo.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace mapedit;
namespace {
int checks = 0;
void check(bool v, const char *message) {
  ++checks;
  if (!v)
    throw std::runtime_error(message);
}
bool close(float a, float b, float tolerance = .001f) {
  return std::abs(a - b) < tolerance;
}
bool matrix_close(const Matrix &a, const Matrix &b, float tolerance = .001f) {
  for (unsigned n = 0; n < 12; ++n)
    if (!close(a[n], b[n], tolerance))
      return false;
  return true;
}
constexpr float pi = 3.14159265358979323846f;
const Matrix identity{1, 0, 0, 7, 0, 1, 0, 11, 0, 0, 1, 17};
Projected projected(const Camera &camera, V3 p) {
  auto q = camera.project(p);
  check(bool(q), "Test point behind camera");
  return *q;
}
V3 unit(unsigned n) {
  V3 p;
  p[n] = 1;
  return p;
}
} // namespace
int main() {
  try {
    Camera c;
    c.width = 1200;
    c.height = 700;
    c.focus(matrix_position(identity), 40);
    const auto eye = c.eye(), pivot = c.target, forward = c.forward();
    c.look(0, 0);
    c.orbit(0, 0);
    c.pan(0, 0);
    c.zoom(0);
    check(length(c.eye() - eye) == 0 && length(c.target - pivot) == 0,
          "Zero input changed camera pose");
    c.look(NAN, 1);
    c.pan(1, NAN);
    c.orbit(NAN, 1);
    c.focus({1, 2, 3}, NAN);
    check(length(c.eye() - eye) == 0 && length(c.target - pivot) == 0,
          "Invalid input damaged camera");
    auto looked = c;
    looked.look(11, 7);
    check(length(looked.eye() - eye) < .0001f,
          "Free look moved the camera eye");
    auto orbit = c;
    orbit.orbit(17, -11);
    check(length(orbit.target - pivot) == 0 &&
              close(length(orbit.eye() - pivot), 40),
          "Orbit changed its fixed pivot or radius");
    auto pan = c;
    pan.pan(11, 9);
    check(length((pan.eye() - eye) - (pan.target - pivot)) < .0001f,
          "Pan changed eye/pivot separation");
    auto fast = c, slow = c;
    for (int n = 0; n < 240; ++n)
      fast.fly(0, 0, 1, 1.f / 240, 20);
    for (int n = 0; n < 30; ++n)
      slow.fly(0, 0, 1, 1.f / 30, 20);
    check(length(fast.eye() - slow.eye()) < .003f, "Flight depends on FPS");
    for (float depth : {.4f, 4.f, 40.f, 4000.f}) {
      auto camera = c;
      camera.distance = depth;
      const float size = gizmo_world_size(camera, pivot);
      const auto a = projected(camera, pivot),
                 b = projected(camera, pivot + camera.right() * size);
      check(close(b.x - a.x, 92, .05f), "Gizmo size depends on zoom");
    }
    const auto size = gizmo_world_size(c, pivot);
    for (unsigned axis = 0; axis < 3; ++axis) {
      GizmoDrag drag;
      auto a = projected(c, pivot + unit(axis) * size * .8f),
           b = projected(c, pivot + unit(axis) * (size * .8f + 3));
      check(drag.begin(TransformMode::Move, GizmoHandle(axis), c, identity,
                       false, a.x, a.y),
            "Axis move did not start");
      check(drag.update(b.x, b.y, false), "Axis move did not update");
      auto expected = identity;
      expected[axis * 4 + 3] += 3;
      check(matrix_close(drag.preview, expected),
            "Axis move changed a locked coordinate");
      check(matrix_close(drag.start, identity),
            "Drag mutated its original matrix");
    }
    for (auto h : {GizmoHandle::XY, GizmoHandle::XZ, GizmoHandle::YZ}) {
      unsigned a = h == GizmoHandle::YZ ? 1 : 0,
               b = h == GizmoHandle::XY ? 1 : 2;
      auto start = projected(c, pivot + (unit(a) + unit(b)) * size * .3f),
           end = projected(c, pivot + (unit(a) + unit(b)) * size * .3f +
                                  unit(a) * 2 + unit(b) * 3);
      GizmoDrag drag;
      check(drag.begin(TransformMode::Move, h, c, identity, false, start.x,
                       start.y),
            "Plane move did not start");
      check(drag.update(end.x, end.y, false), "Plane move did not update");
      auto expected = identity;
      expected[a * 4 + 3] += 2;
      expected[b * 4 + 3] += 3;
      check(matrix_close(drag.preview, expected),
            "Plane move changed locked coordinates");
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
      auto u = unit((axis + 1) % 3);
      auto end_matrix = transformed_rotation(identity, unit(axis), .4f);
      V3 rotated{end_matrix[(axis + 1) % 3], end_matrix[4 + (axis + 1) % 3],
                 end_matrix[8 + (axis + 1) % 3]};
      auto start = projected(c, pivot + u * size),
           end = projected(c, pivot + rotated * size);
      GizmoDrag drag;
      check(drag.begin(TransformMode::Rotate, GizmoHandle(axis), c, identity,
                       false, start.x, start.y),
            "Rotation ring did not start");
      check(drag.update(end.x, end.y, false), "Rotation ring did not update");
      check(matrix_close(drag.preview, end_matrix),
            "Ring rotation differs from authored axis");
      const auto last_valid = drag.preview;
      check(!drag.update(std::numeric_limits<float>::quiet_NaN(), end.y, false),
            "Invalid mouse coordinates changed an active drag");
      check(matrix_close(drag.preview, last_valid),
            "Invalid pointer reset the preview to the original transform");
      if (axis == 0) {
        const auto scale = 2.f * std::tan(pi / 6) / c.height;
        const auto parallel_x = c.width * .5f -
            dot(c.forward(), unit(axis)) / (dot(c.right(), unit(axis)) * scale);
        check(!drag.update(parallel_x, c.height * .5f, false),
              "Parallel rotation plane should defer a drag update");
        check(matrix_close(drag.preview, last_valid),
              "A ray outside the drag plane reset the transform preview");
      }
    }
    {
      GizmoDrag drag;
      auto a = projected(c, pivot + unit(0) * size);
      check(drag.begin(TransformMode::Rotate, GizmoHandle::Y, c, identity,
                       false, a.x, a.y),
            "Continuous rotation start");
      for (unsigned n = 1; n <= 72; ++n) {
        float angle = n * pi / 18;
        auto q = projected(c, pivot + V3{std::cos(angle), 0, -std::sin(angle)} *
                                          size);
        check(drag.update(q.x, q.y, false), "Continuous rotation update");
      }
      check(close(drag.value, 720, .05f),
            "Rotation jumps at the 180-degree boundary");
    }
    {
      auto m = transformed_rotation(identity, {0, 1, 0}, pi / 4);
      auto axes = gizmo_axes(m, true);
      GizmoDrag drag;
      auto a = projected(c, pivot + axes[0] * size * .8f),
           b = projected(c, pivot + axes[0] * size * 1.3f);
      check(drag.begin(TransformMode::Scale, GizmoHandle::X, c, m, false, a.x,
                       a.y),
            "Scale did not start");
      check(drag.update(b.x, b.y, false), "Scale did not update");
      check(matrix_close(drag.preview, transformed_scale(m, {1.5f, 1, 1})),
            "Scale did not follow rotated local axis");
    }
    {
      GizmoDrag drag;
      auto p = projected(c, pivot);
      check(drag.begin(TransformMode::Scale, GizmoHandle::Uniform, c, identity,
                       false, p.x, p.y),
            "Uniform scale did not start");
      drag.update(p.x + 60, p.y, false);
      check(matrix_close(drag.preview,
                         transformed_scale(identity, {1.5f, 1.5f, 1.5f})),
            "Uniform scale changed origin or proportions");
    }
    {
      GizmoDrag drag;
      auto a = projected(c, pivot + unit(0) * size * .8f),
           b = projected(c, pivot + unit(0) * (size * .8f + 1.13f));
      drag.begin(TransformMode::Move, GizmoHandle::X, c, identity, false, a.x,
                 a.y);
      drag.update(b.x, b.y, true, .25f);
      check(close(drag.preview[3], identity[3] + 1.25f),
            "Grid snap did not use the drag origin");
    }
    check(transform_mode_available(4200, TransformMode::Scale) &&
              transform_mode_available(4200, TransformMode::Rotate),
          "Scene models do not expose full transforms");
    check(!transform_mode_available(5000, TransformMode::Scale) &&
              !transform_mode_available(4140, TransformMode::Rotate),
          "Point entity pretends to support model transforms");
    for (auto mode : {TransformMode::Move, TransformMode::Rotate, TransformMode::Scale})
      check(!transform_mode_available(4070, mode),
            "Moving brush permits editing without its native geometry/path");
    check(transform_handle_available(4040, TransformMode::Rotate,
                                     GizmoHandle::Y) &&
              !transform_handle_available(4040, TransformMode::Rotate,
                                          GizmoHandle::X),
          "Spawner allows unsupported pitch/roll");
    for (auto mode :
         {TransformMode::Move, TransformMode::Rotate, TransformMode::Scale}) {
      const auto paths = gizmo_paths(c, identity, mode, false, 4200);
      check(!paths.empty(), "Tool has no dynamic gizmo");
      for (const auto &path : paths)
        check(path.points.size() >= 2, "Gizmo generated incomplete path");
    }
    check(gizmo_paths(c, identity, TransformMode::Scale, false, 4140).empty(),
          "Unsupported point scale drawn");
    std::cout << "PASS camera/gizmo checks=" << checks << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
