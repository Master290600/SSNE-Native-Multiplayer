#pragma once

#include "native_presentation.h"
#include "ssne/recomp_hooks.h"
#include <array>
#include <cmath>
#include <cstring>

namespace port::coop {
namespace local_special_weapon {

// The original scene method at 8013BF44 only reads its BSP/region overrides.
// The GasGun passes its current muzzle (80054D64) and checks region types 1/3.
inline bool gas_muzzle_allows_fire(const PPCContext& context, uint32_t actor,
                                   uint32_t instance) {
    if (mem_read32(actor + 0x360) == 3 || context.r[13] != 0x802E22A0u)
        return false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float coordinate = u2f(mem_read32(instance + 0x180 + axis * 4));
        if (!std::isfinite(coordinate) || std::abs(coordinate) > 1e6f)
            return false;
    }
    const auto world = mem_read32(context.r[13] - 0x75E0u);
    if (!presentation_readable(world, 0x29C))
        return false;
    const auto scene = mem_read32(world + 0x298);
    const auto stack = context.r[1] - 0x200u;
    constexpr uint32_t lookup = 0x8013BF44u;
    if (!presentation_readable(scene, 0xB5C) ||
        mem_read32(scene) != 0x8021E660u ||
        !presentation_readable(mem_read32(scene + 4), 0x84) ||
        !presentation_readable(stack, 0x200) ||
        !ssne_find_func(lookup) || mem_read32(lookup) != 0x9421FFA0u) {
        return false;
    }

    auto* guest_stack = ssne::host_ptr(stack);
    std::array<uint8_t, 0x200> original;
    std::memcpy(original.data(), guest_stack, original.size());
    auto call = context;
    call.msr &= ~0x8000u;
    call.r[1] -= 0x80u;
    mem_write32(call.r[1], context.r[1]);
    call.r[3] = scene;
    call.r[4] = instance + 0x180;
    call.r[5] = 0;
    ssne_call(call, lookup);
    std::memcpy(guest_stack, original.data(), original.size());
    if (!presentation_readable(call.r[3], 5))
        return false;
    const auto region = mem_read8(call.r[3] + 4);
    return region != 1 && region != 3;
}

inline bool available(uint32_t actor, uint32_t instance, unsigned weapon) {
    uint32_t table = 0;
    switch (weapon) {
        case 0: table = 0x801FAB40u; break;
        case 7: table = 0x801FADA8u; break;
        case 9: table = 0x801FB908u; break;
        case 10: table = 0x801FA880u; break;
        case 11: table = 0x801FBDA0u; break;
        default: return false;
    }
    return presentation_readable(actor, 0x5EC) &&
           presentation_readable(instance, 0x250) &&
           mem_read32(actor + 0x48) == 0x802121E8u &&
           mem_read32(instance + 0xC) == table &&
           mem_read32(instance + 0x1D4) == actor &&
           mem_read16(instance + 0xA);
}

inline bool has_ammo(uint32_t actor, uint32_t instance, unsigned mode) {
    const auto modes = mem_read32(instance + 0x1D0);
    const auto inventory = mem_read32(instance + 0x1B0);
    if (mode >= mem_read32(instance + 0x1BC) ||
        !presentation_readable(modes, 60 * (mode + 1)) ||
        !presentation_readable(inventory, 36) ||
        inventory != mem_read32(actor + 0x5E8)) {
        return false;
    }
    const auto config = modes + 60 * mode;
    const auto ammo_kind = int32_t(mem_read32(config + 0x38));
    const auto minimum = int16_t(mem_read16(config + 0x20));
    return ammo_kind >= 0 && ammo_kind < 18 && minimum > 0 &&
           int16_t(mem_read16(inventory + 2 * unsigned(ammo_kind))) >= minimum;
}

} // namespace local_special_weapon

// Read weapon state for client feedback while the host continues to own the
// weapon simulation. World and effect data are sampled on the native thread.
inline bool local_special_weapon_ready(uint32_t actor, uint32_t instance,
                                      unsigned weapon, const PPCContext* context = nullptr) {
    using namespace local_special_weapon;
    if (!available(actor, instance, weapon) || mem_read32(instance + 0x1B4) != 0)
        return false;
    const auto mode = mem_read32(instance + 0x1E0);
    if (weapon == 0)
        return mode == 0 && mem_read32(instance + 0x228) == 0;
    if (mode >= (weapon == 7 ? 3u : 1u) || !has_ammo(actor, instance, mode))
        return false;
    if (weapon == 7)
        return context && gas_muzzle_allows_fire(*context, actor, instance);
    if (weapon == 9)
        return mem_read32(instance + 0x228) == 0xFFFFFFFFu;
    if (weapon == 11)
        return mem_read32(instance + 0x228) == 1;

    // The cannon plays its charge cue (80053064) after its shot cooldown.
    const auto config = mem_read32(instance + 0x1D0);
    const auto attack_rate = u2f(mem_read32(config + 0x1C));
    const auto elapsed = u2f(mem_read32(instance + 0x1E8));
    return mem_read32(instance + 0x230) == 0 &&
           std::isfinite(attack_rate) && attack_rate > 0.f &&
           std::isfinite(elapsed) && elapsed > 1.f / attack_rate;
}

inline bool local_special_weapon_sustained(uint32_t actor, uint32_t instance,
                                           unsigned weapon, const PPCContext* context = nullptr) {
    using namespace local_special_weapon;
    if ((weapon != 0 && weapon != 7 && weapon != 9) ||
        !available(actor, instance, weapon) || mem_read32(instance + 0x1B4) != 3) {
        return false;
    }
    const auto mode = mem_read32(instance + 0x1E0);
    if (weapon == 0) {
        const auto blade = mem_read32(instance + 0x228);
        return mode == 0 && (blade == 1 || blade == 2);
    }
    if (mode >= (weapon == 7 ? 3u : 1u) || !has_ammo(actor, instance, mode))
        return false;
    return weapon == 9 || (context && gas_muzzle_allows_fire(*context, actor, instance));
}

} // namespace port::coop
