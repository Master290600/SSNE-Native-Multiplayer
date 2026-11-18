#include "map_document.h"
#include "gc/editor_button.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>

namespace mapedit {
namespace {
void need(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}
uint32_t u(const std::vector<uint8_t> &b, uint64_t p) {
  need(p + 4 <= b.size(), "Logic field outside map");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
void write_word(std::vector<uint8_t> &b, uint32_t p, uint32_t v) {
  need(uint64_t(p) + 4 <= b.size(), "Logic write outside map");
  for (unsigned k = 0; k < 4; ++k)
    b[p + k] = uint8_t(v >> (24 - 8 * k));
}
void scalar(std::vector<uint8_t> &b, uint32_t p, float f) {
  write_word(b, p, std::bit_cast<uint32_t>(f));
}
std::string name(const std::vector<uint8_t> &b, uint32_t p) {
  std::string s;
  while (p < b.size() && s.size() < 256) {
    auto c = b[p++];
    if (!c)
      return s;
    s += char(c);
  }
  throw std::runtime_error("Invalid logic name");
}
std::vector<uint32_t> list(const std::vector<uint8_t> &b, uint32_t field) {
  std::vector<uint32_t> r;
  std::set<uint32_t> seen;
  for (auto p = u(b, field); p; p = u(b, p)) {
    need(seen.insert(p).second && seen.size() < 50000, "Invalid logic list");
    r.push_back(p);
  }
  return r;
}
std::map<uint32_t, uint32_t> actors(const std::vector<uint8_t> &b) {
  std::map<uint32_t, uint32_t> out;
  for (auto p : list(b, 0x40))
    if (u(b, p + 4))
      need(out.emplace(u(b, p + 20), p).second, "Duplicate logic identifier");
  return out;
}
uint32_t actor(const std::map<uint32_t, uint32_t> &ids, uint32_t id) {
  auto it = ids.find(id);
  need(it != ids.end(), "Logic component is missing");
  return it->second;
}
bool immediate_cycle(const std::vector<uint8_t> &b,
                     const std::map<uint32_t, uint32_t> &ids, uint32_t first) {
  // Native switches dispatch their relay synchronously. A relay's positive
  // delay defers execution to a later update and therefore breaks recursion.
  // Use an explicit stack so even a long authored chain is safe to inspect.
  std::map<uint32_t, unsigned> state;
  std::vector<std::pair<uint32_t, bool>> pending{{first, false}};
  while (!pending.empty()) {
    const auto [id, leaving] = pending.back();
    pending.pop_back();
    if (leaving) {
      state[id] = 2;
      continue;
    }
    if (state[id] == 1)
      return true;
    if (state[id] == 2 || !ids.contains(id))
      continue;
    const auto p = ids.at(id), type = u(b, p + 12), c = u(b, p + 16);
    if ((type != 4080 && type != 4180) ||
        (type == 4080 && std::bit_cast<float>(u(b, c + 0x78)) > 0)) {
      state[id] = 2;
      continue;
    }
    state[id] = 1;
    pending.emplace_back(id, true);
    if (type == 4180) {
      pending.emplace_back(u(b, c + 4), false);
    } else {
      for (unsigned k = 0; k < 10; ++k)
        if (u(b, c + 20 + k * 4) != 0xffffffff &&
            u(b, c + 0x3c + k * 4) == 0)
          pending.emplace_back(u(b, c + 20 + k * 4), false);
    }
  }
  return false;
}
std::string prefix(LogicKind k) {
  return std::string("Editor ") + logic_kind_name(k) + " ";
}
std::optional<gc::editor::Effect> effect_at(const std::vector<uint8_t> &b,
                                            uint32_t c) {
  if (!c || uint64_t(c) + 12 > b.size())
    return {};
  const auto p = u(b, c + 8);
  if (!p || uint64_t(p) + gc::editor::effect_prefix.size() > b.size() ||
      !std::equal(gc::editor::effect_prefix.begin(),
                  gc::editor::effect_prefix.end(), b.begin() + p))
    return {};
  gc::editor::Effect effect;
  std::string error;
  std::string encoded;
  for (size_t n = 0;
       n <= gc::editor::effect_max_bytes && uint64_t(p) + n < b.size(); ++n) {
    const auto c = b[p + n];
    if (!c) {
      if (gc::editor::decode_effect(encoded, effect, error))
        return effect;
      return {};
    }
    encoded += char(c);
  }
  return {};
}
bool authored(const std::string &s) {
  for (unsigned k = 0; k <= unsigned(LogicKind::Explosion); ++k)
    if (s.starts_with(prefix(LogicKind(k))))
      return true;
  return false;
}
LogicKind kind_of(const std::string &s, uint32_t type,
                  const std::vector<uint8_t> &b, uint32_t c) {
  if (type == 4080)
    if (auto effect = effect_at(b, c))
      return effect->kind == gc::editor::EffectKind::Light
                 ? LogicKind::Light
                 : LogicKind::Explosion;
  for (unsigned k = 0; k <= unsigned(LogicKind::Explosion); ++k)
    if (s.starts_with(prefix(LogicKind(k))))
      return LogicKind(k);
  if (type == 4030)
    return LogicKind::Area;
  if (type == 4050)
    return LogicKind::KeyBranch;
  if (type == 4180)
    return LogicKind::Button;
  return u(b, c + 0x64) ? LogicKind::Counter : LogicKind::Relay;
}
void validate(const LogicSpec &s) {
  need(unsigned(s.kind) <= unsigned(LogicKind::Explosion),
       "Unknown logic kind");
  need(!s.name.empty() && s.name.size() <= 90 &&
           std::all_of(s.name.begin(), s.name.end(),
                       [](unsigned char c) { return c >= 32 && c != 127; }),
       "Logic name needs 1..90 printable bytes");
  need(std::isfinite(s.delay) && s.delay >= 0 && s.delay <= 3600 &&
           std::isfinite(s.radius) && s.radius >= .25f && s.radius <= 10000 &&
           std::isfinite(s.poll_interval) && s.poll_interval >= .02f &&
           s.poll_interval <= 10,
       "Logic timing/radius is outside its supported range");
  need(s.threshold >= 1 && s.threshold <= 4096 && s.activation_limit <= 4096 &&
           s.key >= 0 && s.key < 16,
       "Logic condition/count is outside its supported range");
  if (s.kind == LogicKind::Timer && s.repeat)
    need(s.delay >= .05f,
         "Repeating timers need an interval of at least 0.05 s");
  if (s.kind == LogicKind::Light || s.kind == LogicKind::Explosion) {
    auto effect = s.effect;
    effect.kind = s.kind == LogicKind::Light
                      ? gc::editor::EffectKind::Light
                      : gc::editor::EffectKind::Explosion;
    std::string error;
    need(gc::editor::valid_effect(effect, error), error.c_str());
  }
}
} // namespace
const char *logic_kind_name(LogicKind k) {
  switch (k) {
  case LogicKind::Area:
    return "Area";
  case LogicKind::Relay:
    return "Relay";
  case LogicKind::Counter:
    return "Counter";
  case LogicKind::Timer:
    return "Timer";
  case LogicKind::Gate:
    return "Gate";
  case LogicKind::KeyBranch:
    return "Key branch";
  case LogicKind::Button:
    return "Button";
  case LogicKind::Toggle:
    return "Toggle";
  case LogicKind::Light:
    return "Light";
  case LogicKind::Explosion:
    return "Explosion";
  }
  return "Logic";
}
bool Document::is_editor_logic(size_t index) const {
  if (index >= objects_.size())
    return false;
  const auto type = objects_[index].type;
  return (type == 4030 || type == 4050 || type == 4080 || type == 4180) &&
         (authored(objects_[index].name) ||
          (type == 4080 &&
           effect_at(data_, u(data_, objects_[index].offset + 16))
               .has_value()));
}
int Document::logic_owner_index(size_t index) const {
  if (index >= objects_.size() || objects_[index].type != 4200)
    return -1;
  const auto &visual = objects_[index];
  const auto visual_id = u(data_, visual.offset + 20);
  for (size_t i = 0; i < objects_.size(); ++i) {
    const auto &candidate = objects_[i];
    if (candidate.type != 4180 || candidate.transform != visual.transform)
      continue;
    const auto config = u(data_, candidate.offset + 16);
    if (config && uint64_t(config) + 16 <= data_.size() &&
        u(data_, config + 12) == visual_id) {
      if (!logic_lookup_valid_) {
        logic_lookup_ = actors(data_);
        logic_lookup_valid_ = true;
      }
      auto router = logic_lookup_.find(u(data_, config + 4));
      if (router != logic_lookup_.end() &&
          u(data_, router->second + 12) == 4080 &&
          name(data_, u(data_, router->second + 8))
              .starts_with("__SSNE_LOGIC_HELPER_ROUTER_"))
        return int(i);
    }
  }
  return -1;
}
bool Document::is_logic_visual(size_t index) const {
  return logic_owner_index(index) >= 0;
}
void Document::copy_camera_path(uint32_t entity, std::array<float, 3> delta,
                                bool detach_events) {
  auto config = own_logic_config(entity, 48), first = u(data_, config + 8);
  if (!first)
    return;
  std::vector<uint32_t> keys;
  std::set<uint32_t> seen;
  auto p = first;
  do {
    need(p && uint64_t(p) + 60 <= data_.size() && seen.insert(p).second &&
             seen.size() <= 4096,
         "Invalid native camera path");
    need(u(data_, p + 20) && uint64_t(u(data_, p + 20)) + 48 <= data_.size(),
         "Camera path transform is missing");
    keys.push_back(p);
    p = u(data_, p + 24);
  } while (p != first);
  std::map<uint32_t, uint32_t> copies;
  for (auto key : keys) {
    auto copy = copy_block(key, 60);
    copies.emplace(key, copy);
    auto matrix = copy_block(u(data_, key + 20), 48);
    Matrix m;
    for (unsigned k = 0; k < 12; ++k)
      m[k] = std::bit_cast<float>(u(data_, matrix + k * 4));
    for (unsigned r = 0; r < 3; ++r) {
      m[r * 4 + 3] += delta[r];
      need(std::isfinite(m[r * 4 + 3]) && std::abs(m[r * 4 + 3]) <= 1e6f,
           "Camera path moved outside supported bounds");
    }
    this->put(matrix, m);
    pointer(copy + 20, matrix);
    if (detach_events)
      write_word(data_, copy + 12, 0xffffffff);
  }
  for (auto key : keys)
    pointer(copies.at(key) + 24, copies.at(u(data_, key + 24)));
  pointer(config + 8, copies.at(first));
}
void Document::move_logic_path(size_t index, const Matrix &before,
                               const Matrix &after) {
  if (objects_[index].type != 4120)
    return;
  copy_camera_path(
      objects_[index].offset,
      {after[3] - before[3], after[7] - before[7], after[11] - before[11]},
      false);
}

std::optional<LogicSpec> Document::logic_settings(uint32_t id) const {
  const auto ids = actors(data_);
  auto it = ids.find(id);
  if (it == ids.end())
    return {};
  auto p = it->second, t = u(data_, p + 12), c = u(data_, p + 16);
  if (t != 4030 && t != 4050 && t != 4080 && t != 4180)
    return {};
  LogicSpec s;
  const auto label = name(data_, u(data_, p + 8));
  s.kind = kind_of(label, t, data_, c);
  s.name = label.starts_with(prefix(s.kind))
               ? label.substr(prefix(s.kind).size())
               : label;
  if (s.name.empty())
    s.name = "Logic " + std::to_string(id);
  s.enabled = u(data_, c) != 0;
  if (t == 4030) {
    s.radius = std::bit_cast<float>(u(data_, c + 4));
    s.poll_interval = std::bit_cast<float>(u(data_, c + 12));
    if (authored(label)) {
      auto r = actor(ids, u(data_, c + 8)), rc = u(data_, r + 16);
      s.delay = std::bit_cast<float>(u(data_, rc + 0x78));
      s.activation_limit = u(data_, rc + 0x74);
    }
  }
  if (t == 4050) {
    s.radius = std::bit_cast<float>(u(data_, c + 8));
    s.key = std::clamp(int(u(data_, c + 36)), 0, 15);
  }
  if (t == 4180) {
    s.reusable = u(data_, c + 8) != 0;
    if (authored(label)) {
      auto r = actor(ids, u(data_, c + 4));
      c = u(data_, r + 16);
      s.delay = std::bit_cast<float>(u(data_, c + 0x78));
      s.activation_limit = u(data_, c + 0x74);
    }
  }
  if (t == 4080) {
    if (s.kind == LogicKind::Light || s.kind == LogicKind::Explosion) {
      const auto encoded = u(data_, c + 8);
      if (encoded) {
        std::string error;
        need(gc::editor::decode_effect(name(data_, encoded), s.effect, error),
             error.c_str());
      } else
        s.effect.kind = s.kind == LogicKind::Light
                            ? gc::editor::EffectKind::Light
                            : gc::editor::EffectKind::Explosion;
    }
    s.start_on_load = u(data_, c + 4) != 0;
    s.threshold = std::clamp(u(data_, c + 0x68), 1u, 4096u);
    s.repeat = u(data_, c + 0x6c) != 0;
    s.activation_limit = u(data_, c + 0x74);
    s.delay = std::bit_cast<float>(u(data_, c + 0x78));
    if (s.kind == LogicKind::Timer) {
      auto target = u(data_, c + 0x38);
      s.repeat = ids.contains(target) &&
                 name(data_, u(data_, ids.at(target) + 8))
                     .starts_with("__SSNE_LOGIC_HELPER_CLOCK_");
    }
  }
  return s;
}
void Document::write_logic(uint32_t p, const LogicSpec &s) {
  validate(s);
  auto t = u(data_, p + 12), id = u(data_, p + 20);
  const auto old = name(data_, u(data_, p + 8));
  const bool ours =
      authored(old) ||
      (t == 4080 && effect_at(data_, u(data_, p + 16)).has_value());
  auto old_settings = logic_settings(id);
  need(old_settings && old_settings->kind == s.kind,
       "Changing a node's native class requires creating a new node");
  pointer(p + 8,
          append_string((ours ? prefix(s.kind) : std::string{}) + s.name));
  auto c = own_logic_config(p, t == 4080   ? 128
                               : t == 4050 ? 40
                               : t == 4180 ? (ours ? gc::editor::button_config_bytes
                                                  : 12)
                                           : 16);
  if (t == 4180 && ours) {
    write_word(data_, c + gc::editor::button_magic_offset,
               gc::editor::button_magic);
    write_word(data_, c + gc::editor::button_version_offset,
               gc::editor::button_version);
  }
  write_word(data_, c, s.enabled);
  if (t == 4030) {
    scalar(data_, c + 4, s.radius);
    scalar(data_, c + 12, s.poll_interval);
    if (ours) {
      auto ids = actors(data_);
      auto r = actor(ids, u(data_, c + 8));
      auto rc = own_logic_config(r, 128);
      scalar(data_, rc + 0x78, s.delay);
      write_word(data_, rc + 0x74, s.activation_limit);
    }
  } else if (t == 4050) {
    // Original controllers can have different width/height and may watch
    // actors without consuming a key. An unrelated settings edit must keep
    // those native dimensions/modes and raw sentinel keys intact.
    if (ours || s.radius != old_settings->radius)
      scalar(data_, c + 4, s.radius);
    scalar(data_, c + 8, s.radius);
    if (ours) {
      write_word(data_, c + 12, 1);
      write_word(data_, c + 24, 1);
    }
    if (ours || s.key != old_settings->key)
      write_word(data_, c + 36, s.key);
  } else if (t == 4180) {
    write_word(data_, c + 8, s.kind == LogicKind::Toggle || s.reusable);
    if (ours) {
      auto ids = actors(data_);
      auto r = actor(ids, u(data_, c + 4));
      auto rc = own_logic_config(r, 128);
      scalar(data_, rc + 0x78, s.delay);
      write_word(data_, rc + 0x74, s.activation_limit);
    }
  } else {
    write_word(data_, c + 4, s.start_on_load);
    write_word(data_, c + 0x64, s.kind == LogicKind::Counter);
    write_word(data_, c + 0x68, s.threshold);
    write_word(data_, c + 0x6c, s.kind == LogicKind::Counter && s.repeat);
    write_word(data_, c + 0x74, s.activation_limit);
    scalar(data_, c + 0x78, s.delay);
    if (s.kind == LogicKind::Light || s.kind == LogicKind::Explosion) {
      auto effect = s.effect;
      effect.kind = s.kind == LogicKind::Light
                        ? gc::editor::EffectKind::Light
                        : gc::editor::EffectKind::Explosion;
      pointer(c + 8, append_string(gc::editor::encode_effect(effect)));
      if (s.kind == LogicKind::Light) {
        write_word(data_, c + 4, 0);
        write_word(data_, c + 0x74, 0);
        scalar(data_, c + 0x78, 0);
      }
    }
    if (ours && s.kind == LogicKind::Timer) {
      auto ids = actors(data_);
      auto target = u(data_, c + 0x38);
      uint32_t clock = 0;
      if (ids.contains(target) &&
          name(data_, u(data_, ids.at(target) + 8))
              .starts_with("__SSNE_LOGIC_HELPER_CLOCK_"))
        clock = ids.at(target);
      if (s.repeat) {
        if (!clock) {
          auto cfg = append_zero(128);
          write_word(data_, cfg, 1);
          for (unsigned k = 0; k < 10; ++k)
            write_word(data_, cfg + 20 + 4 * k, 0xffffffff);
          write_word(data_, cfg + 20, id);
          scalar(data_, cfg + 0x78, .01f);
          clock = append_entity(4080, "__SSNE_LOGIC_HELPER_CLOCK_",
                                u(data_, p + 4), cfg);
        }
        write_word(data_, c + 0x38, u(data_, clock + 20));
        write_word(data_, c + 0x60, 0);
      } else if (clock) {
        write_word(data_, c + 0x38, 0xffffffff);
        unlink_entity(clock);
      }
    }
  }
  // Authored waves must remain repeatable when their source is reusable.
  // Keep an existing spawner's total in step with the source's edited cap.
  // Native total zero means no further spawning, rather than unlimited.
  if (ours && s.kind != LogicKind::Light && s.kind != LogicKind::KeyBranch) {
    const auto ids = actors(data_);
    std::vector<uint32_t> controllers;
    if (t == 4030)
      controllers.push_back(actor(ids, u(data_, c + 8)));
    else if (t == 4180) {
      if (s.kind == LogicKind::Toggle) {
        controllers.push_back(actor(ids, u(data_, c + 16)));
        controllers.push_back(actor(ids, u(data_, c + 20)));
      } else
        controllers.push_back(actor(ids, u(data_, c + 4)));
    } else
      controllers.push_back(p);
    for (const auto controller : controllers) {
      const auto relay = u(data_, controller + 16);
      for (unsigned k = 0; k < 10; ++k) {
        const auto target = u(data_, relay + 20 + k * 4);
        if (!ids.contains(target))
          continue;
        const auto spawn = ids.at(target);
        if (u(data_, spawn + 12) != 4040 ||
            !wave_helper(4040, name(data_, u(data_, spawn + 8))))
          continue;
        const auto config = own_logic_config(spawn, 60);
        const auto count = u(data_, config + 8);
        need(count >= 1 && count <= 64,
             "Authored wave quota is outside its supported range");
        write_word(data_, config + 12,
                   s.activation_limit ? count * s.activation_limit
                                      : (0x7fffffffu / count) * count);
      }
    }
  }
}
void Document::set_logic_settings(uint32_t id, const LogicSpec &s) {
  if(auto current=logic_settings(id);current&&*current==s)return;
  auto ids = actors(data_);
  auto p = actor(ids, id);
  transaction(
      [&] {
        write_logic(p, s);
        const auto current = actors(data_);
        auto root = id;
        if (u(data_, p + 12) == 4030)
          root = u(data_, u(data_, p + 16) + 8);
        need(!immediate_cycle(data_, current, root),
             "This removes the last delay from a logic loop. Keep a positive "
             "timer/delay before applying these settings");
      },
      true);
}

size_t Document::add_logic(std::array<float, 3> position,
                           const LogicSpec &supplied) {
  auto s = supplied;
  validate(s);
  if (s.kind == LogicKind::Light || s.kind == LogicKind::Explosion) {
    const auto count =
        std::count_if(objects_.begin(), objects_.end(), [&](const auto &o) {
          return o.type == 4080 &&
                 effect_at(data_, u(data_, o.offset + 16)).has_value();
        });
    need(count < gc::editor::effect_max_helpers,
         "A map can contain at most 256 editor light/explosion helpers");
  }
  if (s.kind == LogicKind::Light) {
    const auto count =
        std::count_if(objects_.begin(), objects_.end(), [&](const auto &o) {
          if (o.type != 4080)
            return false;
          auto effect = effect_at(data_, u(data_, o.offset + 16));
          return effect && effect->kind == gc::editor::EffectKind::Light;
        });
    need(count < gc::editor::effect_max_lights,
         "A map can contain at most eight editor lights");
  }
  for (auto f : position)
    need(std::isfinite(f) && std::abs(f) <= 1e6f, "Invalid logic position");
  uint32_t p = 0;
  transaction(
      [&] {
        auto m = append_zero(48);
        Matrix transform{1, 0,           0, position[0], 0, 1,
                         0, position[1], 0, 0,           1, position[2]};
        this->put(m, transform);
        auto relay = [&](const std::string &label, bool enabled = true) {
          auto cfg = append_zero(128);
          write_word(data_, cfg, enabled);
          for (unsigned k = 0; k < 10; ++k)
            write_word(data_, cfg + 20 + k * 4, 0xffffffff);
          return append_entity(4080, label, m, cfg);
        };
        if (s.kind == LogicKind::Area) {
          auto r = relay("__SSNE_LOGIC_HELPER_AREA_");
          auto cfg = append_zero(16);
          write_word(data_, cfg + 8, u(data_, r + 20));
          p = append_entity(4030, prefix(s.kind), m, cfg);
        } else if (s.kind == LogicKind::KeyBranch) {
          auto cfg = append_zero(40);
          for (auto k : {16u, 20u, 32u})
            write_word(data_, cfg + k, 0xffffffff);
          p = append_entity(4050, prefix(s.kind), m, cfg);
        } else if (s.kind == LogicKind::Button || s.kind == LogicKind::Toggle) {
          auto router = relay("__SSNE_LOGIC_HELPER_ROUTER_");
          auto cfg = append_zero(gc::editor::button_config_bytes);
          write_word(data_, cfg + gc::editor::button_magic_offset,
                     gc::editor::button_magic);
          write_word(data_, cfg + gc::editor::button_version_offset,
                     gc::editor::button_version);
          write_word(data_, cfg + 4, u(data_, router + 20));
          write_word(data_, cfg + 12, 0xffffffff);
          p = append_entity(4180, prefix(s.kind), m, cfg);
          if (s.kind == LogicKind::Toggle) {
            auto on = relay("__SSNE_LOGIC_HELPER_ON_"),
                 off = relay("__SSNE_LOGIC_HELPER_OFF_", false),
                 to_off = relay("__SSNE_LOGIC_HELPER_SET_OFF_"),
                 to_on = relay("__SSNE_LOGIC_HELPER_SET_ON_");
            auto rc = u(data_, router + 16);
            write_word(data_, rc + 20, u(data_, on + 20));
            write_word(data_, rc + 24, u(data_, off + 20));
            auto a = u(data_, on + 16), b = u(data_, off + 16),
                 ta = u(data_, to_off + 16), tb = u(data_, to_on + 16);
            write_word(data_, a + 0x38, u(data_, to_off + 20));
            write_word(data_, b + 0x38, u(data_, to_on + 20));
            write_word(data_, ta + 20, u(data_, on + 20));
            write_word(data_, ta + 24, u(data_, off + 20));
            write_word(data_, ta + 0x3c, 2);
            write_word(data_, ta + 0x40, 1);
            scalar(data_, ta + 0x78, .01f);
            write_word(data_, tb + 20, u(data_, on + 20));
            write_word(data_, tb + 24, u(data_, off + 20));
            write_word(data_, tb + 0x3c, 1);
            write_word(data_, tb + 0x40, 2);
            scalar(data_, tb + 0x78, .01f);
            write_word(data_, cfg + 16, u(data_, on + 20));
            write_word(data_, cfg + 20, u(data_, off + 20));
            write_word(data_, cfg + 24, u(data_, to_off + 20));
            write_word(data_, cfg + 28, u(data_, to_on + 20));
          }
        } else
          p = relay(prefix(s.kind), s.kind != LogicKind::Gate);
        // Creation flags become native configuration; no editor-only scheduler.
        if (s.kind == LogicKind::Gate)
          s.enabled = false;
        write_logic(p, s);
      },
      true);
  auto it = std::find_if(objects_.begin(), objects_.end(),
                         [&](const auto &o) { return o.offset == p; });
  need(it != objects_.end(), "New logic object missing");
  return size_t(it - objects_.begin());
}

size_t Document::add_button(const Document &original, size_t brush,
                            std::array<float, 3> position,
                            const LogicSpec &spec) {
  need(spec.kind == LogicKind::Button || spec.kind == LogicKind::Toggle,
       "Choose a button or toggle");
  need(original.data_ == original.base_ &&
           original.relocations_ == original.base_relocations_,
       "Button assets must come from an unchanged original map");
  need(brush < original.objects_.size() &&
           original.objects_[brush].type == 4070,
       "Choose an original native button brush");
  // Bake the original brush into an independent native model source. Its
  // local vertex arrays/materials remain native data and the existing import
  // walker copies all referenced assets. Donor terrain/scripts stay inactive.
  Document prepared = original;
  uint32_t model_actor = 0;
  prepared.transaction(
      [&] {
        const auto &o = prepared.objects_[brush];
        auto cfg = u(prepared.data_, o.offset + 16);
        const auto groups = list(prepared.data_, 0x48);
        auto idx = u(prepared.data_, cfg + 12);
        need(idx < groups.size(), "Original button brush geometry is missing");
        auto g = groups[idx];
        auto matrix = u(prepared.data_, g + 8);
        if (!matrix) {
          auto keys = list(prepared.data_, g + 12);
          need(!keys.empty(), "Original button has no transform");
          matrix = u(prepared.data_, keys.front() + 4);
        }
        need(matrix && u(prepared.data_, g + 16),
             "Original button has no visible surface");
        auto source = prepared.append_zero(44);
        prepared.pointer(source + 4, u(prepared.data_, g + 16));
        for (unsigned k = 0; k < 4; ++k)
          prepared.pointer(source + 16 + k * 4,
                           u(prepared.data_, k == 3 ? 0x20 : 0x10 + k * 4));
        prepared.pointer(source + 32, u(prepared.data_, g + 0x2c));
        prepared.pointer(source + 36, u(prepared.data_, g + 0x28));
        auto sources = list(prepared.data_, 0x5c);
        prepared.pointer(sources.empty() ? 0x5c : sources.back(), source);
        auto visual = prepared.append_zero(48),
             transform = prepared.copy_block(matrix, 48);
        prepared.pointer(visual + 4, transform);
        prepared.pointer(visual + 8,
                         prepared.append_string("SSNE editor button"));
        prepared.pointer(visual + 12,
                         prepared.append_string("Editor button model"));
        auto local_bound = u(prepared.data_, g + 0x2c);
        need(local_bound && uint64_t(local_bound) + 20 <= prepared.data_.size(),
             "Original button bounds are missing");
        auto bound = prepared.copy_block(local_bound, 20);
        float center[3]{},
            radius = std::bit_cast<float>(u(prepared.data_, bound + 16));
        float max_scale = 0;
        for (unsigned r = 0; r < 3; ++r) {
          for (unsigned k = 0; k < 3; ++k)
            center[r] += std::bit_cast<float>(
                             u(prepared.data_, matrix + (r * 4 + k) * 4)) *
                         std::bit_cast<float>(
                             u(prepared.data_, local_bound + 4 + k * 4));
          center[r] +=
              std::bit_cast<float>(u(prepared.data_, matrix + (r * 4 + 3) * 4));
          scalar(prepared.data_, bound + 4 + r * 4, center[r]);
          float axis = 0;
          for (unsigned k = 0; k < 3; ++k) {
            auto value = std::bit_cast<float>(
                u(prepared.data_, matrix + (k * 4 + r) * 4));
            axis += value * value;
          }
          max_scale = std::max(max_scale, std::sqrt(axis));
        }
        radius *= max_scale;
        scalar(prepared.data_, bound + 16, radius);
        scalar(prepared.data_, bound,
               std::sqrt(center[0] * center[0] + center[1] * center[1] +
                         center[2] * center[2]) +
                   radius);
        auto frame = prepared.append_zero(20);
        prepared.pointer(
            frame + 4,
            prepared.append_string(
                "models/ssne/editor_button_" +
                original.source_hash().substr(0, 12) + "_" +
                std::to_string(
                    u(original.data_, original.objects_[brush].offset + 20)) +
                ".ssm"));
        prepared.pointer(frame + 8, source);
        prepared.pointer(frame + 16, bound);
        // Native SceneModel flag0x2 disables the collider. Default0
        // stays visible (SceneInit derives viewport visibility0x10) and retains
        // the original brush collider for precise real Player-Use ray hits.
        write_word(prepared.data_, visual + 36, 0);
        prepared.pointer(visual + 44, frame);
        auto models = list(prepared.data_, 0x58);
        prepared.pointer(models.empty() ? 0x58 : models.back(), visual);
        auto c = prepared.append_zero(16);
        write_word(prepared.data_, c, uint32_t(models.size()));
        write_word(prepared.data_, c + 4, 1);
        write_word(prepared.data_, c + 8, 0xffffffff);
        write_word(prepared.data_, c + 12, 0xffff0000u);
        model_actor =
            prepared.append_entity(4200, "Editor Button Model ", transform, c);
      },
      true);
  Document donor;
  donor.parse(prepared.export_bytes());
  auto it =
      std::find_if(donor.objects_.begin(), donor.objects_.end(),
                   [&](const auto &o) { return o.offset == model_actor; });
  need(it != donor.objects_.end(), "Button model could not be prepared");
  const auto model_index = size_t(it - donor.objects_.begin());
  uint32_t button_id = 0;
  transaction(
      [&] {
        auto model = import_object(donor, model_index, position);
        auto model_offset = objects_[model].offset,
             transform = objects_[model].transform;
        auto next = add_logic(position, spec);
        auto button = objects_[next].offset;
        button_id = u(data_, button + 20);
        pointer(button + 4, transform);
        write_word(data_, u(data_, button + 16) + 12,
                   u(data_, model_offset + 20));
      },
      true);
  const auto ids = actors(data_);
  auto at = actor(ids, button_id);
  auto found = std::find_if(objects_.begin(), objects_.end(),
                            [&](const auto &o) { return o.offset == at; });
  return size_t(found - objects_.begin());
}
size_t Document::duplicate_editor_logic(size_t index,
                                        std::array<float, 3> position) {
  need(index < objects_.size() && is_editor_logic(index),
       "Choose an authored logic node");
  auto source = objects_[index];
  auto id = u(data_, source.offset + 20);
  auto spec = logic_settings(id);
  need(bool(spec), "Unsupported authored logic node");
  auto slots = logic_slots(id);
  spec->name += " copy";
  if (spec->name.size() > 90)
    spec->name.resize(90);
  uint32_t created = 0;
  transaction(
      [&] {
        uint32_t model = 0;
        if (source.type == 4180) {
          auto c = u(data_, source.offset + 16), visual = u(data_, c + 12);
          auto ids = actors(data_);
          need(ids.contains(visual) && u(data_, ids.at(visual) + 12) == 4200,
               "Button's visible model is missing");
          model = clone_entity(ids.at(visual), position, true);
        }
        auto next = add_logic(position, *spec);
        auto p = objects_[next].offset;
        created = u(data_, p + 20);
        if (model) {
          pointer(p + 4, u(data_, model + 4));
          write_word(data_, u(data_, p + 16) + 12, u(data_, model + 20));
        }
        for (const auto &slot : slots)
          if (slot.target)
            set_logic_link(created, slot.slot, slot.target, slot.action);
      },
      true);
  auto ids = actors(data_);
  auto p = actor(ids, created);
  auto it = std::find_if(objects_.begin(), objects_.end(),
                         [&](const auto &o) { return o.offset == p; });
  return size_t(it - objects_.begin());
}
void Document::erase_editor_logic(size_t index) {
  need(index < objects_.size() && is_editor_logic(index),
       "Choose an authored logic node");
  auto target = objects_[index];
  auto ids = actors(data_);
  std::set<uint32_t> removed{u(data_, target.offset + 20)};
  auto cfg = u(data_, target.offset + 16);
  if (target.type == 4180) {
    removed.insert(u(data_, cfg + 4));
    removed.insert(u(data_, cfg + 12));
    if (target.name.starts_with(prefix(LogicKind::Toggle)))
      for (auto k : {16u, 20u, 24u, 28u})
        removed.insert(u(data_, cfg + k));
  } else if (target.type == 4030) {
    removed.insert(u(data_, cfg + 8));
  } else if (target.type == 4080 &&
             target.name.starts_with(prefix(LogicKind::Timer))) {
    auto clock = u(data_, cfg + 0x38);
    if (ids.contains(clock) && name(data_, u(data_, ids.at(clock) + 8))
                                   .starts_with("__SSNE_LOGIC_HELPER_CLOCK_"))
      removed.insert(clock);
  }
  for (auto [id, p] : ids)
    if (!removed.contains(id))
      for (const auto &s : logic_slots(id))
        need(!s.target || !removed.contains(*s.target),
             "Remove incoming connections before deleting this logic node");
  transaction(
      [&] {
        for (auto id : removed) {
          auto it = ids.find(id);
          if (it == ids.end())
            continue;
          auto p = it->second;
          if (u(data_, p + 12) == 4200) {
            auto c = u(data_, p + 16);
            auto models = list(data_, 0x58);
            need(u(data_, c) < models.size(),
                 "Button model binding is invalid");
            auto node = models[u(data_, c)];
            pointer(node + 16, 0);
            pointer(node + 20, 0);
            pointer(node + 44, 0);
            write_word(data_, node + 36, 0);
          }
          unlink_entity(p);
        }
      },
      true);
}
} // namespace mapedit
