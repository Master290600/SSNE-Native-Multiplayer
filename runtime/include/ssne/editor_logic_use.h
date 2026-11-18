#pragma once
#include <string>
namespace ssne {
// Installs the verified PAL post-Use adapter. Clients never execute actions.
bool install_editor_logic_use(std::string& error);
// Role changes are published at the same guest-frame boundary as the session.
// This also blocks the client bootstrap before replica simulation is queued.
void set_editor_logic_use_authoritative(bool enabled);
}
