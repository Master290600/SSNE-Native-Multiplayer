#pragma once
#include "world_state.h"
#include "ssne/native_boss_hud.h"
#include <cmath>
namespace port::coop {
inline bool has_native_boss_hud_mission(const Properties& mission){
    for(const auto& [key,value]:mission)if(key.starts_with("Session.BossHud."))return true;
    return false;
}
inline bool read_native_boss_hud_mission(const Properties& mission,ssne::NativeBossHudState& out,std::string& error){
    for(const auto& [key,value]:mission)if(key.starts_with("Session.BossHud.") &&
       key!="Session.BossHud.Present" && key!="Session.BossHud.Active" && key!="Session.BossHud.Suppressed" && key!="Session.BossHud.Ratio"){
        error="Unknown authoritative boss HUD property";return false;
    }
    auto integer=[&](const char* suffix)->const int64_t*{
        const auto item=mission.find(std::string("Session.BossHud.")+suffix);
        return item==mission.end()?nullptr:std::get_if<int64_t>(&item->second);
    };
    const auto present=integer("Present"),active=integer("Active"),suppressed=integer("Suppressed");
    const auto ratio_item=mission.find("Session.BossHud.Ratio");
    const auto ratio=ratio_item==mission.end()?nullptr:std::get_if<float>(&ratio_item->second);
    if(!present || !active || !suppressed || !ratio || *present<0 || *present>1 || *active<0 || *active>1 ||
       *suppressed<0 || *suppressed>1 || !std::isfinite(*ratio) || *ratio<0 || *ratio>1 ||
       (!*present && (*active || *suppressed || *ratio!=1.f))){error="Invalid authoritative boss HUD metadata";return false;}
    out={bool(*present),bool(*active),bool(*suppressed),*ratio};return true;
}
inline void publish_native_boss_hud_mission(Properties& mission,const ssne::NativeBossHudState& state){
    mission["Session.BossHud.Present"]=int64_t(state.present);
    mission["Session.BossHud.Active"]=int64_t(state.active);
    mission["Session.BossHud.Suppressed"]=int64_t(state.suppressed);
    mission["Session.BossHud.Ratio"]=state.ratio;
}
inline bool apply_native_boss_hud_mission(PPCContext& context,uint64_t epoch,const Properties& mission,std::string& error){
    // Generic mission properties are additive within protocol67. An older
    // host publishes none of these fields; retain its previous presentation.
    if(!has_native_boss_hud_mission(mission))return true;
    ssne::NativeBossHudState desired;
    return read_native_boss_hud_mission(mission,desired,error) && ssne::apply_native_boss_hud(context,epoch,desired,error);
}
}
