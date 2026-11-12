#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <set>
#include <fstream>
#include <filesystem>
#include "ssne/performance_detail.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int64_t ssne::perf::detail::thread_cpu_ns(){
#ifdef _WIN32
    FILETIME created,exited,kernel,user;
    if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))return -1;
    auto ticks=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return int64_t((ticks(kernel)+ticks(user))*100);
#else
    return -1;
#endif
}
#include "ssne/replica_simulation.h"

#include "ssne/ppc_context.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/native_party.h"
#include "ssne/game_addresses.h"

namespace {
std::unordered_map<uint32_t, PPCFunc> g_hooks; // node-based: slot pointers stay valid
std::mutex g_hook_mutex;
ssne::FatalHandler g_fatal = nullptr;
} // namespace

PPCFunc* ssne_hook_slot(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_hook_mutex);
    return &g_hooks[addr];
}

void ssne_install_hook(uint32_t addr, PPCFunc fn) { *ssne_hook_slot(addr) = fn; }

const SSNEFuncEntry* ssne_find_func(uint32_t addr) {
    const SSNEFuncEntry* b = g_ssne_funcs;
    const SSNEFuncEntry* e = g_ssne_funcs + g_ssne_func_count;
    auto it = std::lower_bound(b, e, addr, [](const SSNEFuncEntry& f, uint32_t a) { return f.address < a; });
    return (it != e && it->address == addr) ? it : nullptr;
}

void ssne_call(PPCContext& ctx, uint32_t addr) {
    if (addr < ssne::kRamSize) addr |= 0x80000000u; // physical -> cached virtual
    static const bool trace_weapon=getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES");
    if(trace_weapon && ((addr>=0x8005C000u && addr<0x8005E000u) || (ctx.lr>=0x8008D400u && ctx.lr<0x8008E100u) || (ctx.lr>=0x800A0000u && ctx.lr<0x800B0000u) || (ctx.lr>=0x800C0000u && ctx.lr<0x800D0000u))){
        static thread_local std::set<std::pair<uint32_t,uint32_t>> seen;
        if(seen.emplace(ctx.lr,addr).second){char line[220];snprintf(line,sizeof(line),"[coop-weapon-indirect-call] caller=%08X target=%08X r3=%08X r4=%08X r5=%08X r6=%08X",ctx.lr,addr,ctx.r[3],ctx.r[4],ctx.r[5],ctx.r[6]);ssne::log_message(line);}
        const auto player=trace_weapon?mem_read32(ssne::native_party_registry()):0;
        if(ctx.lr==0x800CE4FCu && mem_read32(ssne::game_address(0x802DB0E0))==4 && player>=0x80000000u && player<=ssne::guest_ram_end()-0x790 && mem_read32(player+0x5DC)==6 && ctx.r[3]>=0x80000000u && ctx.r[3]<=ssne::guest_ram_end()-0x160){
            static unsigned samples=0;
            if(samples<4){
                char name[80];snprintf(name,sizeof(name),"projectile-candidate-%u.bin",samples++);
                if(auto* file=fopen(name,"wb")){for(unsigned i=0;i<0x160;++i){const auto byte=mem_read8(ctx.r[3]+i);fwrite(&byte,1,1,file);}fclose(file);}
                char line[160];snprintf(line,sizeof(line),"[coop-projectile-candidate] object=%08X table=%08X dump=%s",ctx.r[3],mem_read32(ctx.r[3]+0x1C),name);ssne::log_message(line);
            }
        }
    }
    PPCFunc hook = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_hook_mutex);
        auto h = g_hooks.find(addr);
        if (h != g_hooks.end()) hook = h->second;
    }
    if (hook) { hook(ctx); return; }
    if (auto* f = ssne_find_func(addr)) { f->func(ctx); return; }
    {
        char line[240];
        snprintf(line,sizeof(line),"[party-bad-call] target=%08X lr=%08X sp=%08X r3=%08X r4=%08X r5=%08X r6=%08X r9=%08X r11=%08X",addr,ctx.lr,ctx.r[1],ctx.r[3],ctx.r[4],ctx.r[5],ctx.r[6],ctx.r[9],ctx.r[11]);ssne::log_message(line);
        uint32_t sp=ctx.r[1];
        for(unsigned i=0;i<16 && sp>=0x80000000u && sp<ssne::guest_ram_end()-8;++i) {
            auto previous=mem_read32(sp);
            if(previous<=sp || previous>=ssne::guest_ram_end()-8)break;
            snprintf(line,sizeof(line),"[party-stack] sp=%08X previous=%08X return=%08X",sp,previous,mem_read32(previous+4));ssne::log_message(line);
            sp=previous;
        }
    }
    // Preserve the first failing guest state before unwinding destroys the
    // original caller stack. Never continue through an invalid virtual call.
    if(ssne::replica_simulation() || getenv("SSNE_PERF_LOG")){
        static std::atomic<bool> saved{false};
        if(!saved.exchange(true))try{
            std::filesystem::create_directories("crash-diagnostics");
            const auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const auto prefix="crash-diagnostics/native-"+std::to_string(stamp);
            std::ofstream cpu(prefix+".context.bin",std::ios::binary);cpu.write(reinterpret_cast<const char*>(&ctx),sizeof(ctx));
            std::ofstream ram(prefix+".ram.bin",std::ios::binary);ram.write(reinterpret_cast<const char*>(ssne::g_ram),ssne::guest_ram_size());
            char detail[160];snprintf(detail,sizeof(detail),"target=%08X lr=%08X ram_bytes=%u cpu_bytes=%zu",addr,ctx.lr,ssne::guest_ram_size(),sizeof(ctx));
            std::ofstream metadata(prefix+".txt");metadata<<detail<<'\n';for(unsigned i=0;i<32;++i){char reg[48];snprintf(reg,sizeof(reg),"r%u=%08X\n",i,ctx.r[i]);metadata<<reg;}
            ssne::log_message("[native-crash-diagnostic] prefix="+prefix+" "+detail+" ram_complete="+std::to_string(bool(ram)));
        }catch(...){ssne::log_message("[native-crash-diagnostic] could not preserve guest state");}
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "Jump to %08X, which is not a known function start (LR=%08X).\n"
             "Add it with --map or improve function discovery.", addr, ctx.lr);
    ssne::fatal(msg);
}

