#pragma once
#include "ssne/ppc_context.h"
#include <cstdint>
#include <chrono>
#include <string>
namespace ssne {
// Authoritative presentation only; no boss AI, health or guest pointers travel.
struct NativeBossHudState {
    bool present=false,active=false,suppressed=false;
    float ratio=1.f;
};
struct NativeBossHudObservation {
    NativeBossHudState state;
    uint32_t widget=0,drawable=0;
    bool hidden=true;
    float flash_seconds=0;
};
bool validate_native_boss_hud(const NativeBossHudState&,std::string& error);
bool observe_native_boss_hud(NativeBossHudObservation&,std::string& error);
bool capture_native_boss_hud(NativeBossHudState&,std::string& error);
bool apply_native_boss_hud(PPCContext&,uint64_t epoch,const NativeBossHudState&,std::string& error);
// Safe transition cleanup: revalidate the current native widget before hiding.
// Absent/replaced widgets are never accessed through a retained pointer.
// Client presentation only, once per rendered frame, independently of snapshots.
// Original80168A0C advances the boss widget flash/texture animation; no AI runs.
bool advance_native_boss_hud(PPCContext&,uint64_t frame,std::chrono::steady_clock::time_point now,std::string& error);
bool clear_native_boss_hud(PPCContext&,std::string& error);
void forget_native_boss_hud();
}
