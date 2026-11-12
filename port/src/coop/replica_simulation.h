#pragma once
#include <cstdint>
namespace ssne {
void install_replica_simulation_hooks();
void set_replica_simulation(bool enabled);
bool replica_simulation();
struct ReplicaSimulationStats {uint64_t game_updates=0,world_updates=0;};
ReplicaSimulationStats replica_simulation_stats();
}
