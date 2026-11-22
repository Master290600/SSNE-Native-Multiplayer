#pragma once
#include "ssne/native_party.h"

namespace ssne {
// PAL walking controller's short downward sphere cast. A support position is
// the resolved sphere centre, and its normal points into the supporting surface.
bool native_ground_support(PPCContext&,const std::array<float,3>& centre,float radius,
                           std::optional<NativeSegmentHit>& support,std::string& error);
}
