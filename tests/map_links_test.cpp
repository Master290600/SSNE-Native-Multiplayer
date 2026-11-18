#include "map_logic.h"
#include "owned_gc_maps.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace mapedit;
namespace fs = std::filesystem;
size_t checks = 0;
void check(bool ok, const char *why) {
  ++checks;
  if (!ok)
    throw std::runtime_error(why);
}
uint32_t u(const std::vector<uint8_t> &b, uint32_t p) {
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
void u(std::vector<uint8_t> &b, uint32_t p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    b.at(p + i) = uint8_t(v >> (24 - 8 * i));
}
void f(std::vector<uint8_t> &b, uint32_t p, float v) {
  u(b, p, std::bit_cast<uint32_t>(v));
}
std::vector<uint8_t> wrap(const std::vector<uint8_t> &b) {
  auto packed = stored_zlib(b), reloc = stored_zlib({});
  std::vector<uint8_t> out(64);
  std::memcpy(out.data(), "CLIMAX-\0", 8);
  u(out, 12, uint32_t(b.size()));
  u(out, 16, uint32_t(packed.size()));
  u(out, 20, uint32_t(reloc.size()));
  out.insert(out.end(), packed.begin(), packed.end());
  out.insert(out.end(), reloc.begin(), reloc.end());
  return out;
}
struct Fixture {
  std::vector<uint8_t> b = std::vector<uint8_t>(0x8000);
  uint32_t cursor = 0x100, last = 0;
  std::map<uint32_t, uint32_t> configs, entities;
  Fixture() {
    u(b, 0, 1);
    u(b, 4, 100);
  }
  uint32_t add(uint32_t id, uint32_t type, std::string label = {}) {
    auto p = cursor;
    cursor += 44;
    auto matrix = cursor;
    cursor += 48;
    auto cfg = cursor;
    cursor += 128;
    auto text = cursor;
    if (label.empty())
      label = "Entity " + std::to_string(id);
    cursor += (uint32_t(label.size()) + 4) & ~3u;
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
    std::copy(label.begin(), label.end(), b.begin() + text);
    configs[id] = cfg;
    entities[id] = p;
    return cfg;
  }
};
template <class F> void rejected(Document &d, F action, const char *why) {
  auto before = d.export_bytes();
  auto revision = d.structure_revision();
  bool failed = false;
  try {
    action();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed && before == d.export_bytes() &&
            revision == d.structure_revision(),
        why);
}
uint32_t entity(const Document &d, uint32_t id) {
  for (auto p = u(d.payload(), 0x40); p; p = u(d.payload(), p))
    if (u(d.payload(), p + 4) && u(d.payload(), p + 20) == id)
      return p;
  throw std::runtime_error("Entity missing");
}
void synthetic() {
  Fixture x;
  auto relay = x.add(10, 4080, "__SSNE_WAVE_HELPER_RELAY_10");
  for (unsigned i = 0; i < 10; ++i)
    u(x.b, relay + 20 + i * 4, UINT32_MAX);
  u(x.b, relay + 0x74, 1);
  u(x.b, relay, 1);
  x.add(0, 4180);
  x.add(20, 4180);
  x.add(30, 5000);
  x.add(31, 4020);
  x.add(32, 4040);
  auto area = x.add(1, 4030, "Editor Trigger 1");
  u(x.b, area + 8, 10);
  f(x.b, area + 4, 5);
  f(x.b, area + 12, .1f);
  auto sw = x.add(40, 4180);
  u(x.b, sw + 4, 0);
  x.add(41, 4180);
  u(x.b, x.entities[41] + 16, sw);
  auto other = x.add(50, 4080);
  for (unsigned i = 0; i < 10; ++i)
    u(x.b, other + 20 + i * 4, UINT32_MAX);
  u(x.b, other + 24, 32);
  u(x.b, other + 0x40, 1); // Legacy ignored Enable->spawner.
  Document d;
  d.parse(wrap(x.b));
  auto original = d.export_bytes();
  auto slots = d.logic_slots(1);
  check(slots.size() == 10 && slots.front().source == 1 &&
            slots.front().owner == 10,
        "Logical area resolves private relay outputs");
  check(logic_target_supports(4070, LogicAction::Trigger) &&
            !logic_target_supports(4070, LogicAction::Enable) &&
            !logic_target_supports(4030, LogicAction::Trigger) &&
            logic_target_supports(4030, LogicAction::Enable) &&
            !logic_target_supports(4040, LogicAction::Disable),
        "Action capabilities match original native receive handlers");
  auto graph = build_logic_graph(d);
  check(std::any_of(
            graph.edges.begin(), graph.edges.end(),
            [](auto &e) { return e.from == 1 && e.to == 10 && !e.editable; }),
        "Private area relay edge protected");
  d.add_logic_link(1, 0, LogicAction::Enable);
  auto linked = d.export_bytes();
  check(d.logic_slots(1)[0].target == 0 &&
            d.logic_slots(1)[0].action == LogicAction::Enable,
        "ID zero and action roundtrip");
  graph = build_logic_graph(d);
  check(std::any_of(graph.edges.begin(), graph.edges.end(),
                    [](auto &e) {
                      return e.from == 10 && e.to == 0 && e.slot == 20 &&
                             e.editable && e.kind == LogicEdgeKind::Enable;
                    }),
        "Physical graph edge identifies exact slot/action");
  check(d.undo() && d.export_bytes() == original && d.redo() &&
            d.export_bytes() == linked,
        "Add undo redo exact");
  d.set_logic_link(1, 20, 20, LogicAction::Disable);
  check(d.logic_slots(1)[0].target == 20 &&
            d.logic_slots(1)[0].action == LogicAction::Disable,
        "Edit exact physical slot");
  d.set_logic_link(1, 20, std::nullopt, LogicAction::Trigger);
  check(!d.logic_slots(1)[0].target && u(d.payload(), area) == 0,
        "Delete output and disable empty editor watcher");
  check(d.undo() && d.logic_slots(1)[0].target == 20,
        "Delete undo restores connection");
  rejected(
      d, [&] { d.add_logic_link(1, 20, LogicAction::Disable); },
      "Duplicate add rejects atomically");
  rejected(
      d, [&] { d.add_logic_link(1, 30, LogicAction::Trigger); },
      "Raw enemy is not a wave receiver");
  rejected(
      d, [&] { d.set_logic_link(1, 24, 32, LogicAction::Enable); },
      "Native spawner ignores Enable; new nonfunctional pair rejected");
  auto legacy = d.export_bytes();
  d.set_logic_link(50, 24, 32, LogicAction::Enable);
  check(d.export_bytes() == legacy,
        "Unchanged legacy ignored pair is preserved");
  d.set_logic_link(50, 24, std::nullopt, LogicAction::Enable);
  check(!d.logic_slots(50)[1].target,
        "Ignored legacy action pair can be deleted");
  rejected(
      d, [&] { d.add_logic_link(1, 999, LogicAction::Trigger); },
      "Missing target rejects atomically");
  rejected(
      d, [&] { d.set_logic_link(1, 8, std::nullopt, LogicAction::Trigger); },
      "Private relay field cannot be overwritten");
  rejected(
      d, [&] { d.set_logic_link(40, 4, 20, LogicAction::Enable); },
      "Fixed trigger output rejects action change");
  rejected(
      d, [&] { d.set_logic_link(1, 24, 20, static_cast<LogicAction>(9)); },
      "Unknown action rejects atomically");
  check(d.logic_slots(31).empty() && d.logic_slots(32).empty(),
        "Reference/template outputs read-only");
  auto cow_before = d.export_bytes();
  d.set_logic_link(40, 4, 20, LogicAction::Trigger);
  check(d.logic_slots(40)[0].target == 20 && d.logic_slots(41)[0].target == 0,
        "Shared configuration copy-on-write isolates another source");
  check(u(d.payload(), x.entities[40] + 16) !=
            u(d.payload(), x.entities[41] + 16),
        "Shared source config gets own pointer");
  check(d.undo() && d.export_bytes() == cow_before,
        "COW undo includes payload and relocations");
  for (auto &s : d.logic_slots(1))
    d.set_logic_link(1, s.slot, 20, LogicAction::Trigger);
  rejected(
      d, [&] { d.add_logic_link(1, 0, LogicAction::Trigger); },
      "Full controller cannot silently overwrite");
  auto full = d.export_bytes();
  Document round;
  round.parse(full);
  check(round.logic_slots(1).size() == 10, "Export/reparse connection slots");
  auto area_object = std::find_if(d.objects().begin(), d.objects().end(),
                                  [](auto &o) { return o.type == 4030; });
  auto area_index = size_t(area_object - d.objects().begin());
  d.set_logic_link(50, 20, 1, LogicAction::Enable);
  check(!d.can_erase(area_index),
        "Incoming area connection protects trigger deletion");
  rejected(
      d, [&] { d.erase(area_index); },
      "Trigger erase cannot bypass incoming connection protection");
  d.set_logic_link(50, 20, 10, LogicAction::Trigger);
  check(!d.can_erase(area_index),
        "Incoming private relay connection protects trigger deletion");
  d.set_logic_link(50, 20, std::nullopt, LogicAction::Trigger);
  check(d.can_erase(area_index),
        "Removing incoming connection permits owned trigger deletion");
}
int main(int argc, char **argv) {
  try {
    synthetic();
    if (argc < 2) {
      std::cout << "PASS maps=0 checks=" << checks << " (synthetic coverage)\n";
      return 0;
    }
    auto data = fs::u8path(argv[1]);
    auto output = argc >= 3 ? fs::u8path(argv[2])
                            : fs::temp_directory_path() / "ssne-map-links-test";
    output /= std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    size_t maps = 0, original_maps = 0, additional_maps = 0,
           wave_maps = 0, edited_types = 0,
           original_wave_maps = 0, original_edited_types = 0;
    for (auto &e : fs::directory_iterator(data / "files/Levels"))
      if (e.path().extension() == ".ssw") {
        const bool original = original_gc_map(e.path().filename().string());
        const auto edited_before=edited_types;
        Document d;
        d.load(e.path());
        auto graph = build_logic_graph(d);
        auto baseline = d.export_bytes();
        std::set<uint32_t> types;
        for (auto &edge : graph.edges)
          check(!edge.editable ||
                    (edge.slot >= 0 && edge.kind != LogicEdgeKind::Spawn &&
                     edge.kind != LogicEdgeKind::Reference),
                "Only authored activation slots editable");
        for (auto &node : graph.nodes) {
          auto slots = d.logic_slots(node.id);
          if (slots.empty() || !types.insert(node.type).second)
            continue;
          auto slot = slots.front();
          auto before = d.export_bytes();
          auto rev = d.structure_revision();
          std::optional<uint32_t> target;
          if (!slot.target) {
            auto receiver = std::find_if(
                graph.nodes.begin(), graph.nodes.end(), [&](auto &n) {
                  return n.id != slot.owner &&
                         (n.type == 4070 || n.type == 4040 || n.type == 4080);
                });
            if (receiver == graph.nodes.end())
              continue;
            target = receiver->id;
          }
          d.set_logic_link(node.id, slot.slot, target, LogicAction::Trigger);
          auto after = d.export_bytes();
          check(before != after && d.structure_revision() > rev && d.changed(),
                "Original activation change is structural and dirty");
          Document round;
          round.parse(after);
          auto read = round.logic_slots(node.id);
          auto found = std::find_if(read.begin(), read.end(), [&](auto &s) {
            return s.slot == slot.slot;
          });
          check(found != read.end() && found->target == target,
                "Original slot edit survives export reparse");
          check(d.undo() && d.export_bytes() == before && d.redo() &&
                    d.export_bytes() == after && d.undo(),
                "Original edit undo redo exact");
          ++edited_types;
        }
        check(d.export_bytes() == baseline,
              "Original map restored byte exact after link trials");
        auto enemy = std::find_if(d.objects().begin(), d.objects().end(),
                                  [](auto &o) { return o.type == 5000; });
        auto door = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                 [](auto &n) { return n.type == 4070; });
        if (enemy != d.objects().end() && door != graph.nodes.end()) {
          auto duplicate = d.duplicate(size_t(enemy - d.objects().begin()),
                                       {2.5f, -1.25f, 77.5f});
          auto offset = d.objects()[duplicate].offset;
          WaveTrigger wave;
          wave.rows = {{offset, 2}};
          wave.delay = 2;
          auto area = d.add_wave_trigger({2.5f, -.25f, 86.5f}, wave);
          auto area_id = u(d.payload(), d.objects()[area].offset + 20);
          auto slots = d.logic_slots(area_id);
          auto spawn = *slots[0].target;
          auto spawn_slot = slots[0].slot;
          d.add_logic_link(area_id, door->id, LogicAction::Trigger);
          auto control =
              std::find_if(graph.nodes.begin(), graph.nodes.end(), [](auto &n) {
                return n.type == 4080 || n.type == 4180;
              });
          if (control != graph.nodes.end())
            d.add_logic_link(area_id, control->id, LogicAction::Enable);
          wave.repeat = true;
          wave.max_activations = 4;
          wave.rows[0].count = 3;
          d.set_wave_trigger(area, wave);
          slots = d.logic_slots(area_id);
          check(slots[0].target == spawn && slots[1].target == door->id &&
                    slots[1].action == LogicAction::Trigger,
                "Wave property update preserves stable helper ID and external "
                "door action");
          if (control != graph.nodes.end())
            check(slots[2].target == control->id &&
                      slots[2].action == LogicAction::Enable,
                  "Wave property update preserves external activation mode");
          d.set_logic_link(area_id, spawn_slot, std::nullopt,
                           LogicAction::Trigger);
          auto empty = d.wave_trigger(area);
          check(empty && empty->rows.empty(),
                "Removed wave connection disappears from owned wave rows");
          check(
              u(d.payload(), u(d.payload(), offset + 16) + 4) == 1 &&
                  entity(d, spawn),
              "Connection removal leaves dormant spawner and preloaded enemy");
          empty->radius = 7;
          empty->delay = 5;
          d.set_wave_trigger(area, *empty);
          check(d.logic_slots(area_id)[1].target == door->id &&
                    u(d.payload(),
                      u(d.payload(), d.objects()[area].offset + 16)) == 1,
                "Door-only trigger stays enabled after wave property update");
          auto before_add = d.export_bytes();
          auto new_wave = d.link_wave_enemy(area_id, offset, 4);
          auto after_add = d.export_bytes();
          check(entity(d, new_wave) && d.wave_trigger(area)->rows.size() == 1 &&
                    d.wave_trigger(area)->rows[0].count == 4 &&
                    d.wave_trigger(area)->delay == 5,
                "Direct enemy connect creates a real native wave");
          auto relay_id = d.logic_slots(area_id)[0].owner;
          check(
              std::bit_cast<float>(
                  u(d.payload(),
                    u(d.payload(), entity(d, relay_id) + 16) + 0x78)) == 5 &&
                  std::bit_cast<float>(
                      u(d.payload(),
                        u(d.payload(), entity(d, new_wave) + 16) + 28)) == 0,
              "Adding a wave preserves the existing door's common relay delay");
          check(d.undo() && d.export_bytes() == before_add && d.redo() &&
                    d.export_bytes() == after_add,
                "Wave link creation undo redo is one transaction");
          rejected(
              d, [&] { d.link_wave_enemy(area_id, offset, 2); },
              "Duplicate enemy wave rejects atomically");
          if (e.path().stem() == "Rlevel1_1") {
            auto project = Project::create(output, "Connections", "Rlevel1_1");
            project.save(d);
            Document reopened;
            Project::open(project.folder, data, reopened);
            check(reopened.export_bytes() == d.export_bytes() &&
                      reopened.logic_slots(area_id)[1].target == door->id,
                  "Project save reopen retains native connection edits");
          }
          ++wave_maps;
          if(original)++original_wave_maps;
        }
        ++maps;
        if(original){++original_maps;original_edited_types+=edited_types-edited_before;}
        else ++additional_maps;
      }
    check(original_maps == 49 && maps == original_maps + additional_maps &&
              original_wave_maps > 20 && original_edited_types > 100,
          "Original map link coverage");
    std::cout << "PASS maps=" << maps << " original_maps=" << original_maps
              << " additional_maps=" << additional_maps << " wave_maps=" << wave_maps
              << " original_wave_maps=" << original_wave_maps
              << " original_edited_types=" << original_edited_types
              << " edited_types=" << edited_types << " checks=" << checks
              << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
