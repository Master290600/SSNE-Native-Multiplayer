#include "ssne/editor_logic_effects.h"
#include "gc/editor_effect.h"
#include "ssne/native_entity.h"
#include "ssne/ppc_context.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include "state_io.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace ssne {
namespace {
constexpr uint32_t trigger_table = 0x80209758, engine_table = 0x8021EB70;
constexpr uint32_t light_table = 0x8021D1E8;
constexpr unsigned helper_limit = gc::editor::effect_max_helpers;
constexpr uint32_t init_address = 0x800846D0, fire_address = 0x80084C30;
PPCFunc original_init = nullptr, original_fire = nullptr;
uint64_t entity_observer = 0;
std::atomic<bool> authority{true};
struct Binding {
  uint32_t actor = 0, config = 0, action = 0, light = 0, engine = 0;
  uint64_t generation = 0, retry = 0;
  gc::editor::Effect effect;
  std::array<float, 3> position{};
  bool applied = false;
};
std::map<uint32_t, Binding> bindings;
uint64_t generation = 0, frame = 0, explosions = 0, rejected = 0;
bool readable(uint32_t p, unsigned size) {
  return p >= 0x80000000u && size <= guest_ram_size() &&
         p <= guest_ram_end() - size;
}
bool pointer(uint32_t p) { return !(p & 3) && readable(p, 4); }
bool context_ok(const PPCContext &c) {
  return mem_read32(0x80000000) == 0x47334250u && c.r[13] == 0x802E22A0u &&
         c.r[1] >= 0x80008000u && readable(c.r[1] - 0x4000, 0x4000);
}
bool actor_matches(const Binding &b) {
  return readable(b.actor, 0xA8) &&
         mem_read32(b.actor + 0x48) == trigger_table &&
         mem_read32(b.actor + 0x54) == b.config && readable(b.config, 0x80) &&
         mem_read32(b.config + 8) == b.action;
}
bool position_of(uint32_t actor, std::array<float, 3> &out) {
  if (!readable(actor + 0x20, 12))
    return false;
  for (unsigned i = 0; i < 3; ++i) {
    out[i] = u2f(mem_read32(actor + 0x20 + i * 4));
    if (!std::isfinite(out[i]) || std::abs(out[i]) > 1000000)
      return false;
  }
  return true;
}
struct Decoded {
  uint32_t config = 0, action = 0;
  bool custom = false, valid = false;
  gc::editor::Effect effect;
};
Decoded decode(uint32_t actor) {
  Decoded out;
  if (!readable(actor, 0xA8) || mem_read32(actor + 0x48) != trigger_table)
    return out;
  out.config = mem_read32(actor + 0x54);
  if (!readable(out.config, 0x80))
    return out;
  out.action = mem_read32(out.config + 8);
  std::string value;
  if (!out.action)
    return out;
  for (unsigned i = 0; i <= gc::editor::effect_max_bytes; ++i) {
    if (!readable(out.action + i, 1))
      return out;
    const auto ch = char(mem_read8(out.action + i));
    if (!ch) {
      out.custom = gc::editor::is_effect(value);
      std::string why;
      out.valid =
          out.custom && gc::editor::decode_effect(value, out.effect, why);
      return out;
    }
    value += ch;
  }
  out.custom = gc::editor::is_effect(value);
  return out;
}
// Keep scratch under the current guest stack, including original nested-call
// frames. Native calls receive a copy of PPCContext and preserve the caller.
struct Scratch {
  uint32_t start;
  std::array<uint8_t, 0x4000> saved;
  PPCContext call;
  explicit Scratch(const PPCContext &c) : start(c.r[1] - 0x4000), call(c) {
    std::memcpy(saved.data(), host_ptr(start), saved.size());
    call.msr &= ~0x8000u;
    call.r[1] -= 0x200;
    mem_write32(call.r[1], c.r[1]);
  }
  ~Scratch() { std::memcpy(host_ptr(start), saved.data(), saved.size()); }
};
uint32_t current_engine() {
  const auto engine = mem_read32(0x802ABAA4);
  return readable(engine, 0x20) && mem_read32(engine) == engine_table ? engine
                                                                      : 0;
}
bool owned_light(const Binding &b, bool check_list) {
  if (!b.light || b.engine != current_engine() || !readable(b.light, 0x58) ||
      mem_read32(b.light) != light_table ||
      mem_read32(b.light + 0x44) != b.engine)
    return false;
  if (!check_list)
    return true;
  std::set<uint32_t> visited;
  auto node = mem_read32(b.engine + 0x18);
  while (node && visited.size() < 1024 && readable(node, 0x58) &&
         visited.insert(node).second) {
    if (node == b.light)
      return true;
    node = mem_read32(node + 4);
  }
  return false;
}
void release_light(PPCContext &c, Binding &b) {
  if (context_ok(c) && owned_light(b, true)) {
    Scratch temp(c);
    temp.call.r[3] = b.light;
    ssne_call(temp.call, 0x801219B0);
  }
  b.light = b.engine = 0;
  b.applied = false;
  b.retry = 0;
}
unsigned light_count() {
  unsigned count = 0;
  for (const auto &[key, b] : bindings)
    count += b.light != 0;
  return count;
}
void update_light(PPCContext &c, Binding &b) {
  if (!mem_read32(b.actor + 0x8C)) {
    release_light(c, b);
    return;
  }
  std::array<float, 3> position;
  if (!position_of(b.actor, position)) {
    release_light(c, b);
    return;
  }
  if (b.light && !owned_light(b, false)) {
    b.light = b.engine = 0;
    b.applied = false;
  }
  if (!b.light) {
    if (frame < b.retry || light_count() >= gc::editor::effect_max_lights)
      return;
    b.retry = frame + 60;
    const auto engine = current_engine();
    if (!engine)
      return;
    Scratch temp(c);
    const auto out = temp.call.r[1] + 0x80;
    mem_write32(out, 0);
    temp.call.r[3] = engine;
    temp.call.r[4] = out;
    ssne_call(temp.call, 0x801431A8);
    b.light = mem_read32(out);
    b.engine = engine;
    if (!owned_light(b, true)) {
      b.light = b.engine = 0;
      return;
    }
  }
  if (b.applied && b.position == position)
    return;
  Scratch temp(c);
  const auto data = temp.call.r[1] + 0x80;
  for (unsigned i = 0; i < 3; ++i)
    mem_write32(data + i * 4, f2u(position[i]));
  temp.call.r[3] = b.light;
  temp.call.r[4] = data;
  ssne_call(temp.call, 0x801227CC);
  if (!b.applied) {
    temp.call.r[3] = b.light;
    temp.call.r[4] = 0;
    ssne_call(temp.call, 0x80122760);
    for (unsigned i = 0; i < 4; ++i)
      mem_write8(data + i, b.effect.colour[i]);
    temp.call.r[3] = b.light;
    temp.call.r[4] = data;
    ssne_call(temp.call, 0x8012280C);
    temp.call.r[3] = b.light;
    temp.call.ps0[1] = b.effect.radius;
    ssne_call(temp.call, 0x80122844);
  }
  b.position = position;
  b.applied = true;
}
void explode(PPCContext &c, const Binding &b) {
  // The original host captures 800D653C in native_effect.cpp and replicates its
  // visual journal. Clients render that journal and must never dispatch damage.
  if (!authority.load(std::memory_order_relaxed) || replica_simulation() ||
      !context_ok(c))
    return;
  std::array<float, 3> position;
  if (!position_of(b.actor, position))
    return;
  Scratch temp(c);
  const auto data = temp.call.r[1] + 0x80;
  if (b.effect.damage > 0) {
    const auto manager = mem_read32(c.r[13] - 0x7F90);
    if (!readable(manager, 12))
      return;
    temp.call.r[3] = data;
    temp.call.r[4] = 0;
    ssne_call(temp.call, 0x8000E330); // null owner is native environment damage
    mem_write32(data + 0x10, f2u(b.effect.damage));
    // Original8006DAB4 uses+14 for force and+18 for the falloff radius.
    mem_write32(data + 0x14, mem_read32(0x801FEAF8));
    mem_write32(data + 0x18, f2u(b.effect.radius));
    for (unsigned i = 0; i < 3; ++i)
      mem_write32(data + 0x1C + i * 4, f2u(position[i]));
    temp.call.r[3] = manager;
    temp.call.r[4] = data;
    ssne_call(temp.call, 0x8000DF58);
  }
  for (unsigned i = 0; i < 3; ++i) {
    mem_write32(data + i * 4, f2u(position[i]));
    mem_write32(data + 12 + i * 4, f2u(i == 1 ? 1.f : 0.f));
    mem_write32(data + 24 + i * 4, 0);
  }
  temp.call.r[3] = data;
  temp.call.r[4] = data + 12;
  temp.call.r[5] = data + 24;
  temp.call.r[6] = b.effect.style;
  temp.call.ps0[1] = b.effect.scale;
  ssne_call(temp.call, 0x800D653C);
  ++explosions;
}
// RAII preserves original map bytes. Unknown reserved schema versions are also
// suppressed, preventing the native ActionNames parser asserting on our data.
struct HideAction {
  uint32_t actor, config, action;
  bool cached;
  HideAction(uint32_t a, const Decoded &d, bool cache)
      : actor(a), config(d.config), action(d.action), cached(cache) {
    mem_write32(config + 8, 0);
    if (cached)
      mem_write32(actor + 0x84, 0);
  }
  ~HideAction() {
    if (readable(actor, 0xA8) && mem_read32(actor + 0x48) == trigger_table &&
        mem_read32(actor + 0x54) == config && readable(config, 0x80) &&
        mem_read32(config + 8) == 0)
      mem_write32(config + 8, action);
  }
};
void init_hook(PPCContext &c) {
  const auto actor = c.r[3];
  const auto data = decode(actor);
  if (auto it = bindings.find(actor); it != bindings.end()) {
    release_light(c, it->second);
    bindings.erase(it);
  }
  if (!data.custom) {
    ssne_call_original(c, init_address, original_init);
    return;
  }
  {
    HideAction hide(actor, data, false);
    ssne_call_original(c, init_address, original_init);
  }
  unsigned authored_lights = 0;
  for (const auto &[id, b] : bindings)
    authored_lights += b.effect.kind == gc::editor::EffectKind::Light;
  if (!data.valid || bindings.size() >= helper_limit ||
      (data.effect.kind == gc::editor::EffectKind::Light &&
       authored_lights >= gc::editor::effect_max_lights)) {
    ++rejected;
    log_message(
        "[editor-logic] Rejected malformed or excessive native effect helper");
    return;
  }
  Binding b;
  b.actor = actor;
  b.config = data.config;
  b.action = data.action;
  b.effect = data.effect;
  b.generation = ++generation;
  if (actor_matches(b)) {
    mem_write32(actor + 0x84, 0);
    bindings.emplace(actor, b);
  }
}
void fire_hook(PPCContext &c) {
  const auto actor = c.r[3];
  const auto data = decode(actor);
  if (!data.custom) {
    ssne_call_original(c, fire_address, original_fire);
    return;
  }
  const auto it = bindings.find(actor);
  const auto copy = it != bindings.end() ? it->second : Binding{};
  {
    HideAction hide(actor, data, true);
    ssne_call_original(c, fire_address, original_fire);
  }
  const auto now = bindings.find(actor);
  if (data.valid && copy.actor && now != bindings.end() &&
      now->second.generation == copy.generation && actor_matches(copy) &&
      copy.effect.kind == gc::editor::EffectKind::Explosion)
    explode(c, copy);
}
} // namespace
bool install_editor_logic_effects(std::string &error) {
  if (original_init) {
    error.clear();
    return true;
  }
  struct Signature {
    uint32_t address, word;
  };
  constexpr Signature signatures[] = {
      {init_address, 0x9421FFF0}, {fire_address, 0x9421FFB8},
      {0x801431A8, 0x9421FFF0},   {0x801219B0, 0x9421FFF0},
      {0x80122760, 0x9421FFF8},   {0x801227CC, 0xC1A40000},
      {0x8012280C, 0x9421FFF0},   {0x80122844, 0x9421FFF8},
      {0x800D653C, 0x9421FF20},   {0x8000E330, 0x9421FFF0},
      {0x8000DF58, 0x9421FFF8}};
  if (!g_ram || mem_read32(0x80000000) != 0x47334250u ||
      mem_read32(trigger_table + 0x44) != 0x800848C0u ||
      mem_read32(engine_table + 0x4C) != 0x801431A8u ||
      mem_read32(light_table + 0x34) != 0x801219B0u ||
      mem_read32(0x801FEAF8) != 0x41000000u) {
    error = "Editor world effects require the verified PAL native classes";
    return false;
  }
  for (const auto &s : signatures) {
    if (!ssne_find_func(s.address) || mem_read32(s.address) != s.word) {
      error = "Editor world-effect native function signature is incompatible";
      return false;
    }
  }
  auto chain = [](uint32_t address, PPCFunc hook, PPCFunc &previous) {
    previous = *ssne_hook_slot(address);
    if (!previous)
      previous = ssne_find_func(address)->func;
    ssne_install_hook(address, hook);
  };
  chain(init_address, init_hook, original_init);
  chain(fire_address, fire_hook, original_fire);
  entity_observer =
      observe_native_entities([](const NativeEntityNotice &notice) {
        if (notice.event == NativeEntityNotice::Event::LevelExit) {
          if (notice.context)
            reset_editor_logic_effects(*notice.context);
          else {
            bindings.clear();
            ++generation;
          }
        } else if (notice.event == NativeEntityNotice::Event::Removing) {
          if (auto it = bindings.find(notice.object); it != bindings.end()) {
            if (notice.context)
              release_light(*notice.context, it->second);
            bindings.erase(it);
          }
        }
      });
  error.clear();
  return true;
}
void set_editor_logic_effects_authoritative(bool enabled) {
  authority.store(enabled, std::memory_order_relaxed);
}
void editor_logic_effects_frame(PPCContext &c) {
  if (!original_init || !context_ok(c))
    return;
  ++frame;
  for (auto it = bindings.begin(); it != bindings.end();) {
    if (!actor_matches(it->second)) {
      release_light(c, it->second);
      it = bindings.erase(it);
    } else {
      if (it->second.effect.kind == gc::editor::EffectKind::Light)
        update_light(c, it->second);
      ++it;
    }
  }
}
void reset_editor_logic_effects(PPCContext &c) {
  for (auto &[actor, b] : bindings)
    release_light(c, b);
  bindings.clear();
  ++generation;
}
EditorLogicEffectStats editor_logic_effect_stats() {
  return {uint32_t(bindings.size()), light_count(), explosions, rejected};
}
void editor_effect_state_save(StateWriter &w) {
  w.tag("EDF1");
  w.u64(generation);
  w.u64(frame);
  w.u64(explosions);
  w.u64(rejected);
  w.u32(uint32_t(bindings.size()));
  for (const auto &[actor, b] : bindings) {
    w.u32(b.actor);
    w.u32(b.config);
    w.u32(b.action);
    w.u32(b.light);
    w.u32(b.engine);
    w.u64(b.generation);
    w.u64(b.retry);
    const auto encoded = gc::editor::encode_effect(b.effect);
    w.u32(uint32_t(encoded.size()));
    w.bytes(encoded.data(), encoded.size());
    for (const auto value : b.position)
      w.u32(f2u(value));
    w.u32(b.applied);
  }
}
bool editor_effect_state_load(StateReader &r, bool apply) {
  // Old states predate helper ownership. RAM has already been restored during
  // application, so dropping host pointers must not invoke native Release.
  if (r.ok && r.pos == r.n) {
    if (apply) {
      bindings.clear();
      generation = frame = explosions = rejected = 0;
    }
    return true;
  }
  if (!r.tag("EDF1"))
    return false;
  const auto saved_generation = r.u64(), saved_frame = r.u64();
  const auto saved_explosions = r.u64(), saved_rejected = r.u64();
  const auto count = r.u32();
  if (!r.ok || count > helper_limit)
    return false;
  std::map<uint32_t, Binding> candidate;
  std::set<uint32_t> lights;
  for (unsigned i = 0; i < count; ++i) {
    Binding b;
    b.actor = r.u32();
    b.config = r.u32();
    b.action = r.u32();
    b.light = r.u32();
    b.engine = r.u32();
    b.generation = r.u64();
    b.retry = r.u64();
    const auto length = r.u32();
    if (!r.ok || length > gc::editor::effect_max_bytes)
      return false;
    std::string encoded(length, '\0'), error;
    if (!r.bytes(encoded.data(), length) ||
        !gc::editor::decode_effect(encoded, b.effect, error))
      return false;
    for (auto &value : b.position)
      value = u2f(r.u32());
    const auto applied = r.u32();
    if (!r.ok || !pointer(b.actor) || !pointer(b.config) ||
        !readable(b.action, 1) ||
        (b.light && (!pointer(b.light) || !pointer(b.engine))) ||
        (!b.light && b.engine) || applied > 1 || (!b.light && applied) ||
        b.generation > saved_generation || !b.generation ||
        (b.light && b.effect.kind != gc::editor::EffectKind::Light))
      return false;
    for (auto value : b.position)
      if (!std::isfinite(value))
        return false;
    b.applied = applied != 0;
    if (!candidate.emplace(b.actor, b).second ||
        (b.light && !lights.insert(b.light).second))
      return false;
  }
  if (lights.size() > gc::editor::effect_max_lights)
    return false;
  if (apply) {
    bindings.swap(candidate);
    generation = saved_generation;
    frame = saved_frame;
    explosions = saved_explosions;
    rejected = saved_rejected;
  }
  return r.ok;
}
} // namespace ssne
