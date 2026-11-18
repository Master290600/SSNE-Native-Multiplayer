#define SDL_MAIN_HANDLED
#include "multiplayer_input.h"
#include <iostream>
#include <stdexcept>
namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
SDL_Event key(SDL_Scancode code,bool down=true,bool repeat=false,SDL_Keymod mod=KMOD_NONE){SDL_Event e{};e.type=down?SDL_KEYDOWN:SDL_KEYUP;e.key.keysym.scancode=code;e.key.repeat=repeat;e.key.keysym.mod=mod;return e;}
SDL_Event mouse(unsigned button,bool down=true){SDL_Event e{};e.type=down?SDL_MOUSEBUTTONDOWN:SDL_MOUSEBUTTONUP;e.button.button=uint8_t(button);return e;}
}
int main()try{
    port::Settings settings;
    settings.multiplayer[port::MP_CHAT][0]=port::Bind::key(SDL_SCANCODE_C);
    check(!port::multiplayer_event_slots(settings,port::MP_CHAT,key(SDL_SCANCODE_T),true),"Old chat key remained active");
    check(port::multiplayer_event_slots(settings,port::MP_CHAT,key(SDL_SCANCODE_C),true)==1,"Custom chat key was ignored");
    check(!port::multiplayer_event_slots(settings,port::MP_CHAT,key(SDL_SCANCODE_C,true,true),true),"Repeat retriggered an action");
    check(!port::multiplayer_event_slots(settings,port::MP_CHAT,key(SDL_SCANCODE_C,true,false,KMOD_CTRL),true),"Modified shortcut triggered chat");
    check(port::multiplayer_event_slots(settings,port::MP_CHAT,key(SDL_SCANCODE_C,false,false,KMOD_CTRL),false)==1,"Changed modifiers swallowed release");
    settings.multiplayer[port::MP_ROSTER][0]=port::Bind::key(SDL_SCANCODE_B);
    settings.multiplayer[port::MP_ROSTER][1]=port::Bind::mouse(SDL_BUTTON_X1);
    port::MultiplayerHold held;
    check(held.update(settings,port::MP_ROSTER,key(SDL_SCANCODE_B)) && held.active(),"Roster key did not hold");
    check(held.update(settings,port::MP_ROSTER,mouse(SDL_BUTTON_X1)) && held.active(),"Roster mouse alternative did not hold");
    held.update(settings,port::MP_ROSTER,key(SDL_SCANCODE_B,false));check(held.active(),"Releasing one alternative cleared the other held input");
    held.update(settings,port::MP_ROSTER,mouse(SDL_BUTTON_X1,false));check(!held.active(),"Mouse release left roster stuck");
    held.update(settings,port::MP_ROSTER,key(SDL_SCANCODE_B));held.clear();check(!held.active(),"Focus/menu reset left a held roster");
    check(!port::multiplayer_event_slots(settings,-1,key(SDL_SCANCODE_B),true),"Invalid action indexed bindings");
    settings.multiplayer[port::MP_AFK][0]=port::Bind::mouse(SDL_BUTTON_X2);
    check(port::multiplayer_event_slots(settings,port::MP_AFK,mouse(SDL_BUTTON_X2),true)==1,"AFK mouse binding ignored");
    std::cout<<"Custom multiplayer keys/mouse, repeats/modifiers, two-slot holds/releases and cancellation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
