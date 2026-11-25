#pragma once

#include "local_shot_feedback.h"
#include "local_weapon_gait.h"
#include "native_lifecycle.h"
#include "native_local_weapon_clip.h"
#include "ssne/replica_simulation.h"
#include <algorithm>
#include <deque>

namespace port::coop {

inline std::optional<unsigned> local_weapon_mode(const EntityState& own,unsigned hand){
    const auto found=own.properties.find("weapon_modes");
    const auto* modes=found==own.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&found->second);
    if(!modes || modes->size()!=2 || hand>=2 || (*modes)[hand]<0 || (*modes)[hand]>=18)return {};
    return unsigned((*modes)[hand]);
}

inline bool local_discrete_weapon_flash(unsigned weapon){
    return weapon==1 || weapon==3 || weapon==8;
}

inline std::optional<NativeLocalWeaponClipEvent> native_confirmed_weapon_clip_event(
    const ssne::NativeWeaponShot& shot,unsigned equipped_hands=0){
    using Event=NativeLocalWeaponClipEvent;
    if(shot.weapon==1 && shot.cue_sound==416 && shot.cue_flags==1){
        if(equipped_hands==1 && shot.hand==0)return Event::reload_single_pistol;
        if(equipped_hands==3 && shot.hand<2)return Event::reload_dual_pistol;
        return {};
    }
    if(shot.weapon==5 && !shot.cue_sound)return Event::release;
    if(shot.weapon==10 && shot.cue_sound==423)return Event::release;
    const auto cue=local_weapon_cue(shot.weapon);
    if(!cue || shot.cue_sound!=cue->sound || shot.cue_flags!=cue->flags)return {};
    if(shot.weapon==1 && shot.last_round)return Event::last_round;
    return shot.weapon==10?Event::charge:Event::shot;
}

// Own first-person poses are played from the original clip on the client
// clock. Guard the late host firing pose separately from gameplay/HUD state.
class LocalNativeWeaponPresentation {
    using Event=NativeLocalWeaponClipEvent;
    struct Playing {
        ssne::NativeLocalWeaponClip clip;
        float playback;
        double began;

        float elapsed(double now) const {
            return std::max(0.f,float(now-began)*playback);
        }
    };
    struct RenderedKey {
        ssne::NativeLocalWeaponClip clip;
        uint32_t frame;
    };
    struct Reload {
        Event event;
        uint64_t input, fire_press, after_shot;
        double begin;
        bool started, confirmed;
    };
    struct Hand {
        std::optional<Playing> active;
        std::optional<Reload> reload;
        std::optional<RenderedKey> previous;
        std::vector<float> terminal;
        double touched=0.;
        uint64_t latest_press=0;
        uint64_t held_press=0;
        double held_since=0.;
        Event held_phase=Event::shot;
    };
    struct Receipt {
        unsigned weapon,hand;
        Event event;
        uint64_t press;
        double expires;
    };
    struct MinigunBarrel {
        double updated=0.;
        double charge_fraction=0.;
        double degrees=0.;
        bool spinning=false;
    };

    uint64_t epoch_=0;
    PlayerId owner_{};
    EntityId actor_=0;
    unsigned weapon_=12;
    uint64_t reload_seen_=0;
    LocalWeaponGait gait_;
    std::array<Hand,2> hands_{};
    std::array<MinigunBarrel,2> minigun_{};
    std::deque<Receipt> receipts_;

public:
    bool pump_finished(double now) const {
        const auto& active=hands_[0].active;
        return weapon_!=3 || !active || active->elapsed(now)>=active->clip.duration;
    }

    void bind(uint64_t epoch,PlayerId owner,EntityId actor,unsigned weapon,double now){
        if(epoch_!=epoch || owner_!=owner || actor_!=actor){
            *this={};
            epoch_=epoch;
            owner_=owner;
            actor_=actor;
        }
        if(weapon_!=weapon){
            hands_={};
            minigun_={};
            gait_={};
            weapon_=weapon;
        }
        while(!receipts_.empty() && receipts_.front().expires<=now)receipts_.pop_front();
    }

    bool spin_minigun(unsigned hand,bool held,double now,NativeMinigunTiming timing){
        auto& barrel=minigun_[hand];
        if(barrel.updated){
            // PAL integrates its angular velocity during charge, fire and
            // spin-down (8005746C, 800576B4 and 80057C5C).
            const double dt=std::clamp(now-barrel.updated,0.,.1);
            const double remaining=barrel.spinning?1.-barrel.charge_fraction:barrel.charge_fraction;
            const double ramp=std::min(dt,remaining*timing.charge);
            const double next=std::clamp(barrel.charge_fraction+(barrel.spinning?1.:-1.)*
                                         ramp/timing.charge,0.,1.);
            const double turns=(barrel.charge_fraction+next)*.5*ramp+next*(dt-ramp);
            barrel.degrees=std::fmod(barrel.degrees+turns*timing.rounds_per_second*60.,360.);
            barrel.charge_fraction=next;
        }
        barrel.spinning=held;
        barrel.updated=now;
        return barrel.charge_fraction>=1.;
    }

