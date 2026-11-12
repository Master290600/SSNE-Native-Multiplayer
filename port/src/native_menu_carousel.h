#pragma once
#include "native_menu_mouse.h"

namespace port {
// Read the original level carousel's retained draw state and callback registry.
// Selection and confirmation always go through its existing GUI events.
std::vector<NativeMenuMouseRegion> native_carousel_mouse_regions(PPCContext&);
// Guest-thread copied diagnostics for private pointer acceptance fixtures.
std::string native_carousel_mouse_diagnostics();
bool native_carousel_mouse_event(PPCContext&,uint32_t gui,uint32_t node,
                                uint16_t root,uint16_t id,unsigned event);
}
