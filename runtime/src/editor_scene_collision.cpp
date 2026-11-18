#include "ssne/editor_scene_collision.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <set>
#include <vector>

namespace ssne {
namespace {
constexpr uint32_t builder = 0x80137488, builder_return = 0x80137E50;
constexpr uint32_t scene_table = 0x8021E660, max_models = 32768;
PPCFunc original_builder = nullptr;
constexpr std::array<std::array<uint32_t, 2>, 8> pins{{
    {builder, 0x9421FF90}, {0x80137B28, 0x9421FF80},
    {0x80137E4C, 0x4BFFF63D}, {0x8013462C, 0x9421FFF0},
    {0x80137A74, 0x48082405}, {0x80137A80, 0x48082FBD},
    {0x8015B29C, 0x9421FFA8}, {0x8015BC08, 0x9421FFC0}}};

bool readable(uint32_t p, uint64_t n) {
  return g_ram && !(p & 3u) && p >= 0x80000000u &&
         n <= guest_ram_size() && uint64_t(p) + n <= guest_ram_end();
}
bool revision() {
  if (!readable(0x80000000, 4) ||
      mem_read32(0x80000000) != 0x47334250) return false;
  for (const auto& p : pins)
    if (!readable(p[0], 4) || mem_read32(p[0]) != p[1]) return false;
  return true;
}
bool scalar(uint32_t p, double& result) {
  if (!readable(p, 4)) return false;
  result = u2f(mem_read32(p));
  return std::isfinite(result) && std::abs(result) <= 1000000.;
}
bool hierarchy(uint32_t descriptor) {
  // Collision hierarchy spheres are separate from the scene's 20B world
  // bounds. The native query indexes 24B local spheres through this root.
  if (!readable(descriptor, 0x30)) return false;
  const auto root = mem_read32(descriptor + 0x18);
  const auto nodes = mem_read32(descriptor + 0x28);
  if (root > 262143 || !readable(nodes, uint64_t(root + 1) * 24))
    return false;
  const auto at = nodes + root * 24;
  double value = 0;
  if (!scalar(at + 4, value) || value < 0) return false;
  for (unsigned i = 0; i < 3; ++i)
    if (!scalar(at + 8 + i * 4, value)) return false;
  return readable(mem_read32(descriptor + 0x2C), 12);
}
bool scale(uint32_t matrix, uint32_t native_bits, uint32_t& corrected) {
  double m[3][3]{}, gram[3][3]{}, lengths[3]{};
  for (unsigned row = 0; row < 3; ++row)
    for (unsigned col = 0; col < 4; ++col) {
      double value = 0;
      if (!scalar(matrix + row * 16 + col * 4, value)) return false;
      if (col < 3) m[row][col] = value;
    }
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j < 3; ++j)
      for (unsigned row = 0; row < 3; ++row)
        gram[i][j] += m[row][i] * m[row][j];
    lengths[i] = std::sqrt(gram[i][i]);
    if (!std::isfinite(lengths[i]) || lengths[i] < .0001 ||
        lengths[i] > 10000.) return false;
  }
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = i + 1; j < 3; ++j)
      if (std::abs(gram[i][j]) > .001 * lengths[i] * lengths[j])
        return false; // This adapter does not enable sheared transforms.
  const double native_scale = u2f(native_bits);
  if (!std::isfinite(native_scale) || native_scale <= 0 ||
      std::abs(native_scale - lengths[1]) >
          .00001 * std::max(native_scale, lengths[1])) return false;
  corrected = native_bits;
  const auto [lo, hi] = std::minmax_element(lengths, lengths + 3);
  // Leave every native uniform TRS bit-identical, including float rotation
  // noise. Only the newly exposed nonuniform case needs a larger bound.
  if (*hi - *lo <= .00001 * *hi) return true;
  double bound_squared = 0;
  for (unsigned i = 0; i < 3; ++i) {
    double row_bound = gram[i][i];
    for (unsigned j = 0; j < 3; ++j)
      if (i != j) row_bound += std::abs(gram[i][j]);
    bound_squared = std::max(bound_squared, row_bound);
  }
  // Gershgorin's bound is max basis length for orthogonal TRS, and also
  // encloses tiny float orthogonality residuals. Never round down to float.
  const double bound = std::sqrt(bound_squared);
  float rounded = float(bound);
  if (double(rounded) < bound)
    rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
  if (!std::isfinite(rounded) || rounded <= 0) return false;
  if (double(rounded) > native_scale) corrected = f2u(rounded);
  return true;
}
struct Change { uint32_t at, bits; };
bool changes(uint32_t owner, std::vector<Change>& result) {
  if (!readable(owner, 0xB8C) || mem_read32(owner) != scene_table)
    return false;
  const auto resource = mem_read32(owner + 4);
  const auto entries = mem_read32(owner + 0x18);
  const auto count = mem_read32(owner + 0x1C);
  if (!readable(resource, 0x80) || mem_read32(resource) != 1 ||
      mem_read32(resource + 4) != 100 || count > max_models ||
      (count && !readable(entries, uint64_t(count) * 20))) return false;
  auto node = mem_read32(resource + 0x58);
  std::set<uint32_t> seen_nodes, seen_colliders;
  for (uint32_t index = 0; index < count; ++index) {
    if (!readable(node, 48) || !seen_nodes.insert(node).second)
      return false;
    const auto entry = entries + index * 20;
    if (mem_read32(entry + 0x10) != node) return false;
    const auto collider = mem_read32(entry);
    if (collider) {
      if (!readable(collider, 0xB8) ||
          !seen_colliders.insert(collider).second ||
          mem_read16(collider + 8) != 4 ||
          mem_read16(collider + 0xA) != 0 ||
          mem_read32(collider + 0xB4) != (0x04000000u | index) ||
          mem_read32(collider + 0x2C) != 0 ||
          mem_read32(collider + 0x20) != 0 ||
          mem_read32(collider + 0x24) != 0 ||
          mem_read32(collider + 0x28) != 1 ||
          (mem_read32(node + 0x24) & 2)) return false;
      const auto matrix = mem_read32(node + 4);
      if (!readable(matrix, 48) ||
          std::memcmp(host_ptr(matrix), host_ptr(collider + 0x34), 48))
        return false;
      auto source = mem_read32(node + 0x10);
      const auto frame = mem_read32(node + 0x2C);
      if (frame) {
        if (!readable(frame, 20)) return false;
        source = mem_read32(frame + 8);
      }
      if (!readable(source, 0x2C)) return false;
      const auto descriptor = mem_read32(source + 0x28);
      if (mem_read32(collider + 0xC) != descriptor || !hierarchy(descriptor))
        return false;
      uint32_t corrected = 0;
      const auto old = mem_read32(collider + 0x94);
      if (!scale(matrix, old, corrected)) return false;
      if (old != corrected) result.push_back({collider + 0x94, corrected});
    }
    node = mem_read32(node);
  }
  return node == 0;
}
void builder_hook(PPCContext& c) {
  const auto owner = c.r[3], caller = c.lr;
  ssne_call_original(c, builder, original_builder);
  if (caller != builder_return || c.r[3] != 0 || c.r[2] != 0x802F22A0 ||
      c.r[13] != 0x802E22A0 || !revision()) return;
  std::vector<Change> pending;
  // Preflight the complete bounded list before the first write. Invalid
  // geometry leaves the entire native scene untouched.
  if (!changes(owner, pending)) return;
  for (const auto& change : pending) mem_write32(change.at, change.bits);
  static const bool trace = [] {
    const auto hidden = std::getenv("SSNE_TEST_HIDDEN");
    const auto requested = std::getenv("SSNE_EDITOR_COLLISION_TRACE");
    return hidden && std::strcmp(hidden, "0") && requested &&
           std::strcmp(requested, "0");
  }();
  if (trace)
    log_message("[editor-scene-collision] built=" +
                std::to_string(mem_read32(owner + 0x1C)) +
                " corrected=" + std::to_string(pending.size()));
}
} // namespace

bool install_editor_scene_collision(std::string& error) {
  if (original_builder) { error.clear(); return true; }
  if (!revision()) {
    error = "Native SceneModel collision revision unavailable";
    return false;
  }
  const auto entry = ssne_find_func(builder);
  if (!entry || !entry->func) {
    error = "Native SceneModel collision builder unavailable";
    return false;
  }
  original_builder = *ssne_hook_slot(builder);
  if (!original_builder) original_builder = entry->func;
  ssne_install_hook(builder, builder_hook);
  error.clear();
  return true;
}
} // namespace ssne
