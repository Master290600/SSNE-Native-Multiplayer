#pragma once
#include <cstdint>
namespace ssne {
// Guest-thread requests from the authenticated cooperative input stream.
void coop_weapon_group(unsigned slot, int group);
uint16_t injected_buttons(int port);
}
