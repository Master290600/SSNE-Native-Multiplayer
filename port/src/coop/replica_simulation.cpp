#include "ssne/replica_simulation.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/game_addresses.h"
#include <atomic>
namespace ssne {
namespace {
std::atomic<bool> replica{false};
std::atomic<uint64_t> game_skips{0},world_skips{0};
PPCFunc original_game=nullptr,original_world=nullptr;
uint32_t game_update=0,world_update=0;
bool suppress(){return replica && mem_read32(game_address(0x802DB0E0))==4 && mem_read32(game_address(0x802DB1BC))==1;}
void game_hook(PPCContext& context){if(suppress()){++game_skips;return;}ssne_call_original(context,game_update,original_game);}
void world_hook(PPCContext& context){if(suppress()){++world_skips;return;}ssne_call_original(context,world_update,original_world);}
}
void install_replica_simulation_hooks() {
    const bool pal=mem_read32(0x80000000u)==0x47334250u;
    game_update=pal?0x80166724:0x8016677C;world_update=pal?0x801508B4:0x801508FC;
    const auto* game=ssne_find_func(game_update);const auto* world=ssne_find_func(world_update);
    if(!game || !world || mem_read32(game_update)!=0x9421FFB8u || mem_read32(world_update)!=0x9421FFA8u) {
        // Enabling a replica without both verified update hooks would allow local AI/scripts.
        original_game=original_world=nullptr;return;
    }
    original_game=game->func;original_world=world->func;
    ssne_install_hook(game_update,game_hook);ssne_install_hook(world_update,world_hook);
}
void set_replica_simulation(bool enabled){if(enabled && (!original_game || !original_world))fatal("Native replica simulation hooks are unavailable for this revision.");replica=enabled;}
bool replica_simulation(){return replica;}
ReplicaSimulationStats replica_simulation_stats(){return {game_skips.load(),world_skips.load()};}
}
