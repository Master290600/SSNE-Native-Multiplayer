#include "ssne/mods.h"
#include "ssne/ammo_rules.h"
#include "ssne/replica_simulation.h"
#include <atomic>
#include "ssne/native_party.h"
#include "ssne/native_entity.h"
#include "ssne/native_pickup.h"
#include "ssne/game_addresses.h"
#include "ssne/hw.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>

#include <unordered_map>
namespace ssne {
// USA Rev 1: AI GetDefinition() IDs identify enemy classes. Display names are
// checked against EnglishUSA.tdb; Hydra heads and the ant leader retain their
// component/variant names.
const std::vector<ModCatalogEntry> &mod_catalog() {
  static const std::vector<ModCatalogEntry> entries = {
      {0, true, false, "Kamikaze Marine"},
      {1, true, false, "Kamikaze Marine MK II"},
      {2, true, false, "Mini Sam (internal)"},
      {5, true, false, "Sirian Werebull"},
      {6, true, false, "Reeban Tooth Fish"},
      {7, true, false, "Mecha-Arachnoid"},
      {8, true, false, "Auldran Reptiloid - Common"},
      {9, true, false, "Auldran Reptiloid - Highlander"},
      {10, true, false, "Bio-Mechanoid - Minor"},
      {11, true, false, "Bio-Mechanoid - Major"},
      {12, true, false, "Sirian Beheaded Rocketeer"},
      {13, true, false, "Sirian Beheaded Firecracker"},
      {14, true, false, "Sirian Beheaded Bomberman"},
      {15, true, false, "Sirian Werebull - Juvenile"},
      {16, true, false, "Dib Dib Dum Dum"},
      {17, true, false, "Dum Dum"},
      {18, true, false, "Kleer Knight"},
      {19, true, false, "Witch Harpy"},
      {20, true, false, "Tweedle Dum Dum"},
      {21, true, false, "Dum Dum - Juvenile"},
      {22, true, false, "Porcine Berserker"},
      {23, true, false, "Legionnaire Ant"},
      {24, true, false, "Chariot Harpy"},
      {25, true, false, "Devil Stallion"},
      {26, true, false, "Elephant Gunner"},
      {27, true, false, "Legionnaire Ant - Leader"},
      {28, true, false, "Wicker Man"},
      {29, true, false, "Temple Guardian"},
      {30, true, false, "Leikung Warrior"},
      {31, true, false, "Phoenix Bomber"},
      {32, true, false, "Monkey Zombie"},
      {33, true, false, "Guandi Sorcerer"},
      {34, true, false, "Octochop"},
      {35, true, false, "Atlantean"},
      {36, true, false, "Carcinus Lurker"},
      {37, true, false, "Killermari"},
      {38, true, false, "Killermari Rider"},
      {39, true, false, "Atlantean Depthlurker"},
      {40, true, false, "Diablotaur"},
      {41, true, false, "Hydra - Drill Head"},
      {42, true, false, "Sirian Dark Lord"},
      {43, true, false, "Hydra - Fire Head"},
      {44, true, false, "Hydra - Cannon Head"},
      {45, true, false, "Subterranean Emperor Hydra - Body"},
      // GetPickupIdOfCurrentlySelectedWeapon maps weapon slots to these pickup
      // IDs.
      {35, false, true, "Bonecracker P-LAH Chainsaw"},
      {36, false, true, "Shofield Desert Hawk"},
      {37, false, true, "Shofield Uzi Pistols"},
      {38, false, true, "Double Barreled Shotgun"},
      {39, false, true, "XM4000 Mini Gun"},
      {40, false, true, "XPML4000 Rocket Launcher"},
      {41, false, true, "MK III Grenade Launcher"},
      {42, false, true, "XOP Gas Gun"},
      {43, false, true, "RAPTOR 16mm Sniper Rifle"},
      {44, false, true, "Sirian Power Gun"},
      {45, false, true, "Serious Cannon"},
      {55, false, true, "Serious Bomb"},
      {~0u, false, true, "Jeep Minigun", "vehicle weapon"},
      {~0u, false, true, "Jeep Rocket Launcher", "vehicle weapon"},
      {~0u, false, true, "Combine Blade", "vehicle weapon"},
      {~0u, false, true, "Submarine Minigun", "vehicle weapon"},
      {~0u, false, true, "Submarine Rocket Launcher / Torpedoes",
       "vehicle weapon"},
      {~0u, false, true, "Missile Launcher (internal configuration)",
       "no regular pickup"},
      // Pickups use the configuration keys in base.cfg and the native pickup
      // tables.
      {0, false, false, "Health +5"},
      {1, false, false, "Health +10"},
      {2, false, false, "Health +25"},
      {3, false, false, "Health +50"},
      {4, false, false, "Health +100"},
      {5, false, false, "Armor +5"},
      {6, false, false, "Armor +25"},
      {7, false, false, "Armor +50"},
      {8, false, false, "Armor +100"},
      {9, false, false, "Armor +200"},
      {10, false, false, "Serious Skates"},
      {11, false, false, "Serious Invulnerability"},
      {12, false, false, "Serious Damage"},
      {13, false, false, "Serious Smartbomb"},
      {14, false, false, "Serious Bubblegum (internal)"},
      {15, false, false, "Serious Ring"},
      {16, false, false, "Serious Glasses"},
      {17, false, false, "Green Ammopack"},
      {18, false, false, "Red Ammopack"},
      {19, false, false, "9mm Bullets"},
      {20, false, false, "Homing Bullets"},
      {21, false, false, "Ricochet Bullets"},
      {22, false, false, "Sniper Bullets"},
      {23, false, false, "Shotgun Shells"},
      {24, false, false, "Napalm"},
      {25, false, false, "Liquid Nitrogen"},
      {26, false, false, "Laughing Gas"},
      {27, false, false, "Grenades"},
      {28, false, false, "Limpet Grenades"},
      {29, false, false, "Spider Mines"},
      {30, false, false, "Rockets"},
      {31, false, false, "Heat Seeking Rockets"},
      {32, false, false, "Sonic Rockets"},
      {33, false, false, "Power Cells"},
      {34, false, false, "Cannon Balls"}};
  return entries;
}
std::string mod_type_name(bool enemy, unsigned type) {
  for (const auto &entry : mod_catalog())
    if (!entry.availability_note && entry.enemy == enemy && entry.type == type)
      return entry.name;
  if (!enemy && type >= 46 && type <= 54)
    return "Quest item (type " + std::to_string(type) + ")";
  return std::string(enemy ? "Unknown enemy" : "Unknown pickup") + " (type " +
         std::to_string(type) + ")";
}
namespace {
std::recursive_mutex lock;
ModState state;
// Prototype pointers belong to the current level; only the placement matrix is
// copied.
struct Recipe {
  unsigned id, level, kind, proto, flag, type;
  bool enemy;
  std::array<unsigned, 12> matrix;
};
std::vector<Recipe> recipes;
unsigned next_id = 1;
struct Request {
  unsigned id;
  int count;
  float distance;
  int source_slot=-1;
};
std::vector<Request> requests;
PPCFunc item_fn = nullptr, enemy_fn = nullptr, exit_fn = nullptr;
bool replaying = false;
std::atomic<unsigned> spawn_multiplier{1};
thread_local bool multiplying_enemy=false;
void clear_overrides();
auto last_tick = std::chrono::steady_clock::time_point{};
bool valid(unsigned p, unsigned n = 4) {
  return p >= 0x80000000u && n <= guest_ram_size() && p <= guest_ram_end() - n;
}
struct BoneInfo {
  unsigned index;
  std::string name;
  int parent = -1;
  std::array<float, 3> bind_center{};
  bool has_center = false;
};
struct Tracked {
  unsigned type, vtable;
  bool registered = false;
  float center = std::numeric_limits<float>::quiet_NaN();
  float max_health = 0;
  unsigned model = 0, resource = 0, bone_count = 0;
  std::vector<BoneInfo> bones;
};
std::unordered_map<unsigned, Tracked> enemies;
std::vector<ModMarker> markers;
struct TrackedItem {
  unsigned type, vtable;
  ModItemKind kind;
};
std::unordered_map<unsigned, TrackedItem> items;
std::vector<ModItemMarker> item_markers;
std::vector<ModBoneMarker> bone_markers;
PPCFunc secret_fn, secret_destroy_fn;
void track_item(unsigned object) {
  if (!valid(object, 0x18C) || items.size() >= 8192)
    return;
  unsigned vt = mem_read32(object + 0x48), type = mem_read32(object + 0x180);
  if (vt != game_address(0x80213D48) && vt != game_address(0x80214110) && vt != game_address(0x802144C8))
    return;
  if (type <= 4)
    items[object] = {type, vt, ModItemKind::Health};
  else if (type <= 9)
    items[object] = {type, vt, ModItemKind::Armor};
  else if ((type >= 35 && type <= 45) || type == 55)
    items[object] = {type, vt, ModItemKind::Weapon};
}
std::array<float, 12> view{};
float proj_x = 0, proj_y = 0, native_proj_x = 0, native_proj_y = 0;
int local_netplay_port = -1;
double pending_aim_yaw = 0, pending_aim_pitch = 0;
void update_targets(unsigned player, float dt);
bool aim_held = false, aim_enabled = false;
unsigned target = 0;
auto camera_time = std::chrono::steady_clock::time_point{};
PPCFunc create_fn, register_fn, remove_fn, camera_fn;
PPCFunc bounds_fn;
void original(unsigned addr, PPCFunc fn, PPCFunc hook, PPCContext &c) {
  ssne_call_original(c, addr, fn);
}
void create_hook(PPCContext &c) {
  unsigned type = c.r[3];
  original(0x8008A558, create_fn, create_hook, c);
  notify_native_entity({NativeEntityNotice::Event::CreatedActor,c.r[3],type});
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (valid(c.r[3], 0xD4) && type != 2 && enemies.size() < 4096)
    enemies[c.r[3]] = {type, mem_read32(c.r[3] + 0x48), false};
}
void register_hook(PPCContext &c) {
  unsigned object = c.r[4];
  int32_t group=-2;
  if(valid(object,0x4C)) {
    const auto table=mem_read32(object+0x48);
    if(valid(table,0x38)) {
      const auto getter=mem_read32(table+0x34);
      if(valid(getter,8) && (mem_read32(getter)&0xFFFF0000u)==0x38600000u && mem_read32(getter+4)==0x4E800020u)
        group=int16_t(mem_read32(getter));
    }
  }
  original(game_address(0x80150640), register_fn, register_hook, c);
  notify_native_entity({NativeEntityNotice::Event::Registered,object,0,group});
  std::lock_guard<std::recursive_mutex> guard(lock);
  track_item(object);
  if (auto it = enemies.find(object); it != enemies.end()) {
    it->second.registered = true;
    float health = u2f(mem_read32(object + 0xCC));
    if (std::isfinite(health) && health > 0)
      it->second.max_health = std::max(it->second.max_health, health);
  }
}
void remove_hook(PPCContext &c) {
  notify_native_entity({NativeEntityNotice::Event::Removing,c.r[4]});
  {
    std::lock_guard<std::recursive_mutex> guard(lock);
    enemies.erase(c.r[4]);
    items.erase(c.r[4]);
  }
  original(game_address(0x8015070C), remove_fn, remove_hook, c);
}
void secret_hook(PPCContext &c) {
  unsigned object = c.r[4];
  original(game_address(0x8010C228), secret_fn, secret_hook, c);
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (valid(object, 0xA8) && items.size() < 8192) {
    unsigned desc = mem_read32(object + 0x54);
    if (valid(desc, 0x80) && mem_read32(desc + 0x7C) == 1)
      items[object] = {~0u, mem_read32(object + 0x48), ModItemKind::Secret};
  }
}
void secret_destroy_hook(PPCContext &c) {
  {
    std::lock_guard<std::recursive_mutex> guard(lock);
    items.erase(c.r[3]);
  }
  original(0x8008462C, secret_destroy_fn, secret_destroy_hook, c);
}
void bounds_hook(PPCContext &c) {
  unsigned object = c.r[3], sp = c.r[1];
  original(0x80090580, bounds_fn, bounds_hook, c);
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto it = enemies.find(object);
  if (it == enemies.end() || sp < 0x80000050 || !valid(sp - 0x50, 0x50))
    return;
  float low = u2f(mem_read32(sp - 0x50 + 0xC)),
        high = u2f(mem_read32(sp - 0x50 + 0x1C));
  float height = u2f(mem_read32(object + 0xD0));
  // These are the native bounding-box outputs still in the completed init
  // frame. Validate them against the height the game itself recorded before
  // caching.
  if (std::isfinite(low) && std::isfinite(high) && std::abs(low) < 200 &&
      std::abs(high) < 200 && height > .05f && height < 200 &&
      std::abs((high - low) - height) < .001f)
    it->second.center = (low + high) * .5f;
}
void camera_hook(PPCContext &c) {
  int camera_port = valid(c.r[6],8) && u2f(mem_read32(c.r[6]+4))>.001f ? 1 : 0;
  if(local_netplay_port>=0)camera_port=int32_t(mem_read32(c.r[13]-0x6D94));
  const auto input_camera=c.r[3],input_mode=c.r[4];
  original(game_address(0x8014C090), camera_fn, camera_hook, c);
  if(getenv("SSNE_COOP_RENDER_TRACE") && netplay_render_local() && !netplay_render_discard()) {
    static std::array<uint64_t,32> visits{};
    const auto slot=mem_read32(c.r[13]-0x6D94);
    if(slot<32 && ++visits[slot]%120==0) {
      const auto matrix=game_address(0x80291A54),projection=game_address(0x80291BE0);
      char message[450];snprintf(message,sizeof(message),"[coop-active-projection] view=%u camera=%08X mode=%u basis0=%.5f,%.5f,%.5f basis1=%.5f,%.5f,%.5f basis2=%.5f,%.5f,%.5f projection=%.5f,%.5f,%.5f,%.5f",slot,input_camera,input_mode,u2f(mem_read32(matrix)),u2f(mem_read32(matrix+4)),u2f(mem_read32(matrix+8)),u2f(mem_read32(matrix+16)),u2f(mem_read32(matrix+20)),u2f(mem_read32(matrix+24)),u2f(mem_read32(matrix+32)),u2f(mem_read32(matrix+36)),u2f(mem_read32(matrix+40)),u2f(mem_read32(projection)),u2f(mem_read32(projection+20)),u2f(mem_read32(projection+40)),u2f(mem_read32(projection+44)));log_message(message);
    }
  }
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (local_netplay_port>=0 && (!netplay_world_ready() || camera_port!=local_netplay_port)) return;
  if (!state.options.aimbot && !state.options.esp && !state.options.item_esp &&
      !state.options.bone_esp) {
    markers.clear();
    item_markers.clear();
    bone_markers.clear();
    return;
  }
  // The scene camera publishes its final GX view and perspective matrices here,
  // including native zoom, head movement, host FOV and widescreen adjustments.
  for (unsigned i = 0; i < 12; i++)
    view[i] = u2f(mem_read32(game_address(0x80291A54) + i * 4));
  proj_x = u2f(mem_read32(game_address(0x80291BE0)));
  proj_y = u2f(mem_read32(game_address(0x80291BE0) + 20));
  native_proj_x=proj_x;native_proj_y=proj_y;
  camera_time = std::chrono::steady_clock::now();
  if (local_netplay_port>=0) {
    float scale=fov()>0 ? std::tan(63.f*.008726646f)/std::tan(fov()*.008726646f) : 1.f;
    // The GPU expands the local viewport to full screen, then divides its
    // horizontal scale by widescreen once. No split-screen factor remains.
    proj_x *= scale / widescreen();
    proj_y *= scale;
    auto player=netplay_player(local_netplay_port);
    if (valid(player,0x5EC)) update_targets(player,1.f/59.94f);
  }
}
std::array<float, 3> camera_point(float x, float y, float z) {
  return {view[0] * x + view[1] * y + view[2] * z + view[3],
          view[4] * x + view[5] * y + view[6] * z + view[7],
          view[8] * x + view[9] * y + view[10] * z + view[11]};
}
bool enemy_render_root(unsigned object,std::array<float,12>& root){
  if(!valid(object,0x100))return false;
  const auto proxy=mem_read32(object+0xFC);if(!valid(proxy,0x40))return false;
  const auto matrix=mem_read32(proxy+4);if(!valid(matrix,48))return false;
  for(unsigned i=0;i<12;++i){root[i]=u2f(mem_read32(matrix+i*4));if(!std::isfinite(root[i]) || std::abs(root[i])>1000000.f)return false;}
  const double determinant=double(root[0])*(double(root[5])*root[10]-double(root[6])*root[9])-double(root[1])*(double(root[4])*root[10]-double(root[6])*root[8])+double(root[2])*(double(root[4])*root[9]-double(root[5])*root[8]);
  return std::isfinite(determinant) && std::abs(determinant)>1e-8;
}
std::array<float,3> model_world_point(const std::array<float,12>& root,const std::array<float,3>& local){
  std::array<float,3> world{};for(unsigned row=0;row<3;++row){world[row]=root[row*4+3];for(unsigned axis=0;axis<3;++axis)world[row]+=root[row*4+axis]*local[axis];}return world;
}
// Native cModelInstance: +0x78 is the current array of 3x4 bone transforms;
// +0x7C is its count. cAIBase +0xFC points to the model controller;
// controller +0x20 points to the animation instance.
// Names are in resource->data(+0xC)->bone list(+0x30), in matrix order.
struct BonePoint {
  unsigned index;
  std::string name;
  std::array<float, 3> view;
  int parent = -1;
};
// Private integration telemetry: compare gameplay-derived bones with the native
// render proxy's independent root matrix and a captured rendered frame. Never
// changes poses or aim, and remains bounded even in long-running hidden tests.
struct ModAimTrace {
  FILE* file=nullptr;unsigned sample=0,bones=0;
  static void numbers(FILE* f,const float* values,unsigned count){
    std::fputc('[',f);for(unsigned i=0;i<count;++i){if(i)std::fputc(',',f);if(std::isfinite(values[i]))std::fprintf(f,"%.9g",double(values[i]));else std::fputs("null",f);}std::fputc(']',f);
  }
  template<size_t N>void array(const char* name,const std::array<float,N>& values){std::fprintf(file,",\"%s\":",name);numbers(file,values.data(),unsigned(N));}
  explicit ModAimTrace(unsigned player){
    const auto path=std::getenv("SSNE_MOD_TEST_TRACE");if(!std::getenv("SSNE_TEST_HIDDEN") || !path || !*path)return;
    static unsigned calls=0,samples=0;if(calls++%15 || samples>=120)return;sample=++samples;
    file=std::fopen(path,sample==1?"w":"a");if(!file)return;
    std::fprintf(file,"{\"type\":\"camera\",\"sample\":%u,\"player\":%u,\"local_port\":%d",sample,player,local_netplay_port);
    array("view",view);array("native_projection",std::array<float,2>{native_proj_x,native_proj_y});array("projection",std::array<float,2>{proj_x,proj_y});array("display",std::array<float,2>{widescreen(),fov()});std::fputs("}\n",file);
  }
  ~ModAimTrace(){if(file)std::fclose(file);}
  void bone(unsigned object,unsigned proxy,unsigned model,const BoneInfo& bone,unsigned matrix,const std::array<float,3>& world,const std::array<float,3>& eye){
    if(!file || bones++>=128)return;
    std::fprintf(file,"{\"type\":\"bone\",\"sample\":%u,\"enemy\":%u,\"proxy\":%u,\"model\":%u,\"index\":%u",sample,object,proxy,model,bone.index);
    std::array<float,12> skin{};for(unsigned i=0;i<12;++i)skin[i]=u2f(mem_read32(matrix+i*4));
    array("skin",skin);array("bind_center",bone.bind_center);array("world",world);array("eye",eye);
    array("screen",std::array<float,2>{.5f+.5f*proj_x*eye[0]/-eye[2],.5f-.5f*proj_y*eye[1]/-eye[2]});
    const auto root_address=mem_read32(proxy+4);
    if(valid(root_address,48)){
      std::array<float,12> root{};for(unsigned i=0;i<12;++i)root[i]=u2f(mem_read32(root_address+i*4));array("render_root",root);
      std::array<float,3> local{},rendered{},scaled{};
      for(unsigned row=0;row<3;++row){local[row]=skin[row*4+3];for(unsigned axis=0;axis<3;++axis)local[row]+=skin[row*4+axis]*bone.bind_center[axis];}
      for(unsigned row=0;row<3;++row){rendered[row]=scaled[row]=root[row*4+3];for(unsigned axis=0;axis<3;++axis){rendered[row]+=root[row*4+axis]*local[axis];scaled[row]+=root[row*4+axis]*local[axis]*u2f(mem_read32(object+0x68+axis*4));}}
      array("render_world",rendered);array("render_scaled_world",scaled);
      const auto render_eye=camera_point(rendered[0],rendered[1],rendered[2]);array("render_eye",render_eye);
      array("render_screen",std::array<float,2>{.5f+.5f*proj_x*render_eye[0]/-render_eye[2],.5f-.5f*proj_y*render_eye[1]/-render_eye[2]});
    }
    std::fputs("}\n",file);
  }
  void aim(unsigned enemy,unsigned bone,const std::array<float,3>& point,float blend,bool enabled){
    if(!file)return;std::fprintf(file,"{\"type\":\"aim\",\"sample\":%u,\"enemy\":%u,\"bone\":%u,\"enabled\":%s,\"option_aimbot\":%s,\"option_hold\":%s,\"option_point\":%d,\"input_enabled\":%s,\"input_held\":%s",sample,enemy,bone,enabled?"true":"false",state.options.aimbot?"true":"false",state.options.aim_hold?"true":"false",state.options.aim_point,aim_enabled?"true":"false",aim_held?"true":"false");array("eye",point);
    array("delta",std::array<float,2>{enabled?std::atan2(point[0],-point[2])*blend:0.f,enabled?std::atan2(point[1],std::hypot(point[0],point[2]))*blend:0.f});std::fputs("}\n",file);
  }
};
std::vector<BonePoint> read_bones(unsigned object, Tracked &enemy,ModAimTrace* trace) {
  std::vector<BonePoint> result;
  if (!valid(object, 0x12C))
    return result;
  unsigned proxy = mem_read32(object + 0xFC);
  if (!valid(proxy, 0x40))
    return result;
  unsigned model = mem_read32(proxy + 0x20);
  if (!valid(model, 0xB0))
    return result;
  unsigned count = mem_read32(model + 0x7C),
           matrices = mem_read32(model + 0x78),
           resource = mem_read32(model + 0xC);
  if (!count || count > 256 || !valid(matrices, count * 48) ||
      !valid(resource, 0x10))
    return result;
  unsigned data = mem_read32(resource + 0xC);
  if (!valid(data, 0x34))
    return result;
  if (enemy.model != model || enemy.resource != resource ||
      enemy.bone_count != count) {
    enemy.model = model;
    enemy.resource = resource;
    enemy.bone_count = count;
    enemy.bones.clear();
    unsigned node = mem_read32(data + 0x30);
    for (unsigned i = 0; i < std::min(count, 128u); i++) {
      if (!valid(node, 0x34)) {
        enemy.bones.clear();
        break;
      }
      unsigned text = mem_read32(node + 4);
      std::string name;
      for (unsigned k = 0; k < 95 && valid(text + k, 1); k++) {
        unsigned ch = mem_read8(text + k);
        if (!ch)
          break;
        if (ch < 32 || ch > 126) {
          name.clear();
          break;
        }
        name += char(ch);
      }
      if (name.empty())
        name = "Bone " + std::to_string(i);
      BoneInfo info{i, name};
      // These are skinning groups, not joint origins. The matrix transforms
      // model-space bind vertices. Transform their center with the full matrix
      // to obtain a point on the animated body part, never its raw translation.
      unsigned vertices = mem_read32(node + 0x10);
      unsigned vertex_count = mem_read32(node + 0xC);
      if (!vertex_count) {
        vertex_count = mem_read32(node + 0x28);
        vertices = mem_read32(node + 0x2C);
      }
      if (vertex_count && vertex_count <= 65536 &&
          valid(vertices, vertex_count * 12)) {
        std::array<double, 3> sum{};
        info.has_center = true;
        for (unsigned v = 0; v < vertex_count && info.has_center; ++v)
          for (unsigned axis = 0; axis < 3; ++axis) {
            float coordinate = u2f(mem_read32(vertices + v * 12 + axis * 4));
            if (!std::isfinite(coordinate) || std::abs(coordinate) > 200) {
              info.has_center = false;
              break;
            }
            sum[axis] += coordinate;
          }
        for (unsigned axis = 0; axis < 3; ++axis)
          info.bind_center[axis] = float(sum[axis] / vertex_count);
      }
      enemy.bones.push_back(std::move(info));
      node = mem_read32(node);
    }
    // The export stores named skinning groups, rather than parent indices.
    // Link only known anatomical names; all other joints remain individual
    // dots.
    auto index_of = [&](const std::string &name) {
      for (auto &b : enemy.bones)
        if (b.name == name)
          return int(b.index);
      return -1;
    };
    for (auto &b : enemy.bones)
      if (b.parent < 0) {
        std::string parent;
        if (b.name == "pelvis")
          parent = "root";
        if (b.name == "spine1")
          parent = "pelvis";
        if (b.name == "spine2")
          parent = "spine1";
        if (b.name == "spine3")
          parent = "spine2";
        if (b.name == "neck")
          parent = "spine3";
        if (b.name == "head")
          parent = index_of("neck") >= 0 ? "neck" : "spine3";
        for (const char *side : {"left_", "right_"}) {
          std::string prefix = side;
          if (b.name == prefix + "shoulder")
            parent = "spine3";
          if (b.name == prefix + "bicep")
            parent = prefix + "shoulder";
          if (b.name == prefix + "forearm")
            parent = prefix + "bicep";
          if (b.name == prefix + "hand")
            parent = prefix + "forearm";
          if (b.name == prefix + "thigh")
            parent = "pelvis";
          if (b.name == prefix + "knee")
            parent = prefix + "thigh";
          if (b.name == prefix + "ankle")
            parent = prefix + "knee";
          if (b.name == prefix + "toe")
            parent = prefix + "ankle";
        }
        if (!parent.empty())
          b.parent = index_of(parent);
      }
  }
  // The renderer root already contains placement, pivot, rotation and scale.
  // Gameplay actor position is its collision origin and can differ vertically.
  std::array<float,12> root;if(!enemy_render_root(object,root))return result;
  for (auto &bone : enemy.bones) {
    if (bone.name.rfind("dead_", 0) == 0)
      continue;
    unsigned matrix = matrices + bone.index * 48;
    if (!bone.has_center)
      continue;
    auto component = [&](unsigned row) {
      float coordinate = u2f(mem_read32(matrix + row * 16 + 12));
      for (unsigned axis = 0; axis < 3; ++axis)
        coordinate += u2f(mem_read32(matrix + row * 16 + axis * 4)) *
                      bone.bind_center[axis];
      return coordinate;
    };
    float x = component(0), y = component(1), z = component(2);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        std::max({std::abs(x), std::abs(y), std::abs(z)}) > 200)
      continue;
    const auto world=model_world_point(root,std::array<float,3>{x,y,z});
    auto v = camera_point(world[0],world[1],world[2]);
    if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) {
      result.push_back({bone.index, bone.name, v, bone.parent});
      if(trace)trace->bone(object,proxy,model,bone,matrix,world,v);
    }
  }
  return result;
}
void update_targets(unsigned player, float dt) {
  if (local_netplay_port>=0) pending_aim_yaw=pending_aim_pitch=0;
  markers.clear();
  item_markers.clear();
  bone_markers.clear();
  target = 0;
  state.tracked_enemies = 0;
  if (!state.targeting_available ||
      (!state.options.aimbot && !state.options.esp && !state.options.item_esp &&
       !state.options.bone_esp) ||
      std::chrono::steady_clock::now() - camera_time >
          std::chrono::milliseconds(250) ||
      !std::isfinite(proj_x) || !std::isfinite(proj_y) || proj_x <= 0 ||
      proj_y <= 0)
    return;
  ModAimTrace trace(player);
  float best = state.options.aim_fov * 0.017453292f;
  std::array<float, 3> chosen{};
  unsigned chosen_bone = ~0u;
  for (auto &[object, enemy] : enemies) {
    if (!enemy.registered || !valid(object, 0xD4) ||
        mem_read32(object + 0x48) != enemy.vtable)
      continue;
    // Native damage handlers subtract from +0xCC and dispatch death at zero.
    float health = u2f(mem_read32(object + 0xCC));
    if (!std::isfinite(health) || health <= 0)
      continue;
    enemy.max_health = std::max(enemy.max_health, health);
    ++state.tracked_enemies;
    std::array<float,12> root;if(!enemy_render_root(object,root))continue;
    // Native model initialization stores the model's vertical bounds span at
    // +0xD0 (max.y - min.y, computed through the model's bounding-box method).
    float height = u2f(mem_read32(object + 0xD0));
    float center = std::isfinite(enemy.center) ? enemy.center : height * .5f;
    const float body_y = state.options.auto_aim_height && std::isfinite(height) &&
                 height > .05f && height < 200.f
             ? center + state.options.aim_height - 1.f
             : state.options.aim_height;
    const auto body_world=model_world_point(root,std::array<float,3>{0.f,body_y,0.f});
    auto v = camera_point(body_world[0],body_world[1],body_world[2]);
    float dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(dist) || dist < .1f || dist > state.options.range ||
        v[2] >= -.1f)
      continue;
    auto bones = (state.options.bone_esp ||
                  (state.options.aimbot && state.options.aim_point != 0))
                     ? read_bones(object, enemy,&trace)
                     : std::vector<BonePoint>{};
    unsigned aim_bone = ~0u;
    float bone_best = std::numeric_limits<float>::infinity();
    for (auto &bone : bones) {
      auto q = bone.view;
      float distance = std::hypot(q[0], q[1], q[2]);
      if (distance < .1f || distance > state.options.range || q[2] >= -.1f)
        continue;
      float bx = .5f + .5f * proj_x * q[0] / -q[2],
            by = .5f - .5f * proj_y * q[1] / -q[2];
      if (state.options.bone_esp && bx >= 0 && bx <= 1 && by >= 0 && by <= 1)
        bone_markers.push_back(
            {bx, by, object, bone.index, bone.name, false, bone.parent});
      std::string name = bone.name;
      for (char &ch : name)
        if (ch >= 'A' && ch <= 'Z')
          ch += 32;
      bool eligible = state.options.aim_point == 1 ||
                      (state.options.aim_point == 2 &&
                       (name.find("head") != std::string::npos ||
                        name.find("skull") != std::string::npos)) ||
                      (state.options.aim_point == 3 &&
                       (name.find("chest") != std::string::npos ||
                        name.find("torso") != std::string::npos ||
                        name.find("spine") != std::string::npos ||
                        name.find("body") != std::string::npos)) ||
                      (state.options.aim_point == 4 &&
                       int(bone.index) == state.options.aim_bone);
      float angle = std::atan2(std::hypot(q[0], q[1]), -q[2]);
      if (state.options.aimbot && eligible && angle < bone_best) {
        bone_best = angle;
        aim_bone = bone.index;
        v = q;
      }
    }
    dist = std::hypot(v[0], v[1], v[2]);
    float angle = std::atan2(std::hypot(v[0], v[1]), -v[2]);
    if (angle < best) {
      best = angle;
      target = object;
      chosen = v;
      chosen_bone = aim_bone;
    }
    float sx = .5f + .5f * proj_x * v[0] / -v[2],
          sy = .5f - .5f * proj_y * v[1] / -v[2];
    if (sx >= 0 && sx <= 1 && sy >= 0 && sy <= 1)
      markers.push_back(
          {sx, sy, dist, enemy.type, false, health, enemy.max_health});
  }
  if (state.options.item_esp)
    for (auto &[object, item] : items) {
      if (!valid(object, item.kind == ModItemKind::Secret ? 0xA8 : 0x18C) ||
          mem_read32(object + 0x48) != item.vtable)
        continue;
      bool selected =
          item.kind == ModItemKind::Weapon   ? state.options.item_weapons
          : item.kind == ModItemKind::Health ? state.options.item_health
          : item.kind == ModItemKind::Armor  ? state.options.item_armor
                                             : state.options.item_secrets;
      if (!selected)
        continue;
      if (item.kind == ModItemKind::Secret && mem_read32(object + 0xA4) != 0)
        continue;
      if (item.kind != ModItemKind::Secret) {
        // Native pickup acceptance (800C7F78) sets +104 when consumed, +11C
        // while awaiting respawn, or +170[player slot] for retained weapons.
        // +E8 is the destroy-on-pickup configuration, not a collected flag.
        unsigned slot = mem_read32(player + 0x17C);
        if (mem_read32(object + 0x104) != 0 ||
            mem_read32(object + 0x11C) != 0 ||
            (slot < native_party_capacity() && mem_read32(native_party_pickup_flags(object) + slot * 4) != 0))
          continue;
      }
      auto v = camera_point(u2f(mem_read32(object + 0x20)),
                            u2f(mem_read32(object + 0x24)),
                            u2f(mem_read32(object + 0x28)));
      float distance = std::hypot(v[0], v[1], v[2]);
      if (!std::isfinite(distance) || distance < .1f ||
          distance > state.options.range || v[2] >= -.1f)
        continue;
      float x = .5f + .5f * proj_x * v[0] / -v[2],
            y = .5f - .5f * proj_y * v[1] / -v[2];
      if (x >= 0 && x <= 1 && y >= 0 && y <= 1)
        item_markers.push_back({x, y, distance, item.type, item.kind});
    }
  for (auto &bone : bone_markers)
    bone.target = state.options.aimbot && bone.enemy == target &&
                  bone.index == chosen_bone;
  if (target) {
    // Mark by projected coordinates; identical positions are harmless for the
    // overlay.
    float sx = .5f + .5f * proj_x * chosen[0] / -chosen[2],
          sy = .5f - .5f * proj_y * chosen[1] / -chosen[2];
    for (auto &m : markers)
      m.target = std::abs(m.x - sx) < .00001f && std::abs(m.y - sy) < .00001f;
  }
  const bool aiming=target && state.options.aimbot && aim_enabled && (!state.options.aim_hold || aim_held);
  float blend = 1 - std::exp(-state.options.aim_speed * dt);
  trace.aim(target,chosen_bone,chosen,blend,aiming);
  if (!aiming)return;
  if (local_netplay_port>=0) {
    pending_aim_yaw = std::atan2(chosen[0],-chosen[2])*blend;
    pending_aim_pitch = std::atan2(chosen[1],std::hypot(chosen[0],chosen[2]))*blend;
    return; // correction is transported as input, never applied locally to RAM
  }
  float yaw = u2f(mem_read32(player + 0x290)),
        pitch = u2f(mem_read32(player + 0x294));
  unsigned lo = mem_read32(player + 0x194), hi = mem_read32(player + 0x198);
  if (!std::isfinite(yaw) || !std::isfinite(pitch) || !valid(lo, 0x28) ||
      !valid(hi, 0x28))
    return;
  float low = u2f(mem_read32(lo + 0x24)) * .017453292f,
        high = u2f(mem_read32(hi + 0x24)) * .017453292f;
  if (!std::isfinite(low) || !std::isfinite(high))
    return;
  if (low > high)
    std::swap(low, high);
  yaw = std::remainder(yaw + std::atan2(chosen[0], -chosen[2]) * blend,
                       6.283185307f);
  pitch = std::clamp(
      pitch - std::atan2(chosen[1], std::hypot(chosen[0], chosen[2])) * blend,
      low, high);
  mem_write32(player + 0x290, f2u(yaw));
  mem_write32(player + 0x294, f2u(pitch));
}
void capture(PPCContext &c, bool enemy, PPCFunc fn, PPCFunc hook,
             unsigned addr) {
  Recipe r{};
  r.level = c.r[3];
  r.enemy = enemy;
  r.kind = c.r[4];
  r.proto = c.r[enemy ? 5 : 6];
  r.flag = c.r[7];
  unsigned matrix = c.r[enemy ? 4 : 5];
  bool ok = valid(r.level) && valid(r.proto, 8) && valid(matrix, 48);
  // The enemy factory returns null when reusing an existing level object, even
  // though its prototype and class are valid. Keep those templates too; replay
  // always requests a new object.
  bool reused_enemy =
      ok && enemy && c.r[6] != 0 && mem_read32(r.proto + 4) != 0;
  if (ok) {
    r.type = mem_read32(r.proto);
    for (int i = 0; i < 12; i++)
      r.matrix[i] = mem_read32(matrix + i * 4);
  }
  original(addr, fn, hook, c);
  if (!ok || (!valid(c.r[3]) && !reused_enemy) || replaying)
    return;
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (recipes.size() >= 128)
    return;
  for (auto &old : recipes)
    if (old.level == r.level && old.type == r.type && old.enemy == enemy &&
        (enemy || old.kind == r.kind))
      return;
  r.id = next_id++;
  recipes.push_back(r);
  unsigned display_type = r.type;
  if (enemy) {
    // The level prototype index differs from the enemy class ID (e.g. prototype
    // 37 is Kleer, class 18).
    display_type = r.type < 61 ? mem_read32(game_address(0x8021AB2C) + r.type * 4) : ~0u;
  } else if ((r.type <= 45 || r.type == 55 || (r.type >= 57 && r.type <= 76)) &&
             valid(c.r[3], 0x184)) {
    // The factory can turn a random pickup prototype into an ordinary
    // weapon/ammo pickup.
    unsigned resolved = mem_read32(c.r[3] + 0x180);
    if (resolved <= 55)
      display_type = resolved;
  }
  std::string name = mod_type_name(enemy, display_type);
  if (!enemy && r.type >= 57 && r.type <= 76)
    name = "Random pickup: " + name;
  state.templates.push_back({r.id, enemy, std::move(name), display_type});
}
PPCFunc pickup_accept_fn;
void pickup_accept_hook(PPCContext& c) {
  const auto object=c.r[3], player=c.r[4];
  const bool readable=valid(object,0x184) && valid(player,0x180);
  const auto type=readable?mem_read32(object+0x180):~0u;
  const auto slot=readable?mem_read32(player+0x17C):~0u;
  original(0x800C7F78,pickup_accept_fn,pickup_accept_hook,c);
  if(readable && c.r[3]) {
    std::string message="[mods-pickup] object="+std::to_string(object)+" type="+std::to_string(type)+" port="+std::to_string(slot);
    log_message(message);
  }
}
void item_hook(PPCContext &c) {
  const auto matrix=c.r[5];std::array<float,3> spawn{};bool has_spawn=valid(matrix,48);
  if(has_spawn)for(unsigned axis=0;axis<3;++axis){spawn[axis]=u2f(mem_read32(matrix+12+16*axis));has_spawn=has_spawn && std::isfinite(spawn[axis]);}
  capture(c, false, item_fn, item_hook, game_address(0x8010CF00));
  std::lock_guard<std::recursive_mutex> guard(lock);
  track_item(c.r[3]);
  if(valid(c.r[3],0x184))notify_native_entity({NativeEntityNotice::Event::CreatedPickup,c.r[3],mem_read32(c.r[3]+0x180)});
  if(has_spawn && valid(c.r[3],0x184))notify_native_entity({NativeEntityNotice::Event::Spawned,c.r[3],0,-2,spawn});
}
void enemy_hook(PPCContext &c) {
  const auto matrix=c.r[4];std::array<float,3> spawn{};bool has_spawn=valid(matrix,48);
  if(has_spawn)for(unsigned axis=0;axis<3;++axis){spawn[axis]=u2f(mem_read32(matrix+12+16*axis));has_spawn=has_spawn && std::isfinite(spawn[axis]);}
  // Snapshot the factory inputs before it overwrites the caller registers.
  // Resource preloads (r7 == 0), bosses and scripted Mini Sam stay singular.
  const auto multiplier=spawn_multiplier.load(std::memory_order_relaxed);
  const bool requested=multiplier>1 &&
      !replica_simulation() && !replaying && !multiplying_enemy && c.r[7]!=0 &&
      has_spawn && valid(c.r[3]) && valid(c.r[5],32) &&
      c.r[1]>=0x80002000u && valid(c.r[1]-0x2000,0x2000);
  bool duplicate=false;
  PPCContext input{};std::array<unsigned,12> transform{};std::array<unsigned,8> prototype{};
  if(requested) {
    const auto index=mem_read32(c.r[5]);
    const auto type=index<61?mem_read32(game_address(0x8021AB2C)+index*4):~0u;
    duplicate=type<=1 || (type>=5 && type<=39);
    if(duplicate){input=c;for(unsigned i=0;i<12;++i)transform[i]=mem_read32(matrix+i*4);for(unsigned i=0;i<8;++i)prototype[i]=mem_read32(c.r[5]+i*4);}
  }
  capture(c, true, enemy_fn, enemy_hook, game_address(0x8010D70C));
  if(has_spawn && valid(c.r[3],0xD4))notify_native_entity({NativeEntityNotice::Event::Spawned,c.r[3],0,-2,spawn});
  if(!duplicate || !valid(c.r[3],0xD4))return;
  // Run the original factory with a private context/stack. Keep the original
  // return value and prototype untouched; both actors follow native lifetimes.
  struct Scratch {
    unsigned begin;std::array<unsigned,0x2000/4> words;
    Scratch(unsigned sp):begin(sp-0x2000){for(unsigned i=0;i<words.size();++i)words[i]=mem_read32(begin+i*4);multiplying_enemy=true;}
    ~Scratch(){for(unsigned i=0;i<words.size();++i)mem_write32(begin+i*4,words[i]);multiplying_enemy=false;}
  } scratch(input.r[1]);
  for(unsigned extra=1;extra<multiplier;++extra){
  auto call=input;call.msr&=~0x8000u;call.r[1]-=0x200;
  mem_write32(call.r[1],input.r[1]);
  const auto clone_matrix=call.r[1]+0x80,clone_proto=call.r[1]+0xC0;
  for(unsigned i=0;i<12;++i)mem_write32(clone_matrix+i*4,transform[i]);
  for(unsigned i=0;i<8;++i)mem_write32(clone_proto+i*4,prototype[i]);
  mem_write32(clone_proto+4,0);call.r[4]=clone_matrix;call.r[5]=clone_proto;call.r[6]=0;
  original(game_address(0x8010D70C),enemy_fn,enemy_hook,call);
  if(valid(call.r[3],0xD4))notify_native_entity({NativeEntityNotice::Event::Spawned,call.r[3],0,-2,spawn});
  }
}
void exit_hook(PPCContext &c) {
  notify_native_entity({NativeEntityNotice::Event::LevelExit});
  netplay_reset_world();
  {
    std::lock_guard<std::recursive_mutex> guard(lock);
    clear_overrides();
    enemies.clear();
    items.clear();
    markers.clear();
    item_markers.clear();
    bone_markers.clear();
    target = 0;
    camera_time = {};
    pending_aim_yaw=pending_aim_pitch=0;
    state.tracked_enemies = 0;
    recipes.clear();
    requests.clear();
    state.templates.clear();
    last_tick = {};
    state.status = "Enter a level to use cheats and collect spawn templates.";
  }
  original(game_address(0x801664D8), exit_fn, exit_hook, c);
}
bool install(unsigned addr, std::array<unsigned, 3> sig, PPCFunc hook,
             PPCFunc &fn) {
  auto entry = ssne_find_func(addr);
  if (!entry)
    return false;
  for (int i = 0; i < 3; i++)
    if (mem_read32(addr + i * 4) != sig[i])
      return false;
  fn = entry->func;
  ssne_install_hook(addr, hook);
  return true;
}
struct Override {
  unsigned addr = 0, original_value = 0;
  bool applied = false;
  void set(unsigned p, bool enabled) {
    if (applied && (p != addr || !enabled)) {
      if (valid(addr))
        mem_write32(addr, original_value);
      applied = false;
    }
    if (enabled && valid(p)) {
      if (!applied) {
        addr = p;
        original_value = mem_read32(p);
        applied = true;
      }
      mem_write32(p, 1);
    }
  }
};
Override overrides[5];
std::array<Override,8> ammo_overrides;
std::array<unsigned,8> granted_weapons_inventory{};
struct FireRateOverride {
  unsigned weapon, vtable, modes, address, original_value;
};
std::unordered_map<unsigned, FireRateOverride> fire_rates;
bool valid_rate_owner(const FireRateOverride &r) {
  return valid(r.weapon, 0x1D4) && valid(r.address) &&
         mem_read32(r.weapon) == r.vtable &&
         mem_read32(r.weapon + 0x1D0) == r.modes;
}
void restore_fire_rates() {
  for (auto &[address, r] : fire_rates)
    if (valid_rate_owner(r))
      mem_write32(address, r.original_value);
  fire_rates.clear();
}
void update_fire_rates(unsigned root_player) {
  if (!state.options.rapid_fire) {
    restore_fire_rates();
    return;
  }
  std::vector<unsigned> seen;
  std::vector<unsigned> party{root_player};
  if(local_netplay_port>=0 && !replica_simulation())for(unsigned slot=0;slot<native_party_capacity();++slot){const auto actor=mem_read32(native_party_registry()+slot*4);if(actor!=root_player && valid(actor,0x790) && native_party_actor_occupied(actor))party.push_back(actor);}
  for(auto player:party){
  // Twelve inventory slots, with up to three native weapon instances each.
  // Fire-mode records are 60 bytes; AttackRate is the float at +0x1C (the
  // preceding +0x18 is range).
  for (unsigned slot = 0; slot < 12; slot++) {
    unsigned entry = player + 0x4BC + slot * 24;
    unsigned instances = mem_read32(entry + 12);
    if (instances > 3)
      continue;
    for (unsigned instance = 0; instance < instances; instance++) {
      unsigned weapon = mem_read32(entry + instance * 4);
      if (!valid(weapon, 0x1D4))
        continue;
      unsigned count = mem_read32(weapon + 0x1BC);
      unsigned modes = mem_read32(weapon + 0x1D0);
      if (!count || count > 4 || !valid(modes, count * 60))
        continue;
      for (unsigned mode = 0; mode < count; mode++) {
        unsigned address = modes + mode * 60 + 0x1C;
        auto existing = fire_rates.find(address);
        if (existing != fire_rates.end() && !valid_rate_owner(existing->second))
          fire_rates.erase(existing);
        float rate = u2f(mem_read32(address));
        if (!std::isfinite(rate) || rate <= 0 || rate > 1000)
          continue;
        auto [it, inserted] = fire_rates.try_emplace(
            address, FireRateOverride{weapon, mem_read32(weapon), modes,
                                      address, mem_read32(address)});
        float original_rate = u2f(it->second.original_value);
        mem_write32(address, f2u(std::min(std::max(120.f, original_rate),
                                          original_rate *
                                              state.options.fire_multiplier)));
        seen.push_back(address);
      }
    }
  }
  }
  for (auto it = fire_rates.begin(); it != fire_rates.end();) {
    if (std::find(seen.begin(), seen.end(), it->first) == seen.end()) {
      if (valid_rate_owner(it->second))
        mem_write32(it->first, it->second.original_value);
      it = fire_rates.erase(it);
    } else
      ++it;
  }
}
void clear_overrides() {
  restore_fire_rates();
  for(auto& o:ammo_overrides)o.set(0,false);granted_weapons_inventory={};
  for (auto &o : overrides)
    o.set(0, false);
}
} // namespace
void set_enemy_multiplier(unsigned value) {spawn_multiplier.store(value>=1 && value<=5?value:1,std::memory_order_relaxed);}
unsigned enemy_multiplier() {return spawn_multiplier.load(std::memory_order_relaxed);}
void install_mod_hooks() {
  install_ammo_rule_hooks();
  std::lock_guard<std::recursive_mutex> guard(lock);
  bool a = install(game_address(0x8010CF00), {0x9421FFD0, 0x7C0802A6, 0xBF61001C}, item_hook,
                   item_fn);
  bool b = install(game_address(0x8010D70C), {0x9421FF90, 0x7C0802A6, 0xBEC10048}, enemy_hook,
                   enemy_fn);
  bool d = install(game_address(0x801664D8), {0x9421FFE8, 0x7C0802A6, 0xBFA1000C}, exit_hook,
                   exit_fn);
  state.available = a && b && d;
  if(getenv("SSNE_TRACE_MODS"))
    install(0x800C7F78,{0x9421FFC0,0x7C0802A6,0xBF410028},pickup_accept_hook,pickup_accept_fn);

  bool t1 = install(0x8008A558, {0x9421FFE8, 0x7C0802A6, 0xBF810008},
                    create_hook, create_fn);
  bool t2 = install(game_address(0x80150640), {0x9421FFF0, 0x7C0802A6, 0xBFC10008},
                    register_hook, register_fn);
  bool t3 = install(game_address(0x8015070C), {0x9421FFF0, 0x7C0802A6, 0x93E1000C},
                    remove_hook, remove_fn);
  bool t4 = install(game_address(0x8014C090), {0x9421FF10, 0x7C0802A6, 0xDAE100A8},
                    camera_hook, camera_fn);
  install(0x80090580, {0x9421FFB0, 0x7C0802A6, 0xDBE10048}, bounds_hook,
          bounds_fn);
  install(game_address(0x8010C228), {0x8003033C, 0x2C000000, 0x4C820020}, secret_hook,
          secret_fn);
  install(0x8008462C, {0x9421FFF8, 0x7C0802A6, 0x9001000C}, secret_destroy_hook,
          secret_destroy_fn);
  state.targeting_available = state.available && t1 && t2 && t3 && t4;
  if (!state.targeting_available) {
    if (t1)
      *ssne_hook_slot(0x8008A558) = nullptr;
    if (t2)
      *ssne_hook_slot(game_address(0x80150640)) = nullptr;
    if (t3)
      *ssne_hook_slot(game_address(0x8015070C)) = nullptr;
    if (t4)
      *ssne_hook_slot(game_address(0x8014C090)) = nullptr;
  }
  native_entity_hooks_installed(state.targeting_available);
  if (!state.available) {
    if (a)
      *ssne_hook_slot(game_address(0x8010CF00)) = nullptr;
    if (b)
      *ssne_hook_slot(game_address(0x8010D70C)) = nullptr;
    if (d)
      *ssne_hook_slot(game_address(0x801664D8)) = nullptr;
  }
  state.status =
      state.available
          ? "Enter a level to use cheats and collect spawn templates."
          : "This game build is unsupported.";
}
ModState mod_state() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto s = state;
  s.active=s.available && (local_netplay_port>=0 ? netplay_world_ready() && mem_read32(game_address(0x802DB0E0))==4 && valid(netplay_player(local_netplay_port),0x790) : std::chrono::steady_clock::now()-last_tick<std::chrono::milliseconds(250));
  return s;
}
std::vector<BotActor> netplay_bot_enemies() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  std::vector<BotActor> result;
  for (auto &[object, enemy] : enemies) {
    if (!enemy.registered || !valid(object, 0xD4) || mem_read32(object+0x48)!=enemy.vtable) continue;
    float health=u2f(mem_read32(object+0xCC));
    if (!std::isfinite(health) || health<=0) continue;
    BotActor actor; actor.id=object; actor.health=health;
    for (int axis=0;axis<3;++axis) actor.position[axis]=u2f(mem_read32(object+0x20+axis*4));
    float height=u2f(mem_read32(object+0xD0));
    actor.height=std::isfinite(height) && height>.1f && height<100 ? height : 1.6f;
    if (std::isfinite(enemy.center)) actor.height=enemy.center*2;
    if (std::all_of(actor.position.begin(),actor.position.end(),[](float f){return std::isfinite(f) && std::abs(f)<100000.f;})) result.push_back(actor);
  }
  std::sort(result.begin(),result.end(),[](auto& a,auto& b){return a.id<b.id;});
  return result;
}
uint64_t netplay_world_checksum() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  std::vector<std::array<uint32_t, 5>> rows;
  for (auto &[object, enemy] : enemies) {
    if (!enemy.registered || !valid(object, 0xD4) ||
        mem_read32(object + 0x48) != enemy.vtable)
      continue;
    std::array<uint32_t, 5> row{enemy.type};
    int i = 1;
    for (uint32_t offset : {0x20u, 0x24u, 0x28u, 0xCCu}) {
      float v = u2f(mem_read32(object + offset));
      row[i++] = std::isfinite(v) && std::abs(v) < 1000000.f
                     ? uint32_t(int32_t(std::round(double(v) * 10000.)))
                     : 0xFFFFFFFFu;
    }
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end());
  uint64_t h = 14695981039346656037ull;
  for (auto &row : rows)
    for (auto v : row) {
      h ^= v;
      h *= 1099511628211ull;
    }
  return h;
}
void netplay_log_world_state() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  std::vector<unsigned> objects;
  for (auto &[object, enemy] : enemies)
    if (enemy.registered && valid(object, 0xD4) &&
        mem_read32(object + 0x48) == enemy.vtable)
      objects.push_back(object);
  std::sort(objects.begin(), objects.end());
  for (auto object : objects) {
    char line[200];
    snprintf(line, sizeof(line),
             "[netplay-enemy] object=%08X type=%u position=%08X,%08X,%08X "
             "health=%08X",
             object, enemies.at(object).type, mem_read32(object + 0x20),
             mem_read32(object + 0x24), mem_read32(object + 0x28),
             mem_read32(object + 0xCC));
    log_message(line);
  }
}
void mod_options(ModOptions o) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  o.aim_fov =
      std::isfinite(o.aim_fov) ? std::clamp(o.aim_fov, 1.f, 90.f) : 25.f;
  o.aim_speed =
      std::isfinite(o.aim_speed) ? std::clamp(o.aim_speed, 1.f, 60.f) : 12.f;
  o.range = std::isfinite(o.range) ? std::clamp(o.range, 5.f, 500.f) : 150.f;
  o.aim_height =
      std::isfinite(o.aim_height) ? std::clamp(o.aim_height, 0.f, 10.f) : 1.f;
  o.fire_multiplier = std::isfinite(o.fire_multiplier)
                          ? std::clamp(o.fire_multiplier, 1.f, 10.f)
                          : 3.f;
  o.aim_point = std::clamp(o.aim_point, 0, 4);
  o.aim_bone = std::clamp(o.aim_bone, 0, 127);
  state.options = o;
  if (!o.rapid_fire)
    restore_fire_rates();
}
void mod_netplay_player(int port) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  local_netplay_port=port; pending_aim_yaw=pending_aim_pitch=0;
}
void mod_take_netplay_aim(double& yaw,double& pitch) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  yaw=pending_aim_yaw; pitch=pending_aim_pitch;
  pending_aim_yaw=pending_aim_pitch=0;
  if (!netplay_world_ready() || !state.options.aimbot || !aim_enabled || (state.options.aim_hold && !aim_held)) yaw=pitch=0;
}
void mod_local_options(ModOptions o) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  o.god=state.options.god; o.ammo=state.options.ammo; o.weapons=state.options.weapons;
  o.no_enemies=state.options.no_enemies; o.rapid_fire=state.options.rapid_fire;
  o.fire_multiplier=state.options.fire_multiplier;
  mod_options(o);
  pending_aim_yaw=pending_aim_pitch=0;
}
void mod_shared_options(uint32_t flags,float fire_multiplier) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto o=state.options;
  o.god=flags&1; o.ammo=flags&2; o.weapons=flags&4; o.no_enemies=flags&8; o.rapid_fire=flags&16;
  o.fire_multiplier=fire_multiplier;
  mod_options(o);
}
ModOptions mod_current_options() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  return state.options;
}
std::vector<ModMarker> mod_markers() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (!state.options.esp || std::chrono::steady_clock::now() - (local_netplay_port>=0?camera_time:last_tick) >
                                std::chrono::milliseconds(250))
    return {};
  return markers;
}
std::vector<ModItemMarker> mod_item_markers() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (!state.options.item_esp || std::chrono::steady_clock::now() - (local_netplay_port>=0?camera_time:last_tick) >
                                     std::chrono::milliseconds(250))
    return {};
  return item_markers;
}
std::vector<ModBoneMarker> mod_bone_markers() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (!state.options.bone_esp || std::chrono::steady_clock::now() - (local_netplay_port>=0?camera_time:last_tick) >
                                     std::chrono::milliseconds(250))
    return {};
  return bone_markers;
}
void mod_aim_input(bool held, bool enabled) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  aim_held = held;
  aim_enabled = enabled;
}
void mod_spawn(unsigned id, int count, float distance) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (requests.size() < 8)
    requests.push_back(
        {id, std::clamp(count, 1, 10), std::clamp(distance, 3.f, 30.f)});
}
void mod_spawn_type(bool enemy, unsigned type, int count, float distance) {
  auto it =
      std::find_if(mod_catalog().begin(), mod_catalog().end(), [&](auto &e) {
        return e.enemy == enemy && e.type == type && !e.availability_note;
      });
  if (it == mod_catalog().end())
    return;
  mod_spawn((enemy ? 0x80000000u : 0x40000000u) | type, count, distance);
}
void mod_test_pickup_at_player(unsigned type,unsigned slot) {
  if(!getenv("SSNE_TEST_HIDDEN") || slot>=native_party_capacity())return;
  const auto found=std::find_if(mod_catalog().begin(),mod_catalog().end(),[&](const auto& item){return !item.enemy && item.type==type && !item.availability_note;});
  if(found!=mod_catalog().end())requests.push_back({0x40000000u|type,1,0.f,int(slot)});
}
void mod_test_enemy_at_player(unsigned type,unsigned slot,int count,float distance) {
  if(!getenv("SSNE_TEST_HIDDEN") || slot>=native_party_capacity())return;
  std::lock_guard<std::recursive_mutex> guard(lock);
  const auto found=std::find_if(mod_catalog().begin(),mod_catalog().end(),[&](const auto& item){return item.enemy && item.type==type && !item.availability_note;});
  if(found!=mod_catalog().end() && requests.size()<8)
    requests.push_back({0x80000000u|type,std::clamp(count,1,10),std::clamp(distance,3.f,100.f),int(slot)});
}
void mod_tick(PPCContext &ctx, unsigned player) {
  std::unique_lock<std::recursive_mutex> guard(lock);
  if (!state.available || !valid(player, 0x5EC) || ctx.r[13] != game_address(0x802E1BA0))
    return;
  // Retain the game's multiplayer restriction on its built-in cheats.
  unsigned mode = mem_read32(game_address(0x802DB1BC));
  if (replica_simulation())return;
  if (mode >= 2 && mode <= 4 && local_netplay_port<0) {
    clear_overrides();
    requests.clear();
    last_tick = {};
    markers.clear();
    item_markers.clear();
    bone_markers.clear();
    target = 0;
    return;
  }
  auto now = std::chrono::steady_clock::now();
  float dt =
      last_tick == std::chrono::steady_clock::time_point{}
          ? 1.f / 60
          : std::clamp(std::chrono::duration<float>(now - last_tick).count(),
                       0.f, .05f);
  last_tick = now;
  if (local_netplay_port<0) update_targets(player, dt);
  update_fire_rates(player);
  if (state.status.starts_with("Enter a level"))
    state.status = "Ready. Select cheats or a spawn template.";
  overrides[0].set(game_address(0x802DA284), state.options.god);
  overrides[1].set(game_address(0x802DA288), state.options.ammo);
  overrides[2].set(game_address(0x802DA280), state.options.weapons);
  const auto flag=mem_read32(game_address(0x802DA048));
  overrides[4].set(valid(flag,0x20)?flag+0x1c:0,state.options.no_enemies);
  const unsigned count=local_netplay_port>=0?native_party_capacity():1;
  for(unsigned slot=0;slot<count;++slot){
    const auto actor=local_netplay_port>=0?mem_read32(native_party_registry()+slot*4):player;
    if(!valid(actor,0x790) || !native_party_actor_occupied(actor))continue;
    const auto inv=mem_read32(actor+0x5e8);auto& override=ammo_overrides[slot];
    const bool refill=state.options.ammo && valid(inv,0xdc) && (!override.applied || override.addr!=inv+0xd8);
    override.set(valid(inv,0xdc)?inv+0xd8:0,state.options.ammo);
    if(!state.options.weapons)granted_weapons_inventory[slot]=0;
    if(valid(inv,0xdc) && (refill || (state.options.weapons && granted_weapons_inventory[slot]!=inv))){
      for(unsigned type=0;type<18;++type){const auto max=mem_read16(inv+0xb4+type*2);mem_write16(inv+type*2,max);mem_write16(inv+0x24+type*2,max);}
      if(state.options.weapons)granted_weapons_inventory[slot]=inv;
    }
  }
  auto pending = std::move(requests);
  requests.clear();
  for (auto req : pending) {
    bool universal = (req.id & 0xC0000000u) != 0;
    bool universal_enemy = (req.id & 0x80000000u) != 0;
    unsigned universal_type = req.id & 0x3FFFFFFFu;
    auto it = std::find_if(recipes.begin(), recipes.end(),
                           [&](auto &r) { return r.id == req.id; });
    if (!universal &&
        (it == recipes.end() || it->level != mem_read32(game_address(0x802DA5B8)))) {
      state.status = "Template expired; select one from the current level.";
      continue;
    }
    if (universal && universal_enemy) {
      it = std::find_if(recipes.begin(), recipes.end(), [&](auto &recipe) {
        return recipe.enemy && recipe.level == mem_read32(game_address(0x802DA5B8)) &&
               recipe.type < 61 &&
               mem_read32(game_address(0x8021AB2C) + recipe.type * 4) == universal_type;
      });
      if (it == recipes.end()) {
        state.status = "Enemy model absent from this level. Cross-level enemy "
                       "loading is not supported yet.";
        continue;
      }
      universal = false;
    }
    Recipe r{};
    if (!universal)
      r = *it;
    else {
      r.level = mem_read32(game_address(0x802DA5B8));
      r.enemy = universal_enemy;
      r.type = universal_type;
      for (unsigned i = 0; i < 12; i++)
        r.matrix[i] = f2u(i == 0 || i == 5 || i == 10 ? 1.f : 0.f);
      if (!valid(r.level)) {
        state.status = "No active level.";
        continue;
      }
      if (r.enemy) {
        unsigned prototype = ~0u;
        for (unsigned i = 0; i < 61; i++)
          if (mem_read32(game_address(0x8021AB2C) + i * 4) == r.type) {
            prototype = i;
            break;
          }
        if (prototype == ~0u) {
          state.status = "This enemy has no native spawn prototype.";
          continue;
        }
        r.type = prototype;
      }
    }
    unsigned sp = ctx.r[1];
    if (sp < 0x80000200 || !valid(sp - 0x200, 0x200)) {
      state.status = "No valid game stack.";
      continue;
    }
    const auto spawn_player=req.source_slot>=0?mem_read32(native_party_registry()+unsigned(req.source_slot)*4):player;
    if(!valid(spawn_player,0x790) || mem_read32(spawn_player+0x48)!=game_address(0x80212268u))continue;
    float yaw = u2f(mem_read32(spawn_player + 0x290));
    unsigned placed = 0;
    for (int n = 0; n < req.count; n++) {
      // Reserve a separate guest frame below the caller's stack; never alter
      // its CPU state.
      PPCContext call = ctx;
      call.r[1] = sp - 0x200;
      unsigned mat = call.r[1] + 0x80;
      std::optional<NativePickupConfig> pickup_config;
      if (universal && !r.enemy) {
        pickup_config.emplace(call,r.type);
        r.proto=pickup_config->address();
        if(!r.proto){state.status="Pickup configuration allocation failed.";break;}
      } else if (universal) {
        r.proto = call.r[1] + 0xC0;
        for (unsigned i = 0; i < 8; i++)
          mem_write32(r.proto + i * 4, 0);
        mem_write32(r.proto, r.type);
      }
      mem_write32(call.r[1], sp);
      for (int i = 0; i < 12; i++)
        mem_write32(mat + i * 4, r.matrix[i]);
      float x = u2f(mem_read32(spawn_player + 0x20)) + std::sin(yaw) * req.distance +
                (n - (req.count - 1) * .5f) * 1.5f;
      float y = u2f(mem_read32(spawn_player + 0x24));
      float z = u2f(mem_read32(spawn_player + 0x28)) + std::cos(yaw) * req.distance;
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        break;
      mem_write32(mat + 12, f2u(x));
      mem_write32(mat + 28, f2u(y));
      mem_write32(mat + 44, f2u(z));
      call.r[3] = r.level;
      replaying = true;
      try {
        if (r.enemy) {
          call.r[4] = mat;
          call.r[5] = r.proto;
          call.r[6] = 0;
          call.r[7] = 1;
          call.r[8] = call.r[9] = call.r[10] = 0;
          original(game_address(0x8010D70C), enemy_fn, enemy_hook, call);
        } else {
          call.r[4] = r.kind;
          call.r[5] = mat;
          call.r[6] = r.proto;
          call.r[7] = r.flag;
          original(game_address(0x8010CF00), item_fn, item_hook, call);
        }
      } catch (...) {
        replaying = false;
        throw;
      }
      replaying = false;
      if (valid(call.r[3])) {
        if(pickup_config)pickup_config->transfer_to(call.r[3]);
        // Debug replay calls the original factory directly. Preserve the same
        // immutable spawn identity that the ordinary factory wrapper records.
        if(!r.enemy && valid(call.r[3],0x184))notify_native_entity({NativeEntityNotice::Event::CreatedPickup,call.r[3],mem_read32(call.r[3]+0x180)});
        notify_native_entity({NativeEntityNotice::Event::Spawned,call.r[3],0,-2,{x,y,z}});
        placed++;
      }
    }
    state.status = "Spawned " + std::to_string(placed) + " / " +
                   std::to_string(req.count) + ".";
    if (getenv("SSNE_TRACE_MODS"))
      printf("[mods-spawn] global=%d enemy=%d type=%u placed=%u/%d\n",
             int(universal), int(r.enemy), universal ? universal_type : r.type,
             placed, req.count);
  }
}
} // namespace ssne
