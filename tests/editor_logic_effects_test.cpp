// CPU fake-native contract tests. No ROM/game/GPU needed; actual PAL rendering
// and host/client effect replication additionally require integration QA.
#include "gc/editor_effect.h"
#include "ssne/editor_logic_effects.h"
#include "ssne/native_entity.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "state_io.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks = 0, native_calls = 0, creates = 0, releases = 0,
         damage_calls = 0, fx_calls = 0, original_fires = 0;
bool replica = false;
float damage = 0, force = 0, radius = 0;
std::map<uint32_t, PPCFunc> slots;
ssne::NativeEntityObserver observer;
constexpr uint32_t engine = 0x80400000, actors = 0x80500000,
                   configs = 0x80600000, strings = 0x80700001;
std::vector<SSNEFuncEntry> entries;
void check(bool v, const char *why) {
  ++checks;
  if (!v)
    throw std::runtime_error(why);
}
void init_original(PPCContext &c) {
  const auto actor = c.r[3], cfg = mem_read32(actor + 0x54);
  check(mem_read32(cfg + 8) == 0,
        "Reserved effect reached native ActionNames parser");
  mem_write32(actor + 0x84, 0);
  mem_write32(actor + 0x8C, mem_read32(cfg));
}
void fire_original(PPCContext &c) {
  const auto actor = c.r[3];
  check(!mem_read32(actor + 0x84) && !mem_read32(mem_read32(actor + 0x54) + 8),
        "Native fire received reserved cached action");
  ++original_fires;
  mem_write32(actor + 0x94, mem_read32(actor + 0x94) + 1);
}
void unused(PPCContext &) {}
PPCContext context() {
  PPCContext c;
  c.r[13] = 0x802E22A0;
  c.r[1] = 0x817F0000;
  return c;
}
uint32_t make(unsigned index, gc::editor::Effect effect, bool enabled = true) {
  const uint32_t actor = actors + index * 0x100, cfg = configs + index * 0x100,
                 text = strings + index * 0x100;
  mem_write32(actor + 0x48, 0x80209758);
  mem_write32(actor + 0x54, cfg);
  mem_write32(cfg, enabled);
  mem_write32(cfg + 8, text);
  const auto encoded = gc::editor::encode_effect(effect);
  for (unsigned i = 0; i <= encoded.size(); ++i)
    mem_write8(text + i, i == encoded.size() ? 0 : encoded[i]);
  mem_write32(actor + 0x20, f2u(10.f + index));
  mem_write32(actor + 0x24, f2u(3));
  mem_write32(actor + 0x28, f2u(-9));
  auto c = context();
  c.r[3] = actor;
  slots.at(0x800846D0)(c);
  check(mem_read32(cfg + 8) == text,
        "Init lost relocated effect action string");
  return actor;
}
} // namespace
namespace ssne {
uint8_t *g_ram = nullptr;
uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size() { return 0x01800000; }
uint32_t guest_ram_end() { return 0x81800000; }
uint32_t mmio_read(uint32_t, int) {
  throw std::runtime_error("Unexpected MMIO read");
}
void mmio_write(uint32_t, uint32_t, int) {
  throw std::runtime_error("Unexpected MMIO write");
}
void log_message(const std::string &) {}
bool replica_simulation() { return replica; }
uint64_t observe_native_entities(NativeEntityObserver value) {
  observer = std::move(value);
  return 1;
}
void notify_native_entity(PPCContext &c, NativeEntityNotice n) {
  n.context = &c;
  if (observer)
    observer(n);
}
} // namespace ssne
PPCFunc *ssne_hook_slot(uint32_t address) { return &slots[address]; }
void ssne_install_hook(uint32_t address, PPCFunc fn) { slots[address] = fn; }
const SSNEFuncEntry *ssne_find_func(uint32_t address) {
  for (const auto &e : entries)
    if (e.address == address)
      return &e;
  return nullptr;
}
void ssne_call(PPCContext &c, uint32_t address) {
  ++native_calls;
  if (address == 0x801431A8) {
    const auto light = 0x80800000 + creates++ * 0x100;
    mem_write32(light, 0x8021D1E8);
    mem_write32(light + 0x44, engine);
    mem_write32(light + 4, mem_read32(engine + 0x18));
    mem_write32(engine + 0x18, light);
    mem_write32(c.r[4], light);
    c.r[3] = 0;
  } else if (address == 0x801219B0) {
    ++releases;
    const auto target = c.r[3];
    auto link = engine + 0x18;
    while (mem_read32(link) && mem_read32(link) != target)
      link = mem_read32(link) + 4;
    check(mem_read32(link) == target, "Released light not in native scene");
    mem_write32(link, mem_read32(target + 4));
    mem_write32(target + 0x44, 0);
  } else if (address == 0x801227CC) {
    for (unsigned i = 0; i < 3; ++i)
      mem_write32(c.r[3] + 12 + i * 4, mem_read32(c.r[4] + i * 4));
  } else if (address == 0x80122760) {
    mem_write32(c.r[3] + 8, c.r[4]);
  } else if (address == 0x8012280C) {
    mem_write32(c.r[3] + 0x48, mem_read32(c.r[4]));
  } else if (address == 0x80122844) {
    mem_write32(c.r[3] + 0x40, f2u(float(c.ps0[1])));
  } else if (address == 0x8000E330) {
    mem_write32(c.r[3], 2);
    mem_write32(c.r[3] + 0xC, c.r[4]);
  } else if (address == 0x8000DF58) {
    ++damage_calls;
    damage = u2f(mem_read32(c.r[4] + 0x10));
    force = u2f(mem_read32(c.r[4] + 0x14));
    radius = u2f(mem_read32(c.r[4] + 0x18));
    check(!mem_read32(c.r[4] + 0xC),
          "Environment explosion acquired unsafe owner");
  } else if (address == 0x800D653C) {
    ++fx_calls;
    check(c.r[6] <= 4, "Invalid explosion style passed to original factory");
  } else
    throw std::runtime_error("Unrecognized native API");
}
int main() {
  try {
    using namespace ssne;
    auto ram = std::make_unique<uint8_t[]>(kRamSize);
    g_ram = ram.get();
    mem_write32(0x80000000, 0x47334250);
    mem_write32(0x80209758 + 0x44, 0x800848C0);
    mem_write32(0x8021EB70 + 0x4C, 0x801431A8);
    mem_write32(0x8021D1E8 + 0x34, 0x801219B0);
    mem_write32(0x802ABAA4, engine);
    mem_write32(engine, 0x8021EB70);
    mem_write32(0x802E22A0 - 0x7F90, 0x80410000);
    mem_write32(0x802E22A0 - 0x5474, f2u(1));
    mem_write32(0x801FEAF8, f2u(8));
    const std::array<std::pair<uint32_t, uint32_t>, 13> signatures{
        {{0x800846D0, 0x9421FFF0},
         {0x80084C30, 0x9421FFB8},
         {0x801431A8, 0x9421FFF0},
         {0x801219B0, 0x9421FFF0},
         {0x80122760, 0x9421FFF8},
         {0x801227CC, 0xC1A40000},
         {0x8012280C, 0x9421FFF0},
         {0x80122844, 0x9421FFF8},
         {0x800D653C, 0x9421FF20},
         {0x8000E330, 0x9421FFF0},
         {0x8000DF58, 0x9421FFF8},
         {0, 0},
         {0, 0}}};
    for (const auto [address, word] : signatures)
      if (address) {
        mem_write32(address, word);
        entries.push_back({address,
                           address == 0x800846D0   ? init_original
                           : address == 0x80084C30 ? fire_original
                                                   : unused,
                           "mock"});
      }
    std::string error;
    mem_write32(0x80122844, 0);
    check(!install_editor_logic_effects(error),
          "Incompatible PAL function signature accepted");
    mem_write32(0x80122844, 0x9421FFF8);
    mem_write32(0x801FEAF8, f2u(7));
    check(!install_editor_logic_effects(error), "Changed PAL grenade force constant accepted");
    mem_write32(0x801FEAF8, f2u(8));
    check(install_editor_logic_effects(error), "Verified PAL mock rejected");
    auto c = context();
    gc::editor::Effect light;
    const auto first = make(0, light);
    editor_logic_effects_frame(c);
    check(creates == 1 && editor_logic_effect_stats().lights == 1,
          "Enabled helper did not create point light");
    const auto light_handle = mem_read32(engine + 0x18);
    check(mem_read32(light_handle + 0x48) == 0xFFDCAAFF &&
              u2f(mem_read32(light_handle + 0x40)) == 8,
          "Native colour/radius not applied");
    const auto calls = native_calls;
    editor_logic_effects_frame(c);
    check(calls == native_calls, "Unchanged light repeated native setters");
    mem_write32(first + 0x20, f2u(30));
    editor_logic_effects_frame(c);
    check(u2f(mem_read32(light_handle + 12)) == 30,
          "Moved helper did not update original light position");
    mem_write32(first + 0x8C, 0);
    editor_logic_effects_frame(c);
    check(releases == 1 && !editor_logic_effect_stats().lights,
          "Disabled light retained native selection slot");
    mem_write32(first + 0x8C, 1);
    for (unsigned i = 0; i < 61; ++i)
      editor_logic_effects_frame(c);
    check(editor_logic_effect_stats().lights == 1,
          "Re-enabled light did not return");
    StateWriter saved;
    editor_effect_state_save(saved);
    const auto saved_ram =
        std::vector<uint8_t>(g_ram, g_ram + guest_ram_size());
    const auto before = native_calls;
    StateReader inspect{saved.buf.data(), saved.buf.size()};
    check(editor_effect_state_load(inspect, false) &&
              inspect.pos == inspect.n && before == native_calls,
          "Inspect mutated native state");
    for (size_t size = 0; size < saved.buf.size(); ++size) {
      StateReader broken{saved.buf.data(), size};
      check(size == 0 ? editor_effect_state_load(broken, false)
                      : !editor_effect_state_load(broken, false),
            "Truncated checkpoint section accepted");
    }
    auto corrupt = saved.buf;
    corrupt[0] = '!';
    StateReader bad{corrupt.data(), corrupt.size()};
    check(!editor_effect_state_load(bad, false),
          "Corrupt helper section accepted");
    reset_editor_logic_effects(c);
    check(!editor_logic_effect_stats().helpers, "Reset retained helpers");
    std::copy(saved_ram.begin(), saved_ram.end(), g_ram);
    StateReader restored{saved.buf.data(), saved.buf.size()};
    check(editor_effect_state_load(restored) &&
              editor_logic_effect_stats().lights == 1,
          "Restored RAM ownership was lost");
    const auto before_restore = native_calls;
    StateReader absent{};
    check(editor_effect_state_load(absent) && native_calls == before_restore,
          "Legacy state released handles against restored RAM");
    std::copy(saved_ram.begin(), saved_ram.end(), g_ram);
    StateReader rollback{saved.buf.data(), saved.buf.size()};
    check(editor_effect_state_load(rollback),
          "Rollback could not restore captured ownership");
    notify_native_entity(c, {NativeEntityNotice::Event::Removing, first});
    check(!editor_logic_effect_stats().lights,
          "Actor removal leaked native Light");
    for (unsigned i = 1; i <= 10; ++i)
      make(i, light);
    editor_logic_effects_frame(c);
    check(editor_logic_effect_stats().lights == 8,
          "Light budget not bounded to eight");
    notify_native_entity(c, {NativeEntityNotice::Event::LevelExit});
    check(!editor_logic_effect_stats().helpers && !mem_read32(engine + 0x18),
          "Level exit leaked light pool entries");
    gc::editor::Effect boom;
    boom.kind = gc::editor::EffectKind::Explosion;
    boom.damage = 77;
    boom.radius = 12;
    const auto explosive = make(20, boom);
    c.r[3] = explosive;
    slots.at(0x80084C30)(c);
    check(damage_calls == 1 && fx_calls == 1 && damage == 77 && force == 8 && radius == 12,
          "Explosion did not use original radial damage/factory");
    check(original_fires == 1 && mem_read32(explosive + 0x94) == 1,
          "Original trigger counter/fanout was bypassed");
    set_editor_logic_effects_authoritative(false);
    c.r[3] = explosive;
    slots.at(0x80084C30)(c);
    check(damage_calls == 1 && fx_calls == 1,
          "Client helper replayed damage or explosion FX");
    set_editor_logic_effects_authoritative(true);
    replica = true;
    c.r[3] = explosive;
    slots.at(0x80084C30)(c);
    check(damage_calls == 1 && fx_calls == 1,
          "Replica simulation duplicated gameplay effects");
    replica = false;
    boom.radius = 3.25f;
    boom.damage = 41;
    mem_write32(0x802E22A0 - 0x5474, f2u(37));
    const auto second_explosive = make(21, boom);
    c.r[3] = second_explosive;
    slots.at(0x80084C30)(c);
    check(damage_calls == 2 && fx_calls == 2 && damage == 41 &&
              force == 8 && radius == 3.25f,
          "Authored radius/force confused with original grenade radius");
    std::cout << "PASS editor_logic_effects_fake_native checks=" << checks
              << " creates=" << creates << " releases=" << releases
              << " damage=" << damage_calls << " fx=" << fx_calls << "\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
