#pragma once
#include <cstdint>
namespace ssne {
struct AmmoRules {unsigned capacity=0,pickup_multiplier=1;}; // 0 normal, 1 double, 2 native maximum, 3..5 multiplier
void set_ammo_rules(unsigned capacity,unsigned pickup_multiplier);
AmmoRules ammo_rules();
void install_ammo_rule_hooks();
void apply_ammo_limits(uint32_t inventory);
}
