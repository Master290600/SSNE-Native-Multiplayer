#include "custom_levels_runtime.h"
#include "ps2_content_identity.h"
#include "ps2_multiplayer_maps.h"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace port::coop {
namespace {
gc::custom::Catalog catalog;
std::map<std::string, gc::custom::Level, std::less<>> received_levels;
uint64_t generation = 0;
constexpr const char *id_key = "Session.Custom.Id";
constexpr const char *base_key = "Session.Custom.Base";
constexpr const char *hash_key = "Session.Custom.SHA256";
constexpr const char *alias_key = "Session.Custom.Alias";
const std::string *text(const Properties &p, const char *key) {
  const auto field = p.find(key);
  return field == p.end() ? nullptr : std::get_if<std::string>(&field->second);
}
unsigned effective_mode(unsigned mode, const Properties &p) {
  const auto survival = p.find("Session.Survival.Active");
  return survival != p.end() && survival->second == Property(int64_t(1)) ? 5u
                                                                         : mode;
}
bool has_custom_fields(const Properties &p) {
  return p.contains(id_key) || p.contains(base_key) || p.contains(hash_key) ||
         p.contains(alias_key);
}
void clear_custom_fields(Properties &p) {
  for (auto it = p.begin(); it != p.end();)
    if (it->first.starts_with("Session.Custom."))
      it = p.erase(it);
    else
      ++it;
}
bool available(const gc::custom::Level &entry, unsigned mode,
               const Properties &p, std::string &error) {
  if (custom_level_supports_mode(entry, effective_mode(mode, p)))
    return true;
  error = "Custom level does not support the selected game mode: " + entry.id;
  return false;
}
} // namespace

