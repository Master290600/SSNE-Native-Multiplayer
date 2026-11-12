#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace port::coop {
inline constexpr unsigned max_players = 8;
struct PlayerId {
    unsigned slot = max_players;
    uint64_t generation = 0;
    bool operator==(const PlayerId&) const = default;
};
struct Participant {
    PlayerId id;
    bool connected = false;
    bool ready = false;
    bool afk = false;
    uint64_t character = 0;
    std::string name;
};
struct SkipVote {
    uint64_t cutscene = 0;
    unsigned received = 0;
    unsigned required = 0;
    bool skip_requested = false;
};
struct SkipDecision {
    SkipVote vote;
    uint8_t connected_mask=0,eligible_mask=0,afk_mask=0;
};
enum class CommandResult { Applied, Unchanged, StalePlayer, StaleSequence, WrongCutscene, NotEligible };

// Owned by the authoritative server; its caller serializes access. Connection code supplies
// the authenticated PlayerId; requests cannot select another participant.
class SessionAuthority {
public:
    explicit SessionAuthority(unsigned capacity = max_players);
    std::optional<PlayerId> join(std::string name,bool automated=false);
    bool automate(PlayerId);
    bool attach_character(PlayerId player, uint64_t entity);
    bool finish_join(PlayerId player);
    void begin_loading(bool keep_presentation_vote=false);
    bool leave(PlayerId player);
    const Participant* participant(PlayerId player) const;
    const std::array<Participant,max_players>& participants() const { return players_; }
    unsigned connected_count() const;
    uint64_t begin_cutscene(bool presentation = false);
    bool end_cutscene(uint64_t cutscene);
    // The server alone derives allow_unready from its validated results snapshot.
    CommandResult request_afk(PlayerId connection, uint64_t sequence, bool afk,bool allow_unready=false);
    CommandResult request_skip(PlayerId connection, uint64_t sequence, uint64_t cutscene);
    SkipVote skip_vote() const;
    std::optional<SkipDecision> skip_decision() const {return decision_;}
    // Exact transaction participant. The server holds its control mutex for
    // capture/restore/rollback; historical connection reconciliation is separate.
    std::vector<uint8_t> checkpoint_state() const;
    bool restore_checkpoint(const std::vector<uint8_t>&,std::string& error);
    // Saved gameplay belongs to party slots; socket identities and replay
    // floors always belong to the currently authenticated connections.
    bool restore_gameplay_checkpoint(const std::vector<uint8_t>&,std::string& error);
    // Historical restore retains authenticated connections and monotonic
    // command/cutscene IDs from the live session; votes restart after loading.
    bool retain_live_history(const std::vector<uint8_t>& live,std::string& error);
private:
    bool decode_checkpoint(const std::vector<uint8_t>&,std::string&,bool exact_connections);
    Participant* find(PlayerId);
    bool eligible(unsigned slot) const;
    bool accept_sequence(unsigned slot,uint64_t sequence);
    void reconsider_vote();
    unsigned capacity_;
    std::array<Participant,max_players> players_{};
    std::array<uint64_t,max_players> generations_{};
    std::array<uint64_t,max_players> sequences_{};
    std::array<bool,max_players> votes_{};
    uint64_t cutscene_counter_ = 0, cutscene_ = 0;
    bool skip_requested_ = false;
    bool presentation_vote_ = false;
    std::optional<SkipDecision> decision_;
};
}
