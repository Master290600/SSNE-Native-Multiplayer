// Production-hook contract test. The builder is a strict CPU mock; genuine
// walking/jumping against the final map is a separate native-game test.
#include "ssne/editor_scene_collision.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t owner = 0x80400000, resource = 0x80500000;
constexpr uint32_t entries = 0x80600000, node = 0x80700000;
constexpr uint32_t matrix = 0x80701000, frame = 0x80702000;
constexpr uint32_t source = 0x80703000, descriptor = 0x80704000;
constexpr uint32_t hierarchy = 0x80705000, vertices = 0x80706000;
constexpr uint32_t collider = 0x80800000, native_builder = 0x80137488;
constexpr uint32_t region = 0x80400000, region_size = 0x00410000;
constexpr std::array<std::array<uint32_t, 2>, 8> pins{{
    {native_builder, 0x9421FF90}, {0x80137B28, 0x9421FF80},
    {0x80137E4C, 0x4BFFF63D}, {0x8013462C, 0x9421FFF0},
    {0x80137A74, 0x48082405}, {0x80137A80, 0x48082FBD},
    {0x8015B29C, 0x9421FFA8}, {0x8015BC08, 0x9421FFC0}}};
unsigned checks = 0, original_calls = 0, previous_calls = 0;
uint32_t original_result = 0;
bool function_available = true;
bool with_previous = true;
std::map<uint32_t, PPCFunc> hooks;
std::array<uint8_t, sizeof(PPCContext)> native_result{};
void check(bool yes, const char* why) {
  ++checks;
  if (!yes) throw std::runtime_error(why);
}
void original(PPCContext& c) {
  ++original_calls;
  c.r[3] = original_result;
  c.r[4] = 0xbad12345;
  c.ps0[3] = -3.75;
  c.ps1[19] = 92.125;
  c.cr[2] = 15;
  c.ctr = 23;
  c.lr = 0x80137E50;
  std::memcpy(native_result.data(), &c, sizeof(c));
}
void previous_hook(PPCContext& c) {
  ++previous_calls;
  ssne_call_original(c, native_builder, original);
  // The existing hook's result must also survive the new adapter.
  c.r[5] = 0xabc13579;
  std::memcpy(native_result.data(), &c, sizeof(c));
}
const SSNEFuncEntry entry{native_builder, original, "nativeSceneColliders"};
PPCContext context() {
  PPCContext c;
  for (unsigned i = 0; i < 32; ++i) {
    c.r[i] = 100 + i;
    c.ps0[i] = i * 1.375;
    c.ps1[i] = -i * 2.75;
  }
  c.r[1] = 0x817E0000;
  c.r[2] = 0x802F22A0;
  c.r[3] = owner;
  c.r[13] = 0x802E22A0;
  c.lr = 0x80137E50;
  return c;
}
void setup_revision() {
  mem_write32(0x80000000, 0x47334250);
  for (auto p : pins) mem_write32(p[0], p[1]);
}
void transform(float x, float y, float z, double angle = 0) {
  const float cs = float(std::cos(angle)), sn = float(std::sin(angle));
  const std::array<float, 12> m{{cs*x, -sn*y, 0, 12.5f,
                               sn*x, cs*y, 0, -8.75f,
                               0, 0, z, 100.25f}};
  for (unsigned i = 0; i < m.size(); ++i) {
    mem_write32(matrix + i*4, f2u(m[i]));
    mem_write32(collider + 0x34 + i*4, f2u(m[i]));
  }
  const double sy = std::sqrt(double(m[1])*m[1] + double(m[5])*m[5] + double(m[9])*m[9]);
  mem_write32(collider + 0x94, f2u(float(sy)));
}
void setup() {
  std::memset(ssne::host_ptr(region), 0, region_size);
  setup_revision();
  original_result = 0;
  mem_write32(owner, 0x8021E660);
  mem_write32(owner + 4, resource);
  mem_write32(owner + 0x18, entries);
  mem_write32(owner + 0x1C, 1);
  mem_write32(resource, 1);
  mem_write32(resource + 4, 100);
  mem_write32(resource + 0x58, node);
  mem_write32(entries, collider);
  mem_write32(entries + 0x10, node);
  mem_write32(node + 4, matrix);
  mem_write32(node + 0x2C, frame);
  mem_write32(frame + 8, source);
  mem_write32(source + 0x28, descriptor);
  mem_write32(descriptor + 0x18, 0);
  mem_write32(descriptor + 0x28, hierarchy);
  mem_write32(descriptor + 0x2C, vertices);
  mem_write32(hierarchy + 4, f2u(3.076f));
  mem_write32(hierarchy + 8, f2u(.5f));
  mem_write32(hierarchy + 12, f2u(1.25f));
  mem_write32(hierarchy + 16, f2u(-.75f));
  mem_write16(collider + 8, 4);
  mem_write32(collider + 0xC, descriptor);
  mem_write32(collider + 0x28, 1);
  mem_write32(collider + 0x30, 1);
  mem_write32(collider + 0xB4, 0x04000000);
  transform(2, .75f, 1.5f);
}
void call(PPCContext& c) {
  const auto calls = original_calls;
  const auto chained = previous_calls;
  ssne_call(c, native_builder);
  check(original_calls == calls + 1, "Builder original call count changed");
  check(previous_calls == chained + unsigned(with_previous),
        "Preexisting hook was bypassed or called more than once");
  check(std::memcmp(&c, native_result.data(), sizeof(c)) == 0,
        "Hook altered original return context");
}
void rejects(const std::function<void(PPCContext&)>& change, const char* why) {
  setup();
  auto c = context();
  change(c);
  std::vector<uint8_t> before(region_size);
  std::memcpy(before.data(), ssne::host_ptr(region), before.size());
  call(c);
  check(std::memcmp(before.data(), ssne::host_ptr(region), before.size()) == 0, why);
}
void unchanged(float x, float y, float z, double angle, const char* why) {
  rejects([=](PPCContext&) { transform(x, y, z, angle); }, why);
}
void corrected(float x, float y, float z, double angle) {
  setup();
  transform(x, y, z, angle);
  auto c = context();
  std::vector<uint8_t> before(region_size);
  std::memcpy(before.data(), ssne::host_ptr(region), before.size());
  call(c);
  const auto actual_bits = mem_read32(collider + 0x94);
  const double actual = u2f(actual_bits);
  const double maximum = std::max({std::abs(double(x)), std::abs(double(y)), std::abs(double(z))});
  check(actual >= maximum * (1 - 1e-7), "New scalar is below maximum TRS basis");
  // CPU evidence of upward rounding: the result must enclose the exact
  // double length of every stored float matrix column.
  for (unsigned col = 0; col < 3; ++col) {
    double squared = 0;
    for (unsigned row = 0; row < 3; ++row) {
      const double v = u2f(mem_read32(matrix + row*16 + col*4));
      squared += v*v;
    }
    check(actual >= std::sqrt(squared), "Corrected scale rounded below stored basis");
  }
  check(actual < maximum * 1.001 + 1e-6, "Nonuniform bound is unnecessarily inflated");
  // Mask only the four authorized bytes. Everything else, including the
  // source collision hierarchy, matrix and native state, must be unchanged.
  std::memcpy(before.data() + collider + 0x94 - region,
              ssne::host_ptr(collider + 0x94), 4);
  check(std::memcmp(before.data(), ssne::host_ptr(region), before.size()) == 0,
        "Hook changed data other than collider scale");
}
} // namespace

