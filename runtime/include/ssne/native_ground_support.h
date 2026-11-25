#pragma once
#include "ssne/native_party.h"
#include <span>

namespace ssne {
// PAL walking controller's short downward sphere cast. A support position is
// the resolved sphere centre, and its normal points into the supporting surface.
bool native_ground_support(PPCContext&,const std::array<float,3>& centre,float radius,
                           std::optional<NativeSegmentHit>& support,std::string& error);

// The five PAL actor fields at 708..718 retain a pending curb lift, its contact
// point and the cooldown used by the next walking update. This state belongs to
// the local preview, independently of the replicated gameplay actor.
struct NativeGroundMotionState {
    std::array<float,5> step{};
};
struct NativeGroundMotionResult {
    std::array<float,3> position{};
    bool grounded=false;
    bool sliding=false;
    uint32_t contact_kind=0;
    bool player_contact=false;
};
// Run PAL's walking collision controller against a scratch actor. Touch callbacks
// are suppressed and solid non-static contacts decline this preview. A declined
// dynamic contact has an empty error; malformed contexts return a specific error.
// A zero dt is a support/correction query and leaves the step timer untouched.
bool native_static_ground_motion(PPCContext&,const std::array<float,3>& from,
                                const std::array<float,3>& desired,float radius,float dt,
                                NativeGroundMotionState&,NativeGroundMotionResult&,
                                std::string& error);
// The same original PAL collision call for command replay. Only phase0 uses
// the support-cast/minimum-motion rules; jump/fall calls pass walking=false.
bool native_command_ground_motion(PPCContext&,const std::array<float,3>& from,
                                 const std::array<float,3>& desired,float radius,float dt,bool walking,
                                 NativeGroundMotionState&,NativeGroundMotionResult&,
                                 std::string& error,
                                 std::span<const std::optional<std::array<float,3>>> peers={});
}
