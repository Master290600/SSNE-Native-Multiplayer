#include "map_document.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
using namespace mapedit;
namespace fs = std::filesystem;
int checks = 0;
void check(bool ok, const char *s) {
  ++checks;
  if (!ok)
    throw std::runtime_error(s);
}
uint32_t u(const std::vector<uint8_t> &b, uint32_t p) {
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
int main(int argc, char **argv) {
  try {
    check(argc >= 3, "Arguments: owned game_data output [native_exe card]");
    auto data = fs::u8path(argv[1]),
         out = fs::u8path(argv[2]) /
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    int maps = 0;
    for (auto &e : fs::directory_iterator(data / "files/Levels"))
      if (e.path().extension() == ".ssw") {
        Document d;
        d.load(e.path());
        auto enemy = std::find_if(d.objects().begin(), d.objects().end(),
                                  [](auto &o) { return o.type == 5000; });
        if (enemy == d.objects().end())
          continue;
        auto first = d.duplicate(size_t(enemy - d.objects().begin()),
                                 {2.5f, -1.25f, 77.5f});
        auto enemy_offset = d.objects()[first].offset;
        auto baseline = d.export_bytes();
        auto count = d.objects().size();
        WaveTrigger w;
        w.radius = 3;
        w.delay = 2;
        w.rows = {{enemy_offset, 2}};
        auto area = d.add_wave_trigger({2.5f, -.25f, 86.5f}, w);
        check(d.objects().size() == count + 1,
              "Only trigger visible, helper entities hidden");
        check(u(d.payload(), u(d.payload(), enemy_offset + 16) + 4) == 1,
              "Linked enemy template inactive at level load");
        auto read = d.wave_trigger(area);
        check(read && read->rows.size() == 1 && read->rows[0].count == 2 &&
                  read->radius == 3 && read->delay == 2 && !read->repeat,
              "Native once trigger configuration roundtrip");
        check(!d.can_erase(first) && d.can_erase(area),
              "Wave protects linked enemy but permits owned trigger deletion");
        Document round;
        round.parse(d.export_bytes());
        check(round.objects().size() == d.objects().size(),
              "Native helper filtering after export");
        auto once = d.export_bytes();
        w.repeat = true;
        w.max_activations = 4;
        d.set_wave_trigger(area, w);
        read = d.wave_trigger(area);
        check(read && read->repeat && read->max_activations == 4 &&
                  read->delay == 2,
              "Repeat wave configuration");
        auto repeat = d.export_bytes();
        check(d.undo() && d.export_bytes() == once,
              "Undo trigger edits byte exact");
        check(d.redo() && d.export_bytes() == repeat,
              "Redo trigger edits byte exact");
        d.erase(area);
        check(
            d.can_erase(first) &&
                u(d.payload(), u(d.payload(), enemy_offset + 16) + 4) == 0,
            "Deleting trigger restores ordinary enemy and removes references");
        check(d.undo() && d.export_bytes() == repeat,
              "Undo trigger deletion restores hidden helpers");
        check(d.undo() && d.export_bytes() == once && d.undo() &&
                  d.export_bytes() == baseline,
              "Undo creation restores original bytes and active template");
        check(d.redo() && d.redo(), "Redo trigger and repeat settings");
        bool failed = false;
        auto bad = w;
        bad.rows.push_back(bad.rows[0]);
        auto before = d.export_bytes();
        try {
          d.set_wave_trigger(area, bad);
        } catch (const std::exception &) {
          failed = true;
        }
        check(failed && d.export_bytes() == before,
              "Invalid duplicate rows roll back atomically");
        if (e.path().stem() == "Rlevel1_1") {
          auto project = Project::create(out, "Trigger_Repeat", "Rlevel1_1");
          project.save(d);
          Document reopened;
          Project::open(project.folder, data, reopened);
          check(reopened.export_bytes() == d.export_bytes() &&
                    reopened.wave_trigger(area)->max_activations == 4,
                "Save/reopen repeat trigger and templates");
          std::cout << "REPEAT_PROJECT=" << project.folder.string()
                    << std::endl;
          w.repeat = false;
          d.set_wave_trigger(area, w);
          auto single = Project::create(out, "Trigger_Once", "Rlevel1_1");
          single.save(d);
          std::cout << "ONCE_PROJECT=" << single.folder.string() << std::endl;
          if (argc == 5) {
            auto game = fs::u8path(argv[3]), card = fs::u8path(argv[4]);
            auto exe = single.prepare_survival_test(data, game, card);
            auto fst_path = single.folder / "game_data/sys/fst.bin";
            auto bytes = [](auto p) {
              std::ifstream f(p, std::ios::binary);
              return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                          {});
            };
            auto table = bytes(fst_path);
            auto n = u(table, 8);
            auto field = (n - 1) * 12;
            check(u(table, field + 8) ==
                      fs::file_size(data / "files/Levels/Clevel5_4.ssw"),
                  "Survival alias original resource extent");
            check(bytes(single.folder /
                        "game_data/files/EditorSurvivalResources.ssw") ==
                      bytes(data / "files/Levels/Clevel5_4.ssw"),
                  "Survival resource remains original");
            single.prepare_test(data, game, card);
            check(bytes(single.folder / "game_data/sys/boot.bin") ==
                      bytes(data / "sys/boot.bin"),
                  "Ordinary test restores original disc header after Survival");
          }
        }
        ++maps;
      }
    std::cout << "PASS wave_maps=" << maps << " checks=" << checks << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
  }
}
