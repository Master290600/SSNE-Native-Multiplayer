#pragma once
#include "ssne/ppc_context.h"
#include <cstdint>
#include <string>

namespace port::coop {
// Configure once before the guest starts. An empty path leaves all sampling
// and lifetime observation disabled. This is a local editor diagnostic only.
void configure_editor_logic_debug(std::string output_path,
                                  std::string session_token,
                                  std::string expected_level);
// Guest-frame observer only: no guest writes, native calls, synthetic events,
// or full render/world captures. Samples at most four times per wall second.
void sample_editor_logic_debug(PPCContext &, uint64_t frame);
} // namespace port::coop
