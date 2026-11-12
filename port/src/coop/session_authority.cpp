#include "session_authority.h"
#include "checkpoint_codec.h"
#include <stdexcept>
#include <utility>
#include <limits>
#include <algorithm>

namespace port::coop {
std::vector<uint8_t> SessionAuthority::checkpoint_state() const {
    checkpoint::Writer w;w.u32(presentation_vote_?0x53414332:0x53414331);w.u32(capacity_);
    w.u64(cutscene_counter_);w.u64(cutscene_);w.flag(skip_requested_);
    w.flag(bool(decision_));
    if(decision_){const auto& d=*decision_;w.u64(d.vote.cutscene);w.u32(d.vote.received);w.u32(d.vote.required);w.flag(d.vote.skip_requested);w.u32(d.connected_mask);w.u32(d.eligible_mask);w.u32(d.afk_mask);}
    for(unsigned i=0;i<max_players;++i){const auto& p=players_[i];w.u32(p.id.slot);w.u64(p.id.generation);w.flag(p.connected);w.flag(p.ready);w.flag(p.afk);w.u64(p.character);w.text(p.name);w.u64(generations_[i]);w.u64(sequences_[i]);w.flag(votes_[i]);}
    return w.finish();
}
bool SessionAuthority::restore_checkpoint(const std::vector<uint8_t>& bytes,std::string& error) {
    return decode_checkpoint(bytes,error,true);
}
bool SessionAuthority::decode_checkpoint(const std::vector<uint8_t>& bytes,std::string& error,bool exact_connections) {
    try {
        checkpoint::Reader r(bytes,4096);auto next=*this;
        auto require=[](bool ok){if(!ok)throw std::runtime_error("Invalid authority checkpoint state");};
        const auto format=r.u32();
        require((format==0x53414331 || format==0x53414332) && r.u32()==capacity_);
        next.presentation_vote_=format==0x53414332;
        next.cutscene_counter_=r.u64();next.cutscene_=r.u64();next.skip_requested_=r.flag();
        require(next.cutscene_<=next.cutscene_counter_ && (!next.presentation_vote_ || next.cutscene_));
        next.decision_.reset();
        if(r.flag()){
            SkipDecision d;d.vote.cutscene=r.u64();d.vote.received=r.u32();d.vote.required=r.u32();d.vote.skip_requested=r.flag();
            const auto connected=r.u32(),eligible=r.u32(),afk=r.u32(),mask=(1u<<capacity_)-1;
            require(!(connected&~mask) && !(eligible&~connected) && !(afk&~connected) && !(eligible&afk));
            require(d.vote.cutscene==next.cutscene_ && d.vote.cutscene && d.vote.skip_requested && d.vote.required && d.vote.required==d.vote.received && d.vote.required==unsigned(std::popcount(eligible)));
            d.connected_mask=uint8_t(connected);d.eligible_mask=uint8_t(eligible);d.afk_mask=uint8_t(afk);next.decision_=d;
        }
        require(next.skip_requested_==bool(next.decision_));
        for(unsigned i=0;i<max_players;++i){auto& p=next.players_[i];p.id.slot=r.u32();p.id.generation=r.u64();p.connected=r.flag();p.ready=r.flag();p.afk=r.flag();p.character=r.u64();p.name=r.text(64);next.generations_[i]=r.u64();next.sequences_[i]=r.u64();next.votes_[i]=r.flag();
            // Saved sockets/identities must never replace the currently authenticated peers.
            if(exact_connections)require(p.connected==players_[i].connected && p.id==players_[i].id && next.generations_[i]==generations_[i]);
            if(p.connected){require(i<capacity_ && p.id.slot==i && p.id.generation && p.id.generation==next.generations_[i] && !p.name.empty() && std::none_of(p.name.begin(),p.name.end(),[](unsigned char c){return c<32 || c==127;}));require(!p.ready || p.character);}
            else require(p.id==PlayerId{} && !p.ready && !p.afk && !p.character && p.name.empty());
            require(!next.votes_[i] || (next.cutscene_ && next.eligible(i)));
            for(unsigned j=0;j<i;++j)require(!p.character || p.character!=next.players_[j].character);
        }
        r.done();const auto vote=next.skip_vote();
        require(next.skip_requested_ || !vote.cutscene || !vote.required || vote.received!=vote.required);
        *this=std::move(next);return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool SessionAuthority::restore_gameplay_checkpoint(const std::vector<uint8_t>& bytes,std::string& error) {
    auto saved=*this;if(!saved.decode_checkpoint(bytes,error,false))return false;
    for(unsigned i=0;i<max_players;++i){
        const auto character=saved.players_[i].character;
        const bool afk=saved.players_[i].connected && saved.players_[i].afk;
        saved.players_[i]=players_[i];
        if(saved.players_[i].connected){saved.players_[i].character=character;saved.players_[i].ready=false;saved.players_[i].afk=afk;}
    }
    saved.generations_=generations_;saved.sequences_=sequences_;
    saved.cutscene_counter_=std::max(saved.cutscene_counter_,cutscene_counter_);
    saved.votes_.fill(false);saved.presentation_vote_=false;saved.cutscene_=0;saved.skip_requested_=false;saved.decision_.reset();
    *this=std::move(saved);return true;
}
SessionAuthority::SessionAuthority(unsigned capacity):capacity_(capacity) {
    if(capacity<1 || capacity>max_players)throw std::invalid_argument("Coop capacity must be 1..8");
}
bool SessionAuthority::retain_live_history(const std::vector<uint8_t>& bytes,std::string& error) {
    auto live=*this;if(!live.restore_checkpoint(bytes,error))return false;
    // Command replay floors belong to current sockets, never to a save file.
    // A newly started session may reuse slot/generation1 with fresh counters.
    // Same-session rewinds also retain exactly the currently accepted floor.
    for(unsigned i=0;i<max_players;++i){sequences_[i]=live.sequences_[i];players_[i].name=std::move(live.players_[i].name);}
    cutscene_counter_=std::max(cutscene_counter_,live.cutscene_counter_);
    return true;
}
Participant* SessionAuthority::find(PlayerId id) {
    if(id.slot>=capacity_)return nullptr;
    auto& player=players_[id.slot];
    return player.connected && player.id==id?&player:nullptr;
}
const Participant* SessionAuthority::participant(PlayerId id) const {
    if(id.slot>=capacity_)return nullptr;
    const auto& player=players_[id.slot];
    return player.connected && player.id==id?&player:nullptr;
}
bool SessionAuthority::automate(PlayerId id){auto* p=find(id);if(!p)return false;p->afk=true;votes_[id.slot]=false;reconsider_vote();return true;}
std::optional<PlayerId> SessionAuthority::join(std::string name,bool automated) {
    if(name.empty() || name.size()>64 || std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32 || c==127;}))return std::nullopt;
    for(unsigned slot=0;slot<capacity_;++slot) {
        if(players_[slot].connected)continue;
        if(generations_[slot]==std::numeric_limits<uint64_t>::max())continue;
        PlayerId id{slot,++generations_[slot]};
        players_[slot]={id,true,false,automated,0,std::move(name)};
        sequences_[slot]=0;votes_[slot]=false;
        return id;
    }
    return std::nullopt;
}
bool SessionAuthority::attach_character(PlayerId id,uint64_t entity) {
    auto* player=find(id);
    if(!player || !entity || (player->character && player->character!=entity))return false;
    for(const auto& other:players_)
        if(other.connected && other.id!=id && other.character==entity)return false;
    player->character=entity;
    return true;
}
bool SessionAuthority::finish_join(PlayerId id) {
    auto* player=find(id);if(!player || !player->character)return false;
    player->ready=true;reconsider_vote();return true;
}
void SessionAuthority::begin_loading(bool keep_presentation_vote) {
    for(auto& player:players_)if(player.connected){player.ready=false;player.character=0;}
    if(keep_presentation_vote && presentation_vote_)return;
    votes_.fill(false);presentation_vote_=false;cutscene_=0;skip_requested_=false;decision_.reset();
}
bool SessionAuthority::leave(PlayerId id) {
    auto* player=find(id);if(!player)return false;
    *player={};votes_[id.slot]=false;reconsider_vote();return true;
}
unsigned SessionAuthority::connected_count() const {
    unsigned count=0;for(const auto& player:players_)count+=player.connected;return count;
}
bool SessionAuthority::eligible(unsigned slot) const {
    const auto& player=players_[slot];return player.connected && (player.ready || presentation_vote_) && !player.afk;
}
bool SessionAuthority::accept_sequence(unsigned slot,uint64_t sequence) {
    if(!sequence || sequence<=sequences_[slot])return false;
    sequences_[slot]=sequence;return true;
}
uint64_t SessionAuthority::begin_cutscene(bool presentation) {
    if(cutscene_)throw std::logic_error("Cutscene already active");
    if(cutscene_counter_==std::numeric_limits<uint64_t>::max())throw std::overflow_error("Cutscene IDs exhausted");
    presentation_vote_=presentation;cutscene_=++cutscene_counter_;votes_.fill(false);skip_requested_=false;decision_.reset();return cutscene_;
}
bool SessionAuthority::end_cutscene(uint64_t cutscene) {
    if(!cutscene || cutscene!=cutscene_)return false;
    cutscene_=0;presentation_vote_=false;votes_.fill(false);skip_requested_=false;decision_.reset();return true;
}
CommandResult SessionAuthority::request_afk(PlayerId id,uint64_t sequence,bool afk,bool allow_unready) {
    auto* player=find(id);if(!player)return CommandResult::StalePlayer;
    if(!accept_sequence(id.slot,sequence))return CommandResult::StaleSequence;
    if(!player->ready && !presentation_vote_ && !allow_unready)return CommandResult::NotEligible;
    if(player->afk==afk)return CommandResult::Unchanged;
    player->afk=afk;
    // Returning humans must explicitly vote again while a vote remains open.
    votes_[id.slot]=false;reconsider_vote();return CommandResult::Applied;
}
CommandResult SessionAuthority::request_skip(PlayerId id,uint64_t sequence,uint64_t cutscene) {
    if(!find(id))return CommandResult::StalePlayer;
    if(!accept_sequence(id.slot,sequence))return CommandResult::StaleSequence;
    if(!cutscene || cutscene!=cutscene_)return CommandResult::WrongCutscene;
    if(!eligible(id.slot))return CommandResult::NotEligible;
    if(votes_[id.slot])return CommandResult::Unchanged;
    votes_[id.slot]=true;reconsider_vote();return CommandResult::Applied;
}
SkipVote SessionAuthority::skip_vote() const {
    SkipVote result{cutscene_,0,0,skip_requested_};
    if(!cutscene_)return result;
    for(unsigned slot=0;slot<capacity_;++slot)if(eligible(slot)) {
        ++result.required;result.received+=votes_[slot];
    }
    return result;
}
void SessionAuthority::reconsider_vote() {
    const auto vote=skip_vote();
    // A completed vote is a server event and cannot be undone by a late join
    // or AFK return between the decision and the engine's cutscene safe point.
    if(!skip_requested_ && vote.cutscene && vote.required && vote.received==vote.required){
        skip_requested_=true;SkipDecision decision;decision.vote=vote;decision.vote.skip_requested=true;
        for(unsigned slot=0;slot<capacity_;++slot){if(players_[slot].connected){decision.connected_mask|=uint8_t(1u<<slot);if(players_[slot].afk)decision.afk_mask|=uint8_t(1u<<slot);}if(eligible(slot))decision.eligible_mask|=uint8_t(1u<<slot);}
        decision_=decision;
    }
}
}
