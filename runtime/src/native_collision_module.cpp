#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include <array>
#include <optional>
#include <stdexcept>
#include <cstring>
#include <cmath>
namespace ssne {
#include "native_collision_traversal.inc"
#include "native_collision_candidates.inc"
#include "native_distance_squared.inc"
}

namespace ssne::native_collision_module {void install(){native_collision_traversal::install();native_collision_candidates::install();native_distance_squared::install();}}
