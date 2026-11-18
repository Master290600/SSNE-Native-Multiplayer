#include "map_scene.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace mapedit {
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float v) { return {a.x * v, a.y * v, a.z * v}; }
V3 operator/(V3 a, float v) { return a * (1.f / v); }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalized(V3 a) {
  float n = length(a);
  return n > 1e-12f ? a / n : V3{};
}
namespace {
bool finite(V3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
void need(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}
uint32_t u32(const std::vector<uint8_t> &b, uint64_t p) {
  need(p + 4 <= b.size(), "3D geometry pointer is out of range");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
uint16_t u16(const std::vector<uint8_t> &b, uint64_t p) {
  need(p + 2 <= b.size(), "3D triangle pointer is out of range");
  return uint16_t(b[p]) << 8 | b[p + 1];
}
V3 minimum(V3 a, V3 b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
V3 maximum(V3 a, V3 b) {
  return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
Bounds empty() {
  float v = std::numeric_limits<float>::infinity();
  return {{v, v, v}, {-v, -v, -v}};
}
void include(Bounds &b, V3 p) {
  b.min = minimum(b.min, p);
  b.max = maximum(b.max, p);
}
bool box_hit(const Bounds &b, Ray r, float limit) {
  float near = 0, far = limit;
  for (unsigned i = 0; i < 3; ++i) {
    if (std::abs(r.direction[i]) < 1e-10f) {
      if (r.origin[i] < b.min[i] || r.origin[i] > b.max[i])
        return false;
    } else {
      float a = (b.min[i] - r.origin[i]) / r.direction[i],
            c = (b.max[i] - r.origin[i]) / r.direction[i];
      if (a > c)
        std::swap(a, c);
      near = std::max(near, a);
      far = std::min(far, c);
      if (far < near)
        return false;
    }
  }
  return true;
}
} // namespace
Scene Scene::parse(const std::vector<uint8_t> &data) {
  need(data.size() >= 0x80 && u32(data, 0) == 1 && u32(data, 4) == 100,
       "Unsupported 3D map root");
  const uint32_t descriptor = u32(data, 0x10), mesh = u32(data, 0x50);
  need(descriptor >= 0x80 && mesh >= 0x80 &&
           uint64_t(mesh) + 0x4c <= data.size(),
       "Missing level geometry descriptor");
  const uint32_t positions = u32(data, descriptor),
                 bytes = u32(data, descriptor + 4),
                 count = u32(data, descriptor + 8),
                 stride = u32(data, descriptor + 12);
  need(positions >= 0x80 && stride == 12 && count > 0 && count <= 1000000 &&
           uint64_t(count) * 12 == bytes &&
           uint64_t(positions) + bytes <= data.size(),
       "Invalid level vertex buffer");
  need(u32(data, mesh + 8) == positions,
       "Level geometry uses an unsupported vertex layout");
  const uint32_t indices = u32(data, mesh + 12), faces = u32(data, mesh + 0x48);
  need(indices >= 0x80 && faces > 0 && faces <= 2000000 &&
           uint64_t(indices) + uint64_t(faces) * 10 <= data.size(),
       "Invalid level triangle buffer");
  Scene out;
  out.vertices_.reserve(count);
  out.triangles_.reserve(faces);
  out.bounds_ = empty();
  for (uint32_t i = 0; i < count; ++i) {
    V3 v;
    for (unsigned k = 0; k < 3; ++k)
      v[k] =
          std::bit_cast<float>(u32(data, uint64_t(positions) + i * 12 + k * 4));
    need(finite(v) && std::abs(v.x) <= 1000000 && std::abs(v.y) <= 1000000 &&
             std::abs(v.z) <= 1000000,
         "Non-finite/out-of-range level vertex");
    out.vertices_.push_back(v);
  }
  // Original packed collision faces: flags, three position indices, plane
  // index. These are real level surfaces; never infer triangles from arbitrary
  // bytes.
  for (uint32_t i = 0; i < faces; ++i) {
    uint64_t p = uint64_t(indices) + i * 10;
    Triangle t{u16(data, p + 2), u16(data, p + 4), u16(data, p + 6)};
    need(t.a < count && t.b < count && t.c < count,
         "Level triangle references a missing vertex");
    V3 a = out.vertices_[t.a], b = out.vertices_[t.b], c = out.vertices_[t.c];
    if (length(cross(b - a, c - a)) < 1e-7f)
      continue;
    out.triangles_.push_back(t);
    include(out.bounds_, a);
    include(out.bounds_, b);
    include(out.bounds_, c);
  }
  need(!out.triangles_.empty(), "Level geometry contains no valid surfaces");
  out.order_.resize(out.triangles_.size());
  std::iota(out.order_.begin(), out.order_.end(), 0);
  out.nodes_.reserve(out.triangles_.size() / 4 + 1);
  out.build(0, uint32_t(out.order_.size()));
  return out;
}
uint32_t Scene::build(uint32_t start, uint32_t count) {
  uint32_t index = uint32_t(nodes_.size());
  nodes_.push_back({});
  Bounds b = empty(), centres = empty();
  auto centre = [&](uint32_t i) {
    auto t = triangles_[i];
    return (vertices_[t.a] + vertices_[t.b] + vertices_[t.c]) / 3.f;
  };
  for (uint32_t i = start; i < start + count; ++i) {
    auto t = triangles_[order_[i]];
    include(b, vertices_[t.a]);
    include(b, vertices_[t.b]);
    include(b, vertices_[t.c]);
    include(centres, centre(order_[i]));
  }
  nodes_[index].box = b;
  if (count <= 12) {
    nodes_[index].start = start;
    nodes_[index].count = count;
    return index;
  }
  V3 span = centres.max - centres.min;
  unsigned axis = span.y > span.x ? 1 : 0;
  if (span.z > span[axis])
    axis = 2;
  uint32_t middle = start + count / 2;
  std::nth_element(order_.begin() + start, order_.begin() + middle,
                   order_.begin() + start + count, [&](uint32_t a, uint32_t c) {
                     return centre(a)[axis] < centre(c)[axis];
                   });
  uint32_t left = build(start, middle - start),
           right = build(middle, start + count - middle);
  nodes_[index].left = left;
  nodes_[index].right = right;
  return index;
}
std::optional<SurfaceHit> Scene::intersect(Ray ray, float maximum,
                                           float cut) const {
  if (nodes_.empty() || !finite(ray.origin) || !finite(ray.direction) ||
      length(ray.direction) < 1e-8f || !(maximum > 0) || std::isnan(cut))
    return {};
  ray.direction = normalized(ray.direction);
  float nearest = maximum;
  std::optional<SurfaceHit> result;
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    auto id = stack.back();
    stack.pop_back();
    auto &node = nodes_[id];
    if (!box_hit(node.box, ray, nearest))
      continue;
    if (!node.count) {
      stack.push_back(node.left);
      stack.push_back(node.right);
      continue;
    }
    for (uint32_t i = node.start; i < node.start + node.count; ++i) {
      auto ti = order_[i];
      auto t = triangles_[ti];
      V3 a = vertices_[t.a], e1 = vertices_[t.b] - a, e2 = vertices_[t.c] - a,
         p = cross(ray.direction, e2);
      float det = dot(e1, p);
      if (std::abs(det) < 1e-8f)
        continue;
      float inv = 1.f / det;
      V3 s = ray.origin - a;
      float u = dot(s, p) * inv;
      if (u < -.00001f || u > 1.00001f)
        continue;
      V3 q = cross(s, e1);
      float v = dot(ray.direction, q) * inv;
      if (v < -.00001f || u + v > 1.00001f)
        continue;
      float d = dot(e2, q) * inv;
      if (d < .001f || d >= nearest)
        continue;
      V3 point = ray.origin + ray.direction * d;
      if (point.y > cut)
        continue;
      nearest = d;
      result = SurfaceHit{point, normalized(cross(e1, e2)), d, ti};
    }
  }
  return result;
}
V3 Camera::eye() const {
  float c = std::cos(pitch);
  return target +
         V3{std::sin(yaw) * c, std::sin(pitch), std::cos(yaw) * c} * distance;
}
V3 Camera::forward() const { return normalized(target - eye()); }
// The original game camera looks along local +Z with +X to its right.
// Convert its depth to OpenGL in matrix(), keeping this left-handed basis
// shared by rendering, picking, movement and gizmo dragging.
V3 Camera::right() const { return normalized(cross({0, 1, 0}, forward())); }
V3 Camera::up() const { return normalized(cross(forward(), right())); }
std::array<float, 16> Camera::matrix() const {
  auto r = right(), u = up(), f = forward(), e = eye();
  std::array<float, 16> view{r.x,        u.x,        -f.x,      0,   r.y,  u.y,
                             -f.y,       0,          r.z,       u.z, -f.z, 0,
                             -dot(r, e), -dot(u, e), dot(f, e), 1};
  float near = .05f, far = 2000000.f,
        aspect = std::max(.01f, width / std::max(1.f, height)),
        s = 1.f / std::tan(.5235987756f);
  std::array<float, 16> projection{s / aspect,
                                   0,
                                   0,
                                   0,
                                   0,
                                   s,
                                   0,
                                   0,
                                   0,
                                   0,
                                   -(far + near) / (far - near),
                                   -1,
                                   0,
                                   0,
                                   -2 * far * near / (far - near),
                                   0},
      out{};
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned row = 0; row < 4; ++row)
      for (unsigned k = 0; k < 4; ++k)
        out[c * 4 + row] += projection[k * 4 + row] * view[c * 4 + k];
  return out;
}
std::optional<Projected> Camera::project(V3 p) const {
  V3 delta = p - eye();
  float z = dot(delta, forward());
  if (!finite(p) || z < .05f)
    return {};
  float scale = height / (2.f * std::tan(.5235987756f));
  return Projected{width * .5f + dot(delta, right()) * scale / z,
                   height * .5f - dot(delta, up()) * scale / z, z};
}
Ray Camera::ray(float x, float y) const {
  float scale = 2.f * std::tan(.5235987756f) / std::max(1.f, height);
  return {eye(), normalized(forward() + right() * ((x - width * .5f) * scale) +
                            up() * ((height * .5f - y) * scale))};
}
void Camera::orbit(float dx, float dy) {
  if(!std::isfinite(dx)||!std::isfinite(dy)||(dx==0&&dy==0))return;
  yaw = std::remainder(yaw + dx * .008f, 6.283185307f);
  pitch = std::clamp(pitch + dy * .008f, -1.48f, 1.48f);
}
void Camera::pan(float dx, float dy) {
  if(!std::isfinite(dx)||!std::isfinite(dy)||(dx==0&&dy==0))return;
  float scale = distance * 2.f * std::tan(.5235987756f) / std::max(1.f, height);
  target = target + right() * (-dx * scale) + up() * (dy * scale);
}
void Camera::look(float dx, float dy) {
  if(!std::isfinite(dx)||!std::isfinite(dy)||(dx==0&&dy==0))return;
  auto position = eye();
  orbit(dx, dy);
  target = position + forward() * distance;
}
void Camera::fly(float side, float vertical, float ahead, float dt,
                 float speed) {
  if (!std::isfinite(dt) || !std::isfinite(speed) || dt <= 0 || speed <= 0)
    return;
  V3 direction = right() * side + V3{0, 1, 0} * vertical + forward() * ahead;
  float magnitude = length(direction);
  if (magnitude > 1)
    direction = direction / magnitude;
  target = target + direction * (std::min(dt, .1f) * std::min(speed, 1000.f));
}
void Camera::zoom(float wheel) {
  if (std::isfinite(wheel))
    distance = std::clamp(distance * std::pow(.85f, wheel), .3f, 1000000.f);
}
void Camera::fit(Bounds b) {
  target = (b.min + b.max) * .5f;
  float radius = std::max(2.f, length(b.max - b.min) * .5f);
  distance = std::clamp(radius * 2.4f, .3f, 1000000.f);
}
void Camera::focus(V3 p, float radius) {
  if (finite(p) && std::isfinite(radius)) {
    target = p;
    distance = std::clamp(radius, .3f, 1000000.f);
  }
}
std::optional<float> axis_parameter(Ray ray, V3 p, V3 axis) {
  axis = normalized(axis);
  ray.direction = normalized(ray.direction);
  float cosine = dot(axis, ray.direction), denominator = 1 - cosine * cosine;
  if (denominator < .002f)
    return {};
  V3 delta = ray.origin - p;
  float result =
      (dot(axis, delta) - cosine * dot(ray.direction, delta)) / denominator;
  return std::isfinite(result) ? std::optional<float>(result) : std::nullopt;
}
std::optional<V3> horizontal_point(Ray ray, float y) {
  if (std::abs(ray.direction.y) < 1e-5f)
    return {};
  float t = (y - ray.origin.y) / ray.direction.y;
  if (t < 0 || !std::isfinite(t))
    return {};
  V3 p = ray.origin + ray.direction * t;
  return finite(p) ? std::optional<V3>(p) : std::nullopt;
}
V3 snapped(V3 p, float grid) {
  if (std::isfinite(grid) && grid > 0)
    for (unsigned i = 0; i < 3; ++i)
      p[i] = std::round(p[i] / grid) * grid;
  return p;
}
} // namespace mapedit
