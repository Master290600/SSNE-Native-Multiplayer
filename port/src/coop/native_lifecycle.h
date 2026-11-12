#pragma once
#include "world_state.h"
#include "ssne/ppc_context.h"
#include <chrono>
namespace port::coop {
bool validate_native_detached_rotor(const EntityState&,std::string&);
uint32_t create_native_detached_rotor(PPCContext&,const EntityState&,std::string&);
inline constexpr std::array<const char*,8> native_preference_keys{"AutoAimType","AutoLookRecentre","AutoWeaponSelect","InvertLook","HorizontalLookSpeed","VerticalLookSpeed","LookStyle","PauseWeaponWheel"};
std::array<uint8_t,8> capture_native_preferences(uint32_t actor);
bool apply_native_preferences(PPCContext&,uint32_t actor,const std::array<uint8_t,8>&,std::string&);
bool refresh_native_aim_preferences(PPCContext&,uint32_t actor,bool presentation,std::string&);
bool capture_native_combo_hud(uint32_t actor,EntityState&,std::string&);
bool validate_native_combo_hud(uint32_t actor,const EntityState&,std::string&);
bool apply_native_combo_hud(PPCContext&,uint32_t actor,const EntityState&,std::string&);
bool capture_native_reentry_hud(uint32_t actor,EntityState&,std::string&);
bool validate_native_reentry_hud(uint32_t actor,const EntityState&,std::string&);
bool apply_native_reentry_hud(PPCContext&,uint32_t actor,const EntityState&,std::string&);
bool capture_native_weapon_bar(uint32_t actor,EntityState&,std::string&);
bool validate_native_weapon_bar(uint32_t actor,const EntityState&,std::string&);
bool apply_native_weapon_bar(PPCContext&,uint32_t actor,const EntityState&,std::string&);
bool capture_native_weapon_sidebar(uint32_t actor,EntityState&,std::string&);
bool validate_native_weapon_sidebar(uint32_t actor,const EntityState&,std::string&);
bool apply_native_weapon_sidebar(PPCContext&,uint32_t actor,const EntityState&,std::string&);
bool capture_native_model_states(std::vector<int64_t>&,std::string&);
bool validate_native_model_states(const std::vector<int64_t>&,std::string&);
bool apply_native_model_states(PPCContext&,const std::vector<int64_t>&,std::string&);
bool validate_native_marker(uint32_t object,uint32_t definition,int64_t index,int64_t active,std::string& error);
bool apply_native_marker(PPCContext&,uint32_t object,bool active,std::string& error);
bool validate_native_arrow_animation(uint32_t object,const std::vector<float>& animation,std::string& error);
bool apply_native_arrow_animation(PPCContext&,uint32_t object,const std::vector<float>& animation,std::string& error);
bool refresh_native_player_hud(PPCContext&,uint32_t actor,std::string& error);
bool capture_native_interaction_hint(uint32_t actor,EntityState&,std::string& error);
bool validate_native_interaction_hint(uint32_t actor,const EntityState&,std::string& error);
bool apply_native_interaction_hint(PPCContext&,uint32_t actor,const EntityState&,std::string& error);
bool capture_native_player_notices(uint32_t actor,EntityState&,std::string& error);
bool validate_native_player_notices(uint32_t actor,const EntityState&,std::string& error);
bool apply_native_player_notices(PPCContext&,uint64_t epoch,uint32_t actor,const EntityState&,std::string& error);
bool validate_native_destructible(uint32_t object,const EntityState&,std::string& error);
bool apply_native_destructible(PPCContext&,uint32_t object,const EntityState&,std::string& error);
struct NativeHudPresentationClock {
    std::chrono::steady_clock::time_point time{};
    uint64_t frame=0;
    bool initialized=false;
};
bool advance_native_player_huds(PPCContext&,const std::vector<uint32_t>& actors,
    NativeHudPresentationClock&,uint64_t frame,std::chrono::steady_clock::time_point now,std::string& error);
bool refresh_native_bomb_ammo_hud(PPCContext&,uint32_t actor,std::string& error);
bool prepare_native_weapon_model(PPCContext&,uint32_t actor,unsigned weapon,unsigned instance,std::string& error);
bool validate_native_powerups(uint32_t,const std::vector<float>&,std::string&);
bool apply_native_powerups(PPCContext&,uint32_t,const std::vector<float>&,std::string&);
bool refresh_native_weapon_poses(PPCContext&,std::string& error);
bool apply_native_weapon_flash(PPCContext&,uint32_t actor,unsigned weapon,unsigned hand,const std::vector<float>&,uint32_t mask,std::string& error,unsigned effect=0);
bool apply_native_body_flash(PPCContext&,uint32_t actor,unsigned hand,const std::vector<float>&,uint32_t mask,std::string& error);
bool update_native_muzzle_flashes(PPCContext&,float seconds,std::string& error);
bool refresh_native_actor_pose(PPCContext&,uint32_t actor,EntityKind,std::string& error);
bool validate_native_corpse_creation(const EntityState&,std::string& error);
bool apply_native_corpse_lifecycle(PPCContext&,uint32_t,const EntityState&,std::string& error);
bool capture_native_corpse_visual(PPCContext&,uint32_t,EntityState&,std::string& error);
uint32_t create_native_corpse(PPCContext&,const EntityState&,std::string& error);
bool set_native_script_enabled(PPCContext&,uint32_t object,bool enabled,std::string& error);
bool activate_native_brush(PPCContext&,uint32_t object,std::string& error);
bool validate_native_brush_geometry(uint32_t object,int64_t resource,int64_t segment,float progress,std::string& error);
bool apply_native_brush_geometry(PPCContext&,uint32_t object,int64_t resource,int64_t segment,float progress,std::string& error);
bool validate_native_weapon_selection(uint32_t actor,unsigned weapon,std::string& error);
bool apply_native_weapon_selection(PPCContext&,uint32_t actor,unsigned weapon,bool available,std::string& error);
// Committed fire/ammo modes, one per native hand; -1 denotes no instance.
// Inventory banks describe hands, not normal versus alternate ammunition.
bool capture_native_weapon_modes(uint32_t actor,unsigned weapon,std::vector<int64_t>&,std::string& error);
bool validate_native_weapon_modes(uint32_t actor,unsigned weapon,const std::vector<int64_t>&,std::string& error);
bool apply_native_weapon_modes(PPCContext&,uint32_t actor,unsigned weapon,const std::vector<int64_t>&,std::string& error);
bool native_player_fire_origin(PPCContext&,uint32_t actor,std::array<float,3>& origin,std::string& error);
bool native_player_fire_direction(PPCContext&,uint32_t actor,std::array<float,3>& direction,std::string& error);
bool set_native_afk_manual_aim(PPCContext&,uint32_t actor,bool active,std::string& error);
bool prepare_native_afk_manual_aim(std::string& error);
void clear_native_afk_manual_aim();
std::map<uint32_t,uint32_t> native_afk_manual_aim_bindings();
bool validate_native_afk_manual_aim_bindings(const std::map<uint32_t,uint32_t>&,std::string& error);
bool restore_native_afk_manual_aim_bindings(std::map<uint32_t,uint32_t>,std::string& error);
std::optional<int> native_owned_primary_ammo(PPCContext&,uint32_t actor,unsigned weapon,std::string& error);
bool request_native_primary_ammo(PPCContext&,uint32_t actor,unsigned weapon,std::string& error);
bool apply_native_camera_control(PPCContext&,uint32_t actor,bool frozen,std::string& error);
bool prepare_native_player_body(PPCContext&,uint32_t actor,const EntityState&,std::string& error);
bool apply_native_body_weapon_resource(PPCContext&,uint32_t actor,const EntityState&,std::string& error);
bool validate_native_movement_mode(uint32_t actor,unsigned mode,std::string& error);
bool apply_native_movement_mode(PPCContext&,uint32_t actor,unsigned mode,std::string& error);
bool validate_native_npc_piece(const EntityState&,std::string& error);
uint32_t create_native_npc_piece(PPCContext&,const EntityState&,std::string& error);
bool validate_native_boss_parts(const EntityState&,std::string&);
bool capture_native_boss_parts(uint32_t,EntityState&,std::string&);
bool apply_native_boss_parts(uint32_t,const EntityState&,std::string&);
bool validate_native_enemy_creation(const EntityState&,std::string& error);
uint32_t create_native_enemy(PPCContext&,const EntityState&,std::string& error);
// PAL factory8009DD48 definitions and original initialization/visual storage.
// Weapon numbers are not projectile definitions (Minigun mode1 creates9).
struct NativeProjectileClass { unsigned definition; uint32_t vtable,size,initialize,trail,destructor; };
const NativeProjectileClass* native_projectile_class(unsigned definition);
const NativeProjectileClass* native_projectile_class_by_vtable(uint32_t vtable);
std::vector<uint32_t> native_projectile_extra_offsets(unsigned definition);
inline constexpr std::array<uint32_t,12> native_energy_visual_offsets{0x124,0x128,0x12C,0x138,0x13C,0x140,0x14C,0x150,0x154,0x170,0x174,0x178};
bool validate_native_projectile_creation(const EntityState&,std::string& error);
uint32_t create_native_projectile(PPCContext&,const EntityState&,uint32_t source,std::string& error);
bool present_native_projectile(PPCContext&,uint32_t object,std::string& error);
bool validate_native_rocket_creation(const EntityState&,std::string& error);
bool validate_native_weapon_pickup_creation(const EntityState&,std::string& error);
bool capture_native_pickup_metadata(uint32_t object,EntityState&,std::string& error);
bool validate_native_pickup_metadata(const EntityState&,std::string& error);
bool match_native_pickup_metadata(uint32_t object,const EntityState&,std::string& error);
uint32_t create_native_weapon_pickup(PPCContext&,const EntityState&,std::string& error);
uint32_t create_native_rocket(PPCContext&,const EntityState&,uint32_t source,std::string& error);
bool present_native_rocket(PPCContext&,uint32_t object,std::string& error);
bool validate_native_removal(const PPCContext&,uint32_t object,std::string& error);
bool remove_native_entity(PPCContext&,uint32_t object,std::string& error);
bool remove_native_entity(PPCContext&,uint32_t object,std::string& error,EntityKind presentation);
}
