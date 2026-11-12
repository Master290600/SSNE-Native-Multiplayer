#pragma once
#include "ssne/ppc_context.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>
namespace ssne {
// Automatic weapons have native ray invocations, not individual sound events. Their
// IDs occupy a separate positive signed-64-bit namespace; bit0 is the hand.
inline constexpr uint64_t native_automatic_ray_tag=uint64_t(1)<<62;
inline constexpr uint64_t native_uzi_ray_tag=native_automatic_ray_tag;
struct NativeHitIdentity {uint64_t entity=0,generation=0;};
// A confirmation observes original host damage. It never asks a replica to
// subtract health; authoritative world state remains the only health source.
// Source/target are NativeWorld lifetime IDs, including already retired targets.
// generation belongs to the source player's connection, not the victim.
struct NativeConfirmedHit {uint64_t sequence=0,shot=0,source=0,target=0,generation=0;unsigned slot=0,weapon=1,pellet=0;float before=0,after=0,amount=0;};
void set_native_hit_identity_resolver(std::function<NativeHitIdentity(uint32_t)>);
std::vector<NativeConfirmedHit> native_confirmed_hit_history();
bool validate_native_confirmed_hit(const NativeConfirmedHit&,std::string& error);
struct NativeRocketEffect {uint64_t sequence=0;unsigned style=0;std::array<float,10> values{};};
struct NativeWeaponShot {uint64_t sequence=0,order=0;unsigned slot=0;std::array<float,3> position{};unsigned weapon=6,cue_sound=0,cue_flags=1;};
struct NativeWeaponLoop {uint64_t sequence=0,order=0;unsigned slot=0,lane=0,weapon=2;uint64_t origin=0;bool active=false;uint32_t duration_ms=0,elapsed_ms=0;std::array<float,3> position{};};
struct NativeBulletImpact {uint64_t sequence=0;std::array<uint32_t,5> tags{};std::array<float,9> vectors{};};
// Original 800EE020 line effect: world-space start/end and its native view mask.
// It exists for one rendered frame and is independent of a collision impact.
// World endpoints/masks remain authoritative. view[0]==owner slot+1 carries
// the firing camera for that owner's local view; unowned world streaks use0.
struct NativeBulletTrail {uint64_t sequence=0;uint32_t mask=UINT32_MAX;std::array<float,6> vectors{};std::array<float,13> view{};};
std::optional<std::array<float,12>> replica_bullet_trail_view(uint32_t proxy);
// Original flame particle state, excluding all guest pointers and list links.
// tags: mode, emitter index, view mask, collision kind, collision enabled.
// values: position3, velocity3, collision point3, normal3, scale, age, max age,
// rotation and rotation sign, captured after the original native factory.
struct NativeWeaponEmitter {uint64_t sequence=0;std::array<uint32_t,5> tags{};std::array<float,17> values{};};
// Original casing matrix12, velocity3, acceleration3 and age.
struct NativeWeaponCasing {uint64_t sequence=0;unsigned slot=0,weapon=1,type=0;uint32_t mask=UINT32_MAX;std::array<float,19> values{};};
// Additional native weapon surface marks: position3, normal3 and scale.
struct NativeWeaponSurface {uint64_t sequence=0;unsigned weapon=0,kind=7;std::array<float,7> values{};};
// RAPTOR endpoints, or Sirian muzzle/endpoint/secondary endpoints and length.
// flags bit0 active (zero stops), bit1 secondary; hand selects native instance.
struct NativeWeaponBeam {uint64_t sequence=0;unsigned slot=0,weapon=8,hand=0,flags=1;std::array<float,13> values{};};
// Producer acknowledgement is local to the host journal, independent of each
// remote consumer's acknowledgement. Snapshot all streams in one lock.
// Ephemeral UI/audio: kind0 positional sound, kind1 floating score; kind2 bomb
// flash, kind6 ordinary splatter, kind7 hurt flash, kind8 owned combo SFX
// (code0 stops its sustained cue), kind9 owned localized combo announcement,
// kind10 material-selected footstep (owner slot and original world position).
// HUD flash uses code=target slot, variant=0, values[0..1]=duration/strength.
struct NativeFeedbackEvent {uint64_t sequence=0;uint32_t kind=0,code=0,variant=0;std::array<float,12> values{};};
bool validate_native_feedback(const NativeFeedbackEvent&,std::string&);
bool play_native_feedback(PPCContext&,const NativeFeedbackEvent&,std::string&);
struct NativeEventProducerSnapshot {
    uint64_t epoch=0;
    std::array<uint64_t,11> cursors{};
    std::vector<NativeConfirmedHit> hits;
    std::vector<NativeRocketEffect> rockets;
    std::vector<NativeWeaponShot> shots;
    std::vector<NativeWeaponLoop> loops;
    std::vector<NativeBulletImpact> impacts;
    std::vector<NativeFeedbackEvent> feedback;
    std::vector<NativeBulletTrail> trails;
    std::vector<NativeWeaponEmitter> emitters;
    std::vector<NativeWeaponCasing> casings;
    std::vector<NativeWeaponSurface> surfaces;
    std::vector<NativeWeaponBeam> beams;
};
using NativeProducerCursors=std::array<uint64_t,11>; // hits, rockets, shots, loops, impacts, feedback, trails, emitters, casings, surfaces, beams
// Host safe-point component; restore matching RAM before loading. Function
// pointers, observers and the current identity resolver remain process-local.
bool native_effect_save_state(std::vector<uint8_t>&,std::string& error);
bool native_effect_load_state(const std::vector<uint8_t>&,std::string& error);
// Historical saves rebind attribution by saved party slot. Sources whose
// participant is absent/retired lose attribution, not native physical damage.
void rebind_native_effect_sources(const std::array<uint64_t,8>& saved,const std::array<uint64_t,8>& live);
struct NativeEffectCheckpointStats {uint64_t shots=0,hits=0;size_t rockets=0,queued=0,loops=0;};
NativeEffectCheckpointStats native_effect_checkpoint_stats();
bool native_effect_checkpoint_probe(std::string& error); // opt-in native test fixture
NativeEventProducerSnapshot native_event_producer_snapshot();
bool acknowledge_native_event_publication(uint64_t producer_epoch,const NativeProducerCursors&);
std::vector<NativeBulletImpact> native_bullet_impact_history();
bool validate_native_bullet_impact(const NativeBulletImpact&,std::string& error);
bool play_native_bullet_impact(PPCContext&,const NativeBulletImpact&,std::string& error);
std::vector<NativeBulletTrail> native_bullet_trail_history();
bool validate_native_bullet_trail(const NativeBulletTrail&,std::string& error);
// Accept into the bounded presentation queue. The frame updater invokes the
// original allocator and retires its proxies only after a rendered frame.
bool play_native_bullet_trail(PPCContext&,const NativeBulletTrail&,std::string& error);
std::vector<NativeWeaponEmitter> native_weapon_emitter_history();
bool validate_native_weapon_emitter(const NativeWeaponEmitter&,std::string& error);
bool play_native_weapon_emitter(PPCContext&,const NativeWeaponEmitter&,std::string& error);
std::vector<NativeWeaponCasing> native_weapon_casing_history();
bool validate_native_weapon_casing(const NativeWeaponCasing&,std::string& error);
bool play_native_weapon_casing(PPCContext&,const NativeWeaponCasing&,std::string& error);
std::vector<NativeWeaponSurface> native_weapon_surface_history();
bool validate_native_weapon_surface(const NativeWeaponSurface&,std::string& error);
bool play_native_weapon_surface(PPCContext&,const NativeWeaponSurface&,std::string& error);
std::vector<NativeWeaponBeam> native_weapon_beam_history();
bool validate_native_weapon_beam(const NativeWeaponBeam&,std::string& error);
bool play_native_weapon_beam(PPCContext&,const NativeWeaponBeam&,std::string& error);
// W11 sustained presentation: model/controller existence and visibility,
// model matrix12, controller position3/radius, resource UV matrix12.
bool capture_native_bomb_visual(uint32_t actor,unsigned hand,std::vector<float>&,std::array<uint32_t,2>& masks,std::string& error);
bool validate_native_bomb_visual(const std::vector<float>&,const std::array<uint32_t,2>& masks,std::string& error);
bool apply_native_bomb_visual(PPCContext&,uint32_t actor,unsigned hand,const std::vector<float>&,const std::array<uint32_t,2>& masks,std::string& error);
bool install_native_rocket_effect_capture(bool authoritative,std::string& error);
void disable_native_rocket_effect_capture();
std::vector<NativeRocketEffect> native_rocket_effect_history();
std::vector<NativeWeaponShot> native_weapon_shot_history();
std::vector<int64_t> native_weapon_cue_state();
bool reconcile_native_weapon_cues(PPCContext&,const std::vector<int64_t>&,std::string& error);
bool validate_native_weapon_shot(const NativeWeaponShot&,std::string& error);
bool play_native_weapon_shot(PPCContext&,const NativeWeaponShot&,bool local_player,std::string& error);
void native_test_fire_request(bool);
bool native_test_fire_requested();
std::vector<NativeWeaponLoop> native_weapon_loop_history();
std::vector<NativeWeaponLoop> native_weapon_active_loops();
bool validate_native_weapon_loop(const NativeWeaponLoop&,std::string& error);
bool apply_native_weapon_loop(PPCContext&,const NativeWeaponLoop&,bool local_player,std::string& error);
bool update_native_weapon_loops(PPCContext&,std::string& error);
// Stop the old sustained weapon only; preserves other actors and one-shot cues.
bool stop_native_weapon_loops_for_actor(PPCContext&,uint32_t actor,unsigned weapon,std::string& error);
bool stop_native_weapon_loops(PPCContext&,std::string& error);
bool validate_native_rocket_effect(const NativeRocketEffect&,std::string& error);
bool play_native_rocket_effect(PPCContext&,const NativeRocketEffect&,std::string& error);
// Def6 charge/flight -> explosion: reuse original model lifecycle while skipping
// only its verified host-owned area damage message at return address 80072380.
bool play_native_energy_transition(PPCContext&,uint32_t object,std::string& error);
bool update_native_replica_effects(PPCContext&,float seconds,std::string& error);
struct NativePickupNotice {uint64_t sequence=0;unsigned flags=0;std::string text;};
NativePickupNotice native_pickup_notice(uint32_t actor);
// Original visual-only shared flame textures, pickup meshes and sparkle emitters.
bool update_native_pickup_ambient(PPCContext&,float seconds,std::string& error);
}
