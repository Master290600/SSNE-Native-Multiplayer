#pragma once
#include <cstdint>

namespace port {
// Install only for the verified original PAL functions. Guest-thread API.
bool install_native_crosshair();
// Cheap atomic snapshots for the host settings/render thread.
bool native_crosshair_supported();
unsigned native_crosshair_language(); // 0 English, 1 German, 2 French.
// Guest frame safe point: retire deleted/reused HUD identities and publish language.
void native_crosshair_frame();
// Guest-thread observer after the original material bind. Shared by the
// offline/authority hook chains, including packages first received mid-session.
// It receives the original caller/quad without copying a whole PPC context.
using NativeMaterialObserver=void(*)(uint32_t caller,uint32_t quad);
void set_native_material_observer(NativeMaterialObserver);
}
