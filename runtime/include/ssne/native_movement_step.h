#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace ssne {

struct NativeMovementCommand {
    uint64_t sequence=0;
    int8_t stick_x=0,stick_y=0;
    bool jump_pressed=false;
    float yaw=0.f;
};

struct NativeMovementState {
    std::array<float,3> position{},velocity{};
    uint32_t phase=0,pending_jump=0;
    bool grounded=false,sliding=false;
    // Original walking-controller offsets10/14/18/1C/20/24/28.
    // Jump-only entries0..4 are canonical zero outside the jump-curve phase.
    std::array<float,7> curve{};
    // Original actor offsets708..718: pending lift, contact point, cooldown.
    std::array<float,5> ground{};
    uint32_t contact_kind=0;
    bool operator==(const NativeMovementState&) const = default;
};

struct NativeMovementStepInfo {
    unsigned slot=0;
    uint32_t actor=0,controller=0;
    float dt=0.f;
};

enum class NativeMovementStepAction { Legacy,Wait,Execute };
struct NativeMovementStepDecision {
    NativeMovementStepAction action=NativeMovementStepAction::Legacy;
    NativeMovementCommand command{};
    // The provider has real simulation-time credit and more queued commands.
    bool catch_up=false;
};

using NativeMovementStepPrepare=std::function<NativeMovementStepDecision(const NativeMovementStepInfo&)>;
using NativeMovementStepCompleted=std::function<void(const NativeMovementStepInfo&,const NativeMovementCommand&)>;

// Guest-thread session lifetime only. Prepare reserves a command; completed is
// called only after its original10ms walking step actually returned. Wait
// consumes no input or physics time; Legacy preserves the original input path.
void native_party_set_movement_steps(const void* owner,NativeMovementStepPrepare,NativeMovementStepCompleted);
void native_party_clear_movement_steps(const void* owner);
std::optional<NativeMovementState> native_party_movement_state(uint32_t actor);
// For a remote walking actor, count original physics steps that really ran.
// Scheduled console ticks may wait for commands or consume several at once.
std::optional<uint64_t> native_party_completed_movement_steps(uint32_t actor);

}
