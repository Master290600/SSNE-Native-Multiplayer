#pragma once

#include "authoritative_session.h"
#include "native_lifecycle.h"
#include "local_shot_feedback.h"
#include "ssne/runtime.h"
#include <deque>
#include <cstdlib>
#include <map>

namespace port::coop {

class LocalWeaponLoopFeedback {
    struct Preview {
        uint64_t press=0, origin=0;
        unsigned weapon=12,lane=0;
        double next_pulse=0,ended_at=0;
        bool audible=false,flash=false,confirmed=false;
    };
    struct CuePreview {
        uint64_t press=0,id=0;
        unsigned weapon=12,lane=0,sound=0;
        double ended_at=0;
        bool confirmed=false;
    };

    uint64_t epoch_=0,press_=0;
    PlayerId player_{};
    EntityId actor_=0;
    unsigned weapon_=12,attempted_=0,spin_attempted_=0;
    std::array<Preview,2> playing_{};
    std::deque<Preview> completed_;
    std::deque<CuePreview> cues_;
    // One prediction per native cue/hand/press. A snapshot can deliver the
    // actual voice before the newly selected weapon is locally presentable.
    std::map<std::pair<unsigned,unsigned>,uint64_t> received_cues_;

    void stop(PPCContext& context,Preview& preview,double now){
        if(!preview.press)return;
        if(preview.audible)ssne::stop_local_weapon_loop(context,player_.slot,preview.lane,preview.origin);
        preview.ended_at=now;
        if(completed_.size()>=64)completed_.pop_front();
        completed_.push_back(preview);
        preview={};
    }

    static void log_preview(const char* action,unsigned slot,const Preview& preview){
        ssne::log_message(std::string(action)+" slot="+std::to_string(slot)+
            " weapon="+std::to_string(preview.weapon)+" lane="+std::to_string(preview.lane)+
            " input="+std::to_string(preview.press)+" flash="+std::to_string(preview.flash)+
            " sound="+std::to_string(preview.audible)+" origin="+std::to_string(preview.origin));
    }

    void stop_cues(PPCContext& context,double now,unsigned lanes=3){
        for(auto& cue:cues_){
            if(cue.ended_at || !(lanes&(1u<<cue.lane)))continue;
            ssne::stop_local_weapon_cue(context,cue.id);
            cue.ended_at=now;
        }
    }

    bool start_cue(PPCContext& context,unsigned lane,unsigned sound,unsigned flags,
                   const std::array<float,3>& position){
        const auto received=received_cues_.find({sound,lane});
        if(received!=received_cues_.end() && received->second==press_){
            if(std::getenv("SSNE_COOP_TRACE_LOCAL_WEAPON"))ssne::log_message(
                "[coop-local-weapon-cue-already-received] weapon="+std::to_string(weapon_)+
                " lane="+std::to_string(lane)+" cue="+std::to_string(sound)+" input="+std::to_string(press_));
            return false;
        }
        if(cues_.size()>=64 || press_>=(uint64_t(1)<<52))return false;
        ssne::NativeWeaponShot shot;
        shot.sequence=(uint64_t(1)<<62)|(press_<<10)|(uint64_t(sound)<<1)|lane;
        shot.slot=player_.slot;
        shot.weapon=weapon_;
        shot.hand=lane;
        shot.cue_sound=sound;
        shot.cue_flags=flags;
        shot.position=position;
        shot.input_press=press_;
        std::string error;
        if(!ssne::play_native_weapon_shot(context,shot,true,error))return false;
        cues_.push_back({press_,shot.sequence,weapon_,lane,sound});
        ssne::log_message("[coop-local-weapon-cue-preview] weapon="+std::to_string(weapon_)+
            " lane="+std::to_string(lane)+" cue="+std::to_string(sound)+" input="+std::to_string(press_)+
            " sequence="+std::to_string(shot.sequence));
        return true;
    }

public:
    void reset(PPCContext& context){
        for(auto& preview:playing_){
            if(preview.audible)ssne::stop_local_weapon_loop(context,player_.slot,preview.lane,preview.origin);
        }
        stop_cues(context,1.);
        *this={};
    }

