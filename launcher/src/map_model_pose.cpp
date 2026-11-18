#include "map_model_pose.h"
#include "map_document.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
namespace mapedit {
namespace {
struct Read {
  const std::vector<uint8_t> &b;
  void range(uint64_t p, uint64_t n) const {
    if (p > b.size() || n > b.size() - p)
      throw std::runtime_error("Model pose outside original asset");
  }
  uint32_t u(uint64_t p) const {
    range(p, 4);
    return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
           uint32_t(b[p + 2]) << 8 | b[p + 3];
  }
  int16_t h(uint64_t p) const {
    range(p, 2);
    return std::bit_cast<int16_t>(uint16_t(uint16_t(b[p]) << 8 | b[p + 1]));
  }
  float f(uint64_t p) const {
    float x = std::bit_cast<float>(u(p));
    if (!std::isfinite(x) || std::abs(x) > 1e8)
      throw std::runtime_error("Invalid model pose number");
    return x;
  }
  std::vector<uint32_t> chain(uint32_t field, uint32_t minimum,
                              size_t limit) const {
    std::vector<uint32_t> out;
    std::set<uint32_t> seen;
    for (uint32_t p = u(field); p; p = u(p)) {
      range(p, minimum);
      if (!seen.insert(p).second || out.size() >= limit)
        throw std::runtime_error("Invalid model pose chain");
      out.push_back(p);
    }
    return out;
  }
  V3 v(uint64_t p) const { return {f(p), f(p + 4), f(p + 8)}; }
  std::vector<V3> array(uint32_t field) const {
    auto p = u(field);
    if (!p)
      return {};
    range(p, 16);
    auto data = u(p), n = u(p + 8);
    if (n > 1000000 || u(p + 12) != 12 || uint64_t(n) * 12 != u(p + 4))
      throw std::runtime_error("Invalid model pose vector array");
    range(data, uint64_t(n) * 12);
    std::vector<V3> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
      out.push_back(v(uint64_t(data) + i * 12));
    return out;
  }
  std::string name(uint32_t p) const {
    if (!p)
      return {};
    range(p, 1);
    size_t end = p;
    while (end < b.size() && b[end] && end - p < 1024)
      ++end;
    if (end == b.size() || end - p >= 1024)
      throw std::runtime_error("Invalid pose name");
    return std::string(b.begin() + p, b.begin() + end);
  }
};
V3 point(const Matrix &m, V3 v, bool normal = false) {
  V3 o;
  for (unsigned i = 0; i < 3; ++i)
    o[i] = m[i * 4] * v.x + m[i * 4 + 1] * v.y + m[i * 4 + 2] * v.z +
           (normal ? 0 : m[i * 4 + 3]);
  return o;
}
} // namespace
ModelPose preview_model_pose(const std::vector<uint8_t> &image,
                             uint32_t model) {
  Read r{image};
  r.range(model, 80);
  if (r.u(model) != 1 || r.u(model + 4) != 100)
    throw std::runtime_error("Unsupported preview model header");
  ModelPose out;
  out.positions = r.array(model + 28);
  out.normals = r.array(model + 32);
  auto bones = r.chain(model + 48, 52, 64);
  out.bones = bones.size();
  if (bones.empty())
    return out;
  auto clips = r.chain(model + 52, 24, 4096);
  uint32_t selected = 0;
  int best = -1;
  for (auto clip : clips) {
    if (!r.u(clip + 12))
      continue;
    auto name = r.name(r.u(clip + 4)), lower = name;
    for (auto &c : lower)
      c = char(std::tolower(static_cast<unsigned char>(c)));
    int rank = lower.starts_with("idle")    ? 3
               : lower.starts_with("stand") ? 2
                                            : 0;
    if (rank > best) {
      best = rank;
      selected = clip;
      out.name = name;
    }
  }
  if (!selected)
    throw std::runtime_error("Animated model has no authored preview frame");
  auto frames = r.u(selected + 16);
  r.range(frames, 20);
  auto frame = r.u(frames);
  r.range(frame, bones.size() * 2);
  auto packed = r.u(model + 68);
  std::vector<Matrix> matrices(bones.size());
  for (size_t i = 0; i < bones.size(); ++i) {
    auto &m = matrices[i];
    if (packed) {
      r.range(packed, 20);
      uint16_t index = std::bit_cast<uint16_t>(r.h(uint64_t(frame) + i * 2));
      if (index >= r.u(packed + 4))
        throw std::runtime_error("Model preview key outside pool");
      uint64_t key = uint64_t(r.u(packed)) + uint64_t(index) * 20;
      r.range(key, 20);
      float w[10];
      for (unsigned k = 0; k < 10; ++k)
        w[k] = float(r.h(key + k * 2));
      m[0] = w[0] / 4096.f;
      m[4] = w[1] / 4096.f;
      m[8] = w[2] / 4096.f;
      m[1] = w[3] / 4096.f;
      m[5] = w[4] / 4096.f;
      m[9] = w[5] / 4096.f;
      float scale = w[6] / 2048.f;
      m[2] = (m[4] * m[9] - m[8] * m[5]) * scale;
      m[6] = (m[8] * m[1] - m[0] * m[9]) * scale;
      m[10] = (m[0] * m[5] - m[4] * m[1]) * scale;
      m[3] = w[7] * r.f(packed + 8);
      m[7] = w[8] * r.f(packed + 12);
      m[11] = w[9] * r.f(packed + 16);
    } else {
      r.range(frame, bones.size() * 4);
      auto p = r.u(uint64_t(frame) + i * 4);
      r.range(p, 48);
      for (unsigned k = 0; k < 12; ++k)
        m[k] = r.f(uint64_t(p) + k * 4);
    }
  }
  std::vector<V3> skinned(out.positions.size());
  std::vector<bool> influenced(out.positions.size());
  for (size_t i = 0; i < bones.size(); ++i) {
    auto b = bones[i];
    auto np = r.u(b + 12), points = r.u(b + 16), indices = r.u(b + 20),
         weights = r.u(b + 24);
    if (np > 1000000)
      throw std::runtime_error("Model bone influence limit");
    r.range(points, uint64_t(np) * 12);
    r.range(indices, uint64_t(np) * 4);
    r.range(weights, uint64_t(np) * 4);
    for (uint32_t j = 0; j < np; ++j) {
      auto index = r.u(uint64_t(indices) + j * 4);
      float weight = r.f(uint64_t(weights) + j * 4);
      if (index >= out.positions.size() || weight < 0 || weight > 1.01f)
        throw std::runtime_error("Invalid model influence");
      skinned[index] =
          skinned[index] +
          point(matrices[i], r.v(uint64_t(points) + j * 12)) * weight;
      influenced[index] = true;
    }
    auto nn = r.u(b + 40), normals = r.u(b + 44), ni = r.u(b + 48);
    if (nn > 1000000)
      throw std::runtime_error("Model normal influence limit");
    r.range(normals, uint64_t(nn) * 12);
    r.range(ni, uint64_t(nn) * 4);
    for (uint32_t j = 0; j < nn; ++j) {
      auto index = r.u(uint64_t(ni) + j * 4);
      if (index >= out.normals.size())
        throw std::runtime_error("Invalid model normal index");
      out.normals[index] =
          normalized(point(matrices[i], r.v(uint64_t(normals) + j * 12), true));
    }
  }
  for (size_t i = 0; i < out.positions.size(); ++i)
    if (influenced[i])
      out.positions[i] = skinned[i];
  return out;
}
} // namespace mapedit