void ssne_unimplemented(PPCContext& ctx, uint32_t addr, uint32_t insn) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Unimplemented instruction %08X at %08X.", insn, addr);
    ssne::fatal(msg);
}

void ssne_syscall(PPCContext& ctx, uint32_t addr) {
    // Dolphin-SDK games only use `sc` inside PPCHalt/OSResetSystem paths; ignore.
    (void)ctx; (void)addr;
}

uint32_t ssne_mfspr(PPCContext& ctx, uint32_t spr) {
    switch (spr) {
    case 920: return ctx.hid2;
    case 22: return ctx.dec;
    case 26: return ctx.srr0;
    case 27: return ctx.srr1;
    case 272: case 273: case 274: case 275: return ctx.sprg[spr - 272];
    default: return 0; // HID0/1, L2CR, WPAR, DMA regs, etc.
    }
}

void ssne_mtspr(PPCContext& ctx, uint32_t spr, uint32_t v) {
    switch (spr) {
    case 920: ctx.hid2 = v; break;
    case 22: ctx.dec = v; break;
    case 26: ctx.srr0 = v; break;
    case 27: ctx.srr1 = v; break;
    case 272: case 273: case 274: case 275: ctx.sprg[spr - 272] = v; break;
    default: break; // Locked-cache DMA (DMAU/DMAL 922/923) handled later.
    }
}

static std::atomic<int64_t> g_tb_shift{0};  // save states: the guest clock continues from the saved time
static std::atomic<bool> g_tb_frozen{false};
static std::atomic<uint64_t> g_tb_frozen_value{0};
void ssne_timebase_shift(int64_t ticks) {
    // Called only while the checkpoint producer barrier is held when frozen.
    if (g_tb_frozen.load(std::memory_order_acquire)) g_tb_frozen_value += ticks;
    else g_tb_shift += ticks;
}

static uint64_t timebase_running() {
    // Gekko timebase runs at bus clock / 4 = 162 MHz / 4 = 40.5 MHz.
    using namespace std::chrono;
    // Like the console's boot ROM, start it at the RTC time (local seconds since 2000-01-01), so OSGetTime /
    // OSTicksToCalendarTime give the real date (memory card saves get correct timestamps).
    static const auto t0 = steady_clock::now();
    static const uint64_t base = [] {
        std::time_t t = std::time(nullptr);
        std::tm lt{}, gt{};
#ifdef _WIN32
        localtime_s(&lt, &t); gmtime_s(&gt, &t);
#else
        localtime_r(&t, &lt); gmtime_r(&t, &gt);
#endif
        gt.tm_isdst = -1;  // let mktime apply DST for the local interpretation
        int64_t local = int64_t(t) + int64_t(std::mktime(&lt) - std::mktime(&gt));
        return uint64_t(local - 946684800) * 40500000ull;
    }();
    auto ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return uint64_t(int64_t(base + uint64_t(double(ns) * 0.0405)) + g_tb_shift.load());
}

uint64_t ssne_timebase() {
    return g_tb_frozen.load(std::memory_order_acquire) ? g_tb_frozen_value.load() : timebase_running();
}

void ssne_timebase_freeze() {
    g_tb_frozen_value.store(timebase_running());
    g_tb_frozen.store(true, std::memory_order_release);
}

void ssne_timebase_resume() {
    g_tb_shift += int64_t(g_tb_frozen_value.load()) - int64_t(timebase_running());
    g_tb_frozen.store(false, std::memory_order_release);
}

namespace ssne {

void set_fatal_handler(FatalHandler h) { g_fatal = h; }

void fatal(const char* message) {
    fprintf(stderr, "[ssne] FATAL: %s\n", message);
    if (g_fatal) g_fatal(message);
    std::exit(1);
}

void reset_cpu(PPCContext& ctx) {
    ctx = PPCContext{};
    // Apploader/IPL leave the stack near the top of MEM1; the game's __start sets r1/r2/r13 itself.
    ctx.r[1] = 0x816FFFF0;
    ctx.lr = 0;
}

uint32_t guest_ram_size() {
    const auto size=mem_read32(0x800000F0);
    return size==0x02000000 || size==0x04000000?size:0x01800000;
}
uint32_t guest_ram_end(){return 0x80000000u+guest_ram_size();}
void boot_globals(uint32_t mem_size) {
    // "OS globals" low memory that the IPL normally fills (see YAGCD, section 4.2).
    mem_write32(0x80000020, 0x0D15EA5E);  // boot magic (normal boot)
    mem_write32(0x80000024, 1);           // version
    mem_write32(0x80000028, mem_size);    // physical memory size
    mem_write32(0x8000002C, 1);           // console type: retail
    mem_write32(0x800000F0, mem_size);    // simulated memory size
    mem_write32(0x800000F8, 162000000);   // bus clock
    mem_write32(0x800000FC, 486000000);   // CPU clock
}

} // namespace ssne
