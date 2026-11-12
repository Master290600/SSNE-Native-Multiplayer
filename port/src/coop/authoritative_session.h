#pragma once
#include "world_state.h"
#include "native_event_journal.h"
#include "client_presentation.h"
#include "lan_discovery.h"
#include "ssne/input.h"
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace port::coop {
struct ControlInput {
    ssne::PadState pad;
    uint16_t pressed=0;
    int32_t yaw=0,pitch=0; // radians * 1,000,000; requests, never authoritative transforms
    uint8_t weapon=255;
    std::array<uint8_t,8> preferences{255,255,255,255,255,255,255,255};
};
struct PlayerControl {
    PlayerId id;
    bool ready=false,afk=false;
    uint64_t sequence=0;
    ControlInput input;
};
struct ConnectionEvent { enum class Kind { Joined,Left }; Kind kind; PlayerId player; };
struct LobbySkinChoice {PlayerId player{};uint8_t model=255;};
struct NetworkStatus {
    // Local lifecycle only; never serialized into protocol messages.
    bool connecting=false,connected=false;
    uint16_t port=0;
    PlayerId player;
    unsigned capacity=0,joined=0;
    uint64_t bytes_sent=0,bytes_received=0,world_revision=0;
    unsigned ping_ms=0;
    std::string message;
};
class AuthoritativeServer {
public:
    class PreparedResume {
        friend class AuthoritativeServer;
        std::function<void()> commit_;
    public:
        void commit(){auto action=std::move(commit_);if(action)action();}
    };
    explicit AuthoritativeServer(unsigned capacity=8);
    ~AuthoritativeServer();
    bool start(uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& host_name="Host",unsigned skin=255);
    std::string lan_session_id() const;
    LanJoinGrant issue_lan_join_ticket(const std::string& peer,uint32_t nonce,uint32_t lifetime_ms=60000);
    std::array<LobbySkinChoice,max_players> requested_skins() const;
    bool set_match_player_limit(unsigned);
    std::optional<PlayerId> add_bot(unsigned skill=2);
    bool remove_bot(PlayerId);
    uint8_t bot_mask() const;
    unsigned bot_skill(unsigned slot) const;
    NetworkStatus status() const;
    std::shared_ptr<const WorldState> latest_world() const;
    void lobby_ready(bool);
    bool begin_lobby_game();
    std::vector<ConnectionEvent> take_events();
    // Loading/result screens release character bindings and input readiness atomically.
    bool publish(WorldState world,std::string& error,bool loading=false,std::span<const WorldState> native_batches={});
    bool attach_character(PlayerId,EntityId);
    bool ready_host();
    std::array<PlayerControl,max_players> take_controls();
    std::array<Participant,max_players> participants() const;
    // Keep this guest-thread lease alive over the whole runtime transaction.
    // It freezes membership, commands and votes while nested calls remain legal.
    std::shared_ptr<void> checkpoint_control_lease();
    std::vector<uint8_t> checkpoint_authority() const;
    bool restore_checkpoint_authority(const std::vector<uint8_t>&,std::string& error);
    bool restore_gameplay_authority(const std::vector<uint8_t>&,std::string& error);
    std::unique_ptr<PreparedResume> prepare_checkpoint_resume(const std::vector<uint8_t>& live_authority,
        uint64_t epoch,const std::string& level,const Properties& setup,const NativeEventCursors&,std::string& error);
    uint64_t begin_cutscene(bool presentation=false);
    bool end_cutscene(uint64_t);
    SkipVote skip_vote() const;
    std::optional<SkipDecision> skip_decision() const;
    CommandResult host_afk(uint64_t sequence,bool);
    CommandResult host_skip(uint64_t sequence,uint64_t cutscene);
    void submit_host_input(ControlInput);
    void disconnect(PlayerId);
    void stop();
private:
    friend class NativeSession;
    // Only NativeSession may transfer a freshly built graph with no retained
    // mutable aliases. Ordinary publish() preserves its defensive copy.
    bool publish_owned(WorldState world,std::string& error,bool loading,std::span<const WorldState> native_batches);
    bool publish_impl(WorldState world,std::string& error,bool loading,std::span<const WorldState> native_batches,bool owned);
    struct Impl;std::unique_ptr<Impl> impl_;
};
class AuthoritativeClient {
public:
    struct WorldSample {std::shared_ptr<const WorldState> world;double received=0;};
    AuthoritativeClient();~AuthoritativeClient();
    bool start(const std::string& address,uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& name,unsigned skin=255);
    bool start_lan(const std::string& address,uint16_t port,const std::string& ticket,uint64_t fingerprint,const std::string& name,unsigned skin=255);
    NetworkStatus status() const;
    std::shared_ptr<const WorldState> latest_world() const;
    WorldSample world_sample() const;
    std::vector<WorldSample> motion_samples(uint64_t after,uint64_t through) const;
    PredictedLook predicted_look(uint64_t epoch,uint64_t confirmed_input);
    void clear_predicted_look();
    void ready(uint64_t epoch);
    void lobby_ready(bool);
    void submit_input(ControlInput);
    void set_afk(bool);
    void vote_skip(uint64_t cutscene);
    void request_snapshot();
    void acknowledge_native_events(uint64_t epoch,const NativeEventCursors& consumed);
    void stop();
private:
    bool start_connection(const std::string& address,uint16_t port,const std::string& credential,uint64_t fingerprint,const std::string& name,unsigned skin,bool lan);
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
