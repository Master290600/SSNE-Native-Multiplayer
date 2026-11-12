#pragma once

namespace port {
// Install only for the verified original PAL functions. Guest-thread API.
bool install_native_crosshair();
// Cheap atomic snapshots for the host settings/render thread.
bool native_crosshair_supported();
unsigned native_crosshair_language(); // 0 English, 1 German, 2 French.
// Guest frame safe point: retire deleted/reused HUD identities and publish language.
void native_crosshair_frame();
}
