#pragma once
#include <SDL_opengl.h>
#include <array>
#include <cstddef>

namespace port {
// CPU frame queues do not bound work already submitted to an asynchronous GL
// driver. Poll without waiting so Windows events still run when the GPU is late.
class RenderBackpressure {
public:
    enum class State { Ready, Busy, Failed };
    bool initialize(void* (*getproc)(const char*)) {
        fence_ = reinterpret_cast<PFNGLFENCESYNCPROC>(getproc("glFenceSync"));
        wait_ = reinterpret_cast<PFNGLCLIENTWAITSYNCPROC>(getproc("glClientWaitSync"));
        erase_ = reinterpret_cast<PFNGLDELETESYNCPROC>(getproc("glDeleteSync"));
        flush_ = reinterpret_cast<void (APIENTRY*)()>(getproc("glFlush"));
        enabled_ = fence_ && wait_ && erase_ && flush_;
        return enabled_;
    }
    State poll() {
        if (failed_) return State::Failed;
        if (!enabled_) return State::Ready;
        while (count_) {
            const auto status = wait_(pending_[first_], 0, 0);
            if (status == GL_TIMEOUT_EXPIRED) break;
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
                failed_ = true;
                return State::Failed;
            }
            erase_(pending_[first_]);
            pending_[first_] = nullptr;
            first_ = (first_ + 1) % pending_.size();
            --count_;
        }
        return count_ == pending_.size() ? State::Busy : State::Ready;
    }
    bool submitted() {
        if (failed_) return false;
        if (!enabled_) return true;
        if (count_ == pending_.size()) { failed_ = true; return false; }
        const auto sync = fence_(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sync) { failed_ = true; return false; }
        pending_[(first_ + count_) % pending_.size()] = sync;
        ++count_;
        // Explicitly flush even with vsync disabled or a minimized window.
        flush_();
        return true;
    }
    std::size_t pending() const { return count_; }
    void reset() { // Call while this window's GL context is still current.
        while (count_) {
            erase_(pending_[first_]);
            pending_[first_] = nullptr;
            first_ = (first_ + 1) % pending_.size();
            --count_;
        }
        failed_ = false;
    }
private:
    PFNGLFENCESYNCPROC fence_ = nullptr;
    PFNGLCLIENTWAITSYNCPROC wait_ = nullptr;
    PFNGLDELETESYNCPROC erase_ = nullptr;
    void (APIENTRY* flush_)() = nullptr;
    std::array<GLsync, 2> pending_{};
    std::size_t first_ = 0, count_ = 0;
    bool enabled_ = false, failed_ = false;
};
}
