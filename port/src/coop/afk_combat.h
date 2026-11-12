#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace port::coop {
// Maximum useful primary-fire distances. Close weapons never inherit a
// rifle's detection radius merely because both weapons are in the inventory.
inline float bot_weapon_range(unsigned weapon){
    constexpr std::array<float,12> ranges{2.5f,100.f,45.f,15.f,100.f,100.f,35.f,10.f,100.f,100.f,100.f,0.f};
    return weapon<ranges.size()?ranges[weapon]:0.f;
}
inline bool bot_weapon_in_range(unsigned weapon,float distance){
    return std::isfinite(distance) && distance>=0.f && bot_weapon_range(weapon)>0.f && distance<=bot_weapon_range(weapon);
}
inline float bot_acquisition_range(const std::array<bool,12>& usable){
    float range=0.f;for(unsigned weapon=0;weapon<usable.size();++weapon)if(usable[weapon])range=std::max(range,bot_weapon_range(weapon));return range;
}
template<class Query> bool bot_visible_target(float distance,float range,Query&& clear_native_line){
    return std::isfinite(distance) && std::isfinite(range) && distance>=0.f && range>0.f && distance<=range && clear_native_line();
}
inline bool bot_continuous_weapon(unsigned weapon){return weapon==2 || weapon==4 || weapon==7;}
inline bool bot_firing_stance(unsigned weapon,unsigned decision){
    // PAL Minigun ChargeTime is .8 s. Seven 125 ms decisions barely cover
    // spin-up before aim settles; allow two seconds before the next strafe.
    return weapon==4?decision%20<16:decision%12<7;
}
// A held automatic trigger tolerates small tracking errors. Never carry this
// latch to another target/weapon or across a failed visibility/safety check.
inline bool bot_fire_aligned(unsigned weapon,float cosine,bool same_trigger,float distance=0.f){
    if(!std::isfinite(cosine) || !std::isfinite(distance) || distance<0.f)return false;
    float threshold=same_trigger && bot_continuous_weapon(weapon)?.99f:.996f;
    // A five-degree cone can miss a distant actor by several metres. Preserve
    // the close tracking latch but require the muzzle to settle at long range.
    if(distance>30.f){const float angular=.6f/distance;threshold=std::max(threshold,1.f-.5f*angular*angular);}
    return cosine>threshold;
}
// Consume a bounded angular remainder, rather than replaying the same mouse
// delta at every native frame. The total turn cannot exceed the requested turn.
inline int32_t consume_afk_turn(int32_t& remaining,float seconds){
    if(!std::isfinite(seconds) || seconds<=0)return 0;
    const float alpha=-std::expm1(-24.f*std::min(seconds,.25f));
    const int32_t step=int32_t(std::round(double(remaining)*alpha));
    remaining-=step;return step;
}
// Entries mean actual owned primary ammunition, not the all-weapons debug rule.
inline unsigned choose_afk_weapon(const std::array<bool,12>& usable,unsigned current,float distance,unsigned nearby,bool explosive_safe){
    if(!std::isfinite(distance) || distance<0)return 255;
    auto has=[&](unsigned weapon){return weapon<usable.size() && usable[weapon] && bot_weapon_in_range(weapon,distance);};
    auto safe=[&](unsigned weapon){return (weapon!=5 && weapon!=6 && weapon!=10) || (explosive_safe && distance>=12.f);};
    if(distance<2.5f && has(0))return 0;
    if(distance<8.f && has(7))return 7;
    if(distance<7.f && has(3))return 3;
    if(distance>25.f && has(8))return 8;
    if(nearby>=3 && distance>=12.f && explosive_safe && has(6))return 6;
    if(nearby>=3 && distance>=12.f && explosive_safe && has(5))return 5;
    if(has(4) && (nearby>=2 || distance>=22.f))return 4;
    if(distance<22.f && has(2))return 2;
    if(has(1))return 1;
    if(has(3))return 3;
    if(has(4))return 4;
    if(has(2))return 2;
    if(has(current) && safe(current))return current;
    for(unsigned weapon:{8u,9u,7u,6u,5u,10u})if(has(weapon) && safe(weapon))return weapon;
    return 255;
}
inline unsigned choose_game_bot_weapon(const std::array<bool,12>& usable,unsigned current,float distance,unsigned nearby,bool explosive_safe){
    if(!std::isfinite(distance) || distance<0)return 255;
    auto has=[&](unsigned weapon){return usable[weapon] && bot_weapon_in_range(weapon,distance);};
    if(distance<2.5f && has(0))return 0;
    if(has(7) && distance<(current==7?10.f:8.f))return 7;
    if(distance<(current==3?12.f:current==4?6.f:9.f) && has(3))return 3;
    if(current==4 && has(4) && distance>=6.f && distance<40.f)return 4;
    if(distance>25.f && has(8))return 8;
    if(explosive_safe && distance>=12.f && nearby>=3){if(has(6))return 6;if(has(5))return 5;}
    if(has(4) && distance<(current==4?40.f:35.f))return 4;
    if(has(2) && distance<25.f)return 2;
    if(has(9) && distance<25.f)return 9;
    if(has(8) && distance>12.f)return 8;
    if(has(1))return 1;
    return choose_afk_weapon(usable,current,distance,nearby,explosive_safe);
}
}
