#pragma once
#include "ssne/ppc_context.h"
#include "ssne/input.h"
#include "frontend_text.h"
#include "lan_discovery.h"
#include "multiplayer_options.h"
#include <functional>
#include <vector>
#include <string>
namespace port::coop {
// Result confirmation must never carry the gameplay Pause bit. Apply after
// device conversion so keyboard Enter and a controller's Start share policy.
inline ssne::PadState native_results_input(ssne::PadState pad,bool presenting){
    if(presenting && (pad.buttons&ssne::PAD_START))pad.buttons=uint16_t((pad.buttons&~ssne::PAD_START)|ssne::PAD_A);
    return pad;
}
struct NativeFrontendServices {
    std::string options_path;
    std::function<MultiplayerOptions()> shared_options;
    std::function<void(const MultiplayerOptions&)> set_rules;
    std::function<bool()> active,host,connecting,afk;
    std::function<void(bool)> set_afk;
    std::function<unsigned()> players;
    std::function<std::string(bool,const FrontendEndpoint&)> start;
    std::function<std::string(const FrontendRoom&)> join_lan;
    std::function<void()> stop,scan;
    std::function<std::vector<FrontendRoom>()> rooms;
    std::function<bool()> scanning;
    std::function<std::string()> status;
    std::function<unsigned(int,unsigned)> bot; // action: count=0, add=1, remove=-1
    std::function<void(PPCContext&)> initialized;
};
void configure_native_frontend(NativeFrontendServices);
// Guest frame thread only, just like session start and the native widgets.
MultiplayerOptions native_frontend_options();
void install_native_frontend();
void native_frontend_frame(PPCContext&);
void native_frontend_prepare_results_close(PPCContext&);
void native_frontend_tick(PPCContext&);
bool native_frontend_input(const ssne::PadState&);
bool native_frontend_text_input(const std::string&);
bool native_frontend_edit_key(unsigned key); // 1 accept, 2 cancel, 3 backspace, 4 delete
bool native_frontend_visible();
bool native_frontend_text_active();
// All pointer dispatch stays on the guest thread and preserves session ownership.
uint64_t native_frontend_mouse_epoch();
bool native_frontend_mouse_allowed(uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event);
bool native_frontend_mouse_event(PPCContext&,uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event);
unsigned native_menu_language();
void native_frontend_disconnected(std::string);
void native_frontend_return_main();
void native_frontend_engine_scope(bool);
std::vector<uint8_t> save_native_frontend_state();
bool restore_native_frontend_state(const std::vector<uint8_t>&,std::string&,bool apply);
}
