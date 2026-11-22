#pragma once

#include "world_state.h"
#include "ssne/native_effect.h"
#include <algorithm>
#include <deque>
#include <optional>
#include <span>

namespace port::coop {

inline constexpr char local_weapon_key[]="Session.LocalWeapon";
inline constexpr char local_weapon_origin_key[]="Session.LocalWeaponOrigins";
inline constexpr char local_weapon_capacity_key[]="Session.LocalWeaponCapacity";
inline constexpr char local_weapon_last_round_key[]="Session.LocalWeaponLastRound";

struct LocalWeaponRepeat {
    unsigned rounds=0;
    double interval=0.;
    unsigned pistol_rearm=0;
};

inline LocalWeaponRepeat local_weapon_repeat(const Properties& mission,unsigned slot){
    const auto it=mission.find(local_weapon_capacity_key);
    const auto* values=it==mission.end()?nullptr:std::get_if<std::vector<int64_t>>(&it->second);
    if(!values || values->size()!=max_players || slot>=max_players)return {};
    const auto packed=(*values)[slot];
    // Single-hand capacity always carries a positive interval in the high
    // bits. For two Desert Hawks the low bits alone identify the hands
    // that PAL can fire on a fresh trigger edge.
    if(packed>=1 && packed<=3)return {0,0.,unsigned(packed)};
    if(packed<256 || packed>((10000<<8)|255))return {};
    return {unsigned(packed)&255,double(uint64_t(packed)>>8)*.001};
}

struct LocalWeaponStatus {
    unsigned weapon=12;
    unsigned available=0,ready=0,firing=0;

    int64_t encode() const {
        return weapon<12 && available?int64_t((weapon+1)|(available<<4)|(ready<<6)|(firing<<8)):0;
    }
    static LocalWeaponStatus decode(int64_t value){
        if(value<=0 || value>0x3ff)return {};
        const auto weapon=unsigned(value)&15,available=(unsigned(value)>>4)&3;
        const auto ready=(unsigned(value)>>6)&3,firing=(unsigned(value)>>8)&3;
        if(!weapon || weapon>12 || !available || ((ready|firing)&~available))return {};
        return {weapon-1,available,ready,firing};
    }
};

inline LocalWeaponStatus local_weapon_status(const Properties& mission,unsigned slot){
    const auto it=mission.find(local_weapon_key);
    const auto* values=it==mission.end()?nullptr:std::get_if<std::vector<int64_t>>(&it->second);
    return values && values->size()==max_players && slot<max_players?LocalWeaponStatus::decode((*values)[slot]):LocalWeaponStatus{};
}

struct LocalWeaponCue {
    unsigned sound=0;
    unsigned flags=1;
};

inline std::optional<LocalWeaponCue> local_weapon_cue(unsigned weapon){
    switch(weapon){
        case 1:
        case 3:
        case 6:return LocalWeaponCue{};
        case 8:return LocalWeaponCue{418,1};
        case 10:return LocalWeaponCue{422,0};
        case 11:return LocalWeaponCue{434,0};
        default:return {};
    }
}

struct LocalWeaponFlashLease {
    EntityId owner=0;
    unsigned weapon=12,hands=0;
};

inline uint64_t local_weapon_shot_sequence(uint64_t input,unsigned hand){
    return (uint64_t(1)<<62)|((input&((uint64_t(1)<<61)-1))*2)|hand;
}

class LocalShotFeedback {
    struct Preview {
        uint64_t press,floor;
        unsigned weapon,pending,confirmed=0,sound=0,flash=0;
        double began=0.;
        bool dual_pistol=false;
    };
    uint64_t epoch_=0,seen_press_=0;
    PlayerId player_{};
    EntityId character_=0;
    std::deque<Preview> previews_;
    std::array<std::array<uint64_t,2>,12> last_confirmed_press_{};
    bool dual_pistols_=false;
    unsigned last_dual_hand_=2;
    uint64_t dual_start_press_=0,last_dual_press_=0;

    static bool own_shot(const ssne::NativeWeaponShot& shot,PlayerId player) {
        const auto cue=local_weapon_cue(shot.weapon);
        return cue && shot.slot==player.slot &&
               shot.cue_sound==cue->sound && shot.cue_flags==cue->flags && shot.hand<2;
    }

