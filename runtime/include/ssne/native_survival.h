#pragma once
#include "ssne/ppc_context.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace ssne {
using NativeSurvivalPoint=std::array<float,3>;
// Set on both authority and replicas before queuing their native level load.
// Disabling changes no original resource flags or ordinary game-mode state.
void configure_native_survival(bool enabled);
// Freeze before guest startup; project FST contains an unmodified resource pack.
void configure_native_survival_project_resources();
bool native_survival_enabled();
void install_native_survival();
bool native_survival_available();
// Ready only after original campaign model backing and all ten native model
// slots are verified. Resource error copy is observed on the guest thread.
bool native_survival_resources_ready();
std::string native_survival_resource_error();
uint64_t native_survival_resource_revision();
// DM maps have no cooperative primary start. Bind an authored DM anchor via
// the original setter before native_party_prepare_spawn; ordinary modes and
// existing campaign starts are untouched. No synthetic object is created.
bool prepare_native_survival_player_start(PPCContext&,uint32_t actor,std::string& error);
// Original DM spawn-list positions, with a validated primary-start fallback.
// Guest thread only. No spawn selector, actor or random-generator mutation.
bool capture_native_survival_anchors(std::vector<NativeSurvivalPoint>&,std::string& error);
float native_survival_spawn_radius(unsigned definition);
// Bounded native collision/floor queries around authored anchors and living players.
// Return a supported clear sphere centre, separated from living players and
// supplied occupied enemy positions. The ordinary factory owns all actor work.
bool choose_native_survival_spawn(PPCContext&,unsigned definition,
    const std::vector<NativeSurvivalPoint>& living_players,
    const std::vector<NativeSurvivalPoint>& occupied,uint32_t selection,
    NativeSurvivalPoint& position,std::string& error);
}
