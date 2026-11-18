#pragma once
#include "gc/custom_levels.h"
#include <memory>
#include <string>
#include <vector>

struct PPCContext;
namespace ssne {
struct CustomLevelMountPlan {
    std::string alias, map_sha256;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};
// Worker thread: read only a fixed cache map.ssw, check the complete SHA and
// retain immutable bytes. No guest state, FST or active data folder is changed.
std::shared_ptr<const CustomLevelMountPlan>
prepare_custom_level_mount(const gc::custom::Level&, std::string& error);
// Guest thread at the no-map-load boundary, before queueing the native level.
// PAL SDK globals and DI extents switch together; the game is never restarted.
bool activate_custom_level_mount(PPCContext&,
                                const std::shared_ptr<const CustomLevelMountPlan>&,
                                std::string& error);
void reset_custom_level_mounts(); // runtime boot only

// Pure append operation, used by activation and regression tests. Every prior
// entry index, directory extent, file extent and name offset is retained; only
// root.end changes, and its string table relocates as a whole.
bool append_custom_level_fst(const std::vector<uint8_t>& original,
                             const std::string& alias, uint32_t disc_offset,
                             uint32_t map_size, std::vector<uint8_t>& result,
                             uint32_t& entry_index, std::string& error);
} // namespace ssne
