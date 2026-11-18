#include "map_appearance.h"
#include "gc/decompress.h"
#include "map_model_pose.h"
#include "map_texture_decode.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mapedit {
namespace {
Matrix actor_preview_matrix(const Matrix &, PreviewPlacement);
}
namespace {
struct Reader {
  const std::vector<uint8_t> &b;
  void range(uint64_t p, uint64_t n) const {
    if (p > b.size() || n > b.size() - p)
      throw std::runtime_error("Appearance data outside asset");
  }
  uint32_t u(uint64_t p) const {
    range(p, 4);
    return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
           uint32_t(b[p + 2]) << 8 | b[p + 3];
  }
  uint16_t h(uint64_t p) const {
    range(p, 2);
    return uint16_t(b[p]) << 8 | b[p + 1];
  }
  float f(uint64_t p) const {
    float v = std::bit_cast<float>(u(p));
    if (!std::isfinite(v) || std::abs(v) > 1e8)
      throw std::runtime_error("Non-finite appearance coordinate");
    return v;
  }
  std::vector<uint32_t> chain(uint32_t field) const {
    std::vector<uint32_t> out;
    std::set<uint32_t> seen;
    for (auto p = u(field); p; p = u(p)) {
      range(p, 4);
      if (!seen.insert(p).second || out.size() > 100000)
        throw std::runtime_error("Cyclic appearance list");
      out.push_back(p);
    }
    return out;
  }
  std::string name(uint32_t p) const {
    if (!p)
      return {};
    range(p, 1);
    size_t e = p;
    while (e < b.size() && b[e] && e - p < 1024)
      ++e;
    if (e == b.size() || e - p == 1024)
      throw std::runtime_error("Invalid appearance name");
    return std::string(b.begin() + p, b.begin() + e);
  }
  Matrix matrix(uint32_t p) const {
    Matrix m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (p) {
      range(p, 48);
      for (unsigned i = 0; i < 12; ++i)
        m[i] = f(uint64_t(p) + i * 4);
    }
    return m;
  }
};
std::vector<uint8_t> unpack(const std::filesystem::path &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error("Cannot read model archive");
  auto size = f.tellg();
  if (size < 64 || size > 128 * 1024 * 1024)
    throw std::runtime_error("Asset size limit");
  std::vector<uint8_t> b(size_t(size), 0);
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(b.data()), b.size()))
    throw std::runtime_error("Cannot read asset");
  Reader r{b};
  if (std::memcmp(b.data(), "CLIMAX-", 7))
    throw std::runtime_error("Unknown asset container");
  auto n = r.u(12), packed = r.u(16);
  r.range(64, packed);
  if (n > 128 * 1024 * 1024)
    throw std::runtime_error("Inflated asset size limit");
  std::vector<uint8_t> out(n);
  if (!gc::decomp::inflate(b.data() + 64, packed, out.data(), out.size()))
    throw std::runtime_error("Corrupt model archive");
  return out;
}
struct Array {
  uint32_t data = 0, count = 0, stride = 0;
};
Array array(const Reader &r, uint32_t descriptor) {
  if (!descriptor)
    return {};
  r.range(descriptor, 16);
  Array a{r.u(descriptor), r.u(descriptor + 8), r.u(descriptor + 12)};
  if (a.count > 2000000 || uint64_t(a.count) * a.stride != r.u(descriptor + 4))
    throw std::runtime_error("Invalid appearance array");
  r.range(a.data, uint64_t(a.count) * a.stride);
  return a;
}
using Arrays = std::array<Array, 12>;
Arrays root_arrays(const Reader &r) {
  Arrays a{};
  const uint32_t fields[] = {0x10, 0x14, 0x18, 0x1c, 0x20, 0x24,
                             0x28, 0x2c, 0x60, 0x64, 0x68, 0x6c};
  for (unsigned i = 0; i < 12; ++i)
    a[i] = array(r, r.u(fields[i]));
  return a;
}
Arrays source_arrays(const Reader &r, uint32_t p, bool sky = false) {
  Arrays a{};
  unsigned start = sky ? 12 : 16;
  a[0] = array(r, r.u(p + start));
  a[1] = array(r, r.u(p + start + 4));
  a[2] = array(r, r.u(p + start + 8));
  a[4] = array(r, r.u(p + start + 12));
  return a;
}
struct Builder {
  Appearance out;
  const ModelPose *active_pose = nullptr;
  const Document &doc;
  Reader level;
  std::map<std::pair<const void *, uint32_t>, int> texture_cache;
  size_t texture_bytes = 0;
  std::map<uint32_t, int> transforms, instances;
  explicit Builder(const Document &d) : doc(d), level{d.payload()} {
    auto nodes = level.chain(0x58);
    std::set<uint32_t> valid(nodes.begin(), nodes.end());
    for (size_t i = 0; i < d.objects().size(); ++i) {
      auto &o = d.objects()[i];
      transforms[o.transform] = int(i);
      if (o.type == 4200) {
        auto config = level.u(o.offset + 16);
        if (config) {
          auto node = level.u(config + 44);
          if (valid.contains(node))
            instances[node] = int(i);
        }
      }
    }
  }
  int texture(const Reader &r, uint32_t p) {
    if (!p)
      return -1;
    auto key = std::make_pair(static_cast<const void *>(&r.b), p);
    if (auto it = texture_cache.find(key); it != texture_cache.end())
      return it->second;
    r.range(p, 40);
    uint32_t format = r.u(p + 4), w = r.u(p + 8), h = r.u(p + 12),
             pixels = r.u(p + 24);
    if (format > 4 || !w || !h || w > 4096 || h > 4096)
      throw std::runtime_error("Unsupported texture layout");
    const unsigned formats[] = {4, 5, 6, 14, 2};
    auto gx = formats[format];
    auto bytes = ssne::graphics::detail::data_size(gx, int(w), int(h));
    r.range(pixels, bytes);
    if (texture_bytes + uint64_t(w) * h * 4 > 512 * 1024 * 1024)
      throw std::runtime_error("Editor texture memory limit");
    struct Source {
      const Reader &r;
      uint32_t p;
      uint8_t b(uint32_t o) const { return r.b[p + o]; }
      uint16_t h(uint32_t o) const { return r.h(p + o); }
    } source{r, pixels};
    VisualTexture t;
    t.width = w;
    t.height = h;
    ssne::graphics::detail::decode(source, gx, int(w), int(h), 0, t.rgba,
                                   [](uint32_t, uint32_t) { return 0u; });
    for (auto c : t.rgba) {
      auto alpha = c >> 24;
      if (alpha > 0 && alpha < 255) {
        t.translucent = true;
        break;
      }
    }
    texture_bytes += size_t(w) * h * 4;
    int id = int(out.textures.size());
    out.textures.push_back(std::move(t));
    texture_cache[key] = id;
    return id;
  }
  VisualVertex vertex(const Reader &r, const Arrays &a, uint32_t pos,
                      uint32_t norm, uint32_t color, uint32_t uv) {
    if (pos >= a[0].count || a[0].stride != 12)
      throw std::runtime_error("Missing render position");
    VisualVertex v;
    auto p = a[0].data + pos * 12;
    v.position = {r.f(p), r.f(p + 4), r.f(p + 8)};
    if (a[1].count) {
      if (norm >= a[1].count || a[1].stride != 12)
        throw std::runtime_error("Missing render normal");
      p = a[1].data + norm * 12;
      v.normal = {r.f(p), r.f(p + 4), r.f(p + 8)};
    }
    if (a[2].count) {
      if (color >= a[2].count || a[2].stride != 4)
        throw std::runtime_error("Missing render color");
      p = a[2].data + color * 4;
      for (unsigned c = 0; c < 4; ++c)
        v.color[c] = r.b[p + c] / 255.f;
    }
    if (active_pose) {
      if (pos >= active_pose->positions.size())
        throw std::runtime_error("Preview vertex outside skinned pose");
      v.position = active_pose->positions[pos];
      if (norm < active_pose->normals.size())
        v.normal = active_pose->normals[norm];
    }
    if (a[4].count) {
      if (uv >= a[4].count)
        throw std::runtime_error("Missing render UV");
      p = a[4].data + uv * a[4].stride;
      if (a[4].stride == 8) {
        v.u = r.f(p);
        v.v = r.f(p + 4);
      } else if (a[4].stride == 4) {
        v.u = r.h(p) / 65536.f;
        v.v = r.h(p + 2) / 65536.f;
      } else
        throw std::runtime_error("Unsupported render UV stride");
    }
    return v;
  }
  void batch(const Reader &r, uint32_t descriptor, const Arrays &a,
             uint32_t material, uint32_t matrix, int object = -1,
             bool sky = false, bool model_asset = false,
             unsigned material_kind = 0) {
    r.range(descriptor, 16);
    r.range(material, 76);
    auto format = r.u(descriptor + 12), p = r.u(descriptor),
         padded = r.u(descriptor + 4), used = r.u(descriptor + 8);
    if (format > 5 || used > padded || padded - used > 31 ||
        used > 16 * 1024 * 1024)
      throw std::runtime_error("Invalid render primitive descriptor");
    r.range(p, padded);
    VisualBatch b;
    b.source_material = material;
    b.base_mode = r.u(material + 36) == 0
                      ? 0
                      : (model_asset || material_kind >= 2 ? 2 : 1);
    b.blend_option = (r.u(material + 40) & 2) ? 1 : 0;
    if (!model_asset) {
      auto blend = r.u(material + 56), effect = r.u(material + 64);
      if (blend == 2)
        b.blend_option = 1;
      else if (blend == 3)
        b.blend_option = 4;
      if (effect >= 1 && effect <= 4)
        b.blend_option = 2;
      else if (effect == 20 || effect == 21)
        b.blend_option = 4;
    }
    b.first = uint32_t(out.vertices.size());
    b.texture = texture(r, r.u(material + 16));
    b.wrap_u = r.b[material + 51];
    b.wrap_v = r.b[material + 50];
    b.matrix = r.matrix(matrix);
    b.matrix_offset = (&r.b == &level.b) ? matrix : 0;
    b.object = object;
    b.sky = sky;
    if (b.object < 0 && transforms.contains(b.matrix_offset))
      b.object = transforms.at(b.matrix_offset);
    if (b.object >= 0)
      b.reference = doc.matrix(size_t(b.object));
    if (&r.b == &level.b && !model_asset) {
      auto layers = r.chain(material + 28);
      for (size_t i = 0; i < std::min(size_t(2), layers.size()); ++i) {
        auto layer = layers[i];
        b.layers[i] = texture(r, r.u(layer + 4));
        b.operations[i] = r.u(layer + 8);
        r.range(layer, 16);
        b.layer_u[i] = r.b[layer + 15];
        b.layer_v[i] = r.b[layer + 14];
      }
    }
    unsigned nuv = format == 3 ? 8 : format == 1 ? 4 : 1;
    bool two = format == 1 || format == 3, wide = format == 2 || format == 4;
    unsigned stride =
        (format == 0 ? 1 : 0) + 4 + (wide ? 2 : 1) + (two ? 1 : 0) + nuv * 2;
    uint32_t cursor = 0;
    while (cursor < used) {
      uint8_t op = r.b[p + cursor];
      if (!op)
        break;
      auto kind = op & 0xf8;
      if ((op & 7) || (kind != 0x80 && kind != 0x88 && kind != 0x90 &&
                       kind != 0x98 && kind != 0xa0))
        throw std::runtime_error("Unsupported render primitive");
      if (used - cursor < 3)
        throw std::runtime_error("Short render primitive");
      uint32_t n = r.h(p + cursor + 1);
      cursor += 3;
      if (uint64_t(n) * stride > used - cursor)
        throw std::runtime_error("Short render vertices");
      std::vector<VisualVertex> v;
      v.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        auto field = p + cursor + i * stride + (format == 0 ? 1 : 0),
             u = field + 4 + (wide ? 2 : 1) + (two ? 1 : 0);
        auto value = vertex(r, a, r.h(field), r.h(field + 2),
                            wide ? r.h(field + 4) : r.b[field + 4], r.h(u));
        value.u1 = value.u2 = value.u;
        value.v1 = value.v2 = value.v;
        for (unsigned channel = 1; channel < std::min(nuv, 3u); ++channel) {
          auto ar = a;
          ar[4] = a[4 + channel];
          if (!ar[4].count)
            continue;
          auto t = vertex(r, ar, r.h(field), r.h(field + 2),
                          wide ? r.h(field + 4) : r.b[field + 4],
                          r.h(u + channel * 2));
          if (channel == 1) {
            value.u1 = t.u;
            value.v1 = t.v;
          } else {
            value.u2 = t.u;
            value.v2 = t.v;
          }
        }
        v.push_back(value);
      }
      auto tri = [&](unsigned x, unsigned y, unsigned z) {
        if (out.vertices.size() > 8000000)
          throw std::runtime_error("Editor geometry memory limit");
        auto nrm = normalized(cross(v[y].position - v[x].position,
                                    v[z].position - v[x].position));
        for (auto i : {x, y, z}) {
          auto value = v[i];
          if (length(value.normal) < .1f)
            value.normal = nrm;
          out.vertices.push_back(value);
        }
      };
      if (kind == 0x80 || kind == 0x88) {
        if (n % 4)
          throw std::runtime_error("Incomplete render quad");
        for (unsigned i = 0; i < n; i += 4) {
          tri(i, i + 1, i + 2);
          tri(i, i + 2, i + 3);
        }
      } else if (kind == 0x90) {
        if (n % 3)
          throw std::runtime_error("Incomplete render triangle");
        for (unsigned i = 0; i < n; i += 3)
          tri(i, i + 1, i + 2);
      } else if (kind == 0x98) {
        for (unsigned i = 0; i + 2 < n; ++i)
          tri(i + (i & 1), i + 1 - (i & 1), i + 2);
      } else
        for (unsigned i = 1; i + 1 < n; ++i)
          tri(0, i, i + 1);
      cursor += n * stride;
    }
    b.count = uint32_t(out.vertices.size()) - b.first;
    if (b.count)
      out.batches.push_back(b);
  }
  void direct(const Reader &r, uint32_t field, const Arrays &a,
              uint32_t matrix = 0, int object = -1, bool sky = false,
              unsigned kind = 1) {
    for (auto s : r.chain(field))
      batch(r, r.u(s + 12), a, s, matrix, object, sky, false, kind);
  }
  void world() {
    auto a = root_arrays(level);
    for (auto s : level.chain(8))
      for (auto c : level.chain(s + 12))
        batch(level, level.u(c + 4), a, s, 0);
    for (auto g : level.chain(0x48)) {
      auto m = level.u(g + 8);
      if (!m) {
        auto keys = level.chain(g + 12);
        if (!keys.empty())
          m = level.u(keys.front() + 4);
      }
      direct(level, g + 16, a, m);
    }
    // Reusable local-coordinate prop geometry is instantiated through 0x58.
    for (auto node : level.chain(0x58)) {
      auto src = level.u(node + 16);
      auto color = level.u(node + 20);
      if (!src) {
        auto frames = level.chain(node + 44);
        if (!frames.empty()) {
          src = level.u(frames[0] + 8);
          color = level.u(frames[0] + 12);
        }
      }
      if (!src)
        continue;
      auto ar = source_arrays(level, src);
      if (color)
        ar[2] = array(level, color);
      direct(level, src + 4, ar, level.u(node + 4),
             instances.contains(node) ? instances.at(node) : -1, false, 2);
      ++out.instances;
    }
    if (auto sky = level.u(0x70))
      direct(level, sky, source_arrays(level, sky, true), 0, -1, true, 3);
  }
  static std::string lower(std::string s) {
    for (auto &c : s)
      c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }
  void pickups(const std::filesystem::path &data) {
    auto bytes = unpack(data / "files/Data.ssg");
    Reader common{bytes};
    struct Asset {
      const Reader *reader;
      uint32_t model;
    };
    std::map<std::string, Asset> models;
    auto add_archive = [&](const Reader &r, uint32_t root) {
      for (auto p : r.chain(root + 8))
        if (r.u(p + 8) == 0)
          models[lower(r.name(r.u(p + 4)))] = Asset{&r, r.u(p + 12)};
    };
    add_archive(common, 0);
    if (auto archive = level.u(0x7c))
      add_archive(level, archive);
    auto canonical = [&](std::string n) {
      n = lower(n);
      auto slash = n.find_last_of("/\\");
      if (slash != std::string::npos)
        n = n.substr(slash + 1);
      return n;
    };
    std::map<unsigned, std::string> names;
    std::ifstream cfg(data / "files/LevelModels.cfg");
    std::string line;
    while (std::getline(cfg, line)) {
      auto at = line.find("string ObjectModel");
      if (at == std::string::npos)
        continue;
      std::istringstream number(line.substr(at + 18));
      unsigned id;
      if (!(number >> id))
        continue;
      auto begin = line.find('"', at),
           end = begin == std::string::npos ? begin : line.find('"', begin + 1);
      if (end != std::string::npos)
        names[id] = canonical(line.substr(begin + 1, end - begin - 1));
    }
    // Original EU authored enemy kind -> ObjectModels index
    // (game_enemy_factory).
    constexpr unsigned enemy_models[] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 22, 23, 24,
        25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        45, 42, 15, 16, 17, 18, 19, 20, 21, 46, 47, 48, 49, 50, 51, 52,
        53, 54, 55, 56, 57, 58, 59, 60, 41, 43, 44, 13, 14};
    std::map<uint32_t, uint32_t> entities;
    std::map<unsigned, std::string> key_paths;
    for (auto node : level.chain(0x40)) {
      entities.emplace(level.u(node + 20), node);
      auto type = level.u(node + 12);
      if (type >= 2000 && type < 4000) {
        auto c = level.u(node + 16);
        if (c) {
          auto kind = level.u(c);
          if (kind >= 46 && kind <= 53) {
            auto entry = level.u(c + 44);
            if (entry && level.u(entry + 4))
              key_paths[kind] = canonical(level.name(level.u(entry + 4)));
          }
        }
      }
    }
    std::vector<std::unique_ptr<std::vector<uint8_t>>> extra_images;
    std::vector<std::unique_ptr<Reader>> extra_readers;
    std::set<std::string> searched;
    auto find_missing = [&](const std::string &name) {
      if (models.contains(name) || !searched.insert(name).second)
        return;
      std::vector<std::filesystem::path> files;
      for (auto &e : std::filesystem::directory_iterator(data / "files/Levels"))
        if (e.is_regular_file() && e.path().extension() == ".ssw" &&
            e.path().stem() != doc.source().stem())
          files.push_back(e.path());
      std::sort(files.begin(), files.end());
      for (auto &file : files) {
        auto image = std::make_unique<std::vector<uint8_t>>(unpack(file));
        auto reader = std::make_unique<Reader>(Reader{*image});
        auto archive = reader->u(0x7c);
        if (!archive)
          continue;
        bool found = false;
        for (auto node : reader->chain(archive + 8))
          if (reader->u(node + 8) == 0 &&
              lower(reader->name(reader->u(node + 4))) == name) {
            found = true;
            break;
          }
        if (found) {
          for (auto node : reader->chain(archive + 8))
            if (reader->u(node + 8) == 0)
              models.try_emplace(lower(reader->name(reader->u(node + 4))),
                                 Asset{reader.get(), reader->u(node + 12)});
          extra_images.push_back(std::move(image));
          extra_readers.push_back(std::move(reader));
          break;
        }
      }
    };
    struct Cached {
      std::vector<VisualBatch> batches;
      std::string pose, error;
      size_t bones = 0;
    };
    std::map<std::pair<const void *, uint32_t>, Cached> cache;
    for (size_t i = 0; i < doc.objects().size(); ++i) {
      auto &o = doc.objects()[i];
      uint32_t config = level.u(o.offset + 16), type = o.type;
      bool enemy = false, spawner = type == 4040, arrow = type == 4260;
      std::string role, model_name;
      if (!config)
        continue;
      if (spawner) {
        auto it = entities.find(level.u(config + 20));
        if (it == entities.end()) {
          out.previews.push_back(
              {int(i), {}, {}, "Spawner", "Missing authored target"});
          continue;
        }
        type = level.u(it->second + 12);
        config = level.u(it->second + 16);
        if (!config)
          continue;
      }
      if (arrow) {
        // Native CGCArrow (PAL80076BBC) selects these exact resource names
        // from cfg+0x10. It is not a pickup and must retain the authored
        // orientation. The editor deliberately shows a frozen authored pose,
        // not the game's camera-dependent bob/scale or a moving parent pose.
        level.range(config, 28);
        const auto kind = level.u(config + 16);
        role = "Arrow marker";
        if (kind == 1) model_name = "yellowarrow";
        else if (kind == 2) model_name = "greenarrow";
        else {
          out.previews.push_back({int(i), {}, {}, role,
                                  "Unsupported native arrow model kind"});
          continue;
        }
      } else if (type == 5000) {
        enemy = true;
        role = spawner ? "Enemy spawn" : "Enemy template";
        auto authored = level.u(config);
        if (authored < std::size(enemy_models) &&
            names.contains(enemy_models[authored]))
          model_name = names.at(enemy_models[authored]);
      } else if (type >= 2000 && type < 4000) {
        role = spawner ? "Pickup spawn" : "Pickup";
        unsigned kind = level.u(config), id = 63;
        if (kind >= 57 && kind <= 76) {
          constexpr int defaults[] = {36, 38, 37, 39, 40, -1, -1, -1, -1, -1,
                                      19, 22, 19, 19, 30, -1, -1, -1, -1, -1};
          int resolved = defaults[kind - 57];
          if (resolved < 0)
            continue;
          kind = unsigned(resolved);
        }
        if (kind >= 46 && kind <= 54) {
          auto entry = level.u(config + 44);
          if (kind <= 53) {
            if (entry && level.u(entry + 4))
              model_name = canonical(level.name(level.u(entry + 4)));
            else if (key_paths.contains(kind))
              model_name = key_paths.at(kind);
            role = "Key model";
          } else if (entry) {
            model_name = canonical(level.name(level.u(entry)));
            role = "Custom object";
          }
        } else {
          if (kind <= 4)
            id = 63 + kind;
          else if (kind == 5)
            id = 69;
          else if (kind <= 16)
            id = 71 + kind - 6;
          else if (kind == 17 || kind == 18)
            id = 61 + kind - 17;
          else if (kind <= 34)
            id = 110 + kind - 19;
          else if (kind <= 44)
            id = 83 + kind - 35;
          else if (kind == 45)
            id = 94;
          else if (kind == 55)
            id = 95;
          else
            continue;
          if (names.contains(id))
            model_name = names.at(id);
        }
      } else
        continue;
      if (!model_name.empty())
        find_missing(model_name);
      if (model_name.empty() || !models.contains(model_name)) {
        out.previews.push_back(
            {int(i),
             model_name,
             {},
             role,
             "Original model not present in the installed game assets"});
        continue;
      }
      const auto asset = models.at(model_name);
      const auto &r = *asset.reader;
      auto model = asset.model;
      auto key = std::make_pair(static_cast<const void *>(&r.b), model);
      if (!cache.contains(key)) {
        Cached value;
        size_t start = out.batches.size(), vertex_start = out.vertices.size();
        try {
          auto pose = preview_model_pose(r.b, model);
          value.pose = pose.name;
          value.bones = pose.bones;
          active_pose = &pose;
          Arrays ar{};
          ar[0] = array(r, r.u(model + 28));
          ar[1] = array(r, r.u(model + 32));
          ar[2] = array(r, r.u(model + 36));
          ar[4] = array(r, r.u(model + 40));
          for (auto mesh : r.chain(model + 8)) {
            if (!r.u(model + 24))
              batch(r, r.u(mesh + 12), ar, mesh, 0, -1, false, true);
            else {
              bool found = false;
              for (auto list : r.chain(mesh + 12))
                if (r.u(list + 4) == 0) {
                  batch(r, r.u(list + 8), ar, mesh, 0, -1, false, true);
                  found = true;
                }
              if (!found)
                throw std::runtime_error("No CPU preview mesh variant");
            }
          }
          active_pose = nullptr;
          value.batches.assign(out.batches.begin() + start, out.batches.end());
          out.batches.resize(start);
        } catch (const std::exception &e) {
          active_pose = nullptr;
          out.batches.resize(start);
          out.vertices.resize(vertex_start);
          value.error = e.what();
        }
        cache.emplace(key, std::move(value));
      }
      auto &cached = cache.at(key);
      out.previews.push_back(
          {int(i), model_name, cached.pose, role, cached.error, cached.bones});
      for (auto b : cached.batches) {
        b.object = int(i);
        b.reference = doc.matrix(i);
        b.placement = arrow ? PreviewPlacement::Arrow : enemy ? (spawner ? PreviewPlacement::EnemySpawner
                                       : PreviewPlacement::EnemyTemplate)
                            : PreviewPlacement::Pickup;
        b.matrix = actor_preview_matrix(b.reference, b.placement);
        b.enemy_preview = enemy;
        out.batches.push_back(b);
      }
      if (!cached.batches.empty()) {
        if (arrow)
          ++out.arrows;
        else if (enemy)
          ++out.enemies;
        else {
          ++out.pickups;
          if (o.type == 3210 || o.type == 3215 || spawner)
            ++out.extra_objects;
        }
      }
    }
  }
};
} // namespace
Appearance Appearance::load(const Document &doc,
                            const std::filesystem::path &data) {
  Builder b(doc);
  b.world();
  try {
    b.pickups(data);
  } catch (const std::exception &e) {
    b.out.warnings.push_back(std::string("Pickup archive: ") + e.what());
  }
  b.out.prepare_pick_bounds();
  return std::move(b.out);
}
void Appearance::prepare_pick_bounds() {
  // Multiple actors reuse one posed model's vertex span. Compute its local
  // bounds once even when thousands of instances reference the same triangles.
  std::map<std::pair<uint32_t, uint32_t>, Bounds> cached;
  for (auto &batch : batches) {
    batch.pick_bounds_valid = false;
    if (batch.sky || batch.object < 0 || batch.count < 3 || batch.count % 3 ||
        uint64_t(batch.first) + batch.count > vertices.size()) continue;
    const auto key = std::pair{batch.first, batch.count};
    auto it = cached.find(key);
    if (it == cached.end()) {
      const auto inf = std::numeric_limits<float>::infinity();
      Bounds box{{inf, inf, inf}, {-inf, -inf, -inf}};
      bool valid = true;
      for (size_t i = batch.first; i < size_t(batch.first) + batch.count; ++i)
        for (unsigned axis = 0; axis < 3; ++axis) {
          const auto value = vertices[i].position[axis];
          if (!std::isfinite(value)) { valid = false; break; }
          box.min[axis] = std::min(box.min[axis], value);
          box.max[axis] = std::max(box.max[axis], value);
        }
      if (!valid) continue;
      it = cached.emplace(key, box).first;
    }
    batch.local_bounds = it->second;
    batch.pick_bounds_valid = true;
  }
}
namespace {
Matrix actor_preview_matrix(const Matrix &source, PreviewPlacement placement) {
  if (placement == PreviewPlacement::Arrow)
    return source;
  // Native enemy/pickup factories consume the authored position, not its
  // basis. Enemy templates start at zero yaw; pickups rotate while playing.
  // Spawners derive actor yaw from -authored forward and negate it at spawn
  // (game_entity_factory -> NativeGameEnemySpawnerActor::spawn).
  Matrix out{1, 0, 0, source[3], 0, 1, 0, source[7], 0, 0, 1, source[11]};
  if (placement == PreviewPlacement::EnemySpawner) {
    const float horizontal = std::hypot(source[2], source[10]);
    if (horizontal > 1e-7f) {
      const float sine = -source[2] / horizontal;
      const float cosine = -source[10] / horizontal;
      out[0] = cosine;
      out[2] = sine;
      out[8] = -sine;
      out[10] = cosine;
    }
  }
  return out;
}
} // namespace
Matrix Appearance::model_matrix(const VisualBatch &b, const Document &doc,
                                int preview_object, V3 preview,
                                const Matrix *transform_preview) const {
  if (b.object < 0)
    return b.matrix;
  auto m = doc.matrix(size_t(b.object));
  const bool same_transform = preview_object >= 0 &&
      size_t(preview_object) < doc.objects().size() &&
      size_t(b.object) < doc.objects().size() &&
      doc.objects()[size_t(preview_object)].transform ==
          doc.objects()[size_t(b.object)].transform;
  if (b.object == preview_object || same_transform) {
    if (transform_preview) m = *transform_preview;
    else {
      m[3] = preview.x;
      m[7] = preview.y;
      m[11] = preview.z;
    }
  }
  if (b.placement != PreviewPlacement::Scene)
    return actor_preview_matrix(m, b.placement);
  auto &r = b.reference;
  float det = r[0] * (r[5] * r[10] - r[6] * r[9]) -
              r[1] * (r[4] * r[10] - r[6] * r[8]) +
              r[2] * (r[4] * r[9] - r[5] * r[8]);
  if (std::abs(det) < 1e-10f)
    return b.matrix;
  Matrix inv{
      (r[5] * r[10] - r[6] * r[9]) / det, (r[2] * r[9] - r[1] * r[10]) / det,
      (r[1] * r[6] - r[2] * r[5]) / det,  0,
      (r[6] * r[8] - r[4] * r[10]) / det, (r[0] * r[10] - r[2] * r[8]) / det,
      (r[2] * r[4] - r[0] * r[6]) / det,  0,
      (r[4] * r[9] - r[5] * r[8]) / det,  (r[1] * r[8] - r[0] * r[9]) / det,
      (r[0] * r[5] - r[1] * r[4]) / det,  0};
  for (unsigned i = 0; i < 3; ++i)
    inv[i * 4 + 3] =
        -inv[i * 4] * r[3] - inv[i * 4 + 1] * r[7] - inv[i * 4 + 2] * r[11];
  auto multiply = [](const Matrix &a, const Matrix &b) {
    Matrix o{};
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < 4; ++j) {
        for (unsigned k = 0; k < 3; ++k)
          o[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        if (j == 3)
          o[i * 4 + j] += a[i * 4 + 3];
      }
    return o;
  };
  return multiply(m, multiply(inv, b.matrix));
}
} // namespace mapedit
