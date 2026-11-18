#pragma once
#include <cstdint>
#include <optional>
#include <utility>
#include <type_traits>
namespace ssne {
// Guest-thread scheduling only. Eligibility is freshly checked at all three
// boundaries; native simulation and the callback/context remain caller-owned.
class HostFrameDispatchPolicy {
public:
    // Call for every render, including a legacy/early-return render. Never
    // carry an unconsumed ticket or an old render permission into a new frame.
    void begin_render(bool eligible) noexcept {
        pending_.reset();render_eligible_=eligible;
    }
    // Call once at the corresponding native flip. False selects the existing
    // pre-flip observer path; the caller must drain any outstanding RAM lease.
    bool schedule(uint64_t frame_token,bool eligible) noexcept {
        pending_.reset();
        const bool defer=std::exchange(render_eligible_,false) && eligible;
        if(defer)pending_=frame_token;
        return defer;
    }
    // A changed gate cancels only this ticket. Even a rejected/consumed ticket
    // cannot run later when eligibility returns, and no work was consumed here.
    std::optional<uint64_t> take(bool eligible) noexcept {
        const auto ticket=std::exchange(pending_,std::nullopt);
        return eligible?ticket:std::nullopt;
    }
    // Clear before replacing/removing the observer or its owning session.
    void clear() noexcept {render_eligible_=false;pending_.reset();}
    bool pending() const noexcept {return pending_.has_value();}
private:
    bool render_eligible_=false;
    std::optional<uint64_t> pending_;
};
// Keep the scheduler's exact CPU object identity while invoking an observer
// with its original flip-entry registers. Only CPU state is restored; guest
// RAM changes made by the callback intentionally remain in effect.
template<class Context,class Callback>
void with_preserved_context(Context& current,const Context& entry,Callback&& callback){
    static_assert(std::is_nothrow_copy_assignable_v<Context>);
    struct Restore {
        Context& target;
        Context sdk_return;
        ~Restore() noexcept {target=sdk_return;}
    } restore{current,current};
    current=entry;
    std::forward<Callback>(callback)(current);
}
} // namespace ssne
