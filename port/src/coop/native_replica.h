#pragma once
#include "native_world.h"
#include "native_bone_spans.h"
#include "ssne/ppc_context.h"
#include <set>
#include <algorithm>
#include <vector>
namespace port::coop {
namespace presentation_detail {
// Stable last-write/try-emplace semantics, independent of the sorting path.
// The ordinal is assigned before every append; no guest address is cached.
struct VisualWrite {uint32_t address,value,ordinal;bool replace;};
inline void normalize_visual_plan(std::vector<VisualWrite>& plan,bool reuse){
    if(reuse)std::sort(plan.begin(),plan.end(),[](const auto& a,const auto& b){
        return a.address!=b.address?a.address<b.address:a.ordinal<b.ordinal;
    });
    else std::stable_sort(plan.begin(),plan.end(),[](const auto& a,const auto& b){return a.address<b.address;});
    size_t unique=0;
    for(size_t first=0;first<plan.size();){
        size_t next=first+1;auto write=plan[first];
        while(next<plan.size() && plan[next].address==write.address){if(plan[next].replace)write=plan[next];++next;}
        plan[unique++]=write;first=next;
    }
    plan.resize(unique);
}
}
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
    NativeReplica(const NativeReplica&)=delete;
    NativeReplica& operator=(const NativeReplica&)=delete;
    NativeReplica(NativeReplica&&)=delete;
    NativeReplica& operator=(NativeReplica&&)=delete;
    NativeReplicaResult apply(uint64_t epoch,const std::map<EntityId,EntityState>& authoritative);
    NativeReplicaResult apply(PPCContext&,uint64_t epoch,const std::map<EntityId,EntityState>& authoritative);
    NativeReplicaResult apply_with_vehicles(PPCContext&,uint64_t epoch,const std::map<EntityId,EntityState>&,const Properties& mission);
    uint32_t local_address(EntityId server_id) const;
    bool play_projectile_visual(PPCContext&,uint64_t epoch,const NativeProjectileVisual&,uint64_t frame,std::string& error);
    bool advance_projectile_visuals(PPCContext&,uint64_t frame,std::string& error);
    bool advance_combo_huds(PPCContext&,float seconds,std::string& error);
    bool advance_particles(PPCContext&,float seconds,std::string& error);
    // A render lease changes only verified camera/model/bone storage. Gameplay
    // fields and proxy visibility/lifetime are never borrowed for smoothing.
    bool begin_presentation(const std::map<EntityId,EntityState>&,std::string& error);
    bool begin_presentation_with_vehicles(const std::map<EntityId,EntityState>&,const Properties&,std::string& error);
    void end_presentation();
    void reset();
private:
    friend struct NativeReplicaRenderTest; // Private owned-PAL lease/rollback observability.
    bool begin_presentation_native(const std::map<EntityId,EntityState>&,const Properties*,std::string& error);
    NativeReplicaResult apply_native(PPCContext&,uint64_t,const std::map<EntityId,EntityState>&,const Properties* vehicles);
    NativeReplicaResult apply_scalars(uint64_t,const std::map<EntityId,EntityState>&,bool lifecycle_projection=false);
    NativeWorld& local_;
    uint64_t epoch_=0;
    std::map<EntityId,EntityId> bindings_;
    std::vector<EntityId> particle_ids_; // Epoch/lifetime IDs, never cached guest pointers.
    std::map<EntityId,Properties> combo_presented_;
    struct ComboTimer {uint32_t actor=0;int64_t phase=0;float received=0,remaining=0;};
    std::map<EntityId,ComboTimer> combo_timers_;
    struct ProjectileVisual {NativeProjectileVisual event;EntityId local=0;uint64_t frame=0;bool final_pose=false;};
    std::map<EntityId,ProjectileVisual> projectile_visuals_;
    std::set<EntityId> seen_projectiles_;
    uint64_t projectile_visual_sequence_=0;
    std::vector<presentation_detail::VisualWrite> presentation_plan_;
    std::vector<std::pair<uint32_t,uint32_t>> presentation_saved_;
    struct BorrowedPose {uint32_t object=0,model=0;int weapon=-1;unsigned hand=0;std::vector<float> original,visual;};
    // Capacity only: addresses, metadata and all values are refreshed per lease.
    // Oversized working frames use transient storage and retain no extra cache.
    struct PresentationBuffers {
        std::vector<BorrowedPose> poses;
        std::vector<presentation_detail::SavedBoneSpan> saved_spans;
        std::vector<std::vector<uint8_t>> packed_spans;
        std::vector<presentation_detail::NativeBoneSpan> spans;
        std::vector<presentation_detail::BoneWriteRange> scalar_ranges;
        size_t pose_count=0,span_count=0;
        void clear_frame();
        size_t retained_bytes() const;
    };
    static constexpr size_t presentation_scratch_budget_=8u*1024u*1024u;
    static constexpr size_t presentation_scratch_pose_limit_=512;
    PresentationBuffers presentation_scratch_,presentation_reference_;
    bool presentation_reference_active_=false;
    bool presentation_active_=false;
};
}
