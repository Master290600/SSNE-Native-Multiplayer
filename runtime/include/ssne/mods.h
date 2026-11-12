#pragma once
#include "ssne/bot.h"
#include "ssne/ppc_context.h"
#include <string>
#include <vector>
namespace ssne {
struct ModOptions {
  bool god = false, ammo = false, weapons = false, no_enemies = false;
  bool aimbot = false, esp = false, aim_hold = true;
  bool esp_names = true, esp_distance = true;
  bool auto_aim_height = true;
  bool esp_health = true, rapid_fire = false;
  float aim_fov = 25, aim_speed = 12, range = 150, aim_height = 1;
  float fire_multiplier = 3;
  bool item_esp = false;
  bool item_weapons = true, item_health = true, item_armor = true,
       item_secrets = true;
  bool bone_esp = false, bone_names = false;
  int aim_point = 0,
      aim_bone = 0; // 0 body, 1 nearest bone, 2 head, 3 torso, 4 index.
};
struct ModTemplate {
  unsigned id;
  bool enemy;
  std::string name;
  unsigned type =
      0; // Enemy class ID, or resolved pickup ID (not the level prototype ID).
};
struct ModCatalogEntry {
  unsigned type;
  bool enemy, weapon;
  std::string name;
  const char *availability_note =
      nullptr; // Non-pickup weapon configurations have no spawn template ID.
};
const std::vector<ModCatalogEntry> &mod_catalog();
std::string mod_type_name(bool enemy, unsigned type);
struct ModState {
  bool available = false, active = false;
  ModOptions options;
  std::vector<ModTemplate> templates;
  std::string status;
  bool targeting_available = false;
  unsigned tracked_enemies = 0;
};
struct ModMarker {
  float x, y, distance; // Normalized coordinates in the game image.
  unsigned type;
  bool target;
  float health = 0, max_health = 0;
};
enum class ModItemKind { Weapon, Health, Armor, Secret };
struct ModItemMarker {
  float x, y, distance;
  unsigned type;
  ModItemKind kind;
};
struct ModBoneMarker {
  float x, y;
  unsigned enemy, index;
  std::string name;
  bool target = false;
  int parent = -1; // Recognized anatomical-name link; absent for unknown names.
};
std::vector<ModItemMarker> mod_item_markers();
std::vector<ModBoneMarker> mod_bone_markers();
std::vector<ModMarker> mod_markers();
void mod_aim_input(bool held,
                   bool enabled); // Host input; menus suspend targeting.
// Host startup rule; replicas always create only the received actors.
void set_enemy_multiplier(unsigned value);
unsigned enemy_multiplier();
void install_mod_hooks();
void mod_tick(PPCContext &, unsigned player);
ModState mod_state();
ModOptions mod_current_options();
void mod_options(ModOptions);
void mod_netplay_player(int port);
void mod_local_options(ModOptions);
void mod_shared_options(uint32_t flags, float fire_multiplier);
void mod_take_netplay_aim(double& yaw, double& pitch);
void mod_spawn(unsigned id, int count, float distance);
void mod_spawn_type(bool enemy, unsigned type, int count, float distance);
// Hidden native integration fixtures use the original factory at a real player.
void mod_test_pickup_at_player(unsigned type,unsigned slot);
void mod_test_enemy_at_player(unsigned type,unsigned slot,int count,float distance);
uint64_t netplay_world_checksum();
void netplay_log_world_state();
} // namespace ssne
