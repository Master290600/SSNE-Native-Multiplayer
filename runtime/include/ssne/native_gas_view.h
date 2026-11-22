#pragma once

#include <cstdint>

struct PPCContext;

namespace ssne {
// Called after the local render lease has installed the actual hand model,
// camera and animated bone palette for this frame.
void present_local_gas_stream(PPCContext&, uint32_t actor, uint64_t epoch,
                              bool firing, double now);
}
