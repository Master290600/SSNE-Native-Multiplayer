// Save states. The recompiled game runs as native code on host threads, so a state can only be taken or restored
// where the host side is in a known shape: at the start of the game's frame flip (CcEngine::Flip), called from
// its main loop with only the main thread alive. There the host call stack holds nothing but the recompiled
// functions of that call chain (which keep all guest state in the PPCContext and guest memory), so swapping guest
// memory, CPU registers and the emulated hardware is enough. The chain of guest return addresses is stored and
// must match when loading (it is the same in menus and gameplay; only the boot screens differ).
//
// File: "SSNESTAT", version, game ID, function count, call chain, timebase, CRC32 of the payload, then the payload:
// CPU registers, RAM (zero pages elided), locked cache, scheduler (interrupts, alarms), MMIO/ARAM/DSP/AI, SI,
// EXI (memory card contents included), GX.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "ssne/gx.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/exi.h"
#include "ssne/native_party.h"
#include "ssne/scheduler.h"
#include "ssne/audio.h"
#include "ssne/checkpoint.h"
#include "state_io.h"

void ssne_timebase_shift(int64_t ticks);
void ssne_timebase_freeze();
void ssne_timebase_resume();

namespace ssne {
namespace {
std::atomic<uint64_t> g_runtime_state_generation{0};
thread_local unsigned g_runtime_state_depth = 0;
}
std::recursive_mutex& runtime_state_mutex() { static std::recursive_mutex mutex; return mutex; }
uint64_t runtime_state_generation() { return g_runtime_state_generation.load(std::memory_order_acquire); }
RuntimeStateGuard::RuntimeStateGuard() : lock_(runtime_state_mutex()) {
    if (g_runtime_state_depth++ == 0) ssne_timebase_freeze();
}
RuntimeStateGuard::~RuntimeStateGuard() {
    if (--g_runtime_state_depth == 0) {
        ssne_timebase_resume();
        g_runtime_state_generation.fetch_add(1, std::memory_order_release);
    }
}
namespace {

constexpr char kMagic[8] = {'S', 'S', 'N', 'E', 'S', 'T', 'A', 'T'};
constexpr uint32_t kVersion = 1;
constexpr size_t kMaxStateBytes = 256u * 1024u * 1024u;

std::mutex g_mx;
bool g_req = false, g_req_save = false;
std::string g_req_path;
int g_req_tries = 0;
bool g_have_result = false;
SavestateResult g_result;
thread_local SavestateResult* g_probe_result = nullptr;

uint32_t crc32(const uint8_t* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<uint32_t> call_chain(const PPCContext& ctx) {
    std::vector<uint32_t> chain{ctx.lr};
    uint32_t sp = ctx.r[1];
    for (int i = 0; i < 32 && sp >= 0x80000000u && sp < guest_ram_end(); i++) {
        uint32_t back = mem_read32(sp);
        if (back < 0x80000000u || back >= guest_ram_end() || back <= sp) break;
        if (back > guest_ram_end() - 8) break;
        chain.push_back(mem_read32(back + 4));
        sp = back;
    }
    return chain;
}

std::string game_id() {
    char id[7] = {};
    for (int i = 0; i < 6; i++) id[i] = char(mem_read8(0x80000000u + uint32_t(i)));
    return id;
}

void finish(bool save, bool ok, const std::string& path, const std::string& msg) {
    if (g_probe_result) { *g_probe_result = {save, ok, path, msg}; return; }
    std::lock_guard<std::mutex> lk(g_mx);
    g_result = {save, ok, path, msg};
    g_have_result = true;
    fprintf(stdout, "[state] %s\n", msg.c_str());
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data, std::string& err, unsigned* replace_retries=nullptr) {
    if(replace_retries)*replace_retries=0;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::u8path(path), tmp = fs::u8path(path + ".tmp");
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + path; return false; }
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        f.flush(); f.close();
        if (!f) { err = "cannot write " + path + " (disk full?)"; return false; }
    }
#ifdef _WIN32
    // File indexers/sync tools can briefly hold the destination without delete
    // sharing. Keep the old save intact and retry only transient Windows errors.
    for(unsigned attempt=0;;++attempt){
        if(MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))break;
        const auto code=GetLastError();
        if(attempt==20 || (code!=ERROR_SHARING_VIOLATION && code!=ERROR_LOCK_VIOLATION && code!=ERROR_ACCESS_DENIED)){
            err="cannot replace "+path+" (Windows error "+std::to_string(code)+")";return false;
        }
        if(replace_retries)++*replace_retries;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    fs::rename(tmp, p, ec);
    if (ec) { err = "cannot write " + path; return false; }
#endif
    return true;
}

std::vector<uint8_t> capture_runtime(const PPCContext& ctx) {
    StateWriter pay;
    pay.tag("CPU1");
    pay.pod(ctx);
    pay.tag("RAM1");
    pay.block(g_ram, guest_ram_size()>0x02000000?kRamSize:0x02000000);
    pay.bytes(g_locked_cache, kLockedCacheSize);
    sched_state_save(pay);
    hw_state_save(pay);
    si_state_save(pay);
    exi_state_save(pay);
    gx_state_save(pay);
    StateWriter w;
    w.bytes(kMagic, 8);
    w.u32(kVersion);
    std::string id = game_id();
    w.bytes(id.data(), 6);
    w.u64(g_ssne_func_count);
    auto chain = call_chain(ctx);
    w.u32(uint32_t(chain.size()));
    for (uint32_t a : chain) w.u32(a);
    w.u64(ssne_timebase());
    w.u64(uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    w.u32(crc32(pay.buf.data(), pay.buf.size()));
    w.u64(pay.buf.size());
    w.bytes(pay.buf.data(), pay.buf.size());
    return std::move(w.buf);
}

void do_save(PPCContext& ctx, const std::string& path) {
    RuntimeStateGuard guard;
    std::string why;
    if (!sched_state_check(why)) { finish(true, false, path, "Can't save now: " + why); return; }
    const auto data = capture_runtime(ctx);
    std::string err;
    if (!write_file(path, data, err)) { finish(true, false, path, "Save state failed: " + err); return; }
    char msg[96];
    snprintf(msg, sizeof(msg), "State saved (%.1f MB)", data.size() / 1048576.0);
    finish(true, true, path, msg);
}

struct RuntimeImage {
    StateReader payload;
    PPCContext context;
    uint64_t timebase = 0;
};
bool inspect_runtime(const PPCContext& ctx, const std::vector<uint8_t>& data,
                     StateReader& payload, PPCContext& saved_ctx, uint64_t& saved_tb, std::string& error) {
    if (data.size() > kMaxStateBytes) { error = "Invalid state file size"; return false; }
    StateReader r{data.data(), data.size()};
    char magic[8] = {};
    r.bytes(magic, 8);
    if (!r.ok || memcmp(magic, kMagic, 8)) { error = "Not a save state file"; return false; }
    if (r.u32() != kVersion) { error = "This state was made by another version of the port"; return false; }
    char id[7] = {};
    r.bytes(id, 6);
    if (game_id() != id) { error = std::string("This state is from another release of the game (") + id + ")"; return false; }
    if (r.u64() != g_ssne_func_count) { error = "This state was made with a different game build"; return false; }
    const auto count = r.u32();
    if (!r.ok || count < 1 || count > 33) { error = "Invalid state call chain"; return false; }
    std::vector<uint32_t> chain(count);
    for (auto& a : chain) a = r.u32();
    uint64_t tb = r.u64();
    r.u64();  // wall-clock time of the save (shown by the port)
    uint32_t crc = r.u32();
    uint64_t len = r.u64();
    if (!r.ok || len != data.size() - r.pos || crc32(data.data() + r.pos, len) != crc) {
        error = "The state file is damaged";
        return false;
    }
    if (chain != call_chain(ctx)) {
        error = "State call chain differs from the current safe point";
        return false;
    }
    StateReader p{data.data() + r.pos, size_t(len)};
    PPCContext saved;
    if (!p.tag("CPU1") || !p.pod(saved)) { error = "The state file is damaged"; return false; }
    // A checksum alone does not prove the internal layout. Validate every
    // section, including the final GX block and card compatibility, before any
    // RAM/register, clock, scheduler, hardware or persistent-card change.
    auto check = p;
    const bool valid = check.tag("RAM1") && check.block(nullptr, guest_ram_size()>0x02000000?kRamSize:0x02000000) && check.bytes(nullptr, kLockedCacheSize)
        && sched_state_load(check, false) && hw_state_load(check, false) && si_state_load(check, false)
        && exi_state_load(check, false) && gx_state_load(check, false) && check.ok && check.pos == check.n;
    if (!valid) { error = "The state layout is damaged or incompatible; running game unchanged"; return false; }
    if (tb > uint64_t(std::numeric_limits<int64_t>::max()) || ssne_timebase() > uint64_t(std::numeric_limits<int64_t>::max())) {
        error = "Invalid state timebase"; return false;
    }
    payload = p; saved_ctx = saved; saved_tb = tb;
    return true;
}

bool apply_runtime(PPCContext& ctx, RuntimeImage image) {
    auto& p = image.payload;
    ssne_timebase_shift(int64_t(image.timebase) - int64_t(ssne_timebase()));
    const bool ok = p.tag("RAM1") && p.block(g_ram, guest_ram_size()>0x02000000?kRamSize:0x02000000)
        && p.bytes(g_locked_cache, kLockedCacheSize) && sched_state_load(p) && hw_state_load(p)
        && si_state_load(p) && exi_state_load_volatile(p) && gx_state_load(p) && p.ok && p.pos == p.n;
    if (ok) ctx = image.context;
    return ok;
}

// Private coordinator until all session participants and epoch reconciliation
// are wired. Callbacks run under the frozen runtime barrier and may not publish
// network events or persist files. Restore must accept its own captured backup.
using CaptureExtra = std::function<bool(std::vector<uint8_t>&, std::string&)>;
using RestoreExtra = std::function<bool(const std::vector<uint8_t>&, std::string&)>;
bool load_transaction(PPCContext& ctx, const std::vector<uint8_t>& data,
                      const std::vector<uint8_t>& extra, CaptureExtra capture,
                      RestoreExtra restore, std::string& error, std::function<void()> finalize = {}, std::function<void()> discard = {}) {
    RuntimeStateGuard guard;
    if (!sched_state_check(error)) return false;
    RuntimeImage target, backup;
    if (!inspect_runtime(ctx, data, target.payload, target.context, target.timebase, error)) return false;
    const auto old_runtime = capture_runtime(ctx);
    if (!inspect_runtime(ctx, old_runtime, backup.payload, backup.context, backup.timebase, error)) return false;
    const auto old_card = exi_memcard_snapshot();
    std::vector<uint8_t> old_extra;
    if (bool(capture) != bool(restore)) { error = "Incomplete checkpoint participant"; return false; }
    if (capture && !capture(old_extra, error)) return false;
    bool committed = false;
    try {
        if (!apply_runtime(ctx, target)) error = "Runtime application failed";
        else if (restore && !restore(extra, error)) { /* Restore below. */ }
        else committed = exi_state_commit(old_card, error);
    } catch (const std::exception& e) { error = std::string("Checkpoint application failed: ") + e.what(); }
      catch (...) { error = "Checkpoint application failed"; }
    if (committed) {
        // The backing card has crossed the commit point. Never roll RAM back
        // after this point, even if local presentation cleanup unexpectedly fails.
        try { if (finalize) finalize(); audio_state_reset(); }
        catch (...) { fatal("Checkpoint committed but finalization failed; session cannot continue"); }
        return true;
    }
    // RAM first: external native bindings are validated against the old world.
    // A failed rollback must never release the barrier into a mixed world.
    std::string rollback_error;bool rolled_back=false;
    try {
        if (!apply_runtime(ctx, backup)) rollback_error="Runtime backup application failed";
        else if (!restore || restore(old_extra, rollback_error)) { if(discard)discard();rolled_back=true; }
    } catch (const std::exception& e) { rollback_error=e.what(); }
      catch (...) { rollback_error="Unknown rollback exception"; }
    if(!rolled_back)fatal(("Checkpoint rollback failed: "+rollback_error+"; original error: "+error).c_str());
    return false;
}

void do_load(PPCContext& ctx, const std::string& path) {
    RuntimeStateGuard guard;
    std::string why;
    if (!sched_state_check(why)) { finish(false, false, path, "Can't load now: " + why); return; }
    std::vector<uint8_t> data;
    {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
        if (!f) { finish(false, false, path, "No state saved in this slot"); return; }
        const auto size = f.tellg();
        if (size < 0 || uint64_t(size) > kMaxStateBytes) { finish(false, false, path, "Invalid state file size"); return; }
        f.seekg(0);
        data.assign(std::istreambuf_iterator<char>(f), {});
        if (data.size() != size_t(size)) { finish(false, false, path, "Cannot read complete state file"); return; }
    }
    std::string error;
    const bool ok = load_transaction(ctx, data, {}, {}, {}, error);
    finish(false, ok, path, ok ? "State loaded" : "State rejected; running game unchanged: " + error);

}

} // namespace

bool runtime_checkpoint_visit(const std::function<void()>& operation,std::string& error) {
    RuntimeStateGuard guard;
    if (!sched_state_check(error)) return false;
    operation();return true;
}
bool runtime_checkpoint_capture(PPCContext& ctx,std::vector<uint8_t>& runtime,
        std::vector<uint8_t>& external,const RuntimeCheckpointHooks& hooks,std::string& error) {
    RuntimeStateGuard guard;
    if (!sched_state_check(error)) return false;
    std::vector<uint8_t> extra;
    if (hooks.capture && !hooks.capture(extra,error)) return false;
    auto core=capture_runtime(ctx);
    runtime=std::move(core);external=std::move(extra);return true;
}
bool runtime_checkpoint_restore(PPCContext& ctx,const std::vector<uint8_t>& runtime,
        const std::vector<uint8_t>& external,const RuntimeCheckpointHooks& hooks,std::string& error) {
    return load_transaction(ctx,runtime,external,hooks.capture,hooks.restore,error,hooks.commit,hooks.abort);
}

void savestate_request(bool save, const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_req = true;
    g_req_save = save;
    g_req_path = path;
    g_req_tries = 0;
}

bool savestate_pending() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_req;
}

