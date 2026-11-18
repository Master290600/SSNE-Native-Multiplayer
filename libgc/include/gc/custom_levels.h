#pragma once
#include "gc/sha256.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gc::custom {
// Bits follow native mode numbers: 0 single, 1 Coop, 2 Deathmatch,
// 3 Hold the Flag, 4 Pass the Bomb, 5 Survival.
constexpr uint32_t kDefaultModes = (1u << 0) | (1u << 1) | (1u << 5);
constexpr uint32_t kAllModes = 63;
struct Level {
  std::string id, alias, title, base_level, map_sha256;
  uint32_t modes = kDefaultModes;
  std::filesystem::path folder, map_path, preview_path;
};
struct Catalog {
  std::vector<Level> levels;
  std::vector<std::string> warnings;
  std::filesystem::path data_dir;
};
struct Prepared {
  std::filesystem::path data_dir, catalog_file;
  std::vector<Level> levels;
};
struct DownloadedPackage {
  Level level;
  std::string package_sha256, preview_sha256;
};
// Received packages live in a separate immutable cache. A damaged/incomplete
// version is ignored and a subsequent download creates a fresh repair version.
std::optional<DownloadedPackage>
find_downloaded_package(const std::filesystem::path &download_root,
                        const std::string &alias, const std::string &id,
                        const std::string &map_sha256);
Level write_downloaded_package(const std::filesystem::path &download_root,
                               const Level &expected,
                               const std::vector<uint8_t> &map_bytes,
                               const std::vector<uint8_t> &preview_bytes,
                               const std::string &package_sha256,
                               const std::string &preview_sha256);
bool valid_id(const std::string &);
bool supports_mode(const Level &, unsigned native_mode);
std::string map_alias(const std::string &id, const std::string &map_sha256);
std::string catalog_fingerprint(const Catalog &);
inline std::filesystem::path prepared_cache_path(const std::filesystem::path &root,
                                                 const std::filesystem::path &data,
                                                 const Catalog &catalog) {
  auto signature = catalog_fingerprint(catalog);
  for (const auto *name : {"fst.bin", "boot.bin", "main.dol"}) {
    const auto file = (data / "sys" / name).u8string();
    signature += gc::sha256_file(std::string(file.begin(), file.end()));
  }
  gc::Sha256 digest;
  digest.update(signature.data(), signature.size());
  return root / "CustomLevelData" / gc::Sha256::hex(digest.finish());
}
// Invalid packages are isolated and reported in warnings. No package path may
// escape the root or contain a symlink/reparse point.
Catalog scan(const std::filesystem::path &packages_root);
// Atomic local package export. Empty preview uses the original base-map image.
// PNG containers are bounded/validated; the UI decodes pixels before export.
Level write_package(const std::filesystem::path &packages_root,
                    const std::string &id, const std::string &title,
                    const std::string &base_level,
                    const std::vector<uint8_t> &map_bytes,
                    const std::filesystem::path &preview_png = {},
                    uint32_t modes = kDefaultModes);
// A separate virtual disc retains original files and adds unique alias maps.
// Only target is written. Original FST, boot data, maps and packages are read.
Prepared prepare_data(const std::filesystem::path &original_data,
                      const Catalog &catalog,
                      const std::filesystem::path &target);
Catalog read_prepared_catalog(const std::filesystem::path &data_dir);
Catalog read_catalog_file(const std::filesystem::path &catalog_file,
                          const std::filesystem::path &expected_data_dir = {});
} // namespace gc::custom
