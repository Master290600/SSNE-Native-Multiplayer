#include "gc/custom_levels.h"
#include "../../launcher/src/launcher_json.h"
#include "gc/decompress.h"
#include "gc/sha256.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace gc::custom {
namespace {
namespace fs = std::filesystem;
using J = launcher::JSONValue;
constexpr size_t map_limit = 128u * 1024 * 1024;
constexpr size_t png_limit = 4u * 1024 * 1024;
constexpr size_t package_limit = 128;
void need(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
uint32_t word(const std::vector<uint8_t> &b, size_t p) {
  need(p <= b.size() && b.size() - p >= 4,
       "Custom-level field is out of range");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
void word(std::vector<uint8_t> &b, size_t p, uint32_t value) {
  need(p <= b.size() && b.size() - p >= 4,
       "Custom-level write is out of range");
  for (unsigned i = 0; i < 4; ++i)
    b[p + i] = uint8_t(value >> (24 - i * 8));
}
std::string hash(const std::vector<uint8_t> &b) {
  Sha256 value;
  value.update(b.data(), b.size());
  return Sha256::hex(value.finish());
}
std::string hash(const std::string &s) {
  Sha256 value;
  value.update(s.data(), s.size());
  return Sha256::hex(value.finish());
}
std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
  return s;
}
bool hex_hash(const std::string &s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
void plain(const fs::path &p) {
  need(!fs::is_symlink(p), "Custom-level paths cannot be symlinks");
#ifdef _WIN32
  const auto flags = GetFileAttributesW(p.c_str());
  need(flags == INVALID_FILE_ATTRIBUTES ||
           !(flags & FILE_ATTRIBUTE_REPARSE_POINT),
       "Custom-level paths cannot be reparse points");
#endif
}
fs::path under(const fs::path &root, const fs::path &relative) {
  need(!relative.is_absolute() && !relative.has_root_name(),
       "Custom-level path must be relative");
  plain(root);
  auto p = root;
  for (const auto &part : relative) {
    need(part != "." && part != ".." && part != "",
         "Invalid custom-level path component");
    p /= part;
    plain(p);
  }
  return p;
}
// The lock is an OS-owned handle, not a directory/existence flag: a crashed
// preparer releases it automatically. Keep it outside the virtual FST.
class CacheLock {
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int handle_ = -1;
#endif
public:
  explicit CacheLock(const fs::path &target) {
    fs::create_directories(target);
    const auto path = under(target, ".custom-levels.lock");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
#ifdef _WIN32
    while (handle_ == INVALID_HANDLE_VALUE) {
      handle_ = CreateFileW(
          path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle_ != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle_, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
          CloseHandle(handle_);
          handle_ = INVALID_HANDLE_VALUE;
          throw std::runtime_error("Custom data cache lock cannot be a link");
        }
        return;
      }
      const auto error = GetLastError();
      need(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION,
           "Cannot open custom data cache lock");
      need(std::chrono::steady_clock::now() < deadline,
           "Another launcher is still preparing custom levels; retry when it "
           "finishes");
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#else
    handle_ =
        ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    need(handle_ >= 0, "Cannot open custom data cache lock");
    while (::flock(handle_, LOCK_EX | LOCK_NB) != 0) {
      if ((errno != EWOULDBLOCK && errno != EAGAIN) ||
          std::chrono::steady_clock::now() >= deadline) {
        ::close(handle_);
        handle_ = -1;
        throw std::runtime_error("Another launcher is still preparing custom "
                                 "levels; retry when it finishes");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
  }
  CacheLock(const CacheLock &) = delete;
  CacheLock &operator=(const CacheLock &) = delete;
  ~CacheLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE)
      CloseHandle(handle_);
#else
    if (handle_ >= 0) {
      ::flock(handle_, LOCK_UN);
      ::close(handle_);
    }
#endif
  }
};
std::vector<uint8_t> bytes(const fs::path &p, size_t maximum) {
  plain(p);
  std::ifstream in(p, std::ios::binary | std::ios::ate);
  need(bool(in), "Cannot read custom-level file");
  const auto n = in.tellg();
  need(n >= 0 && uint64_t(n) <= maximum, "Custom-level file exceeds its limit");
  std::vector<uint8_t> result(static_cast<size_t>(n));
  in.seekg(0);
  if (!result.empty())
    in.read(reinterpret_cast<char *>(result.data()), result.size());
  need(bool(in), "Custom-level file is incomplete");
  return result;
}
void atomic(const fs::path &p, const std::vector<uint8_t> &value) {
  plain(p);
  fs::create_directories(p.parent_path());
  if (fs::is_regular_file(p) && fs::file_size(p) == value.size() &&
      bytes(p, value.size()) == value)
    return;
  // Keep the sibling name independent of the destination filename. Appending
  // a suffix to received-package.json can exceed Windows MAX_PATH even when
  // the complete package and all its final filenames fit comfortably.
  static std::atomic<uint64_t> nonce{0};
  const auto hex = [](uint64_t value) {
    std::array<char,16> buffer;
    const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,16);
    return std::string(buffer.data(),result.ptr);
  };
#ifdef _WIN32
  const auto process=uint64_t(GetCurrentProcessId());
#else
  const auto process=uint64_t(::getpid());
#endif
  const auto temporary = p.parent_path() /
      (".c"+hex(process)+"-"+
       hex(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()))+"-"+
       hex(nonce.fetch_add(1,std::memory_order_relaxed)));
  need(!fs::exists(temporary), "Custom-level temporary file already exists");
  try {
    std::ofstream out(temporary, std::ios::binary);
    need(bool(out), "Cannot create custom-level export");
    out.write(reinterpret_cast<const char *>(value.data()), value.size());
    out.close();
    need(bool(out), "Cannot finish custom-level export");
#ifdef _WIN32
    need(MoveFileExW(temporary.c_str(), p.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
         "Cannot replace custom-level export; close its running game first");
#else
    fs::rename(temporary, p);
#endif
  } catch (...) {
    std::error_code ec;
    fs::remove(temporary, ec);
    throw;
  }
}
void atomic_copy(const fs::path &source, const fs::path &target) {
  plain(source);
  plain(target);
  fs::create_directories(target.parent_path());
  auto temporary = target;
  temporary += ".custom-copy-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
  need(!fs::exists(temporary),
       "Custom data cache temporary file already exists");
  try {
    fs::copy_file(source, temporary);
#ifdef _WIN32
    need(MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
         "Cannot replace custom data cache file");
#else
    fs::rename(temporary, target);
#endif
    fs::last_write_time(target, fs::last_write_time(source));
  } catch (...) {
    std::error_code ec;
    fs::remove(temporary, ec);
    throw;
  }
}
std::vector<uint8_t> encoded(const J &v) {
  const auto s = launcher::write_json(v, true) + "\n";
  return {s.begin(), s.end()};
}
J json(const fs::path &p, size_t maximum = 64 * 1024) {
  const auto b = bytes(p, maximum);
  J value;
  std::string error;
  need(launcher::parse_json(
           std::string_view(reinterpret_cast<const char *>(b.data()), b.size()),
           value, error, {maximum, 4096, 10000, 12}),
       "Invalid custom-level JSON");
  need(value.object() != nullptr, "Custom-level metadata must be an object");
  return value;
}
std::string text(const J &v, const char *key) {
  const auto p = v.find(key);
  need(p && p->string(), "Custom-level text field is missing");
  return *p->string();
}
int64_t integer(const J &v, const char *key) {
  int64_t value;
  const auto p = v.find(key);
  need(p && p->integer(value), "Custom-level integer field is missing");
  return value;
}
void title_valid(const std::string &s) {
  need(!s.empty() && s.size() <= 96 &&
           std::none_of(s.begin(), s.end(),
                        [](unsigned char c) { return c < 32 || c == 127; }),
       "Use a one-line custom-level title of 1 to 96 UTF-8 bytes");
  (void)launcher::write_json(J(s)); // Existing strict UTF-8 validation.
}
void ssw(const std::vector<uint8_t> &b) {
  need(b.size() >= 64 && b.size() <= map_limit &&
           !std::memcmp(b.data(), "CLIMAX-\0", 8),
       "Custom map must be a CLIMAX SSW file");
  const auto size = word(b, 12), packed = word(b, 16), reloc = word(b, 20),
             count = word(b, 8);
  need(size >= 0x80 && size <= map_limit && count <= map_limit / 4 &&
           uint64_t(64) + packed + reloc == b.size() && packed >= 6 &&
           reloc >= 6,
       "Invalid custom map container lengths");
  std::vector<uint8_t> data(size);
  std::string error;
  need(decomp::inflate(b.data() + 64, packed, data.data(), data.size(), true,
                       &error),
       "Custom map data cannot be decompressed");
  need(word(data, 0) == 1 && word(data, 4) == 100,
       "Unsupported custom map root version");
  std::vector<uint8_t> pointers(size_t(count) * 4);
  need(decomp::inflate(b.data() + 64 + packed, reloc, pointers.data(),
                       pointers.size(), true, &error),
       "Custom map relocation data cannot be decompressed");
}
uint32_t crc(const std::vector<uint8_t> &b, size_t p, size_t n) {
  uint32_t value = ~0u;
  for (size_t i = p; i < p + n; ++i) {
    value ^= b[i];
    for (unsigned k = 0; k < 8; ++k)
      value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
  }
  return ~value;
}
void png(const std::vector<uint8_t> &b) {
  constexpr uint8_t signature[]{137, 80, 78, 71, 13, 10, 26, 10};
  need(b.size() >= 33 && b.size() <= png_limit &&
           std::equal(std::begin(signature), std::end(signature), b.begin()),
       "Custom preview must be a PNG of at most 4 MiB");
  bool header = false, data = false, end = false;
  for (size_t p = 8; p < b.size();) {
    need(b.size() - p >= 12, "Custom PNG chunk is truncated");
    const auto n = word(b, p);
    need(n <= b.size() - p - 12 &&
             crc(b, p + 4, size_t(n) + 4) == word(b, p + 8 + n),
         "Custom PNG chunk checksum differs");
    const std::string type(reinterpret_cast<const char *>(b.data() + p + 4), 4);
    need(header || type == "IHDR", "Custom PNG header must be first");
    if (type == "IHDR") {
      need(!header && n == 13 && word(b, p + 8) && word(b, p + 12) &&
               word(b, p + 8) <= 1024 && word(b, p + 12) <= 1024,
           "Custom PNG dimensions must be 1 to 1024 pixels");
      header = true;
    } else if (type == "IDAT") {
      need(n > 0, "Custom PNG data chunk is empty");
      data = true;
    } else if (type == "IEND") {
      need(!n && data && p + 12 == b.size(), "Custom PNG ending is invalid");
      end = true;
    }
    p += size_t(n) + 12;
  }
  need(header && data && end, "Custom PNG is incomplete");
}
Level metadata(const J &j) {
  need(integer(j, "schema") == 1, "Unsupported custom-level metadata version");
  Level value;
  value.id = text(j, "id");
  value.title = text(j, "display_name");
  value.base_level = text(j, "base_level");
  value.map_sha256 = text(j, "map_sha256");
  need(valid_id(value.id) && valid_id(value.base_level) &&
           hex_hash(value.map_sha256),
       "Invalid custom-level identity");
  title_valid(value.title);
  const auto modes = integer(j, "modes");
  need(modes >= 1 && modes <= kAllModes, "Custom-level mode flags are invalid");
  value.modes = uint32_t(modes);
  value.alias = map_alias(value.id, value.map_sha256);
  if (const auto alias = j.find("alias"))
    need(alias->string() && *alias->string() == value.alias,
         "Custom-level alias does not match its identity");
  return value;
}
J record(const Level &value, bool prepared) {
  return J::Object{
      {"schema", 1},
      {"id", value.id},
      {"alias", value.alias},
      {"display_name", value.title},
      {"base_level", value.base_level},
      {"modes", int64_t(value.modes)},
      {"map_sha256", value.map_sha256},
      {"map", prepared ? "files/Levels/" + value.alias + ".ssw" : "map.ssw"},
      {"preview", value.preview_path.empty() ? ""
                  : prepared ? "custom-level-previews/" + value.alias + ".png"
                             : "preview.png"}};
}
bool descendant(const fs::path &child, const fs::path &parent) {
  const auto c = fs::weakly_canonical(child), p = fs::weakly_canonical(parent);
  auto a = c.begin(), b = p.begin();
  for (; b != p.end(); ++a, ++b)
    if (a == c.end() || lower(a->string()) != lower(b->string()))
      return false;
  return true;
}
struct FstNode {
  std::string name;
  bool directory = false;
  uint32_t offset = 0, size = 0;
  std::vector<FstNode> children;
};
FstNode parse_fst(const std::vector<uint8_t> &b, uint64_t &end) {
  need(b.size() >= 12 && word(b, 0) >> 24 == 1,
       "Invalid original virtual disc root");
  const auto count = word(b, 8);
  need(count >= 1 && count <= 100000 && uint64_t(count) * 12 <= b.size(),
       "Invalid original virtual disc entry count");
  const auto strings = size_t(count) * 12;
  auto read_name = [&](uint32_t i) {
    size_t at = strings + (word(b, size_t(i) * 12) & 0xffffff);
    std::string name;
    for (unsigned k = 0;; ++k) {
      need(k < 256 && at < b.size(), "Invalid original virtual disc name");
      const auto c = b[at++];
      if (!c)
        break;
      need(c != '/' && c != '\\', "Invalid original virtual disc path");
      name += char(c);
    }
    need(!name.empty() && name != "." && name != "..",
         "Invalid original virtual disc component");
    return name;
  };
  auto walk = [&](auto &&self, uint32_t index, uint32_t parent,
                  unsigned depth) -> std::pair<FstNode, uint32_t> {
    need(index < count && depth < 64, "Invalid original virtual disc nesting");
    const auto at = size_t(index) * 12;
    const auto tag = word(b, at);
    FstNode n;
    n.directory = tag >> 24;
    need((tag >> 24) <= 1, "Unknown original virtual disc entry type");
    n.name = index ? read_name(index) : "";
    if (n.directory) {
      const auto next = word(b, at + 8);
      need(next > index && next <= count && word(b, at + 4) == parent,
           "Invalid original virtual disc directory extent");
      std::set<std::string> names;
      for (uint32_t i = index + 1; i < next;) {
        auto [child, following] = self(self, i, index, depth + 1);
        need(following <= next && names.insert(lower(child.name)).second,
             "Duplicate or crossing original virtual disc entry");
        n.children.push_back(std::move(child));
        i = following;
      }
      return {std::move(n), next};
    }
    n.offset = word(b, at + 4);
    n.size = word(b, at + 8);
    end = std::max(end, uint64_t(n.offset) + n.size);
    return {std::move(n), index + 1};
  };
  auto [root, next] = walk(walk, 0, 0, 0);
  need(next == count, "Invalid original virtual disc total extent");
  return root;
}
std::vector<uint8_t> encode_fst(const FstNode &root) {
  struct Entry {
    const FstNode *node;
    uint32_t parent, next;
  };
  std::vector<Entry> entries;
  auto walk = [&](auto &&self, const FstNode &node, uint32_t parent) -> void {
    need(entries.size() < 100000, "Custom virtual disc has too many entries");
    const auto index = uint32_t(entries.size());
    entries.push_back({&node, parent, 0});
    for (const auto &child : node.children)
      self(self, child, index);
    entries[index].next = uint32_t(entries.size());
  };
  walk(walk, root, 0);
  std::vector<uint8_t> b(entries.size() * 12), strings(1);
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &entry = entries[i];
    const auto &node = *entry.node;
    const auto offset = i ? strings.size() : 0;
    need(offset < 0x1000000, "Custom virtual disc name table is too large");
    if (i) {
      strings.insert(strings.end(), node.name.begin(), node.name.end());
      strings.push_back(0);
    }
    word(b, i * 12, uint32_t(offset) | (node.directory ? 0x01000000u : 0));
    word(b, i * 12 + 4, node.directory ? entry.parent : node.offset);
    word(b, i * 12 + 8, node.directory ? entry.next : node.size);
  }
  b.insert(b.end(), strings.begin(), strings.end());
  return b;
}
} // namespace

