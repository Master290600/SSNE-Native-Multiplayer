#include "spectator.h"
#include <array>
#include <cmath>
namespace port::coop {
void SpectatorController::reset(){*this={};}
SpectatorDecision SpectatorController::update(uint64_t epoch,PlayerId self,std::span<const SpectatorTarget> candidates,
        bool watching,const ssne::PadState& input,bool menu_open){
    SpectatorDecision result;result.watching=watching;
    if(!watching){reset();result.menu=input;return result;}
    constexpr uint16_t menu_buttons=ssne::PAD_START|ssne::PAD_A|ssne::PAD_B|ssne::PAD_X|ssne::PAD_Y|ssne::PAD_Z|ssne::PAD_LEFT|ssne::PAD_RIGHT|ssne::PAD_UP|ssne::PAD_DOWN;
    result.menu.buttons=input.buttons&(menu_open?menu_buttons:ssne::PAD_START);
    if(menu_open){result.menu.stick_x=input.stick_x;result.menu.stick_y=input.stick_y;}
    const int stick=input.stick_x<56?-1:input.stick_x>200?1:0;
    const bool fresh=!watching_ || epoch_!=epoch || self_!=self;
    if(fresh){epoch_=epoch;self_=self;target_={};previous_buttons_=input.buttons;previous_stick_=stick;}
    watching_=true;
    if(!epoch || self.slot>=max_players || !self.generation){target_={};return result;}
    std::array<std::optional<PlayerId>,max_players> eligible{};
    std::array<unsigned,max_players> count{};
    for(const auto& candidate:candidates){
        const auto id=candidate.player;
        if(id.slot>=max_players || !id.generation)continue;
        ++count[id.slot];
        if(candidate.connected && candidate.ready && !candidate.spectator && id!=self && std::isfinite(candidate.health) && candidate.health>0.f)eligible[id.slot]=id;
    }
    // A malformed duplicate slot is ambiguous, including two generations.
    for(unsigned i=0;i<max_players;++i)if(count[i]!=1)eligible[i].reset();
    if(target_.slot>=max_players || !eligible[target_.slot] || *eligible[target_.slot]!=target_)target_={};
    if(!target_.generation)for(const auto& id:eligible)if(id){target_=*id;break;}
    const uint16_t pressed=input.buttons&~previous_buttons_;previous_buttons_=input.buttons;
    int direction=0;
    if(!fresh && !menu_open){
        const bool left=(pressed&ssne::PAD_LEFT) || (stick<0 && previous_stick_>=0);
        const bool right=(pressed&ssne::PAD_RIGHT) || (stick>0 && previous_stick_<=0);
        direction=left==right?0:left?-1:1;
    }
    previous_stick_=stick;
    if(direction && target_.generation)for(unsigned offset=1;offset<=max_players;++offset){
        const unsigned slot=unsigned(int(target_.slot)+int(max_players)+direction*int(offset))%max_players;
        if(eligible[slot]){target_=*eligible[slot];break;}
    }
    result.target=target();return result;
}
}
