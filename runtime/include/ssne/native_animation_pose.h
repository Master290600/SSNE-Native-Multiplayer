#pragma once
#include "ppc_context.h"
namespace ssne {
bool native_animation_pose_available();
void native_animation_pose_dispatch(PPCContext&,PPCFunc translated);
}
