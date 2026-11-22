#pragma once

#include <optional>

namespace port::coop {

enum class NativeLocalWeaponClipEvent {
    shot,
    last_round,
    reload_single_pistol,
    reload_dual_pistol,
    charge,
    release,
    held_begin,
    held_transition,
    held,
    held_end,
};

struct NativeLocalWeaponClipSpec {
    unsigned logical;
    float playback;
};

// Only maps a verified firing/animation edge to the original PAL logical
// clip. The caller owns readiness, trigger phase and original clip lifetime.
// The animation-only sampler resolves the logical clip using each weapon's
// installed native animation binding.
inline constexpr std::optional<NativeLocalWeaponClipSpec> native_local_weapon_clip_for_feedback(
    unsigned weapon,unsigned mode,NativeLocalWeaponClipEvent event){
    using Event=NativeLocalWeaponClipEvent;
    using Clip=NativeLocalWeaponClipSpec;
    if(mode>=18)return {};
    switch(weapon){
        case 0: // Drill 80053634: start, work and release.
            if(mode!=0)return {};
            if(event==Event::held_begin)return Clip{25,1.f};
            if(event==Event::held)return Clip{24,4.f};
            if(event==Event::held_end)return Clip{26,1.f};
            break;
        case 1: // Desert Hawks 800561AC selects 4 only when hand+228 == 11.
            if(mode!=0)return {};
            if(event==Event::shot)return Clip{2,1.f};
            if(event==Event::last_round)return Clip{4,1.f};
            // 80056098 selects the reload clip by active gun count and
            // starts the same sound (416) for either rig at normal speed.
            if(event==Event::reload_single_pistol)return Clip{3,1.f};
            if(event==Event::reload_dual_pistol)return Clip{5,1.f};
            break;
        case 2: // Uzi 800612A0 / 800615FC use the same clip state machine.
            if(mode>1)return {};
            if(event==Event::shot || event==Event::held_begin)return Clip{8,1.f};
            if(event==Event::held_transition)return Clip{9,1.f};
            if(event==Event::held)return Clip{10,2.f};
            if(event==Event::held_end)return Clip{11,1.f};
            break;
        case 3: // Double shotgun 8005E9D0: the clip includes its pump.
            if(event==Event::shot)return Clip{12,1.f};
            break;
        case 4: // Minigun 800576B4 uses clip 12 after spool-up.
            // The secondary branch 80057C5C only rotates its native barrel.
            if(mode==0 && event==Event::shot)return Clip{12,1.f};
            break;
        case 5: // Grenade 80055414 charges before launching (800556E4 etc.).
            if(mode>2)return {};
            if(event==Event::charge)return Clip{12,2.f};
            if(event==Event::release)return Clip{13,2.f};
            break;
        case 6: // All three rocket launch routines 8005CDEC/8005D048/8005D298.
            if(mode<=2 && event==Event::shot)return Clip{12,1.f};
            break;
        case 7: // GasGun 80054C00 has no separate skeletal firing clip.
            break;
        case 8: // Sniper 8005FF38 starts clip 12 in the visible, unscoped view.
            if(event==Event::shot)return Clip{12,1.f};
            break;
        case 9: // Sirian PowerGun has separate primary beam and charge paths.
            if(mode==0){
                if(event==Event::held_begin)return Clip{12,1.f};
                if(event==Event::held)return Clip{18,1.f};
                if(event==Event::held_end)return Clip{19,1.f};
            }else if(mode==1){
                if(event==Event::charge)return Clip{13,1.f};
                if(event==Event::held)return Clip{20,1.f};
                if(event==Event::release)return Clip{21,1.f};
            }
            break;
        case 10: // Cannon charge at 80053064 and accepted release at 8005320C.
            if(mode!=0)return {};
            if(event==Event::charge)return Clip{22,1.f};
            if(event==Event::release)return Clip{12,1.f};
            break;
        case 11: // Serious Bomb 8005DD3C lights its fuse on the first action.
            // Detonation and optional refill follow host bomb state.
            if(mode==0 && event==Event::shot)return Clip{12,1.f};
            break;
    }
    return {};
}

} // namespace port::coop
