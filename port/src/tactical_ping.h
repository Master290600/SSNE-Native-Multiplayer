#pragma once
#include "coop/tactical_ping.h"
#include "game_view_geometry.h"
#include "ssne/mods.h"
#include <optional>
#include <span>
namespace port {
// Guest-thread safe point only: the native geometry query executes original
// collision code. The supplied camera is a copied final local view.
std::optional<coop::TacticalPingRequest> resolve_tactical_ping(PPCContext&,const coop::WorldState&,coop::PlayerId,const ssne::ModViewCamera&);
// Presentation thread only; consumes immutable values and never reads RAM.
void draw_tactical_pings(std::span<const coop::TacticalPing>,const coop::WorldState*,const ssne::ModViewCamera&,const GameViewRect&,unsigned language);
}
