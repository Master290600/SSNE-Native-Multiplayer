#pragma once
#include "ssne/ppc_context.h"
#include "ssne/input.h"
#include <array>
#include <cstdint>
#include <string>
namespace ssne {
inline constexpr unsigned no_spectator_target=8;
// One guest-thread native-render lease. Begin AFTER client visual interpolation,
// end BEFORE that interpolation is restored, and drain issued GX work first.
// Host render-tail pipelining must be disabled while this lease is requested.
class NativeSpectatorView {
    struct Word {uint32_t address=0,original=0;};
    std::array<Word,40> words_{};
    unsigned count_=0,view_words_=0,camera_words_=0,body_words_=0,local_=no_spectator_target,target_=no_spectator_target;
    uint32_t local_actor_=0,target_actor_=0,camera_=0,proxy_=0,engine_=0,view_=0,hud_=0,hud_root_=0;
    std::array<uint32_t,4> handles_{};
    bool active_=false,extended_=false;
public:
    NativeSpectatorView()=default;
    NativeSpectatorView(const NativeSpectatorView&)=delete;
    NativeSpectatorView& operator=(const NativeSpectatorView&)=delete;
    ~NativeSpectatorView();
    bool begin(PPCContext&,unsigned local_slot,unsigned target_slot,std::string& error);
    bool end(std::string& error);
    bool active()const{return active_;}
    unsigned target()const{return target_;}
    uint32_t filter_render_mask(uint32_t mask)const;
};
// Call in hle_netplay_render immediately before ssne_call_original(80117434),
// AFTER ClientPresentationScope has invoked the render-begin observer. Removes
// only the local weapon/HUD cameras, retaining world and shared/pause GUIs.
uint32_t native_spectator_filter_render_mask(uint32_t mask);
class NativeSpectatorMenu {
    friend struct NativeSpectatorMenuAccess;
    uint64_t epoch_=0;
    uint32_t actor_=0,gui_=0;
    unsigned slot_=no_spectator_target;
    bool start_down_=false,owns_=false,closing_=false;
    bool binding_valid(const PPCContext&)const;
    bool close_owned(PPCContext&,std::string&);
public:
    NativeSpectatorMenu()=default;
    NativeSpectatorMenu(const NativeSpectatorMenu&)=delete;
    NativeSpectatorMenu& operator=(const NativeSpectatorMenu&)=delete;
    ~NativeSpectatorMenu(){reset();}
    // Call on EVERY local tick, before SpectatorController/menu-open selection.
    // Client Pause remains owned by the existing replica path. Host uses only
    // the local original GUI tail, with no shared-world pause/audio callbacks.
    bool update(PPCContext&,uint64_t epoch,unsigned slot,const PadState& raw,bool watching,bool& consume_start,std::string& error);
    void reset();
};
bool native_spectator_menu_open();
// Authority only, before deactivating a new watcher. Original routines release
// held Flag/Bomb and their native visual/HUD/world ownership. Rejection before
// invocation writes nothing. An original callback failure may have native side
// effects: return false MUST abort spectator deactivation; never clear fields
// manually. Caller CPU, guest stack and native owner are always preserved.
bool release_native_spectator_objectives(PPCContext&,uint32_t actor,std::string& error);
}
