#pragma once
#include "authoritative_session.h"
#include "native_bootstrap.h"
#include "native_replica.h"
#include "native_lifecycle.h"
#include <functional>
#include <chrono>
#include <deque>
#include <variant>
#include <mutex>
#include <atomic>
#include "ssne/native_effect.h"
#include "ssne/checkpoint.h"
#include "checkpoint_file.h"
#include "../test_bot.h"
#include "bot_navigation.h"
namespace port::coop {
class NativeSession {
public:
    using Log=std::function<void(const std::string&)>;
    NativeSession(AuthoritativeServer&,unsigned capacity,std::string level,Log);
    NativeSession(AuthoritativeClient&,Log);
    ~NativeSession();
    void tick(PPCContext&,uint64_t tick);
    void frame(PPCContext&,uint64_t frame);
    void render_begin(PPCContext&);
    void render_end(PPCContext&);
    std::array<uint8_t,8> local_preferences() const {auto bits=local_preferences_.load();std::array<uint8_t,8> out{};for(auto& n:out){n=uint8_t(bits);bits>>=8;}return out;}
    NativeWorld& world(){return native_;}
    struct CombatFeedback {uint64_t hits=0,kills=0;float last_damage=0;bool flash=false;};
    CombatFeedback combat_feedback() const;
    struct ParticipantHealth {PlayerId player;float health=0;bool valid=false;};
    std::array<ParticipantHealth,max_players> participant_health() const;
    bool request_lobby_campaign(bool resume);
    struct SaveResult {bool save=false,ok=false;int slot=0;std::string message;};
    void configure_saves(savefile::Identity);
    void configure_startup_save(savefile::State state,int slot);
    bool request_save(bool save,int slot,std::string path,std::string& error);
    bool take_save_result(SaveResult&);
private:
    std::atomic<uint64_t> local_preferences_{UINT64_MAX};
    std::optional<savefile::State> startup_save_;
    int startup_slot_=0;
    void write_save_test_evidence(bool save,uint64_t frame);
    struct SaveRequest {bool save;int slot;std::string path;std::chrono::steady_clock::time_point deadline;};
    std::mutex save_mutex_;
    std::mutex lobby_mutex_;
    std::optional<bool> lobby_campaign_request_;
    std::optional<savefile::Identity> save_identity_;
    std::optional<SaveRequest> save_request_;
    std::optional<SaveResult> save_result_;
    bool save_busy_=false;
    void process_save_request(PPCContext&,bool ready,uint64_t frame);
    void perform_save_request(PPCContext&,bool ready,uint64_t frame);
    AuthoritativeServer* server_=nullptr;
    AuthoritativeClient* client_=nullptr;
    NativeWorld native_;
    NativeReplica replica_{native_};
    ClientPresentation presentation_;
    std::shared_ptr<const WorldState> presentation_applied_;
    std::chrono::steady_clock::time_point motion_started_=std::chrono::steady_clock::now();
    uint64_t presentation_frames_=0;
    uint64_t presentation_revision_=0,presentation_generation_=1;
    Log log_;
    unsigned capacity_=2;
    std::string level_,last_error_;
    std::array<PlayerId,max_players> players_{};
    std::array<EntityId,max_players> characters_{};
    struct JoinPlacement {PlayerId player{};uint32_t actor=0;uint64_t spawn_revision=0;bool requested=false;};
    std::array<JoinPlacement,max_players> join_placements_{};
    bool queued_=false,host_ready_=false,client_ready_=false;
    std::chrono::steady_clock::time_point ready_retry_next_{};
    uint64_t ready_retry_epoch_=0;
    PlayerId ready_retry_owner_{};
    bool fixture_dumped_=false;
    bool level_loading_=false;
    uint64_t loading_epoch_=0;
    uint64_t loading_native_epoch_=0; // Client: the old local scene must actually exit before reload can finish.
    uint64_t epoch_=1,last_revision_=0;
    Properties setup_;
    std::map<EntityId,std::vector<float>> enemy_pose_expected_;
    std::map<uint32_t,uint32_t> cutscene_trace_states_;
    uint32_t active_cutscene_object_=0;
    uint64_t active_cutscene_vote_=0;
    uint64_t active_movie_sequence_=0;
    bool cutscene_skip_applied_=false;
    uint64_t cutscene_epoch_=0;
    std::array<float,14> replica_cutscene_camera_{};
    uint64_t replica_cutscene_id_=0,replica_cutscene_tick_=0;
    uint64_t replica_music_epoch_=0,replica_music_sequence_=0;
    uint64_t replica_speech_sequence_=0;
    uint64_t replica_speech_running_sequence_=0;
    uint64_t replica_effect_epoch_=0,replica_effect_sequence_=0;
    uint64_t replica_shot_epoch_=0,replica_shot_sequence_=0;
    uint64_t replica_loop_epoch_=0,replica_loop_sequence_=0;
    uint64_t replica_impact_epoch_=0,replica_impact_sequence_=0;
    uint64_t replica_feedback_epoch_=0,replica_feedback_sequence_=0;
    uint64_t replica_trail_epoch_=0,replica_trail_sequence_=0;
    uint64_t replica_emitter_epoch_=0,replica_emitter_sequence_=0;
    uint64_t replica_casing_epoch_=0,replica_casing_sequence_=0;
    uint64_t replica_projectile_epoch_=0,replica_projectile_sequence_=0;
    uint64_t replica_surface_epoch_=0,replica_surface_sequence_=0;
    uint64_t replica_beam_epoch_=0,replica_beam_sequence_=0;
    uint64_t replica_hit_epoch_=0,replica_hit_sequence_=0;
    mutable std::mutex combat_mutex_;PlayerId combat_owner_{};CombatFeedback combat_feedback_{};std::chrono::steady_clock::time_point combat_flash_until_{};
    std::array<ParticipantHealth,max_players> participant_health_{};
    void accept_confirmed_hit(const ssne::NativeConfirmedHit&,PlayerId);
    using WeaponSound=std::variant<ssne::NativeWeaponShot,ssne::NativeWeaponLoop>;
    struct PendingSound {WeaponSound event;std::chrono::steady_clock::time_point received;};
    std::deque<PendingSound> weapon_sounds_;
    uint64_t weapon_sound_order_=0;
    bool drain_weapon_sounds(PPCContext&);
    std::chrono::steady_clock::time_point test_replica_next_{};
    std::chrono::steady_clock::time_point replica_effect_time_{};
    NativeHudPresentationClock replica_hud_clock_{};
    uint64_t speech_phase_sequence_=0;
    int speech_phase_=-1;
    uint64_t test_delayed_speech_=0;
    std::chrono::steady_clock::time_point test_speech_received_{};
    bool test_speech_waiting_=false;
    std::array<port::netplay::CompanionBot,max_players> afk_bots_{};
    struct GameBotState {PlayerId player{};BotNavigation navigation;uint32_t target=0;unsigned reaction=0;BotPoint goal{},previous{};EntityId pickup=0;unsigned goal_age=0;std::map<EntityId,unsigned> deferred;};
    std::array<GameBotState,max_players> game_bots_{};
    std::array<ControlInput,max_players> afk_inputs_{};
    std::array<uint32_t,max_players> afk_follow_slots_{};
    std::array<bool,max_players> afk_native_active_{};
    std::array<bool,max_players> afk_states_{};
    struct AfkAimLease {PlayerId player;uint32_t actor=0,aim=0;bool saved=false;};
    std::array<AfkAimLease,max_players> afk_aim_leases_{};
    struct NativeLifeWait {PlayerId player;uint32_t actor=0;uint64_t spawn_revision=0;};
    std::array<NativeLifeWait,max_players> native_life_wait_{};
    struct AfkRespawnWait {uint32_t actor=0;uint64_t spawn_revision=0;};
    std::array<AfkRespawnWait,max_players> afk_respawn_wait_{};
    std::array<unsigned,max_players> afk_decisions_{};
    uint64_t afk_epoch_=0;
    std::chrono::steady_clock::time_point afk_next_decision_{};
    void update_afk_controls(PPCContext&,uint64_t,const std::array<PlayerControl,max_players>&);
    void report(const std::string&);
    bool initialize(PPCContext&,const NativeBootstrapRequest&,uint32_t presentation_scene=0);
    void capture_fixture(const NativeWorldSample&,const PPCContext&);
    bool save_session_checkpoint(std::vector<uint8_t>&,std::string&) const;
    bool load_session_checkpoint(const std::vector<uint8_t>&,std::string&,bool reconcile_connections=false);
    bool test_session_checkpoint(std::string&);
    ssne::RuntimeCheckpointHooks session_checkpoint_hooks();
    bool restore_historical_checkpoint(PPCContext&,const std::vector<uint8_t>& core,const std::vector<uint8_t>& extra,std::string&,bool reject_before_commit=false);
    bool test_historical_checkpoint(PPCContext&,uint64_t frame,std::string&);
};
}
