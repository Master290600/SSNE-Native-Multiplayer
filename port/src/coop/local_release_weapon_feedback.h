#pragma once

#include "authoritative_session.h"
#include "local_shot_feedback.h"

namespace port::coop {

// Grenade and cannon fire on release or at their own native charge limit.
// Only preview an early, confirmed-ready press and release pair.
class LocalReleaseWeaponFeedback {
    uint64_t epoch_=0,seen_press_=0,press_=0,charge_sequence_=0;
    PlayerId player_{};
    EntityId actor_=0;
    unsigned weapon_=12;
    double pressed_at_=0.,released_at_=0.;
    bool armed_=false,awaiting_=false,sound_=false;

    static bool supported(unsigned weapon){return weapon==5 || weapon==10;}
    static unsigned release_cue(unsigned weapon){return weapon==10?423:0;}
    static unsigned release_flags(unsigned weapon){return weapon==10?0:1;}
    static uint64_t preview_sequence(uint64_t press){
        return (uint64_t(1)<<62)|(uint64_t(1)<<61)|((press&((uint64_t(1)<<60)-1))*2);
    }
public:
    bool update(PPCContext& context,double now,uint64_t epoch,PlayerId player,const EntityState& own,
                const LocalControlSample& sample,LocalWeaponStatus status,uint64_t ack,bool can_present){
        if(epoch!=epoch_ || player!=player_ || own.id!=actor_){
            *this={};
            epoch_=epoch;
            player_=player;
            actor_=own.id;
            seen_press_=sample.fire_pressed;
            return false;
        }
        if(awaiting_ && now-released_at_>=12.)awaiting_=false;
        if(!can_present || status.weapon!=weapon_)armed_=false;
        if(sample.fire_pressed>seen_press_){
            const auto previous=seen_press_;
            seen_press_=sample.fire_pressed;
            if(awaiting_ || !can_present || !supported(status.weapon) || !(status.ready&1) ||
               sample.fire_released>=sample.fire_pressed || ack<previous || ack<sample.fire_released)return false;
            weapon_=status.weapon;
            press_=sample.fire_pressed;
            pressed_at_=now;
            armed_=true;
            sound_=false;
            charge_sequence_=0;
        }
        if(!armed_ || sample.fire_released<=press_)return false;
        armed_=false;
        const auto held=now-pressed_at_;
        // Grenades need >33 ms of native charge before an early release.
        // A brief hold avoids the original auto-release/clip-complete paths.
        const double first=weapon_==5?.075:.04;
        const double last=weapon_==5?.22:.20;
        if(held<first || held>last || !(status.available&1) || (status.firing&1))return false;
        if(weapon_==10){
            ssne::stop_local_weapon_cue(context,local_weapon_shot_sequence(press_,0));
            if(charge_sequence_)ssne::stop_local_weapon_cue(context,charge_sequence_);
        }
        ssne::NativeWeaponShot shot;
        shot.slot=player.slot;
        shot.weapon=weapon_;
        shot.cue_sound=release_cue(weapon_);
        shot.cue_flags=release_flags(weapon_);
        shot.position=std::get<std::array<float,3>>(own.properties.at("position"));
        shot.sequence=preview_sequence(press_);
        shot.input_press=press_;
        shot.hand=0;
        std::string error;
        sound_=ssne::play_native_weapon_shot(context,shot,true,error);
        released_at_=now;
        awaiting_=true;
        return true;
    }

    bool confirm(const ssne::NativeWeaponShot& shot,uint64_t epoch,PlayerId player){
        if(epoch!=epoch_ || player!=player_ || !press_ || shot.slot!=player.slot ||
           shot.hand!=0 || shot.weapon!=weapon_ || shot.input_press!=press_)return false;
        if(weapon_==10 && shot.cue_sound==422 && shot.cue_flags==0){
            charge_sequence_=shot.sequence;
            return false;
        }
        if(shot.cue_sound!=release_cue(weapon_) || shot.cue_flags!=release_flags(weapon_))return false;
        // An automatic host release before the client lets go cancels the
        // preview. The next correctly tagged host release remains independent.
        armed_=false;
        if(!awaiting_)return false;
        awaiting_=false;
        if(sound_ && shot.cue_sound)ssne::adopt_local_weapon_cue(preview_sequence(press_),shot);
        return sound_;
    }

};
}
