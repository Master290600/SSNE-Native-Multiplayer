#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace port::coop {
struct NativeLobbyLevel { std::string level; int64_t level_set = 0; int64_t difficulty = 1; uint32_t mode = 1; };
// Owned by the guest thread. A complete map/difficulty choice is a proposal
// until the host explicitly starts. A replacement stays private until complete.
// Native confirmation closes the selector before calling its start callback,
// so a close must not immediately erase ownership.
class LobbyLevelSelection {
public:
    enum class Phase { Idle, Selecting, Closed, Difficulty, Selected, Queued };
    bool begin(uint32_t parent) {
        if (!parent || (phase_ != Phase::Idle && phase_ != Phase::Selected)) return false;
        parent_ = parent; pending_.reset(); phase_ = Phase::Selecting; return true;
    }
    bool owns(uint32_t parent) const { return parent_ == parent && phase_ != Phase::Idle; }
    void closed(uint32_t parent) {
        if (owns(parent) && phase_ == Phase::Selecting) phase_ = Phase::Closed;
    }
    bool confirm(uint32_t parent, NativeLobbyLevel level) {
        if (!owns(parent) || (phase_ != Phase::Selecting && phase_ != Phase::Closed) || level.level.empty()) return false;
        if (selected_ && selected_->mode != level.mode) selected_.reset();
        pending_ = std::move(level); phase_ = Phase::Difficulty; return true;
    }
    bool confirm_difficulty(uint32_t parent, int64_t difficulty) {
        if (!owns(parent) || phase_ != Phase::Difficulty || !pending_ || difficulty < 0 || difficulty > 3) return false;
        pending_->difficulty = difficulty;
        selected_ = std::exchange(pending_, std::nullopt); phase_ = Phase::Selected; return true;
    }
    bool back_from_difficulty(uint32_t parent) {
        if (!owns(parent) || phase_ != Phase::Difficulty) return false;
        pending_.reset(); phase_ = Phase::Selecting; return true;
    }
    const std::optional<NativeLobbyLevel>& selected() const { return selected_; }
    void cancel() {
        if (phase_ == Phase::Queued) return;
        pending_.reset(); phase_ = selected_ ? Phase::Selected : Phase::Idle;
        if (phase_ == Phase::Idle) parent_ = 0;
    }
    void sync_mode(uint32_t mode) {
        if ((selected_ && selected_->mode != mode) || (pending_ && pending_->mode != mode)) reset();
    }
    // Only the explicit Start path may consume this confirmed proposal.
    std::optional<NativeLobbyLevel> take() {
        if (phase_ != Phase::Selected) return std::nullopt;
        phase_ = Phase::Queued; return std::exchange(selected_, std::nullopt);
    }
    Phase phase() const { return phase_; }
    void reset() { parent_ = 0; phase_ = Phase::Idle; selected_.reset(); pending_.reset(); }
private:
    uint32_t parent_ = 0;
    Phase phase_ = Phase::Idle;
    std::optional<NativeLobbyLevel> selected_;
    std::optional<NativeLobbyLevel> pending_;
};
// Guest arms it on opening; UI releases it using the real local held buttons,
// not the deliberately neutral network input submitted while the lobby is open.
class LobbyConfirmRelease {
public:
    void arm() { armed_ = true; }
    void reset() { armed_ = false; }
    uint16_t filter(uint16_t buttons, uint16_t confirmation) {
        if (!armed_.load()) return buttons;
        if (!(buttons & confirmation)) armed_ = false;
        return buttons & ~confirmation;
    }
private:
    std::atomic<bool> armed_{false};
};
}
