#pragma once
#include <chrono>
#include <cstdint>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace port {
// Pace BEFORE polling input/recorded images, so a completed image is never
// deliberately held for the rest of the frame budget. Keep an absolute cadence
// across small scheduling overruns, but never burst to catch up after a stall.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    FramePacer() {
#ifdef _WIN32
        // Windows 10 1803+: a precise sleeping timer avoids both millisecond
        // sleep overshoots and a busy-spinning render thread. Older systems
        // retain the portable bounded-sleep fallback below.
        using CreateTimer = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        const auto create = reinterpret_cast<CreateTimer>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW"));
        if (create) timer_ = create(nullptr, nullptr, 0x2, TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
    }
    ~FramePacer() {
#ifdef _WIN32
        if (timer_) CloseHandle(timer_);
#endif
    }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;
    Clock::time_point begin(Clock::time_point now, int fps) {
        if (fps <= 0) { limit_ = 0; return now; }
        const auto period = std::chrono::nanoseconds(1000000000LL / fps);
        if (fps != limit_) { limit_ = fps; next_ = now; }
        auto target = next_;
        if (now >= target + period) target = now;
        if (target < now) target = now;
        // A small oversleep must not shift every later deadline.
        next_ = (now < next_ + period ? next_ : now) + period;
        return target;
    }
    void wait(int fps) {
        const auto target = begin(Clock::now(), fps);
#ifdef _WIN32
        const auto remaining = target - Clock::now();
        if (timer_ && remaining > Clock::duration::zero()) {
            LARGE_INTEGER due;
            due.QuadPart = -(duration_cast<std::chrono::nanoseconds>(remaining).count() + 99) / 100;
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(timer_, DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 10)) == WAIT_OBJECT_0)
                return;
        }
#endif
        const auto coarse = target - std::chrono::microseconds(200);
        if (Clock::now() < coarse) std::this_thread::sleep_until(coarse);
        while (Clock::now() < target) std::this_thread::yield();
    }
private:
    int limit_ = 0;
    Clock::time_point next_{};
#ifdef _WIN32
    HANDLE timer_ = nullptr;
#endif
};

// Counters use elapsed wall time, including event-only iterations while the GPU
// is busy. Input-update dt is not a valid FPS denominator in that case.
class FrameRateSample {
public:
    using Clock = std::chrono::steady_clock;
    bool update(Clock::time_point now, uint64_t native, uint64_t presented, uint64_t game_updates=0) {
        if (!started_) { started_ = true; at_ = now; native_ = native; presented_ = presented; updates_ = game_updates; return false; }
        const double elapsed = std::chrono::duration<double>(now - at_).count();
        if (elapsed < 1.) return false;
        ticks = double(native - native_) / elapsed;
        renders = double(presented - presented_) / elapsed;
        updates = double(game_updates - updates_) / elapsed;
        at_ = now; native_ = native; presented_ = presented; updates_ = game_updates;
        return true;
    }
    double ticks = 0, renders = 0, updates = 0;
private:
    bool started_ = false;
    Clock::time_point at_{};
    uint64_t native_ = 0, presented_ = 0, updates_ = 0;
};

inline bool smooth_network_frames(bool network, bool background) {
    // Local camera prediction is useful at 60 Hz too, when native rendering or
    // snapshot delivery is slower. The simulation keeps its original clock.
    return network && !background;
}
}
