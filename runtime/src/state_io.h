// Save states: byte streams written / read by each runtime module (savestate.cpp drives them).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ssne {

// Background producers take this lock before module locks; they must release
// module locks before waiting here. Only the sole guest thread at Flip may use
// RuntimeStateGuard. Nested guards share one frozen clock and resume generation.
std::recursive_mutex& runtime_state_mutex();
uint64_t runtime_state_generation();
class RuntimeStateGuard {
    std::unique_lock<std::recursive_mutex> lock_;
public:
    RuntimeStateGuard();
    ~RuntimeStateGuard();
    RuntimeStateGuard(const RuntimeStateGuard&) = delete;
    RuntimeStateGuard& operator=(const RuntimeStateGuard&) = delete;
};

struct StateWriter {
    std::vector<uint8_t> buf;
    void bytes(const void* p, size_t n) { auto* b = static_cast<const uint8_t*>(p); buf.insert(buf.end(), b, b + n); }
    template <class T> void pod(const T& v) { bytes(&v, sizeof(T)); }
    void u32(uint32_t v) { pod(v); }
    void u64(uint64_t v) { pod(v); }
    void vec(const std::vector<uint8_t>& v) { u64(v.size()); bytes(v.data(), v.size()); }
    // Large memory block: 4 KiB pages, all-zero pages stored as a flag only.
    void block(const uint8_t* p, size_t n) {
        u64(n);
        for (size_t off = 0; off < n; off += 4096) {
            size_t len = std::min<size_t>(4096, n - off);
            bool zero = true;
            for (size_t i = 0; i < len && zero; i++) zero = p[off + i] == 0;
            uint8_t flag = zero ? 0 : 1;
            pod(flag);
            if (!zero) bytes(p + off, len);
        }
    }
    void tag(const char (&t)[5]) { bytes(t, 4); }
};

struct StateReader {
    const uint8_t* p = nullptr;
    size_t n = 0, pos = 0;
    bool ok = true;
    bool bytes(void* dst, size_t len) {
        if (!ok || pos > n || len > n - pos) { ok = false; return false; }
        if (dst && len) memcpy(dst, p + pos, len);
        pos += len;
        return true;
    }
    template <class T> bool pod(T& v) { return bytes(&v, sizeof(T)); }
    uint32_t u32() { uint32_t v = 0; pod(v); return v; }
    uint64_t u64() { uint64_t v = 0; pod(v); return v; }
    bool vec(std::vector<uint8_t>& v) {
        uint64_t len = u64();
        if (!ok || len > n - pos) { ok = false; return false; }
        v.assign(p + pos, p + pos + len);
        pos += len;
        return true;
    }
    bool skip_vec() {
        const uint64_t len = u64();
        if (!ok || len > n - pos) { ok = false; return false; }
        return bytes(nullptr, size_t(len));
    }
    bool block(uint8_t* dst, size_t expect) {
        uint64_t len = u64();
        if (!ok || len != expect) { ok = false; return false; }
        for (size_t off = 0; off < len; off += 4096) {
            size_t l = std::min<size_t>(4096, len - off);
            uint8_t flag = 0;
            if (!pod(flag) || flag > 1) { ok = false; return false; }
            if (flag) { if (!bytes(dst ? dst + off : nullptr, l)) return false; }
            else if (dst) memset(dst + off, 0, l);
        }
        return ok;
    }
    bool tag(const char (&t)[5]) {
        char got[4];
        if (!bytes(got, 4) || memcmp(got, t, 4)) { ok = false; return false; }
        return true;
    }
};

// Module hooks (each in its own source file). apply=false consumes and checks
// the same layout without changing runtime state or touching persistent cards.
// The coordinator must validate the complete payload before applying any module.
void hw_state_save(StateWriter& w);      // MMIO registers, ARAM, DSP mailbox, AI DMA, VI
bool hw_state_load(StateReader& r, bool apply = true);
void si_state_save(StateWriter& w);
bool si_state_load(StateReader& r, bool apply = true);
void exi_state_save(StateWriter& w);     // EXI channels, IPL/RTC, memory card (contents too)
bool exi_state_load(StateReader& r, bool apply = true);
bool exi_state_load_volatile(StateReader& r); // apply without touching the backing image
bool exi_state_commit(const std::vector<uint8_t>& previous, std::string& error);
bool sched_state_check(std::string& why);  // a state can be taken now (one guest thread, no queued work)
void sched_state_save(StateWriter& w);   // pending interrupts, alarms
bool sched_state_load(StateReader& r, bool apply = true);
void gx_state_save(StateWriter& w);      // GX registers, TMEM, FIFO remainder, PE
bool gx_state_load(StateReader& r, bool apply = true);
void audio_state_reset();                // drop resampler history and queued output

} // namespace ssne
