#pragma once
#include <array>
#include "authoritative_session.h"
#include "ssne/ppc_context.h"
#include "lobby_level_selection.h"
namespace port::coop {
inline ControlInput native_lobby_choice(const ssne::PadState& pad){
    ControlInput result;result.pad.buttons=pad.buttons&(ssne::PAD_LEFT|ssne::PAD_RIGHT|ssne::PAD_B);
    // PC arrows/A/D use the movement axis in the normal input settings.
    if(pad.stick_x<96)result.pad.buttons|=ssne::PAD_LEFT;
    if(pad.stick_x>160)result.pad.buttons|=ssne::PAD_RIGHT;
    return result;
}
bool native_lobby_visible();
// Guest-thread ownership of independent roster GUIs: -2 main, -1 unknown.
int native_lobby_mouse_owner(uint32_t gui);
uint32_t native_lobby_mouse_main();
std::array<uint32_t,8> native_lobby_mouse_guis();
bool attach_existing_native_lobby(PPCContext&);
void back_native_lobby(PPCContext&);
void show_native_lobby(PPCContext&);
void install_native_lobby(AuthoritativeServer*,AuthoritativeClient*);
void deactivate_native_lobby();
void suspend_native_lobby(PPCContext&);
void resume_native_lobby(PPCContext&);
bool apply_native_lobby_skin_choices(PPCContext&,std::string& error);
bool open_native_lobby_level_selection(PPCContext&,std::string& error);
std::optional<NativeLobbyLevel> take_native_lobby_level();
void cancel_native_lobby_level_selection();
uint16_t filter_native_lobby_confirmation(uint16_t buttons);
void native_lobby_character_input(PPCContext&,unsigned slot,uint16_t pressed);
void publish_native_lobby_characters(Properties&);
bool receive_native_lobby_characters(PPCContext&,const Properties&,std::string& error);
}
