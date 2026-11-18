#include "map_model_pick.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace mapedit;
namespace {
unsigned checks = 0, real_models = 0;
void check(bool value, const char *why) {
  ++checks;
  if (!value)
    throw std::runtime_error(why);
}
uint32_t word(const Document &d, uint32_t p) {
  const auto &b = d.payload();
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
V3 vector(const Matrix &m, V3 p) {
  return {m[0] * p.x + m[1] * p.y + m[2] * p.z,
          m[4] * p.x + m[5] * p.y + m[6] * p.z,
          m[8] * p.x + m[9] * p.y + m[10] * p.z};
}
V3 point(const Matrix &m, V3 p) { return vector(m, p) + V3{m[3], m[7], m[11]}; }
constexpr Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
VisualBatch plane(size_t object, float z) {
  VisualBatch b;
  b.object = int(object);
  b.count = 3;
  b.matrix = identity;
  b.matrix[11] = z;
  b.reference = identity;
  return b;
}
Appearance planes(size_t object) {
  Appearance a;
  for (auto p : {V3{0, 0, 0}, V3{1, 0, 0}, V3{0, 1, 0}}) {
    VisualVertex v;
    v.position = p;
    v.u = v.v = .5f;
    a.vertices.push_back(v);
  }
  a.batches.push_back(plane(object, 0));
  a.prepare_pick_bounds();
  return a;
}
void real_model(const Appearance &a, const Document &doc, size_t object) {
  ModelPickOptions options;
  options.alpha_test = false;
  options.accept_object = [&](size_t i) { return i == object; };
  options.maximum = .101f;
  bool hit_model = false;
  for (size_t batch = 0; batch < a.batches.size() && !hit_model; ++batch) {
    const auto &b = a.batches[batch];
    if (b.object < 0 || size_t(b.object) >= doc.objects().size())
      continue;
    const auto owner = doc.logic_owner_index(size_t(b.object));
    if ((owner >= 0 ? size_t(owner) : size_t(b.object)) != object)
      continue;
    for (size_t k = b.first; k + 2 < size_t(b.first) + b.count; k += 3) {
      const auto &v0 = a.vertices[k].position, &v1 = a.vertices[k + 1].position,
                 &v2 = a.vertices[k + 2].position;
      const auto matrix = a.model_matrix(b, doc);
      const auto normal =
          normalized(cross(vector(matrix, v1 - v0), vector(matrix, v2 - v0)));
      if (length(normal) < .9f)
        continue;
      const auto center = point(matrix, (v0 + v1 + v2) / 3);
      const auto hit =
          pick_model(a, doc, {center + normal * .05f, normal * -4}, options);
      if (!hit)
        continue;
      check(hit->object == object && hit->visual_object == size_t(b.object),
            "Real model selected wrong visual/logical owner");
      check(hit->distance <= .0501f && hit->distance > 0,
            "Real model ray distance is not world distance");
      auto occluded = options;
      occluded.maximum = .00001f;
      check(
          !pick_model(a, doc, {center + normal * .05f, normal * -4}, occluded),
          "Occluded model ignored terrain depth limit");
      auto hidden = options;
      hidden.cut_height = center.y - 1;
      const auto invisible =
          pick_model(a, doc, {center + normal * .05f, normal * -4}, hidden);
      if (b.enemy_preview)
        check(invisible && std::abs(invisible->distance - hit->distance) < .001f,
              "Visible enemy preview was incorrectly removed by cutaway");
      else
        check(!invisible || invisible->point.y <= hidden.cut_height,
              "Cutaway model remained selectable above cut plane");
      ++real_models;
      hit_model = true;
      break;
    }
  }
  check(hit_model, "Could not select a real model surface");
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 2, "Use owned game_data root");
    const auto data = std::filesystem::u8path(argv[1]);
    Document original;
    original.load(data / "files/Levels/Rlevel1_1.ssw");
    size_t prop = original.objects().size(), enemy = prop, arrow = prop,
           brush = prop;
    for (size_t i = 0; i < original.objects().size(); ++i) {
      const auto &o = original.objects()[i];
      if (o.type == 4200 && prop == original.objects().size())
        prop = i;
      if (o.type == 5000 && enemy == original.objects().size())
        enemy = i;
      if (o.type == 4260 && arrow == original.objects().size())
        arrow = i;
      if (o.type == 4070) {
        const auto switch_id =
            word(original, word(original, o.offset + 16) + 36);
        for (const auto &target : original.objects())
          if (target.type == 4180 &&
              word(original, target.offset + 20) == switch_id)
            brush = i;
      }
    }
    check(prop < original.objects().size() &&
              enemy < original.objects().size() &&
              brush < original.objects().size(),
          "Native prop/enemy/button templates unavailable");
    auto actual = Appearance::load(original, data);
    real_model(actual, original, prop);
    real_model(actual, original, enemy);
    if (arrow < original.objects().size())
      real_model(actual, original, arrow);
    else {
      Document other;
      other.load(data / "files/Levels/Alevel10_2.ssw");
      const auto a = Appearance::load(other, data);
      bool found = false;
      for (size_t i = 0; i < other.objects().size(); ++i)
        if (other.objects()[i].type == 4260) {
          real_model(a, other, i);
          found = true;
          break;
        }
      check(found, "No real Arrow4260 tested");
    }
    auto button_doc = original;
    LogicSpec button;
    button.kind = LogicKind::Button;
    button.name = "Mesh pick fixture";
    const auto logical =
        button_doc.add_button(original, brush, {2.5f, .75f, 82.25f}, button);
    auto buttons = Appearance::load(button_doc, data);
    real_model(buttons, button_doc, logical);
    bool paired = false;
    for (size_t i = 0; i < button_doc.objects().size(); ++i)
      if (button_doc.logic_owner_index(i) == int(logical)) {
        paired = true;
        check(button_doc.objects()[i].type == 4200,
              "Button owner remapped unrelated class");
      }
    check(paired, "Paired visible button mesh not identified");
    auto doc = original;
    doc.set_matrix(prop, identity);
    auto a = planes(prop);
    ModelPickStats work;
    const Ray ray{{.25f, .25f, 2}, {0, 0, -7}};
    auto hit = pick_model(a, doc, ray, {}, &work);
    check(hit && hit->object == prop && std::abs(hit->distance - 2) < .0001f,
          "Plane surface hit failed");
    check(work.bounds_tested == 1 && work.triangles_tested == 1,
          "Cached bounds not used");
    ModelPickOptions options;
    options.maximum = 1.99f;
    check(!pick_model(a, doc, ray, options), "Terrain occlusion limit ignored");
    options.maximum = 2;
    check(pick_model(a, doc, ray, options).has_value(),
          "Exact depth contact rejected");
    options = {};
    options.cut_height = .2f;
    check(!pick_model(a, doc, ray, options), "Cutaway triangle selected");
    options = {};
    options.accept_object = [](size_t) { return false; };
    check(!pick_model(a, doc, ray, options), "Search/category filter ignored");
    check(!pick_model(a, doc, {{3, 3, 2}, {0, 0, -1}}, {}, &work) &&
              work.triangles_tested == 0,
          "Ray missed bounds but walked triangles");
    check(!pick_model(a, doc, {{.25f, .25f, 2}, {0, 0, 0}}),
          "Zero ray accepted");
    check(!pick_model(a, doc,
                      {{.25f, .25f, 2},
                       {0, 0, std::numeric_limits<float>::quiet_NaN()}}),
          "NaN ray accepted");
    check(pick_model(a, doc, {{.25f, .25f, 2}, {0, 0, -1e30f}}).has_value(),
          "Large finite ray magnitude overflowed");
    Matrix preview{2, 0, 0, 4, 0, .5f, 0, -2, 0, 0, 3, 10};
    options = {};
    options.preview_object = int(prop);
    options.transform_preview = &preview;
    hit = pick_model(a, doc, {{4.5f, -1.9f, 12}, {0, 0, -2}}, options);
    check(hit && std::abs(hit->distance - 2) < .0001f &&
              std::abs(hit->point.z - 10) < .0001f,
          "Nonuniform scale preview changed ray parameter");
    check(!pick_model(a, doc, ray, options),
          "Transform preview kept picking stale geometry");
    preview = {0, 0, 1, 10, 0, 1, 0, 0, -1, 0, 0, 0};
    hit = pick_model(a, doc, {{12, .25f, -.25f}, {-4, 0, 0}}, options);
    check(hit && std::abs(hit->distance - 2) < .0001f &&
              std::abs(hit->normal.x) > .99f,
          "Rotated preview surface/normal wrong");
    preview = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    check(
        pick_model(a, doc, {{-.25f, .25f, 2}, {0, 0, -1}}, options).has_value(),
        "Mirrored affine model not selectable");
    preview[0] = 0;
    check(!pick_model(a, doc, ray, options),
          "Singular model transform accepted");
    options = {};
    options.preview_object = int(prop);
    options.preview_position = {0, 0, 4};
    hit = pick_model(a, doc, {{.25f, .25f, 6}, {0, 0, -1}}, options);
    check(hit && std::abs(hit->distance - 2) < .0001f,
          "Move preview surface not selectable");
    a.textures.push_back({1, 1, {0x00ffffff}, false});
    a.batches[0].texture = 0;
    a.batches[0].blend_option = 1;
    check(!pick_model(a, doc, ray), "Transparent alpha-test hole selected");
    a.textures[0].rgba[0] = 0xffffffff;
    check(pick_model(a, doc, ray).has_value(),
          "Opaque alpha-test texel rejected");
    a.textures[0].rgba[0] = 0x80ffffff;
    check(!pick_model(a, doc, ray), "Native alpha threshold boundary selected");
    // The viewport/native mapping is0=Repeat,1=Clamp,2=Mirror. A1x1 texture
    // cannot expose a swapped0/1 mapping, so probe outside a two-texel image.
    a.textures[0] = {2, 1, {0x00ffffff, 0xffffffff}, false};
    for (auto &v : a.vertices) v.u = 1.25f;
    a.batches[0].wrap_u = 1;
    check(pick_model(a, doc, ray).has_value(), "Native Clamp1 did not keep opaque edge texel");
    a.batches[0].wrap_u = 0;
    check(!pick_model(a, doc, ray), "Native Repeat0 did not wrap into transparent texel");
    a.batches[0].wrap_u = 2;
    check(pick_model(a, doc, ray).has_value(), "Native Mirror2 did not reflect opaque texel");
    for (auto &v : a.vertices) v.u = -.25f;
    a.batches[0].wrap_u = 1;
    check(!pick_model(a, doc, ray), "Negative Clamp1 did not keep transparent edge texel");
    a.batches[0].wrap_u = 0;
    check(pick_model(a, doc, ray).has_value(), "Negative Repeat0 did not wrap to opaque texel");
    a.batches[0].wrap_u = 2;
    check(!pick_model(a, doc, ray), "Negative Mirror2 did not reflect transparent texel");
    a.batches[0].wrap_u = 1;
    a.batches[0].layers[0] = 0;
    a.batches[0].operations[0] = 2;
    for (auto &v : a.vertices) { v.u1 = 1.25f; v.v1 = .5f; }
    a.batches[0].layer_u[0] = 1;
    check(pick_model(a, doc, ray).has_value(), "Layer alpha override ignored native Clamp1");
    a.batches[0].layer_u[0] = 0;
    check(!pick_model(a, doc, ray), "Layer alpha override ignored native Repeat0");
    a.batches[0].operations[0] = 0;
    a.batches[0].base_mode = 0;
    for (auto &v : a.vertices)
      v.color[3] = 0;
    check(!pick_model(a, doc, ray), "Transparent vertex alpha selected");
    a.batches[0].enemy_preview = true;
    options = {};
    options.alpha_test = false;
    options.cut_height = .2f;
    check(pick_model(a, doc, ray, options).has_value(),
          "Visible enemy preview above cutaway plane was not selectable");
    options.show_enemies = false;
    check(!pick_model(a, doc, ray, options), "Hidden enemy model selected");
    a.batches[0].enemy_preview = false;
    a.batches[0].sky = true;
    check(!pick_model(a, doc, ray, options), "Sky model selected");
    a = planes(prop);
    const auto other = doc.duplicate(prop, {0, 0, 0});
    a.batches[0].matrix[11] = 10;
    a.batches.push_back(plane(other, 5));
    a.prepare_pick_bounds();
    hit = pick_model(a, doc, {{.25f, .25f, 20}, {0, 0, -1}});
    check(hit && hit->object == prop && std::abs(hit->distance - 10) < .0001f,
          "Nearest world surface did not win");
    options = {};
    options.accept_object = [&](size_t i) { return i == other; };
    hit = pick_model(a, doc, {{.25f, .25f, 20}, {0, 0, -1}}, options);
    check(hit && hit->object == other && std::abs(hit->distance - 15) < .0001f,
          "Filtered surface candidate not selected");
    // Dense instancing bound: 20,001 boxes, but only the nearest candidate's
    // one triangle is visited; a miss visits zero triangles. Bounds are cached
    // once.
    a = planes(prop);
    a.batches[0].matrix[11] = 10;
    for (unsigned i = 0; i < 20000; ++i)
      a.batches.push_back(plane(prop, -100 - float(i)));
    a.prepare_pick_bounds();
    const auto begin = std::chrono::steady_clock::now();
    hit = pick_model(a, doc, {{.25f, .25f, 20}, {0, 0, -1}}, {}, &work);
    check(hit && work.bounds_tested == 20001 && work.bounds_hit == 20001 &&
              work.triangles_tested == 1,
          "Dense nearest-first pruning regressed");
    check(!pick_model(a, doc, {{4, 4, 20}, {0, 0, -1}}, {}, &work) &&
              work.triangles_tested == 0,
          "Dense miss walked triangle data");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    std::cout << "PASS map_model_pick checks=" << checks
              << " real_models=" << real_models
              << " dense_boxes=20001 dense_clicks_ms=" << elapsed << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
