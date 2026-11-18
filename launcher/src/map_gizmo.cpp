#include "map_gizmo.h"
#include <algorithm>
#include <cmath>

namespace mapedit {
namespace {
constexpr float pi = 3.14159265358979323846f;
V3 unit(unsigned a) {
  V3 v;
  v[a] = 1;
  return v;
}
int axis_of(GizmoHandle h) { return int(h) >= 0 && int(h) < 3 ? int(h) : -1; }
std::array<int, 2> plane_axes(GizmoHandle h) {
  if (h == GizmoHandle::XY)
    return {0, 1};
  if (h == GizmoHandle::XZ)
    return {0, 2};
  return {1, 2};
}
float snap_value(float v, float step) {
  return step > 0 ? std::round(v / step) * step : v;
}
float segment(float x, float y, Projected a, Projected b) {
  const float dx = b.x - a.x, dy = b.y - a.y, n = dx * dx + dy * dy;
  const float t =
      n > 1e-8f ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / n, 0.f, 1.f)
                : 0;
  return std::hypot(x - a.x - t * dx, y - a.y - t * dy);
}
bool inside(float x, float y, const std::vector<Projected> &p) {
  bool in = false;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
    if (((p[i].y > y) != (p[j].y > y)) &&
        x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
      in = !in;
  return in;
}
} // namespace
const char *transform_mode_name(TransformMode m) {
  return m == TransformMode::Move     ? "Move"
         : m == TransformMode::Rotate ? "Rotate"
                                      : "Scale";
}
bool transform_mode_available(uint32_t t, TransformMode m) {
  return t != 4070 && (m == TransformMode::Move || t == 4200 || t == 4260 ||
                      (t == 4040 && m == TransformMode::Rotate));
}
bool transform_handle_available(uint32_t t, TransformMode m, GizmoHandle h) {
  return transform_mode_available(t, m) &&
         !(t == 4040 && m == TransformMode::Rotate && h != GizmoHandle::Y);
}
const char *transform_limit_reason(uint32_t t) {
  return t == 4070
             ? "Moving doors/brushes use authored motion paths; transform "
               "editing requires path/geometry editing."
         : t == 4200 || t == 4260
             ? "Native model: move, rotate and scale its authored transform."
         : t == 4040 ? "Enemy spawner: move and Y rotation. Native actors use "
                       "yaw; size comes from the enemy model."
                     : "Point entity: move its origin. Configure radius, "
                       "bounds or direction in its properties; the game does "
                       "not apply a model scale.";
}
V3 matrix_position(const Matrix &m) { return {m[3], m[7], m[11]}; }
std::array<V3, 3> gizmo_axes(const Matrix &m, bool local) {
  std::array<V3, 3> out;
  for (unsigned a = 0; a < 3; ++a)
    out[a] = local ? normalized({m[a], m[4 + a], m[8 + a]}) : unit(a);
  return out;
}
float gizmo_world_size(const Camera &c, V3 pivot, float pixels) {
  const auto q = c.project(pivot);
  if (!q)
    return 0;
  return std::max(.00001f, q->depth * 2.f * std::tan(pi / 6) * pixels /
                               std::max(1.f, c.height));
}
std::vector<GizmoPath> gizmo_paths(const Camera &c, const Matrix &m,
                                   TransformMode mode, bool local,
                                   uint32_t type) {
  std::vector<GizmoPath> out;
  if (!transform_mode_available(type, mode))
    return out;
  const auto origin = matrix_position(m);
  const auto axes = gizmo_axes(m, local || mode == TransformMode::Scale);
  const auto size = gizmo_world_size(c, origin);
  if (size <= 0)
    return out;
  auto path = [&](GizmoHandle h, const std::vector<V3> &points,
                  bool filled = false) {
    GizmoPath p;
    p.handle = h;
    p.filled = filled;
    for (auto v : points) {
      auto q = c.project(v);
      if (!q)
        return;
      p.points.push_back(*q);
    }
    out.push_back(std::move(p));
  };
  if (mode == TransformMode::Rotate) {
    for (unsigned a = 0; a < 3; ++a)
      if (transform_handle_available(type, mode, GizmoHandle(a))) {
        std::vector<V3> ring;
        for (unsigned n = 0; n <= 96; ++n) {
          float r = n * 2 * pi / 96;
          ring.push_back(origin + (axes[(a + 1) % 3] * std::cos(r) +
                                   axes[(a + 2) % 3] * std::sin(r)) *
                                      size);
        }
        path(GizmoHandle(a), ring);
      }
    return out;
  }
  for (unsigned a = 0; a < 3; ++a)
    path(GizmoHandle(a),
         {origin + axes[a] * size * .12f, origin + axes[a] * size});
  if (mode == TransformMode::Move) {
    for (auto h : {GizmoHandle::XY, GizmoHandle::XZ, GizmoHandle::YZ}) {
      auto pair = plane_axes(h);
      auto a = axes[pair[0]] * size, b = axes[pair[1]] * size;
      path(h,
           {origin + a * .23f + b * .23f, origin + a * .41f + b * .23f,
            origin + a * .41f + b * .41f, origin + a * .23f + b * .41f,
            origin + a * .23f + b * .23f},
           true);
    }
  }
  if (mode == TransformMode::Scale) {
    auto q = c.project(origin);
    if (q) {
      GizmoPath p;
      p.handle = GizmoHandle::Uniform;
      p.filled = true;
      for (auto xy :
           {std::array<float, 2>{-6, -6}, {6, -6}, {6, 6}, {-6, 6}, {-6, -6}})
        p.points.push_back({q->x + xy[0], q->y + xy[1], q->depth});
      out.push_back(std::move(p));
    }
  }
  return out;
}
GizmoHandle gizmo_pick(const std::vector<GizmoPath> &paths, float x, float y,
                       float tolerance) {
  GizmoHandle hit = GizmoHandle::None;
  float near = tolerance;
  for (const auto &p : paths) {
    if (p.points.size() < 2)
      continue;
    float d = 1e10f;
    if (p.filled && inside(x, y, p.points))
      d = 0;
    else
      for (size_t n = 1; n < p.points.size(); ++n)
        d = std::min(d, segment(x, y, p.points[n - 1], p.points[n]));
    if (d < near) {
      near = d;
      hit = p.handle;
    }
  }
  return hit;
}
std::optional<V3> plane_point(Ray ray, V3 pivot, V3 normal) {
  const float divisor = dot(ray.direction, normal);
  if (std::abs(divisor) < 1e-5f)
    return {};
  const float t = dot(pivot - ray.origin, normal) / divisor;
  if (t < 0 || !std::isfinite(t))
    return {};
  return ray.origin + ray.direction * t;
}
Matrix transformed_rotation(const Matrix &in, V3 axis, float radians) {
  const auto a = normalized(axis);
  const float c = std::cos(radians), s = std::sin(radians);
  auto out = in;
  for (unsigned col = 0; col < 3; ++col) {
    V3 v{in[col], in[4 + col], in[8 + col]};
    auto rotated = v * c + cross(a, v) * s + a * (dot(a, v) * (1 - c));
    for (unsigned row = 0; row < 3; ++row)
      out[row * 4 + col] = rotated[row];
  }
  return out;
}
Matrix transformed_scale(const Matrix &in, V3 factors) {
  auto out = in;
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 3; ++c)
      out[r * 4 + c] *= factors[c];
  return out;
}
bool GizmoDrag::begin(TransformMode kind, GizmoHandle h, const Camera &c,
                      const Matrix &matrix, bool local, float x, float y) {
  active = false;
  if (h == GizmoHandle::None || !std::isfinite(x) || !std::isfinite(y))
    return false;
  mode = kind;
  handle = h;
  camera = c;
  start = preview = matrix;
  pivot = matrix_position(matrix);
  axes = gizmo_axes(matrix, local || kind == TransformMode::Scale);
  world_size = gizmo_world_size(c, pivot);
  start_x = x;
  start_y = y;
  value = 0;
  const auto ray = c.ray(x, y);
  const int a = axis_of(h);
  if (kind == TransformMode::Rotate) {
    if (a < 0)
      return false;
    plane_normal = axes[a];
    auto p = plane_point(ray, pivot, plane_normal);
    if (!p || length(*p - pivot) < world_size * .05f)
      return false;
    start_vector = normalized(*p - pivot);
  } else if (a >= 0) {
    const auto t = axis_parameter(ray, pivot, axes[a]);
    axis_parameter_valid = bool(t);
    start_parameter = t.value_or(0);
  } else if (kind == TransformMode::Move) {
    auto pair = plane_axes(h);
    plane_normal = normalized(cross(axes[pair[0]], axes[pair[1]]));
    auto p = plane_point(ray, pivot, plane_normal);
    if (!p)
      return false;
    anchor = *p;
  }
  last_angle = accumulated_angle = 0;
  active = true;
  return true;
}
bool GizmoDrag::update(float x, float y, bool snap, float move_grid,
                       float angle_degrees, float scale_step) {
  if (!active || !std::isfinite(x) || !std::isfinite(y))
    return false;
  const auto ray = camera.ray(x, y);
  const int a = axis_of(handle);
  if (mode == TransformMode::Rotate) {
    auto p = plane_point(ray, pivot, plane_normal);
    if (!p || length(*p - pivot) < world_size * .01f)
      return false;
    auto v = normalized(*p - pivot);
    float angle = std::atan2(dot(plane_normal, cross(start_vector, v)),
                             dot(start_vector, v));
    accumulated_angle += std::remainder(angle - last_angle, 2 * pi);
    last_angle = angle;
    angle = accumulated_angle;
    if (snap)
      angle = snap_value(angle, angle_degrees * pi / 180);
    value = angle * 180 / pi;
    preview = transformed_rotation(start, plane_normal, angle);
    return true;
  }
  float delta = 0;
  if (a >= 0) {
    auto now = axis_parameter(ray, pivot, axes[a]);
    if (axis_parameter_valid && now)
      delta = *now - start_parameter;
    else {
      auto p = camera.project(pivot),
           q = camera.project(pivot + axes[a] * world_size);
      if (!p || !q)
        return false;
      const float dx = q->x - p->x, dy = q->y - p->y, n = dx * dx + dy * dy;
      if (n < 25)
        return false;
      delta = ((x - start_x) * dx + (y - start_y) * dy) / n * world_size;
    }
  }
  if (mode == TransformMode::Move) {
    V3 shift{};
    if (a >= 0) {
      if (snap)
        delta = snap_value(delta, move_grid);
      shift = axes[a] * delta;
      value = delta;
    } else {
      auto p = plane_point(ray, pivot, plane_normal);
      if (!p)
        return false;
      auto pair = plane_axes(handle);
      for (auto n : pair) {
        float d = dot(*p - anchor, axes[n]);
        if (snap)
          d = snap_value(d, move_grid);
        shift = shift + axes[n] * d;
      }
      value = length(shift);
    }
    preview = start;
    for (unsigned n = 0; n < 3; ++n)
      preview[n * 4 + 3] = pivot[n] + shift[n];
    return true;
  }
  float factor = 1 + (a >= 0 ? delta / std::max(.00001f, world_size)
                             : ((x - start_x) - (y - start_y)) / 120.f);
  if (snap)
    factor = 1 + snap_value(factor - 1, scale_step);
  factor = std::clamp(factor, .01f, 100.f);
  V3 factors{1, 1, 1};
  if (a >= 0)
    factors[a] = factor;
  else
    factors = {factor, factor, factor};
  value = factor;
  preview = transformed_scale(start, factors);
  return true;
}
} // namespace mapedit
