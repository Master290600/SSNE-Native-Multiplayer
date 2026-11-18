#include "../launcher/src/launcher_json.h"
#include "gc/custom_levels.h"
#include "gc/sha256.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
namespace fs = std::filesystem;
using namespace gc::custom;
size_t checks = 0;
void check(bool value, const char *why) {
  ++checks;
  if (!value)
    throw std::runtime_error(why);
}
template <class F> void rejected(F fn, const char *why) {
  bool no = false;
  try {
    fn();
  } catch (...) {
    no = true;
  }
  check(no, why);
}
std::vector<uint8_t> read(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void write(const fs::path &p, const std::vector<uint8_t> &b) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char *>(b.data()), b.size());
  check(bool(f), "Fixture write");
}
uint32_t u(const std::vector<uint8_t> &b, size_t p) {
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
void u(std::vector<uint8_t> &b, size_t p, uint32_t n) {
  for (unsigned k = 0; k < 4; ++k)
    b.at(p + k) = uint8_t(n >> (24 - k * 8));
}
uint32_t crc(const std::vector<uint8_t> &b) {
  uint32_t n = ~0u;
  for (auto c : b) {
    n ^= c;
    for (unsigned k = 0; k < 8; ++k)
      n = (n >> 1) ^ ((n & 1) ? 0xedb88320u : 0);
  }
  return ~n;
}
std::vector<uint8_t> zlib(const std::vector<uint8_t> &input) {
  check(input.size() < 65536, "Stored fixture extent");
  std::vector<uint8_t> b{0x78,
                         1,
                         1,
                         uint8_t(input.size()),
                         uint8_t(input.size() >> 8),
                         uint8_t(~input.size()),
                         uint8_t((~input.size()) >> 8)};
  b.insert(b.end(), input.begin(), input.end());
  uint32_t a = 1, d = 0;
  for (auto c : input) {
    a = (a + c) % 65521;
    d = (d + a) % 65521;
  }
  const auto at = b.size();
  b.resize(at + 4);
  u(b, at, d << 16 | a);
  return b;
}
std::vector<uint8_t> ssw(unsigned variation = 0) {
  std::vector<uint8_t> data(256);
  u(data, 0, 1);
  u(data, 4, 100);
  data[128] = uint8_t(variation);
  const auto packed = zlib(data), reloc = zlib({});
  std::vector<uint8_t> b(64);
  std::memcpy(b.data(), "CLIMAX-\0", 8);
  u(b, 12, data.size());
  u(b, 16, packed.size());
  u(b, 20, reloc.size());
  b.insert(b.end(), packed.begin(), packed.end());
  b.insert(b.end(), reloc.begin(), reloc.end());
  return b;
}
std::vector<uint8_t> png() {
  std::vector<uint8_t> b{137, 80, 78, 71, 13, 10, 26, 10};
  auto chunk = [&](const char *name, std::vector<uint8_t> v) {
    const auto at = b.size();
    b.resize(at + 8);
    u(b, at, v.size());
    std::memcpy(b.data() + at + 4, name, 4);
    b.insert(b.end(), v.begin(), v.end());
    std::vector<uint8_t> hash(b.begin() + at + 4, b.end());
    const auto end = b.size();
    b.resize(end + 4);
    u(b, end, crc(hash));
  };
  std::vector<uint8_t> header(13);
  u(header, 0, 1);
  u(header, 4, 1);
  header[8] = 8;
  header[9] = 6;
  chunk("IHDR", header);
  chunk("IDAT", zlib({0, 30, 120, 255, 255}));
  chunk("IEND", {});
  return b;
}
std::vector<uint8_t> fst(size_t a, size_t b) {
  std::vector<uint8_t> table(7 * 12), names(1);
  auto name = [&](const char *s) {
    auto p = names.size();
    names.insert(names.end(), s, s + std::strlen(s) + 1);
    return uint32_t(p);
  };
  u(table, 0, 0x01000000);
  u(table, 8, 7);
  u(table, 12, 0x01000000 | name("Levels"));
  u(table, 12 + 8, 4);
  u(table, 24, name("Rlevel1_1.ssw"));
  u(table, 28, 0x10000);
  u(table, 32, a);
  u(table, 36, name("Rlevel1_2.ssw"));
  u(table, 40, 0x20000);
  u(table, 44, b);
  u(table, 48, 0x01000000 | name("Nested"));
  u(table, 56, 6);
  u(table, 60, name("keep.bin"));
  u(table, 64, 0x30000);
  u(table, 68, 4);
  u(table, 72, name("after.bin"));
  u(table, 76, 0x40000);
  u(table, 80, 8);
  table.insert(table.end(), names.begin(), names.end());
  return table;
}
using Extents = std::map<std::string, std::pair<uint32_t, uint32_t>>;
Extents entries(const std::vector<uint8_t> &b) {
  const auto count = u(b, 8);
  check(count * 12 <= b.size(), "FST table extent");
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, ""}};
  Extents result;
  for (uint32_t i = 1; i < count; ++i) {
    while (dirs.size() > 1 && i >= dirs.back().first)
      dirs.pop_back();
    const auto tag = u(b, i * 12);
    const auto n = count * 12 + (tag & 0xffffff);
    check(n < b.size(), "FST string extent");
    std::string name(reinterpret_cast<const char *>(b.data() + n));
    const auto path = dirs.back().second + name;
    if (tag >> 24) {
      check(u(b, i * 12 + 4) < i && u(b, i * 12 + 8) > i &&
                u(b, i * 12 + 8) <= count,
            "FST directory indices");
      dirs.emplace_back(u(b, i * 12 + 8), path + "/");
    } else
      check(result.emplace(path, std::pair{u(b, i * 12 + 4), u(b, i * 12 + 8)})
                .second,
            "FST file identity");
  }
  return result;
}
void run(const fs::path &original, const fs::path &root,
         const std::vector<uint8_t> &map) {
  const auto source_fst = read(original / "sys/fst.bin"),
             source_boot = read(original / "sys/boot.bin"),
             source_map = read(original / "files/Levels/Rlevel1_1.ssw");
  const auto image = root / "chosen.png";
  write(image, png());
  const auto alpha =
      write_package(root / "CustomLevels", "Alpha", "Eigene Höhle", "Rlevel1_1",
                    map, image, kDefaultModes);
  const auto beta = write_package(root / "CustomLevels", "Beta", "Second map",
                                  "Rlevel1_1", map, {}, kAllModes);
  check(alpha.alias != beta.alias && alpha.alias.size() <= 48,
        "Two maps from one base have independent aliases");
  check(alpha.base_level == "Rlevel1_1" && !alpha.preview_path.empty(),
        "Preview and source metadata retained");
  const auto download_root = root / "DownloadedCustomLevels";
  const auto thumbnail = png();
  gc::Sha256 preview_hash;
  preview_hash.update(thumbnail.data(), thumbnail.size());
  const auto preview_sha = gc::Sha256::hex(preview_hash.finish());
  const std::string package_sha(64, 'a');
  check(!find_downloaded_package(download_root, alpha.alias, alpha.id,
                                 alpha.map_sha256), "Empty download cache");
  auto received = write_downloaded_package(download_root, alpha, map, thumbnail,
                                           package_sha, preview_sha);
  auto cached = find_downloaded_package(download_root, alpha.alias, alpha.id,
                                        alpha.map_sha256);
  check(cached && cached->package_sha256 == package_sha &&
            cached->preview_sha256 == preview_sha && cached->level.title == alpha.title &&
            read(cached->level.map_path) == map &&
            read(cached->level.preview_path) == thumbnail,
        "Received map, title and preview are reusable as one exact cache identity");
  check(received.folder != alpha.folder &&
            read(alpha.map_path) == map,
        "Received packages preserve local originals and package files");
#ifdef _WIN32
  // Real regression: folder211 + received-package.json22 =233 characters.
  // The former suffix plus a14-digit steady timestamp made its temporary
  // filename263 characters, while map/preview/level.json had already saved.
  constexpr size_t folder_chars=211,cache_chars=folder_chars-1-64;
  const auto parent=fs::absolute(root);
  check(parent.native().size()+1<cache_chars,"Long-path fixture root is too deep");
  const auto long_cache=parent/std::wstring(cache_chars-parent.native().size()-1,L'x');
  const auto long_received=write_downloaded_package(long_cache,alpha,map,thumbnail,
                                                   package_sha,preview_sha);
  check(long_received.folder.native().size()==folder_chars &&
            (long_received.folder/"received-package.json").native().size()==233,
        "Long-cache regression did not exercise the original path boundary");
  const auto long_hit=find_downloaded_package(long_cache,alpha.alias,alpha.id,alpha.map_sha256);
  check(long_hit && read(long_hit->level.map_path)==map &&
            read(long_hit->level.preview_path)==thumbnail,
        "Long cache did not publish and reload the complete received package");
  check(write_downloaded_package(long_cache,alpha,map,thumbnail,package_sha,preview_sha).folder==long_received.folder,
        "Long cache could not reuse its completed package");
  for(const auto& entry:fs::directory_iterator(long_received.folder))
    check(entry.path().filename()=="map.ssw" || entry.path().filename()=="preview.png" ||
              entry.path().filename()=="level.json" || entry.path().filename()=="received-package.json",
          "Completed long-cache export left a temporary sibling");
#endif
  check(write_downloaded_package(download_root, alpha, map, thumbnail,
                                  package_sha, preview_sha).folder == received.folder,
        "An intact matching received version is reused");
  write(received.map_path, {1, 2, 3});
  check(!find_downloaded_package(download_root, alpha.alias, alpha.id,
                                 alpha.map_sha256), "Corrupt cache is not advertised as installed");
  const auto repaired = write_downloaded_package(download_root, alpha, map,
                                                   thumbnail, package_sha, preview_sha);
  check(repaired.folder != received.folder && read(received.map_path) == std::vector<uint8_t>({1,2,3}) &&
            read(repaired.map_path) == map,
        "Cache repair creates a new immutable version and preserves old open sources");
  check(bool(find_downloaded_package(download_root, alpha.alias, alpha.id,
                                      alpha.map_sha256)), "Repaired cache is reusable");
  check(write_downloaded_package(download_root, alpha, map, thumbnail,
                                  package_sha, preview_sha).folder == repaired.folder,
        "Repeated join reuses an intact repair instead of creating another version");
  auto remote_renamed = alpha;
  remote_renamed.title = "Host renamed this map";
  const auto renamed_copy = write_downloaded_package(download_root, remote_renamed, map,
                   {}, std::string(64, '0'), "");
  check(renamed_copy.alias == alpha.alias && renamed_copy.preview_path.empty() &&
            renamed_copy.title == remote_renamed.title && renamed_copy.folder != repaired.folder,
        "Same map bytes can carry new host title and preview without overwriting versions");
  fs::last_write_time(repaired.folder / "received-package.json",
      fs::file_time_type::clock::now() - std::chrono::seconds(10));
  fs::last_write_time(renamed_copy.folder / "received-package.json",
      fs::file_time_type::clock::now());
  cached = find_downloaded_package(download_root, alpha.alias, alpha.id, alpha.map_sha256);
  check(cached && cached->package_sha256 == std::string(64, '0') &&
            cached->level.title == remote_renamed.title,
        "Most recently received metadata wins regardless of lexicographic SHA order");
  check(write_downloaded_package(download_root, alpha, map, thumbnail,
                                  package_sha, preview_sha).folder == repaired.folder &&
            find_downloaded_package(download_root, alpha.alias, alpha.id,
                                     alpha.map_sha256)->package_sha256 == package_sha,
        "Rejoining an older host version updates cache preference without replacing files");
  rejected([&] { write_downloaded_package(download_root, alpha, map,
                         thumbnail, package_sha, std::string(64,'c')); },
           "Received preview hash mismatch is rejected before cache mutation");
  auto forged = alpha;
  forged.map_sha256 = std::string(64,'d');
  rejected([&] { write_downloaded_package(download_root, forged, map,
                         thumbnail, package_sha, preview_sha); },
           "Received map identity mismatch is rejected");
  check(supports_mode(alpha, 0) && supports_mode(alpha, 1) &&
            supports_mode(alpha, 5) && !supports_mode(alpha, 2),
        "Mode flags");
  auto catalog = scan(root / "CustomLevels");
  check(catalog.levels.size() == 2 && catalog.warnings.empty(),
        "Package catalogue roundtrip");
  const auto identity = catalog_fingerprint(catalog);
  std::reverse(catalog.levels.begin(), catalog.levels.end());
  check(identity == catalog_fingerprint(catalog),
        "Catalogue fingerprint order independent");
  const auto target = root / "CustomLevelData" / identity;
  const auto prepared = prepare_data(original, catalog, target);
  check(prepared.levels.size() == 2 && prepared.data_dir == target,
        "Prepared private aggregate");
  std::atomic<unsigned> completed{0};
  const auto parallel_target = root / "ParallelCache";
  auto worker = [&] {
    try {
      if (prepare_data(original, catalog, parallel_target).levels.size() == 2)
        ++completed;
    } catch (...) {
    }
  };
  std::thread first(worker), second(worker);
  first.join();
  second.join();
  check(completed == 2 &&
            read_prepared_catalog(parallel_target).levels.size() == 2,
        "Concurrent preparers serialize and both obtain a complete cache");
  check(read(original / "sys/fst.bin") == source_fst &&
            read(original / "sys/boot.bin") == source_boot &&
            read(original / "files/Levels/Rlevel1_1.ssw") == source_map,
        "Original map/disc untouched");
  const auto old = entries(source_fst),
             now = entries(read(target / "sys/fst.bin"));
  for (auto &[path, value] : old)
    check(now.contains(path) && now.at(path) == value,
          "Original FST file extents preserved");
  uint64_t end = 0;
  for (auto &[path, value] : old)
    end = std::max(end, uint64_t(value.first) + value.second);
  std::vector<std::pair<uint64_t, uint64_t>> added;
  for (const auto &level : prepared.levels) {
    auto extent = now.at("Levels/" + level.alias + ".ssw");
    check(extent.first >= end && extent.second == map.size(),
          "Custom map has a new full extent");
    check(read(level.map_path) == map, "Custom alias contains edited bytes");
    added.emplace_back(extent.first, uint64_t(extent.first) + extent.second);
  }
  std::sort(added.begin(), added.end());
  check(added[0].second <= added[1].first, "Aliases do not overlap");
  const auto boot = read(target / "sys/boot.bin"),
             table = read(target / "sys/fst.bin");
  check(u(boot, 0x424) >= added.back().second &&
            u(boot, 0x428) == table.size() && u(boot, 0x42c) == table.size(),
        "Boot header binds new FST");
  const auto mounted = read_catalog_file(prepared.catalog_file, target);
  check(mounted.levels.size() == 2 && mounted.warnings.empty() &&
            mounted.data_dir == target,
        "Runtime catalogue reads actual mounted aliases");
  rejected([&] { read_catalog_file(prepared.catalog_file, original); },
           "Catalogue cannot bind another mounted data directory");
  const auto stamp = fs::last_write_time(target / "sys/fst.bin");
  check(prepare_data(original, catalog, target).levels.size() == 2 &&
            fs::last_write_time(target / "sys/fst.bin") == stamp,
        "Complete cache reused without mutation");
  write_package(root / "CustomLevels", "Alpha", "Renamed level", "Rlevel1_1",
                map, image);
  const auto renamed = scan(root / "CustomLevels");
  check(catalog_fingerprint(renamed) != identity,
        "Title update changes cache identity");
  rejected([&] { prepare_data(original, renamed, target); },
           "Active completed cache cannot be overwritten");
  rejected([&] { prepare_data(original, catalog, original); },
           "Original data cannot be a custom cache");
  rejected(
      [&] {
        write_package(root / "CustomLevels", "../escape", "Invalid",
                      "Rlevel1_1", map);
      },
      "Traversal ID rejected");
  rejected(
      [&] {
        write_package(root / "CustomLevels", "Bad", "Two\nlines", "Rlevel1_1",
                      map);
      },
      "Multiline name rejected");
  rejected(
      [&] {
        write_package(root / "CustomLevels", "Bad", "Name", "Rlevel1_1", map,
                      {}, 0);
      },
      "Empty modes rejected");
  rejected(
      [&] {
        write_package(root / "CustomLevels", "alpha", "Name", "Rlevel1_1", map);
      },
      "Case-folding package collision rejected");
  auto broken = png();
  broken[broken.size() - 1] ^= 1;
  write(root / "broken.png", broken);
  rejected(
      [&] {
        write_package(root / "CustomLevels", "Bad", "Name", "Rlevel1_1", map,
                      root / "broken.png");
      },
      "Broken PNG rejected");
  const auto saved = read(root / "CustomLevels/Beta/map.ssw");
  write(root / "CustomLevels/Beta/map.ssw", {0, 1, 2});
  check(scan(root / "CustomLevels").levels.size() == 1,
        "Bad package isolated from other entries");
  write(root / "CustomLevels/Beta/map.ssw", saved);
  check(scan(root / "CustomLevels").levels.size() == 2,
        "Repaired package visible again");
  const auto empty = prepare_data(original, {}, root / "Unused");
  check(empty.data_dir == original && !fs::exists(root / "Unused"),
        "No custom levels leaves normal launch unchanged");
  check(read_prepared_catalog(original).levels.empty(),
        "Normal original data has an empty catalogue");
  rejected([&] { read_catalog_file(root / "missing.json"); },
           "Explicit missing catalogue rejected");
}
int main(int argc, char **argv) {
  try {
    const auto root = argc > 2 ? fs::u8path(argv[2]) :
        fs::temp_directory_path() /
        ("ssne-custom-level-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto original = root / "original";
    auto a = ssw(), b = ssw(1);
    write(original / "files/Levels/Rlevel1_1.ssw", a);
    write(original / "files/Levels/Rlevel1_2.ssw", b);
    write(original / "files/Nested/keep.bin", {1, 2, 3, 4});
    write(original / "files/after.bin", {1, 2, 3, 4, 5, 6, 7, 8});
    write(original / "sys/main.dol", std::vector<uint8_t>(128, 7));
    auto table = fst(a.size(), b.size());
    write(original / "sys/fst.bin", table);
    std::vector<uint8_t> boot(0x440);
    std::memcpy(boot.data(), "G3BP9G", 6);
    u(boot, 0x420, 0x4000);
    u(boot, 0x424, 0x2000);
    u(boot, 0x428, table.size());
    u(boot, 0x42c, table.size());
    write(original / "sys/boot.bin", boot);
    run(original, root / "synthetic", a);
    unsigned owned = 0;
    if (argc > 1) {
      const auto data = fs::u8path(argv[1]);
      run(data, root / "owned", read(data / "files/Levels/Rlevel1_1.ssw"));
      owned = 1;
    }
    std::cout << "PASS checks=" << checks << " owned_maps=" << owned
              << " evidence=" << root.string() << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
