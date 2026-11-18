#include "../port/src/coop/afk_combat.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace port::coop;
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void perception_and_fire_are_separate(){
    std::array<bool,12> usable{};usable[3]=true;
    check(bot_acquisition_range(usable)==15.f && bot_perception_range(usable)>=160.f,"Short loadout lost independent perception");
    unsigned queries=0;
    const auto clear=[&]{++queries;return true;};
    const auto wall=[&]{++queries;return false;};
    check(bot_visible_target(150,bot_perception_range(usable),clear),"Visible distant enemy not perceived with short weapon");
    check(queries==1 && choose_afk_weapon(usable,3,150,4,true)==255 && choose_game_bot_weapon(usable,3,150,4,true)==255,"Perception authorized distant shotgun fire");
    check(!bot_visible_target(150,bot_perception_range(usable),wall) && queries==2,"Perception ignored native wall query");
    check(choose_afk_weapon(usable,3,12,1,true)==3,"Close weapon became unusable after perception separation");
    const auto before=queries;
    check(!bot_visible_target(161,160,clear) && !bot_visible_target(-1,160,clear) &&
          !bot_visible_target(std::numeric_limits<float>::quiet_NaN(),160,clear) &&
          !bot_visible_target(150,std::numeric_limits<float>::infinity(),clear) && queries==before,"Invalid/far inputs performed a geometry query");
}
void usable_inventory_limits_fire(){
    std::array<bool,12> usable{};
    check(bot_perception_range(usable)==0.f && choose_afk_weapon(usable,1,20,1,true)==255,"Empty inventory acquired or fired");
    usable[1]=true;
    check(bot_perception_range(usable)==160.f && bot_weapon_in_range(1,120) && !bot_weapon_in_range(1,120.01f),"Pistol useful range incorrect");
    check(choose_afk_weapon(usable,1,110,1,true)==1 && choose_afk_weapon(usable,1,150,1,true)==255,"Visible distant target bypassed pistol range");
    usable[8]=true;
    check(bot_perception_range(usable)==250.f && choose_afk_weapon(usable,1,220,1,true)==8 &&
          choose_game_bot_weapon(usable,1,220,1,true)==8,"Owned long-range rifle was not selected");
    check(bot_weapon_in_range(8,250) && !bot_weapon_in_range(8,250.01f),"Sniper useful range boundary incorrect");
    usable[8]=false; // Missing ownership or exhausted primary ammo.
    check(bot_perception_range(usable)==160.f && choose_afk_weapon(usable,8,220,1,true)==255,"Unavailable rifle retained range or selection");
    usable={};usable[4]=true;
    check(bot_weapon_in_range(4,150) && !bot_weapon_in_range(4,150.01f) && choose_afk_weapon(usable,4,140,1,true)==4,"Minigun useful range incorrect");
    usable={};usable[2]=true;
    check(bot_weapon_in_range(2,60) && !bot_weapon_in_range(2,60.01f),"Uzi range was unbounded");
    usable={};usable[9]=true;
    check(bot_weapon_in_range(9,150) && !bot_weapon_in_range(9,150.01f),"Primary beam range was unbounded");
    check(!bot_fire_aligned(8,.9999f,false,220) && bot_fire_aligned(8,1.f,false,220),"Long-range muzzle alignment was relaxed");
    check(!bot_fire_aligned(4,.9999f,true,140),"Continuous trigger bypassed long-range muzzle alignment");
}
void close_and_ballistic_weapons_stay_bounded(){
    check(bot_weapon_range(0)==2.5f && bot_weapon_range(3)==15.f && bot_weapon_range(7)==10.f,"Close weapon limits expanded with sight");
    check(bot_weapon_range(5)==100.f && bot_weapon_range(6)==35.f && bot_weapon_range(10)==100.f,"Ballistic limits expanded without prediction");
    std::array<bool,12> usable{};usable[6]=true;
    check(bot_perception_range(usable)==160.f && choose_afk_weapon(usable,6,80,4,true)==255 &&
          choose_afk_weapon(usable,6,20,4,true)==6,"Grenade perception bypassed ballistic fire range");
    usable={};usable[5]=true;
    check(choose_afk_weapon(usable,5,80,4,false)==255 && choose_afk_weapon(usable,5,80,4,true)==5,"Explosive teammate safety changed");
    check(!bot_weapon_in_range(12,1) && !bot_weapon_in_range(1,-1) && !bot_weapon_in_range(1,std::numeric_limits<float>::quiet_NaN()),"Invalid weapon/distance accepted");
}
}
int main()try{perception_and_fire_are_separate();usable_inventory_limits_fire();close_and_ballistic_weapons_stay_bounded();std::cout<<"AFK perception/fire separation, owned ammunition, LOS, long-range alignment and bounded ballistics passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
