#include "map_document.h"
#include "map_scene.h"
#include "owned_gc_maps.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace mapedit;
int checks = 0;
void check(bool v) {
  ++checks;
  if (!v)
    throw std::runtime_error("3D assertion " + std::to_string(checks));
}

// Independent original-game camera oracle: the native view is inverse pose
// followed by negation of the depth row (EU setup_scene_camera 8014D904).
// A native identity pose therefore displays world +X on the right, looking +Z.
void camera_orientation_checks() {
  constexpr float pi = 3.14159265358979323846f;
  for (float heading : {0.f, pi / 2, pi, -pi / 2})
    for (float elevation : {-.4f, 0.f, .55f}) {
      V3 forward{std::sin(heading) * std::cos(elevation), std::sin(elevation),
                 std::cos(heading) * std::cos(elevation)};
      V3 right{std::cos(heading), 0, -std::sin(heading)};
      V3 up{-std::sin(heading) * std::sin(elevation), std::cos(elevation),
            -std::cos(heading) * std::sin(elevation)};
      Camera c;
      c.width = 900;
      c.height = 600;
      c.distance = 30;
      c.yaw = heading + pi;
      c.pitch = -elevation;
      const V3 eye{7, 11, 17};
      c.target = eye + forward * c.distance;
      check(length(c.eye() - eye) < .00003f);
      check(length(c.forward() - forward) < .000001f);
      check(length(c.right() - right) < .000001f);
      check(length(c.up() - up) < .000001f);
      auto point = eye + forward * 20 + right * 3 + up * 2;
      auto q = c.project(point);
      check(bool(q));
      const float scale = 600 / (2 * std::tan(pi / 6));
      check(std::abs(q->x - (450 + 3 * scale / 20)) < .001f);
      check(std::abs(q->y - (300 - 2 * scale / 20)) < .001f);
      auto ray = c.ray(q->x, q->y);
      check(length(cross(normalized(point - eye), ray.direction)) < .000001f);
      auto m = c.matrix();
      float v[4]{point.x, point.y, point.z, 1}, clip[4]{};
      for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
          clip[row] += m[col * 4 + row] * v[col];
      check(std::abs((clip[0] / clip[3] + 1) * 450 - q->x) < .001f);
      check(std::abs((1 - clip[1] / clip[3]) * 300 - q->y) < .001f);
      for (int i = 0; i < 3; ++i) {
        V3 axis{};
        axis[i] = 1;
        auto p = c.target;
        auto a = c.project(p), b = c.project(p + axis * 3);
        check(a && b);
        // The exactly end-on axis has no screen handle; skip only that case.
        if (length(cross(axis, forward)) > .1f) {
          auto start = axis_parameter(c.ray(a->x, a->y), p, axis),
               end = axis_parameter(c.ray(b->x, b->y), p, axis);
          check(start && end && std::abs((*end - *start) - 3) < .002f);
        }
      }
      for (auto command : {V3{1, 0, 0}, V3{-1, 0, 0}, V3{0, 0, 1}, V3{0, 0, -1},
                           V3{0, 1, 0}, V3{0, -1, 0}}) {
        auto moved = c;
        moved.fly(command.x, command.y, command.z, .1f, 20);
        auto expected =
            right * command.x + V3{0, 1, 0} * command.y + forward * command.z;
        check(length((moved.eye() - c.eye()) - expected * 2) < .00003f);
      }
      auto look = c;
      look.look(10, 0);
      check(dot(look.forward() - forward, right) > 0);
      check(length(look.eye() - eye) < .00003f);
      look = c;
      look.look(0, 10);
      check(dot(look.forward() - forward, up) < 0);
      check(length(look.eye() - eye) < .00003f);
      auto pan = c;
      pan.pan(12, 0);
      check(dot(pan.eye() - eye, right) < 0);
    }
}
int main(int argc, char **argv) {
  try {
    camera_orientation_checks();
    check(argc == 2);
    size_t maps = 0, original_maps = 0, additional_maps = 0, triangles = 0;
    for (auto &e : std::filesystem::directory_iterator(
             std::filesystem::path(argv[1]) / "files/Levels"))
      if (e.path().extension() == ".ssw") {
        Document d;
        d.load(e.path());
        auto before = d.payload();
        auto s = Scene::parse(before);
        check(!s.triangles().empty());
        check(d.payload() == before);
        triangles += s.triangles().size();
        ++maps;
        if (original_gc_map(e.path().filename().string()))
          ++original_maps;
        else
          ++additional_maps;
        Camera c;
        c.width = 900;
        c.height = 600;
        c.fit(s.bounds());
        for (auto t : s.triangles()) {
          check(t.a < s.vertices().size() && t.b < s.vertices().size() &&
                t.c < s.vertices().size());
        }
        auto center = c.project(c.target);
        check(center && std::abs(center->x - 450) < .01 &&
              std::abs(center->y - 300) < .01);
        for (auto v : {c.target, c.target + c.right() * 3 + c.up() * 2}) {
          auto q = c.project(v);
          check(bool(q));
          auto r = c.ray(q->x, q->y);
          check(length(cross(normalized(v - r.origin), r.direction)) < .0001f);
          auto m = c.matrix();
          float p[4]{v.x, v.y, v.z, 1}, o[4]{};
          for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
              o[i] += m[j * 4 + i] * p[j];
          check(std::abs((o[0] / o[3] + 1) * 450 - q->x) < .03f &&
                std::abs((1 - o[1] / o[3]) * 300 - q->y) < .03f);
        }
        auto t = s.triangles().front();
        auto a = s.vertices()[t.a], b = s.vertices()[t.b],
             cc = s.vertices()[t.c], p = (a + b + cc) / 3.f,
             n = normalized(cross(b - a, cc - a));
        auto hit = s.intersect({p + n * .01f, n * -1}, .02f);
        check(hit && length(hit->point - p) < .001f);
        check(!s.intersect({p + n * .01f, n * -1}, .02f, p.y - .1f));
        auto corrupt = before;
        corrupt[0x10] = 255;
        bool rejected = false;
        try {
          Scene::parse(corrupt);
        } catch (...) {
          rejected = true;
        }
        check(rejected);
        if (e.path().stem() == "Rlevel1_1") {
          auto floor = s.intersect({{2.5f, 0, 86.5f}, {0, -1, 0}}, 10);
          check(floor && std::abs(floor->point.y + 1.25f) < .01f);
        }
      }
    check(original_maps == 49 && maps == original_maps + additional_maps);
    Camera c;
    c.width = 900;
    c.height = 600;
    c.focus({2, -1, 80}, 40);
    for (unsigned i = 0; i < 3; ++i) {
      V3 axis;
      axis[i] = 1;
      auto p = c.target;
      auto q = c.project(p + axis * 4);
      check(bool(q));
      auto t = axis_parameter(c.ray(q->x, q->y), p, axis);
      check(t && std::abs(*t - 4) < .001f);
    }
    check(!horizontal_point({{}, {1, 0, 0}}, 0));
    check(!axis_parameter({{}, {1, 0, 0}}, {}, {1, 0, 0}));
    std::cout << maps << " maps (original_maps=" << original_maps
              << ", additional_maps=" << additional_maps << "), "
              << triangles << " triangles, " << checks
              << " assertions passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
