#include "../launcher/src/map_document.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
unsigned checks = 0;
void check(bool v, const char *m) {
  ++checks;
  if (!v)
    throw std::runtime_error(m);
}
uint32_t u(const mapedit::Document &d, uint32_t p) {
  const auto &b = d.payload();
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
float f(const mapedit::Document &d, uint32_t p) {
  return std::bit_cast<float>(u(d, p));
}
std::vector<uint32_t> keys(const mapedit::Document &d, uint32_t config) {
  std::vector<uint32_t> out;
  auto first = u(d, config + 8), p = first;
  if (!first)
    return out;
  std::set<uint32_t> seen;
  do {
    check(p && uint64_t(p) + 60 <= d.payload().size() &&
              seen.insert(p).second && seen.size() <= 4096,
          "Invalid camera key cycle");
    out.push_back(p);
    p = u(d, p + 24);
  } while (p != first);
  return out;
}
void copied(const mapedit::Document &source, size_t source_index,
            const mapedit::Document &target, size_t target_index,
            std::array<float, 3> placement) {
  const auto &a = source.objects()[source_index];
  const auto &b = target.objects()[target_index];
  check(a.type == b.type, "Template native class changed");
  auto ac = u(source, a.offset + 16), bc = u(target, b.offset + 16);
  if (b.type == 4130) {
    const auto identifier = u(target, b.offset + 20);
    check(target.can_logic_target(identifier, mapedit::LogicAction::Trigger),
          "Native message Trigger is unavailable");
    check(!target.can_logic_target(identifier, mapedit::LogicAction::Enable) &&
              !target.can_logic_target(identifier, mapedit::LogicAction::Disable),
          "Native message exposes inverted generic Activate/Deactivate inputs");
  }
  check(bc && uint64_t(bc) + mapedit::logic_template_config_size(b.type) <=
                  target.payload().size(),
        "Cloned native config outside map");
  auto am = source.matrix(source_index), bm = target.matrix(target_index);
  for (unsigned k = 0; k < 3; ++k)
    check(bm[k * 4 + 3] == placement[k], "Cloned native position differs");
  if (b.type == 4100) {
    check(u(target, bc + 8) == 0xffffffff,
          "Copied sound still targets original mission callback");
    check(u(target, bc + 0x1c) == u(source,ac+0x1c) && u(target, bc + 0x20) == 0,
          "Copied sound lost its streamed clip or kept a global story action");
  }
  if (b.type == 4120) {
    const auto before = keys(source, ac), after = keys(target, bc);
    check(before.size() == after.size(), "Camera lost timeline keys");
    for (size_t n = 0; n < after.size(); ++n) {
      check(u(target, after[n] + 12) == 0xffffffff,
            "Copied camera still targets original mission callback");
      auto ap = u(source, before[n] + 20), bp = u(target, after[n] + 20);
      check(bp && uint64_t(bp) + 48 <= target.payload().size(),
            "Camera key transform missing");
      for (unsigned k = 0; k < 3; ++k)
        check(std::abs(f(target, bp + 12 + k * 16) -
                       (f(source, ap + 12 + k * 16) + placement[k] -
                        am[k * 4 + 3])) < .002f,
              "Camera path placement was not translated");
    }
  }
  mapedit::Document reload;
  reload.parse(target.export_bytes());
  check(reload.payload() == target.payload(),
        "Template resources/relocations changed on save/reload");
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc > 1, "Pass extracted original game_data");
    auto data = std::filesystem::u8path(argv[1]);
    mapedit::Document base;
    base.load(data / "files/Levels/Rlevel1_1.ssw");
    std::vector<std::filesystem::path> maps;
    for (auto preferred : {"Alevel11_1", "Alevel10_3", "Rlevel1_1", "Clevel5_4"})
      maps.push_back(data / "files/Levels" / (std::string(preferred) + ".ssw"));
    for (const auto &entry :
         std::filesystem::directory_iterator(data / "files/Levels"))
      if (entry.path().extension() == ".ssw")
        maps.push_back(entry.path());
    std::map<uint32_t, std::pair<std::string, unsigned>> found;
    for (auto type : {4090u, 4100u, 4120u, 4130u, 4310u, 4340u}) {
      bool done = false;
      for (const auto &path : maps) {
        if (!std::filesystem::exists(path))
          continue;
        mapedit::Document original;
        original.load(path);
        for (size_t i = 0; i < original.objects().size(); ++i) {
          const auto &o = original.objects()[i];
          if (o.type != type || !original.can_duplicate(i))
            continue;
          if (type == 4120 && !u(original, u(original, o.offset + 16) + 8))
            continue;
          if(type==4120){const auto path=keys(original,u(original,o.offset+16));if(std::none_of(path.begin(),path.end(),[&](auto key){return u(original,key+12)<32768;}))continue;}
          const auto untouched = original.export_bytes();
          auto local = original;
          const std::array<float, 3> position{10, 3, 90};
          auto copy = local.duplicate(i, position);
          copied(original, i, local, copy, position);
          check(original.export_bytes() == untouched,
                "Local duplication changed original template");
          auto target = base;
          const auto count = target.objects().size();
          auto imported = target.import_object(original, i, position);
          check(target.objects().size() == count + 1,
                "Import activated donor actors/scripts");
          copied(original, i, target, imported, position);
          check(original.export_bytes() == untouched,
                "Cross-map import changed donor");
          check(!original.can_erase(i),
                "Original story action was not protected");
          check(target.can_erase(imported),
                "New independent action cannot be deleted");
          target.erase(imported);
          check(target.objects().size() == count,
                "Deleting imported action changed other actors");
          found[type] = {
              path.filename().string(),
              unsigned(type == 4120
                           ? keys(original, u(original, o.offset + 16)).size()
                           : 0)};
          done = true;
          break;
        }
        if (done)
          break;
      }
      check(done, "No safe independent native action template found");
    }
    std::cout << "PASS checks=" << checks << " templates=" << found.size();
    for (auto [type, source] : found)
      std::cout << " type" << type << "=" << source.first << "/keys"
                << source.second;
    std::cout << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
