#pragma once
#include "coop/authoritative_session.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace port {
struct RosterHealth {coop::PlayerId player{};float health=0;bool valid=false;};
enum class RosterStatus {Loading,Playing,Dead,Afk,Spectator};
struct RosterRow {
    coop::PlayerId player{};
    std::string name;
    RosterStatus status=RosterStatus::Loading;
    bool host=false,bot=false,health_known=false,ping_known=false;
    float health=0;
    unsigned ping_ms=0;
};
// Presentation only. A reused slot cannot inherit another connection's health
// or latency; unknown data remains unknown instead of displaying a false zero.
inline std::vector<RosterRow> multiplayer_roster(
    const std::array<coop::Participant,coop::max_players>& participants,
    const std::array<coop::PlayerLatency,coop::max_players>& latencies,
    const std::array<RosterHealth,coop::max_players>& health,uint8_t bots) {
    std::vector<RosterRow> rows;
    for(unsigned slot=0;slot<participants.size();++slot){
        const auto& p=participants[slot];
        if(!p.connected || p.id.slot!=slot || !p.id.generation)continue;
        RosterRow row;row.player=p.id;row.name=p.name;row.host=slot==0;row.bot=bots&(1u<<slot);
        const auto& hp=health[slot];
        row.health_known=p.ready && !p.spectator && hp.valid && hp.player==p.id && std::isfinite(hp.health);
        row.health=row.health_known?std::max(0.f,hp.health):0;
        row.status=p.spectator?RosterStatus::Spectator:!p.ready?RosterStatus::Loading:
            row.health_known && row.health<=0?RosterStatus::Dead:p.afk?RosterStatus::Afk:RosterStatus::Playing;
        const auto& latency=latencies[slot];
        row.ping_known=!row.bot && latency.id==p.id && latency.known;
        row.ping_ms=row.ping_known?latency.ping_ms:0;
        rows.push_back(std::move(row));
    }
    return rows;
}
}
