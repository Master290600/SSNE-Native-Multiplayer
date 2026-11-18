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
enum class NativeInvitationCopyState {Idle,Waiting,Copied,Failed};
struct NativeInvitationStatus {
    unsigned phase=0,route=0,peers=0;
    std::string code;
    NativeInvitationCopyState copy_state=NativeInvitationCopyState::Idle;
    uint64_t copy_sequence=0;
};
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
    std::function<bool()> spectating,spectator_available,spectator_locked;
    std::function<void(bool)> set_spectating;
    std::function<unsigned()> players;
    std::function<std::string(bool,const FrontendEndpoint&)> start;
    std::function<std::string(const FrontendRoom&)> join_lan;
    std::function<void()> stop,scan;
    std::function<std::vector<FrontendRoom>()> rooms;
    std::function<bool()> scanning;
    std::function<std::string()> status;
    std::function<NativeInvitationStatus()> invitation;
    // Guest-thread only: an Internet host can share its invitation before the
    // explicit transition into the original game setup. Continuing is idempotent.
    std::function<bool()> invitation_host_pending;
    std::function<std::string()> continue_invitation_host;
    std::function<void(const std::string&)> copy_invitation;
    // Invites through the existing host server, without changing native setup.
    // Status reports Copied only after the UI thread acknowledges the clipboard.
    std::function<void()> copy_lobby_invitation;
    std::function<unsigned(int,unsigned)> bot; // action: count=0, add=1, remove=-1
    std::function<void(PPCContext&)> initialized;
    std::function<void()> request_quit;
};
// Set once after loading host settings and before starting the guest thread.
// Later F12 changes are saved for the next launch; this override stays fixed.
void configure_native_startup_language(int);
// Open the original profile chooser for an explicit project start.
bool native_frontend_start_profile(PPCContext&);
// Observes completed native frontend construction, including nested Opens.
// Safe during loading SDKFlip callbacks; never opens or changes any GUI.
bool native_frontend_startup_ready();
void configure_native_frontend(NativeFrontendServices);
// Guest frame thread only, just like session start and the native widgets.
MultiplayerOptions native_frontend_options();
void install_native_frontend();
void native_frontend_frame(PPCContext&);
void native_frontend_prepare_results_close(PPCContext&);
void native_frontend_tick(PPCContext&);
void native_frontend_lobby_ready();
void native_frontend_lobby_map();
bool native_frontend_input(const ssne::PadState&);
bool native_frontend_text_input(const std::string&);
bool native_frontend_edit_key(unsigned key); // 1 accept, 2 cancel, 3 backspace, 4 delete
bool native_frontend_visible();
bool native_frontend_text_active();
// Guest-thread scheduling query; observes pending menu work without consuming
// it. Ordinary gameplay buttons do not make the frame pipeline ineligible.
bool native_frontend_pending_actions();
// All pointer dispatch stays on the guest thread and preserves session ownership.
uint64_t native_frontend_mouse_epoch();
bool native_frontend_mouse_allowed(uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event);
bool native_frontend_mouse_event(PPCContext&,uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event);
unsigned native_menu_language();
void native_frontend_disconnected(std::string);
// Safe from a NativeSession load callback: only queues the failure. The next
// guest frontend frame leaves the session and shows its specific message.
void native_frontend_custom_level_error(std::string);
// Transfer workers publish snapshots only; the guest owns every native GUI
// operation. None resets a previous join/cancel. Ready is acknowledged as None
// by the guest frame after the original Connecting/pause GUI has been closed;
// only then may the session queue the next native level.
enum class NativeCustomMapPhase {None,Requesting,Downloading,Verifying,Installing,Ready};
struct NativeCustomMapProgress {
    NativeCustomMapPhase phase=NativeCustomMapPhase::None;
    std::string title,alias,preview_path;
    uint64_t received=0,total=0;
};
void native_frontend_custom_map_progress(NativeCustomMapProgress);
NativeCustomMapProgress native_frontend_custom_map_progress();
bool native_frontend_take_custom_map_cancel();
void native_frontend_return_main();
void native_frontend_engine_scope(bool);
std::vector<uint8_t> save_native_frontend_state();
bool restore_native_frontend_state(const std::vector<uint8_t>&,std::string&,bool apply);
}
