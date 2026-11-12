#pragma once
#include "ssne/ppc_context.h"
#include "ssne/input.h"
#include <vector>
#include <array>
#include <optional>
#include <string>
namespace ssne {
void native_party_stabilize_idle(PPCContext&,uint32_t actor,const std::array<float,3>& before);
struct NativeSegmentHit {std::array<float,3> position{},normal{};uint32_t flags=0;};
// Executes the native PAL geometry query at a guest-thread safe point. No hit
// is distinct from unavailable/invalid context; callers must check both.
bool native_world_segment(PPCContext&,const std::array<float,3>& from,const std::array<float,3>& to,std::optional<NativeSegmentHit>& hit,std::string& error);
struct NativeSphereResult {std::array<float,3> position{};std::vector<NativeSegmentHit> contacts;};
bool native_world_sphere(PPCContext&,const std::array<float,3>& from,const std::array<float,3>& to,float radius,NativeSphereResult&,std::string& error);
std::optional<float> native_player_collision_radius(uint32_t actor);
bool native_party_prepare_spawn(PPCContext&,uint32_t actor,unsigned request);
void native_party_spawn_completed(uint32_t actor);
uint64_t native_party_spawn_revision(uint32_t actor);
bool native_party_request_join_spawn(PPCContext&,uint32_t actor,std::string& error);
void native_party_cancel_join_spawn(uint32_t actor);
bool native_party_spawn_waiting(uint32_t actor);
bool native_party_retry_spawn(PPCContext&,uint32_t actor);
void native_party_retry_waiting(PPCContext&);
void native_party_restore_test_collider_at_exit(uint32_t original);
bool native_party_replica_spawn_state(PPCContext&,uint32_t actor,bool waiting,const std::array<float,3>& host_position);
void native_party_install_geometry_trace();
void native_party_configure(unsigned count);
void native_party_leave_network();
void netplay_register_party_players();
void native_party_prepare_lobby(PPCContext& ctx);
uint32_t native_party_profile(unsigned slot);
bool native_party_joined(unsigned slot);
void native_party_set_occupancy(uint8_t mask,uint8_t connected_mask=255);
void native_party_refresh_occupancy(PPCContext&);
bool native_party_actor_occupied(uint32_t actor);
void native_party_install_occupancy_hooks();
uint32_t native_party_memory_size();
void native_party_extended_memory(bool enabled);
bool native_party_extended();
unsigned native_party_capacity();
uint32_t native_party_registry();
uint32_t native_party_loadouts();
void native_party_reset_extra_loadouts();
uint32_t native_party_ranking();
unsigned native_party_view_last();
uint32_t native_party_controller_offset(uint32_t slot);
void native_party_controller_channel(PPCContext& ctx);
uint32_t native_party_view_engine(uint32_t engine);
uint32_t native_party_view_slot(uint32_t slot);
uint32_t native_party_frame_view_mask(uint32_t canonical);
uint32_t native_party_frame_camera(uint32_t engine,unsigned physical_slot);
void native_party_initialize_player_masks(PPCContext&,uint32_t engine);
uint32_t native_party_pickup_flags(uint32_t pickup);
uint32_t native_party_pickup_value(uint32_t pickup,unsigned slot);
bool native_party_can_prepare_pickups(const std::vector<uint32_t>& pickups);
void native_party_set_input(unsigned slot,const PadState& pad);
void native_party_begin(PPCContext& ctx);
void native_party_end(PPCContext& ctx);
void native_party_reset_pickup(uint32_t pickup);
uint64_t native_party_checksum(uint64_t hash);
void native_party_trace(const char* phase,unsigned slot,uint32_t actor);
uint32_t native_party_second_bank();
uint32_t native_party_second_status();
void native_party_sample_second();
int native_party_owner();
void native_party_owner_set(int owner);
void native_party_bind_view(uint32_t view,unsigned slot);
// Checkpoint component; call only at a guest-thread safe point. Matching guest
// RAM must already be restored. The session must reconcile current occupancy
// and inputs before resuming simulation. Rejection leaves all party state intact.
bool native_party_save_state(std::vector<uint8_t>&,std::string& error);
bool native_party_load_state(const std::vector<uint8_t>&,std::string& error);
struct NativePartyOwnerScope {
    int previous;
    explicit NativePartyOwnerScope(PPCContext& ctx);
    ~NativePartyOwnerScope();
};
struct NativePartySpawnScope {
    PPCContext* context=nullptr;
    uint32_t count_address=0,previous_count=0,previous_msr=0,position=0,rotation=0,slot=0;
    explicit NativePartySpawnScope(PPCContext& ctx);
    bool prepared=false,successful=true;
    bool prepare();
    ~NativePartySpawnScope();
};
}
