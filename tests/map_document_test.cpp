#include "gc/sha256.h"
#include "launcher_json.h"
#include "map_document.h"
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
using namespace mapedit;
namespace fs = std::filesystem;
int checks = 0;
void check(bool yes, const char *message) {
  ++checks;
  if (!yes)
    throw std::runtime_error(message);
}
void fails(const std::function<void()> &f, const char *message) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, message);
}
void word(std::vector<uint8_t> &b, size_t p, uint32_t n) {
  for (int i = 0; i < 4; ++i)
    b.at(p + i) = uint8_t(n >> (24 - i * 8));
}
std::vector<uint8_t> fixture(uint32_t type=4140) {
  std::vector<uint8_t> d(512);
  word(d, 0, 1);
  word(d, 4, 100);
  word(d, 0x40, 0x80);
  word(d, 0x84, 0x100);
  word(d, 0x88, 0x180);
  word(d, 0x8c, type);
  word(d, 0x90, 0x1a0);
  const char name[] = "Player Quick Start";
  std::memcpy(d.data() + 0x180, name, sizeof(name));
  for (auto p : {0x100, 0x114, 0x128})
    word(d, p, std::bit_cast<uint32_t>(1.f));
  word(d, 0x10c, std::bit_cast<uint32_t>(2.5f));
  word(d, 0x11c, std::bit_cast<uint32_t>(-1.25f));
  word(d, 0x12c, std::bit_cast<uint32_t>(86.5f));
  auto z = stored_zlib(d), rel = stored_zlib({});
  std::vector<uint8_t> b(64);
  std::memcpy(b.data(), "CLIMAX-\0", 8);
  word(b, 12, d.size());
  word(b, 16, z.size());
  word(b, 20, rel.size());
  b.insert(b.end(), z.begin(), z.end());
  b.insert(b.end(), rel.begin(), rel.end());
  return b;
}
int main(int argc, char **argv) {
  try {
    if (argc == 6 && std::string(argv[1]) == "--prepare") {
      Document d;
      auto project = Project::open(fs::u8path(argv[2]), fs::u8path(argv[3]), d);
      auto exe = project.prepare_test(fs::u8path(argv[3]), fs::u8path(argv[4]),
                                      fs::u8path(argv[5]));
      std::cout << "PREPARED_TEST_EXE=" << exe.string() << "\n";
      return 0;
    }
    auto root =
        fs::temp_directory_path() /
        ("ssne-editor-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    Document d;
    auto original = fixture();
    d.parse(original);
    check(d.objects().size() == 1 && d.objects()[0].type == 4140,
          "Spawn descriptor parse");
    check(d.export_bytes() == original, "No-op export must be byte identical");
    auto mixed = d.payload();
    word(mixed, 0x80, 0x1b0);
    word(mixed, 0x1bc, 5020);
    auto mz = stored_zlib(mixed), mr = stored_zlib({});
    auto mb = std::vector<uint8_t>(original.begin(), original.begin() + 64);
    word(mb, 16, mz.size());
    mb.insert(mb.end(), mz.begin(), mz.end());
    mb.insert(mb.end(), mr.begin(), mr.end());
    Document nonspatial;
    nonspatial.parse(mb);
    check(nonspatial.objects().size() == 1,
          "Non-spatial global nodes must never expose root header as a "
          "transform");
    auto before = d.matrix(0);
    d.move(0, {10, 2, 30});
    auto edited = d.export_bytes();
    Document r;
    r.parse(edited);
    check(r.matrix(0)[3] == 10 && r.matrix(0)[7] == 2 && r.matrix(0)[11] == 30,
          "Edited native container roundtrip");
    auto base = Document();
    base.parse(original);
    for (size_t i = 0; i < r.payload().size(); ++i)
      if ((i < 0x10c || i >= 0x110) && (i < 0x11c || i >= 0x120) &&
          (i < 0x12c || i >= 0x130))
        check(r.payload()[i] == base.payload()[i],
              "Unrelated world/script bytes changed");
    check(d.undo() && d.matrix(0) == before && !d.changed(),
          "Undo exact restoration");
    check(d.redo() && d.matrix(0)[3] == 10, "Redo");
    fails([&]{d.rotate_y(0,90);},"Point entity cannot silently ignore a rotation");
    Document directional;directional.parse(fixture(4040));directional.move(0,{10,2,30});directional.rotate_y(0,90);
    check(std::abs(directional.matrix(0)[2]-1)<.0001f&&directional.matrix(0)[3]==10,
          "Native spawner yaw rotation keeps translation");
    auto keep = d.matrix(0);
    fails([&] { d.move(0, {NAN, 0, 0}); }, "Reject NaN");
    fails([&] { d.move(0, {1000001, 0, 0}); }, "Reject out-of-range");
    auto bad = keep;
    bad[0] = 9;
    fails([&] { d.set_matrix(0, bad); }, "Reject scale/shear");
    check(d.matrix(0) == keep, "Failed edits are atomic");
    auto corrupt = original;
    corrupt[70] ^= 1;
    fails(
        [&] {
          Document x;
          x.parse(corrupt);
        },
        "Reject checksum damage");
    fails(
        [&] {
          Document x;
          x.parse({1, 2, 3});
        },
        "Reject truncation");
    auto cyc = base.payload();
    word(cyc, 0x80, 0x80);
    auto cycle = original;
    auto z = stored_zlib(cyc);
    std::copy(z.begin(), z.end(), cycle.begin() + 64);
    fails(
        [&] {
          Document x;
          x.parse(cycle);
        },
        "Reject cyclic lists");
    auto input = root / "data/files/Levels/Rlevel1_1.ssw";
    fs::create_directories(input.parent_path());
    atomic_write(input, original);
    d.load(input);
    auto hash = gc::sha256_file(input.string());
    fails([&] { d.write_copy(input); }, "Reject original overwrite");
    auto link = root / "alias.ssw";
    fs::create_hard_link(input, link);
    fails([&] { d.write_copy(link); }, "Reject hardlink overwrite");
    d.move(0, {10, 2, 30});
    auto p = Project::create(root / "MapProjects", "Test_Map", "Rlevel1_1");
    p.save(d);
    check(!d.changed(), "Project save baseline");
    Document loaded;
    auto reopened = Project::open(p.folder, root / "data", loaded);
    check(loaded.matrix(0) == d.matrix(0),
          "Project reload applies confirmed transforms");
    check(!loaded.can_undo(), "Reopened project starts a fresh undo history");
    check(gc::sha256_file(input.string()) == hash,
          "Original preserved through project save");
    fails(
        [&] { Project::create(root / "MapProjects", "../evil", "Rlevel1_1"); },
        "Reject project traversal");
    fails(
        [&] { Project::create(root / "MapProjects", "Test_Map", "Rlevel1_1"); },
        "Reject accidental project overwrite");
    auto invalid = root / "wrong";
    fs::create_directories(invalid / "files/Levels");
    auto changed = original;
    changed.back() ^= 1;
    atomic_write(invalid / "files/Levels/Rlevel1_1.ssw", changed);
    fails(
        [&] {
          Document x;
          Project::open(p.folder, invalid, x);
        },
        "Reject changed original project base"); // A larger map must get a
                                                 // private FST entry and a
                                                 // non-overlapping virtual
                                                 // extent.
    std::vector<uint8_t> fst(36);
    word(fst, 0, 0x01000000);
    word(fst, 8, 3);
    word(fst, 12, 0x01000001);
    word(fst, 20, 3);
    word(fst, 24, 8);
    word(fst, 28, 4096);
    word(fst, 32, original.size());
    const char names[] = "\0Levels\0Rlevel1_1.ssw\0";
    fst.insert(fst.end(), names, names + sizeof(names));
    fs::create_directories(root / "data/sys");
    atomic_write(root / "data/sys/fst.bin", fst);
    auto original_fst_hash =
        gc::sha256_file((root / "data/sys/fst.bin").string());
    atomic_write(root / "test-game.exe", {1, 2, 3});
    atomic_write(root / "test-card.raw", {4, 5, 6});
    auto testexe = p.prepare_test(root / "data", root / "test-game.exe",
                                  root / "test-card.raw");
    check(fs::is_regular_file(testexe), "Test executable is a private copy");
    check(gc::sha256_file((root / "data/sys/fst.bin").string()) ==
              original_fst_hash,
          "Original disc FST unchanged");
    std::ifstream ft(p.folder / "game_data/sys/fst.bin", std::ios::binary);
    std::vector<uint8_t> test_fst((std::istreambuf_iterator<char>(ft)), {});
    auto be = [&](size_t at) {
      return uint32_t(test_fst[at]) << 24 | uint32_t(test_fst[at + 1]) << 16 |
             uint32_t(test_fst[at + 2]) << 8 | test_fst[at + 3];
    };
    check(be(28) >= 4096 + original.size() && be(28) % 32 == 0,
          "Edited map gets a separate aligned virtual disc extent");
    check(be(32) ==
              fs::file_size(p.folder / "game_data/files/Levels/Rlevel1_1.ssw"),
          "Virtual disc entry matches actual edited map size");
    check(!fs::equivalent(root / "data/sys/fst.bin",
                          p.folder / "game_data/sys/fst.bin"),
          "Private FST update breaks original asset hard link");
    int maps = 0, objects = 0;
    if (argc > 1) {
      auto data = fs::u8path(argv[1]);
      for (auto &e : fs::directory_iterator(data / "files/Levels"))
        if (e.path().extension() == ".ssw") {
          Document x;
          x.load(e.path());
          auto out = x.export_bytes();
          check(gc::sha256_file(e.path().string()) == x.source_hash(),
                "Owned map no-op original hash");
          size_t editable = 0;
          while (editable < x.objects().size() &&
                 x.transform_type(editable) == 4070)
            ++editable;
          check(editable < x.objects().size(),
                "Owned map has a movable native entity");
          auto m = x.matrix(editable);
          m[3] += .25f;
          x.set_matrix(editable, m);
          Document y;
          y.parse(x.export_bytes());
          check(y.matrix(editable) == m,
                "Owned map modified native-container roundtrip");
          check(x.undo() && x.export_bytes() == out,
                "Owned map undo restores all original bytes");
          ++maps;
          objects += int(x.objects().size());
        }
      auto src = data / "files/Levels/Rlevel1_1.ssw";
      Document x;
      x.load(src);
      size_t spawn = x.objects().size();
      for (size_t i = 0; i < x.objects().size(); ++i)
        if (x.objects()[i].type == 4140 &&
            x.objects()[i].name == "Player Quick Start") {
          spawn = i;
          break;
        }
      check(spawn < x.objects().size(), "Owned first-map player spawn found");
      auto m = x.matrix(spawn);
      m[3] += 8;
      x.set_matrix(spawn, m);
      auto test = Project::create(root / "MapProjects", "Owned_Spawn_Test",
                                  "Rlevel1_1");
      test.save(x);
      std::cout << "NATIVE_TEST_MAP=" << (test.folder / "game_data").string()
                << "\n";
    }
    std::cout << "PASS checks=" << checks << " owned_maps=" << maps
              << " objects=" << objects << " evidence=" << root.string()
              << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
  }
}
