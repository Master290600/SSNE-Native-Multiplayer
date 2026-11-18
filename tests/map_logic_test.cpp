#include "map_logic.h"
#include "owned_gc_maps.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
using namespace mapedit;
size_t checks = 0;
void check(bool v, const char *m) {
  ++checks;
  if (!v)
    throw std::runtime_error(m);
}
void u(std::vector<uint8_t> &b, size_t p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    b.at(p + i) = uint8_t(n >> (24 - i * 8));
}
void f(std::vector<uint8_t> &b, size_t p, float n) {
  u(b, p, std::bit_cast<uint32_t>(n));
}
std::vector<uint8_t> wrap(const std::vector<uint8_t> &data) {
  auto packed = stored_zlib(data), relocations = stored_zlib({});
  std::vector<uint8_t> out(64);
  std::memcpy(out.data(), "CLIMAX-\0", 8);
  u(out, 12, uint32_t(data.size()));
  u(out, 16, uint32_t(packed.size()));
  u(out, 20, uint32_t(relocations.size()));
  out.insert(out.end(), packed.begin(), packed.end());
  out.insert(out.end(), relocations.begin(), relocations.end());
  return out;
}
struct Fixture {
  std::vector<uint8_t> b = std::vector<uint8_t>(0x8000);
  uint32_t cursor = 0x100, last = 0;
  std::map<uint32_t, uint32_t> configs;
  Fixture() {
    u(b, 0, 1);
    u(b, 4, 100);
  }
  uint32_t add(uint32_t id, uint32_t type, std::string name = {}) {
    auto p = cursor;
    cursor += 44;
    auto matrix = cursor;
    cursor += 48;
    auto cfg = cursor;
    cursor += 128;
    auto text = cursor;
    name = name.empty() ? "Entity " + std::to_string(id) : name;
    cursor += uint32_t(name.size() + 1);
    cursor = (cursor + 3) & ~3u;
    u(b, last ? last : 0x40, p);
    last = p;
    u(b, p + 4, matrix);
    u(b, p + 8, text);
    u(b, p + 12, type);
    u(b, p + 16, cfg);
    u(b, p + 20, id);
    f(b, matrix, 1);
    f(b, matrix + 20, 1);
    f(b, matrix + 40, 1);
    f(b, matrix + 12, float(id));
    std::copy(name.begin(), name.end(), b.begin() + text);
    configs[id] = cfg;
    return cfg;
  }
};
const LogicEdge *edge(const LogicGraph &g, uint32_t from, uint32_t to,
                      LogicEdgeKind kind) {
  auto i = std::find_if(g.edges.begin(), g.edges.end(), [&](const auto &e) {
    return e.from == from && e.to == to && e.kind == kind;
  });
  return i == g.edges.end() ? nullptr : &*i;
}
void synthetic() {
  Fixture x;
  auto area = x.add(1, 4030);
  u(x.b, area, 1);
  f(x.b, area + 4, 5);
  u(x.b, area + 8, 2);
  f(x.b, area + 12, .1f);
  auto relay = x.add(2, 4080);
  u(x.b, relay, 1);
  for (unsigned i = 0; i < 10; ++i)
    u(x.b, relay + 20 + 4 * i, UINT32_MAX);
  u(x.b, relay + 20, 3);
  u(x.b, relay + 24, 5);
  u(x.b, relay + 28, 6);
  u(x.b, relay + 32, 99);
  u(x.b, relay + 0x40, 1);
  u(x.b, relay + 0x44, 2);
  u(x.b, relay + 0x48, 3);
  u(x.b, relay + 0x64, 1);
  u(x.b, relay + 0x68, 2);
  u(x.b, relay + 0x6c, 1);
  u(x.b, relay + 0x74, 4);
  f(x.b, relay + 0x78, 2.5f);
  auto spawn = x.add(3, 4040);
  f(x.b, spawn, .25f);
  f(x.b, spawn + 4, 3);
  u(x.b, spawn + 8, 3);
  u(x.b, spawn + 12, 6);
  u(x.b, spawn + 20, 7);
  f(x.b, spawn + 28, 1.5f);
  u(x.b, spawn + 32, 3);
  u(x.b, spawn + 36, UINT32_MAX);
  auto door = x.add(4, 4070);
  f(x.b, door + 4, 2);
  u(x.b, door + 16, 1);
  u(x.b, door + 36, 5);
  x.b[door + 61] = 2;
  auto sw = x.add(5, 4180);
  u(x.b, sw, 1);
  u(x.b, sw + 4, 4);
  u(x.b, sw + 8, 1);
  auto touch = x.add(6, 4170);
  u(x.b, touch, 1);
  u(x.b, touch + 8, 2);
  u(x.b, touch + 12, 2);
  auto enemy = x.add(7, 5000);
  u(x.b, enemy + 4, 1);
  u(x.b, enemy + 8, 2);
  auto controller = x.add(8, 4050);
  const auto controller_matrix=x.last+44;
  // A rotated/scaled transform must not rotate/scale the native4050 volume.
  f(x.b,controller_matrix,0);
  f(x.b,controller_matrix+8,5);
  f(x.b,controller_matrix+32,-3);
  f(x.b,controller_matrix+40,0);
  f(x.b,controller_matrix+12,10);
  f(x.b,controller_matrix+28,20);
  f(x.b,controller_matrix+44,30);
  u(x.b, controller, 1);
  f(x.b, controller + 4, 3);
  f(x.b, controller + 8, 4);
  u(x.b, controller + 16, 4);
  u(x.b, controller + 20, 2);
  u(x.b, controller + 24, 1);
  u(x.b, controller + 32, 9);
  u(x.b, controller + 36, 3);
  auto helper = x.add(9, 4080, "__SSNE_WAVE_HELPER_RELAY_9");
  for (unsigned i = 0; i < 10; ++i)
    u(x.b, helper + 20 + i * 4, UINT32_MAX);
  auto pickup = x.add(10, 2100);
  u(x.b, pickup + 8, 2);
  auto teleport = x.add(11, 4020);
  u(x.b, teleport + 12, 12);
  x.add(12, 4140);
  x.add(13, 4990);
  auto bad = x.add(14, 4030);
  f(x.b, bad + 4, std::numeric_limits<float>::quiet_NaN());
  u(x.b, bad + 8, 2);
  Document d;
  d.parse(wrap(x.b));
  auto before = d.export_bytes();
  auto g = build_logic_graph(d);
  check(g.find(8)->box_width==4 && g.find(8)->box_height==3 &&
            g.find(8)->radius==0,
        "Native4050 full width/height must remain distinct from a sphere radius");
  const auto box=logic_box_bounds(*g.find(8),g.find(8)->position);
  check(box && box->minimum==std::array<float,3>{10,20,26} &&
            box->maximum==std::array<float,3>{14,23,30},
        "Native4050 world-aligned corner anchor/bounds differ from gameplay");
  const auto moved_box=logic_box_bounds(*g.find(8),{-1,2,3});
  check(moved_box && moved_box->minimum==std::array<float,3>{-1,2,-1} &&
            moved_box->maximum==std::array<float,3>{3,5,3},
        "Pending translation must preserve native full box dimensions");
  check(g.find(1)->radius==5 &&
            !logic_box_bounds(*g.find(1),g.find(1)->position),
        "Native4030 sphere was changed into a box");
  for(float invalid:{0.f,-1.f,10001.f,std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN()}) {
    auto bad_box=*g.find(8);bad_box.box_width=invalid;
    check(!logic_box_bounds(bad_box,bad_box.position),"Invalid native box width was visualized");
    bad_box=*g.find(8);bad_box.box_height=invalid;
    check(!logic_box_bounds(bad_box,bad_box.position),"Invalid native box height was visualized");
  }
  check(g.nodes.size() == 14,
        "Graph retains hidden helpers and unsupported entities");
  check(g.find(9) && g.find(9)->helper && g.find(9)->object == -1,
        "Helper node has stable authored identity without visible object");
  check(g.find(2)->threshold == 2 && g.find(2)->repeat_counter &&
            g.find(2)->activation_limit == 4 && g.find(2)->delay == 2.5f,
        "Counter configuration decoded exactly");
  check(edge(g, 1, 2, LogicEdgeKind::Trigger), "Area entry signal");
  check(edge(g, 2, 3, LogicEdgeKind::Trigger) &&
            edge(g, 2, 5, LogicEdgeKind::Enable) &&
            edge(g, 2, 6, LogicEdgeKind::Disable),
        "Relay target signal modes remain distinct");
  check(edge(g, 2, 99, LogicEdgeKind::Unknown) &&
            edge(g, 2, 99, LogicEdgeKind::Unknown)->missing,
        "Unknown modes and missing targets are visible, not invented triggers");
  check(g.find(3)->wave_count == 3 && g.find(3)->total_spawn_count == 6 &&
            g.find(3)->delay == 1.5f && edge(g, 3, 7, LogicEdgeKind::Spawn),
        "Wave counts, delay and template link");
  check(edge(g, 4, 5, LogicEdgeKind::Reference) &&
            edge(g, 4, 2, LogicEdgeKind::Trigger),
        "Brush interaction reference and damage target distinguished");
  check(edge(g, 5, 4, LogicEdgeKind::Trigger) &&
            edge(g, 6, 2, LogicEdgeKind::Disable),
        "Switch trigger and touch-field deactivate");
  check(edge(g, 7, 2, LogicEdgeKind::Trigger),
        "Native configured enemy death signal connects counter");
  check(edge(g, 8, 4, LogicEdgeKind::DoorEnter) &&
            edge(g, 8, 2, LogicEdgeKind::DoorEnter) &&
            edge(g, 8, 9, LogicEdgeKind::DoorDenied),
        "Door controller has two accepted-entry targets, no invented leave "
        "signal");
  check(
      std::none_of(g.edges.begin(), g.edges.end(),
                   [](auto &e) { return e.kind == LogicEdgeKind::DoorLeave; }),
      "No fabricated door leave event");
  check(g.find(8)->key_index == 3 && edge(g, 10, 2, LogicEdgeKind::Collected) &&
            edge(g, 11, 12, LogicEdgeKind::Reference),
        "Key requirements and pickup/teleport references");
  check(!g.find(13)->supported && !g.find(14)->supported &&
            !edge(g, 14, 2, LogicEdgeKind::Trigger),
        "Unsupported and malformed configs remain explicit");
  check(d.export_bytes() == before && !d.changed(),
        "Graph inspection never modifies map data");
  Fixture zero;
  auto zero_relay = zero.add(0, 4080, "Trigger ID ZERO");
  for (unsigned i = 0; i < 10; ++i)
    u(zero.b, zero_relay + 20 + i * 4, UINT32_MAX);
  auto zero_area = zero.add(30, 4030);
  u(zero.b, zero_area, 1);
  f(zero.b, zero_area + 4, 5);
  u(zero.b, zero_area + 8, 0);
  zero.add(0, 6002, "Global metadata");
  u(zero.b, zero.last + 4, 0);
  Document zero_document;
  zero_document.parse(wrap(zero.b));
  auto zero_graph = build_logic_graph(zero_document);
  check(zero_graph.nodes.size() == 2 && zero_graph.find(0) &&
            zero_graph.find(0)->type == 4080,
        "Null-transform metadata cannot shadow a real authored ID zero");
  check(edge(zero_graph, 30, 0, LogicEdgeKind::Trigger) &&
            !edge(zero_graph, 30, 0, LogicEdgeKind::Trigger)->missing,
        "ID zero is a valid target, not a no-selection sentinel");
}
int main(int argc, char **argv) {
  try {
    synthetic();
    size_t maps = 0, original_maps = 0, additional_maps = 0, nodes = 0, edges = 0;
    if (argc == 2)
      for (const auto &e : std::filesystem::directory_iterator(
               std::filesystem::path(argv[1]) / "files/Levels"))
        if (e.path().extension() == ".ssw") {
          Document d;
          d.load(e.path());
          auto before = d.export_bytes();
          auto g = build_logic_graph(d);
          check(g.nodes.size() >= d.objects().size(),
                "Owned map retains every object in graph");
          check(d.export_bytes() == before && !d.changed(),
                "Owned graph inspection byte exact");
          std::set<uint32_t> ids;
          for (auto &n : g.nodes) {
            check(ids.insert(n.id).second, "Owned entity IDs unique");
            check(n.offset < d.payload().size(),
                  "Owned node points into payload");
            if (n.object >= 0)
              check(d.objects()[n.object].offset == n.offset,
                    "Visible-object identity mapping exact");
          }
          for (auto &e : g.edges) {
            check(g.find(e.from), "Edge source exists");
            check(e.missing == !bool(g.find(e.to)),
                  "Missing target flag exact");
          }
          if (e.path().stem() == "Rlevel1_1")
            for (auto &n : g.nodes)
              if (n.type == 4080 && n.threshold > 0) {
                std::cout << "R1_COUNTER=" << n.id
                          << " threshold=" << n.threshold << " name=" << n.name
                          << "\n";
                for (auto &e : g.edges)
                  if (e.from == n.id) {
                    auto *p = g.find(e.to);
                    std::cout << "  -> " << e.to << " "
                              << logic_edge_name(e.kind) << " "
                              << (p ? p->name : "MISSING") << "\n";
                  }
              }
          ++maps;
          if (original_gc_map(e.path().filename().string()))
            ++original_maps;
          else
            ++additional_maps;
          nodes += g.nodes.size();
          edges += g.edges.size();
        }
    if (argc == 2)
      check(original_maps == 49 && maps == original_maps + additional_maps,
            "All49 original GC maps and every additional map covered");
    std::cout << "PASS maps=" << maps << " original_maps=" << original_maps
              << " additional_maps=" << additional_maps << " nodes=" << nodes
              << " edges=" << edges << " checks=" << checks << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
  }
}