    bool start(uint32_t actor,const EntityState& own,unsigned weapon,unsigned hand,Event event,
               uint64_t press,double now,bool predicted){
        if(own.id!=actor_ || weapon!=weapon_ || hand>=hands_.size())return false;
        auto& track=hands_[hand];
        if((press && press<track.latest_press) || (predicted && (!press || receipts_.size()>=96)))return false;
        const auto mode=local_weapon_mode(own,hand);
        const auto spec=mode?native_local_weapon_clip_for_feedback(weapon,*mode,event):std::nullopt;
        auto clip=spec?ssne::native_local_weapon_clip(actor,weapon,hand,spec->logical):std::nullopt;
        if(!clip)return false;
        // The native weapon starts its new clip on the firing edge. Waiting
        // for the prior clip here puts the first-person pose behind its cues.
        if(track.previous){
            const auto& previous=track.previous->clip;
            if(clip->model==previous.model && clip->resource==previous.resource && clip->data==previous.data){
                clip->initial_animation=previous.animation;
                clip->initial_frame=track.previous->frame;
            }
        }
        track.active=Playing{*clip,spec->playback,now};
        if(event!=Event::reload_single_pistol && event!=Event::reload_dual_pistol)track.reload.reset();
        track.latest_press=std::max(track.latest_press,press);
        track.touched=now;
        if(predicted)receipts_.push_back({weapon,hand,event,press,now+12.});
        return true;
    }

    unsigned reloading_pistols(double now) const {
        unsigned blocked=0;
        for(unsigned hand=0;hand<2;++hand){
            const auto& track=hands_[hand];
            if(track.reload && (!track.reload->started ||
                    (track.active && track.active->elapsed(now)<track.active->clip.duration)))blocked|=1u<<hand;
        }
        return blocked;
    }

    bool predict_pistol_reload(uint32_t actor,const EntityState& own,unsigned hands,unsigned hand,
                               uint64_t input,uint64_t fire_press,uint64_t shot_floor,double now,bool after_shot){
        if(weapon_!=1 || !input || hand>=2 || !(hands&(1u<<hand)) || (reloading_pistols(now)&(1u<<hand)))return false;
        const auto event=hands==3?Event::reload_dual_pistol:Event::reload_single_pistol;
        if(!after_shot && !start(actor,own,1,hand,event,input,now,false))return false;
        // The final bullet selects clip 4. On the next PAL controller tick
        // the empty hand requests its regular reload, including while held.
        hands_[hand].reload=Reload{event,input,fire_press,shot_floor,now+(after_shot?.01:0.),!after_shot,false};
        return true;
    }

    unsigned manual_pistol_reload(uint32_t actor,const EntityState& own,LocalWeaponStatus weapon,
                                  LocalPistolState pistol,uint64_t input,uint64_t fire_press,
                                  uint64_t acknowledged,uint64_t shot_floor,double now){
        if(!input || input<=reload_seen_)return 0;
        reload_seen_=input;
        if(weapon.weapon!=1 || acknowledged>=input)return 0;
        unsigned started=0;
        for(unsigned hand=0;hand<2;++hand)if((pistol.reloadable&(1u<<hand)) &&
            predict_pistol_reload(actor,own,weapon.available,hand,input,fire_press,shot_floor,now,false))started|=1u<<hand;
        return started;
    }

    bool confirm_pistol_reload(unsigned hand,Event event,uint64_t fire_press,uint64_t sequence,uint64_t acknowledged){
        if(hand>=2)return false;
        auto& reload=hands_[hand].reload;
        if(!reload || reload->confirmed || reload->event!=event || sequence<=reload->after_shot ||
           fire_press!=reload->fire_press || acknowledged<reload->input)return false;
        reload->confirmed=true;
        return true;
    }

    bool acknowledge(unsigned weapon,unsigned hand,Event event,uint64_t press){
        if(!press)return false;
        const auto match=std::find_if(receipts_.begin(),receipts_.end(),[&](const Receipt& value){
            return value.weapon==weapon && value.hand==hand && value.event==event && value.press==press;
        });
        if(match==receipts_.end())return false;
        receipts_.erase(match);
        return true;
    }

    void automatic_pulse(uint32_t actor,const EntityState& own,unsigned weapon,unsigned hand,uint64_t press,double now){
        if((weapon!=2 && weapon!=4) || hand>=hands_.size() || !press)return;
        auto& track=hands_[hand];
        if(track.held_press!=press){
            track.held_press=press;
            track.held_since=now;
            track.held_phase=Event::shot;
        }
        // PAL's animation setter keeps a running repeat of the same clip.
        // Evaluate this against the local clock, independent of render order.
        if(track.active && track.active->elapsed(now)<track.active->clip.duration)return;
        auto event=Event::shot;
        if(weapon==2 && now-track.held_since>=2.-.000001){
            event=track.held_phase==Event::shot?Event::held_transition:Event::held;
        }
        if(start(actor,own,weapon,hand,event,press,now,false))track.held_phase=event;
    }

