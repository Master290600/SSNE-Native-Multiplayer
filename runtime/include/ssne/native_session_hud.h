#pragma once
#include "ssne/ppc_context.h"
#include <string>
namespace ssne {
// Guest-thread-only, local presentation. Native Latin-1 text uses the game's
// existing HUD font and shared 2D camera; it never opens or focuses a menu.
void install_native_session_hud();
bool update_native_session_hud(PPCContext&,const std::string& title,const std::string& detail,bool shown);
void clear_native_session_hud(PPCContext&); // Hide and reuse the two native rows.
void end_native_session_hud(PPCContext&);   // Retire owned rows before scene teardown.
}
