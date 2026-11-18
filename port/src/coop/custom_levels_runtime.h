#pragma once
#include "gc/custom_levels.h"
#include "world_state.h"
#include <string_view>

namespace port::coop {
// Configured once before the guest starts. The prepared catalogue owns every
// alias and file identity used by both the native menu and network loader.
void configure_custom_levels(gc::custom::Catalog);
const gc::custom::Catalog &custom_levels_catalog();
// Boot entries never move. Received entries have stable addresses and can
// override display metadata for the same alias without invalidating GUI data.
std::vector<const gc::custom::Level *> custom_levels_available();
uint64_t custom_levels_generation();
void register_downloaded_custom_level(gc::custom::Level);
const gc::custom::Level *find_custom_level(std::string_view alias);
bool is_custom_level_alias(std::string_view);
// Menu and actual host/client load validation share the same exact PS2 map
// identity. Existing catalog/wire mode masks remain untouched.
bool custom_level_is_bundled_ps2(const gc::custom::Level &);
bool custom_level_supports_mode(const gc::custom::Level &, unsigned native_mode);
bool custom_level_in_user_category(const gc::custom::Level &, unsigned native_mode);

// Authority publishes an exact package identity; replicas check it before any
// native level load. Display names and thumbnails do not affect compatibility.
bool bind_custom_level_identity(const std::string &level, unsigned native_mode,
                                Properties &, std::string &error,
                                bool require_mode = true);
bool verify_custom_level_identity(const std::string &level,
                                  unsigned native_mode, const Properties &,
                                  std::string &error, bool require_mode = true);
} // namespace port::coop
