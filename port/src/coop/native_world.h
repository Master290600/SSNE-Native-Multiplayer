#pragma once
#include "world_state.h"
#include <memory>
#include <optional>
#include "ssne/native_effect.h"
namespace port::coop {
struct NativeUnmappedObject {uint32_t address=0,vtable=0;};
// Completed host lifetimes whose original visuals might fall entirely between
// published snapshots. All tags are logical IDs; no guest address is sent.
struct NativeProjectileVisual {
    uint64_t sequence=0;
    std::array<int64_t,4> tags{}; // entity, definition, source entity, class visual phase or generic model ID
    std::array<float,30> values{}; // birth/final position, velocity, rotation, scale, energy, power
};
struct NativeProjectileVisualSnapshot {
    uint64_t epoch=0,tail=0;
    std::vector<NativeProjectileVisual> events;
    std::string error;
};
bool validate_native_projectile_visual(const NativeProjectileVisual&,std::string& error);
EntityState native_projectile_visual_state(const NativeProjectileVisual&,bool final_pose);
struct NativeEnemyPoseBinding {uint32_t model=0,matrices=0,count=0;uint64_t layout=0;};
// Reuse resource-only skeleton descriptions during a guest-thread read phase.
// No guest writes or native calls may occur while this scope is alive. Each
// model's current storage/count is still checked; nothing survives the phase.
class NativePoseReadScope {
public:
    NativePoseReadScope() noexcept;
    ~NativePoseReadScope();
    NativePoseReadScope(const NativePoseReadScope&)=delete;
    NativePoseReadScope& operator=(const NativePoseReadScope&)=delete;
private:
    friend struct NativePoseLayoutCache;
    struct Layout {uint32_t data,count;uint64_t hash;};
    std::array<Layout,64> layouts_;
    unsigned count_=0;
    NativePoseReadScope* previous_;
};
std::optional<NativeEnemyPoseBinding> native_enemy_pose_binding(uint32_t object);
std::optional<NativeEnemyPoseBinding> native_corpse_pose_binding(uint32_t object);
std::optional<NativeEnemyPoseBinding> native_body_pose_binding(uint32_t object,unsigned index);
std::optional<NativeEnemyPoseBinding> native_weapon_pose_binding(uint32_t actor,unsigned weapon,unsigned instance);
// Epoch-local level script indices and logical player slots; no guest pointer is sent.
std::optional<int64_t> native_script_reference(uint32_t object);
std::optional<uint32_t> native_script_reference_address(int64_t reference);
struct NativeWorldSample {
    std::string error;
    uint64_t epoch=1,tick=0,created=0,registered=0,removed=0;
    unsigned mode=0,scene=0;
    std::map<EntityId,EntityState> entities;
    std::vector<NativeUnmappedObject> unmapped;
};
// Index of native object lifetimes and verified fields. Unknown native classes
// are reported explicitly; this is not a claim of complete mission replication.
class NativeWorld {
public:
    NativeWorld();~NativeWorld();
    NativeWorld(const NativeWorld&)=delete;
    NativeWorld& operator=(const NativeWorld&)=delete;
    void set_player_owner(unsigned slot,PlayerId);
    NativeWorldSample capture(uint64_t tick) const;
    // Local reconciliation only: lifetime/class/level identities, no bone,
    // camera, weapon or HUD snapshots. Never publish this as host world state.
    NativeWorldSample capture_bindings() const;
    struct PlayerBinding {uint32_t actor;unsigned slot;};
    std::vector<PlayerBinding> player_bindings() const;
    NativeProjectileVisualSnapshot projectile_visuals() const;
    bool acknowledge_projectile_visuals(uint64_t epoch,uint64_t tail);
    uint32_t local_address(EntityId) const;
    EntityId local_entity_id(uint32_t address) const;
    std::function<ssne::NativeHitIdentity(uint32_t)> hit_identity_resolver() const;
    // Guest-thread safe point only. This is the lifetime-index component of a
    // checkpoint, not a standalone world save. Restore matching guest RAM first.
    // Ownership belongs to the current session; saved connection IDs are not loaded.
    bool save_lifetimes(std::vector<uint8_t>&,std::string& error) const;
    class PreparedLifetimes {
    public:
        ~PreparedLifetimes();
        void commit() noexcept;
        uint64_t projectile_visual_tail() const noexcept;
        PreparedLifetimes(const PreparedLifetimes&)=delete;
        PreparedLifetimes& operator=(const PreparedLifetimes&)=delete;
    private:
        friend class NativeWorld;
        struct State;
        explicit PreparedLifetimes(std::unique_ptr<State>);
        std::unique_ptr<State> state_;
    };
    // Owns the index lock until commit/discard. No world access or native calls
    // may occur while a candidate is held. Discard leaves identities/epoch intact.
    std::unique_ptr<PreparedLifetimes> prepare_lifetimes(const std::vector<uint8_t>&,uint64_t fresh_epoch,std::string& error);
    bool load_lifetimes(const std::vector<uint8_t>&,uint64_t fresh_epoch,std::string& error);
private:
    NativeWorldSample capture(uint64_t tick,bool bindings_only) const;
    struct Impl;std::shared_ptr<Impl> impl_;uint64_t subscription_=0;
};
}
