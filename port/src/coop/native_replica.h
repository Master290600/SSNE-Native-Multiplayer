#pragma once
#include "native_world.h"
#include "ssne/ppc_context.h"
#include <set>
namespace port::coop {
struct NativeReplicaResult {
    bool applied=false;
    unsigned objects=0,writes=0;
    std::string error;
};
// Applies verified native actor fields to already loaded replicas. Matching uses
// player slots or definition/spawn coordinates, never a transmitted guest pointer.
// Missing creations/removals fail explicitly until handled by the lifecycle adapter.
class NativeReplica {
public:
    explicit NativeReplica(NativeWorld& local):local_(local){}
    NativeReplicaResult apply(uint64_t epoch,const std::map<EntityId,EntityState>& authoritative);
    NativeReplicaResult apply(PPCContext&,uint64_t epoch,const std::map<EntityId,EntityState>& authoritative);
    NativeReplicaResult apply_with_vehicles(PPCContext&,uint64_t epoch,const std::map<EntityId,EntityState>&,const Properties& mission);
    uint32_t local_address(EntityId server_id) const;
    bool play_projectile_visual(PPCContext&,uint64_t epoch,const NativeProjectileVisual&,uint64_t frame,std::string& error);
    bool advance_projectile_visuals(PPCContext&,uint64_t frame,std::string& error);
    bool advance_combo_huds(PPCContext&,float seconds,std::string& error);
    // A render lease changes only verified camera/model/bone storage. Gameplay
    // fields and proxy visibility/lifetime are never borrowed for smoothing.
    bool begin_presentation(const std::map<EntityId,EntityState>&,std::string& error);
    bool begin_presentation_with_vehicles(const std::map<EntityId,EntityState>&,const Properties&,std::string& error);
    void end_presentation();
    void reset();
private:
    bool begin_presentation_native(const std::map<EntityId,EntityState>&,const Properties*,std::string& error);
    NativeReplicaResult apply_native(PPCContext&,uint64_t,const std::map<EntityId,EntityState>&,const Properties* vehicles);
    NativeReplicaResult apply_scalars(uint64_t,const std::map<EntityId,EntityState>&);
    NativeWorld& local_;
    uint64_t epoch_=0;
    std::map<EntityId,EntityId> bindings_;
    std::map<EntityId,Properties> combo_presented_;
    struct ComboTimer {uint32_t actor=0;int64_t phase=0;float received=0,remaining=0;};
    std::map<EntityId,ComboTimer> combo_timers_;
    struct ProjectileVisual {NativeProjectileVisual event;EntityId local=0;uint64_t frame=0;bool final_pose=false;};
    std::map<EntityId,ProjectileVisual> projectile_visuals_;
    std::set<EntityId> seen_projectiles_;
    uint64_t projectile_visual_sequence_=0;
    std::vector<std::pair<uint32_t,uint32_t>> presentation_saved_;
    struct BorrowedPose {uint32_t object,model;int weapon;unsigned hand;std::vector<float> original,visual;};
    std::vector<BorrowedPose> presentation_poses_;
};
}
