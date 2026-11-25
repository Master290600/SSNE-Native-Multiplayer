#pragma once
#include "ssne/native_movement_step.h"
#include <bit>
#include <cmath>
#include <utility>

namespace ssne::movement_step_detail {
struct Binding {unsigned slot=0;uint32_t controller=0;};
inline bool range(uint32_t p,unsigned bytes,uint32_t end){
    return p>=0x80000000u && end>=0x80000000u && bytes<=end-0x80000000u && p<=end-bytes;
}
inline bool step_time(float dt){return std::isfinite(dt) && dt==.01f;}
inline bool command_valid(const NativeMovementCommand& command){
    return command.sequence && std::isfinite(command.yaw) && std::abs(command.yaw)<=10000.f;
}
template<class Read>
std::optional<Binding> binding(uint32_t actor,uint32_t registry,unsigned count,uint32_t end,bool waiting,Read read){
    if(waiting || !count || count>8 || !range(actor,0x790,end) || !range(registry,count*4,end) ||
       read(0x80000000u)!=0x47334250u || read(0x802DB7E8u)!=4 || read(actor+0x48)!=0x802121E8u)return {};
    const auto slot=read(actor+0x17C),controller=read(actor+0x344);
    if(slot>=count || read(actor+0x178)>=count || read(registry+slot*4)!=actor || read(actor+0x340)!=1 ||
       read(actor+0x328) || read(actor+0x370) || read(actor+0x6C4) || read(actor+0x6FC) || read(actor+0x330) ||
       !range(controller,0x38,end) || read(controller)!=0x80210E80u)return {};
    const float health=std::bit_cast<float>(read(actor+0xCC));
    const auto phase=read(controller+4);
    if(!std::isfinite(health) || health<=0.f || (phase!=0 && phase!=1 && phase!=3) || read(controller+0xC)>1)return {};
    const auto step_source=read(actor+0x23C);
    if(!range(step_source,0x28,end) || !step_time(std::bit_cast<float>(read(step_source+0x24))))return {};
    return Binding{slot,controller};
}
template<class Read>
std::optional<NativeMovementState> state(uint32_t actor,const Binding& b,Read read){
    NativeMovementState out;
    out.phase=read(b.controller+4);out.pending_jump=read(b.controller+0xC);
    out.contact_kind=read(actor+0x6C8);
    if(out.contact_kind>1)return {};
    out.grounded=read(actor+0x6D0)!=0;out.sliding=read(actor+0x6E4)!=0;
    for(unsigned i=0;i<3;++i){
        out.position[i]=std::bit_cast<float>(read(actor+0x20+i*4));
        out.velocity[i]=std::bit_cast<float>(read(actor+0x298+i*4));
        if(!std::isfinite(out.position[i]) || std::abs(out.position[i])>1000000.f ||
           !std::isfinite(out.velocity[i]) || std::abs(out.velocity[i])>=160.f)return {};
    }
    for(unsigned i=0;i<7;++i){
        // The original constructor leaves jump-only10..20 undefined until a
        // jump begins. Those words are not read outside the curve phase.
        if(i<5 && out.phase!=1)continue;
        const float value=std::bit_cast<float>(read(b.controller+0x10+i*4));
        if(!std::isfinite(value) || std::abs(value)>1000000.f)return {};
        out.curve[i]=value;
    }
    if(out.curve[5]<0.f || out.curve[5]>.2f ||
       (out.phase==1 && (out.curve[0]<0.f || out.curve[1]<=0.f || out.curve[1]>60.f || out.curve[2]<0.f || out.curve[2]>out.curve[1]+.01f)))return {};
    for(unsigned i=0;i<5;++i){
        const float value=std::bit_cast<float>(read(actor+0x708+i*4));
        if(!std::isfinite(value) || std::abs(value)>1000000.f)return {};
        out.ground[i]=value;
    }
    return out;
}
// Restore only values still owned by this scope. A native callback which
// explicitly replaced an input word or the player's yaw keeps its new value.
template<class Read,class Write>
class ScopedWords {
    struct Word {uint32_t address,previous,injected;};
    std::array<Word,6> words_{};unsigned count_=0;
    Read read_;Write write_;
public:
    ScopedWords(Read read,Write write):read_(std::move(read)),write_(std::move(write)){}
    void set(uint32_t address,uint32_t value){
        words_[count_++]={address,read_(address),value};write_(address,value);
    }
    ~ScopedWords(){for(unsigned i=count_;i>0;--i){const auto& w=words_[i-1];if(read_(w.address)==w.injected)write_(w.address,w.previous);}}
    ScopedWords(const ScopedWords&)=delete;
    ScopedWords& operator=(const ScopedWords&)=delete;
};
}
