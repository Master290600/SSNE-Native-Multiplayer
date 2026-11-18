#include "map_appearance.h"
#include "map_document.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
using namespace mapedit;
namespace fs = std::filesystem;
int checks = 0;
void check(bool x, const char *m) {
  ++checks;
  if (!x)
    throw std::runtime_error(m);
}
uint32_t word(const std::vector<uint8_t> &b, uint32_t p) {
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
std::set<std::pair<uint32_t, std::string>> directory(const Document &d) {
  auto &b = d.payload();
  std::set<std::pair<uint32_t, std::string>> names;
  auto root = word(b, 0x7c);
  if (root)
    for (auto p = word(b, root + 8); p; p = word(b, p)) {
      auto text = word(b, p + 4);
      std::string s;
      while (b.at(text)) {
        auto c = b.at(text++);
        if (c >= 'A' && c <= 'Z')
          c += 32;
        s += char(c);
      }
      names.emplace(word(b, p + 8), s);
    }
  return names;
}
int main(int argc, char **argv) {
  try {
    check(argc == 3, "Arguments: game_data output");
    auto data = fs::u8path(argv[1]);
    auto out = fs::u8path(argv[2]) /
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    int maps = 0;
    for (auto &e : fs::directory_iterator(data / "files/Levels"))
      if (e.path().extension() == ".ssw" && e.path().stem() != "Rlevel1_1") {
        Document d, donor;
        d.load(data / "files/Levels/Rlevel1_1.ssw");
        donor.load(e.path());
        auto original = d.export_bytes();
        size_t placed = 0;
        for (auto type : {5000u, 4200u, 2100u}) {
          auto it = std::find_if(
              donor.objects().begin(), donor.objects().end(), [&](auto &o) {
                return o.type == type &&
                       donor.can_duplicate(size_t(&o - donor.objects().data()));
              });
          if (it == donor.objects().end())
            continue;
          auto before = d.payload().size();
          auto i = d.import_object(donor, size_t(it - donor.objects().begin()),
                                   {2.5f + float(placed) * 3, -1.25f, 78.5f});
          check(d.objects()[i].added && d.objects()[i].type == type,
                "Imported record and independent ID");
          Document round;
          round.parse(d.export_bytes());
          check(round.payload() == d.payload(),
                "Imported map native roundtrip");
          if (placed)
            check(d.payload().size() - before < 100000,
                  "Source assets reused for second template");
          ++placed;
        }
        if (placed) {
          auto source_names = directory(donor), target_names = directory(d);
          for (auto &name : source_names)
            check(target_names.contains(name),
                  "Imported archive dependency registered in target map");
          auto a = Appearance::load(d, data);
          check(!a.vertices.empty(), "Imported scene renders");
          for (auto &p : a.previews)
            if (d.objects().at(p.object).added)
              check(p.error.empty() || p.model.empty() || p.model == "dragon",
                    "Imported enemy/item asset found and skinned");
          auto changed = d.export_bytes();
          for (size_t i = 0; i < placed; ++i)
            check(d.undo(), "Import undo");
          check(d.export_bytes() == original,
                "Import undo removes assets and pointers exactly");
          for (size_t i = 0; i < placed; ++i)
            check(d.redo(), "Import redo");
          check(d.export_bytes() == changed, "Import redo byte exact");
        }
        std::cout << e.path().stem().string() << " bytes=" << d.payload().size()
                  << " added=" << placed << std::endl;
        ++maps;
        if (e.path().stem() == "Clevel5_4") {
          auto p = Project::create(out, "Cross_Map_Test", "Rlevel1_1");
          p.save(d);
          Document reopened;
          Project::open(p.folder, data, reopened);
          check(reopened.export_bytes() == d.export_bytes(),
                "Cross-map project saved/reopened");
          auto it = std::find_if(donor.objects().begin(), donor.objects().end(),
                                 [](auto &o) { return o.type == 5000; });
          if (it != donor.objects().end()) {
            auto before = reopened.payload().size();
            reopened.import_object(donor, size_t(it - donor.objects().begin()),
                                   {8, -1.25f, 78});
            check(reopened.payload().size() - before < 100000,
                  "Import cache survives project reopen");
          }
          std::cout << "PROJECT=" << p.folder.string() << std::endl;
        }
      }
    Document multi, a, b;
    multi.load(data / "files/Levels/Rlevel1_1.ssw");
    a.load(data / "files/Levels/Clevel5_4.ssw");
    b.load(data / "files/Levels/Rlevel1_2.ssw");
    auto enemy = [](const Document &d) {
      for (size_t i = 0; i < d.objects().size(); ++i)
        if (d.objects()[i].type == 5000)
          return i;
      throw std::runtime_error("Enemy fixture missing");
    };
    auto untouched = multi.export_bytes();
    bool failed = false;
    try {
      multi.import_object(a, enemy(a),
                          {std::numeric_limits<float>::quiet_NaN(), 0, 70});
    } catch (const std::exception &) {
      failed = true;
    }
    check(failed && multi.export_bytes() == untouched && !multi.can_undo(),
          "Failed import rolls back bytes, relocations and cache atomically");
    multi.import_object(a, enemy(a), {0, 0, 70});
    auto one = multi.export_bytes();
    multi.import_object(b, enemy(b), {3, 0, 70});
    auto two = multi.export_bytes();
    check(multi.undo() && multi.export_bytes() == one,
          "Undo second source restores first source");
    check(multi.redo() && multi.export_bytes() == two,
          "Redo multiple source caches");
    auto before = multi.payload().size();
    multi.import_object(a, enemy(a), {6, 0, 70});
    check(multi.payload().size() - before < 100000,
          "First source reused after importing second map");
    std::cout << "PASS maps=" << maps << " checks=" << checks << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
  }
}
