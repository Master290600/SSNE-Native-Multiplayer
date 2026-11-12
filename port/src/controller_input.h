#pragma once
#include "settings.h"
#include "ssne/input.h"
#include <algorithm>
#include <cmath>
namespace port {
inline uint8_t controller_axis_to_u8(float v){return uint8_t(std::clamp(128.f+v*127.f,0.f,255.f));}
inline void controller_deadzone(float& x,float& y,float dz){
    float m=std::sqrt(x*x+y*y);if(m<=dz || m<=0){x=y=0;return;}
    float k=std::min(1.f,(m-dz)/std::max(1e-3f,1.f-dz))/m;x*=k;y*=k;
}
inline int controller_player(const Settings& settings,int slot,bool network){
    return network && settings.pad_player[slot]>0?std::clamp(settings.kbm_player-1,0,3):settings.pad_player[slot]-1;
}
struct PlayerInput {
    float act[port::ACT_COUNT] = {};  // 0..1 per control
    float lx = 0, ly = 0, cx = 0, cy = 0;
};

// A controller's part of its player's input.
inline void sample_controller(const Settings& settings, SDL_GameController* c, PlayerInput& in, bool control, bool held_jump) {
    for (int a = port::kFirstButtonAction; a < port::ACT_COUNT; a++) {
        const port::Bind& b = settings.pad[a];
        float v = 0;
        if (b.type == port::Bind::PAD_BUTTON) v = SDL_GameControllerGetButton(c, SDL_GameControllerButton(b.code)) ? 1.f : 0.f;
        else if (b.type == port::Bind::PAD_AXIS) v = std::max(0.f, SDL_GameControllerGetAxis(c, SDL_GameControllerAxis(b.code)) / 32767.f);
        int target = a;
        if (a == port::ACT_L && b.type == port::Bind::PAD_BUTTON) {
            if (held_jump) continue;
            if (!control) target = port::ACT_A;
        }
        in.act[target] = std::max(in.act[target], v);
    }
    auto ax = [c](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(c, a) / 32767.f; };
    float mx = ax(SDL_CONTROLLER_AXIS_LEFTX), my = -ax(SDL_CONTROLLER_AXIS_LEFTY);
    float rx = ax(SDL_CONTROLLER_AXIS_RIGHTX), ry = -ax(SDL_CONTROLLER_AXIS_RIGHTY);
    if (settings.swap_sticks) { std::swap(mx, rx); std::swap(my, ry); }
    controller_deadzone(mx, my, settings.pad_deadzone);
    controller_deadzone(rx, ry, settings.pad_deadzone);
    in.lx += mx; in.ly += my;
    in.cx += rx * settings.look_sens;
    in.cy += (settings.pad_invert_y ? -ry : ry) * settings.look_sens;
}

inline ssne::PadState map_player_input(const Settings& settings,const PlayerInput& in) {
    ssne::PadState p;
    struct { int a; uint16_t m; } map[] = {
        {port::ACT_A, ssne::PAD_A}, {port::ACT_B, ssne::PAD_B}, {port::ACT_X, ssne::PAD_X}, {port::ACT_Y, ssne::PAD_Y},
        {port::ACT_Z, ssne::PAD_Z}, {port::ACT_START, ssne::PAD_START},
        {port::ACT_DPAD_UP, ssne::PAD_UP}, {port::ACT_DPAD_DOWN, ssne::PAD_DOWN},
        {port::ACT_DPAD_LEFT, ssne::PAD_LEFT}, {port::ACT_DPAD_RIGHT, ssne::PAD_RIGHT}};
    for (auto& m : map) if (in.act[m.a] >= 0.5f) p.buttons |= m.m;
    float lt = in.act[port::ACT_L], rt = in.act[port::ACT_R];
    p.stick_x = controller_axis_to_u8(std::clamp(in.lx, -1.f, 1.f));
    p.stick_y = controller_axis_to_u8(std::clamp(in.ly, -1.f, 1.f));
    p.cstick_x = controller_axis_to_u8(std::clamp(in.cx, -1.f, 1.f));
    p.cstick_y = controller_axis_to_u8(std::clamp(in.cy, -1.f, 1.f));
    p.trigger_l = uint8_t(std::clamp(lt, 0.f, 1.f) * 255);
    p.trigger_r = uint8_t(std::clamp(rt, 0.f, 1.f) * 255);
    float click = std::min(settings.trigger_threshold, 0.999f);
    if (lt >= click) p.buttons |= ssne::PAD_L;   // full press = digital click, as on the real controller
    if (rt >= click) p.buttons |= ssne::PAD_R;
    return p;
}

}
