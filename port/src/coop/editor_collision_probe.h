#pragma once
#include "ssne/ppc_context.h"
#include <cstdint>
namespace port::coop {
// Hidden acceptance diagnostics only. Inactive unless both dedicated test
// environment variables are present; never changes an actor or game outcome.
void sample_editor_collision_probe(PPCContext&,uint64_t frame);
}
