#pragma once
#include "session_authority.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace port::coop {
inline constexpr size_t chat_text_bytes=256,chat_history_limit=64;
inline constexpr unsigned chat_burst_limit=4;
inline constexpr uint64_t chat_rate_window_ms=5000;
struct ChatMessage {
    uint64_t sequence=0;
    PlayerId sender;
    std::string name,text;
    bool operator==(const ChatMessage&) const=default;
};
namespace chat_detail {
struct Character {uint32_t value;size_t begin,end;};
inline bool decode(std::string_view text,std::vector<Character>& characters) {
    for(size_t offset=0;offset<text.size();) {
        const auto begin=offset;const auto lead=uint8_t(text[offset++]);uint32_t value=lead;unsigned more=0;
        if(lead<0x80){}
        else if(lead>=0xc2 && lead<=0xdf){value=lead&0x1f;more=1;}
        else if(lead>=0xe0 && lead<=0xef){value=lead&0xf;more=2;}
        else if(lead>=0xf0 && lead<=0xf4){value=lead&7;more=3;}
        else return false;
        if(more>text.size()-offset)return false;
        for(unsigned i=0;i<more;++i){const auto part=uint8_t(text[offset++]);if((part&0xc0)!=0x80)return false;value=(value<<6)|(part&0x3f);}
        if((more==1 && value<0x80)||(more==2 && value<0x800)||(more==3 && value<0x10000)||value>0x10ffff||(value>=0xd800 && value<=0xdfff))return false;
        characters.push_back({value,begin,offset});
    }
    return true;
}
inline bool space(uint32_t value) {
    return value==0x20 || (value>=9 && value<=13) || value==0xa0 || value==0x1680 ||
        (value>=0x2000 && value<=0x200a) || value==0x2028 || value==0x2029 || value==0x202f || value==0x205f || value==0x3000;
}
inline bool control(uint32_t value){return value<0x20 || (value>=0x7f && value<=0x9f) || value==0x2028 || value==0x2029;}
}
// The byte limit never cuts a UTF-8 sequence. Invalid or overlong input is
// rejected as a whole; outer whitespace is trimmed and multiline text rejected.
inline std::optional<std::string> normalize_chat_text(std::string_view text) {
    if(text.empty() || text.size()>chat_text_bytes)return {};
    std::vector<chat_detail::Character> chars;if(!chat_detail::decode(text,chars))return {};
    size_t begin=0,end=chars.size();
    while(begin<end && chat_detail::space(chars[begin].value))++begin;
    while(end>begin && chat_detail::space(chars[end-1].value))--end;
    if(begin==end)return {};
    for(size_t i=begin;i<end;++i)if(chat_detail::control(chars[i].value))return {};
    return std::string(text.substr(chars[begin].begin,chars[end-1].end-chars[begin].begin));
}
inline bool chat_name_valid(std::string_view name) {
    if(name.empty() || name.size()>64)return false;
    std::vector<chat_detail::Character> chars;if(!chat_detail::decode(name,chars))return false;
    bool visible=false;for(const auto& ch:chars){if(chat_detail::control(ch.value))return false;visible|=!chat_detail::space(ch.value);}return visible;
}
class ChatRateLimit {
    std::deque<uint64_t> sent_;
public:
    bool accept(uint64_t now) {
        while(!sent_.empty() && now>=sent_.front() && now-sent_.front()>=chat_rate_window_ms)sent_.pop_front();
        if(sent_.size()>=chat_burst_limit)return false;
        sent_.push_back(now);return true;
    }
    void clear(){sent_.clear();}
};
// Session-owned presentation history, intentionally independent of snapshots
// and checkpoint rollback. The server supplies the authenticated participant.
class ChatLog {
    struct SenderRate {PlayerId sender;ChatRateLimit limit;};
    std::array<SenderRate,max_players> rates_{};
    std::deque<ChatMessage> messages_;
    uint64_t sequence_=0;
public:
    bool send(const Participant& participant,std::string_view text,uint64_t now) {
        auto normalized=normalize_chat_text(text);
        if(!participant.connected || !participant.id.generation || participant.id.slot>=max_players ||
           !chat_name_valid(participant.name) || !normalized || sequence_==std::numeric_limits<uint64_t>::max())return false;
        auto& rate=rates_[participant.id.slot];
        if(rate.sender!=participant.id){rate.sender=participant.id;rate.limit.clear();}
        if(!rate.limit.accept(now))return false;
        messages_.push_back({++sequence_,participant.id,participant.name,std::move(*normalized)});
        while(messages_.size()>chat_history_limit)messages_.pop_front();return true;
    }
    std::vector<ChatMessage> messages()const{return {messages_.begin(),messages_.end()};}
    void clear(){messages_.clear();rates_={};sequence_=0;}
};
}