    bool update(PPCContext& context,double now,uint64_t epoch,PlayerId player,EntityId actor,
                uint32_t native_actor,const std::array<float,3>& position,const LocalControlSample& sample,
                unsigned weapon,unsigned ready_mask,bool can_present,
                const std::array<std::optional<NativeMinigunTiming>,2>& minigun,unsigned minigun_spooled,
                const std::function<bool(uint32_t,unsigned,unsigned,double)>& flash){
        if(epoch!=epoch_ || player!=player_ || actor!=actor_){
            reset(context);
            epoch_=epoch;
            player_=player;
            actor_=actor;
            press_=sample.fire_pressed;
            weapon_=weapon;
            attempted_=spin_attempted_=3;
        }
        while(!completed_.empty() && now-completed_.front().ended_at>=12.)completed_.pop_front();
        std::erase_if(cues_,[&](const CuePreview& cue){return cue.ended_at && now-cue.ended_at>=12.;});

        if(weapon_!=weapon){
            for(auto& preview:playing_)stop(context,preview,now);
            stop_cues(context,now);
            weapon_=weapon;
            attempted_=spin_attempted_=3;
        }
        const bool held=can_present && sample.epoch==epoch && sample.fire_pressed>sample.fire_released &&
            ((sample.input.pad.buttons&ssne::PAD_R) || sample.input.pad.trigger_r>=128);
        if(sample.fire_pressed!=press_){
            for(auto& preview:playing_)stop(context,preview,now);
            stop_cues(context,now);
            press_=sample.fire_pressed;
            attempted_=spin_attempted_=held?0:3;
        }
        if(!held){
            for(auto& preview:playing_)stop(context,preview,now);
            stop_cues(context,now);
            attempted_=spin_attempted_=3;
            return false;
        }
        if(weapon==0 || weapon==7 || weapon==9){
            const unsigned sound=weapon==0?432u:weapon==7?412u:420u;
            stop_cues(context,now,3u&~ready_mask);
            bool started=false;
            for(unsigned lane=0;lane<2;++lane){
                const unsigned bit=1u<<lane;
                if((attempted_&bit) || !(ready_mask&bit))continue;
                attempted_|=bit;
                started=start_cue(context,lane,sound,weapon==0?0:1,position)||started;
            }
            return started;
        }
        if(weapon!=2 && weapon!=4)return false;
        bool started=false;
        for(unsigned lane=0;lane<2;++lane){
            const unsigned bit=1u<<lane;
            auto& preview=playing_[lane];
            if(!(ready_mask&bit) || (weapon==4 && !minigun[lane])){
                stop(context,preview,now);
                stop_cues(context,now,bit);
                continue;
            }
            if(weapon==4 && !(spin_attempted_&bit)){
                spin_attempted_|=bit;
                started=start_cue(context,lane,409,1,position)||started;
            }
            const bool spooled=weapon==2 || (minigun_spooled&bit);
            const bool starting=!(attempted_&bit) && spooled;
            // An authoritative loop may already own the voice when a weapon is
            // equipped with fire held, or after a snapshot briefly clears
            // readiness. Its native first-person pulses still run locally.
            if(spooled && !preview.press){
                preview.press=press_;
                preview.origin=press_|(uint64_t(1)<<62);
                preview.weapon=weapon;
                preview.lane=lane;
                preview.next_pulse=now;
            }
            if(starting){
                attempted_|=bit;
                if(weapon==4)start_cue(context,lane,408,1,position);
                ssne::NativeWeaponLoop start;
                start.sequence=start.origin=preview.origin;
                start.slot=player.slot;
                start.lane=lane;
                start.weapon=weapon;
                start.active=true;
                start.position=position;
                start.input_press=press_;
                std::string error;
                preview.audible=ssne::start_local_weapon_loop(context,start,error);
                started=true;
            }
            if(!preview.press || now<preview.next_pulse)continue;
            // PAL clears the shot timer after firing. The Uzi's 1/8-second
            // interval therefore occupies eight native 60 Hz ticks.
            const double period=weapon==4?std::ceil(60./minigun[lane]->rounds_per_second)/60.:8./60.;
            const auto pulse_time=std::max(preview.next_pulse,now-1./60.);
            preview.flash=flash(native_actor,weapon,lane,pulse_time)||preview.flash;
            preview.next_pulse+=period;
            if(preview.next_pulse<=now)preview.next_pulse=now+period;
            if(starting)log_preview("[coop-local-weapon-loop-preview]",player.slot,preview);
        }
        return started;
    }

