#include "map_document.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
using namespace mapedit;
namespace fs = std::filesystem;
int checks = 0;
void check(bool yes, const char *m) {
  ++checks;
  if (!yes)
    throw std::runtime_error(m);
}
uint32_t u(const std::vector<uint8_t> &b, size_t p) {
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
void fails(auto f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, "Protected operation must fail");
}
int main(int argc, char **argv) {
  try {
    check(argc == 3, "Usage: map_structure_test game_data evidence");
    auto data = fs::u8path(argv[1]),
         out = fs::u8path(argv[2]) /
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(out);
    int maps = 0, clones = 0;
    for (auto &e : fs::directory_iterator(data / "files/Levels"))
      if (e.path().extension() == ".ssw") {
        Document d;
        d.load(e.path());
        auto initial = d.export_bytes();
        auto n = d.objects().size();
        std::set<uint32_t> tested;
        for (size_t i = 0; i < n; ++i) {
          auto o = d.objects()[i];
          if (!d.can_duplicate(i) || !tested.insert(o.type).second)
            continue;
          auto m = d.matrix(i);
          auto at = d.duplicate(i, {m[3] + 2, m[7] + 1, m[11] + 3});
          check(at >= n && d.objects()[at].added,
                "New record appended with editor metadata");
          check(d.matrix(at)[3] == m[3] + 2, "Placement coordinate");
          check(u(d.payload(), d.objects()[at].offset + 20) !=
                    u(d.payload(), o.offset + 20),
                "Unique entity ID");
          if (d.objects()[at].type == 5000) {
            auto c = u(d.payload(), d.objects()[at].offset + 16);
            check(u(d.payload(), c + 4) == 0,
                  "New enemy is active instead of preload-only");
            check(u(d.payload(), c + 8) ==
                      u(d.payload(), d.objects()[at].offset + 20),
                  "Enemy ID follows placement");
          }
          auto wire = d.export_bytes();
          Document re;
          re.parse(wire);
          check(re.objects().size() == d.objects().size() &&
                    re.payload() == d.payload(),
                "Structural native container roundtrip");
          check(d.undo() && d.export_bytes() == initial,
                "Undo restores exact original bytes including relocations");
          check(d.redo() && d.export_bytes() == wire,
                "Redo restores exact clone bytes");
          auto count = d.objects().size();
          check(d.can_erase(at), "New independent entity deletable");
          d.erase(at);
          check(d.objects().size() == count - 1, "Deletion unlinks entity");
          Document deleted;
          deleted.parse(d.export_bytes());
          check(deleted.objects().size() == count - 1,
                "Deleted native export roundtrip");
          check(d.undo() && d.export_bytes() == wire,
                "Deletion undo restores model/pointers");
          check(d.redo() && d.undo(), "Deletion redo and undo");
          check(d.undo() && d.export_bytes() == initial,
                "Undo clone after deletion cycle");
          ++clones;
        }
        auto unsupported =
            std::find_if(d.objects().begin(), d.objects().end(), [&](auto &o) {
              return !d.can_duplicate(size_t(&o - d.objects().data()));
            });
        if (unsupported != d.objects().end()) {
          auto i = size_t(unsupported - d.objects().begin());
          fails([&] { d.duplicate(i, {0, 0, 0}); });
          fails([&] { d.erase(i); });
          check(d.export_bytes() == initial, "Failed action unchanged");
        }
        ++maps;
      }
    Document d;
    d.load(data / "files/Levels/Rlevel1_1.ssw");
    auto original = d.export_bytes();
    auto orig_count = d.objects().size();
    auto find = [&](uint32_t type) {
      for (size_t i = 0; i < d.objects().size(); ++i)
        if (d.objects()[i].type == type && d.can_duplicate(i))
          return i;
      throw std::runtime_error("Required original template missing");
    };
    auto enemy = d.duplicate(find(5000), {2.5f, -1.25f, 76.5f});
    auto enemy_offset = d.objects()[enemy].offset;
    auto pickup = d.duplicate(find(2100), {5.5f, -1.25f, 81.5f});
    auto prop = d.duplicate(find(4200), {-.5f, -1.25f, 81.5f});
    auto prop_index = prop;
    auto cloned = d.export_bytes();
    d.rotate_y(prop, 45);
    d.move(prop, {-.5f, -1.25f, 80.5f});
    check(d.undo() && d.undo() && d.export_bytes() == cloned,
          "Prop bounds and rotation undo");
    auto project = Project::create(out / "MapProjects", "Structure_Native_Test",
                                   "Rlevel1_1");
    project.save(d);
    check(!d.changed(), "Structural save clean");
    Document reopened;
    Project::open(project.folder, data, reopened);
    check(reopened.export_bytes() == d.export_bytes() && !reopened.changed(),
          "Schema 2 save reopen byte exact");
    check(reopened.original_matrix(prop) == d.original_matrix(prop),
          "Added origin survives save reopen");
    d.erase(prop_index);
    check(d.undo() && !d.changed(), "Undo to saved structural state clean");
    auto n = d.objects().size();
    auto again = d.duplicate(enemy, {8.5f, -1.25f, 76.5f});
    check(u(d.payload(), d.objects()[again].offset + 20) >
              u(d.payload(), enemy_offset + 20),
          "Duplicate gets another unique ID");
    check(d.undo() && d.objects().size() == n, "Duplicate added enemy undo");
    std::cout << "NATIVE_PROJECT=" << project.folder.string() << "\n";
    std::cout << "PASS checks=" << checks << " maps=" << maps
              << " clones=" << clones << " original=" << orig_count
              << " edited=" << d.objects().size() << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
  }
}
