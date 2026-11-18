#include "map_appearance.h"
#include "map_model_pose.h"
#include "map_texture_decode.h"
#include "owned_gc_maps.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace mapedit;
size_t checks = 0;
void check(bool v, const char *what) {
  ++checks;
  if (!v)
    throw std::runtime_error(what);
}
bool close(const Matrix &a, const Matrix &b) {
  for (unsigned i = 0; i < 12; ++i)
    if (std::abs(a[i] - b[i]) > .003f)
      return false;
  return true;
}
void pose_fixture() {
  std::vector<uint8_t> b(1024);
  auto u = [&](size_t p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
      b[p + i] = uint8_t(v >> (24 - i * 8));
  };
  auto h = [&](size_t p, uint16_t v) {
    b[p] = uint8_t(v >> 8);
    b[p + 1] = uint8_t(v);
  };
  auto f = [&](size_t p, float v) { u(p, std::bit_cast<uint32_t>(v)); };
  u(0, 1);
  u(4, 100);
  u(28, 80);
  u(32, 96);
  u(48, 256);
  u(52, 400);
  u(68, 448);
  u(80, 128);
  u(84, 12);
  u(88, 1);
  u(92, 12);
  u(96, 144);
  u(100, 12);
  u(104, 1);
  u(108, 12);
  f(148, 1);
  // Two independent bones blend one vertex equally: translations 2 and 4 -> 3.
  u(256, 320);
  u(268, 1);
  u(272, 160);
  u(276, 184);
  u(280, 192);
  u(296, 1);
  u(300, 208);
  u(304, 232);
  u(332, 1);
  u(336, 172);
  u(340, 188);
  u(344, 196);
  u(360, 1);
  u(364, 220);
  u(368, 236);
  f(164, 1);
  f(176, 1);
  f(192, .5f);
  f(196, .5f);
  f(212, 1);
  f(224, 1);
  u(404, 600);
  b[600] = 'i';
  b[601] = 'd';
  b[602] = 'l';
  b[603] = 'e';
  u(412, 1);
  u(416, 432);
  u(432, 624);
  h(624, 0);
  h(626, 1);
  u(448, 480);
  u(452, 2);
  f(456, .5f);
  f(460, .5f);
  f(464, .5f);
  for (size_t key : {size_t(480), size_t(500)}) {
    h(key, 4096);
    h(key + 8, 4096);
    h(key + 12, 2048);
  }
  h(494, 4);
  h(514, 8);
  auto pose = preview_model_pose(b, 0);
  check(pose.bones == 2 && pose.name == "idle",
        "Authored preview pose selection");
  check(length(pose.positions.at(0) - V3{3, 1, 0}) < .0001f,
        "Weighted bone pose blend");
  check(length(pose.normals.at(0) - V3{0, 1, 0}) < .0001f, "Skinned normal");
  auto original = b;
  h(626, 12);
  bool rejected = false;
  try {
    preview_model_pose(b, 0);
  } catch (...) {
    rejected = true;
  }
  check(rejected, "Out-of-range preview frame key accepted");
  b = original;
  u(320, 256);
  rejected = false;
  try {
    preview_model_pose(b, 0);
  } catch (...) {
    rejected = true;
  }
  check(rejected, "Cyclic preview skeleton accepted");
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "Owned data argument required");
    pose_fixture();
    size_t enemies = 0, extra_objects = 0, missing_original = 0, arrows = 0;
    size_t terrain_mask_vertices = 0, alpha_test_batches = 0, blend_batches = 0;
    size_t maps = 0, vertices = 0, instances = 0, pickups = 0, textures = 0,
           cutouts = 0, translucent = 0;
    size_t original_maps=0, additional_maps=0, original_enemies=0,
           original_arrows=0, original_extra_objects=0, original_instances=0,
           original_pickups=0, original_textures=0, original_cutouts=0,
           original_translucent=0, original_alpha_test_batches=0,
           original_blend_batches=0;
    for (auto &e : std::filesystem::directory_iterator(
             std::filesystem::path(argv[1]) / "files/Levels"))
      if (e.path().extension() == ".ssw") {
        const bool original = original_gc_map(e.path().filename().string());
        Document d;
        d.load(e.path());
        auto before = d.payload();
        auto a = Appearance::load(d, argv[1]);
        enemies += a.enemies;
        arrows += a.arrows;
        extra_objects += a.extra_objects;
        if(original){original_enemies+=a.enemies;original_arrows+=a.arrows;
          original_extra_objects+=a.extra_objects;original_instances+=a.instances;
          original_pickups+=a.pickups;original_textures+=a.textures.size();}
        for (auto &p : a.previews) {
          if (!p.error.empty()) {
            check(e.path().stem() == "Clevel8_4" && p.model == "dragon",
                  "Unresolved available original model");
            ++missing_original;
            continue;
          }
          if (p.role == "Enemy spawn" || p.role == "Enemy template") {
            check(p.bones > 0 && !p.pose.empty(),
                  "Enemy did not get an authored pose");
            auto batch =
                std::find_if(a.batches.begin(), a.batches.end(), [&](auto &b) {
                  return b.object == p.object && b.enemy_preview;
                });
            check(batch != a.batches.end(),
                  "Enemy model missing from renderer");
            V3 low{1e9f, 1e9f, 1e9f}, high{-1e9f, -1e9f, -1e9f};
            for (size_t k = batch->first;
                 k < size_t(batch->first) + batch->count; ++k)
              for (unsigned axis = 0; axis < 3; ++axis) {
                auto v = a.vertices[k].position[axis];
                low[axis] = std::min(low[axis], v);
                high[axis] = std::max(high[axis], v);
              }
            check(length(high - low) > .1f, "Enemy pose collapsed to a point");
          }
        }
        check(!a.vertices.empty() && !a.textures.empty(),
              "Missing visible level");
        check(a.warnings.empty(), "Model/material warning");
        check(d.payload() == before, "Read-only asset load modified map");
        for (auto &t : a.textures) {
          check(t.rgba.size() == size_t(t.width) * t.height,
                "Texture dimensions");
          bool transparent = false;
          for (auto c : t.rgba)
            if ((c >> 24) == 0)
              transparent = true;
          cutouts += transparent;
          translucent += t.translucent;
          if(original){original_cutouts+=transparent;original_translucent+=t.translucent;}
        }
        for (auto &v : a.vertices)
          check(std::isfinite(v.position.x) && std::isfinite(v.u) &&
                    std::isfinite(v.u1) && std::isfinite(v.u2),
                "Non-finite render data");
        int movable = -1;
        const VisualBatch *sample = nullptr;
        for (auto &b : a.batches) {
          check(b.base_mode <= 2 && b.blend_option <= 4,
                "Unsupported native material rule");
          alpha_test_batches += b.blend_option == 1;
          blend_batches += b.blend_option >= 2;
          if(original){original_alpha_test_batches+=b.blend_option==1;
            original_blend_batches+=b.blend_option>=2;}
          if (e.path().stem() == "Clevel5_4" && b.object < 0 && !b.sky &&
              b.operations[0] == 4) {
            check(b.base_mode == 1 && b.blend_option == 0,
                  "Terrain blend mask was treated as surface transparency");
            for (size_t k = b.first; k < size_t(b.first) + b.count; ++k)
              if (a.vertices[k].color[3] < .5f)
                ++terrain_mask_vertices;
          }
          check(b.count % 3 == 0 &&
                    uint64_t(b.first) + b.count <= a.vertices.size(),
                "Invalid render draw range");
          for (auto t : {b.texture, b.layers[0], b.layers[1]})
            check(t == -1 || (t >= 0 && size_t(t) < a.textures.size()),
                  "Invalid material texture");
          auto m = a.model_matrix(b, d);
          check(close(m, b.matrix), "Original model transform changed");
          if (b.placement != PreviewPlacement::Scene) {
            auto authored = d.matrix(size_t(b.object));
            Matrix expected{1, 0,           0, authored[3], 0, 1,
                            0, authored[7], 0, 0,           1, authored[11]};
            if (b.placement == PreviewPlacement::Arrow) {
              expected = authored;
              check(d.objects().at(size_t(b.object)).type == 4260,
                    "Arrow geometry bound to wrong native actor type");
            } else if (b.placement == PreviewPlacement::EnemySpawner) {
              // Independent original path: negative forward -> acos yaw,
              // signed by X -> negate in entity factory -> negate at spawn.
              const float x = -authored[2], z = -authored[10],
                          horizontal = std::hypot(x, z);
              if (horizontal > 1e-7f) {
                float yaw = std::acos(std::clamp(z / horizontal, -1.f, 1.f));
                if (x < 0)
                  yaw = -yaw;
                expected[0] = expected[10] = std::cos(yaw);
                expected[2] = std::sin(yaw);
                expected[8] = -std::sin(yaw);
              }
            }
            check(close(m, expected),
                  "Preview differs from native actor factory facing");
            // Actor factories leave the native geometry and all authored map
            // fields intact. Scene props must never receive this correction.
            check(d.payload() == before,
                  "Native facing preview changed authored data");
          }

          if (b.object >= 0) {
            check(size_t(b.object) < d.objects().size(),
                  "Invalid object binding");
            movable = b.object;
            sample = &b;
          }
        }
        if (sample) {
          auto orig = d.matrix(movable);
          auto old = a.model_matrix(*sample, d);
          auto target = V3{orig[3] + 3.25f, orig[7], orig[11]};
          auto preview = a.model_matrix(*sample, d, movable, target);
          check(std::abs(preview[3] - old[3] - 3.25f) < .003f,
                "Model drag preview offset");
          check(d.payload() == before, "Preview modified document");
          d.move(movable, {target.x, target.y, target.z});
          auto moved = a.model_matrix(*sample, d);
          check(close(moved, preview), "Model and marker edit diverged");
          check(d.undo() && d.payload() == before &&
                    close(a.model_matrix(*sample, d), old),
                "Model transform undo");
        }
        ++maps;
        if(original)++original_maps;else ++additional_maps;
        vertices += a.vertices.size();
        instances += a.instances;
        pickups += a.pickups;
        textures += a.textures.size();
        std::cout << e.path().stem().string() << ": " << a.vertices.size() / 3
                  << " triangles, " << a.textures.size() << " textures, "
                  << a.instances << " instances, " << a.pickups << " pickups\n";
      }
    check(original_maps == 49 && maps == original_maps + additional_maps &&
              original_instances > 1000 && original_pickups > 1000 && original_textures > 1000,
          "Owned map coverage");
    check(original_cutouts > 0 && original_translucent > 0, "No original alpha texture coverage");
    struct Source {
      std::vector<uint8_t> b_;
      uint8_t b(uint32_t p) const { return b_.at(p); }
      uint16_t h(uint32_t p) const { return uint16_t(b(p)) << 8 | b(p + 1); }
    };
    std::vector<uint32_t> rgba;
    auto palette = [](uint32_t, uint32_t) { return 0u; };
    Source cmpr{std::vector<uint8_t>(32, 255)};
    for (unsigned i = 0; i < 4; ++i)
      cmpr.b_[i * 8] = cmpr.b_[i * 8 + 1] = 0;
    ssne::graphics::detail::decode(cmpr, 14, 8, 8, 0, rgba, palette);
    check(std::all_of(rgba.begin(), rgba.end(), [](auto p) { return p == 0; }),
          "CMPR transparent pixels became black/opaque");
    Source planar{std::vector<uint8_t>(64)};
    for (unsigned i = 0; i < 16; ++i) {
      planar.b_[i * 2] = 77;
      planar.b_[i * 2 + 1] = 91;
      planar.b_[32 + i * 2] = 88;
      planar.b_[33 + i * 2] = 66;
    }
    ssne::graphics::detail::decode(planar, 6, 4, 4, 0, rgba, palette);
    check(rgba[0] == (91u | (88u << 8) | (66u << 16) | (77u << 24)),
          "RGBA8 planar decode");
    Camera c;
    c.focus({2, -1, 80}, 40);
    auto eye = c.eye();
    c.look(50, -20);
    check(length(c.eye() - eye) < .0001f, "Mouse look moved camera position");
    Camera a60 = c, a240 = c;
    for (int i = 0; i < 60; ++i)
      a60.fly(0, 0, 1, 1.f / 60, 20);
    for (int i = 0; i < 240; ++i)
      a240.fly(0, 0, 1, 1.f / 240, 20);
    check(length(a60.eye() - a240.eye()) < .002f,
          "Flight speed depends on FPS");
    check(std::abs(length(a60.eye() - eye) - 20) < .002f,
          "Wrong metres-per-second flight speed");
    Camera diag = c;
    for (int i = 0; i < 60; ++i)
      diag.fly(1, 0, 1, 1.f / 60, 20);
    check(std::abs(length(diag.eye() - eye) - 20) < .002f,
          "Diagonal flight faster than straight");
    check(original_arrows == 156, "Not all native ArrowMarker actors have original model previews");
    check(original_enemies == 8529 && original_extra_objects == 480 && missing_original == 2,
          "Enemy/object preview coverage");
    check(terrain_mask_vertices > 0 && original_alpha_test_batches > 0 &&
              original_blend_batches > 0,
          "Missing material/alpha regression coverage");
    std::cout << "MATERIAL terrain_mask_vertices=" << terrain_mask_vertices
              << " alpha_test_batches=" << alpha_test_batches
              << " blend_batches=" << blend_batches << "\n";
    std::cout << "PASS enemies=" << enemies
              << " arrows=" << arrows
              << " extra_objects=" << extra_objects
              << " missing_original=" << missing_original << " maps=" << maps
              << " original_maps=" << original_maps << " additional_maps=" << additional_maps
              << " original_enemies=" << original_enemies
              << " original_arrows=" << original_arrows
              << " original_extra_objects=" << original_extra_objects
              << " triangles=" << vertices / 3 << " instances=" << instances
              << " pickups=" << pickups << " textures=" << textures
              << " alpha_cutouts=" << cutouts << " translucent=" << translucent
              << " checks=" << checks << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
