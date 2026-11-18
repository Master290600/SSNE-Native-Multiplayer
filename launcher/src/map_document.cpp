#include "map_document.h"
#include "gc/decompress.h"
#include "gc/sha256.h"
#include "launcher_json.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace mapedit {
namespace {
constexpr size_t limit = 128u * 1024 * 1024;
void need(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}
uint32_t word(const std::vector<uint8_t> &b, size_t p) {
  need(p <= b.size() && b.size() - p >= 4, "Map pointer is out of range");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
void word(std::vector<uint8_t> &b, size_t p, uint32_t v) {
  need(p <= b.size() && b.size() - p >= 4, "Map write is out of range");
  for (int i = 0; i < 4; ++i)
    b[p + i] = uint8_t(v >> (24 - i * 8));
}
std::vector<uint8_t> bytes(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  need(bool(f), "Cannot open map/project file");
  auto n = f.tellg();
  need(n >= 0 && uint64_t(n) <= limit, "Map/project file is too large");
  std::vector<uint8_t> b(static_cast<size_t>(n));
  f.seekg(0);
  if (!b.empty())
    f.read(reinterpret_cast<char *>(b.data()), b.size());
  need(bool(f), "Cannot read complete file");
  return b;
}
Matrix matrix_at(const std::vector<uint8_t> &b, uint32_t p) {
  Matrix m;
  for (unsigned i = 0; i < 12; ++i)
    m[i] = std::bit_cast<float>(word(b, p + i * 4));
  return m;
}
std::vector<uint8_t> inflate(const std::vector<uint8_t> &b, size_t p, size_t n,
                             size_t expected) {
  need(n >= 6 && p <= b.size() && n <= b.size() - p && expected <= limit,
       "Invalid compressed map extent");
  std::vector<uint8_t> out(expected);
  std::string err;
  need(gc::decomp::inflate(b.data() + p, n, out.data(), out.size(), true, &err),
       "Cannot decompress this map");
  need(word(b, p + n - 4) == adler32(out), "Map compression checksum differs");
  return out;
}
bool slug(const std::string &s) {
  return !s.empty() && s.size() <= 48 &&
         std::all_of(s.begin(), s.end(),
                     [](unsigned char c) {
                       return (c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-';
                     }) &&
         s.back() != '.';
}
void plain_directory(const std::filesystem::path &p) {
  need(!std::filesystem::is_symlink(p), "A project directory cannot be a link");
#ifdef _WIN32
  auto a = GetFileAttributesW(p.c_str());
  need(a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT),
       "A project directory cannot be a reparse point");
#endif
}
std::filesystem::path safe_under(const std::filesystem::path &root,
                                 const std::filesystem::path &relative) {
  need(!relative.is_absolute(), "Invalid project path");
  for (const auto &part : relative)
    need(part != ".." && part != ".", "Invalid project path component");
  auto p = root;
  plain_directory(p);
  for (const auto &part : relative) {
    p /= part;
    plain_directory(p);
  }
  return p;
}
std::string text(const launcher::JSONValue &v, const char *key) {
  auto p = v.find(key);
  need(p && p->string(), "Invalid project text field");
  return *p->string();
}
int64_t number(const launcher::JSONValue &v, const char *key) {
  auto p = v.find(key);
  int64_t n = 0;
  need(p && p->integer(n), "Invalid project integer field");
  return n;
}
double real(const launcher::JSONValue &v) {
  if (auto n = std::get_if<double>(&v.value))
    return *n;
  int64_t n;
  need(v.integer(n), "Invalid project coordinate");
  return double(n);
}
} // namespace
uint32_t adler32(const std::vector<uint8_t> &b) {
  uint32_t a = 1, c = 0;
  for (auto v : b) {
    a = (a + v) % 65521;
    c = (c + a) % 65521;
  }
  return c << 16 | a;
}
std::vector<uint8_t> stored_zlib(const std::vector<uint8_t> &b) {
  std::vector<uint8_t> out{0x78, 0x01};
  size_t at = 0;
  do {
    const size_t n = std::min<size_t>(65535, b.size() - at);
    out.push_back(at + n == b.size() ? 1 : 0);
    for (auto v : {uint16_t(n), uint16_t(~uint16_t(n))}) {
      out.push_back(uint8_t(v));
      out.push_back(uint8_t(v >> 8));
    }
    out.insert(out.end(), b.begin() + at, b.begin() + at + n);
    at += n;
  } while (at < b.size());
  auto sum = adler32(b);
  for (int i = 3; i >= 0; --i)
    out.push_back(uint8_t(sum >> (i * 8)));
  return out;
}
void atomic_write(const std::filesystem::path &p,
                  const std::vector<uint8_t> &b) {
  plain_directory(p);
  auto tmp = p;
  tmp += ".editor-pending-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count());
  need(!std::filesystem::exists(tmp), "Temporary export already exists");
  try {
    std::ofstream f(tmp, std::ios::binary);
    need(bool(f), "Cannot create map export");
    f.write(reinterpret_cast<const char *>(b.data()), b.size());
    f.close();
    need(bool(f), "Cannot finish map export");
#ifdef _WIN32
    need(MoveFileExW(tmp.c_str(), p.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
         "Cannot replace map export; close its test game first");
#else
    std::filesystem::rename(tmp, p);
#endif
  } catch (...) {
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    throw;
  }
}
void Document::load(const std::filesystem::path &path) {
  auto file = bytes(path);
  Document next;
  next.parse(std::move(file));
  next.source_ = std::filesystem::weakly_canonical(path);
  *this = std::move(next);
}
void Document::parse(std::vector<uint8_t> file) {
  logic_lookup_valid_=false;
  need(file.size() >= 64 && !std::memcmp(file.data(), "CLIMAX-\0", 8),
       "Only original CLIMAX SSW maps are supported");
  const auto size = word(file, 12), compressed = word(file, 16),
             reloc = word(file, 20), count = word(file, 8);
  need(size >= 0x80 && size <= limit && count <= limit / 4 &&
           uint64_t(64) + compressed + reloc == file.size(),
       "Invalid map container lengths");
  auto data = inflate(file, 64, compressed, size);
  auto pointers = inflate(file, 64 + compressed, reloc, uint64_t(count) * 4);
  need(word(data, 0) == 1 && word(data, 4) == 100,
       "Unsupported map root version");
  std::set<uint32_t> fields;
  for (size_t at = 0; at < pointers.size(); at += 4) {
    auto v = word(pointers, at);
    if (v & 0x80000000u) {
      need(at + 8 <= pointers.size(), "Incomplete relocation run");
      auto start = word(pointers, at + 4), n = (v >> 10) & 0x1fffff,
           stride = v & 1023;
      need(fields.size() + n <= 2000000, "Map relocation limit");
      for (uint32_t j = 0; j < n; ++j) {
        uint64_t field = uint64_t(start) + uint64_t(j) * stride;
        need(field + 4 <= data.size(), "Relocation outside map");
        fields.insert(uint32_t(field));
      }
      at += 4;
    } else {
      need(uint64_t(v) + 4 <= data.size(), "Relocation outside map");
      fields.insert(v);
    }
  }
  for (auto field : fields)
    need(word(data, field) > 0 && word(data, field) < data.size(),
         "Invalid relocated pointer target");
  std::vector<Object> objects;
  std::set<uint32_t> seen;
  auto p = word(data, 0x40);
  while (p) {
    need(p % 4 == 0 && p + uint64_t(44) <= data.size() &&
             seen.insert(p).second && seen.size() <= 50000,
         "Invalid/cyclic map object list");
    Object o;
    o.offset = p;
    o.transform = word(data, p + 4);
    o.type = word(data, p + 12);
    auto name = word(data, p + 8);
    if (o.transform == 0) {
      need(o.type >= 1000 && o.type <= 10000,
           "Invalid non-spatial object type");
      p = word(data, p);
      continue;
    }
    need(o.type >= 1000 && o.type <= 10000 && o.transform >= 0x80 &&
             o.transform % 4 == 0 &&
             o.transform + uint64_t(48) <= data.size() && name < data.size(),
         "Unsupported map object descriptor");
    for (unsigned i = 0; i < 256; ++i) {
      need(name + i < data.size(), "Invalid object name");
      auto ch = data[name + i];
      if (!ch)
        break;
      need(i < 255 && ch >= 32, "Invalid object name encoding");
      o.name += char(ch);
    }
    auto m = matrix_at(data, o.transform);
    for (float v : m)
      need(std::isfinite(v) && std::abs(v) <= 1000000.f,
           "Invalid object transform");
    if (!wave_helper(o.type, o.name))
      objects.push_back(std::move(o));
    p = word(data, p);
  }
  need(!objects.empty(), "This map contains no editable object list");
  file_ = std::move(file);
  base_ = data;
  saved_ = data;
  data_ = std::move(data);
  objects_ = std::move(objects);
  relocations_ = base_relocations_ = saved_relocations_ = std::move(fields);
  next_identifier_ = 0;
  for (auto node = word(data_, 0x40); node; node = word(data_, node)) {
    auto id = word(data_, node + 20);
    if (id < 32767)
      next_identifier_ = std::max(next_identifier_, id + 1);
  }
  structure_revision_ = 0;
  undo_.clear();
  redo_.clear();
  source_.clear();
  imports_.clear();
}
Matrix Document::matrix(size_t i) const {
  need(i < objects_.size(), "Unknown map object");
  return matrix_at(data_, objects_[i].transform);
}
Matrix Document::original_matrix(size_t i) const {
  need(i < objects_.size(), "Unknown map object");
  if (objects_[i].added)
    return objects_[i].initial;
  return matrix_at(base_, word(base_, objects_[i].offset + 4));
}
void Document::put(uint32_t p, const Matrix &m) {
  for (unsigned i = 0; i < 12; ++i)
    word(data_, p + i * 4, std::bit_cast<uint32_t>(m[i]));
}
uint32_t Document::transform_type(size_t index) const {
  need(index<objects_.size(),"Unknown map object");
  const auto& object=objects_[index];
  if(object.type==4180&&(object.name.starts_with("Editor Button ")||object.name.starts_with("Editor Toggle ")))
    for(const auto& paired:objects_)if(paired.type==4200&&paired.transform==object.transform)return 4200;
  return object.type;
}
void Document::set_matrix(size_t index, const Matrix &m) {
  auto previous = matrix(index), original = original_matrix(index);
  for (auto v : m)
    need(std::isfinite(v) && std::abs(v) <= 1000000.f,
         "Coordinates must be finite and within +/-1,000,000");
  const auto type=transform_type(index);
  need(type!=4070 || m==previous,
       "Moving doors/brushes use authored motion paths; transform editing requires path/geometry editing");
  auto same=[](double a,double b){return std::abs(a-b)<=.00001*std::max({1.,std::abs(a),std::abs(b)});};
  if(type!=4200 && type!=4260 && type!=4040) {
    // Native point-entity factories consume position rather than this basis.
    // Refuse edits the running game would silently ignore.
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
      need(same(previous[r*4+c],m[r*4+c]),
           "This point entity supports movement only; configure its radius or direction in properties");
  } else if(type==4040) {
    for(unsigned c=0;c<3;++c)
      need(same(original[4+c],m[4+c]),"Enemy spawners support Y rotation only");
    for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b){
      double old=0,now=0;for(unsigned r=0;r<3;++r){old+=double(original[r*4+a])*original[r*4+b];now+=double(m[r*4+a])*m[r*4+b];}
      need(same(old,now),"Enemy spawner scale cannot be changed by its transform");
    }
  } else {
    auto determinant=[](const Matrix& v){return double(v[0])*(double(v[5])*v[10]-double(v[6])*v[9])-double(v[1])*(double(v[4])*v[10]-double(v[6])*v[8])+double(v[2])*(double(v[4])*v[9]-double(v[5])*v[8]);};
    const auto old_det=determinant(original),new_det=determinant(m);
    need(std::abs(new_det)>1e-10 && old_det*new_det>0,
         "Model transform cannot collapse or introduce a mirror");
    double old_length[3]{},new_length[3]{};
    for(unsigned c=0;c<3;++c){for(unsigned r=0;r<3;++r){old_length[c]+=double(original[r*4+c])*original[r*4+c];new_length[c]+=double(m[r*4+c])*m[r*4+c];}old_length[c]=std::sqrt(old_length[c]);new_length[c]=std::sqrt(new_length[c]);
      need(new_length[c]>=.0001 && new_length[c]<=10000,"Model axis scale must remain between 0.0001 and 10,000");}
    // Local non-uniform scale and arbitrary rotation preserve basis angles.
    // Prevent accidental shear, which is not supported by native prop collision.
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b){double old=0,now=0;for(unsigned r=0;r<3;++r){old+=double(original[r*4+a])*original[r*4+b];now+=double(m[r*4+a])*m[r*4+b];}
      need(old_length[a]>1e-8&&old_length[b]>1e-8&&std::abs(old/(old_length[a]*old_length[b])-now/(new_length[a]*new_length[b]))<.001,"Model transform cannot introduce shear; scale along its local axes");}
  }
  if (m == previous)
    return;
  transaction([&] {
    put(objects_[index].transform, m);
    move_prop_bounds(index, previous, m);
    move_logic_path(index,previous,m);
  });
}
void Document::move(size_t i, std::array<float, 3> p) {
  auto m = matrix(i);
  for (unsigned r = 0; r < 3; ++r)
    m[r * 4 + 3] = p[r];
  set_matrix(i, m);
}
void Document::rotate_y(size_t i, float degrees) {
  need(std::isfinite(degrees) && std::abs(degrees) <= 3600, "Invalid rotation");
  auto m = matrix(i);
  const double a = degrees * 3.14159265358979323846 / 180., c = std::cos(a),
               s = std::sin(a);
  for (unsigned j = 0; j < 3; ++j) {
    float x = m[j], z = m[8 + j];
    m[j] = float(c * x + s * z);
    m[8 + j] = float(-s * x + c * z);
  }
  set_matrix(i, m);
}
size_t Document::edited_objects() const {
  if (base_.empty())
    return 0;
  std::set<uint32_t> original;
  for (auto p = word(base_, 0x40); p; p = word(base_, p))
    if (word(base_, p + 4))
      original.insert(p);
  size_t n = 0;
  for (size_t i = 0; i < objects_.size(); ++i) {
    n += objects_[i].added || matrix(i) != original_matrix(i);
    original.erase(objects_[i].offset);
  }
  return n + original.size();
}
std::vector<uint8_t> Document::export_bytes() const {
  if (data_ == base_ && relocations_ == base_relocations_)
    return file_;
  auto zip = stored_zlib(data_);
  std::vector<uint8_t> out(file_.begin(), file_.begin() + 64);
  word(out, 12, uint32_t(data_.size()));
  word(out, 16, uint32_t(zip.size()));
  out.insert(out.end(), zip.begin(), zip.end());
  if (relocations_ == base_relocations_) {
    auto start = 64 + word(file_, 16);
    out.insert(out.end(), file_.begin() + start, file_.end());
  } else {
    std::vector<uint8_t> table(relocations_.size() * 4);
    size_t at = 0;
    for (auto field : relocations_) {
      need(uint64_t(field) + 4 <= data_.size() && word(data_, field) > 0 &&
               word(data_, field) < data_.size(),
           "Invalid structural export relocation");
      word(table, at, field);
      at += 4;
    }
    auto packed = stored_zlib(table);
    word(out, 8, uint32_t(relocations_.size()));
    word(out, 20, uint32_t(packed.size()));
    out.insert(out.end(), packed.begin(), packed.end());
  }
  return out;
}
void Document::write_copy(const std::filesystem::path &p) const {
  need(source_.empty() || std::filesystem::weakly_canonical(p) != source_,
       "The original map cannot be overwritten");
  if (!source_.empty() && std::filesystem::exists(p))
    need(!std::filesystem::equivalent(p, source_),
         "The original map cannot be overwritten through a hard link");
  std::filesystem::create_directories(p.parent_path());
  atomic_write(p, export_bytes());
}
std::string Document::source_hash() const {
  gc::Sha256 h;
  h.update(file_.data(), file_.size());
  return gc::Sha256::hex(h.finish());
}
const char *category(uint32_t t) {
  switch (t) {
  case 4140:
    return "Player spawn";
  case 4040:
    return "Enemy spawner";
  case 5000:
    return "Enemy template";
  case 4150:
    return "Marker";
  case 3100:
    return "Weapon";
  case 3200:
  case 3210:
    return "Ammo";
  case 2100:
    return "Health";
  case 2200:
    return "Armour / item";
  case 4070:
    return "Door";
  case 4080:
    return "Trigger";
  case 4160:
    return "Level exit";
  case 4200:
    return "Prop";
  case 4030:
    return "Watcher";
  case 4010:
    return "Copier";
  case 4100:
    return "Sound";
  case 4120:
    return "Camera";
  case 4050:
    return "Door / key condition";
  case 4090:
    return "Music";
  case 4110:
    return "Damage action";
  case 4130:
    return "Message";
  case 4170:
    return "Touch field";
  case 4180:
    return "Button / switch";
  case 4190:
    return "Vehicle";
  case 4210:
    return "Model destruction";
  case 4220:
    return "Bounce pad";
  case 4230:
    return "Rolling stone";
  case 4240:
    return "Destructible structure";
  case 4260:
    return "Arrow marker";
  case 4270:
    return "Lockdown";
  case 4280:
    return "Warp players";
  case 4310:
    return "Particles / effects";
  case 4320:
    return "Multiplayer flag";
  case 4330:
    return "Par watcher";
  case 4340:
    return "Movie / FMV";
  default:
    return "Object";
  }
}
Project Project::create(const std::filesystem::path &root,
                        const std::string &name, const std::string &level) {
  need(slug(name) && slug(level),
       "Use 1-48 letters, digits, hyphens or underscores for the project name");
  auto folder = safe_under(root, name);
  need(!std::filesystem::exists(folder),
       "A project with this name already exists; open it instead");
  std::filesystem::create_directories(folder);
  return {folder, level, name};
}
void Project::save(Document &doc) const {
  need(slug(level), "Invalid level name");
  auto path =
      safe_under(folder, std::filesystem::path("game_data/files/Levels") /
                             (level + ".ssw"));
  auto output = doc.export_bytes();
  Document check;
  check.parse(output);
  need(check.payload() == doc.payload(), "Map export verification failed");
  auto manifest = safe_under(folder, "project.json");
  if (doc.structural()) {
    launcher::JSONValue::Array origins;
    for (auto &o : doc.objects_)
      if (o.added) {
        launcher::JSONValue::Array values;
        for (float v : o.initial)
          values.emplace_back(double(v));
        origins.emplace_back(launcher::JSONValue::Object{
            {"offset", int64_t(o.offset)}, {"matrix", std::move(values)}});
      }
    launcher::JSONValue::Array imports;
    for (auto &m : doc.imports_) {
      launcher::JSONValue::Array ranges;
      for (auto &r : m.segments)
        ranges.emplace_back(launcher::JSONValue::Array{
            int64_t(r.source), int64_t(r.end), int64_t(r.target)});
      imports.emplace_back(
          launcher::JSONValue::Object{{"hash", m.hash},
                                      {"size", int64_t(m.size)},
                                      {"segments", std::move(ranges)}});
    }
    gc::Sha256 hash;
    hash.update(output.data(), output.size());
    auto json = launcher::write_json(
        launcher::JSONValue::Object{
            {"schema", 2},
            {"format", "SSNE object editor"},
            {"level", level},
            {"title", title},
            {"source_sha256", doc.source_hash()},
            {"map_sha256", gc::Sha256::hex(hash.finish())},
            {"next_identifier", int64_t(doc.next_identifier_)},
            {"new_origins", std::move(origins)},
            {"imports", std::move(imports)}},
        true);
    std::filesystem::create_directories(path.parent_path());
    doc.write_copy(path);
    atomic_write(manifest, {json.begin(), json.end()});
    doc.mark_saved();
    return;
  }
  launcher::JSONValue::Array edits;
  for (size_t i = 0; i < doc.objects().size(); ++i)
    if (doc.matrix(i) != doc.original_matrix(i)) {
      launcher::JSONValue::Array transform;
      for (auto v : doc.matrix(i))
        transform.emplace_back(double(v));
      edits.emplace_back(launcher::JSONValue::Object{
          {"offset", int64_t(doc.objects()[i].offset)},
          {"matrix", std::move(transform)}});
    }
  auto json = launcher::write_json(
      launcher::JSONValue::Object{{"schema", 1},
                                  {"format", "SSNE object editor"},
                                  {"level", level},
                                  {"title", title},
                                  {"source_sha256", doc.source_hash()},
                                  {"edits", std::move(edits)}},
      true);
  std::filesystem::create_directories(path.parent_path());
  doc.write_copy(path);
  atomic_write(manifest, {json.begin(), json.end()});
  doc.mark_saved();
}
Project Project::open(const std::filesystem::path &folder,
                      const std::filesystem::path &data, Document &doc) {
  auto b = bytes(safe_under(folder, "project.json"));
  launcher::JSONValue j;
  std::string err;
  need(launcher::parse_json(std::string(b.begin(), b.end()), j, err),
       "Invalid editor project JSON");
  need((number(j, "schema") == 1 || number(j, "schema") == 2) &&
           text(j, "format") == "SSNE object editor",
       "Unsupported editor project version");
  Project p{folder, text(j, "level"), text(j, "title")};
  need(slug(p.level), "Invalid project level");
  Document next;
  next.load(data / "files/Levels" / (p.level + ".ssw"));
  need(next.source_hash() == text(j, "source_sha256"),
       "The original map changed; this project belongs to a different map "
       "version");
  if (number(j, "schema") == 2) {
    auto wire = bytes(
        safe_under(folder, std::filesystem::path("game_data/files/Levels") /
                               (p.level + ".ssw")));
    gc::Sha256 hash;
    hash.update(wire.data(), wire.size());
    need(gc::Sha256::hex(hash.finish()) == text(j, "map_sha256"),
         "Saved structural map checksum differs");
    next.adopt_snapshot(std::move(wire));
    auto origins = j.find("new_origins");
    need(origins && origins->array() && origins->array()->size() <= 10000,
         "Invalid new-object origins");
    std::set<int64_t> seen;
    for (auto &item : *origins->array()) {
      auto offset = number(item, "offset");
      need(seen.insert(offset).second, "Duplicate new-object origin");
      auto it =
          std::find_if(next.objects_.begin(), next.objects_.end(),
                       [&](auto &o) { return o.offset == offset && o.added; });
      need(it != next.objects_.end(), "New project object missing");
      auto v = item.find("matrix");
      need(v && v->array() && v->array()->size() == 12,
           "Invalid new-object matrix");
      for (unsigned k = 0; k < 12; ++k) {
        it->initial[k] = float(real(v->array()->at(k)));
        need(std::isfinite(it->initial[k]) && std::abs(it->initial[k]) <= 1e6f,
             "Invalid new-object origin");
      }
    }
    for (auto &o : next.objects_)
      if (o.added)
        need(seen.contains(o.offset), "Missing new-object origin");
    auto id = number(j, "next_identifier");
    need(id >= 0 && id <= 32767, "Invalid next entity ID");
    next.next_identifier_ = std::max(next.next_identifier_, uint32_t(id));
    if (auto imports = j.find("imports")) {
      need(imports->array() && imports->array()->size() <= 64,
           "Invalid imported-map cache");
      std::set<std::string> hashes;
      for (auto &item : *imports->array()) {
        Document::ImportedMap m;
        m.hash = text(item, "hash");
        auto size = number(item, "size");
        need(m.hash.size() == 64 &&
                 m.hash.find_first_not_of("0123456789abcdef") ==
                     std::string::npos &&
                 hashes.insert(m.hash).second && size >= 128 && size <= limit,
             "Invalid imported-map identity");
        m.size = uint32_t(size);
        auto ranges = item.find("segments");
        need(ranges && ranges->array() && ranges->array()->size() <= 100000,
             "Invalid import ranges");
        uint32_t last = 0;
        for (auto &range : *ranges->array()) {
          need(range.array() && range.array()->size() == 3,
               "Invalid import segment");
          auto a = real(range.array()->at(0)), b = real(range.array()->at(1)),
               c = real(range.array()->at(2));
          need(a == std::floor(a) && b == std::floor(b) && c == std::floor(c) &&
                   a >= last && b > a && b <= size && c >= next.base_.size() &&
                   c + b - a <= next.data_.size(),
               "Import segment outside map");
          m.segments.push_back({uint32_t(a), uint32_t(b), uint32_t(c)});
          last = uint32_t(b);
        }
        next.imports_.push_back(std::move(m));
      }
    }
    next.mark_saved();
    next.clear_history();
    doc = std::move(next);
    return p;
  }
  auto edits = j.find("edits");
  need(edits && edits->array() &&
           edits->array()->size() <= next.objects().size(),
       "Invalid project edit list");
  std::set<int64_t> seen;
  for (const auto &e : *edits->array()) {
    auto offset = number(e, "offset");
    need(seen.insert(offset).second, "Duplicate project object");
    auto found =
        std::find_if(next.objects().begin(), next.objects().end(),
                     [&](const Object &o) { return o.offset == offset; });
    need(found != next.objects().end(),
         "Project object is missing from original map");
    auto v = e.find("matrix");
    need(v && v->array() && v->array()->size() == 12,
         "Invalid project transform");
    Matrix m;
    for (unsigned k = 0; k < 12; ++k)
      m[k] = float(real(v->array()->at(k)));
    if(found->type!=4200&&found->type!=4260) {
      // Schema 1 predates per-entity transform capabilities. Older editors
      // allowed point-entity rotations which the native factory ignores.
      // Preserve those existing projects under the exact old rigidity checks;
      // new transform edits still pass through the stricter public API.
      const auto original=next.original_matrix(size_t(found-next.objects().begin()));
      for(auto value:m)need(std::isfinite(value)&&std::abs(value)<=1e6f,"Invalid legacy project transform");
      for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b){double old=0,now=0;for(unsigned r=0;r<3;++r){old+=double(original[r*4+a])*original[r*4+b];now+=double(m[r*4+a])*m[r*4+b];}need(std::abs(old-now)<=.001*std::max(1.,std::abs(old)),"Legacy point entity cannot introduce scale or shear");}
      next.put(found->transform,m);
    } else next.set_matrix(size_t(found - next.objects().begin()), m);
  }
  next.mark_saved();
  next.clear_history();
  doc = std::move(next);
  return p;
}
std::filesystem::path
Project::prepare_test(const std::filesystem::path &original,
                      const std::filesystem::path &game,
                      const std::filesystem::path &card) const {
  need(std::filesystem::is_regular_file(game),
       "Build the game before testing maps");
  const auto target = safe_under(folder, "game_data");
  const auto edited = std::filesystem::path("files/Levels") / (level + ".ssw");
  need(std::filesystem::is_regular_file(target / edited),
       "Save the project before testing");
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(original)) {
    need(!entry.is_symlink(), "Linked original game assets are unsupported");
    auto rel = std::filesystem::relative(entry.path(), original);
    auto to = safe_under(target, rel);
    if (entry.is_directory())
      std::filesystem::create_directories(to);
    else if (entry.is_regular_file() && rel != edited &&
             !std::filesystem::exists(to)) {
      std::filesystem::create_directories(to.parent_path());
      std::error_code ec;
      std::filesystem::create_hard_link(entry.path(), to, ec);
      if (ec)
        std::filesystem::copy_file(entry.path(), to);
    }
  }
  // The original virtual disc FST contains fixed file sizes and offsets.
  // A larger edited map needs its own non-overlapping virtual extent. Replace
  // only the private FST file atomically, never its hard-linked original.
  auto fst = bytes(original / "sys/fst.bin");
  need(fst.size() >= 12 && (word(fst, 0) >> 24) == 1,
       "Invalid original disc file table");
  const auto count = word(fst, 8);
  need(count > 0 && count <= 100000 && uint64_t(count) * 12 <= fst.size(),
       "Invalid disc file table count");
  const size_t strings = size_t(count) * 12;
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, ""}};
  size_t entry = 0;
  uint64_t end = 0;
  auto lowered = [](std::string v) {
    for (auto &c : v)
      if (c >= 'A' && c <= 'Z')
        c = char(c - 'A' + 'a');
    return v;
  };
  for (uint32_t i = 1; i < count; ++i) {
    while (dirs.size() > 1 && i >= dirs.back().first)
      dirs.pop_back();
    const auto at = size_t(i) * 12;
    const auto tag = word(fst, at);
    size_t name = strings + (tag & 0xffffff);
    need(name < fst.size(), "Invalid disc file name");
    std::string text;
    for (unsigned k = 0;; ++k) {
      need(k < 256 && name + k < fst.size(), "Invalid disc file name length");
      auto ch = fst[name + k];
      if (!ch)
        break;
      need(ch != '/' && ch != '\\', "Invalid disc path component");
      text += char(ch);
    }
    need(!text.empty() && text != "." && text != "..",
         "Invalid disc name component");
    auto path = dirs.back().second + text;
    if (tag >> 24) {
      const auto next = word(fst, at + 8);
      need(next > i && next <= count, "Invalid disc directory extent");
      dirs.emplace_back(next, path + "/");
    } else {
      end = std::max(end, uint64_t(word(fst, at + 4)) + word(fst, at + 8));
      if (lowered(path) == lowered("Levels/" + level + ".ssw")) {
        need(!entry, "Duplicate edited disc entry");
        entry = at;
      }
    }
  }
  need(entry != 0, "Edited level is missing from original disc file table");
  end = (end + 31) & ~uint64_t(31);
  const auto map_size = std::filesystem::file_size(target / edited);
  need(end + map_size < 0x100000000ull && map_size <= limit,
       "Edited virtual disc exceeds address limit");
  word(fst, entry + 4, uint32_t(end));
  word(fst, entry + 8, uint32_t(map_size));
  atomic_write(safe_under(target, "sys/fst.bin"), fst);
  if (std::filesystem::is_regular_file(original / "sys/boot.bin"))
    atomic_write(safe_under(target, "sys/boot.bin"),
                 bytes(original / "sys/boot.bin"));
  auto run = safe_under(folder, "test");
  std::filesystem::create_directories(run);
  auto exe = run / game.filename();
  if (!std::filesystem::exists(exe) ||
      gc::sha256_file(exe.string()) != gc::sha256_file(game.string()))
    std::filesystem::copy_file(
        game, exe, std::filesystem::copy_options::overwrite_existing);
  if (!std::filesystem::exists(run / "MemoryCardA.EUR.raw") &&
      std::filesystem::is_regular_file(card))
    std::filesystem::copy_file(card, run / "MemoryCardA.EUR.raw");
  return exe;
}
std::filesystem::path
Project::prepare_survival_test(const std::filesystem::path &original,
                               const std::filesystem::path &game,
                               const std::filesystem::path &card) const {
  auto exe = prepare_test(original, game, card);
  auto target = safe_under(folder, "game_data");
  auto resource = bytes(original / "files/Levels/Clevel5_4.ssw");
  const std::string name = "EditorSurvivalResources.ssw";
  atomic_write(safe_under(target, std::filesystem::path("files") / name),
               resource);
  auto fst = bytes(target / "sys/fst.bin");
  auto count = word(fst, 8);
  need(count && uint64_t(count) * 12 <= fst.size(),
       "Invalid Survival file table");
  uint64_t end = 0;
  for (uint32_t i = 1; i < count; ++i)
    if (!(word(fst, i * 12) >> 24))
      end = std::max(end,
                     uint64_t(word(fst, i * 12 + 4)) + word(fst, i * 12 + 8));
  end = (end + 31) & ~uint64_t(31);
  need(end + resource.size() + fst.size() + 64 < 0x100000000ull,
       "Survival disc exceeds address limit");
  auto string_offset = fst.size() - size_t(count) * 12;
  need(string_offset < 0x1000000, "Survival file name table too large");
  std::vector<uint8_t> table(fst.begin(), fst.begin() + size_t(count) * 12);
  table.resize(table.size() + 12);
  word(table, 8, count + 1);
  word(table, count * 12, uint32_t(string_offset));
  word(table, count * 12 + 4, uint32_t(end));
  word(table, count * 12 + 8, uint32_t(resource.size()));
  table.insert(table.end(), fst.begin() + size_t(count) * 12, fst.end());
  table.insert(table.end(), name.begin(), name.end());
  table.push_back(0);
  atomic_write(safe_under(target, "sys/fst.bin"), table);
  auto boot = bytes(original / "sys/boot.bin");
  need(boot.size() >= 0x440, "Invalid Survival disc header");
  auto fst_offset = (end + resource.size() + 31) & ~uint64_t(31);
  word(boot, 0x424, uint32_t(fst_offset));
  word(boot, 0x428, uint32_t(table.size()));
  word(boot, 0x42c, uint32_t(table.size()));
  atomic_write(safe_under(target, "sys/boot.bin"), boot);
  return exe;
}
} // namespace mapedit
