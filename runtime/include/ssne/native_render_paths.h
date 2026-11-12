#pragma once
#include "ssne/ppc_context.h"
namespace ssne {
// Keeps the replica pose/weapon guards installed at80129958. Only its native
// body is replaced; the translated function remains available for diagnostics.
void native_skinning_dispatch(PPCContext&,PPCFunc translated);
}
