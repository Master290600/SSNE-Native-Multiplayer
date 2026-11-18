#pragma once
#include "session_authority.h"
#include <span>

namespace port::coop {
enum class HitmarkerEvent {None,Hit,Kill};
struct HitmarkerReplica {
    PlayerId owner{};
    uint64_t epoch=0;
    int64_t hits=0,kills=0;

    HitmarkerEvent consume(uint64_t next_epoch,PlayerId own,std::span<const int64_t> counters) {
        if(!next_epoch || !own.generation || own.slot>=max_players || counters.size()!=3*max_players)return HitmarkerEvent::None;
        const auto base=3*own.slot;
        const auto next_hits=counters[base+1],next_kills=counters[base+2];
        if(counters[base]<=0 || uint64_t(counters[base])!=own.generation ||
           next_hits<0 || next_kills<0 || next_kills>next_hits)return HitmarkerEvent::None;
        // The host starts from zero for each level and each connection generation.
        // This includes a first shot made before the client's first sampled frame.
        if(next_epoch<epoch)return HitmarkerEvent::None;
        if(owner!=own || epoch!=next_epoch){owner=own;epoch=next_epoch;hits=kills=0;}
        if(next_hits<=hits || next_kills<kills || next_kills-kills>next_hits-hits)return HitmarkerEvent::None;
        const auto event=next_kills>kills?HitmarkerEvent::Kill:HitmarkerEvent::Hit;
        hits=next_hits;kills=next_kills;
        return event;
    }
};
}
