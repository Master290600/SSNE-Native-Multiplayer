#pragma once
#include <functional>
#include "ssne/mods.h"
namespace port {
struct ModMenuControls {
    bool netplay = false, host = true;
    ssne::ModOptions requested;
    std::function<void(ssne::ModOptions)> apply;
    std::function<void(ssne::ModOptions)> apply_local;
    std::function<void(unsigned,int,float)> spawn;
};
void draw_rapid_fire(const ModMenuControls& controls, const ssne::ModState& state);
void draw_spawn_controls(const ModMenuControls& controls, const ssne::ModState& state);
void draw_aim_controls(const ModMenuControls& controls, const ssne::ModState& state);
void draw_mod_overlay(float x,float y,float width,float height);
}
