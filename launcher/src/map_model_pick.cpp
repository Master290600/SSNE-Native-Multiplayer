#include "map_model_pick.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace mapedit {
namespace {
bool finite(V3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
V3 direction(const Matrix &m, V3 p) {
  return {m[0] * p.x + m[1] * p.y + m[2] * p.z,
          m[4] * p.x + m[5] * p.y + m[6] * p.z,
          m[8] * p.x + m[9] * p.y + m[10] * p.z};
}
V3 position(const Matrix &m, V3 p) {
  return direction(m, p) + V3{m[3], m[7], m[11]};
}
std::optional<Matrix> inverse(const Matrix &m) {
  for (auto value : m)
    if (!std::isfinite(value))
      return {};
  const double det =
      double(m[0]) * (double(m[5]) * m[10] - double(m[6]) * m[9]) -
      double(m[1]) * (double(m[4]) * m[10] - double(m[6]) * m[8]) +
      double(m[2]) * (double(m[4]) * m[9] - double(m[5]) * m[8]);
  if (!std::isfinite(det) || std::abs(det) < 1e-15)
    return {};
  Matrix out{float((double(m[5]) * m[10] - double(m[6]) * m[9]) / det),
             float((double(m[2]) * m[9] - double(m[1]) * m[10]) / det),
             float((double(m[1]) * m[6] - double(m[2]) * m[5]) / det),
             0,
             float((double(m[6]) * m[8] - double(m[4]) * m[10]) / det),
             float((double(m[0]) * m[10] - double(m[2]) * m[8]) / det),
             float((double(m[2]) * m[4] - double(m[0]) * m[6]) / det),
             0,
             float((double(m[4]) * m[9] - double(m[5]) * m[8]) / det),
             float((double(m[1]) * m[8] - double(m[0]) * m[9]) / det),
             float((double(m[0]) * m[5] - double(m[1]) * m[4]) / det),
             0};
  const auto offset = direction(out, {-m[3], -m[7], -m[11]});
  out[3] = offset.x;
  out[7] = offset.y;
  out[11] = offset.z;
  for (auto value : out)
    if (!std::isfinite(value))
      return {};
  return out;
}
std::optional<float> box_hit(const Bounds &b, Ray ray, float limit) {
  float low = 0, high = limit;
  for (unsigned i = 0; i < 3; ++i) {
    if (std::abs(ray.direction[i]) < 1e-15f) {
      if (ray.origin[i] < b.min[i] || ray.origin[i] > b.max[i])
        return {};
    } else {
      auto a = (b.min[i] - ray.origin[i]) / ray.direction[i],
           c = (b.max[i] - ray.origin[i]) / ray.direction[i];
      if (a > c)
        std::swap(a, c);
      low = std::max(low, a);
      high = std::min(high, c);
      if (high < low)
        return {};
    }
  }
  return low;
}
int texel(int p, int size, unsigned wrap) {
  if (wrap == 1)
    return std::clamp(p, 0, size - 1);
  const int period = wrap == 2 ? 2 * size : size;
  p %= period;
  if (p < 0)
    p += period;
  return wrap == 2 && p >= size ? 2 * size - 1 - p : p;
}
float texture_alpha(const Appearance &a, int index, float u, float v,
                    unsigned wu, unsigned wv) {
  if (index < 0 || size_t(index) >= a.textures.size())
    return 1;
  const auto &t = a.textures[size_t(index)];
  if (!t.width || !t.height || t.width > 65536 || t.height > 65536 ||
      uint64_t(t.width) * t.height != t.rgba.size() || !std::isfinite(u) ||
      !std::isfinite(v))
    return 1;
  // Base-level bilinear alpha follows the viewport sampler and native wrapping.
  // Geometry picking remains bounded to candidate triangle texture samples.
  auto coordinate = [](float value, unsigned wrap) {
    return wrap == 1 ? std::clamp(value, 0.f, 1.f)
                     : std::fmod(value, wrap == 2 ? 2.f : 1.f);
  };
  u = coordinate(u, wu);
  v = coordinate(v, wv);
  const double x = double(u) * t.width - .5, y = double(v) * t.height - .5;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const float fx = float(x - ix), fy = float(y - iy);
  auto sample = [&](int px, int py) {
    return float(t.rgba[size_t(texel(py, int(t.height), wv)) * t.width +
                        texel(px, int(t.width), wu)] >>
                 24) /
           255;
  };
  const float top = sample(ix, iy) * (1 - fx) + sample(ix + 1, iy) * fx;
  const float bottom =
      sample(ix, iy + 1) * (1 - fx) + sample(ix + 1, iy + 1) * fx;
  return top * (1 - fy) + bottom * fy;
}
bool opaque(const Appearance &a, const VisualBatch &b, const VisualVertex &v0,
            const VisualVertex &v1, const VisualVertex &v2, float u, float v) {
  if (b.blend_option != 1)
    return true;
  auto mix = [&](float a, float c, float d) {
    return a * (1 - u - v) + c * u + d * v;
  };
  const auto vertex = mix(v0.color[3], v1.color[3], v2.color[3]);
  const auto base = texture_alpha(a, b.texture, mix(v0.u, v1.u, v2.u),
                                  mix(v0.v, v1.v, v2.v), b.wrap_u, b.wrap_v);
  float alpha = b.base_mode == 0   ? vertex
                : b.base_mode == 1 ? base
                                   : base * vertex;
  for (unsigned layer = 0; layer < 2; ++layer) {
    const auto op = b.operations[layer];
    if (op != 1 && op != 2 && op != 7)
      continue;
    const auto ta = texture_alpha(
        a, b.layers[layer],
        layer ? mix(v0.u2, v1.u2, v2.u2) : mix(v0.u1, v1.u1, v2.u1),
        layer ? mix(v0.v2, v1.v2, v2.v2) : mix(v0.v1, v1.v1, v2.v1),
        b.layer_u[layer], b.layer_v[layer]);
    alpha = op == 1 ? alpha * ta : ta;
  }
  return alpha > 128.f / 255;
}
struct Candidate {
  size_t batch, owner;
  Ray local;
  Matrix matrix;
  float entry;
};
} // namespace
std::optional<ModelHit> pick_model(const Appearance &a, const Document &doc,
                                   Ray ray, const ModelPickOptions &options,
                                   ModelPickStats *stats) {
  ModelPickStats work;
  auto finish = [&](std::optional<ModelHit> value) {
    if (stats)
      *stats = work;
    return value;
  };
  const auto magnitude =
      std::hypot(double(ray.direction.x), double(ray.direction.y),
                 double(ray.direction.z));
  if (!finite(ray.origin) || !finite(ray.direction) || magnitude < 1e-8 ||
      !(options.maximum > 0) || std::isnan(options.cut_height))
    return finish({});
  for (unsigned axis = 0; axis < 3; ++axis)
    ray.direction[axis] = float(double(ray.direction[axis]) / magnitude);
  std::vector<Candidate> candidates;
  std::unordered_map<size_t, int> owners;
  std::unordered_map<size_t, bool> accepted;
  for (size_t i = 0; i < a.batches.size(); ++i) {
    const auto &b = a.batches[i];
    ++work.batches;
    if (!b.pick_bounds_valid || b.sky || b.object < 0 ||
        size_t(b.object) >= doc.objects().size() ||
        (!options.show_enemies && b.enemy_preview) ||
        uint64_t(b.first) + b.count > a.vertices.size())
      continue;
    const size_t visual = size_t(b.object);
    auto [owner, fresh] = owners.try_emplace(visual, -1);
    if (fresh)
      owner->second = doc.logic_owner_index(visual);
    const size_t logical = owner->second >= 0 ? size_t(owner->second) : visual;
    if (options.accept_object) {
      auto [ok, new_object] = accepted.try_emplace(logical, false);
      if (new_object)
        ok->second = options.accept_object(logical);
      if (!ok->second)
        continue;
    }
    const auto matrix =
        a.model_matrix(b, doc, options.preview_object, options.preview_position,
                       options.transform_preview);
    const auto inv = inverse(matrix);
    if (!inv)
      continue;
    Ray local{position(*inv, ray.origin), direction(*inv, ray.direction)};
    if (!finite(local.origin) || !finite(local.direction))
      continue;
    ++work.bounds_tested;
    if (const auto entry = box_hit(b.local_bounds, local, options.maximum)) {
      ++work.bounds_hit;
      candidates.push_back({i, logical, local, matrix, *entry});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto &x, const auto &y) { return x.entry < y.entry; });
  float nearest = options.maximum;
  std::optional<ModelHit> result;
  for (const auto &candidate : candidates) {
    if (candidate.entry > nearest)
      break;
    const auto &b = a.batches[candidate.batch];
    for (size_t index = b.first; index + 2 < size_t(b.first) + b.count;
         index += 3) {
      ++work.triangles_tested;
      const auto &v0 = a.vertices[index], &v1 = a.vertices[index + 1],
                 &v2 = a.vertices[index + 2];
      const V3 edge1 = v1.position - v0.position,
               edge2 = v2.position - v0.position,
               p = cross(candidate.local.direction, edge2);
      const double det = dot(edge1, p);
      if (std::abs(det) < 1e-12)
        continue;
      const V3 offset = candidate.local.origin - v0.position;
      const double u = dot(offset, p) / det;
      if (u < -.00001 || u > 1.00001)
        continue;
      const auto q = cross(offset, edge1);
      const double v = dot(candidate.local.direction, q) / det;
      if (v < -.00001 || u + v > 1.00001)
        continue;
      const double distance = dot(edge2, q) / det;
      if (distance < .0001 || distance > nearest)
        continue;
      const auto world = ray.origin + ray.direction * float(distance);
      if ((!b.enemy_preview && world.y > options.cut_height) || !finite(world) ||
          (options.alpha_test && !opaque(a, b, v0, v1, v2, float(u), float(v))))
        continue;
      if (result && std::abs(distance - nearest) < 1e-6 &&
          candidate.owner >= result->object)
        continue;
      nearest = float(distance);
      const auto normal = normalized(cross(direction(candidate.matrix, edge1),
                                           direction(candidate.matrix, edge2)));
      result = ModelHit{candidate.owner, size_t(b.object),
                        candidate.batch, uint32_t((index - b.first) / 3),
                        nearest,         world,
                        normal};
    }
  }
  return finish(result);
}
} // namespace mapedit
