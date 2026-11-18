#pragma once
#include "session_authority.h"
#include "ssne/input.h"
#include <optional>
#include <span>
namespace port::coop {
struct SpectatorTarget {
    PlayerId player;
    bool connected=false,ready=false,spectator=false;
    float health=0.f;
};
struct SpectatorDecision {
    bool watching=false;
    std::optional<PlayerId> target;
    ssne::PadState menu;
};
// Presentation selection only. The server owns watching/round permissions and
// native occupancy; this class never changes a participant or a game entity.
class SpectatorController {
    uint64_t epoch_=0;
    PlayerId self_{},target_{};
    uint16_t previous_buttons_=0;
    int previous_stick_=0;
    bool watching_=false;
public:
    void reset();
    SpectatorDecision update(uint64_t epoch,PlayerId self,std::span<const SpectatorTarget>,
        bool watching,const ssne::PadState& input,bool menu_open=false);
    std::optional<PlayerId> target()const{return target_.generation?std::optional(target_):std::nullopt;}
};
}
