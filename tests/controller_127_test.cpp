#define SDL_MAIN_HANDLED
#include "controller_input.h"
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main() try {
    SDL_SetMainReady();require(SDL_Init(SDL_INIT_GAMECONTROLLER)==0,SDL_GetError());
    SDL_VirtualJoystickDesc desc{};desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;desc.naxes=SDL_CONTROLLER_AXIS_MAX;desc.nbuttons=SDL_CONTROLLER_BUTTON_MAX;desc.name="SSNE isolated virtual controller";
    const int device=SDL_JoystickAttachVirtualEx(&desc);require(device>=0,SDL_GetError());
    auto* controller=SDL_GameControllerOpen(device);require(controller,"Cannot open the owned virtual controller");
    auto* joystick=SDL_GameControllerGetJoystick(controller);
    require(SDL_JoystickIsVirtual(device)==SDL_TRUE,"Test selected a real device");
    for(int axis=0;axis<SDL_CONTROLLER_AXIS_MAX;++axis)SDL_JoystickSetVirtualAxis(joystick,axis,axis>=SDL_CONTROLLER_AXIS_TRIGGERLEFT?-32768:0);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_A,1);SDL_JoystickUpdate();
    port::Settings settings;port::PlayerInput input;
    port::sample_controller(settings,controller,input,true,false);auto pad=port::map_player_input(settings,input);
    require(pad.trigger_l==255 && (pad.buttons&ssne::PAD_L),"Xbox A no longer jumps");
    input={};port::sample_controller(settings,controller,input,false,false);pad=port::map_player_input(settings,input);
    require((pad.buttons&ssne::PAD_A) && !pad.trigger_l,"Jump button does not select in native menus");
    input={};port::sample_controller(settings,controller,input,false,true);pad=port::map_player_input(settings,input);
    require(!(pad.buttons&ssne::PAD_A),"Held jump skips a newly opened menu/cutscene");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_A,0);
    SDL_JoystickSetVirtualAxis(joystick,SDL_CONTROLLER_AXIS_LEFTX,2000);SDL_JoystickUpdate();
    input={};port::sample_controller(settings,controller,input,true,false);require(input.lx==0,"Small stick drift bypasses deadzone");
    SDL_JoystickSetVirtualAxis(joystick,SDL_CONTROLLER_AXIS_LEFTX,24000);
    SDL_JoystickSetVirtualAxis(joystick,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,32767);SDL_JoystickUpdate();
    input={};port::sample_controller(settings,controller,input,true,false);pad=port::map_player_input(settings,input);
    require(pad.stick_x>200 && pad.trigger_r==255 && (pad.buttons&ssne::PAD_R),"Move/fire mapping is broken");
    settings.swap_sticks=true;input={};port::sample_controller(settings,controller,input,true,false);
    require(input.lx==0 && input.cx>.5f,"Swap sticks has no effect");
    settings.kbm_player=3;settings.pad_player[1]=4;settings.pad_player[2]=0;
    require(port::controller_player(settings,1,false)==3 && port::controller_player(settings,1,true)==2 && port::controller_player(settings,2,true)==-1,"Online controller routed to an absent local player");
    // Only this test-owned virtual device is sampled by the chat release gate.
    port::ControllerReleaseGate gate;
    require(gate.ready(controller),"An unused chat gate blocked normal controller input");
    SDL_JoystickSetVirtualAxis(joystick,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_A,1);SDL_JoystickUpdate();
    gate.suppress();require(!gate.ready(controller),"A button pressed in chat leaked after chat closed");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_A,0);SDL_JoystickUpdate();
    require(gate.ready(controller),"Releasing the consumed button did not resume controller input");
    for(auto trigger:{SDL_CONTROLLER_AXIS_TRIGGERLEFT,SDL_CONTROLLER_AXIS_TRIGGERRIGHT}){
        SDL_JoystickSetVirtualAxis(joystick,trigger,32767);SDL_JoystickUpdate();
        gate.suppress();require(!gate.ready(controller),"A trigger pressed in chat leaked after chat closed");
        SDL_JoystickSetVirtualAxis(joystick,trigger,-32768);SDL_JoystickUpdate();
        require(gate.ready(controller),"Releasing the consumed trigger did not resume controller input");
    }
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_B,1);SDL_JoystickUpdate();
    gate.suppress();require(!gate.ready(controller),"Consumed controller button was not gated");
    gate.clear();require(gate.ready(controller),"Explicit chat gate reset did not restore input");
    SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_B,0);SDL_JoystickUpdate();
    SDL_GameControllerClose(controller);SDL_JoystickDetachVirtual(device);SDL_Quit();
    std::cout<<"Owned virtual controller: move/fire, menu selection, held-input/chat release gates, deadzone, stick swap and online/offline routing passed. No video subsystem or physical input used.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';SDL_Quit();return 1;}