    bool confirm_cue(const ssne::NativeWeaponShot& shot,uint64_t epoch,PlayerId player,double now){
        if(epoch!=epoch_ || player!=player_ || shot.slot!=player.slot || !shot.input_press || !shot.cue_sound)return false;
        if(shot.hand<2 && (shot.cue_sound==432 || shot.cue_sound==412 || shot.cue_sound==420 ||
                          shot.cue_sound==409 || shot.cue_sound==408))
            received_cues_[{shot.cue_sound,shot.hand}]=shot.input_press;
        for(auto& cue:cues_){
            if(cue.press!=shot.input_press || cue.weapon!=shot.weapon || cue.lane!=shot.hand ||
               cue.sound!=shot.cue_sound || cue.confirmed || (cue.ended_at && now-cue.ended_at>=12.))continue;
            cue.confirmed=true;
            if(!cue.ended_at){
                if(!ssne::adopt_local_weapon_cue(cue.id,shot))return false;
                cue.id=shot.sequence;
            }
            ssne::log_message("[coop-local-weapon-cue-confirmed] weapon="+std::to_string(shot.weapon)+
                " lane="+std::to_string(shot.hand)+" cue="+std::to_string(shot.cue_sound)+
                " input="+std::to_string(shot.input_press)+" sequence="+std::to_string(shot.sequence));
            return true;
        }
        return false;
    }

    bool confirm(const ssne::NativeWeaponLoop& event,uint64_t epoch,PlayerId player,double now){
        if(epoch!=epoch_ || player!=player_ || event.slot!=player.slot || event.lane>=playing_.size() ||
           !event.input_press)return false;
        auto& active=playing_[event.lane];
        if(!event.active)return false;
        if(active.press==event.input_press && active.weapon==event.weapon && !active.confirmed){
            active.confirmed=true;
            if(active.audible && ssne::adopt_local_weapon_loop(active.origin,event)){
                active.origin=event.origin;
                return true;
            }
            return false;
        }
        for(auto& prior:completed_){
            if(prior.press!=event.input_press || prior.weapon!=event.weapon || prior.lane!=event.lane ||
               prior.confirmed || now-prior.ended_at>=12.)continue;
            prior.confirmed=true;
            if(prior.audible){
                log_preview("[coop-local-weapon-loop-confirmed-after-release]",player.slot,prior);
                return true;
            }
            return false;
        }
        if(event.weapon==weapon_ && event.input_press==press_)attempted_|=1u<<event.lane;
        return false;
    }

    unsigned preserved_flash_mask(uint64_t epoch,PlayerId player,unsigned weapon,double now,
                                  std::span<const ssne::NativeWeaponLoop> events={},uint64_t cursor=0)const{
        if(epoch!=epoch_ || player!=player_ || weapon!=weapon_)return 0;
        unsigned hands=0;
        for(const auto& preview:playing_)if(preview.flash)hands|=1u<<preview.lane;
        for(const auto& prior:completed_)
            if(prior.weapon==weapon && prior.flash && now-prior.ended_at<12.)hands|=1u<<prior.lane;
        for(const auto& event:events){
            if(event.sequence<=cursor || !event.active || event.weapon!=weapon ||
               event.slot!=player.slot || event.lane>=playing_.size())continue;
            const auto was_shown=[&](const Preview& preview){
                return preview.press && preview.press==event.input_press && preview.weapon==weapon &&
                       preview.lane==event.lane && preview.flash;
            };
            if(!was_shown(playing_[event.lane]) &&
               std::none_of(completed_.begin(),completed_.end(),was_shown))hands&=~(1u<<event.lane);
        }
        return hands;
    }

};
}
