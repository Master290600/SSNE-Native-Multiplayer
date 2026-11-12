#pragma once
#include <functional>
#include <string>
#include <vector>
#include "ssne/ppc_context.h"
namespace ssne {
// Sole guest thread at Flip only. The coordinator freezes runtime producers.
// These hooks do not yet define a complete network-session checkpoint.
struct RuntimeCheckpointHooks {
    std::function<bool(std::vector<uint8_t>&,std::string&)> capture;
    // Called after target RAM, or again with the backup after failed application.
    // May prepare reversible state; must not send packets or persist files.
    std::function<bool(const std::vector<uint8_t>&,std::string&)> restore;
    // Last, after persistent commit. Must perform only prepared, no-fail swaps.
    std::function<void()> commit;
    // After restoring the backup, release prepared candidates/locks. No publish.
    std::function<void()> abort;
};
// Run a multi-part operation under one uninterrupted producer/clock barrier.
// A busy runtime returns false without calling the operation; retry next Flip.
bool runtime_checkpoint_visit(const std::function<void()>& operation,std::string& error);
bool runtime_checkpoint_capture(PPCContext&,std::vector<uint8_t>& runtime,
    std::vector<uint8_t>& external,const RuntimeCheckpointHooks&,std::string& error);
bool runtime_checkpoint_restore(PPCContext&,const std::vector<uint8_t>& runtime,
    const std::vector<uint8_t>& external,const RuntimeCheckpointHooks&,std::string& error);
}
