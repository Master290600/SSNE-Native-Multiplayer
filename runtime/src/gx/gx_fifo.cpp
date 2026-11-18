// GX command processor: parses the FIFO / display-list command stream and drives the pipeline.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

#include "gx_gpu_frame.h"
#include "gx_decode_timeline.h"
#include "gx_crosshair.h"
#include "gx_internal.h"
#include "../state_io.h"
#include "ssne/gx.h"
#include "ssne/ppc_context.h"
#include "ssne/scheduler.h"

namespace ssne {

namespace gx {
State g;
Counters counters;
}

namespace {

using namespace gx;

std::mutex G;   // command processing state (g_pending, register state, the renderers' recording)
std::mutex PM;  // g_pe: written by the command processing (draw done, tokens), read / acknowledged by the game
std::vector<uint8_t> g_pending;
// Context metadata is separate from FIFO bytes. The producer never takes G
// while publishing a scope marker, so it cannot wait on a whole decoder job.
constexpr size_t kContextMarkerLimit=4096;
std::mutex g_context_mutex;
std::condition_variable g_context_space;
detail::GxDecodeMarkerQueue<kContextMarkerLimit> g_context_marks;
detail::GxDecodeTimeline g_decode_timeline; // G protects parser-owned history
// Context clock is protected by g_context_mutex; raw ring counters never rewind.
detail::GxDecodeOffsetClock g_context_clock;
detail::GxDecodeChoice g_producer_choice;
GxDecodeMarkerDebug g_producer_debug;
uint64_t g_producer_context_offset=0,g_pending_begin_abs=0;
bool decode_debug_enabled(){
#ifdef SSNE_GX_FIFO_CONTEXT_TEST
    return true; // Exercise snapshots in the direct-TU parser regression.
#else
    static const bool enabled=[] {const auto* v=std::getenv("SSNE_GX_CONTEXT_VERIFY");return v&&!std::strcmp(v,"1");}();return enabled;
#endif
}
thread_local const detail::GxDecodeChange* g_decode_source=nullptr;
thread_local uint64_t g_decode_opcode_start=0;
thread_local uint32_t g_decode_opcode=0,g_decode_depth=0;
uint64_t g_last_fence_head=0,g_last_fence_begin=0,g_last_fence_bytes=0,g_last_fence_need=0; // G

thread_local const GxDecodeContext* g_decode_override=nullptr;
struct DecodeContextScope {
    bool installed=false,debug=false;const GxDecodeContext* previous=nullptr;
    const detail::GxDecodeChange* previous_source=nullptr;
    uint64_t previous_start=0;uint32_t previous_opcode=0,previous_depth=0;
    DecodeContextScope(bool enabled,const detail::GxDecodeChoice& choice,const detail::GxDecodeChange* source,uint64_t start,uint8_t opcode,int depth):installed(enabled),debug(decode_debug_enabled()){
        if(installed){previous=g_decode_override;g_decode_override=choice.marked?&choice.context:nullptr;}
        if(debug){previous_source=g_decode_source;previous_start=g_decode_opcode_start;previous_opcode=g_decode_opcode;previous_depth=g_decode_depth;
            if(installed)g_decode_source=source;
            g_decode_opcode_start=start;g_decode_opcode=opcode;g_decode_depth=uint32_t(depth);}
    }
    ~DecodeContextScope(){if(installed)g_decode_override=previous;if(debug){g_decode_source=previous_source;g_decode_opcode_start=previous_start;g_decode_opcode=previous_opcode;g_decode_depth=previous_depth;}}
};
#ifdef SSNE_GX_FIFO_CONTEXT_TEST
using DecodeTestObserver=bool(*)(uint8_t,uint64_t,int);
DecodeTestObserver g_decode_test_observer=nullptr;
bool observe_decode(uint8_t opcode,uint64_t offset,int depth){return g_decode_test_observer&&g_decode_test_observer(opcode,offset,depth);}
#endif
void ingest_context_marks(uint64_t raw_head){
    // At most one partial command remains after process_pending. Its compacted
    // timeline has<=2 entries, so one full marker batch always fits this reserve.
    g_decode_timeline.reserve(kContextMarkerLimit+4);
    bool consumed=false;
    {
        std::lock_guard lock(g_context_mutex);const auto logical_head=g_context_clock.logical(raw_head);
        while(g_context_marks.size()&&g_context_marks.front().offset<=logical_head){
            if(!g_decode_timeline.append(g_context_marks.front())){std::fprintf(stderr,"GX context marker order invalid\n");std::abort();}
            g_context_marks.pop();consumed=true;
        }
    }
    if(consumed)g_context_space.notify_all();
}
void compact_context_pending(){g_decode_timeline.compact_pending(g_pending_begin_abs,g_pending_begin_abs+g_pending.size());}

// Bytes g_pending must hold before parse() can make progress (0 = unknown, parse on the next write). The game writes
// a draw command 4 bytes at a time; without this, every write re-parsed the incomplete command just to find it still
// incomplete. Only the head of g_pending is ever unparsed, and the CP registers that size it (VCD/VAT) only change
// through parsed commands or a state load, so the requirement stays valid until it is met.
size_t g_need = 0;
uint16_t g_pe[0x40];
std::atomic<uint64_t> s_bytes{0}, s_cmds{0}, s_prims{0}, s_verts{0}, s_dls{0}, s_copies{0}, s_done{0}, s_tokens{0};
std::atomic<uint32_t> s_last_token{0}, s_unknown{0};
std::vector<Vtx> g_vtx;  // scratch

void raise_pe(bool finish) {
    std::lock_guard<std::mutex> lk(PM);
    uint16_t& ctrl = g_pe[0x0A >> 1];
    if (finish) {
        ctrl |= 0x8;
        s_done++;
        if (ctrl & 0x2) raise_interrupt(IRQ_PE_FINISH);
    } else {
        ctrl |= 0x4;
        if (ctrl & 0x1) raise_interrupt(IRQ_PE_TOKEN);
    }
}

void load_bp(uint32_t v) {
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;
    if (reg == 0xFE) { g.bp_mask = val; return; }
    uint32_t old = g.bp[reg];
    g.bp[reg] = (old & ~g.bp_mask) | (val & g.bp_mask);
    g.bp_mask = 0xFFFFFF;
    if (g.bp[reg] != old) g_cfg_dirty = true;  // (writes with side effects on textures invalidate them, which also sets it)
    switch (reg) {
    case 0x45: raise_pe(true); break;                                       // PE_DONE
    case 0x47: { std::lock_guard<std::mutex> lk(PM); g_pe[0x0E >> 1] = uint16_t(val); } s_tokens++; s_last_token = val & 0xFFFF; break;
    case 0x48: { std::lock_guard<std::mutex> lk(PM); g_pe[0x0E >> 1] = uint16_t(val); } s_tokens++; s_last_token = val & 0xFFFF; raise_pe(false); break;
    case 0x52: s_copies++; efb_copy(g.bp[0x52]); break;                     // EFB copy (display / texture)
    case 0x65: load_tlut(g.bp[0x65]); break;                                // load TLUT into TMEM
    default: bp_written(reg, g.bp[reg]); break;
    }
}

inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void load_xf(const uint8_t* p) {
    uint32_t h = rd32(p);
    uint32_t count = (h >> 16) + 1, addr = h & 0xFFFF;
    g_xf_dirty = true;
    for (uint32_t k = 0; k < count; k++, addr++) {
        uint32_t v = rd32(p + 4 + 4 * k);
        if (addr < 0x1000) g.xf_mem[addr] = v;
        else if (addr < 0x1100 && g.xf_reg[addr - 0x1000] != v) { g.xf_reg[addr - 0x1000] = v; g_cfg_dirty = true; g_vsetup_dirty = true; }
    }
}

size_t parse(const uint8_t* p, size_t n, int depth);

void call_display_list(uint32_t addr, uint32_t size, int depth) {
    if (depth > 4 || size > 0x400000) return;
    std::vector<uint8_t> dl(size);
    // Same bytes as mem_read8 one by one: a list in plain RAM (no MMIO / locked cache, no wrap) is copied at once.
    uint64_t end = uint64_t(addr) + size;
    if (size && end <= 0x100000000ull && (addr >> 24) == ((end - 1) >> 24) && !ssne::is_mmio(addr) &&
        (addr >> 28) != 0xE && (addr & (ssne::kRamSize - 1)) + uint64_t(size) <= ssne::kRamSize)
        memcpy(dl.data(), ssne::host_ptr(addr), size);
    else
        for (uint32_t i = 0; i < size; i++) dl[i] = mem_read8(addr + i);
    s_dls++;
    parse(dl.data(), dl.size(), depth + 1);
}

size_t parse(const uint8_t* p, size_t n, int depth) {
    size_t i = 0;
    while (i < n) {
        const bool scoped=depth==0&&gx_scoped_context_enabled();
        detail::GxDecodeChoice choice;
        const detail::GxDecodeChange* source=nullptr;
        if(scoped){if(decode_debug_enabled()){source=g_decode_timeline.find(g_pending_begin_abs+i);if(source)choice=source->choice;}else choice=g_decode_timeline.at(g_pending_begin_abs+i);}
        uint8_t op = p[i];
        // Nested display lists inherit the parent marker. Their bytes have no
        // top-level FIFO offset, so diagnostics report UINT64_MAX and depth.
        DecodeContextScope decode_scope(scoped,choice,source,depth==0?g_pending_begin_abs+i:UINT64_MAX,op,depth);
        size_t need;
        switch (op) {
        case 0x00: need = 1; break;
        case 0x08: need = 6; break;
        case 0x10: {
            if (n - i < 5) return i;
            need = 5 + 4 * ((rd32(p + i + 1) >> 16) + 1);
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: need = 5; break;
        case 0x40: need = 9; break;
        case 0x44: need = 1; break;
        case 0x48: need = 1; break;
        case 0x61: need = 5; break;
        case gx::kCrosshairOpcode:
            if(depth==0){if(n-i<2)return i;need=p[i+1]==gx::kCrosshairMagic?gx::kCrosshairTokenBytes:1;}else need=1;
            break;
        default:
            if ((op & 0x80) && op <= 0xBF) {
                if (n - i < 3) return i;
                uint32_t count = uint32_t(p[i + 1]) << 8 | p[i + 2];
                int vat = op & 7;
                need = 3 + size_t(count) * vertex_size(vat);
                if (n - i < need) return i;
#ifdef SSNE_GX_FIFO_CONTEXT_TEST
                if(observe_decode(op,depth==0?g_pending_begin_abs+i:UINT64_MAX,depth)){i+=need;s_cmds++;continue;}
#endif
                s_prims++;
                s_verts += count;
                static const bool gpu_tl = gpu_vertex_shading();
                const bool crosshair=custom_crosshair_stream_active();
                auto cpu_draw=[&]{decode_vertices(vat,p+i+3,count,g_vtx);if(crosshair)custom_crosshair_prepare(op&0xf8,g_vtx);draw(op&0xf8,g_vtx);if(crosshair)custom_crosshair_finish_draw();};
                if (gpu_enabled() && gpu_compare_on()) {  // testing: record with both vertex paths
                    bool dirty = g_cfg_dirty;
                    gpu_select_ctx(0);
                    cpu_draw();
                    g_cfg_dirty = dirty;
                    gpu_select_ctx(1);
                    if (crosshair || !gpu_draw(op & 0xF8, vat, p + i + 3, count)) cpu_draw();
                    gpu_select_ctx(0);
                } else if (!(gpu_tl && gpu_enabled() && !crosshair && gpu_draw(op & 0xF8, vat, p + i + 3, count))) {
                    cpu_draw();
                }
                i += need;
                s_cmds++;
                continue;
            }
            s_unknown = op;
            need = 1;
        }
        if (n - i < need) return i;
#ifdef SSNE_GX_FIFO_CONTEXT_TEST
        if(observe_decode(op,depth==0?g_pending_begin_abs+i:UINT64_MAX,depth)){i+=need;s_cmds++;continue;}
#endif
        switch (op) {
        case 0x08: g.cp[p[i + 1]] = rd32(p + i + 2); g_vsetup_dirty = true; break;
        case 0x10: load_xf(p + i + 1); break;
        case 0x20: load_indexed_xf(12, rd32(p + i + 1)); break;
        case 0x28: load_indexed_xf(13, rd32(p + i + 1)); break;
        case 0x30: load_indexed_xf(14, rd32(p + i + 1)); break;
        case 0x38: load_indexed_xf(15, rd32(p + i + 1)); break;
        case 0x40: call_display_list(rd32(p + i + 1), rd32(p + i + 5), depth); break;
        case 0x61: load_bp(rd32(p + i + 1)); break;
        case gx::kCrosshairOpcode:
            if(depth==0&&need==gx::kCrosshairTokenBytes)custom_crosshair_marker((uint64_t(rd32(p+i+2))<<32)|rd32(p+i+6));
            else s_unknown=op;
            break;
        default: break;
        }
        i += need;
        s_cmds++;
    }
    return i;
}

// Size parse() needs to make progress on an unparsed head command `p[0..n)` (see g_need); 0 = parse anyway.
size_t head_need(const uint8_t* p, size_t n) {
    if (!n) return 0;
    uint8_t op = p[0];
    if(op==gx::kCrosshairOpcode)return n<2?2:p[1]==gx::kCrosshairMagic?gx::kCrosshairTokenBytes:0;
    if (op == 0x10) return n < 5 ? 5 : 5 + 4 * size_t((rd32(p + 1) >> 16) + 1);
    if ((op & 0x80) && op <= 0xBF) return n < 3 ? 3 : 3 + size_t(uint32_t(p[1]) << 8 | p[2]) * vertex_size(op & 7);
    return 0;
}

// Parses what g_pending holds (caller holds G).
void process_pending() {
    const bool scoped=gx_scoped_context_enabled();
    if (g_pending.size() < g_need) {if(scoped)compact_context_pending();return;}
    size_t used = parse(g_pending.data(), g_pending.size(), 0);
    if (used) {g_pending.erase(g_pending.begin(), g_pending.begin() + long(used));if(scoped)g_pending_begin_abs+=used;}
    g_need = head_need(g_pending.data(), g_pending.size());
    if(scoped)compact_context_pending();
}

// ---- GX thread. The game's writes to the pipe go into a ring buffer (single producer: the game's thread, which
// runs the guest threads one at a time); a separate thread parses them and drives the pipeline (vertex processing,
// the renderers' recording, PE interrupts). The game's code and the command processing then run in parallel, as
// CPU and GPU do on the console: the game synchronizes with the GPU the same way it does there (draw done / token
// interrupts, PE register polling), and it already has to keep the memory the GPU reads (display lists, vertex
// arrays, textures) intact until then. gx_sync() catches up fully for save states and for game-side state the
// processing reads. Sleeping uses mutex + condition variable (not C++20 atomic waits, which on Windows can need
// Windows 8 functions).
constexpr uint64_t kRingSize = uint64_t(1) << 22, kRingMask = kRingSize - 1;
uint8_t* g_ring = nullptr;
bool g_threaded = getenv("SSNE_GX_SYNC") == nullptr;
bool g_started = false;                  // game thread: the GX thread runs (decided on the first write)
std::atomic<uint64_t> g_head{0};         // bytes written by the game
std::atomic<uint64_t> g_taken{0};        // bytes the GX thread has taken out of the ring
std::atomic<uint64_t> g_done{0};         // bytes the GX thread has processed
std::atomic<bool> g_gx_sleeping{false}, g_sync_waiting{false}, g_stop{false};
std::mutex WM;                           // wake-ups (with the two condition variables)
std::condition_variable g_wake_cv, g_done_cv;
std::thread* g_gx_thread = nullptr;
void enqueue_context_choice(const detail::GxDecodeChoice& choice,const GxDecodeMarkerDebug& debug={}){
    std::unique_lock lock(g_context_mutex);
    if(choice==g_producer_choice&&debug.serial==g_producer_debug.serial)return;
    const auto offset=g_context_clock.logical(g_head.load(std::memory_order_relaxed));
    if(offset<g_producer_context_offset){std::fprintf(stderr,"GX producer context clock moved backwards\n");std::abort();}
    const detail::GxDecodeChange change{offset,choice,debug,offset};
    while(!g_context_marks.push(change)){
        if(g_stop.load(std::memory_order_relaxed))return;
        // Distinct marker offsets imply intervening published bytes. The worker
        // consumes metadata even when those bytes end in a partial command.
        // Same-offset marks coalesce and cannot exhaust the bounded ring.
        g_wake_cv.notify_one();g_context_space.wait_for(lock,std::chrono::milliseconds(10));
    }
    g_producer_choice=choice;g_producer_debug=debug;g_producer_context_offset=offset;
}
void record_context_fence(uint64_t head){
    if(!decode_debug_enabled())return;
    std::lock_guard lock(G);g_last_fence_head=head;g_last_fence_begin=g_pending_begin_abs;g_last_fence_bytes=g_pending.size();g_last_fence_need=g_need;
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

void gx_thread_main() {
    uint64_t taken = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        uint64_t h = g_head.load(std::memory_order_acquire);
        if (h == taken) {
            // Writes come in bursts: wait a little before sleeping (a wake-up costs more than a short spin).
            for (int i = 0; i < 4000 && h == taken; i++) { cpu_relax(); h = g_head.load(std::memory_order_acquire); }
            if (h == taken) {
                std::unique_lock<std::mutex> lk(WM);
                g_gx_sleeping.store(true);
                while ((h = g_head.load()) == taken && !g_stop.load()) g_wake_cv.wait(lk);
                g_gx_sleeping.store(false);
                if (g_stop.load()) return;
            }
        }
        {
            std::lock_guard<std::mutex> lk(G);
            size_t old = g_pending.size(), n = size_t(h - taken), at = size_t(taken & kRingMask);
            g_pending.resize(old + n);
            size_t first = std::min<size_t>(n, size_t(kRingSize) - at);
            memcpy(g_pending.data() + old, g_ring + at, first);
            if (n > first) memcpy(g_pending.data() + old + first, g_ring, n - first);
            g_taken.store(h, std::memory_order_release);
            if(gx_scoped_context_enabled())ingest_context_marks(h);
            process_pending();
        }
        taken = h;
        g_done.store(h);
        if (g_sync_waiting.load()) {
            std::lock_guard<std::mutex> lk(WM);
            g_done_cv.notify_all();
        }
    }
}

void stop_gx_thread() {
    if (!g_gx_thread) return;
    {
        std::lock_guard<std::mutex> lk(WM);
        g_stop.store(true);
    }
    g_wake_cv.notify_all();g_context_space.notify_all();
    g_gx_thread->join();
}

void start_gx_thread() {
    g_started = true;
    if (!g_threaded) return;
    g_ring = new uint8_t[kRingSize];
    g_gx_thread = new std::thread(gx_thread_main);
    atexit(stop_gx_thread);  // before the state it uses is destroyed
}

} // namespace

bool gx_scoped_context_enabled(){
    static const bool enabled=[] {const auto* value=std::getenv("SSNE_GX_SCOPED_CONTEXT");return !value || std::strcmp(value,"1")==0;}();
    return enabled;
}
void gx_mark_decode_context(const GxDecodeContext& context){if(gx_scoped_context_enabled())enqueue_context_choice({true,context});}
void gx_mark_decode_context_debug(const GxDecodeContext& context,const GxDecodeMarkerDebug& debug){if(gx_scoped_context_enabled())enqueue_context_choice({true,context},debug);}
GxDecodeDebugState gx_decode_debug_state(){
    // Decoder-only: G is already held, including for recursive display lists.
    GxDecodeDebugState state;if(g_decode_source){state.marker=g_decode_source->debug;state.marker_offset=g_decode_source->marker_offset;}
    state.opcode_start=g_decode_opcode_start;state.opcode=g_decode_opcode;state.depth=g_decode_depth;
    state.raw_head=g_head.load();state.raw_taken=g_taken.load();state.raw_done=g_done.load();
    state.pending_begin=g_pending_begin_abs;state.pending_bytes=g_pending.size();state.pending_need=g_need;
    state.last_fence_head=g_last_fence_head;state.last_fence_pending_begin=g_last_fence_begin;state.last_fence_pending_bytes=g_last_fence_bytes;state.last_fence_need=g_last_fence_need;return state;
}
void gx_clear_decode_context(){if(gx_scoped_context_enabled())enqueue_context_choice({});}
const GxDecodeContext* gx_decode_context_override(){return g_decode_override;}

void gx_set_threaded(bool on) {
    if (!g_started) g_threaded = on && getenv("SSNE_GX_SYNC") == nullptr;
}

void gx_sync() {
    // Capture even the early-complete path: g_done includes partial headers.
    struct FenceSnapshot {uint64_t head;~FenceSnapshot(){record_context_fence(head);}} snapshot{g_head.load()};
    if (!g_gx_thread) return;
    uint64_t h = snapshot.head;
    if (g_done.load() >= h) return;
    for (int i = 0; i < 4000; i++) {
        if (g_done.load(std::memory_order_acquire) >= h) return;
        cpu_relax();
    }
    std::unique_lock<std::mutex> lk(WM);
    g_sync_waiting.store(true);
    while (g_done.load() < h && !g_stop.load()) g_done_cv.wait(lk);
    g_sync_waiting.store(false);
}

void gx_fifo_write(uint32_t value, int size) {
    if (!g_started) start_gx_thread();
    s_bytes += size;
    if (!g_gx_thread) {
        std::lock_guard<std::mutex> lk(G);
        size_t old = g_pending.size();
        g_pending.resize(old + size_t(size));
        for (int k = 0; k < size; k++) g_pending[old + size_t(k)] = uint8_t(value >> (8 * (size - 1 - k)));
        if(gx_scoped_context_enabled()){
            const auto h=g_head.load(std::memory_order_relaxed)+uint64_t(size);
            g_head.store(h,std::memory_order_release);g_taken.store(h,std::memory_order_release);
            ingest_context_marks(h);process_pending();g_done.store(h,std::memory_order_release);
        }else process_pending();
        return;
    }
    uint64_t h = g_head.load(std::memory_order_relaxed);
    while (h + uint64_t(size) - g_taken.load(std::memory_order_acquire) > kRingSize) {  // full (rare): wait for room
        if (g_stop.load()) return;
        std::this_thread::yield();
    }
    for (int k = 0; k < size; k++) g_ring[(h + uint64_t(k)) & kRingMask] = uint8_t(value >> (8 * (size - 1 - k)));
    g_head.store(h + uint64_t(size));
    if (g_gx_sleeping.load()) {
        std::lock_guard<std::mutex> lk(WM);
        g_wake_cv.notify_one();
    }
    // Draw done (GXSetDrawDone writes BP 0x45 as the byte 0x61, then 0x45xxxxxx): the game waits for it next
    // (GXDrawDone, about twice per frame). Wait here, in host code, as the synchronous processing did. Waiting in the
    // game's GXDrawDone instead lets its retrace callback run, and the game's GPU watchdog counts retraces spent there:
    // after 30 it reports "System looks hung", after 600 it resets the GPU (with uncapped frame rates, retraces come
    // fast). Something else that happens to look like this only costs an extra wait.
    static bool after_bp_opcode = false;
    bool draw_done = after_bp_opcode && size == 4 && (value >> 24) == 0x45;
    after_bp_opcode = size == 1 && (value & 0xFF) == 0x61;
    if (draw_done) gx_sync();
}

// No catching up with the GX thread here: like the console's PE registers, these show the events the command
// processing has reached so far (the game waits for them through interrupts or by polling).
uint16_t gx_pe_read16(uint32_t off) { std::lock_guard<std::mutex> lk(PM); return g_pe[(off & 0x7F) >> 1]; }

void gx_pe_write16(uint32_t off, uint16_t v) {
    std::lock_guard<std::mutex> lk(PM);
    uint16_t& r = g_pe[(off & 0x7F) >> 1];
    if ((off & 0x7F) == 0x0A) {
        uint16_t status = r & 0xC;
        r = uint16_t((v & 0x3) | (status & ~(v & 0xC)));
        return;
    }
    r = v;
}

uint16_t gx_cp_read16(uint32_t off) { return (off & 0xFF) == 0 ? 0x000E : 0; }
void gx_cp_write16(uint32_t, uint16_t) {}

GxStats gx_stats() {
    return {s_bytes.load(), s_cmds.load(), s_prims.load(), s_verts.load(), s_dls.load(),
            s_copies.load(), s_done.load(), s_tokens.load(), s_last_token.load(), s_unknown.load()};
}

void gx_state_save(StateWriter& w) {
    gx_sync();
    std::lock_guard<std::mutex> lk(G);
    std::lock_guard<std::mutex> lp(PM);
    const bool scoped=gx_scoped_context_enabled();
    std::vector<detail::GxDecodeChange> changes;
    if(scoped){ingest_context_marks(g_head.load());compact_context_pending();changes=g_decode_timeline.pending_state(g_pending_begin_abs,g_pending_begin_abs+g_pending.size());}
    // Unmarked/live-only decoding needs no extension. Preserve byte-compatible
    // GX01 for SP and other modes, including when support is enabled by default.
    // A known pending prefix OR a known next-opcode choice still needs GX02.
    const bool known_context=std::any_of(changes.begin(),changes.end(),[](const auto& change){return change.choice.marked;});
    w.tag(known_context?"GX02":"GX01");
    w.bytes(g.cp, sizeof(g.cp)); w.bytes(g.xf_mem, sizeof(g.xf_mem)); w.bytes(g.xf_reg, sizeof(g.xf_reg));
    w.bytes(g.bp, sizeof(g.bp)); w.u32(g.bp_mask);
    w.block(g.tmem, sizeof(g.tmem));
    w.vec(g_pending);
    w.bytes(g_pe, sizeof(g_pe));
    int16_t regs[4][4], konst[4][4];
    tev_regs_access(regs, konst, false);
    w.bytes(regs, sizeof(regs)); w.bytes(konst, sizeof(konst));
    if(known_context){
        w.u32(1);w.u32(uint32_t(changes.size()));
        for(const auto& change:changes){w.u64(change.offset);w.u32(detail::gx_decode_choice_bits(change.choice));}
    }
}

bool gx_state_load(StateReader& r, bool apply) {
    gx_sync();
    std::lock_guard<std::mutex> lk(G);
    std::lock_guard<std::mutex> lp(PM);
    const bool scoped_state=r.ok&&r.pos<=r.n&&r.n-r.pos>=4&&std::memcmp(r.p+r.pos,"GX02",4)==0;
    if(scoped_state){
        if(!gx_scoped_context_enabled()){std::fprintf(stderr,"GX02 context state requires SSNE_GX_SCOPED_CONTEXT=1\n");return false;}
        if(!r.tag("GX02"))return false;
    }else if (!r.tag("GX01")) return false;
    r.bytes(apply ? g.cp : nullptr, sizeof(g.cp)); r.bytes(apply ? g.xf_mem : nullptr, sizeof(g.xf_mem)); r.bytes(apply ? g.xf_reg : nullptr, sizeof(g.xf_reg));
    r.bytes(apply ? g.bp : nullptr, sizeof(g.bp)); const auto mask = r.u32(); if (apply) g.bp_mask = mask;
    if (!r.block(apply ? g.tmem : nullptr, sizeof(g.tmem))) return false;
    const auto pending_bytes=r.u64();
    if(!r.ok||pending_bytes>r.n-r.pos)return false;
    std::vector<uint8_t> loaded_pending;
    if(apply){loaded_pending.resize(size_t(pending_bytes));if(!r.bytes(loaded_pending.data(),loaded_pending.size()))return false;}
    else if(!r.bytes(nullptr,size_t(pending_bytes)))return false;
    r.bytes(apply ? g_pe : nullptr, sizeof(g_pe));
    int16_t regs[4][4], konst[4][4];
    r.bytes(regs, sizeof(regs)); r.bytes(konst, sizeof(konst));
    std::vector<detail::GxDecodeChange> loaded_context{{0,{}}};
    if(scoped_state){
        if(r.u32()!=1)return false;const auto count=r.u32();if(!r.ok||count<1||count>2)return false;
        loaded_context.clear();loaded_context.reserve(count);
        for(unsigned i=0;i<count;++i){detail::GxDecodeChange change;change.offset=r.u64();if(!detail::gx_decode_choice_from_bits(r.u32(),change.choice))return false;loaded_context.push_back(change);}
        if(!detail::valid_gx_decode_pending_state(loaded_context,pending_bytes))return false;
    }
    if (!r.ok) return false;
    if (!apply) return true;
    g_pending=std::move(loaded_pending);
    if(gx_scoped_context_enabled()){
        std::lock_guard context_lock(g_context_mutex);g_context_marks.clear();
        g_context_clock.restore(g_head.load(),g_pending.size());g_pending_begin_abs=0;
        g_decode_timeline.restore_pending(loaded_context);g_producer_choice=g_decode_timeline.at(g_pending.size());g_producer_debug={};
        g_producer_context_offset=g_pending.size();
        if(!scoped_state&&!g_pending.empty())std::fprintf(stderr,"GX01 pending prefix uses explicit legacy/live decode fallback\n");
    }
    custom_crosshair_reset_stream(); // Private tickets/config are not guest state; stale queued tags fall back safely.
    g_need=0; // state files preserve pending bytes, not derived parser thresholds
    tev_regs_access(regs, konst, true);
    texture_cache_clear();
    g_cfg_dirty=true;g_xf_dirty=true;g_vsetup_dirty=true;
    return true;
}

} // namespace ssne
