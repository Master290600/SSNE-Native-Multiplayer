#include "native_particles.h"
#include "native_world.h"
#include "ssne/native_effect.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>

namespace port::coop {
namespace {
constexpr std::array<NativeParticleClass, 7> classes{{
    {0,0x802156A8,0x2D8,0x800D20E8,0x9421FF50,0x800D2740,0x9421FF60},
    {1,0x802157F8,0x158,0x800D111C,0x9421FFE8,0x800D13F8,0x9421FFF0},
    {2,0x80215600,0x168,0x800D2EA0,0x9421FF30,0x800D376C,0x9421FFD8},
    {3,0x80215750,0x158,0x800D1A08,0x9421FFE8,0x800D1CE4,0x9421FFF0},
    {4,0x80215558,0x064,0x800D3EC0,0x9421FF28,0x800D457C,0x9421FFF0},
    {5,0x802154B0,0x064,0x800D4BA0,0x9421FF30,0x800D52D0,0x9421FFF0},
    {6,0x80215408,0x054,0x800D54A0,0x4E800020,0x800D5A38,0x9421FFF0},
}};
bool readable(uint32_t p, unsigned n) {
  return p >= 0x80000000u && n <= ssne::guest_ram_size() &&
         p <= ssne::guest_ram_end() - n;
}
bool fail(std::string &error, const char *why) { error = why; return false; }
struct Binding {
  const NativeParticleClass *type = nullptr;
  uint32_t emitter = 0;
};
bool inspect(uint32_t object, Binding &binding, std::string &error) {
  if (!ssne::g_ram || mem_read32(0x80000000u) != 0x47334250u ||
      !readable(object, 0x64) ||
      mem_read32(object + 0x48) != native_particles_vtable)
    return fail(error, "Native particle holder class is unavailable");
  const auto config = mem_read32(object + 0x54);
  if (!readable(config, 56))
    return fail(error, "Native particle holder configuration is unavailable");
  const auto *type = native_particle_class(mem_read16(config + 2));
  const auto emitter = mem_read32(object + 0x60);
  if (!type || !readable(emitter, type->size) ||
      mem_read32(emitter + 0x1C) != type->vtable ||
      mem_read32(emitter + 0x18) != 0 ||
      mem_read32(object + 0x4C) > 1 || mem_read32(object + 0x58) > 1)
    return fail(error, "Native particle holder emitter/flags are incompatible");
  if (mem_read32(native_particles_vtable + 0x44) != 0x800814C4u ||
      mem_read32(native_particles_vtable + 0xAC) != 0x80081578u ||
      mem_read32(native_particles_vtable + 0xB4) != 0x800815D4u ||
      mem_read32(native_particles_vtable + 0x54) != 0x8008147Cu ||
      mem_read16(native_particles_vtable + 0x50) ||
      !ssne_find_func(0x8008147Cu) ||
      mem_read32(0x8008147Cu) != 0x9421FFF0u ||
      mem_read16(native_particles_vtable + 0xA8) ||
      mem_read16(native_particles_vtable + 0xB0) ||
      mem_read32(type->vtable + 0x34) != type->update ||
      mem_read32(type->vtable + 0x74) != type->enable ||
      mem_read16(type->vtable + 0x30) || mem_read16(type->vtable + 0x70) ||
      !ssne_find_func(0x80081578u) || !ssne_find_func(0x800815D4u) ||
      mem_read32(0x80081578u) != 0x9421FFF0u ||
      mem_read32(0x800815D4u) != 0x9421FFF0u ||
      !ssne_find_func(type->update) || !ssne_find_func(type->enable) ||
      mem_read32(type->update) != type->update_prologue ||
      mem_read32(type->enable) != type->enable_prologue)
    return fail(error, "Native particle holder method signatures differ");
  binding = {type, emitter};
  return true;
}
bool context_ok(const PPCContext &context) {
  return ssne::replica_simulation() && context.r[13] == 0x802E22A0u &&
         readable(context.r[1] - 0x1000, 0x1000);
}
const int64_t *integer(const EntityState &e, const char *name) {
  const auto it = e.properties.find(name);
  return it == e.properties.end() ? nullptr : std::get_if<int64_t>(&it->second);
}
} // namespace

const NativeParticleClass *native_particle_class(unsigned kind) {
  return kind < classes.size() ? &classes[kind] : nullptr;
}
bool capture_native_particles(uint32_t object, EntityState &e,
                              std::string &error) {
  Binding binding;
  if (!inspect(object, binding, error))
    return false;
  e.properties["particles_kind"] = int64_t(binding.type->kind);
  e.properties["particles_enabled"] = int64_t(mem_read32(object + 0x58));
  return true;
}
bool validate_native_particles(uint32_t object, const EntityState &e,
                               bool allow_change, std::string &error) {
  Binding binding;
  if (e.kind != EntityKind::Mission ||
      e.definition != native_particles_definition ||
      !inspect(object, binding, error))
    return fail(error, "Particle state targets another native class");
  const auto *kind = integer(e, "particles_kind"),
             *enabled = integer(e, "particles_enabled"),
             *index = integer(e, "script_index");
  const auto reference = index ? native_script_reference_address(*index) : std::nullopt;
  const bool unindexed = index && *index == 0 && !mem_read32(object + 0x50);
  if (!kind || *kind != binding.type->kind ||
      (allow_change && !enabled) ||
      (enabled && *enabled != 0 && *enabled != 1) || !index ||
      (!unindexed && (!reference || *reference != object)))
    return fail(error, "Invalid native particle identity/kind/emission state");
  if (!allow_change && enabled && *enabled != mem_read32(object + 0x58))
    return fail(error, "Particle emission change requires native lifecycle context");
  return true;
}
bool apply_native_particles(PPCContext &context, uint32_t object,
                            const EntityState &e, std::string &error) {
  if (!context_ok(context) || !validate_native_particles(object, e, true, error))
    return fail(error, "Replica particle lifecycle is unavailable");
  const auto desired = uint32_t(*integer(e, "particles_enabled"));
  if (mem_read32(object + 0x58) == desired)
    return true; // Repeated samples must not restart emission or its sound.
  return ssne::set_native_scene_particle_enabled(
      context, object, unsigned(*integer(e, "particles_kind")), desired, error);
}
bool update_native_particles(PPCContext &context,
                             std::span<const uint32_t> objects, float seconds,
                             std::string &error) {
  if (!context_ok(context) || !std::isfinite(seconds) || seconds < 0 ||
      seconds > .1f || objects.size() > 4096)
    return fail(error, "Invalid replica particle presentation context");
  std::vector<Binding> bindings;
  std::set<uint32_t> emitters;
  for (const auto object : objects) {
    Binding binding;
    if (!inspect(object, binding, error))
      return false;
    if (emitters.insert(binding.emitter).second)
      bindings.push_back(binding);
  }
  // Complete read-only preflight precedes every visual call. Kind0 already
  // runs in update_native_pickup_ambient; kind6 has a native no-op updater.
  for (const auto &binding : bindings)
    if (binding.type->kind != 0 && binding.type->kind != 6 &&
        !ssne::update_native_scene_particle(context, binding.emitter,
                                            binding.type->kind, seconds, error))
      return false;
  return true;
}
} // namespace port::coop
