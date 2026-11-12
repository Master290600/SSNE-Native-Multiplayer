#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <optional>
struct PPCContext;
namespace ssne {
bool replica_hide_npc_part(uint32_t object);
bool replica_suppressed_npc_proxy(uint32_t proxy);
void install_replica_simulation_hooks();
void set_replica_simulation(bool enabled);
bool replica_simulation();
uint32_t native_game_scene();
uint32_t native_gameplay_gui();
uint32_t prepare_native_gameplay_gui(PPCContext&,std::string& error);
void replica_clear_enemy_poses();
bool replica_enemy_pose(uint32_t object,uint32_t model,const std::vector<float>& pose);
bool replica_weapon_pose(uint32_t actor,unsigned weapon,unsigned index,const std::vector<float>& pose);
std::optional<std::array<float,12>> native_weapon_camera_reference(uint32_t instance);
struct NativeMovieState {uint64_t sequence=0;std::string asset;uint32_t frame=0;bool active=false;};
NativeMovieState native_movie_state();
bool native_movie_presenting();
void set_native_movie_vote_gate(bool enabled);
bool request_native_movie_skip(PPCContext&,uint64_t sequence,std::string&);
bool apply_native_movie(PPCContext&,const NativeMovieState&,std::string&);
struct ReplicaSimulationStats {uint64_t game_updates=0,world_updates=0;};
ReplicaSimulationStats replica_simulation_stats();
}