    void release_or_continue(uint32_t actor,const EntityState& own,unsigned weapon,unsigned held_hands,double now){
        if(weapon!=2 && weapon!=4 && weapon!=0 && weapon!=9)return;
        for(unsigned hand=0;hand<hands_.size();++hand){
            auto& track=hands_[hand];
            if(!track.held_press)continue;
            if(!(held_hands&(1u<<hand))){
                if(weapon==2 && track.held_phase==Event::held_transition && track.active)continue;
                if(weapon==0 || weapon==9 || track.held_phase==Event::held || track.held_phase==Event::held_transition)
                    start(actor,own,weapon,hand,Event::held_end,track.held_press,now,false);
                track.held_press=0;
            }else if((weapon==0 || weapon==9) && !track.active){
                start(actor,own,weapon,hand,Event::held,track.held_press,now,false);
            }
        }
    }

    void begin_sustained(uint32_t actor,const EntityState& own,unsigned weapon,unsigned hand,uint64_t press,double now){
        if((weapon!=0 && weapon!=9) || hand>=hands_.size() || !press || hands_[hand].held_press==press)return;
        if(start(actor,own,weapon,hand,Event::held_begin,press,now,false))hands_[hand].held_press=press;
    }

    void draw(PPCContext& context,uint32_t actor,const EntityState& latest,
              EntityState& visible,LocalWeaponStatus status,double now,std::optional<WeaponGaitInput> gait_input,
              LocalPistolState pistol={},uint64_t acknowledged=0){
        if(!actor || latest.id!=actor_ || visible.id!=actor_ || status.weapon!=weapon_)return;
        const auto selected=visible.properties.find("weapon_model_slot");
        const auto* slot=selected==visible.properties.end()?nullptr:std::get_if<int64_t>(&selected->second);
        if(!slot || *slot!=weapon_)return;
        PresentationMatrix camera;
        std::optional<std::array<float,2>> offset;
        if(gait_input && presentation_matrix(visible.properties,"camera_row",camera))
            offset=gait_.sample(now,*gait_input);
        else gait_={};
        for(unsigned hand=0;hand<hands_.size();++hand){
            if(!(status.available&(1u<<hand)))continue;
            if(weapon_==4 && minigun_[hand].updated)
                visible.properties["weapon_model"+std::to_string(hand)+"_local_spin"]=float(minigun_[hand].degrees);
            if(offset){
                const auto prefix="weapon_model"+std::to_string(hand)+"_row";
                PresentationMatrix root;
                if(presentation_matrix(visible.properties,prefix,root)){
                    LocalWeaponGait::apply(root,camera,*offset);
                    put_presentation_matrix(visible.properties,prefix,root);
                }
            }
            auto& track=hands_[hand];
            if(track.reload && !track.reload->started && now>=track.reload->begin){
                const auto pending=*track.reload;
                if(start(actor,latest,1,hand,pending.event,pending.input,pending.begin,false))track.reload->started=true;
                else track.reload.reset();
            }
            const auto key="weapon_model"+std::to_string(hand)+"_bone_matrices";
            const auto found=visible.properties.find(key);
            auto* output=found==visible.properties.end()?nullptr:std::get_if<std::vector<float>>(&found->second);
            if(!output)continue;
            if(track.active){
                const auto& current=*track.active;
                const auto age=current.elapsed(now);
                const auto duration=current.clip.duration;
                uint32_t frame=0;
                if(ssne::sample_native_local_weapon_clip(context,actor,weapon_,hand,current.clip,
                                                         std::min(age,duration),track.terminal,&frame) &&
                   output->size()==track.terminal.size()){
                    *output=track.terminal;
                    track.previous=RenderedKey{current.clip,frame};
                    track.touched=now;
                    if(age>=duration)track.active.reset();
                    continue;
                }
                // The draw hook can temporarily lack a usable guest stack.
                // Keep the local clock running and retry at the current time.
                if(weapon_==1 && (current.clip.logical==3 || current.clip.logical==5) && age<duration){
                    if(track.terminal.size()==output->size())*output=track.terminal;
                    continue;
                }
                track.active.reset();
                track.previous.reset();
                track.terminal.clear();
            }
            if(track.terminal.size()!=output->size() || now-track.touched>=12.){
                track.reload.reset();
                track.previous.reset();
                continue;
            }
            // After local completion the host can still be showing an earlier
            // frame of this same reload. Keep the final native pose through it.
            const bool wait_reload=track.reload && (acknowledged<track.reload->input ||
                                                    (pistol.reloading&(1u<<hand)));
            if(track.reload && track.reload->confirmed && !wait_reload)track.reload.reset();
            if(wait_reload || (status.firing&(1u<<hand)) || track.held_press){
                *output=track.terminal;
            }else{
                // Avoid sampling an older buffered firing pose once the host
                // has already reached a different, e.g. reload, native pose.
                const auto last=latest.properties.find(key);
                const auto* bones=last==latest.properties.end()?nullptr:std::get_if<std::vector<float>>(&last->second);
                if(bones && bones->size()==output->size())*output=*bones;
                track.previous.reset();
            }
        }
    }
};

}
