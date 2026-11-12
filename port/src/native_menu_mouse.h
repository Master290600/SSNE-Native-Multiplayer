#pragma once
#include "ssne/ppc_context.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace port {
// SDL supplies coordinates in the displayed game image; guest addresses never
// cross to the window thread. Generations invalidate queued clicks on transitions.
struct NativeMenuPointerEvent {
    enum class Kind : uint8_t { Move, Down, Up, Wheel, Cancel };
    Kind kind=Kind::Move;
    float u=0, v=0;
    float display_wide=1; // Factor belonging to the image that received this event.
    bool inside=false;
    uint8_t button=0; // SDL: left=1, middle=2, right=3.
    float wheel_x=0, wheel_y=0;
    uint64_t sequence=0, image_sequence=0;
    uint64_t surface_generation=0, geometry_generation=0;
};
struct NativeMenuMouseSnapshot {
    bool active=false;
    bool has_surface=false;
    uint64_t surface_generation=0, geometry_generation=0;
};
void queue_native_menu_pointer(const NativeMenuPointerEvent&);
// The display thread supplies the same factor used to fit the displayed image.
void set_native_menu_mouse_display(float wide);
NativeMenuMouseSnapshot native_menu_mouse_snapshot();
bool menu_mouse_wants_pointer();
uint64_t menu_mouse_surface_generation();
// Guest-thread entry, after native GUI update and before the next frame input.
void consume_native_menu_pointer(PPCContext&);
void install_native_menu_mouse();
// Authority is supplied by the multiplayer adapter; standalone menus allow all
// native selectable controls. event is the original native GUI event number.
struct NativeMenuMouseRegion {
    uint32_t gui=0,node=0;
    // Optional original native draw object; includes its retained renderer path.
    uint32_t primitive=0;
    // Optional live GUI node whose text viewport supplies the projected region.
    uint32_t viewport_node=0;
    uint16_t root=0,id=0;
    // Original centered 640x480 canvas coordinates, with the live animation applied.
    float left=0,top=0,right=0,bottom=0;
    unsigned event=22; // Zero: wheel-only; no hover focus or left-click action.
    unsigned previous_event=0,next_event=0;
    std::string control;
};
struct NativeMenuMouseServices {
    // Logical pages may reuse the same native nodes and definitions.
    std::function<uint64_t()> surface_epoch;
    std::function<bool(uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event)> allowed;
    // Return true when network/custom controls have handled the original intent.
    std::function<bool(PPCContext&,uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event)> intercept;
    std::function<std::vector<NativeMenuMouseRegion>(PPCContext&)> regions;
    std::function<bool(uint32_t gui,uint32_t node,uint16_t root,uint16_t id)> model_region;
    std::function<int(uint32_t gui,uint32_t node,uint16_t root,uint16_t id)> model_owner;
};
void configure_native_menu_mouse(NativeMenuMouseServices);
// Hidden diagnostics expose only copied numeric state, never live UI-thread RAM.
std::string native_menu_mouse_diagnostics();
}