namespace ssne {
uint8_t* g_ram = nullptr;
uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size() { return 0x01800000; }
uint32_t guest_ram_end() { return 0x81800000; }
uint32_t mmio_read(uint32_t, int) { throw std::runtime_error("Unexpected MMIO"); }
void mmio_write(uint32_t, uint32_t, int) { throw std::runtime_error("Unexpected MMIO"); }
void log_message(const std::string&) {}
} // namespace ssne
const SSNEFuncEntry* ssne_find_func(uint32_t address) {
  return function_available && address == entry.address ? &entry : nullptr;
}
PPCFunc* ssne_hook_slot(uint32_t address) { return &hooks[address]; }
void ssne_install_hook(uint32_t address, PPCFunc fn) { hooks[address] = fn; }
void ssne_call(PPCContext& c, uint32_t address) {
  if (address != native_builder) throw std::runtime_error("Unexpected dispatch");
  if (!SSNEOriginalCall::consume(c, address) && hooks[address]) hooks[address](c);
  else original(c);
}

int main(int argc, char** argv) {
  try {
    with_previous = !(argc == 2 && std::strcmp(argv[1], "--without-previous-hook") == 0);
    std::string error;
    check(!ssne::install_editor_scene_collision(error), "Installed without RAM");
    auto ram = std::make_unique<uint8_t[]>(ssne::guest_ram_size());
    ssne::g_ram = ram.get();
    setup();
    function_available = false;
    check(!ssne::install_editor_scene_collision(error), "Installed without original function");
    function_available = true;
    for (const auto& p : pins) {
      mem_write32(p[0], 0);
      check(!ssne::install_editor_scene_collision(error), "Installed mismatched PAL signature");
      mem_write32(p[0], p[1]);
    }
    if (with_previous) hooks[native_builder] = previous_hook;
    error = "stale installation error";
    check(ssne::install_editor_scene_collision(error), error.c_str());
    check(error.empty(), "Successful install retained an old error");
    check(hooks.size() == 1 && hooks.count(native_builder), "Installed extra hook");
    const auto installed = hooks[native_builder];
    error = "stale repeated installation error";
    check(ssne::install_editor_scene_collision(error), error.c_str());
    check(error.empty() && hooks[native_builder] == installed,
          "Repeated install retained an error or chained itself");
    corrected(2, .75f, 1.5f, 0);
    corrected(.75f, 1.5f, 2, .31);
    corrected(1.1f, .7f, .9f, .71);
    corrected(.2f, .1f, .15f, 1.13);
    unchanged(1, 1, 1, 0, "Uniform identity was written");
    unchanged(1.2f, 1.2f, 1.2f, .71, "Rotated uniform original was written");
    unchanged(-1.2f, 1.2f, 1.2f, .31, "Mirrored uniform original was written");
    unchanged(.75f, 2, 1.5f, 0, "Already conservative Y-max scale was written");
    rejects([](PPCContext& c) { c.lr = 0x80137E54; }, "Wrong caller was repaired");
    rejects([](PPCContext&) { original_result = uint32_t(-1); }, "Failed builder was repaired");
    rejects([](PPCContext& c) { c.r[2] = 0; }, "Wrong r2 context was repaired");
    rejects([](PPCContext& c) { c.r[13] = 0; }, "Wrong r13 context was repaired");
    rejects([](PPCContext& c) { c.r[3] = 0x817FFFFC; }, "Out-of-RAM owner was repaired");
    rejects([](PPCContext& c) { c.r[3] = owner + 1; }, "Unaligned owner was repaired");
    rejects([](PPCContext&) { mem_write32(0x80000000, 0); }, "Wrong PAL was repaired");
    for (const auto& p : pins)
      rejects([=](PPCContext&) { mem_write32(p[0], 0); }, "Runtime signature mismatch was repaired");
    rejects([](PPCContext&) { mem_write32(owner, 0); }, "Wrong owner class was repaired");
    rejects([](PPCContext&) { mem_write32(owner + 4, 0); }, "Missing resource was repaired");
    rejects([](PPCContext&) { mem_write32(resource, 2); }, "Wrong resource class was repaired");
    rejects([](PPCContext&) { mem_write32(resource + 4, 99); }, "Wrong resource version was repaired");
    rejects([](PPCContext&) { mem_write32(owner + 0x1C, 32769); }, "Huge model loop was accepted");
    rejects([](PPCContext&) { mem_write32(owner + 0x18, 0x817FFFFC); }, "Invalid entry array was accepted");
    rejects([](PPCContext&) { mem_write32(resource + 0x58, 0); }, "Short node chain was accepted");
    rejects([](PPCContext&) { mem_write32(node, node); }, "Cyclic node chain was accepted");
    rejects([](PPCContext&) { mem_write32(entries + 0x10, node + 48); }, "Mismatched entry node was accepted");
    rejects([](PPCContext&) { mem_write32(entries, 0x817FFFFC); }, "Invalid collider was accepted");
    rejects([](PPCContext&) { mem_write16(collider + 8, 2); }, "World BSP collider was changed");
    rejects([](PPCContext&) { mem_write16(collider + 0xA, 1); }, "Wrong collision mask was accepted");
    rejects([](PPCContext&) { mem_write32(collider + 0xB4, 0x04000001); }, "Wrong native scene index was accepted");
    rejects([](PPCContext&) { mem_write32(collider + 0x2C, 1); }, "Worldspace collider was accepted");
    rejects([](PPCContext&) { mem_write32(collider + 0x20, 0x80900000); }, "Already bound collider was changed");
    rejects([](PPCContext&) { mem_write32(collider + 0x24, 1); }, "Already queued collider was changed");
    rejects([](PPCContext&) { mem_write32(collider + 0x28, 0); }, "Clean bounds were changed");
    rejects([](PPCContext&) { mem_write32(node + 0x24, 2); }, "Disabled source collider was accepted");
    rejects([](PPCContext&) { mem_write32(node + 4, 0); }, "Missing matrix was accepted");
    rejects([](PPCContext&) { mem_write32(collider + 0x34, f2u(1)); }, "Matrix mismatch was accepted");
    rejects([](PPCContext&) { mem_write32(node + 0x2C, 0x817FFFFC); }, "Invalid frame was accepted");
    rejects([](PPCContext&) { mem_write32(frame + 8, 0); }, "Missing source was accepted");
    rejects([](PPCContext&) { mem_write32(source + 0x28, 0); }, "Missing descriptor was accepted");
    rejects([](PPCContext&) { mem_write32(collider + 0xC, descriptor + 64); }, "Wrong descriptor was accepted");
    rejects([](PPCContext&) { mem_write32(descriptor + 0x18, uint32_t(-1)); }, "Huge hierarchy root was accepted");
    rejects([](PPCContext&) { mem_write32(descriptor + 0x28, 0); }, "Missing hierarchy was accepted");
    rejects([](PPCContext&) { mem_write32(hierarchy + 4, f2u(-1)); }, "Negative local radius was accepted");
    rejects([](PPCContext&) { mem_write32(hierarchy + 8, f2u(std::numeric_limits<float>::infinity())); }, "Infinite center was accepted");
    rejects([](PPCContext&) { mem_write32(descriptor + 0x2C, 0); }, "Missing triangle vertices were accepted");
    rejects([](PPCContext&) { mem_write32(collider + 0x94, f2u(2)); }, "Unexpected old scalar was accepted");
    rejects([](PPCContext&) { mem_write32(collider + 0x94, f2u(std::numeric_limits<float>::quiet_NaN())); }, "NaN native scalar was accepted");
    rejects([](PPCContext&) { transform(0, .75f, 1.5f); }, "Singular matrix was accepted");
    rejects([](PPCContext&) { transform(10001, .75f, 1.5f); }, "Extreme scale was accepted");
    rejects([](PPCContext&) { transform(std::numeric_limits<float>::quiet_NaN(), .75f, 1.5f); }, "NaN matrix was accepted");
    rejects([](PPCContext&) {
      mem_write32(matrix + 4, f2u(.5f));
      mem_write32(collider + 0x38, f2u(.5f));
    }, "Sheared transform was accepted");
    rejects([](PPCContext&) {
      // The first collider is otherwise repairable. A broken final entry
      // must prevent any partial write to the first entry.
      mem_write32(owner + 0x1C, 2);
      mem_write32(node, node + 0x100);
      mem_write32(entries + 20 + 0x10, node + 0x100);
      mem_write32(entries + 20, collider);
    }, "Duplicate collider caused a partial write");
    rejects([](PPCContext&) {
      mem_write32(owner + 0x1C, 0);
      mem_write32(resource + 0x58, 0);
    }, "Empty scene was changed");
    rejects([](PPCContext&) { mem_write32(entries, 0); }, "Noncollidable model was changed");
    setup();
    mem_write32(node + 0x2C, 0);
    mem_write32(node + 0x10, source);
    auto c = context();
    call(c);
    check(u2f(mem_read32(collider + 0x94)) >= 2, "Static source fallback was not repaired");
    setup();
    auto* saved_ram = ssne::g_ram;
    ssne::g_ram = nullptr;
    c = context();
    call(c);
    ssne::g_ram = saved_ram;
    check(mem_read32(collider + 0x94) == f2u(.75f), "Null RAM call changed the scene");
    corrected(2, .75f, 1.5f, .31); // Reused addresses do not retain stale owners.
    std::cout << "PASS editor_scene_collision checks=" << checks
              << " original_calls=" << original_calls
              << " previous_calls=" << previous_calls << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << " checks=" << checks << '\n';
    return 1;
  }
}
