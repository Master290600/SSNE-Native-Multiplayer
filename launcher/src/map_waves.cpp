#include "map_document.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
namespace mapedit {
namespace {
void need(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
uint32_t word(const std::vector<uint8_t> &b, uint32_t p) {
  need(uint64_t(p) + 4 <= b.size(), "Wave field outside map");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
void write_word(std::vector<uint8_t> &b, uint32_t p, uint32_t v) {
  need(uint64_t(p) + 4 <= b.size(), "Wave write outside map");
  for (unsigned i = 0; i < 4; ++i)
    b[p + i] = uint8_t(v >> (24 - 8 * i));
}
void scalar(std::vector<uint8_t> &b, uint32_t p, float f) {
  write_word(b, p, std::bit_cast<uint32_t>(f));
}
std::vector<uint32_t> entities(const std::vector<uint8_t> &b) {
  std::vector<uint32_t> out;
  std::set<uint32_t> seen;
  for (auto p = word(b, 0x40); p; p = word(b, p)) {
    need(seen.insert(p).second && seen.size() <= 50000,
         "Invalid wave entity list");
    out.push_back(p);
  }
  return out;
}
std::string name(const std::vector<uint8_t> &b, uint32_t p) {
  std::string s;
  for (unsigned n = 0; n < 256; ++n) {
    need(p + n < b.size(), "Wave name outside map");
    if (!b[p + n])
      return s;
    s += char(b[p + n]);
  }
  throw std::runtime_error("Wave name too long");
}
bool helper(const std::vector<uint8_t> &b, uint32_t p) {
  return wave_helper(word(b, p + 12), name(b, word(b, p + 8)));
}
} // namespace
bool wave_helper(uint32_t type, const std::string &name) {
  return (type == 4040 || type == 4080) &&
         (name.starts_with("__SSNE_WAVE_HELPER_") ||
          name.starts_with("__SSNE_LOGIC_HELPER_"));
}
uint32_t Document::append_zero(size_t n) {
  need(data_.size() + n + 4 < 128u * 1024 * 1024,
       "Wave exceeds map size limit");
  while (data_.size() % 4)
    data_.push_back(0);
  auto p = uint32_t(data_.size());
  data_.resize(data_.size() + n);
  return p;
}
uint32_t Document::append_entity(uint32_t type, const std::string &label,
                                 uint32_t transform, uint32_t config) {
  logic_lookup_valid_=false;
  auto id = allocate_identifier();
  auto p = append_zero(44);
  pointer(p + 4, transform);
  pointer(p + 8, append_string(label + std::to_string(id)));
  write_word(data_, p + 12, type);
  pointer(p + 16, config);
  write_word(data_, p + 20, id);
  auto list = entities(data_);
  pointer(list.empty() ? 0x40 : list.back(), p);
  return p;
}
void Document::unlink_entity(uint32_t target) {
  logic_lookup_valid_=false;
  uint32_t prev = 0;
  for (auto p : entities(data_)) {
    if (p == target) {
      pointer(prev ? prev : 0x40, word(data_, p));
      return;
    }
    prev = p;
  }
  throw std::runtime_error("Wave entity missing");
}
std::optional<WaveTrigger> Document::wave_trigger(size_t index) const {
  if (index >= objects_.size() || objects_[index].type != 4030 ||
      !objects_[index].name.starts_with("Editor Trigger "))
    return {};
  auto area = objects_[index].offset, c = word(data_, area + 16);
  auto list = entities(data_);
  std::map<uint32_t, uint32_t> ids;
  for (auto p : list)
    ids.emplace(word(data_, p + 20), p);
  auto id = word(data_, c + 8);
  need(ids.contains(id) && helper(data_, ids.at(id)) &&
           word(data_, ids.at(id) + 12) == 4080,
       "Wave relay missing");
  auto relay = word(data_, ids.at(id) + 16);
  WaveTrigger wave;
  auto relay_delay = std::bit_cast<float>(word(data_, relay + 0x78));
  wave.delay = relay_delay;
  wave.radius = std::bit_cast<float>(word(data_, c + 4));
  auto maximum = word(data_, relay + 0x74);
  need(maximum >= 1 && maximum <= 4096, "Invalid wave activation limit");
  wave.repeat = maximum > 1;
  wave.max_activations = wave.repeat ? maximum : 16;
  for (unsigned i = 0; i < 10; ++i) {
    auto target = word(data_, relay + 0x14 + i * 4);
    if (target == 0xffffffff)
      continue;
    // Other relay slots can drive doors, counters, or existing native waves.
    // They remain physical connections and are not owned enemy rows.
    if (!ids.contains(target) || !helper(data_, ids.at(target)) ||
        word(data_, ids.at(target) + 12) != 4040 ||
        word(data_, relay + 0x3c + i * 4) != 0)
      continue;
    auto cfg = word(data_, ids.at(target) + 16);
    auto enemy = word(data_, cfg + 20);
    need(ids.contains(enemy) && word(data_, ids.at(enemy) + 12) == 5000,
         "Wave enemy template missing");
    auto delay = std::bit_cast<float>(word(data_, cfg + 28));
    if (wave.rows.empty())
      wave.delay = relay_delay + delay;
    else
      need(wave.delay == relay_delay + delay, "Wave delays differ");
    wave.rows.push_back({ids.at(enemy), word(data_, cfg + 8)});
  }
  return wave;
}
void Document::write_wave(uint32_t area, const WaveTrigger &w) {
  need(std::isfinite(w.radius) && w.radius >= .25f && w.radius <= 10000 &&
           std::isfinite(w.delay) && w.delay >= 0 && w.delay <= 3600,
       "Radius or delay outside supported range");
  need(w.rows.size() <= 10 &&
           (!w.repeat || (w.max_activations >= 2 && w.max_activations <= 4096)),
       "Wave limit: ten spawn points, 2..4096 repeat activations");
  std::set<uint32_t> used;
  for (auto &r : w.rows) {
    auto it = std::find_if(objects_.begin(), objects_.end(), [&](auto &o) {
      return o.offset == r.enemy_offset && o.type == 5000 && o.added;
    });
    need(it != objects_.end() && r.count >= 1 && r.count <= 64 &&
             used.insert(r.enemy_offset).second,
         "Choose distinct newly placed enemies and counts 1..64");
  }
  std::map<uint32_t, uint32_t> ids;
  for (auto p : entities(data_))
    ids.emplace(word(data_, p + 20), p);
  auto c = own_logic_config(area, 16), relay_id = word(data_, c + 8);
  need(ids.contains(relay_id), "Wave relay missing");
  auto relay = own_logic_config(ids.at(relay_id), 128);
  struct Previous {
    unsigned slot;
    uint32_t spawn, enemy;
    bool reused = false;
  };
  std::vector<Previous> previous;
  unsigned external_count = 0;
  std::vector<uint32_t> previous_enemies;
  for (unsigned i = 0; i < 10; ++i) {
    auto id = word(data_, relay + 20 + i * 4);
    if (id != 0xffffffff) {
      if (!ids.contains(id) || !helper(data_, ids.at(id)) ||
          word(data_, ids.at(id) + 12) != 4040 ||
          word(data_, relay + 0x3c + i * 4) != 0) {
        ++external_count;
        continue;
      }
      auto cfg = word(data_, ids.at(id) + 16);
      auto enemy = word(data_, cfg + 20);
      if (ids.contains(enemy))
        previous_enemies.push_back(ids.at(enemy));
      previous.push_back(
          {i, ids.at(id), ids.contains(enemy) ? ids.at(enemy) : 0});
    }
    write_word(data_, relay + 20 + i * 4, 0xffffffff);
  }
  need(w.rows.size() + external_count <= 10,
       "Door/control connections and wave rows share ten relay outputs");
  write_word(data_, c, w.rows.empty() && !external_count ? 0 : 1);
  scalar(data_, c + 4, w.radius);
  scalar(data_, c + 12, .1f);
  write_word(data_, relay, 1);
  write_word(data_, relay + 0x74, w.repeat ? w.max_activations : 1);
  // A relay delay can also apply to door outputs. Keep that common delay
  // when a wave row is added rather than making existing doors fire early.
  auto previous_delay = std::bit_cast<float>(word(data_, relay + 0x78));
  need(std::isfinite(previous_delay) && previous_delay >= 0 &&
           previous_delay <= 3600,
       "Existing relay delay is outside the supported range");
  auto common_delay =
      w.rows.empty() ? w.delay : std::clamp(previous_delay, 0.f, w.delay);
  scalar(data_, relay + 0x78, common_delay);
  for (unsigned i = 0; i < w.rows.size(); ++i) {
    auto row = w.rows[i];
    auto old = std::find_if(previous.begin(), previous.end(), [&](auto &p) {
      return !p.reused && p.enemy == row.enemy_offset;
    });
    auto cfg = old == previous.end() ? append_zero(60)
                                     : own_logic_config(old->spawn, 60);
    scalar(data_, cfg, .25f);
    write_word(data_, cfg + 8, row.count);
    write_word(data_, cfg + 12, row.count * (w.repeat ? w.max_activations : 1));
    write_word(data_, cfg + 20, word(data_, row.enemy_offset + 20));
    scalar(data_, cfg + 28, w.delay - common_delay);
    write_word(data_, cfg + 32, 3);
    write_word(data_, cfg + 36, 0xffffffff);
    scalar(data_, cfg + 48, 1);
    auto spawn = old == previous.end()
                     ? append_entity(4040, "__SSNE_WAVE_HELPER_SPAWN_",
                                     word(data_, row.enemy_offset + 4), cfg)
                     : old->spawn;
    unsigned output = old == previous.end() ? 0 : old->slot;
    if (old != previous.end())
      old->reused = true;
    while (output < 10 && word(data_, relay + 20 + output * 4) != 0xffffffff)
      ++output;
    need(output < 10, "No free output for this wave row");
    write_word(data_, relay + 20 + output * 4, word(data_, spawn + 20));
    write_word(data_, relay + 0x3c + output * 4, 0);
    auto enemy_cfg = own_logic_config(row.enemy_offset, 24);
    write_word(data_, enemy_cfg + 4, 1);
  }
  // Keep stable helper IDs when wave settings change. Removed rows can be
  // shared by another controller; remove an owned helper only when no other
  // authored activation output still points at it.
  for (auto &old : previous)
    if (!old.reused) {
      auto id = word(data_, old.spawn + 20);
      bool referenced = false;
      for (auto p : entities(data_)) {
        if (p == ids.at(relay_id) || !word(data_, p + 4))
          continue;
        for (auto &slot : logic_slots(word(data_, p + 20)))
          if (slot.target == id)
            referenced = true;
      }
      if (!referenced)
        unlink_entity(old.spawn);
    }
  for (auto enemy : previous_enemies) {
    auto id = word(data_, enemy + 20);
    bool referenced = false;
    for (auto p : entities(data_))
      if (word(data_, p + 12) == 4040 &&
          word(data_, word(data_, p + 16) + 20) == id)
        referenced = true;
    if (!referenced)
      write_word(data_, word(data_, enemy + 16) + 4, 0);
  }
}
size_t Document::add_wave_trigger(std::array<float, 3> p,
                                  const WaveTrigger &w) {
  uint32_t area = 0;
  transaction(
      [&] {
        auto m = append_zero(48);
        Matrix transform{1, 0, 0, p[0], 0, 1, 0, p[1], 0, 0, 1, p[2]};
        for (auto f : p)
          need(std::isfinite(f) && std::abs(f) <= 1e6f,
               "Invalid trigger position");
        this->put(m, transform);
        auto cfg = append_zero(128);
        for (unsigned i = 0; i < 10; ++i)
          write_word(data_, cfg + 20 + i * 4, 0xffffffff);
        write_word(data_, cfg, 1);
        write_word(data_, cfg + 0x74, 1);
        auto relay = append_entity(4080, "__SSNE_WAVE_HELPER_RELAY_", m, cfg);
        auto watcher = append_zero(16);
        write_word(data_, watcher + 8, word(data_, relay + 20));
        area = append_entity(4030, "Editor Trigger ", m, watcher);
        write_wave(area, w);
      },
      true);
  auto it = std::find_if(objects_.begin(), objects_.end(),
                         [&](auto &o) { return o.offset == area; });
  return size_t(it - objects_.begin());
}
void Document::set_wave_trigger(size_t index, const WaveTrigger &w) {
  need(bool(wave_trigger(index)), "Select an editor-created trigger");
  auto area = objects_[index].offset;
  transaction([&] { write_wave(area, w); }, true);
}
void Document::erase_wave(size_t index) {
  auto wave = wave_trigger(index);
  need(bool(wave), "Unknown wave trigger");
  auto area = objects_[index].offset;
  transaction(
      [&] {
        auto cfg = word(data_, area + 16), relay_id = word(data_, cfg + 8);
        WaveTrigger empty = *wave;
        empty.rows.clear();
        write_wave(area, empty);
        uint32_t relay = 0;
        for (auto p : entities(data_))
          if (word(data_, p + 20) == relay_id)
            relay = p;
        need(relay, "Wave relay missing");
        unlink_entity(relay);
        unlink_entity(area);
      },
      true);
}
} // namespace mapedit