bool savestate_take_result(SavestateResult& out) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_have_result) return false;
    out = g_result;
    g_have_result = false;
    return true;
}

bool savestate_read_info(const std::string& path, uint64_t& unix_time) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> head(4096);
    f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
    head.resize(size_t(f.gcount()));
    StateReader r{head.data(), head.size()};
    char magic[8] = {};
    r.bytes(magic, 8);
    if (!r.ok || memcmp(magic, kMagic, 8) || r.u32() != kVersion) return false;
    char id[6];
    r.bytes(id, 6);
    r.u64();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) r.u32();
    r.u64();
    unix_time = r.u64();
    return r.ok;
}

// Called at the start of every CcEngine::Flip (hle.cpp), on the game's main thread.
void savestate_safe_point(PPCContext& ctx) {
    bool save;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_req) return;
        save = g_req_save;
        path = g_req_path;
    }
    RuntimeStateGuard guard;
    std::string why;
    if (!sched_state_check(why)) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (++g_req_tries < 180) return;  // try again on the next frames (~3 s)
        g_req = false;
        g_result = {save, false, path, std::string(save ? "Can't save now: " : "Can't load now: ") + why};
        g_have_result = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_req = false;
    }
    gx_sync();  // the GX thread writes RAM too (EFB copies): let it finish before RAM is saved or replaced
    if (save) do_save(ctx, path);
    else do_load(ctx, path);
}

#include "savestate_probe.inc"

} // namespace ssne
