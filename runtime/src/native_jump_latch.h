#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace ssne::detail {
struct NativeJumpBinding {
    uint32_t actor=0,bank=0,mode=0;
    uint64_t life=0;
    bool operator==(const NativeJumpBinding&)const=default;
};
struct NativeJumpWorld {
    uint32_t ram_end=0,actor_type=0,first_bank=0,second_bank=0,registry=0;
    unsigned count=4;
    bool network=false;
    uint64_t life=0;
};
// The input controller(+178) and network membership(+17C) are separate native
// identities. Offline input follows the assigned controller; online mailboxes
// follow membership while still reading that actor's actual controller bank.
template<class ReadWord>
bool resolve_native_jump_binding(uint32_t actor,const NativeJumpWorld& world,
                                 ReadWord read,unsigned& slot,NativeJumpBinding& out){
    auto valid=[&](uint32_t p,uint32_t n){return world.ram_end>=0x80000000u+n && p>=0x80000000u && p<=world.ram_end-n;};
    if(!valid(actor,0x790) || read(actor+0x48)!=world.actor_type || !world.count || world.count>8)return false;
    const auto controller=read(actor+0x178);
    if(controller>=(world.network?world.count:4u) || controller>=8)return false;
    slot=world.network?read(actor+0x17C):controller;
    if(slot>=(world.network?world.count:4u) || slot>=8)return false;
    if(world.network && (!valid(world.registry,4*world.count) || read(world.registry+4*slot)!=actor))return false;
    if(!valid(world.first_bank,0x70C) || (controller>=4 && !valid(world.second_bank,0x70C)))return false;
    const uint64_t wide=controller<4?uint64_t(world.first_bank)+controller*0x70C:uint64_t(world.second_bank)+(controller-4)*0x70C;
    if(wide>0xFFFFFFFFu || !valid(uint32_t(wide),0x70C))return false;
    const auto mode=read(actor+0x344);if(!valid(mode,0x38))return false;
    out={actor,uint32_t(wide),mode,world.life};return true;
}

// Guest-thread retention of a genuine jump edge until native physics gets one
// chance to read it. The mailbox is the only cross-thread part: producers use
// the currently published actor generation, never guest addresses or RAM.
class NativeJumpLatch {
public:
    static constexpr unsigned slots=8;
    static constexpr uint32_t lifetime_ms=50;
    struct Decision {bool pressed=false,retained=false,queued=false;};
    void queue(unsigned slot,uint32_t now) {
        if(slot>=slots)return;
        const auto generation=published_[slot].load(std::memory_order_acquire);
        if(generation)mailbox_[slot].store((uint64_t(generation)<<32)|now,std::memory_order_release);
    }
    // Thread-safe cancellation; guest state is retired on its next visit.
    void cancel(unsigned slot) {
        if(slot>=slots)return;
        published_[slot].store(0,std::memory_order_release);
        mailbox_[slot].store(0,std::memory_order_release);
        cancelled_[slot].fetch_add(1,std::memory_order_release);
    }
    void cancel_all(){for(unsigned i=0;i<slots;++i)cancel(i);}
    uint32_t cancellation(unsigned slot)const{return slot<slots?cancelled_[slot].load(std::memory_order_acquire):0;}
    Decision prepare(unsigned slot,const NativeJumpBinding& binding,bool eligible,
                     bool native_pressed,uint64_t sample,uint32_t now) {
        if(slot>=slots)return {};
        auto& state=state_[slot];
        const auto cancelled=cancelled_[slot].load(std::memory_order_acquire);
        if(!eligible || !state.generation || state.binding!=binding || state.cancelled!=cancelled){
            state={};state.cancelled=cancelled;state.binding=binding;
            // A late write from a producer holding the retired generation is
            // rejected below even if it races this exchange.
            mailbox_[slot].exchange(0,std::memory_order_acq_rel);
            if(eligible){if(++next_generation_==0)++next_generation_;state.generation=next_generation_;}
            published_[slot].store(state.generation,std::memory_order_release);
        }
        if(!eligible)return {};
        if(state.sample!=sample){
            state.sample=sample;
            if(native_pressed){state.pending=true;state.when=now;}
        }
        const auto queued=mailbox_[slot].exchange(0,std::memory_order_acq_rel);
        const bool fresh=queued && uint32_t(queued>>32)==state.generation && uint32_t(now-uint32_t(queued))<=lifetime_ms;
        if(fresh){state.pending=true;state.when=uint32_t(queued);}
        if(state.pending && uint32_t(now-state.when)>lifetime_ms)state.pending=false;
        return {state.pending,state.pending && !native_pressed,fresh};
    }
    void finish(unsigned slot,bool physics_stepped){if(slot<slots && physics_stepped)state_[slot].pending=false;}
private:
    struct State {
        NativeJumpBinding binding{};
        uint64_t sample=~uint64_t(0);
        uint32_t generation=0,cancelled=0,when=0;
        bool pending=false;
    };
    std::array<State,slots> state_{};
    std::array<std::atomic<uint64_t>,slots> mailbox_{};
    std::array<std::atomic<uint32_t>,slots> published_{},cancelled_{};
    uint32_t next_generation_=0; // Guest thread only;0 never denotes an owner.
};
} // namespace ssne::detail
