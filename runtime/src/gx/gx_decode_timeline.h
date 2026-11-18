#pragma once
#include "ssne/gx_decode_context.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace ssne::detail {
struct GxDecodeChoice {
    bool marked=false;
    GxDecodeContext context{};
    bool operator==(const GxDecodeChoice&)const=default;
};
struct GxDecodeChange {
    uint64_t offset=0;
    GxDecodeChoice choice{};
    GxDecodeMarkerDebug debug{};
    uint64_t marker_offset=0; // Original issue offset; compaction never relabels it.
    bool operator==(const GxDecodeChange&)const=default;
};
// Caller supplies synchronization. Same-offset changes have no intervening
// bytes and may replace the last unpublished-to-byte-stream classification.
template<size_t Capacity> class GxDecodeMarkerQueue {
    static_assert(Capacity>0);
    std::array<GxDecodeChange,Capacity> changes_{};
    size_t first_=0,count_=0,high_water_=0;
public:
    bool push(const GxDecodeChange& change){
        if(count_){auto& last=changes_[(first_+count_-1)%Capacity];if(change.offset<last.offset)return false;if(change.offset==last.offset){last=change;return true;}}
        if(count_==Capacity)return false;
        changes_[(first_+count_)%Capacity]=change;++count_;high_water_=std::max(high_water_,count_);return true;
    }
    const GxDecodeChange& front()const{return changes_[first_];}
    void pop(){first_=(first_+1)%Capacity;--count_;}
    void clear(){first_=count_=0;}
    size_t size()const{return count_;}
    size_t high_water()const{return high_water_;}
};
// Parser-owned: an opcode is classified by its absolute START offset. A
// partial header/payload keeps that classification until the command ends.
class GxDecodeTimeline {
    std::vector<GxDecodeChange> changes_;
public:
    void reserve(size_t count){changes_.reserve(count);}
    bool append(const GxDecodeChange& change){
        if(!changes_.empty()){
            if(change.offset<changes_.back().offset)return false;
            if(change.offset==changes_.back().offset){changes_.back()=change;return true;}
        }
        changes_.push_back(change);return true;
    }
    const GxDecodeChange* find(uint64_t offset)const{
        auto it=std::upper_bound(changes_.begin(),changes_.end(),offset,[](uint64_t position,const GxDecodeChange& change){return position<change.offset;});
        return it==changes_.begin()?nullptr:&*--it;
    }
    GxDecodeChoice at(uint64_t offset)const{const auto* change=find(offset);return change?change->choice:GxDecodeChoice{};}
    // parse() stops only at one incomplete command. All changes inside that
    // unparsed prefix cannot affect its start; only the latest survives for
    // the NEXT command. This also bounds unlimited byte-less scope changes.
    void compact_pending(uint64_t begin,uint64_t end){
        const auto* f=find(begin);const auto* l=find(end);
        auto first=f?*f:GxDecodeChange{},last=l?*l:GxDecodeChange{};
        first.offset=begin;last.offset=end;changes_.clear();changes_.push_back(first);
        // A newer diagnostic source can share all five semantic flags. Retain
        // its issue tag for the next opcode without changing state-file masks.
        if(end>begin&&(last.choice!=first.choice||last.debug.serial!=first.debug.serial))changes_.push_back(last);
    }
    std::vector<GxDecodeChange> pending_state(uint64_t begin,uint64_t end)const{
        const auto first=at(begin),last=at(end);std::vector<GxDecodeChange> result{{0,first}};
        if(end>begin&&last!=first)result.push_back({end-begin,last});return result;
    }
    void restore_pending(const std::vector<GxDecodeChange>& relative){changes_=relative;}
    size_t size()const{return changes_.size();}
};
// Ring counters never rewind on savestate load. Loaded pending bytes form a
// virtual prefix; subsequent raw-ring offsets follow that prefix monotonically.
struct GxDecodeOffsetClock {
    uint64_t raw_origin=0,logical_prefix=0;
    uint64_t logical(uint64_t raw)const{return logical_prefix+(raw-raw_origin);}
    void restore(uint64_t raw_head,uint64_t pending_bytes){raw_origin=raw_head;logical_prefix=pending_bytes;}
};
inline uint32_t gx_decode_choice_bits(const GxDecodeChoice& choice){
    if(!choice.marked)return 0;
    return 32u|(choice.context.discard?1u:0u)|(choice.context.local?2u:0u)|(choice.context.ui?4u:0u)|(choice.context.gui_camera?8u:0u)|(choice.context.frontend?16u:0u);
}
inline bool gx_decode_choice_from_bits(uint32_t bits,GxDecodeChoice& choice){
    if(bits&~63u || (!(bits&32u)&&bits))return false;
    choice.marked=(bits&32u)!=0;choice.context={(bits&1u)!=0,(bits&2u)!=0,(bits&4u)!=0,(bits&8u)!=0,(bits&16u)!=0};return true;
}
inline bool valid_gx_decode_pending_state(const std::vector<GxDecodeChange>& changes,uint64_t pending_bytes){
    if(changes.empty()||changes.size()>2||changes[0].offset!=0)return false;
    if(changes.size()==2&&(pending_bytes==0||changes[1].offset!=pending_bytes||changes[0].choice==changes[1].choice))return false;
    return true;
}
}
