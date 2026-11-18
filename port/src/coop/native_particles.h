#pragma once
#include "world_state.h"
#include "ssne/ppc_context.h"
#include <span>

namespace port::coop {
inline constexpr uint32_t native_particles_vtable = 0x80207320u;
inline constexpr uint32_t native_particles_definition = 2;
struct NativeParticleClass {
  unsigned kind;
  uint32_t vtable, size, update, update_prologue, enable, enable_prologue;
};
const NativeParticleClass *native_particle_class(unsigned kind);
bool capture_native_particles(uint32_t object, EntityState &, std::string &error);
// No native calls/writes during preflight. allow_change requires the later
// native lifecycle commit; raw scalar application never changes emission.
bool validate_native_particles(uint32_t object, const EntityState &,
                               bool allow_change, std::string &error);
bool apply_native_particles(PPCContext &, uint32_t object, const EntityState &,
                            std::string &error);
bool update_native_particles(PPCContext &, std::span<const uint32_t> objects,
                             float seconds, std::string &error);
} // namespace port::coop
