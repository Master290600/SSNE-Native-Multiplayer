#include "map_document.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>
#include <map>
#include <stdexcept>
namespace mapedit {
namespace {
void need(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
uint32_t u(const std::vector<uint8_t> &b, size_t p) {
  need(p <= b.size() && b.size() - p >= 4, "Structural map field out of range");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
void write_word(std::vector<uint8_t> &b, size_t p, uint32_t v) {
  need(p <= b.size() && b.size() - p >= 4, "Structural map write out of range");
  for (unsigned i = 0; i < 4; ++i)
    b[p + i] = uint8_t(v >> (24 - i * 8));
}
std::vector<uint32_t> chain(const std::vector<uint8_t> &b, uint32_t field) {
  std::vector<uint32_t> out;
  std::set<uint32_t> seen;
  for (uint32_t p = u(b, field); p; p = u(b, p)) {
    need(p % 4 == 0 && uint64_t(p) + 4 <= b.size() && seen.insert(p).second &&
             out.size() < 50000,
         "Invalid structural map list");
    out.push_back(p);
  }
  return out;
}
std::string name(const std::vector<uint8_t> &b, uint32_t p) {
  std::string s;
  for (unsigned i = 0; i < 256; ++i) {
    need(uint64_t(p) + i < b.size(), "Invalid object name");
    auto c = b[p + i];
    if (!c)
      return s;
    need(c >= 32 && i < 255, "Invalid object name encoding");
    s += char(c);
  }
  throw std::runtime_error("Object name too long");
}
Matrix read_matrix(const std::vector<uint8_t> &b, uint32_t p) {
  Matrix m;
  for (unsigned i = 0; i < 12; ++i) {
    m[i] = std::bit_cast<float>(u(b, uint64_t(p) + i * 4));
    need(std::isfinite(m[i]) && std::abs(m[i]) <= 1e6f,
         "Invalid structural object matrix");
  }
  return m;
}
Matrix delta_matrix(const Matrix &a, const Matrix &b) {
  float d = b[0] * (b[5] * b[10] - b[6] * b[9]) -
            b[1] * (b[4] * b[10] - b[6] * b[8]) +
            b[2] * (b[4] * b[9] - b[5] * b[8]);
  need(std::abs(d) > 1e-10f, "Singular model transform");
  Matrix inv{(b[5] * b[10] - b[6] * b[9]) / d, (b[2] * b[9] - b[1] * b[10]) / d,
             (b[1] * b[6] - b[2] * b[5]) / d,  0,
             (b[6] * b[8] - b[4] * b[10]) / d, (b[0] * b[10] - b[2] * b[8]) / d,
             (b[2] * b[4] - b[0] * b[6]) / d,  0,
             (b[4] * b[9] - b[5] * b[8]) / d,  (b[1] * b[8] - b[0] * b[9]) / d,
             (b[0] * b[5] - b[1] * b[4]) / d,  0};
  for (unsigned r = 0; r < 3; ++r)
    inv[r * 4 + 3] =
        -inv[r * 4] * b[3] - inv[r * 4 + 1] * b[7] - inv[r * 4 + 2] * b[11];
  Matrix out{};
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 4; ++c) {
      for (unsigned k = 0; k < 3; ++k)
        out[r * 4 + c] += a[r * 4 + k] * inv[k * 4 + c];
      if (c == 3)
        out[r * 4 + c] += a[r * 4 + 3];
    }
  return out;
}
} // namespace
void Document::rebuild_objects() {
  logic_lookup_valid_=false;
  std::map<uint32_t, Object> prior;
  for (auto &o : objects_)
    prior.emplace(o.offset, o);
  std::vector<Object> out;
  for (auto p : chain(data_, 0x40)) {
    need(uint64_t(p) + 44 <= data_.size(), "Short object record");
    auto transform = u(data_, p + 4), type = u(data_, p + 12);
    if (!transform)
      continue;
    need(type >= 1000 && type <= 10000 && transform >= 128 &&
             transform % 4 == 0,
         "Invalid editable object");
    Object o;
    o.offset = p;
    o.transform = transform;
    o.type = type;
    o.name = name(data_, u(data_, p + 8));
    o.added = p >= base_.size();
    o.initial = read_matrix(data_, transform);
    if (auto it = prior.find(p); it != prior.end() && it->second.added)
      o.initial = it->second.initial;
    if (!wave_helper(o.type, o.name))
      out.push_back(std::move(o));
  }
  objects_ = std::move(out);
}
void Document::pointer(uint32_t p, uint32_t target) {
  need(!target || target < data_.size(), "Pointer target outside map");
  write_word(data_, p, target);
  if (target)
    relocations_.insert(p);
  else
    relocations_.erase(p);
}
uint32_t Document::copy_block(uint32_t p, size_t n) {
  need(uint64_t(p) + n <= data_.size() &&
           data_.size() + n + 4 <= 128u * 1024 * 1024,
       "Clone exceeds map limits");
  std::vector<uint8_t> b(data_.begin() + p, data_.begin() + p + n);
  std::vector<uint32_t> fields;
  for (auto field : relocations_)
    if (field >= p && uint64_t(field) < uint64_t(p) + n) {
      need(uint64_t(field) + 4 <= uint64_t(p) + n, "Partial pointer in clone");
      fields.push_back(field - p);
    }
  while (data_.size() % 4)
    data_.push_back(0);
  auto at = uint32_t(data_.size());
  data_.insert(data_.end(), b.begin(), b.end());
  for (auto f : fields)
    relocations_.insert(at + f);
  return at;
}
uint32_t Document::append_string(const std::string &s) {
  need(!s.empty() && s.size() < 256, "Invalid new object name");
  while (data_.size() % 4)
    data_.push_back(0);
  auto p = uint32_t(data_.size());
  data_.insert(data_.end(), s.begin(), s.end());
  data_.push_back(0);
  return p;
}
uint32_t Document::allocate_identifier() {
  for (auto p : chain(data_, 0x40)) {
    auto id = u(data_, p + 20);
    if (id < 32767)
      next_identifier_ = std::max(next_identifier_, id + 1);
  }
  need(next_identifier_ < 32767, "No free signed 16-bit entity IDs");
  return next_identifier_++;
}
void Document::transaction(const std::function<void()> &action,
                           bool structural_change) {
  if (transaction_depth_) {
    action();
    rebuild_objects();
    return;
  }
  struct Depth {
    unsigned &depth;
    explicit Depth(unsigned &d) : depth(d) { ++depth; }
    ~Depth() { --depth; }
  } depth(transaction_depth_);
  auto before = data_;
  auto old_relocations = relocations_;
  auto old_objects = objects_;
  auto old_id = next_identifier_;
  auto old_imports = imports_;
  try {
    action();
    rebuild_objects();
  } catch (...) {
    logic_lookup_valid_=false;
    data_ = std::move(before);
    relocations_ = std::move(old_relocations);
    objects_ = std::move(old_objects);
    next_identifier_ = old_id;
    imports_ = std::move(old_imports);
    throw;
  }
  if (before == data_ && old_relocations == relocations_)
    return;
  Change c;
  c.before_imports = std::move(old_imports);
  c.after_imports = imports_;
  c.before_size = before.size();
  c.after_size = data_.size();
  need(c.after_size >= c.before_size, "Invalid structural transaction shrink");
  for (size_t at = 0; at < before.size();) {
    if (before[at] == data_[at]) {
      ++at;
      continue;
    }
    size_t start = at;
    while (at < before.size() && before[at] != data_[at])
      ++at;
    c.patches.push_back({uint32_t(start),
                         {before.begin() + start, before.begin() + at},
                         {data_.begin() + start, data_.begin() + at}});
  }
  c.tail.assign(data_.begin() + before.size(), data_.end());
  c.before_objects = std::move(old_objects);
  c.after_objects = objects_;
  std::set_difference(relocations_.begin(), relocations_.end(),
                      old_relocations.begin(), old_relocations.end(),
                      std::back_inserter(c.added_relocations));
  std::set_difference(old_relocations.begin(), old_relocations.end(),
                      relocations_.begin(), relocations_.end(),
                      std::back_inserter(c.removed_relocations));
  c.structural = structural_change || c.before_size != c.after_size ||
                 !c.added_relocations.empty() || !c.removed_relocations.empty();
  if (c.structural)
    ++structure_revision_;
  undo_.push_back(std::move(c));
  if (undo_.size() > 128)
    undo_.erase(undo_.begin());
  redo_.clear();
}
void Document::apply(const Change &c, bool forward) {
  logic_lookup_valid_=false;
  if (forward) {
    data_.resize(c.before_size);
    data_.insert(data_.end(), c.tail.begin(), c.tail.end());
  }
  for (auto &p : c.patches) {
    auto &v = forward ? p.after : p.before;
    need(uint64_t(p.offset) + v.size() <= data_.size(), "Invalid undo patch");
    std::copy(v.begin(), v.end(), data_.begin() + p.offset);
  }
  if (!forward)
    data_.resize(c.before_size);
  for (auto p : c.added_relocations) {
    if (forward)
      relocations_.insert(p);
    else
      relocations_.erase(p);
  }
  for (auto p : c.removed_relocations) {
    if (forward)
      relocations_.erase(p);
    else
      relocations_.insert(p);
  }
  objects_ = forward ? c.after_objects : c.before_objects;
  imports_ = forward ? c.after_imports : c.before_imports;
  if (c.structural)
    ++structure_revision_;
}
bool Document::undo() {
  if (undo_.empty())
    return false;
  auto c = std::move(undo_.back());
  undo_.pop_back();
  apply(c, false);
  redo_.push_back(std::move(c));
  return true;
}
bool Document::redo() {
  if (redo_.empty())
    return false;
  auto c = std::move(redo_.back());
  redo_.pop_back();
  apply(c, true);
  undo_.push_back(std::move(c));
  return true;
}
bool Document::structural() const {
  if (data_.size() != base_.size() || relocations_ != base_relocations_)
    return true;
  for (auto &o : objects_)
    if (o.added)
      return true;
  return chain(data_, 0x40) != chain(base_, 0x40);
}
bool Document::can_duplicate(size_t i) const {
  if (i >= objects_.size())
    return false;
  auto &o = objects_[i];
  if (is_editor_logic(i))
    return true;
  if (logic_template_type(o.type))
    return u(data_,o.offset+16)!=0;
  if (o.type == 5000 || o.type == 4040 || o.type == 4140 ||
      (o.type >= 2000 && o.type < 4000))
    return true;
  if (o.type == 4200) {
    auto c = u(data_, o.offset + 16);
    auto nodes = chain(data_, 0x58);
    return c && u(data_, c) < nodes.size() &&
           u(data_, nodes[u(data_, c)] + 4) == o.transform;
  }
  return false;
}
bool Document::can_erase(size_t i) const {
  if (i >= objects_.size())
    return false;
  auto wave = wave_trigger(i);
  if (!wave && !can_duplicate(i))
    return false;
  auto &o = objects_[i];
  if(logic_template_type(o.type) && !o.added)
    return false; // Original opaque action sequences may reference these.
  if(o.type==4200) {
    const auto id=u(data_,o.offset+20);
    for(const auto& button:objects_)
      if(button.type==4180 && (button.name.starts_with("Editor Button ")||button.name.starts_with("Editor Toggle ")) && u(data_,u(data_,button.offset+16)+12)==id)
        return false; // The controller owns its visible mesh as one component.
  }
  // Erasing a receiver must not silently leave an incoming authored link
  // dangling. An editor area's private relay is removed with the area, so
  // both identities are protected against external references.
  std::set<uint32_t> targets{u(data_, o.offset + 20)};
  uint32_t private_relay = 0xffffffff;
  if (wave) {
    private_relay = u(data_, u(data_, o.offset + 16) + 8);
    targets.insert(private_relay);
  }
  try {
    for (auto p : chain(data_, 0x40)) {
      if (!u(data_, p + 4) || p == o.offset ||
          (wave && u(data_, p + 20) == private_relay))
        continue;
      auto type = u(data_, p + 12), cfg = u(data_, p + 16);
      if (!cfg)
        continue;
      auto incoming = [&](unsigned field) {
        return targets.contains(u(data_, cfg + field));
      };
      if (type == 4080) {
        for (unsigned k = 0; k < 10; ++k)
          if (incoming(20 + k * 4))
            return false;
      } else if (type == 4030 || type == 5000 ||
                 (type >= 2000 && type < 4000)) {
        if (incoming(8))
          return false;
      } else if (type == 4050) {
        if (incoming(16) || incoming(20) || incoming(32))
          return false;
      } else if (type == 4180) {
        if (incoming(4))
          return false;
      } else if (type == 4170 || type == 4020) {
        if (incoming(12))
          return false;
      } else if (type == 4040) {
        if (incoming(20) || (u(data_, cfg + 36) > 0 &&
                             u(data_, cfg + 36) < 0x80000000u && incoming(36)))
          return false;
      } else if (type == 4070) {
        if (u(data_, cfg + 36) > 0 && u(data_, cfg + 36) < 0x80000000u &&
            incoming(36))
          return false;
        auto field = cfg + 0x3c;
        need(uint64_t(field) + 2 <= data_.size(), "Short brush damage target");
        auto target = uint16_t(uint16_t(data_[field]) << 8 | data_[field + 1]);
        if (target > 0 && target < 32768 && targets.contains(target))
          return false;
      }
    }
  } catch (...) {
    return false;
  }
  if (wave)
    return true;
  if (o.type == 4140)
    return std::count_if(objects_.begin(), objects_.end(),
                         [](auto &o) { return o.type == 4140; }) > 1;
  if (o.type == 5000) {
    auto id = u(data_, o.offset + 20);
    for (auto p : chain(data_, 0x40))
      if (u(data_, p + 12) == 4040) {
        auto c = u(data_, p + 16);
        if (c && u(data_, c + 20) == id)
          return false;
      }
  }
  return true;
}
void Document::move_prop_bounds(size_t index, const Matrix &before,
                                const Matrix &after) {
  if(objects_[index].type==4180&&transform_type(index)==4200){
    for(size_t i=0;i<objects_.size();++i)if(objects_[i].type==4200&&objects_[i].transform==objects_[index].transform){move_prop_bounds(i,before,after);return;}
  }
  if (objects_[index].type != 4200 || before == after)
    return;
  auto cfg = u(data_, objects_[index].offset + 16);
  auto nodes = chain(data_, 0x58);
  need(cfg && u(data_, cfg) < nodes.size(), "Invalid prop scene reference");
  auto node = nodes[u(data_, cfg)];
  need(u(data_, node + 4) == objects_[index].transform,
       "Prop transform binding differs");
  auto delta = delta_matrix(after, before);
  auto stretch=[](const Matrix& m){
    double a[3][3]{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)for(unsigned k=0;k<3;++k)a[i][j]+=double(m[k*4+i])*m[k*4+j];
    // Jacobi diagonalization of A^T A gives the sphere's maximum stretch.
    // Rotation alone keeps radius unchanged; repeat edits do not inflate it.
    for(unsigned n=0;n<24;++n){unsigned p=0,q=1;for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j)if(std::abs(a[i][j])>std::abs(a[p][q])){p=i;q=j;}if(std::abs(a[p][q])<1e-14)break;
      const double t=.5*std::atan2(2*a[p][q],a[q][q]-a[p][p]),c=std::cos(t),s=std::sin(t),app=a[p][p],aqq=a[q][q],apq=a[p][q];
      for(unsigned k=0;k<3;++k)if(k!=p&&k!=q){const auto x=a[k][p],y=a[k][q];a[k][p]=a[p][k]=c*x-s*y;a[k][q]=a[q][k]=s*x+c*y;}
      a[p][p]=c*c*app-2*c*s*apq+s*s*aqq;a[q][q]=s*s*app+2*c*s*apq+c*c*aqq;a[p][q]=a[q][p]=0;
    }
    return std::sqrt(std::max({a[0][0],a[1][1],a[2][2],1e-20}));
  };
  const auto reference=original_matrix(index);
  const auto radius_factor=stretch(delta_matrix(after,reference))/stretch(delta_matrix(before,reference));
  auto bound = [&](uint32_t p) {
    if (!p)
      return 0u;
    auto at = copy_block(p, 20);
    float x = std::bit_cast<float>(u(data_, at + 4)),
          y = std::bit_cast<float>(u(data_, at + 8)),
          z = std::bit_cast<float>(u(data_, at + 12)),
          r = float(std::bit_cast<float>(u(data_, at + 16))*radius_factor);
    float v[3];
    for (unsigned j = 0; j < 3; ++j) {
      v[j] = delta[j * 4] * x + delta[j * 4 + 1] * y + delta[j * 4 + 2] * z +
             delta[j * 4 + 3];
      write_word(data_, at + 4 + j * 4, std::bit_cast<uint32_t>(v[j]));
    }
    write_word(data_,at+16,std::bit_cast<uint32_t>(r));
    write_word(data_, at,
               std::bit_cast<uint32_t>(
                   std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + r));
    return at;
  };
  pointer(node + 24, bound(u(data_, node + 24)));
  auto frames = chain(data_, node + 44);
  uint32_t previous = 0;
  for (auto old : frames) {
    auto at = copy_block(old, 20);
    pointer(at, 0);
    pointer(at + 16, bound(u(data_, old + 16)));
    if (previous)
      pointer(previous, at);
    else
      pointer(node + 44, at);
    previous = at;
  }
}
uint32_t Document::clone_entity(uint32_t source, std::array<float, 3> position,
                                bool active, uint32_t external_visual,
                                uint32_t external_destruction) {
  auto type = u(data_, source + 12), config = u(data_, source + 16);
  if (type == 4040 && active) {
    auto id = u(data_, config + 20);
    bool found = false;
    for (auto p : chain(data_, 0x40))
      if (u(data_, p + 20) == id) {
        source = p;
        type = u(data_, p + 12);
        config = u(data_, p + 16);
        found = true;
        break;
      }
    need(found && (type == 5000 || (type >= 2000 && type < 4000)),
         "Spawner has no usable original enemy/item template");
  }
  need(type == 5000 || type == 4040 || type == 4140 || type == 4200 ||
           type == 4210 || logic_template_type(type) || (type >= 2000 && type < 4000),
       "Unsupported creation template");
  auto id = allocate_identifier(), old_matrix = u(data_, source + 4);
  auto m = old_matrix ? read_matrix(data_, old_matrix)
                      : Matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  for (unsigned i = 0; i < 3; ++i) {
    need(std::isfinite(position[i]) && std::abs(position[i]) <= 1e6f,
         "Invalid placement coordinate");
    m[i * 4 + 3] = position[i];
  }
  auto node = copy_block(source, 44);
  pointer(node, 0);
  auto transform = old_matrix ? copy_block(old_matrix, 48)
                              : uint32_t((data_.size() + 3) & ~size_t(3));
  if (!old_matrix)
    data_.resize(size_t(transform) + 48);
  put(transform, m);
  pointer(node + 4, transform);
  auto label = name(data_, u(data_, source + 8));
  if (label.size() > 215)
    label.resize(215);
  label += "_copy_" + std::to_string(id);
  pointer(node + 8, append_string(label));
  write_word(data_, node + 20, id);
  write_word(data_, node + 24, u(data_, node + 24) & ~0x70u);
  for (unsigned k = 28; k < 44; k += 4) {
    write_word(data_, node + k, 0);
    relocations_.erase(node + k);
  }
  size_t minimum = type == 4090 ? 24
                   : type == 4100 ? 52
                   : type == 4120 ? 48
                   : type == 4130 ? 32
                   : type == 4310 ? 56
                   : type == 4340 ? 12
                   : type == 5000   ? 24
                   : type == 4040 ? 60
                   : type == 4200 ? 16
                   : type == 4210 ? 32
                   : type == 4140 ? 4
                                  : 56;
  size_t extent = minimum;
  if (config) {
    std::set<uint32_t> targets;
    for (auto p : relocations_)
      targets.insert(u(data_, p));
    auto next = targets.upper_bound(config);
    if (next != targets.end() && *next - config >= minimum &&
        *next - config <= 256)
      extent = *next - config;
    auto clone = copy_block(config, extent);
    pointer(node + 16, clone);
    config = clone;
  } else
    need(type == 4140, "Template has no configuration");
  if(type==4100) {
    // A copied sound must not signal an unrelated original mission receiver.
    write_word(data_,config+8,0xffffffff);
    // +1C is the actual streamed voice/clip asset (80111540), retained.
    // +20 is an optional global named game action, detached from a copy.
    pointer(config+0x20,0);
  } else if(type==4120) {
    const auto before=read_matrix(data_,old_matrix);
    copy_camera_path(node,{position[0]-before[3],position[1]-before[7],position[2]-before[11]},true);
  } else if (type == 5000) {
    if (active)
      write_word(data_, config + 4, 0);
    write_word(data_, config + 8, id);
  } else if (type >= 2000 && type < 4000) {
    write_word(data_, config + 8, 0xffffffff);
    write_word(data_, config + 12, 0);
    write_word(data_, config + 48, 0xffffffff);
    auto entry = u(data_, config + 44);
    if (entry && relocations_.contains(config + 44)) {
      auto copy = copy_block(entry, 8);
      for (auto field : {0u, 4u})
        if (relocations_.contains(entry + field) && u(data_, entry + field))
          pointer(copy + field,
                  append_string(name(data_, u(data_, entry + field))));
      pointer(config + 44, copy);
    }
  } else if (type == 4210) {
    write_word(data_, config + 20, u(data_, config + 20) | 0xffff0000u);
    pointer(node + 4, 0);
  } else if (type == 4200) {
    auto nodes = chain(data_, 0x58);
    auto old_cfg = u(data_, source + 16), idx = u(data_, old_cfg);
    auto old_visual = external_visual ? external_visual
                                      : (idx < nodes.size() ? nodes[idx] : 0);
    need(old_visual && u(data_, old_visual + 4) == old_matrix,
         "Prop is not an independent model instance");
    need(std::count_if(objects_.begin(), objects_.end(),
                       [](auto &o) { return o.type == 4200; }) < 195,
         "Native model-holder registry is full");
    auto visual = copy_block(old_visual, 48);
    pointer(visual, 0);
    pointer(visual + 4, transform);
    pointer(visual + 12, u(data_, node + 8));
    if (nodes.empty())
      pointer(0x58, visual);
    else
      pointer(nodes.back(), visual);
    write_word(data_, config, uint32_t(nodes.size()));
    write_word(data_, config + 4, 1);
    write_word(data_, config + 12, u(data_, config + 12) | 0xffff0000u);
    auto destruction = u(data_, old_cfg + 8);
    write_word(data_, config + 8, 0xffffffff);
    uint32_t descriptor = external_destruction;
    if (!external_visual)
      for (auto p : chain(data_, 0x40))
        if (u(data_, p + 20) == destruction && u(data_, p + 12) == 4210) {
          descriptor = p;
          break;
        }
    if (descriptor) {
      auto d = clone_entity(descriptor, position, false);
      write_word(data_, config + 8, u(data_, d + 20));
    }
    auto clone_colors = [&](uint32_t p) {
      if (!p)
        return 0u;
      auto descriptor = copy_block(p, 16);
      auto n = u(data_, p + 4);
      need(n < 4 * 1024 * 1024, "Prop color array too large");
      if (n)
        pointer(descriptor, copy_block(u(data_, p), n));
      return descriptor;
    };
    pointer(visual + 20, clone_colors(u(data_, visual + 20)));
    auto old_frames = chain(data_, visual + 44);
    uint32_t prev = 0;
    for (auto f : old_frames) {
      auto copy = copy_block(f, 20);
      pointer(copy, 0);
      pointer(copy + 12, clone_colors(u(data_, f + 12)));
      if (prev)
        pointer(prev, copy);
      else
        pointer(visual + 44, copy);
      prev = copy;
    }
    // Bound copies are adjusted after the new editable record has been linked.
  }
  auto list = chain(data_, 0x40);
  if (list.empty())
    pointer(0x40, node);
  else
    pointer(list.back(), node);
  if (type == 4210)
    return node;
  rebuild_objects();
  auto found = std::find_if(objects_.begin(), objects_.end(),
                            [&](auto &o) { return o.offset == node; });
  need(found != objects_.end(), "New object not linked");
  found->initial = m;
  if (type == 4200)
    move_prop_bounds(size_t(found - objects_.begin()),
                     read_matrix(data_, old_matrix), m);
  return node;
}
size_t Document::duplicate(size_t index, std::array<float, 3> position,
                           bool active) {
  if (is_editor_logic(index))
    return duplicate_editor_logic(index, position);
  if(index<objects_.size() && objects_[index].type==4200) {
    const auto id=u(data_,objects_[index].offset+20);
    for(size_t k=0;k<objects_.size();++k)
      if(objects_[k].type==4180 && is_editor_logic(k) && u(data_,u(data_,objects_[k].offset+16)+12)==id)
        return duplicate_editor_logic(k,position);
  }
  need(can_duplicate(index),
       "This object type cannot be safely duplicated yet");
  need(objects_.size() < 10000, "Editable object limit reached");
  uint32_t added = 0;
  transaction(
      [&] { added = clone_entity(objects_[index].offset, position, active); },
      true);
  auto found = std::find_if(objects_.begin(), objects_.end(),
                            [&](auto &o) { return o.offset == added; });
  return size_t(found - objects_.begin());
}
void Document::erase(size_t index) {
  need(can_erase(index),
       "This is the last player start, a referenced enemy "
       "template/logic receiver, or an unsupported script/control object");
  if (is_editor_logic(index)) {
    erase_editor_logic(index);
    return;
  }
  if (wave_trigger(index)) {
    erase_wave(index);
    return;
  }
  auto target = objects_[index];
  transaction(
      [&] {
        if (target.type == 4200) {
          auto cfg = u(data_, target.offset + 16);
          auto nodes = chain(data_, 0x58);
          auto node = nodes.at(u(data_, cfg));
          pointer(node + 12, append_string("Editor removed model"));
          pointer(node + 16, 0);
          pointer(node + 20, 0);
          pointer(node + 44, 0);
          write_word(data_, node + 36, 0);
        }
        auto unlink = [&](uint32_t target_offset) {
          uint32_t previous = 0;
          for (auto p : chain(data_, 0x40)) {
            if (p == target_offset) {
              pointer(previous ? previous : 0x40, u(data_, p));
              return;
            }
            previous = p;
          }
        };
        if (target.type == 4200) {
          auto cfg = u(data_, target.offset + 16), id = u(data_, cfg + 8);
          uint32_t sidecar = 0;
          bool shared = false;
          for (auto p : chain(data_, 0x40)) {
            if (u(data_, p + 20) == id && u(data_, p + 12) == 4210 &&
                p >= base_.size())
              sidecar = p;
            if (p != target.offset && u(data_, p + 12) == 4200) {
              auto c = u(data_, p + 16);
              if (c && u(data_, c + 8) == id)
                shared = true;
            }
          }
          if (sidecar && !shared)
            unlink(sidecar);
        }
        uint32_t previous = 0;
        for (auto p : chain(data_, 0x40)) {
          if (p == target.offset) {
            pointer(previous ? previous : 0x40, u(data_, p));
            break;
          }
          previous = p;
        }
      },
      true);
}
void Document::adopt_snapshot(std::vector<uint8_t> file) {
  Document edited;
  edited.parse(std::move(file));
  data_ = std::move(edited.data_);
  relocations_ = std::move(edited.relocations_);
  rebuild_objects();
  next_identifier_ = std::max(next_identifier_, edited.next_identifier_);
  ++structure_revision_;
  undo_.clear();
  redo_.clear();
}
} // namespace mapedit
