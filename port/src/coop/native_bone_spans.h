#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iterator>
#include <span>
#include <vector>
namespace port::coop::presentation_detail {
// Per-call descriptors retain only fresh guest addresses and owning-pose indices.
// They never point at a growing vector element or retain source snapshot storage.
struct NativeBoneSpan {uint32_t address=0,count=0,ordinal=0;size_t pose_index=0;};
struct BoneWriteRange {uint32_t address=0,size=0;};
struct SavedBoneSpan {uint32_t address=0;std::vector<uint8_t> original;};
inline bool reserve_visual_ordinals(uint64_t& next,uint64_t count){
    if(next>std::numeric_limits<uint32_t>::max()||count>uint64_t(std::numeric_limits<uint32_t>::max())-next)return false;next+=count;return true;
}
inline bool valid_bone_values(std::span<const float> values){
    // Preserve the render preflight's1e6 threshold. The later pose registry's
    // stricter1e4 rejection intentionally remains a post-write rollback case.
    for(float value:values)if(!std::isfinite(value)||std::abs(value)>1000000.f)return false;return true;
}
inline bool disjoint_bone_spans(std::span<const NativeBoneSpan> spans,std::span<const BoneWriteRange> words,uint32_t ram_end){
    struct Range {uint64_t begin,end;};std::vector<Range> ranges;ranges.reserve(spans.size());
    for(const auto& span:spans){const uint64_t end=uint64_t(span.address)+uint64_t(span.count)*4;
        if(!span.count||span.count>256*12||span.address<0x80000000u||end>ram_end)return false;
        ranges.push_back({span.address,end});}
    std::sort(ranges.begin(),ranges.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
    for(size_t i=1;i<ranges.size();++i)if(ranges[i].begin<ranges[i-1].end)return false;
    for(const auto& word:words){const uint64_t begin=word.address,end=begin+word.size;if(!word.size||word.address<0x80000000u||end>ram_end)return false;
        const auto next=std::lower_bound(ranges.begin(),ranges.end(),end,[](const auto& range,uint64_t position){return range.begin<position;});
        if(next!=ranges.begin()&&std::prev(next)->end>begin)return false;}
    return true;
}
inline uint32_t guest_word(uint32_t bits){
    if constexpr(std::endian::native==std::endian::little)return (bits>>24)|((bits>>8)&0x0000ff00u)|((bits<<8)&0x00ff0000u)|(bits<<24);
    return bits;
}
inline bool prepare_bone_bytes(std::span<const uint8_t> original_bytes,std::span<const float> visual,std::vector<float>& original,std::vector<uint8_t>& packed){
    if(original_bytes.size()!=visual.size()*4||!valid_bone_values(visual))return false;
    original.resize(visual.size());packed.resize(original_bytes.size());
    for(size_t i=0;i<visual.size();++i){uint32_t before;std::memcpy(&before,original_bytes.data()+i*4,4);original[i]=std::bit_cast<float>(guest_word(before));
        const uint32_t after=guest_word(std::bit_cast<uint32_t>(visual[i]));std::memcpy(packed.data()+i*4,&after,4);}
    return true;
}
// Alias fallback restores the original append order BEFORE the old stable
// normalizer sees it, including gaps occupied by every expanded bone word.
template<class Word,class Pose> bool expand_bone_words(std::vector<Word>& words,std::span<const NativeBoneSpan> spans,const std::vector<Pose>& poses){
    size_t count=words.size();for(const auto& span:spans){if(span.pose_index>=poses.size()||poses[span.pose_index].visual.size()!=span.count||uint64_t(span.ordinal)+span.count>std::numeric_limits<uint32_t>::max()||span.count>std::numeric_limits<size_t>::max()-count)return false;count+=span.count;}
    words.reserve(count);
    for(const auto& span:spans)for(uint32_t i=0;i<span.count;++i)words.push_back({span.address+i*4,std::bit_cast<uint32_t>(poses[span.pose_index].visual[i]),span.ordinal+i,true});
    std::sort(words.begin(),words.end(),[](const auto& a,const auto& b){return a.ordinal<b.ordinal;});return true;
}
}
