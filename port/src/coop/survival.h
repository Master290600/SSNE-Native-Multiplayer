#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace port::coop {
enum class SurvivalPhase : uint8_t { Waiting,Countdown,Running,Defeat };
struct SurvivalConfig {
    uint32_t seed=0x53535631u,countdown_ms=5000,wave_ms=30000,restart_ms=15000,spawn_ms=750;
    unsigned alive_cap=32,enemy_multiplier=1;
};
struct SurvivalObservation {
    // Host simulation clock, not wall time or display frames. Reset the
    // controller on map/epoch change; never advance this clock while paused.
    uint64_t now_ms=0;
    uint8_t connected=0,ready=0,alive=0,voluntary_spectators=0;
    std::array<uint64_t,8> generations{};
    unsigned live_enemies=0,confirmed_kills=0;
};
struct SurvivalSpawn {uint64_t ticket=0;unsigned definition=0;};
struct SurvivalState {
    SurvivalPhase phase=SurvivalPhase::Waiting;
    uint32_t round=1,wave=0,total_spawned=0,kills=0,quota_remaining=0,active_enemies=0;
    uint64_t survival_ms=0,best_ms=0,phase_remaining_ms=0,wave_remaining_ms=0;
    uint8_t members=0,playing_mask=0,waiting_mask=0,eliminated_mask=0;
    std::array<uint64_t,8> member_generations{};
};
struct SurvivalStep {SurvivalState state;std::vector<SurvivalSpawn> spawns;bool restart_due=false;};
// Deterministic authority policy. Native creation, collisions, damage,
// resources, respawn and network publication belong to the integration layer.
class SurvivalController {
public:
    explicit SurvivalController(SurvivalConfig config={}){reset(config);}
    void reset(SurvivalConfig config={});
    SurvivalStep advance(const SurvivalObservation&);
    // Confirm the actual native factory result before the next advance.
    // Failed or duplicate confirmations cannot consume quota or increase kills.
    bool finish_spawn(uint64_t ticket,bool success);
    // Call only after the host has cleaned the round and requested native
    // player respawns. Dead/late players become eligible for the new countdown.
    void restart(uint64_t now_ms);
    // Live host rule: adjust future quota without resetting time, membership
    // or already-authorized native factory tickets. Never deletes live enemies.
    void set_enemy_multiplier(unsigned factor);
    const SurvivalState& state()const{return state_;}
    const SurvivalConfig& config()const{return config_;}
    bool playing(unsigned slot,uint64_t generation)const;
private:
    SurvivalConfig config_{};SurvivalState state_{};
    uint64_t last_ms_=0,phase_started_ms_=0,round_started_ms_=0,next_spawn_ms_=0,next_ticket_=0;
    uint32_t spawned_wave_=0,failed_wave_=0;
    bool clock_set_=false;
    struct Pending {uint64_t ticket;uint32_t wave;};
    std::vector<Pending> pending_;
    unsigned wave_quota()const;
};
// This finite ground roster deliberately excludes bosses, story actors,
// aquatic classes and flying enemies that need authored encounter volumes.
const std::vector<unsigned>& survival_enemy_roster();
unsigned survival_enemy_for(uint32_t seed,uint32_t wave,uint32_t spawn_index);
}
