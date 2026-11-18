#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace mapedit {
struct V3 {
  float x = 0, y = 0, z = 0;
  float &operator[](unsigned i) { return i == 0 ? x : i == 1 ? y : z; }
  float operator[](unsigned i) const { return i == 0 ? x : i == 1 ? y : z; }
};
V3 operator+(V3, V3);
V3 operator-(V3, V3);
V3 operator*(V3, float);
V3 operator/(V3, float);
float dot(V3, V3);
V3 cross(V3, V3);
float length(V3);
V3 normalized(V3);
struct Ray {
  V3 origin, direction;
};
struct Bounds {
  V3 min, max;
};
struct Triangle {
  uint32_t a = 0, b = 0, c = 0;
};
struct SurfaceHit {
  V3 point, normal;
  float distance = 0;
  uint32_t triangle = 0;
};
class Scene {
public:
  static Scene parse(const std::vector<uint8_t> &payload);
  const std::vector<V3> &vertices() const { return vertices_; }
  const std::vector<Triangle> &triangles() const { return triangles_; }
  Bounds bounds() const { return bounds_; }
  std::optional<SurfaceHit>
  intersect(Ray, float maximum = std::numeric_limits<float>::infinity(),
            float cut_height = std::numeric_limits<float>::infinity()) const;

private:
  struct Node {
    Bounds box;
    uint32_t start = 0, count = 0, left = 0, right = 0;
  };
  std::vector<V3> vertices_;
  std::vector<Triangle> triangles_;
  Bounds bounds_{};
  std::vector<Node> nodes_;
  std::vector<uint32_t> order_;
  uint32_t build(uint32_t start, uint32_t count);
};
struct Projected {
  float x = 0, y = 0, depth = 0;
};
struct Camera {
  V3 target{};
  float yaw = .5f, pitch = .65f, distance = 40.f;
  float width = 1, height = 1;
  V3 eye() const;
  V3 forward() const;
  V3 right() const;
  V3 up() const;
  std::array<float, 16> matrix() const;
  std::optional<Projected> project(V3) const;
  Ray ray(float x, float y) const;
  void orbit(float dx, float dy);
  void look(float dx, float dy);
  void fly(float side, float vertical, float ahead, float dt, float speed);
  void pan(float dx, float dy);
  void zoom(float wheel);
  void fit(Bounds);
  void focus(V3, float radius = 30.f);
};
std::optional<float> axis_parameter(Ray, V3 pivot, V3 axis);
std::optional<V3> horizontal_point(Ray, float y);
V3 snapped(V3, float grid);
} // namespace mapedit
