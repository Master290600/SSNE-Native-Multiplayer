#pragma once
#include <cstdint>

namespace port::coop {
// SDKFlip may run while a native GUI Open is still importing its resources.
// A scene number or an assigned GUI pointer is therefore not startup readiness.
// This guest-thread state follows the lifetime of the initial frontend GUI.
class FrontendStartupReadiness {
    uint64_t generation_=1;
    uint32_t owner_=0;
    unsigned depth_=0;
    bool complete_=false,failed_=false;

    void finish(uint64_t generation,bool initial,bool success) noexcept {
        // Reset invalidates completion tokens, but even an older lifetime's
        // original call must return before any startup action is safe.
        if(depth_)--depth_;
        if(generation!=generation_)return;
        if(!success){failed_=true;complete_=false;}
        if(initial && success && !failed_)complete_=true;
    }
public:
    class Open {
        FrontendStartupReadiness& state_;
        uint64_t generation_;
        bool initial_,success_=false;
    public:
        Open(FrontendStartupReadiness& state,uint32_t owner,bool initial) noexcept
            :state_(state),generation_(state.generation_),initial_(initial && owner!=0){
            if(initial_){
                if(state.owner_ && state.owner_!=owner)state.reset();
                state.owner_=owner;
                // A fresh top-level initial Open may recover from an aborted one.
                if(!state.depth_)state.failed_=false;
                state.complete_=false;
            }
            generation_=state.generation_;
            ++state.depth_;
        }
        Open(const Open&)=delete;
        Open& operator=(const Open&)=delete;
        ~Open(){state_.finish(generation_,initial_,success_);}
        // Readiness remains blocked until this scope has actually returned.
        void completed() noexcept {success_=true;}
    };
    void reset() noexcept {
        ++generation_;owner_=0;complete_=false;failed_=false;
    }
    void destroyed(uint32_t owner) noexcept {if(owner && owner==owner_)reset();}
    bool ready() const noexcept {return owner_ && complete_ && !depth_ && !failed_;}
    uint32_t owner() const noexcept {return owner_;}
};
}
