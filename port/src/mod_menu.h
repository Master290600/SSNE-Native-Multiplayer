#pragma once
#include <functional>
#include "ssne/mods.h"
namespace port {
bool mod_menu_open();
void mod_menu_select_tab(int);
void mod_menu_set_open(bool open);
struct ModMenuControls {
    bool netplay = false, host = true;
    ssne::ModOptions requested;
    std::function<void(ssne::ModOptions)> apply;
    std::function<void(ssne::ModOptions)> apply_local;
    std::function<void(unsigned,int,float)> spawn;
};
void draw_mod_menu(const ModMenuControls& controls = {});
void draw_mod_overlay(float x,float y,float width,float height);
}
