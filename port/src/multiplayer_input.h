#pragma once
#include "settings.h"
#include <array>
#include <cstdint>

namespace port {
// Physical releases must be recognized even after modifier keys change.
inline uint8_t multiplayer_event_slots(const Settings& settings,int action,const SDL_Event& event,bool pressed){
    if(action<0 || action>=MP_COUNT)return 0;
    Bind physical;
    if(event.type==(pressed?SDL_KEYDOWN:SDL_KEYUP)){
        if(pressed && (event.key.repeat || (event.key.keysym.mod&(KMOD_CTRL|KMOD_ALT|KMOD_GUI))))return 0;
        physical=Bind::key(event.key.keysym.scancode);
    }else if(event.type==(pressed?SDL_MOUSEBUTTONDOWN:SDL_MOUSEBUTTONUP))physical=Bind::mouse(event.button.button);
    else return 0;
    uint8_t slots=0;
    for(unsigned i=0;i<Settings::kMpSlots;++i)
        if(physical.type!=Bind::NONE && settings.multiplayer[action][i]==physical)slots|=uint8_t(1u<<i);
    return slots;
}
class MultiplayerHold {
    uint8_t held_=0;
public:
    bool update(const Settings& settings,int action,const SDL_Event& event){
        const auto pressed=multiplayer_event_slots(settings,action,event,true);
        held_&=uint8_t(~multiplayer_event_slots(settings,action,event,false));held_|=pressed;
        return pressed!=0;
    }
    void clear(){held_=0;}
    bool active()const{return held_!=0;}
};
inline std::string multiplayer_binding_label(const Settings& settings,int action){
    std::string result;
    if(action>=0 && action<MP_COUNT)for(const auto& bind:settings.multiplayer[action])if(bind.type!=Bind::NONE){
        if(!result.empty())result+=" / ";result+=bind_name(bind);
    }
    return result.empty()?"--":result;
}
}
