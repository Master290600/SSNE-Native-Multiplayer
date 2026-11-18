#include "gc/sha256.h"
#include "map_document.h"
#include "map_gizmo.h"
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace mapedit;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void check(bool v, const char *m) {
  ++checks;
  if (!v)
    throw std::runtime_error(m);
}
uint32_t word(const std::vector<uint8_t> &b, size_t p) {
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
float number(const std::vector<uint8_t> &b, size_t p) {
  return std::bit_cast<float>(word(b, p));
}
uint32_t bound(const Document &d, size_t index) {
  auto cfg = word(d.payload(), d.objects()[index].offset + 16),
       scene_index = word(d.payload(), cfg);
  auto scene = word(d.payload(), 0x58);
  for (unsigned n = 0; n < scene_index; ++n)
    scene = word(d.payload(), scene);
  return word(d.payload(), scene + 24);
}
bool matrix_close(const Matrix &a, const Matrix &b) {
  for (unsigned n = 0; n < 12; ++n)
    if (std::abs(a[n] - b[n]) > .0001f)
      return false;
  return true;
}
template <class F> void rejects(F f, const char *m) {
  bool bad = false;
  try {
    f();
  } catch (...) {
    bad = true;
  }
  check(bad, m);
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 3, "Use data root and private evidence root");
    const auto data = fs::u8path(argv[1]), out = fs::u8path(argv[2]);
    fs::create_directories(out);
    unsigned props = 0, arrows = 0;
    for (const auto &level : {"Rlevel1_1", "Clevel5_4", "Alevel10_2"}) {
      const auto source = data / "files/Levels" / (std::string(level) + ".ssw");
      if (!fs::exists(source))
        continue;
      const auto before_hash = gc::sha256_file(source.string());
      Document d;
      d.load(source);
      for (size_t i = 0; i < d.objects().size(); ++i)
        if (d.objects()[i].type == 4070) {
          const auto wire = d.export_bytes();
          const auto m = d.matrix(i);
          rejects([&] { d.move(i, {m[3] + 2, m[7], m[11]}); },
                  "Moving brush origin changed without its geometry/path");
          check(d.export_bytes() == wire && !d.can_undo(),
                "Rejected moving-brush edit changed document/history");
          break;
        }
      bool did_prop = false, did_arrow = false;
      for (size_t i = 0; i < d.objects().size(); ++i) {
        const auto type = d.objects()[i].type;
        if (type != 4200 && type != 4260)
          continue;
        if ((type == 4200 && did_prop) || (type == 4260 && did_arrow))
          continue;
        const auto original = d.matrix(i);
        const auto wire = d.export_bytes();
        const auto old_bound = type == 4200 ? bound(d, i) : 0;
        const auto radius = old_bound ? number(d.payload(), old_bound + 16) : 0;
        auto scaled = transformed_scale(original, {2, .5f, 1.25f});
        scaled = transformed_rotation(scaled, {1, 0, 0}, .4f);
        scaled = transformed_rotation(scaled, {0, 1, 0}, .7f);
        scaled = transformed_rotation(scaled, {0, 0, 1}, -.2f);
        d.set_matrix(i, scaled);
        check(matrix_close(d.matrix(i), scaled),
              "Native affine transform changed during edit");
        if (old_bound) {
          const auto p = bound(d, i);
          check(std::abs(number(d.payload(), p + 16) - radius * 2) <
                    .003f * std::max(1.f, radius),
                "Scaled prop sphere has wrong radius");
        }
        const auto changed = d.export_bytes();
        Document fresh;
        fresh.parse(changed);
        check(matrix_close(fresh.matrix(i), scaled),
              "Native export did not preserve rotate/scale");
        check(d.undo() && d.export_bytes() == wire,
              "Single transform Undo is not byte-exact");
        check(d.redo() && d.export_bytes() == changed,
              "Transform Redo changed relocation data");
        d.set_matrix(i, original);
        if (old_bound) {
          const auto p = bound(d, i);
          check(std::abs(number(d.payload(), p + 16) - radius) <
                    .003f * std::max(1.f, radius),
                "Repeated scaling inflated original collision sphere");
        }
        d.set_matrix(i, scaled);
        const auto before_rejection = d.export_bytes();
        auto bad = scaled;
        for (unsigned r = 0; r < 3; ++r)
          bad[r * 4] = 0;
        rejects([&] { d.set_matrix(i, bad); }, "Collapsed model accepted");
        bad = scaled;
        for (unsigned r = 0; r < 3; ++r)
          bad[r * 4] *= -1;
        rejects([&] { d.set_matrix(i, bad); }, "New mirrored model accepted");
        bad = scaled;
        bad[1] += 2;
        rejects([&] { d.set_matrix(i, bad); }, "Sheared model accepted");
        check(d.export_bytes() == before_rejection,
              "Rejected affine transform modified map");
        if (type == 4200 && props == 0) {
          auto project =
              Project::create(out / "projects", "Scaled_Prop", level);
          project.save(d);
          Document loaded;
          Project::open(project.folder, data, loaded);
          check(matrix_close(loaded.matrix(i), scaled),
                "Saved project lost affine transform");
          check(loaded.export_bytes() == d.export_bytes(),
                "Reopened project changed native geometry");
        }
        if (type == 4200) {
          did_prop = true;
          ++props;
        } else {
          did_arrow = true;
          ++arrows;
        }
        d.undo();
        while (d.can_undo())
          d.undo();
        // One real example of each available native kind per map is sufficient.
        if (did_prop && did_arrow)
          break;
      }
      check(gc::sha256_file(source.string()) == before_hash,
            "Original map changed during editor transform tests");
    }
    check(props > 0 && arrows > 0,
          "No real native prop/arrow transformation tested");
    std::cout << "PASS affine native export/project checks=" << checks
              << " props=" << props << " arrows=" << arrows << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