bool is_custom_level_alias(std::string_view name) {
  return name.starts_with("Custom_");
}
bool custom_level_is_bundled_ps2(const gc::custom::Level &entry) {
  return bundled_ps2_map_identity(
      entry, installed_skin_count.load(std::memory_order_relaxed));
}
bool custom_level_supports_mode(const gc::custom::Level &entry, unsigned mode) {
  return gc::custom::supports_mode(entry, mode) ||
         (mode == 5 && custom_level_is_bundled_ps2(entry));
}
bool custom_level_in_user_category(const gc::custom::Level &entry, unsigned mode) {
  return !custom_level_is_bundled_ps2(entry) && custom_level_supports_mode(entry, mode);
}
void configure_custom_levels(gc::custom::Catalog next) {
  if (next.levels.size() > 256)
    throw std::invalid_argument("Too many prepared custom levels");
  std::set<std::string> aliases, ids;
  for (const auto &entry : next.levels) {
    const bool hash =
        entry.map_sha256.size() == 64 &&
        std::all_of(entry.map_sha256.begin(), entry.map_sha256.end(),
                    [](char c) {
                      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                    });
    if (!hash || !gc::custom::valid_id(entry.id) || entry.base_level.empty() ||
        is_custom_level_alias(entry.base_level) || !entry.modes ||
        (entry.modes & ~gc::custom::kAllModes) ||
        entry.alias != gc::custom::map_alias(entry.id, entry.map_sha256) ||
        !aliases.insert(entry.alias).second || !ids.insert(entry.id).second)
      throw std::invalid_argument(
          "Invalid or duplicate prepared custom level identity");
  }
  catalog = std::move(next);
  received_levels.clear();
  ++generation;
}
const gc::custom::Catalog &custom_levels_catalog() { return catalog; }
std::vector<const gc::custom::Level *> custom_levels_available() {
  std::vector<const gc::custom::Level *> result;
  for (const auto &entry : catalog.levels)
    if (!received_levels.contains(entry.alias)) result.push_back(&entry);
  for (const auto &[alias, entry] : received_levels) result.push_back(&entry);
  return result;
}
uint64_t custom_levels_generation() { return generation; }
void register_downloaded_custom_level(gc::custom::Level entry) {
  if (!gc::custom::valid_id(entry.id) || !gc::custom::valid_id(entry.base_level) ||
      is_custom_level_alias(entry.base_level) || entry.title.empty() ||
      entry.title.size() > 96 || !entry.modes || (entry.modes & ~gc::custom::kAllModes) ||
      entry.alias != gc::custom::map_alias(entry.id, entry.map_sha256) ||
      entry.map_path.empty())
    throw std::invalid_argument("Invalid received custom level identity");
  if (!received_levels.contains(entry.alias) && received_levels.size() >= 256)
    throw std::invalid_argument("Too many received custom levels in one session");
  const auto old = received_levels.find(entry.alias);
  if (old != received_levels.end()) {
    const auto &v = old->second;
    if (v.title == entry.title && v.base_level == entry.base_level &&
        v.modes == entry.modes && v.map_path == entry.map_path &&
        v.preview_path == entry.preview_path) return;
  }
  received_levels.insert_or_assign(entry.alias, std::move(entry));
  ++generation;
}
const gc::custom::Level *find_custom_level(std::string_view alias) {
  if (const auto received = received_levels.find(alias); received != received_levels.end())
    return &received->second;
  const auto found =
      std::find_if(catalog.levels.begin(), catalog.levels.end(),
                   [&](const auto &entry) { return entry.alias == alias; });
  return found == catalog.levels.end() ? nullptr : &*found;
}
bool bind_custom_level_identity(const std::string &level, unsigned mode,
                                Properties &setup, std::string &error,
                                bool require_mode) {
  const auto *entry = find_custom_level(level);
  if (!entry) {
    if (is_custom_level_alias(level)) {
      error = "Custom level is not installed: " + level;
      return false;
    }
    clear_custom_fields(setup);
    return true;
  }
  if (require_mode && !available(*entry, mode, setup, error))
    return false;
  setup[id_key] = entry->id;
  setup[base_key] = entry->base_level;
  setup[hash_key] = entry->map_sha256;
  setup[alias_key] = entry->alias;
  // Set3 belongs only to the menu's carousel. The original rules writer keeps
  // its proven 0..2 range; engine loading uses the full alias string.
  for (const char *prefix :
       {"GameSetup.RulesCoop", "GameSetup.RulesFragmatch",
        "GameSetup.RulesHoldTheFlag", "GameSetup.RulesPassTheBomb"}) {
    const auto name = setup.find(std::string(prefix) + ".Level");
    if (name != setup.end() && name->second == Property(level))
      setup[std::string(prefix) + ".LevelSet"] = int64_t(0);
  }
  return true;
}
bool verify_custom_level_identity(const std::string &level, unsigned mode,
                                  const Properties &setup, std::string &error,
                                  bool require_mode) {
  const auto *entry = find_custom_level(level);
  if (!entry && !is_custom_level_alias(level)) {
    if (!has_custom_fields(setup))
      return true;
    error = "Host custom level identity does not match the selected level";
    return false;
  }
  const auto id = text(setup, id_key), base = text(setup, base_key),
             hash = text(setup, hash_key), alias = text(setup, alias_key);
  if (!id || !base || !hash || !alias || *alias != level) {
    error = "Host custom level identity is missing or invalid";
    return false;
  }
  if (!entry) {
    // A changed package has a different alias. Distinguish it from an absent
    // installation without using a display name as a compatibility key.
    const auto same_id =
        std::find_if(catalog.levels.begin(), catalog.levels.end(),
                     [&](const auto &local) { return local.id == *id; });
    error = same_id == catalog.levels.end()
                ? "Custom level is not installed: " + *id +
                      ". Install the same CustomLevels package as the host."
                : "Custom level differs from the host: " + *id +
                      ". Install the matching map version.";
    return false;
  }
  if (entry->id != *id || entry->base_level != *base ||
      entry->map_sha256 != *hash || entry->alias != *alias) {
    error = "Custom level differs from the host: " + *id;
    return false;
  }
  return !require_mode || available(*entry, mode, setup, error);
}
} // namespace port::coop