bool valid_id(const std::string &s) {
  return !s.empty() && s.size() <= 48 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
         });
}
bool supports_mode(const Level &level, unsigned mode) {
  return mode < 6 && (level.modes & (1u << mode));
}
std::string map_alias(const std::string &id, const std::string &sha) {
  need(valid_id(id) && hex_hash(sha), "Invalid custom-level alias input");
  return "Custom_" + hash(id + "\n" + sha).substr(0, 24);
}
std::string catalog_fingerprint(const Catalog &catalog) {
  std::vector<const Level *> sorted;
  for (const auto &entry : catalog.levels)
    sorted.push_back(&entry);
  std::sort(sorted.begin(), sorted.end(),
            [](const auto *a, const auto *b) { return a->id < b->id; });
  J::Array rows;
  for (const auto *level : sorted) {
    auto row = record(*level, false);
    row.object()->emplace("preview_sha256",
                          level->preview_path.empty()
                              ? ""
                              : hash(bytes(level->preview_path, png_limit)));
    rows.push_back(std::move(row));
  }
  return hash(encoded(J(std::move(rows))));
}
Catalog scan(const fs::path &root) {
  Catalog result;
  plain(root);
  if (!fs::exists(root))
    return result;
  need(fs::is_directory(root), "CustomLevels must be a directory");
  std::vector<fs::path> folders;
  for (const auto &entry : fs::directory_iterator(root))
    if (valid_id(entry.path().filename().string()) &&
        fs::exists(entry.path() / "level.json"))
      folders.push_back(entry.path());
  std::sort(folders.begin(), folders.end());
  need(folders.size() <= package_limit, "Custom-level package limit is 128");
  std::set<std::string> aliases, ids;
  for (const auto &folder : folders) {
    try {
      plain(folder);
      if (!fs::is_directory(folder) || !fs::exists(folder / "level.json"))
        continue;
      const auto info = json(under(folder, "level.json"));
      auto value = metadata(info);
      need(value.id == folder.filename().string(),
           "Custom-level ID must match its directory");
      need(text(info, "map") == "map.ssw", "Unsupported custom-level map path");
      value.folder = folder;
      value.map_path = under(folder, "map.ssw");
      const auto map = bytes(value.map_path, map_limit);
      ssw(map);
      need(hash(map) == value.map_sha256, "Custom-level map hash differs");
      const auto preview = text(info, "preview");
      need(preview.empty() || preview == "preview.png",
           "Unsupported custom-level preview path");
      if (!preview.empty()) {
        value.preview_path = under(folder, preview);
        png(bytes(value.preview_path, png_limit));
      }
      need(ids.insert(lower(value.id)).second &&
               aliases.insert(lower(value.alias)).second,
           "Duplicate custom-level identity");
      result.levels.push_back(std::move(value));
    } catch (const std::exception &error) {
      result.warnings.push_back(folder.filename().string() + ": " +
                                error.what());
    }
  }
  return result;
}
Level write_package(const fs::path &root, const std::string &id,
                    const std::string &title, const std::string &base,
                    const std::vector<uint8_t> &map, const fs::path &preview,
                    uint32_t modes) {
  need(valid_id(id) && valid_id(base) && modes && !(modes & ~kAllModes),
       "Invalid custom-level ID, base level or modes");
  title_valid(title);
  ssw(map);
  std::vector<uint8_t> image;
  if (!preview.empty()) {
    image = bytes(preview, png_limit);
    png(image);
  }
  if (fs::is_directory(root))
    for (const auto &entry : fs::directory_iterator(root))
      need(lower(entry.path().filename().string()) != lower(id) ||
               entry.path().filename().string() == id,
           "A custom-level ID with different letter case already exists");
  const auto folder = under(root, id);
  need(!fs::exists(folder) || fs::is_directory(folder),
       "Custom-level package destination is not a directory");
  fs::create_directories(folder);
  Level value;
  value.id = id;
  value.title = title;
  value.base_level = base;
  value.map_sha256 = hash(map);
  value.alias = map_alias(id, value.map_sha256);
  value.modes = modes;
  value.folder = folder;
  value.map_path = under(folder, "map.ssw");
  if (!image.empty())
    value.preview_path = under(folder, "preview.png");
  const auto manifest = under(folder, "level.json");
  struct Before {
    fs::path path;
    bool exists;
    std::vector<uint8_t> data;
  };
  std::vector<Before> before;
  for (const auto &p : {value.map_path, under(folder, "preview.png"), manifest})
    before.push_back({p, fs::is_regular_file(p),
                      fs::is_regular_file(p) ? bytes(p, map_limit)
                                             : std::vector<uint8_t>{}});
  try {
    atomic(value.map_path, map);
    if (!image.empty())
      atomic(value.preview_path, image);
    atomic(manifest, encoded(record(value, false)));
  } catch (...) {
    for (auto it = before.rbegin(); it != before.rend(); ++it)
      try {
        if (it->exists)
          atomic(it->path, it->data);
        else if (fs::exists(it->path))
          fs::remove(it->path);
      } catch (...) {
      }
    throw;
  }
  return value;
}
namespace {
DownloadedPackage received(const fs::path &root, const fs::path &version,
                           const std::string &alias, const std::string &id,
                           const std::string &map_sha) {
  const auto folder = under(root, version);
  const auto marker = json(under(folder, "received-package.json"));
  DownloadedPackage result;
  result.package_sha256 = text(marker, "package_sha256");
  result.preview_sha256 = text(marker, "preview_sha256");
  need(integer(marker, "schema") == 1 && hex_hash(result.package_sha256) &&
           version.string().substr(0,64) == result.package_sha256 &&
           (result.preview_sha256.empty() || hex_hash(result.preview_sha256)),
       "Invalid downloaded package identity");
  auto level = metadata(marker);
  need(level.id == id && level.alias == alias && level.map_sha256 == map_sha,
       "Downloaded package does not match requested map");
  // Check the cheap manifest first; only matching candidates read/hash data.
  const auto manifest = json(under(folder, "level.json"));
  const auto installed = metadata(manifest);
  need(installed.id == level.id && installed.alias == level.alias &&
           installed.title == level.title && installed.base_level == level.base_level &&
           installed.modes == level.modes && installed.map_sha256 == level.map_sha256 &&
           text(manifest, "map") == "map.ssw",
       "Downloaded package metadata differs");
  level.folder = folder;
  level.map_path = under(folder, "map.ssw");
  const auto map = bytes(level.map_path, map_limit);
  need(hash(map) == level.map_sha256, "Downloaded map checksum differs");
  ssw(map);
  const auto preview_name = text(manifest, "preview");
  need(preview_name.empty() || preview_name == "preview.png",
       "Invalid downloaded preview path");
  if (!preview_name.empty()) {
    level.preview_path = under(folder, "preview.png");
    const auto image = bytes(level.preview_path, png_limit);
    need(hash(image) == result.preview_sha256,
         "Downloaded preview checksum differs");
    png(image);
  } else {
    need(result.preview_sha256.empty() ||
             result.preview_sha256 == hash(std::vector<uint8_t>{}),
         "Downloaded preview is missing");
  }
  result.level = std::move(level);
  return result;
}
} // namespace
std::optional<DownloadedPackage>
find_downloaded_package(const fs::path &root, const std::string &alias,
                        const std::string &id, const std::string &map_sha) {
  need(valid_id(id) && hex_hash(map_sha) && alias == map_alias(id, map_sha),
       "Invalid downloaded package lookup");
  plain(root);
  if (!fs::exists(root)) return {};
  need(fs::is_directory(root), "DownloadedCustomLevels must be a directory");
  struct Candidate { fs::path version; fs::file_time_type modified; };
  std::vector<Candidate> candidates;
  for (const auto &entry : fs::directory_iterator(root)) {
    const auto name = entry.path().filename().string();
    if (name.size() >= 64 && hex_hash(name.substr(0,64)) &&
        (name.size() == 64 || name[64] == '-')) {
      try {
        const auto version = entry.path().filename();
        const auto marker = under(root, version / "received-package.json");
        candidates.push_back({version, fs::last_write_time(marker)});
      } catch (const std::exception &) { continue; }
      need(candidates.size() <= 4096, "Downloaded package cache is too large");
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
    return a.modified != b.modified ? a.modified > b.modified
                                     : a.version > b.version;
  });
  for (const auto &candidate : candidates) {
    try { return received(root, candidate.version, alias, id, map_sha); }
    catch (const std::exception &) { /* incomplete or damaged: download again */ }
  }
  return {};
}
Level write_downloaded_package(const fs::path &root, const Level &expected,
                               const std::vector<uint8_t> &map,
                               const std::vector<uint8_t> &image,
                               const std::string &package_sha,
                               const std::string &preview_sha) {
  need(hex_hash(package_sha) &&
           (preview_sha.empty() || hex_hash(preview_sha)),
       "Invalid received package checksum");
  // Re-use the same strict identity/container validators as local exports.
  const auto normalized = metadata(record(expected, false));
  need(normalized.alias == expected.alias && hash(map) == expected.map_sha256,
       "Received custom map checksum differs");
  ssw(map);
  if (!image.empty()) {
    need(hash(image) == preview_sha, "Received preview checksum differs");
    png(image);
  } else {
    need(preview_sha.empty() || preview_sha == hash(image),
         "Received preview is missing");
  }
  plain(root);
  const CacheLock lock(root);
  // Existing versions may be used by another live client. Never replace their
  // files, including when repairing corrupted data or receiving new metadata.
  fs::path version = package_sha;
  // A previous damaged base version may already have a valid repair. Reuse
  // that exact package too. Its data stays immutable; marker mtime records the
  // most recently selected host version for the next cache advertisement.
  for (const auto &entry : fs::directory_iterator(root)) {
    const auto candidate = entry.path().filename();
    const auto name = candidate.string();
    if (name != package_sha && !name.starts_with(package_sha + "-")) continue;
    try {
      const auto old = received(root, candidate, expected.alias, expected.id,
                                expected.map_sha256);
      if (old.package_sha256 == package_sha && old.preview_sha256 == preview_sha &&
          old.level.title == expected.title && old.level.base_level == expected.base_level &&
          old.level.modes == expected.modes) {
        fs::last_write_time(under(root, candidate / "received-package.json"),
                            fs::file_time_type::clock::now());
        return old.level;
      }
    } catch (const std::exception &) {}
  }
  if (fs::exists(under(root, version))) {
    for (unsigned repair = 1;; ++repair) {
      need(repair <= 4096, "Too many repaired downloaded package versions");
      version = package_sha + "-" + std::to_string(repair);
      if (!fs::exists(under(root, version))) break;
    }
  }
  const auto folder = under(root, version);
  fs::create_directories(folder);
  auto result = expected;
  result.folder = folder;
  result.map_path = under(folder, "map.ssw");
  result.preview_path = image.empty() ? fs::path{} : under(folder, "preview.png");
  atomic(result.map_path, map);
  if (!image.empty()) atomic(result.preview_path, image);
  atomic(under(folder, "level.json"), encoded(record(result, false)));
  auto marker = record(result, false);
  marker.object()->emplace("package_sha256", package_sha);
  marker.object()->emplace("preview_sha256", preview_sha);
  // The marker is last: interrupted downloads are never considered installed.
  atomic(under(folder, "received-package.json"), encoded(marker));
  return received(root, version, expected.alias, expected.id,
                  expected.map_sha256).level;
}
Prepared prepare_data(const fs::path &original, const Catalog &catalog,
                      const fs::path &target) {
  need(catalog.levels.size() <= package_limit,
       "Custom-level package limit is 128");
  plain(original);
  plain(target);
  if (catalog.levels.empty())
    return {original, {}, {}};
  need(!descendant(target, original) && !descendant(original, target),
       "Custom game data must be separate from original game data");
  const CacheLock cache_lock(target);
  auto fst = bytes(under(original, "sys/fst.bin"), 16 * 1024 * 1024),
       boot = bytes(under(original, "sys/boot.bin"), 1024 * 1024);
  const auto source_dol_sha =
      hash(bytes(under(original, "sys/main.dol"), map_limit));
  need(boot.size() >= 0x440, "Original boot header is incomplete");
  const auto source_boot_sha = hash(boot);
  uint64_t end = 0;
  auto tree = parse_fst(fst, end);
  end = std::max(end, uint64_t(word(boot, 0x424)) + fst.size());
  if (fs::is_regular_file(original / "sys/main.dol"))
    end = std::max(end, uint64_t(word(boot, 0x420)) +
                            fs::file_size(original / "sys/main.dol"));
  auto levels =
      std::find_if(tree.children.begin(), tree.children.end(), [](auto &n) {
        return n.directory && lower(n.name) == "levels";
      });
  need(levels != tree.children.end(),
       "Original virtual disc has no Levels directory");
  const auto manifest_path = under(target, "custom-levels.json");
  if (fs::exists(manifest_path)) {
    const auto old = read_catalog_file(manifest_path, target);
    const auto info = json(manifest_path, 1024 * 1024);
    need(old.warnings.empty() &&
             catalog_fingerprint(old) == catalog_fingerprint(catalog) &&
             text(info, "source_fst_sha256") == hash(fst) &&
             text(info, "source_boot_sha256") == hash(boot) &&
             text(info, "source_dol_sha256") == source_dol_sha,
         "Completed custom data cache differs; use a new cache directory");
    const auto cached_fst =
                   bytes(under(target, "sys/fst.bin"), 16 * 1024 * 1024),
               cached_boot = bytes(under(target, "sys/boot.bin"), 1024 * 1024);
    uint64_t cached_end = 0;
    const auto cached_tree = parse_fst(cached_fst, cached_end);
    need(cached_boot.size() >= 0x440 &&
             word(cached_boot, 0x428) == cached_fst.size() &&
             word(cached_boot, 0x42c) == cached_fst.size(),
         "Cached custom virtual disc header differs");
    const auto directory = std::find_if(
        cached_tree.children.begin(), cached_tree.children.end(),
        [](const auto &n) { return n.directory && lower(n.name) == "levels"; });
    need(directory != cached_tree.children.end(),
         "Cached custom virtual disc has no Levels directory");
    std::map<std::string, std::pair<uint32_t, uint32_t>> original_files,
        cached_files;
    auto collect = [&](auto &&self, const FstNode &node,
                       const std::string &parent, auto &out) -> void {
      const auto path = parent + node.name;
      if (node.directory)
        for (const auto &child : node.children)
          self(self, child, path.empty() ? "" : path + "/", out);
      else
        out.emplace(lower(path), std::pair{node.offset, node.size});
    };
    collect(collect, tree, "", original_files);
    collect(collect, cached_tree, "", cached_files);
    for (const auto &[name, extent] : original_files)
      need(cached_files.contains(name) && cached_files.at(name) == extent,
           "Cached custom disc altered an original file extent");
    std::vector<std::pair<uint64_t, uint64_t>> custom_extents;
    for (const auto &level : old.levels) {
      const auto found =
          std::find_if(directory->children.begin(), directory->children.end(),
                       [&](const auto &n) {
                         return !n.directory && n.name == level.alias + ".ssw";
                       });
      need(found != directory->children.end() &&
               found->size == fs::file_size(level.map_path) &&
               found->offset >= end,
           "Cached custom virtual disc map extent differs");
      custom_extents.emplace_back(found->offset,
                                  uint64_t(found->offset) + found->size);
    }
    std::sort(custom_extents.begin(), custom_extents.end());
    for (size_t n = 1; n < custom_extents.size(); ++n)
      need(custom_extents[n].first >= custom_extents[n - 1].second,
           "Cached custom map extents overlap");
    need(!custom_extents.empty() &&
             word(cached_boot, 0x424) >= custom_extents.back().second,
         "Cached custom FST overlaps map data");
    return {target, manifest_path, old.levels};
  }
  std::set<std::string> names;
  for (const auto &n : levels->children)
    names.insert(lower(n.name));
  Prepared result{target, under(target, "custom-levels.json"), {}};
  fs::create_directories(target);
  for (const auto &entry : fs::recursive_directory_iterator(original)) {
    plain(entry.path());
    const auto relative = entry.path().lexically_relative(original);
    const auto to = under(target, relative);
    if (entry.is_directory())
      fs::create_directories(to);
    else if (entry.is_regular_file()) {
      fs::create_directories(to.parent_path());
      bool linked = false;
      std::error_code ec;
      if (fs::exists(to)) {
        need(fs::is_regular_file(to), "Custom data cache has a non-file asset");
        linked = fs::equivalent(entry.path(), to, ec);
        if (linked)
          continue;
        if (fs::file_size(entry.path()) == fs::file_size(to) &&
            fs::last_write_time(entry.path()) == fs::last_write_time(to))
          continue;
      } else {
        fs::create_hard_link(entry.path(), to, ec);
        if (!ec)
          continue;
      }
      atomic_copy(entry.path(), to);
    }
  }
  J::Array records;
  std::set<std::string> ids;
  for (const auto &input : catalog.levels) {
    need(valid_id(input.id) && valid_id(input.base_level) &&
             input.alias == map_alias(input.id, input.map_sha256) &&
             input.modes && !(input.modes & ~kAllModes),
         "Invalid custom-level catalog entry");
    title_valid(input.title);
    need(ids.insert(lower(input.id)).second &&
             names.insert(lower(input.alias + ".ssw")).second,
         "Duplicate custom-level alias");
    need(std::any_of(levels->children.begin(), levels->children.end(),
                     [&](auto &n) {
                       return !n.directory &&
                              lower(n.name) == lower(input.base_level + ".ssw");
                     }),
         "Custom-level base map does not exist in this ROM");
    const auto map = bytes(input.map_path, map_limit);
    ssw(map);
    need(hash(map) == input.map_sha256,
         "Custom-level map changed during preparation");
    end = (end + 31) & ~uint64_t(31);
    need(end + map.size() < 0x100000000ull,
         "Custom virtual disc exceeds its address range");
    levels->children.push_back(
        {input.alias + ".ssw", false, uint32_t(end), uint32_t(map.size()), {}});
    end += map.size();
    auto value = input;
    value.folder = target;
    value.map_path =
        under(target, fs::path("files/Levels") / (input.alias + ".ssw"));
    atomic(value.map_path, map);
    if (!input.preview_path.empty()) {
      const auto image = bytes(input.preview_path, png_limit);
      png(image);
      value.preview_path = under(target, fs::path("custom-level-previews") /
                                             (input.alias + ".png"));
      atomic(value.preview_path, image);
    }
    records.push_back(record(value, true));
    result.levels.push_back(std::move(value));
  }
  const auto table = encode_fst(tree);
  const auto fst_offset = (end + 31) & ~uint64_t(31);
  need(fst_offset + table.size() < 0x100000000ull,
       "Custom virtual disc FST exceeds its address range");
  word(boot, 0x424, uint32_t(fst_offset));
  word(boot, 0x428, uint32_t(table.size()));
  word(boot, 0x42c, uint32_t(table.size()));
  atomic(under(target, "sys/fst.bin"), table);
  atomic(under(target, "sys/boot.bin"), boot);
  atomic(result.catalog_file,
         encoded(J::Object{{"schema", 1},
                           {"source_fst_sha256", hash(fst)},
                           {"source_boot_sha256", source_boot_sha},
                           {"source_dol_sha256", source_dol_sha},
                           {"levels", std::move(records)}}));
  return result;
}
Catalog read_catalog_file(const fs::path &path, const fs::path &expected) {
  const auto root = path.parent_path();
  plain(root);
  plain(path);
  if (!expected.empty())
    need(fs::weakly_canonical(root) == fs::weakly_canonical(expected),
         "Custom catalog does not belong to the mounted game data");
  Catalog result;
  result.data_dir = root;
  need(fs::is_regular_file(path), "Custom-level catalog file does not exist");
  const auto info = json(path, 1024 * 1024);
  need(integer(info, "schema") == 1, "Unsupported custom catalog version");
  const auto list = info.find("levels");
  need(list && list->array() && list->array()->size() <= package_limit,
       "Invalid custom catalog level list");
  // The catalogue is an index of the mounted private disc, rather than an
  // independent list of arbitrary loose files. Verify the aliases are actually
  // addressable through its FST before the native engine sees them.
  const auto table = bytes(under(root, "sys/fst.bin"), 16 * 1024 * 1024);
  const auto boot = bytes(under(root, "sys/boot.bin"), 1024 * 1024);
  uint64_t total_end = 0;
  const auto tree = parse_fst(table, total_end);
  need(boot.size() >= 0x440 && word(boot, 0x428) == table.size() &&
           word(boot, 0x42c) == table.size(),
       "Custom catalog FST does not match its mounted boot header");
  const auto directory = std::find_if(
      tree.children.begin(), tree.children.end(), [](const auto &node) {
        return node.directory && lower(node.name) == "levels";
      });
  need(directory != tree.children.end(),
       "Custom catalog has no mounted Levels directory");
  uint64_t original_end = 0;
  auto collect_original = [&](auto &&self, const FstNode &node,
                              const std::string &parent) -> void {
    const auto path = parent + node.name;
    if (node.directory)
      for (const auto &child : node.children)
        self(self, child, path.empty() ? "" : path + "/");
    else if (!lower(path).starts_with("levels/custom_"))
      original_end = std::max(original_end, uint64_t(node.offset) + node.size);
  };
  collect_original(collect_original, tree, "");
  std::vector<std::pair<uint64_t, uint64_t>> extents;
  for (const auto &node : directory->children)
    if (!node.directory && node.name.starts_with("Custom_")) {
      need(node.offset >= original_end && !(node.offset & 31) &&
               uint64_t(node.offset) + node.size <= word(boot, 0x424),
           "Custom catalog alias overlaps original data or its FST");
      extents.emplace_back(node.offset, uint64_t(node.offset) + node.size);
    }
  std::sort(extents.begin(), extents.end());
  for (size_t n = 1; n < extents.size(); ++n)
    need(extents[n].first >= extents[n - 1].second,
         "Custom catalog alias extents overlap");
  std::set<std::string> aliases, ids;
  for (const auto &entry : *list->array()) {
    try {
      auto value = metadata(entry);
      need(ids.insert(lower(value.id)).second &&
               aliases.insert(lower(value.alias)).second,
           "Duplicate custom catalog identity");
      need(text(entry, "map") == "files/Levels/" + value.alias + ".ssw",
           "Custom catalog map is outside its prepared data");
      value.folder = root;
      value.map_path = under(root, fs::u8path(text(entry, "map")));
      need(std::any_of(directory->children.begin(), directory->children.end(),
                       [&](const auto &node) {
                         return !node.directory &&
                                node.name == value.alias + ".ssw" &&
                                node.size == fs::file_size(value.map_path);
                       }),
           "Custom map is not registered in the mounted virtual disc");
      const auto map = bytes(value.map_path, map_limit);
      ssw(map);
      need(hash(map) == value.map_sha256, "Prepared custom map hash differs");
      const auto preview = text(entry, "preview");
      need(preview.empty() ||
               preview == "custom-level-previews/" + value.alias + ".png",
           "Custom catalog preview is outside its prepared data");
      if (!preview.empty()) {
        value.preview_path = under(root, fs::u8path(preview));
        png(bytes(value.preview_path, png_limit));
      }
      result.levels.push_back(std::move(value));
    } catch (const std::exception &error) {
      result.warnings.push_back(std::string("Custom catalog entry: ") +
                                error.what());
    }
  }
  return result;
}
Catalog read_prepared_catalog(const fs::path &root) {
  const auto path = under(root, "custom-levels.json");
  if (!fs::exists(path)) {
    Catalog empty;
    empty.data_dir = root;
    return empty;
  }
  return read_catalog_file(path, root);
}
} // namespace gc::custom
