#include "ssne/mods.h"
#include "ssne/game_addresses.h"
using ssne::game_address;
#include "ssne/recomp_hooks.h"
#include "ssne/native_entity.h"
#include "ssne/native_party.h"
#include <cassert>
#include <cmath>
#include <cstdio>

#include <map>
#include <vector>
bool test_replica=false;
namespace ssne {
// Synthetic mod-hook harness: no live renderer, network session or extended party.
// These collaborators are deliberately inert; native campaign behavior is tested separately.
void install_ammo_rule_hooks() {}
void native_entity_hooks_installed(bool) {}
void notify_native_entity(NativeEntityNotice) {}
uint32_t guest_ram_size() { return kRamSize; }
uint32_t guest_ram_end() { return 0x80000000u + kRamSize; }
bool replica_simulation() { return test_replica; }
bool netplay_render_local() { return false; }
bool netplay_render_discard() { return false; }
unsigned native_party_capacity() { return 4; }
bool native_party_actor_occupied(uint32_t) { return true; }
uint32_t native_party_registry() { return game_address(0x802D8948); }
uint32_t native_party_pickup_flags(uint32_t pickup) { return pickup + 0x170; }
float test_fov=63.f,test_wide=1.f;
float fov() {return test_fov;}
float widescreen() {return test_wide;}
void log_message(const std::string&) {}
int netplay_world_resets = 0;
void netplay_reset_world() { ++netplay_world_resets; }
std::atomic<bool> g_irq_pending{false};
void safepoint(PPCContext&) {}
bool netplay_world_ready() { return true; }
uint32_t netplay_player(int) { return 0x80300000; }
uint8_t *g_ram;
uint8_t g_locked_cache[kLockedCacheSize];
uint32_t mmio_read(uint32_t, int) {
  assert(false);
  return 0;
}
void mmio_write(uint32_t, uint32_t, int) { assert(false); }
} // namespace ssne
namespace {
std::map<unsigned, PPCFunc> slots;
int spawned = 0;
unsigned factory_calls=0;
int refills = 0;
bool replay = false;
bool global_spawn = false;

void item(PPCContext &c) {
  if (replay) {
    assert(c.r[3] == 0x80310000);
    if (global_spawn) {
      assert(c.r[4] == 0 && mem_read32(c.r[6]) == 39);
    } else {
      assert(c.r[4] == 2000);
      assert(c.r[6] == 0x80320000);
    }
    assert(u2f(mem_read32(c.r[5] + 28)) == 2);
    assert(u2f(mem_read32(c.r[5] + 44)) == 10);
    spawned++;
    c.r[17] = 123;
    c.ps0[1] = 42;
    c.lr = 99;
  }
  c.r[3] = 0x80330000;
  mem_write32(c.r[3] + 0x180,
              39); // XM4000 pickup returned by the native factory.
}
void enemy(PPCContext &c) {
  ++factory_calls;
  if (replay) {
    assert(c.r[6] == 0 && c.r[7] == 1 && c.r[8] == 0 && c.r[9] == 0 &&
           c.r[10] == 0);
    spawned++;
  }
  c.r[3] = c.r[6] ? 0 : 0x80330000;
}
void shutdown(PPCContext &) {}
void create(PPCContext &c) {
  c.r[3] = 0x80360000;
  mem_write32(c.r[3] + 0x48, 0x80200000);
  mem_write32(c.r[3] + 0xCC, f2u(20));
}
void registry(PPCContext &) {}
bool nested_registration = false;
void register_entity(PPCContext& ctx) {
  SSNE_HOOK(game_address(0x80150640));
  if (nested_registration) {
    nested_registration = false;
    PPCContext child = ctx;
    child.r[4] = 0x80360000;
    register_entity(child);
  }
}
void camera(PPCContext &) {}
void bounds(PPCContext &c) {
  mem_write32(c.r[3] + 0xD0, f2u(.4f));
  mem_write32(c.r[1] - 0x50 + 0xC, f2u(-.1f));
  mem_write32(c.r[1] - 0x50 + 0x1C, f2u(.3f));
}
void ammo(PPCContext &c) {
  assert(c.r[3] == 1 && c.r[4] == 0x80300000);
  refills++;
  c.r[19] = 42;
  c.lr = 99;
}
void allocate_pickup(PPCContext& c) {
  assert(c.r[4] == 0x40);
  c.r[3] = 0x80390000;
  mem_write32(c.r[3] - 0x18, 0x40);
}
void free_pickup(PPCContext&) {}
SSNEFuncEntry entries[] = {
    {0x801160CC, allocate_pickup, ""}, {0x80116234, free_pickup, ""},
    {0x800B6698, ammo, ""},     {0x8010CF00, item, ""},
    {0x8010D70C, enemy, ""},    {0x801664D8, shutdown, ""},
    {0x8008A558, create, ""},   {0x80150640, register_entity, ""},
    {0x8015070C, registry, ""}, {0x8014C090, camera, ""},
    {0x80090580, bounds, ""},   {0x8010C228, registry, ""},
    {0x8008462C, registry, ""}};
void seed(unsigned addr, std::initializer_list<unsigned> values) {
  for (auto v : values) {
    mem_write32(addr, v);
    addr += 4;
  }
}
} // namespace
PPCFunc *ssne_hook_slot(uint32_t a) { return &slots[a]; }
void ssne_install_hook(uint32_t a, PPCFunc fn) { slots[a] = fn; }
const SSNEFuncEntry *ssne_find_func(uint32_t a) {
  for (auto &e : entries)
    if (game_address(e.address) == a ||
        (mem_read32(0x80000000) == 0x47334250u &&
         ((e.address == 0x801160CC && a == 0x80116084) ||
          (e.address == 0x80116234 && a == 0x801161EC))))
      return &e;
  return nullptr;
}
void ssne_call(PPCContext& context, uint32_t address) {
  if (auto hook = *ssne_hook_slot(address)) { hook(context); return; }
  auto entry = ssne_find_func(address);
  assert(entry && "unexpected game call in synthetic mod test");
  entry->func(context);
}
int main(int argc,char** argv) {
  std::vector<uint8_t> ram(ssne::kRamSize);
  ssne::g_ram = ram.data();
  if(argc>1) mem_write32(0x80000000,0x47334250);
  seed(game_address(0x8010CF00), {0x9421FFD0, 0x7C0802A6, 0xBF61001C});
  seed(game_address(0x8010D70C), {0x9421FF90, 0x7C0802A6, 0xBEC10048});
  seed(game_address(0x801664D8), {0x9421FFE8, 0x7C0802A6, 0xBFA1000C});
  seed(0x8008A558, {0x9421FFE8, 0x7C0802A6, 0xBF810008});
  seed(game_address(0x80150640), {0x9421FFF0, 0x7C0802A6, 0xBFC10008});
  seed(game_address(0x8015070C), {0x9421FFF0, 0x7C0802A6, 0x93E1000C});
  seed(game_address(0x8014C090), {0x9421FF10, 0x7C0802A6, 0xDAE100A8});
  seed(0x80090580, {0x9421FFB0, 0x7C0802A6, 0xDBE10048});
  seed(game_address(0x8010C228), {0x8003033C, 0x2C000000, 0x4C820020});
  seed(0x8008462C, {0x9421FFF8, 0x7C0802A6, 0x9001000C});
  ssne::install_mod_hooks();
  assert(ssne::mod_state().available);
  assert(!ssne::mod_state().active);
  constexpr unsigned player = 0x80300000, inv = 0x80340000, flag = 0x80350000;
  mem_write32(player + 0x48, game_address(0x80212268u));
  mem_write32(0x802D8948, player);
  mem_write32(game_address(0x802DA5B8), 0x80310000);
  mem_write32(player + 0x5E8, inv);
  mem_write32(game_address(0x802DA048), flag);
  mem_write32(player + 0x24, f2u(2));
  mem_write32(player + 0x28, f2u(4));
  mem_write32(game_address(0x802DA284), 7);
  mem_write32(inv + 0xD8, 3);
  for (unsigned type = 0; type < 18; ++type) {
    mem_write16(inv + type * 2, uint16_t(type + 1));
    mem_write16(inv + 0x24 + type * 2, uint16_t(type + 2));
    mem_write16(inv + 0xB4 + type * 2, uint16_t(type + 30));
  }
  PPCContext c{};
  c.r[1] = 0x81700000;
  c.r[13] = game_address(0x802E1BA0);
  ssne::mod_options({true, true, true, true});
  ssne::mod_tick(c, player);
  assert(c.r[19] == 0 && c.lr == 0);
  for (unsigned type = 0; type < 18; ++type) {
    assert(mem_read16(inv + type * 2) == type + 30);
    assert(mem_read16(inv + 0x24 + type * 2) == type + 30);
    assert(mem_read16(inv + 0xB4 + type * 2) == type + 30);
  }
  ssne::mod_tick(c, player);
  assert(mem_read16(inv) == 30 && mem_read16(inv + 0x24) == 30);
  assert(ssne::mod_state().active);
  assert(mem_read32(game_address(0x802DA284)) == 1);
  assert(mem_read32(inv + 0xD8) == 1);
  assert(mem_read32(game_address(0x802DA280)) == 1 && mem_read32(flag + 0x1C) == 1);
  ssne::mod_options({});
  ssne::mod_tick(c, player);
  assert(mem_read32(game_address(0x802DA284)) == 7 && mem_read32(inv + 0xD8) == 3);
  assert(mem_read32(flag + 0x1C) == 0);
  PPCContext factory = c;
  factory.r[3] = 0x80310000;
  factory.r[4] = 2000;
  factory.r[5] = 0x80360000;
  factory.r[6] = 0x80320000;
  slots[game_address(0x8010CF00)](factory);
  factory.r[3] = 0x80310000;
  slots[game_address(0x8010CF00)](factory);
  auto s = ssne::mod_state();
  assert(s.templates.size() == 1 && !s.templates[0].enemy);
  assert(s.templates[0].type == 39 && s.templates[0].name == "XM4000 Mini Gun");
  replay = true;
  auto before = c;
  ssne::mod_spawn(s.templates[0].id, 100, 6);
  ssne::mod_tick(c, player);
  assert(spawned == 10);
  assert(!memcmp(&before, &c, sizeof(c)));
  assert(ssne::mod_state().templates.size() == 1);
  replay = false;
  factory.r[3] = 0x80310000;
  factory.r[4] = 0x80360000;
  factory.r[5] = 0x80320000;
  factory.r[6] = 1;
  mem_write32(0x80320000,
              37); // Level prototype 37 resolves to class 18, not class 37.
  mem_write32(game_address(0x8021AB2C) + 37 * 4, 18);
  mem_write32(0x80320004, 1);
  slots[game_address(0x8010D70C)](factory);
  s = ssne::mod_state();
  assert(s.templates.size() == 2 && s.templates[1].enemy);
  assert(s.templates[1].type == 18 && s.templates[1].name == "Kleer Knight");
  assert(ssne::mod_type_name(false, 55) == "Serious Bomb");
  assert(ssne::mod_type_name(true, 40) == "Diablotaur");
  assert(ssne::mod_type_name(true, 999) == "Unknown enemy (type 999)");
  for (size_t i = 0; i < ssne::mod_catalog().size(); i++) {
    const auto &a = ssne::mod_catalog()[i];
    assert(!a.name.empty());
    for (size_t j = i + 1; j < ssne::mod_catalog().size(); j++) {
      const auto &b = ssne::mod_catalog()[j];
      if (!a.availability_note && !b.availability_note)
        assert(a.enemy != b.enemy || a.type != b.type);
    }
  }
  replay = true;
  ssne::mod_spawn(s.templates[1].id, 1, 6);
  ssne::mod_tick(c, player);
  assert(spawned == 11);
  // Real lifecycle hooks, perspective projection, activation and dead-target
  // rejection.
  assert(ssne::mod_state().targeting_available);
  ssne::ModOptions aim;
  aim.aimbot = aim.esp = true;
  aim.aim_height = 0;
  ssne::mod_options(aim);
  PPCContext call = c;
  call.r[3] = 18;
  slots[0x8008A558](call);
  call.r[4] = call.r[3];
  slots[game_address(0x80150640)](call);
  mem_write32(0x80360000 + 0x20, f2u(2));
  mem_write32(0x80360000 + 0x28, f2u(-10));
  // Native rendering uses an independent root, not the collision position.
  auto root_matrix=[](unsigned address,const std::array<float,12>& root){for(unsigned i=0;i<12;++i)mem_write32(address+i*4,f2u(root[i]));};
  mem_write32(0x80360000+0xFC,0x80379100);mem_write32(0x80379104,0x80379000);
  root_matrix(0x80379000,{1,0,0,2,0,1,0,0,0,0,1,-10});
  for (unsigned i = 0; i < 12; i++)
    mem_write32(game_address(0x80291A54) + i * 4,
                f2u(i == 0 || i == 5 || i == 10 ? 1.f : 0.f));
  mem_write32(game_address(0x80291BE0), f2u(1));
  mem_write32(game_address(0x80291BE0) + 20, f2u(1));
  mem_write32(player + 0x194, 0x80370000);
  mem_write32(player + 0x198, 0x80370040);
  mem_write32(0x80370024, f2u(-80));
  mem_write32(0x80370064, f2u(80));
  slots[game_address(0x8014C090)](call);
  ssne::mod_aim_input(false, true);
  ssne::mod_tick(c, player);
  auto marks = ssne::mod_markers();
  assert(marks.size() == 1 && marks[0].target && marks[0].type == 18);
  assert(std::abs(marks[0].x - .6f) < .001f &&
         std::abs(marks[0].y - .5f) < .001f);
  assert(u2f(mem_read32(player + 0x290)) == 0);
  assert(marks[0].health == 20 && marks[0].max_health == 20);
  mem_write32(0x80360000 + 0xCC, f2u(5));
  ssne::mod_tick(c, player);
  assert(ssne::mod_markers().at(0).health == 5);
  assert(ssne::mod_markers().at(0).max_health == 20);
  mem_write32(0x80360000 + 0xCC, f2u(20));
  ssne::mod_aim_input(true, true);
  ssne::mod_tick(c, player);
  assert(u2f(mem_read32(player + 0x290)) > 0 && c.r[19] == 0 && c.lr == 0);
  // Each peer captures only its own camera, selected by the native view slot.
  // Netplay aiming emits
  // once-only input deltas without modifying guest player angles locally.
  for (int port = 0; port < 2; ++port) {
    ssne::mod_netplay_player(port);
    mem_write32(call.r[13] - 0x6D94, unsigned(port));
    call.r[6] = 0x80378000;
    mem_write32(call.r[6] + 4, f2u(port ? .5f : 0.f));
    mem_write32(player + 0x290, 0);
    mem_write32(player + 0x294, 0);
    slots[game_address(0x8014C090)](call);
    assert(mem_read32(player + 0x290) == 0 && mem_read32(player + 0x294) == 0);
    double yaw = 0, pitch = 0;
    ssne::mod_take_netplay_aim(yaw, pitch);
    assert(yaw > 0);
    assert(std::abs(ssne::mod_markers().at(0).x - .6f) < .001f);
    ssne::mod_take_netplay_aim(yaw, pitch);
    assert(yaw == 0 && pitch == 0);
    mem_write32(call.r[6] + 4, f2u(port ? 0.f : .5f));
    mem_write32(call.r[13] - 0x6D94, unsigned(1-port));
    slots[game_address(0x8014C090)](call);
    ssne::mod_take_netplay_aim(yaw, pitch);
    assert(yaw == 0 && pitch == 0);
  }
  // Native local views are full-screen before the GPU applies widescreen/FOV.
  // Compare markers with an independent canonical 640x448 viewport mapping.
  ssne::mod_netplay_player(0);mem_write32(call.r[13]-0x6D94,0);
  for(float wide:{1.f,4.f/3.f,1.65f})for(float field:{63.f,90.f}) {
    ssne::test_wide=wide;ssne::test_fov=field;
    slots[game_address(0x8014C090)](call);
    const float scale=std::tan(63.f*.00872664626f)/std::tan(field*.00872664626f);
    const float pixel=320.f+(320.f/wide*scale)*2.f/10.f;
    assert(std::abs(ssne::mod_markers().at(0).x*640.f-pixel)<.001f);
  }
  ssne::test_wide=1;ssne::test_fov=63;
  ssne::mod_netplay_player(-1);
  slots[game_address(0x8014C090)](call);
  aim.aim_height = 1;
  ssne::mod_options(aim);
  mem_write32(0x80360000 + 0xD0, f2u(.4f));
  ssne::mod_tick(c, player);
  assert(std::abs(ssne::mod_markers().at(0).y - .49f) < .001f);
  mem_write32(0x80360000 + 0xD0, f2u(12.f));
  ssne::mod_tick(c, player);
  assert(std::abs(ssne::mod_markers().at(0).y - .2f) < .001f);
  aim.auto_aim_height = false;
  ssne::mod_options(aim);
  ssne::mod_tick(c, player);
  assert(std::abs(ssne::mod_markers().at(0).y - .45f) < .001f);
  call = c;
  call.r[3] = 0x80360000;
  slots[0x80090580](call);
  aim.auto_aim_height = true;
  ssne::mod_options(aim);
  ssne::mod_tick(c, player);
  assert(std::abs(ssne::mod_markers().at(0).y - .495f) < .001f);
  // Named animation bones, world transforms, fallback, CPU preservation.
  const unsigned proxy = 0x80410000, model = 0x80411000, resource = 0x80412000,
                 data = 0x80413000, matrices = 0x80414000, nodes = 0x80415000,
                 names = 0x80416000;
  mem_write32(0x80360000 + 0xFC, proxy);
  mem_write32(proxy + 0x20, model);
  constexpr unsigned render_root=0x80418000;mem_write32(proxy+4,render_root);
  root_matrix(render_root,{1,0,0,2,0,1,0,0,0,0,1,-10});
  mem_write32(0x80360000 + 0x20, f2u(2));
  mem_write32(0x80360000 + 0x28, f2u(-10));
  for (unsigned i = 0; i < 3; i++)
    mem_write32(0x80360000 + 0x68 + i * 4, f2u(1));
  mem_write32(model + 0xC, resource);
  mem_write32(resource + 0xC, data);
  mem_write32(data + 0x30, nodes);
  mem_write32(model + 0x78, matrices);
  mem_write32(model + 0x7C, 3);
  const char *bone_names[] = {"chest", "head", "left_hand"};
  for (unsigned i = 0; i < 3; i++) {
    mem_write32(nodes + i * 64, i < 2 ? nodes + (i + 1) * 64 : 0);
    mem_write32(nodes + i * 64 + 4, names + i * 96);
    unsigned vertices = 0x80417000 + i * 12;
    mem_write32(nodes + i * 64 + 0xC, 1);
    mem_write32(nodes + i * 64 + 0x10, vertices);
    // Nonzero bind pose: a raw-translation implementation projects incorrectly.
    mem_write32(vertices, f2u(3));
    mem_write32(vertices + 4, f2u(2));
    for (unsigned axis = 0; axis < 3; ++axis)
      mem_write32(matrices + i * 48 + axis * 16 + axis * 4, f2u(1));
    for (unsigned k = 0; bone_names[i][k]; k++)
      mem_write8(names + i * 96 + k, bone_names[i][k]);
    mem_write32(matrices + i * 48 + 12, f2u(i == 2 ? -5.f : -3.f));
    mem_write32(matrices + i * 48 + 28, f2u(i == 1   ? -1.6f
                                            : i == 0 ? -1.8f
                                                     : -2.f));
  }
  aim.bone_esp = true;
  aim.aim_point = 2;
  ssne::mod_options(aim);
  ssne::mod_aim_input(false, true);
  auto cpu = c;
  ssne::mod_tick(c, player);
  assert(!memcmp(&cpu, &c, sizeof(c)));
  auto bones = ssne::mod_bone_markers();
  assert(bones.size() == 3);
  assert(bones[1].name == "head" && bones[1].target);
  assert(std::abs(ssne::mod_markers()[0].y - .48f) < .001f);
  // The selected head point is (2,.4,-10) after skinning the nonzero bind
  // center. Display FOV/aspect changes its pixel location, never the physical
  // aiming angle. Both local peer slots emit the correction once as input,
  // without changing the authoritative actor's RAM angles.
  ssne::mod_aim_input(true,true);
  for(int port:{0,1})for(float wide:{1.f,4.f/3.f})for(float field:{63.f,90.f}) {
    ssne::mod_netplay_player(port);mem_write32(call.r[13]-0x6D94,unsigned(port));
    ssne::test_wide=wide;ssne::test_fov=field;
    const auto yaw_before=mem_read32(player+0x290),pitch_before=mem_read32(player+0x294);
    slots[game_address(0x8014C090)](call);
    assert(mem_read32(player+0x290)==yaw_before && mem_read32(player+0x294)==pitch_before);
    const auto projected=ssne::mod_bone_markers();assert(projected.size()==3 && projected[1].target);
    const float scale=std::tan(63.f*.00872664626f)/std::tan(field*.00872664626f);
    assert(std::abs(projected[1].x*640.f-(320.f+320.f/wide*scale*2.f/10.f))<.001f);
    assert(std::abs(projected[1].y*448.f-(224.f-224.f*scale*.4f/10.f))<.001f);
    const float blend=1-std::exp(-aim.aim_speed/59.94f);
    double yaw=0,pitch=0;ssne::mod_take_netplay_aim(yaw,pitch);
    assert(std::abs(yaw-std::atan2(2.f,10.f)*blend)<.000001);
    assert(std::abs(pitch-std::atan2(.4f,std::hypot(2.f,10.f))*blend)<.000001);
    ssne::mod_take_netplay_aim(yaw,pitch);assert(yaw==0 && pitch==0);
  }
  ssne::test_wide=1;ssne::test_fov=63;ssne::mod_netplay_player(-1);
  ssne::mod_aim_input(false,true);slots[game_address(0x8014C090)](call);
  // Renderer root includes nonuniform scale, quarter-turn rotation and a
  // translation deliberately different from actor collision position/Euler.
  // The head's skinned local center (0,.4,0) renders at (5,-.8,-20).
  root_matrix(render_root,{0,0,4,5,0,3,0,-2,-2,0,0,-20});
  mem_write32(0x80360000+0x20,f2u(40));mem_write32(0x80360000+0x24,f2u(60));mem_write32(0x80360000+0x28,f2u(-100));
  mem_write32(0x80360000+0x2C,f2u(.7f));mem_write32(0x80360000+0x30,f2u(.9f));mem_write32(0x80360000+0x34,f2u(.4f));
  for(unsigned axis=0;axis<3;++axis)mem_write32(0x80360000+0x68+axis*4,f2u(9.f-axis));
  aim.aim_point=2;ssne::mod_options(aim);ssne::mod_tick(c,player);
  auto rendered_bones=ssne::mod_bone_markers();assert(rendered_bones.size()==3 && rendered_bones[1].target);
  assert(std::abs(rendered_bones[1].x-.625f)<.00001f && std::abs(rendered_bones[1].y-.52f)<.00001f);
  assert(std::abs(ssne::mod_markers()[0].x-.625f)<.00001f && std::abs(ssne::mod_markers()[0].y-.52f)<.00001f);
  ssne::mod_netplay_player(1);mem_write32(call.r[13]-0x6D94,1);ssne::mod_aim_input(true,true);
  slots[game_address(0x8014C090)](call);double rendered_yaw=0,rendered_pitch=0;ssne::mod_take_netplay_aim(rendered_yaw,rendered_pitch);
  const float rendered_blend=1-std::exp(-aim.aim_speed/59.94f);
  assert(std::abs(rendered_yaw-std::atan2(5.f,20.f)*rendered_blend)<.000001);
  assert(std::abs(rendered_pitch-std::atan2(-.8f,std::hypot(5.f,20.f))*rendered_blend)<.000001);
  ssne::mod_netplay_player(-1);ssne::mod_aim_input(false,true);
  // Model-center aiming must share the same native root/pivot as bones.
  aim.aim_point=0;ssne::mod_options(aim);ssne::mod_tick(c,player);
  assert(std::abs(ssne::mod_markers()[0].x-.625f)<.00001f && std::abs(ssne::mod_markers()[0].y-.5425f)<.00001f);
  mem_write32(proxy+4,0);ssne::mod_tick(c,player);assert(ssne::mod_markers().empty() && ssne::mod_bone_markers().empty());mem_write32(proxy+4,render_root);
  mem_write32(0x80360000+0x20,f2u(2));mem_write32(0x80360000+0x24,0);mem_write32(0x80360000+0x28,f2u(-10));
  for(unsigned axis=0;axis<3;++axis){mem_write32(0x80360000+0x2C+axis*4,0);mem_write32(0x80360000+0x68+axis*4,f2u(1));}
  root_matrix(render_root,{1,0,0,2,0,1,0,0,0,0,1,-10});slots[game_address(0x8014C090)](call);
  aim.aim_point = 1;
  ssne::mod_options(aim);
  ssne::mod_tick(c, player);
  assert(ssne::mod_bone_markers()[2].target &&
         std::abs(ssne::mod_markers()[0].x - .5f) < .001f);
  aim.aim_point = 3;
  ssne::mod_options(aim);
  ssne::mod_tick(c, player);
  assert(ssne::mod_bone_markers()[0].target);
  aim.aim_point = 4;
  aim.aim_bone = 1;
  ssne::mod_options(aim);
  ssne::mod_tick(c, player);
  assert(ssne::mod_bone_markers()[1].target);
  aim.aim_bone = 127;
  ssne::mod_options(aim);
  ssne::mod_tick(c, player);
  assert(std::abs(ssne::mod_markers()[0].y - .495f) < .001f);
  mem_write32(0x80360000 + 0x30, f2u(1.570796327f));
  root_matrix(render_root,{0,0,1,2,0,1,0,0,-1,0,0,-10});
  aim.aim_point = 1;
  ssne::mod_options(aim);
  ssne::mod_tick(c, player);
  assert(std::abs(ssne::mod_bone_markers()[2].x - .625f) < .001f);
  mem_write32(0x80360000 + 0x30, 0);
  root_matrix(render_root,{1,0,0,2,0,1,0,0,0,0,1,-10});
  mem_write32(model + 0x7C, 9999);
  ssne::mod_tick(c, player);
  assert(ssne::mod_bone_markers().empty() &&
         std::abs(ssne::mod_markers()[0].y - .495f) < .001f);
  aim.aim_point = 0;
  aim.bone_esp = false;
  ssne::mod_options(aim); // Items are independent of enemy ESP, retain each
                          // category, hide collected
  // and discovered objects, and remove markers through native lifecycle hooks.
  ssne::ModOptions esp;
  esp.item_esp = true;
  for (unsigned i = 0; i < 3; i++) {
    unsigned p = 0x80400000 + i * 0x200;
    mem_write32(p + 0x48, game_address(0x80213D48));
    mem_write32(p + 0x180, i == 0 ? 39 : i == 1 ? 4 : 9);
    mem_write32(p + 0x28, f2u(-10));
    call.r[4] = p;
    slots[game_address(0x80150640)](call);
  }
  unsigned secret = 0x80401000, desc = 0x80402000;
  mem_write32(secret + 0x48, 0x802097D8);
  mem_write32(secret + 0x54, desc);
  mem_write32(desc + 0x7C, 1);
  mem_write32(secret + 0x28, f2u(-10));
  call.r[4] = secret;
  slots[game_address(0x8010C228)](call);
  ssne::mod_options(esp);
  slots[game_address(0x8014C090)](call);
  ssne::mod_tick(c, player);
  assert(ssne::mod_markers().empty() && ssne::mod_item_markers().size() == 4);
  esp.item_weapons = false;
  esp.item_health = false;
  esp.item_armor = false;
  ssne::mod_options(esp);
  ssne::mod_tick(c, player);
  assert(ssne::mod_item_markers().size() == 1 &&
         ssne::mod_item_markers()[0].kind == ssne::ModItemKind::Secret);
  mem_write32(secret + 0xA4, 1);
  ssne::mod_tick(c, player);
  assert(ssne::mod_item_markers().empty());
  esp.item_weapons = true;
  ssne::mod_options(esp);
  // Destroy-on-pickup is configuration: it must not hide an available item.
  mem_write32(0x80400000 + 0xE8, 1);
  ssne::mod_tick(c, player);
  assert(ssne::mod_item_markers().size() == 1);
  // Each native collection path hides weapons, health and armor. Objects may
  // remain registered; clearing the respawn state makes them visible again.
  for (unsigned i = 0; i < 3; ++i) {
    esp.item_weapons = i == 0;
    esp.item_health = i == 1;
    esp.item_armor = i == 2;
    ssne::mod_options(esp);
    unsigned pickup = 0x80400000 + i * 0x200;
    for (unsigned flag : {0x104u, 0x11Cu, 0x170u}) {
      mem_write32(pickup + flag, 1);
      ssne::mod_tick(c, player);
      assert(ssne::mod_item_markers().empty());
      mem_write32(pickup + flag, 0);
      ssne::mod_tick(c, player);
      assert(ssne::mod_item_markers().size() == 1);
    }
  }
  esp.item_weapons = true;
  esp.item_health = esp.item_armor = false;
  ssne::mod_options(esp);
  // Only the current player's retained-pickup flag applies.
  mem_write32(0x80400000 + 0x174, 1);
  ssne::mod_tick(c, player);
  assert(ssne::mod_item_markers().size() == 1);
  mem_write32(player + 0x17C, 1);
  ssne::mod_tick(c, player);
  assert(ssne::mod_item_markers().empty());
  mem_write32(player + 0x17C, 0);
  mem_write32(0x80400000 + 0x174, 0);
  call.r[4] = 0x80400000;
  slots[game_address(0x8015070C)](call);
  ssne::mod_tick(c, player);
  assert(ssne::mod_item_markers().empty());
  mem_write32(secret + 0xA4, 0);
  call.r[3] = secret;
  slots[0x8008462C](call);
  ssne::mod_tick(c, player);
  assert(ssne::mod_item_markers().empty());
  ssne::mod_options(aim);
  mem_write32(0x80360000 + 0xCC, f2u(0));
  ssne::mod_tick(c, player);
  assert(ssne::mod_markers().empty());
  mem_write32(0x80360000 + 0xCC, f2u(20));
  call.r[4] = 0x80360000;
  slots[game_address(0x8015070C)](call);
  ssne::mod_tick(c, player);
  assert(ssne::mod_state().tracked_enemies == 0);
  ssne::mod_options({});
  mem_write32(player + 0x290, 0);
  // Multiple fire modes: scale from the original rate, never compound, and
  // restore on disable, inventory removal, multiplayer entry and shutdown.
  unsigned weapon = 0x80390000, modes = 0x80391000;
  mem_write32(player + 0x4BC, weapon);
  mem_write32(player + 0x4C8, 1);
  mem_write32(weapon, 0x80201000);
  mem_write32(weapon + 0x1BC, 2);
  mem_write32(weapon + 0x1D0, modes);
  mem_write32(modes + 0x18, f2u(400)); // Range must not change.
  mem_write32(modes + 0x1C, f2u(8));
  mem_write32(modes + 60 + 0x1C, f2u(2));
  ssne::ModOptions rapid;
  rapid.rapid_fire = true;
  rapid.fire_multiplier = 4;
  ssne::mod_options(rapid);
  ssne::mod_tick(c, player);
  assert(u2f(mem_read32(modes + 0x1C)) == 32);
  assert(u2f(mem_read32(modes + 0x18)) == 400);
  assert(u2f(mem_read32(modes + 60 + 0x1C)) == 8);
  ssne::mod_tick(c, player);
  assert(u2f(mem_read32(modes + 0x1C)) == 32);
  assert(u2f(mem_read32(modes + 0x18)) == 400);
  rapid.fire_multiplier = 10;
  ssne::mod_options(rapid);
  ssne::mod_tick(c, player);
  assert(u2f(mem_read32(modes + 0x1C)) == 80);
  rapid.rapid_fire = false;
  ssne::mod_options(rapid);
  assert(u2f(mem_read32(modes + 0x1C)) == 8);
  rapid.rapid_fire = true;
  ssne::mod_options(rapid);
  ssne::mod_tick(c, player);
  mem_write32(player + 0x4C8, 0);
  ssne::mod_tick(c, player);
  assert(u2f(mem_read32(modes + 0x1C)) == 8);
  mem_write32(player + 0x4C8, 1);
  ssne::mod_tick(c, player);
  mem_write32(game_address(0x802DB1BC), 2);
  ssne::mod_tick(c, player);
  assert(u2f(mem_read32(modes + 0x1C)) == 8);
  mem_write32(game_address(0x802DB1BC), 0);
  ssne::mod_tick(c, player);
  slots[game_address(0x801664D8)](c);
  assert(ssne::netplay_world_resets == 1);
  assert(u2f(mem_read32(modes + 0x1C)) == 8);
  ssne::mod_options({});
  mem_write32(game_address(0x802DB1BC), 2);
  ssne::mod_options({true, true, true, true});
  ssne::mod_tick(c, player);
  assert(!ssne::mod_state().active && mem_read32(game_address(0x802DA284)) == 7);
  mem_write32(game_address(0x802DB1BC), 0);
  ssne::mod_tick(c, player);
  assert(mem_read32(game_address(0x802DA284)) == 1);
  slots[game_address(0x801664D8)](c);
  assert(ssne::mod_state().templates.empty());
  assert(ssne::mod_item_markers().empty() && ssne::mod_bone_markers().empty());
  assert(mem_read32(game_address(0x802DA284)) == 7);
  ssne::mod_spawn(s.templates[0].id, 1, 6);
  ssne::mod_tick(c, player);
  assert(spawned == 11);
  global_spawn = true;
  // Refuse absent enemy models before invoking a factory; weapon pickups
  // remain available after every borrowed level template has expired.
  ssne::mod_spawn_type(true, 18, 1, 6);
  ssne::mod_tick(c, player);
  assert(spawned == 11);
  assert(ssne::mod_state().status.find("Enemy model absent") !=
         std::string::npos);
  ssne::mod_spawn_type(false, 39, 1, 6);
  ssne::mod_tick(c, player);
  assert(spawned == 12 && c.r[19] == 0 && c.lr == 0);
  ssne::mod_spawn_type(true, 999, 1, 6);
  ssne::mod_tick(c, player);
  assert(spawned == 12);
  // Local targeting changes cannot overwrite host cheats, and new host
  // controls cannot overwrite the client's targeting preferences.
  ssne::mod_options({});
  ssne::mod_shared_options(3, 4);
  ssne::ModOptions local;
  local.aimbot = local.esp = local.item_esp = true;
  local.aim_fov = 42;
  ssne::mod_local_options(local);
  auto separated = ssne::mod_current_options();
  assert(separated.god && separated.ammo && separated.fire_multiplier == 4);
  assert(separated.aimbot && separated.esp && separated.item_esp && separated.aim_fov == 42);
  ssne::mod_shared_options(0, 2);
  separated = ssne::mod_current_options();
  assert(!separated.god && !separated.ammo && separated.fire_multiplier == 2);
  assert(separated.aimbot && separated.esp && separated.item_esp && separated.aim_fov == 42);
  // A pickup registration may synchronously register spawned enemies. The
  // outer original call must not disable tracking of its nested registrations.
  PPCContext created = c;
  created.r[3] = 12;
  slots[0x8008A558](created);
  auto before_registration = ssne::netplay_world_checksum();
  nested_registration = true;
  created.r[4] = 0x80330000; // outer pickup, not the newly created enemy
  slots[game_address(0x80150640)](created);
  assert(!nested_registration);
  assert(ssne::netplay_world_checksum() != before_registration);
  // Authored factory multiplication must preserve its caller and leave bosses,
  // preload-only calls and replica construction singular.
  ssne::mod_options({});replay=false;
  constexpr unsigned matrix=0x80400000,prototype=0x80400100,stack=0x80500000;
  for(unsigned i=0;i<12;++i)mem_write32(matrix+i*4,f2u(float(i+1)));
  mem_write32(prototype,3);mem_write32(prototype+4,77);
  for(unsigned multiplier=1;multiplier<=5;++multiplier)for(unsigned type:{0u,5u,39u,40u,42u,60u})for(bool replica:{false,true})for(unsigned preload:{0u,1u}){
    ssne::set_enemy_multiplier(multiplier);test_replica=replica;
    mem_write32(game_address(0x8021AB2C)+12,type);
    PPCContext factory{};factory.r[1]=stack;factory.r[3]=0x80310000;factory.r[4]=matrix;factory.r[5]=prototype;factory.r[7]=preload;factory.r[19]=0x12345678;
    mem_write32(stack-0x200,0x5a5a5a5a);const auto before=factory_calls;
    slots[game_address(0x8010D70C)](factory);
    const auto expected=!replica && preload && (type<=1 || (type>=5 && type<=39))?multiplier:1;
    assert(factory_calls-before==expected && factory.r[3]==0x80330000 && factory.r[19]==0x12345678);
    assert(mem_read32(prototype+4)==77 && mem_read32(stack-0x200)==0x5a5a5a5a);
  }
  test_replica=false;ssne::set_enemy_multiplier(1);
  puts("mods: enemy factors 1..5, boss/preload/replica exclusions, caller preservation, nested enemy registration, local/shared options, cheat restoration, multiplayer restriction, template lifetime, "
       "count cap and CPU "
       "preservation passed");
}