    static std::optional<unsigned> matching_hand(const Preview& preview,const ssne::NativeWeaponShot& shot) {
        if(shot.sequence<=preview.floor || shot.input_press!=preview.press || shot.weapon!=preview.weapon)return {};
        if(preview.pending&(1u<<shot.hand))return shot.hand;
        // The two Desert Hawks share a trigger. PAL may have rearmed the
        // other hand since our snapshot. Consume just the one predicted shot.
        if(preview.dual_pistol && (preview.pending==1 || preview.pending==2))
            return preview.pending==1?0u:1u;
        return {};
    }

public:
    unsigned observe(uint64_t epoch,PlayerId player,EntityId character,uint64_t ack,uint64_t press,uint64_t release,
                     uint64_t shot_floor,LocalWeaponStatus status,double now,LocalWeaponRepeat repeat={},
                     bool require_ready_idle=false){
        const bool dual=status.weapon==1 && status.available==3;
        if(epoch_!=epoch || player_!=player || character_!=character){
            *this={};
            epoch_=epoch;
            player_=player;
            character_=character;
            seen_press_=press;
            dual_pistols_=dual;
            dual_start_press_=press;
            return 0;
        }
        if(dual!=dual_pistols_){
            dual_pistols_=dual;
            last_dual_hand_=2;
            dual_start_press_=press;
            last_dual_press_=0;
        }
        while(!previews_.empty() && now-previews_.front().began>=12.)previews_.pop_front();
        if(press<=seen_press_)return 0;
        const auto previous=seen_press_;
        seen_press_=press;
        if(!character || !local_weapon_cue(status.weapon) || previews_.size()>=64)return 0;
        if(dual && (ack>=press || release>=press || last_confirmed_press_[1][0]==press ||
                    last_confirmed_press_[1][1]==press))return 0;
        const bool pending=std::any_of(previews_.begin(),previews_.end(),[&](const Preview& preview){
            return preview.weapon==status.weapon && preview.pending;
        });
        unsigned hands=0;
        if(!pending && ack>=previous && (release>=press || ack>=release))hands=status.ready;
        if(dual && !hands && !pending && previous && last_dual_press_==previous &&
           ack>=previous && release>previous && release<press && last_dual_hand_<2){
            // The previous shot already happened. Its release need not have
            // reached this snapshot: PAL resets the two fire timers on a new
            // trigger, but still checks the opposite hand's native lock/clip.
            hands=repeat.pistol_rearm&(1u<<(1-last_dual_hand_));
        }
        // A quickly re-pressed rocket launcher can still be cooling down from
        // an automatic shot on the previous held input. Only its acknowledged
        // idle hand can promise a fresh shot; otherwise use the actual event.
        if(require_ready_idle){
            if(release>=press || ack<release || ack>=press)return 0;
            hands&=~status.firing;
        }
        if(!require_ready_idle && !hands && status.available==1 && (status.ready || status.firing) && repeat.rounds &&
           !previews_.empty() && release<press){
            const auto& last=previews_.back();
            const auto outstanding=std::count_if(previews_.begin(),previews_.end(),[&](const Preview& value){
                return value.weapon==status.weapon && value.pending;
            });
            // The PAL pistol's trigger release rearms its native timer.
            // Keep the held-fire cadence for other stock single-hand weapons.
            const bool pistol_rearmed=status.weapon==1 && release>last.press;
            if(last.weapon==status.weapon && (pistol_rearmed || now-last.began>=repeat.interval) &&
               outstanding+1<repeat.rounds)
                hands=1;
        }
        for(unsigned hand=0;hand<2;++hand)
            if(last_confirmed_press_[status.weapon][hand]==press)hands&=~(1u<<hand);
        if(!hands)return 0;
        if(dual && (hands&1))hands=1; // PAL updates the right hand first.
        previews_.push_back({press,shot_floor,status.weapon,hands,0,0,0,now,dual});
        return hands;
    }

    unsigned visual_hands(unsigned hands)const{
        // The idle right hand has priority. Once the two pistols alternate,
        // only predict a side when the ready snapshot agrees with that order.
        if(!dual_pistols_)return hands;
        return hands&(1u<<(last_dual_hand_==0?1:0));
    }

    void set_played(unsigned sound,unsigned flash){
        previews_.back().sound=sound;
        previews_.back().flash=flash;
    }

    bool confirm(const ssne::NativeWeaponShot& shot,uint64_t epoch,PlayerId player,bool* local_flash=nullptr,
                 unsigned* predicted_hand=nullptr){
        if(epoch!=epoch_ || player!=player_ || !own_shot(shot,player))return false;
        last_confirmed_press_[shot.weapon][shot.hand]=shot.input_press;
        if(shot.weapon==1 && dual_pistols_){
            last_dual_hand_=shot.hand;
            if(shot.input_press>dual_start_press_)last_dual_press_=shot.input_press;
        }
        const unsigned bit=1u<<shot.hand;
        for(auto& preview:previews_){
            if(preview.dual_pistol && shot.weapon==1 && shot.sequence>preview.floor && shot.input_press>preview.press)
                preview.pending=0;
            const auto hand=matching_hand(preview,shot);
            if(!hand)continue;
            const unsigned local_bit=1u<<*hand;
            preview.pending&=~local_bit;
            preview.confirmed|=local_bit;
            if(predicted_hand)*predicted_hand=*hand;
            if(local_flash)*local_flash=(preview.flash&local_bit)!=0;
            return (preview.sound&local_bit)!=0;
        }
        for(auto& preview:previews_)
            if(preview.weapon==shot.weapon && shot.sequence>preview.floor)preview.confirmed&=~bit;
        return false;
    }

    LocalWeaponFlashLease preserved_flash(uint64_t epoch,PlayerId player,
                                         std::span<const ssne::NativeWeaponShot> shots,uint64_t cursor,
                                         LocalWeaponStatus status,double now) const {
        if(epoch!=epoch_ || player!=player_)return {};
        unsigned hands=0;
        for(const auto& preview:previews_)
            if(preview.weapon==status.weapon && now-preview.began<12.)
                hands|=preview.flash&(preview.pending|preview.confirmed)&status.available;
        for(const auto& shot:shots){
            if(shot.sequence<=cursor || shot.weapon!=status.weapon || !own_shot(shot,player))continue;
            if(std::none_of(previews_.begin(),previews_.end(),[&](const Preview& p){return matching_hand(p,shot).has_value();}))
                hands&=~(1u<<shot.hand);
        }
        return {hands?character_:0,status.weapon,hands};
    }

};
}
