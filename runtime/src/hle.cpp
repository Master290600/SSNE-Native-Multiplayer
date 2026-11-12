// High-level emulation: loading the game and replacing Dolphin SDK functions with native code.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <map>
#include <vector>
#include <set>
#include <mutex>
#include <thread>
#include <fstream>
#include <iterator>
#include <string>

#include "ssne/gx.h"
#include "gc/dol.h"
#include "ssne/hw.h"
#include "ssne/input.h"
#include "ssne/recomp_hooks.h"
#include "ssne/scheduler.h"
#include "ssne/runtime.h"
#include "ssne/weapon_groups.h"
#include "ssne/native_render_list.h"
#include "ssne/performance.h"
#include "ssne/gx_gpu.h"
#include "ssne/gx.h"
#include "ssne/native_party.h"
#include "ssne/native_results.h"
#include "ssne/native_cutscene.h"
#include "ssne/native_music.h"
#include "ssne/replica_simulation.h"
#include "ssne/input.h"
#include "ssne/mods.h"
#include "ssne/bot.h"
#include "native_vehicle_input.h"
#include "ssne/game_addresses.h"
#include <array>
#include <utility>

namespace ssne {
namespace gx { bool hud_edges(); }
void frame_limit_wait_impl(int lim);
void savestate_checkpoint_probe(PPCContext&);

namespace {
LogSink g_log_sink = nullptr;
NativeHudListObserver g_native_hud_list_observer = nullptr;
}

void set_log_sink(LogSink sink) { g_log_sink = sink; }
void log_message(const std::string& line) {
    fprintf(stdout, "%s\n", line.c_str());
    fflush(stdout);
    if (g_log_sink) g_log_sink(line.c_str());
}

static std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

uint32_t load_game(const std::string& data_dir, std::string& error) {
    auto data = slurp(data_dir + "/sys/main.dol");
    if (data.empty()) { error = "main.dol not found in " + data_dir + "/sys (run the launcher to install your disc)"; return 0; }
    gc::Dol dol;
    if (!gc::Dol::parse(data, dol, error)) return 0;
    memset(g_ram, 0, kRamSize);
    dol.load_into(g_ram, kRamSize);
    boot_globals(native_party_memory_size());

    // What the IPL + apploader leave in memory (YAGCD 4.2 "OS globals"):
    auto boot = slurp(data_dir + "/sys/boot.bin");
    auto bi2 = slurp(data_dir + "/sys/bi2.bin");
    auto fst = slurp(data_dir + "/sys/fst.bin");
    if(native_party_memory_size()>0x01800000 && (boot.size()<6 || memcmp(boot.data(),"G3BP9G",6))){error="More than four native players currently require the PAL game revision.";return 0;}
    if (boot.size() >= 0x20) memcpy(g_ram, boot.data(), 0x20);              // disc ID @ 0x80000000
    if (!fst.empty()) {
        uint32_t fst_addr = (guest_ram_end() - uint32_t(fst.size())) & ~31u;
        memcpy(g_ram + (fst_addr & (kRamSize-1)), fst.data(), fst.size());
        mem_write32(0x80000038, fst_addr);                                   // FST location
        mem_write32(0x8000003C, uint32_t(fst.size()));                       // FST max size
        if (bi2.size() >= 0x2000) {
            uint32_t bi2_addr = (fst_addr - 0x2000) & ~31u;
            memcpy(g_ram + (bi2_addr & (kRamSize-1)), bi2.data(), 0x2000);
            mem_write32(0x800000F4, bi2_addr);                               // BI2 location
        }
    }
    mem_write32(0x800000CC, 0);                                              // TV mode: NTSC
    mem_write32(0x800030D8, 0);                                              // system time (low)

    if (!disc_open(data_dir, error)) return 0;
    return dol.entry;
}

uint32_t boot(PPCContext& ctx, const std::string& data_dir, std::string& error) {
    uint32_t entry = load_game(data_dir, error);
    if (!entry) return 0;
    reset_cpu(ctx);
    install_sdk_hooks();
    install_thread_hooks();
    scheduler_init(ctx);
    return entry;
}

namespace {

std::string guest_string(uint32_t addr, size_t max = 1024) {
    std::string s;
    if (!addr) return "(null)";
    for (uint32_t a = addr; s.size() < max; a++) { char c = char(mem_read8(a)); if (!c) break; s += c; }
    return s;
}

// printf for guest varargs (PowerPC EABI): integer/pointer args in r3..r10, doubles in f1..f8.
std::string guest_format(PPCContext& ctx, uint32_t fmt_addr, int first_gpr) {
    std::string fmt = guest_string(fmt_addr), out;
    int gpr = first_gpr, fpr = 1;
    auto next_int = [&]() -> uint32_t { return gpr <= 10 ? ctx.r[gpr++] : 0; };
    auto next_dbl = [&]() -> double { return fpr <= 8 ? ctx.ps0[fpr++] : 0.0; };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0", fmt[j])) j++;
        if (j < fmt.size() && fmt[j] == '*') j++;
        while (j < fmt.size() && isdigit(uint8_t(fmt[j]))) j++;
        if (j < fmt.size() && fmt[j] == '.') { j++; while (j < fmt.size() && isdigit(uint8_t(fmt[j]))) j++; }
        while (j < fmt.size() && strchr("hlLqjzt", fmt[j])) j++;
        if (j >= fmt.size()) break;
        std::string spec = fmt.substr(i, j - i);        // flags/width/precision, no length modifiers
        spec.erase(std::remove_if(spec.begin(), spec.end(), [](char c) { return strchr("hlLqjzt", c); }), spec.end());
        char conv = fmt[j], buf[512];
        buf[0] = 0;
        switch (conv) {
        case '%': strcpy(buf, "%"); break;
        case 'd': case 'i': snprintf(buf, sizeof(buf), (spec + "d").c_str(), int32_t(next_int())); break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_int()); break;
        case 'p': snprintf(buf, sizeof(buf), "0x%08X", next_int()); break;
        case 's': snprintf(buf, sizeof(buf), (spec + "s").c_str(), guest_string(next_int(), 400).c_str()); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_dbl()); break;
        default: out += spec + conv; i = j; continue;
        }
        out += buf;
        i = j;
    }
    return out;
}

// --- Native replacements. Each one fully replaces the game's copy of the SDK function.
void hle_OSReport(PPCContext& ctx) {
    std::string s = guest_format(ctx, ctx.r[3], 4);
    // Split into lines for the log sink; keep stdout output for headless runs.
    static std::string pending;
    pending += s;
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        pending.erase(0, nl + 1);
        fprintf(stdout, "[game] %s\n", line.c_str());
        if (g_log_sink) g_log_sink(line.c_str());
    }
    fflush(stdout);
}

void hle_OSPanic(PPCContext& ctx) {
    // OSPanic(const char* file, int line, const char* fmt, ...)
    std::string msg = guest_format(ctx, ctx.r[5], 6);
    fatal(("OSPanic at " + guest_string(ctx.r[3], 128) + ":" + std::to_string(ctx.r[4]) + ": " + msg).c_str());
}

void hle_nop(PPCContext&) {}

// The IPL's SRAM isn't emulated (EXI reads return zeros), which reads as "mono" and would make the game mix
// every voice to the center. Report the console's sound setting as stereo.
void hle_OSGetSoundMode(PPCContext& ctx) { ctx.r[3] = 1; }  // OS_SOUND_MODE_STEREO

// ---- Widescreen: the game's camera projections all go through the SDK's C_MTXPerspective (and projected
// textures through C_MTXLightPerspective). The replacements below compute the same matrices (same SDK constants,
// the game's own tanf) with the aspect ratio multiplied by the widescreen factor, which widens the horizontal
// field of view; the renderer and display then show the image at the wider aspect.
std::atomic<float> g_widescreen{1.0f};

// Addresses of the game functions the port hooks, per version of main.dol (the disc ID is at 0x80000000). The PAL
// main.dol has the same functions as the USA one, shifted (see data/symbols/G3BP9G_r0.sym); every hook still checks
// the code it patches before installing.
struct GameAddresses {
    const char* id;
    uint32_t look_update;    // player look update (mouse aim)
    uint32_t flip_bl;        // CcEngine::Flip: bl VIWaitForRetrace
    uint32_t vi_wait;        // VIWaitForRetrace
    uint32_t tanf;           // the game's tanf (used by the projection replacements)
    uint32_t mtx_persp;      // C_MTXPerspective
    uint32_t mtx_light;      // C_MTXLightPerspective
    uint32_t weapon_lr;      // return address of the first-person weapon's projection call (gameplay detector)
    uint32_t cam_lr[4];      // return addresses of the scene camera's projection calls (FOV slider)
    uint32_t pad_clamp;      // PADClamp (called after PADRead in the VI retrace callback)
    uint32_t pad_process;    // the game's controller processing (rescales the sticks in place)
    uint32_t flip;           // CcEngine::Flip (save states are taken at its start)
    uint32_t player_update;  // the player's per-frame update (r3 = player), on foot and in vehicles
};
const GameAddresses kVersions[] = {
    {"G3BE9G", 0x800BFCA4, 0x8011855C, 0x801BF964, 0x80181590, 0x801BA874, 0x801BA5EC, 0x8014CDF0,
     {0x8014C690, 0x8014C6B8, 0x8014C6D8, 0x8014C6FC}, 0x801C0BAC, 0x8014F83C, 0x801183F0, 0x800BEEC8},  // USA Rev 1 (FINAL.107)
    {"G3BP9G", 0x800BFCA4, 0x80118514, 0x801BF904, 0x80181530, 0x801BA814, 0x801BA58C, 0x8014CDA8,
     {0x8014C648, 0x8014C670, 0x8014C690, 0x8014C6B4}, 0x801C0B4C, 0x8014F7F4, 0x801183A8, 0x800BEEC8},  // Europe (FINAL.113)
};
const GameAddresses* g_ver = &kVersions[0];
void select_version() {
    char id[7] = {};
    for (int i = 0; i < 6; i++) id[i] = char(mem_read8(0x80000000u + uint32_t(i)));
    for (const auto& v : kVersions)
        if (!strcmp(v.id, id)) { g_ver = &v; fprintf(stdout, "[ssne] game version %s\n", id); return; }
    fprintf(stdout, "[ssne] unknown game version %s: using USA addresses (hooks verify their code first)\n", id);
}
// ---- player control. The player's update (800BEEC8, hooked below) runs once in each frame the player controls Sam,
// on foot or driving, between the frame's drawing and its flip; it doesn't run in the pause menu, NETRICSA, briefings
// or cutscenes. So the frame after a flip is a player-control frame if the player update ran before that flip (the
// first frame of a menu still is: the game opens it in that frame's update).
// Without both hooks (another game build), gameplay is detected by the weapon alone.
std::atomic<bool> g_player_seen{false}, g_player_control{false};
// Per controller port (multiplayer: a player's port is player+0x178, 0-3):
int64_t g_jump_latched_ms[4] = {-1, -1, -1, -1};  // a jump press not yet seen by a physics step (when; see latch_jump)
int g_switch_boost[4] = {};      // player updates left to run like 60 fps frames after a controller switch
// The port of the player the keyboard and mouse play: mouse aim, weapon number keys and the vehicle check for the
// keyboard's steering apply to that player only.
std::atomic<int> g_kbm_port{0};
int player_port(uint32_t player) { uint32_t p = mem_read32(player + 0x178); return p < 4 ? int(p) : 0; }
std::atomic<int> g_control_hooks{0};  // player update and flip hooks installed (2: player control is known)
void end_player_frame() { g_player_control = g_player_seen.exchange(false); }

// Gameplay widens the native projection. The frontend instead uses a wide
// framebuffer with the original widget projections: GPU viewport correction
// preserves its 3D models and HUD recording preserves orthographic text.
// Full-screen background artwork fills that framebuffer. Movies and cutscenes
// retain their original presentation.
std::atomic<bool> g_netplay_active{false};
bool g_authoritative_gameplay=false;
std::atomic<int64_t> g_last_gameplay_ms{-1000000};
std::atomic<int64_t> g_cam_zoom_ms{-1000000};
std::atomic<float> g_frame_factor{1.0f};  // factor used by the most recent camera projection
std::atomic<bool> g_frame_zoom{false};    // that projection's scene camera was zoomed in (the sniper scope)
int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
// The scene camera's vertical FOV as the game sets it (before the FOV slider) is below 63 while zoomed in (the sniper
// rifle, up to 12x). Zoomed in: a scene camera (any player's, in split screen) was zoomed in the last 250 ms.
constexpr float kGameFov = 63.0f;
bool zoomed_in() { return now_ms() - g_cam_zoom_ms.load() < 250; }
// A player's zoomed-in FOV (degrees), 0 if not zoomed in: the sniper rifle (weapon 8; a weapon's object is at
// player + 0x4BC + 0x18 * id) keeps its zoom at +0x230 (1 to 12), and the camera's FOV is then 80 / zoom.
float player_zoom_fov(uint32_t player) {
    if (mem_read32(player + 0x5DC) != 8) return 0;
    uint32_t rifle = mem_read32(player + 0x4BC + 8 * 0x18);
    if (rifle < 0x80000000u || rifle >= 0x817FFF00u) return 0;
    float zoom = u2f(mem_read32(rifle + 0x230));
    return zoom > 1.01f && zoom < 100.0f ? 80.0f / zoom : 0;
}

float effective_widescreen(uint32_t caller) {
    const bool frontend=mem_read32(game_address(0x802DB0E0u))==3;
    if(frontend || g_netplay_active){
        // Keep native widget cameras unchanged while presenting the original
        // frontend artwork across the selected display aspect. Local network
        // gameplay corrects its camera/UI in the GPU executor, not twice here.
        const float factor=frontend?g_widescreen.load():1.f;
        if(factor!=g_frame_factor.load() || g_frame_zoom.load())gx_sync();
        g_frame_factor=factor;g_frame_zoom=false;
        return 1.f;
    }
    if (caller == g_ver->weapon_lr) g_last_gameplay_ms = now_ms();
    bool gameplay = now_ms() - g_last_gameplay_ms.load() < 400 && (g_control_hooks.load() < 2 || g_player_control.load());
    float f = gameplay ? g_widescreen.load() : 1.0f;
    // Zoomed in, the scope overlay is laid out for 4:3: the GPU recorder keeps it round (see note_hud).
    bool zoom = gameplay && zoomed_in();
    // The GX thread reads the factor when it records the frame's display copy: let it finish the frames rendered
    // with the old factor first (the factor changes only between gameplay and the menus, the zoom with the scope).
    if (f != g_frame_factor.load() || zoom != g_frame_zoom.load()) gx_sync();
    g_frame_factor = f;
    g_frame_zoom = zoom;
    return f;
}

// Gameplay field of view (FOV slider). The scene camera (8014C090) builds its projections from these call
// sites; the first-person weapon (8014CDF0) keeps its own. 0 = the game's own FOV (63 degrees vertical).
std::atomic<float> g_fov{0.0f};
bool scene_camera_caller(uint32_t lr) {
    return lr == g_ver->cam_lr[0] || lr == g_ver->cam_lr[1] || lr == g_ver->cam_lr[2] || lr == g_ver->cam_lr[3];
}
// Widens (or narrows) the game's vertical FOV by the ratio of the chosen FOV to the default, applied to the
// tangent so the game's own zooms keep their proportions.
float adjust_fov(uint32_t lr, float fovy) {
    if(g_netplay_active) return fovy;
    float target = g_fov.load();
    if (target <= 0 || !scene_camera_caller(lr) || now_ms() - g_last_gameplay_ms.load() >= 400) return fovy;
    constexpr float d2r = 0.017453292f;
    float k = std::tan(0.5f * target * d2r) / std::tan(0.5f * kGameFov * d2r);
    float out = 2.0f * std::atan(std::tan(0.5f * fovy * d2r) * k) / d2r;
    return std::clamp(out, 1.0f, 170.0f);
}

inline float sda2_float(PPCContext& ctx, int32_t off) { return u2f(mem_read32(ctx.r[13] + uint32_t(off))); }
inline void put_f(uint32_t a, float v) { mem_write32(a, f2u(v)); }

float game_tanf(PPCContext& ctx, float x) {
    static PPCFunc tanf_fn = [] { const SSNEFuncEntry* e = ssne_find_func(g_ver->tanf); return e ? e->func : nullptr; }();
    if (!tanf_fn) return std::tan(x);
    ctx.ps0[1] = ctx.ps1[1] = double(x);
    tanf_fn(ctx);
    return float(ctx.ps0[1]);
}

// C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f)  @ 801BA874
void hle_MTXPerspective(PPCContext& ctx) {
    if(scene_camera_caller(ctx.lr) && ctx.ps0[1]<kGameFov-.5f)g_cam_zoom_ms=now_ms();
    if (static const char* tp = getenv("SSNE_TRACE_PROJ"); tp) {
        static const int every = std::max(1, atoi(tp));
        static std::map<uint32_t, int> seen;
        if (seen[ctx.lr]++ % every == 0)
            fprintf(stdout, "[proj] persp from %08X aspect %.3f fovy %.2f (x%d)\n", ctx.lr, ctx.ps0[2], ctx.ps0[1], seen[ctx.lr]);
    }
    uint32_t m = ctx.r[3];
    if (scene_camera_caller(ctx.lr) && ctx.ps0[1] < kGameFov - 0.5) g_cam_zoom_ms = now_ms();
    float aspect = float(ctx.ps0[2]) * effective_widescreen(ctx.lr), n = float(ctx.ps0[3]), f = float(ctx.ps0[4]);
    float fovy = adjust_fov(ctx.lr, float(ctx.ps0[1]));
    float half = sda2_float(ctx, -0x44B0), d2r = sda2_float(ctx, -0x44AC);  // 0.5, pi/180
    float one = sda2_float(ctx, -0x44C0), zero = sda2_float(ctx, -0x44B8), mone = sda2_float(ctx, -0x44B4);
    float angle = d2r * (half * fovy);
    float cot = one / game_tanf(ctx, angle);
    float tmp = one / (f - n);
    float r[16] = {cot / aspect, zero, zero, zero,  zero, cot, zero, zero,
                   zero, zero, -n * tmp, -(f * n) * tmp,  zero, zero, mone, zero};
    for (int i = 0; i < 16; i++) put_f(m + i * 4, r[i]);
}

// C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT)  @ 801BA5EC
void hle_MTXLightPerspective(PPCContext& ctx) {
    if (getenv("SSNE_TRACE_PROJ")) { static std::map<uint32_t, int> seen; if (seen[ctx.lr]++ % 600 == 0) fprintf(stdout, "[proj] light from %08X aspect %.3f fovy %.1f (x%d)\n", ctx.lr, ctx.ps0[2], ctx.ps0[1], seen[ctx.lr]); }
    uint32_t m = ctx.r[3];
    float aspect = float(ctx.ps0[2]) * effective_widescreen(ctx.lr);
    float fovy = adjust_fov(ctx.lr, float(ctx.ps0[1]));
    float sS = float(ctx.ps0[3]), sT = float(ctx.ps0[4]), tS = float(ctx.ps0[5]), tT = float(ctx.ps0[6]);
    float half = sda2_float(ctx, -0x4518), d2r = sda2_float(ctx, -0x4504);
    float one = sda2_float(ctx, -0x4520), zero = sda2_float(ctx, -0x451C), mone = sda2_float(ctx, -0x4508);
    float angle = d2r * (half * fovy);
    float cot = one / game_tanf(ctx, angle);
    float r[12] = {sS * (cot / aspect), zero, -tS, zero,  zero, cot * sT, -tT, zero,  zero, zero, mone, zero};
    for (int i = 0; i < 12; i++) put_f(m + i * 4, r[i]);
}

// Installed by address, only if the code at that address is the expected SDK function (USA Rev 1 main.dol).
struct AddressHook { uint32_t addr; uint32_t sig[6]; PPCFunc fn; const char* name; };
std::vector<AddressHook> address_hooks() {
    return {
        {g_ver->mtx_persp, {0x7C0802A6, 0x90010004, 0x9421FFC0, 0xDBE10038, 0xFFE02090, 0xDBC10030}, hle_MTXPerspective,
         "C_MTXPerspective"},
        {g_ver->mtx_light, {0x7C0802A6, 0x90010004, 0x9421FFA8, 0xDBE10050, 0xDBC10048, 0xDBA10040}, hle_MTXLightPerspective,
         "C_MTXLightPerspective"},
    };
}

struct NamedHook { const char* name; PPCFunc fn; };
const NamedHook kHooks[] = {
    {"OSReport", hle_OSReport},
    {"OSPanic", hle_OSPanic},
    {"OSGetSoundMode", hle_OSGetSoundMode},
    // Hardware-poking init routines that are meaningless on PC:
    {"__OSInitAudioSystem", hle_nop},
    {"OSInitCache", hle_nop},
    {"ICFlashInvalidate", hle_nop},
    {"DCFlushRange", hle_nop},
    {"DCStoreRange", hle_nop},
    {"DCInvalidateRange", hle_nop},
    {"ICInvalidateRange", hle_nop},
    // TODO(roadmap): OSThread*, OSAlarm*, DVD*, PAD*, VI*, GX*, AX*/AI*, CARD*, ARQ*
};

} // namespace

// ---- debug: pass-through call tracer (SSNE_TRACE_CALLS=addr[,addr...], hex). Counts calls per second and prints
// a guest backtrace (LR chain) for the first calls of each function. Used to find the game's frame pacing.
namespace {
struct CallTrace { uint32_t addr = 0; PPCFunc orig = nullptr; std::atomic<uint64_t> calls{0}; int printed = 0; };
CallTrace g_ct[8];
int g_ct_n = 0;
template <int N>
void trace_tramp(PPCContext& ctx) {
    CallTrace& t = g_ct[N];
    t.calls++;
    static std::mutex seen_mu;
    static std::set<uint64_t> seen;  // (lr, r3) pairs already reported
    bool fresh;
    {
        std::lock_guard<std::mutex> lk(seen_mu);
        fresh = t.printed < 40 && seen.insert(uint64_t(t.addr) << 32 ^ uint64_t(ctx.lr) << 16 ^ ctx.r[3]).second;
        static const bool all = getenv("SSNE_TRACE_CALLS_ALL") != nullptr;  // every call, up to 5 per second
        if (all) {
            static int64_t sec = 0; static int n = 0;
            int64_t now = now_ms() / 1000;
            if (now != sec) { sec = now; n = 0; }
            if (n++ < 5) fprintf(stdout, "[call] %lld %08X lr=%08X r3=%08X r4=%08X\n", (long long)now, t.addr, ctx.lr, ctx.r[3], ctx.r[4]);
        }
    }
    if (fresh) {
        t.printed++;
        fprintf(stdout, "[calls] t=%.2f ", double(now_ms() % 1000000) / 1000.0);
        fprintf(stdout, "[calls] %08X from %08X", t.addr, ctx.lr);
        uint32_t sp = ctx.r[1];
        for (int i = 0; i < 10 && sp >= 0x80000000u && sp < guest_ram_end(); i++) {
            uint32_t next = mem_read32(sp);
            if (next < 0x80000000u || next >= guest_ram_end()) break;
            fprintf(stdout, " <- %08X", mem_read32(next + 4));
            sp = next;
        }
        fprintf(stdout, "  r3=%08X r4=%08X f1=%g\n", ctx.r[3], ctx.r[4], ctx.ps0[1]);
    }
    PPCFunc* slot = ssne_hook_slot(t.addr);
    *slot = nullptr;
    t.orig(ctx);
    *slot = &trace_tramp<N>;
}
PPCFunc kTramps[8] = {trace_tramp<0>, trace_tramp<1>, trace_tramp<2>, trace_tramp<3>,
                      trace_tramp<4>, trace_tramp<5>, trace_tramp<6>, trace_tramp<7>};
void install_call_tracer() {
    const char* e = getenv("SSNE_TRACE_CALLS");
    if (!e) return;
    std::string list = e;
    size_t pos = 0;
    while (pos < list.size() && g_ct_n < 8) {
        size_t c = list.find(',', pos);
        uint32_t a = uint32_t(strtoul(list.substr(pos, c - pos).c_str(), nullptr, 16));
        const SSNEFuncEntry* f = ssne_find_func(a);
        if (f && !*ssne_hook_slot(a)) {
            g_ct[g_ct_n].addr = a;
            g_ct[g_ct_n].orig = f->func;
            ssne_install_hook(a, kTramps[g_ct_n]);
            g_ct_n++;
        } else fprintf(stdout, "[calls] can't trace %08X\n", a);
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    std::thread([] {
        std::vector<uint64_t> last(8, 0);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            fprintf(stdout, "[calls/s]");
            for (int i = 0; i < g_ct_n; i++) { uint64_t c = g_ct[i].calls; fprintf(stdout, " %08X:%llu", g_ct[i].addr, (unsigned long long)(c - last[i])); last[i] = c; }
            fprintf(stdout, "\n");
            fflush(stdout);
        }
    }).detach();
}
} // namespace

// ---- vehicles. The player's per-frame update (800BEEC8, r3 = player) runs on foot and while driving; the
// player's vehicle (CcBaseVehicle::Enter / Exit) is at +0x6C4, 0 on foot. The jeep's physics only work at the
// original time step: above ~120 fps it jolts, and once stopped it no longer drives or steers (it still does at
// 60 fps from the same state). So frames are paced at 60 fps while the player is in a vehicle.
namespace {
const uint32_t kPlayerUpdateSig[6] = {0x9421FF18u, 0x7C0802A6u, 0xDB6100C0u, 0xDB8100C8u, 0xDBA100D0u, 0xDBC100D8u};
PPCFunc g_orig_player_update = nullptr;
detail::NativeVehicleInputCache g_vehicle_input;
std::atomic<bool> g_vehicle_presentation_blocked{false};
void reset_vehicle_input(bool awaiting_world=false) {
    g_vehicle_input.reset();g_vehicle_presentation_blocked=awaiting_world;
}
// Guest-thread publication only. UI queries never inspect actor/vehicle RAM.
bool vehicle_actor_driving(uint32_t actor,bool network) {
    if(actor<0x80000000u || actor>guest_ram_end()-0x790u)return false;
    const auto vehicle=mem_read32(actor+0x6C4);
    if(vehicle<0x80000000u || vehicle>guest_ram_end()-0x164u)return false;
    if(!network)return true; // retain the original single-player vehicle detector.
    const float health=u2f(mem_read32(actor+0xCC));
    return mem_read32(actor+0x48)==game_address(0x80212268u) &&
           std::isfinite(health) && health>0 && mem_read32(actor+0x340)==3 &&
           mem_read32(vehicle+0x160)==actor;
}
void publish_vehicle_presentation() {
    g_vehicle_presentation_blocked=native_movie_presenting() ||
        native_cutscene_presenting() || native_results_presenting();
}

// ---- weapon number keys. The player's weapons by id: 0 chainsaw, 1 Desert Hawk, 2 Uzis, 3 double shotgun,
// 4 minigun, 5 grenade launcher, 6 rocket launcher, 7 flamethrower (gas gun), 8 sniper rifle, 9 Sirian power gun,
// 10 cannon, 11 Serious Bomb; player+0x5DC is the weapon in hand, +0x5F8 the one highlighted in the weapon bar /
// wheel, +0x5E0 the previous one. Next weapon (800C59CC, X) steps the highlight to the next id that is owned
// (800C6170) and has ammo (800C61A0) and marks a switch: the weapon in hand lowers, and the weapon update (800C4EF8,
// part of the player update) then commits the highlight (800C5C18) and raises it. A number key picks its group's
// first available weapon (the group's next one if one of them is already in hand or highlighted), taps X so the game
// starts a normal switch, and once the game has taken the tap (highlight != weapon in hand) puts the chosen id into
// the highlight: the game finishes the switch with it. Same addresses on PAL (unchanged below 800E270C).
const int kWeaponGroups[9][2] = {{0, -1}, {1, -1}, {3, -1}, {2, 4}, {6, 5}, {9, 7}, {8, -1}, {10, -1}, {11, -1}};
const uint32_t kWeaponOwned = 0x800C6170, kWeaponHasAmmo = 0x800C61A0;
const uint32_t kWeaponOwnedSig[6] = {0x1C840018u, 0x386304CCu, 0x39200000u, 0x7C03202Eu, 0x2C000000u, 0x40820010u};
const uint32_t kWeaponHasAmmoSig[6] = {0x9421FFE8u, 0x7C0802A6u, 0xBF810008u, 0x9001001Cu, 0x1FA40018u, 0x7C7C1B78u};
PPCFunc g_weapon_owned = nullptr, g_weapon_has_ammo = nullptr;
std::atomic<int> g_weapon_req{-1};
std::atomic<int64_t> g_weapon_req_ms{0};
int g_weapon_targets[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
int64_t g_weapon_deadlines[8] = {};
int g_coop_weapon_groups[8] = {-1,-1,-1,-1,-1,-1,-1,-1}; // guest thread only

bool weapon_available(PPCContext& ctx, uint32_t player, int id) {
    PPCContext saved = ctx;  // the game's functions run on the caller's registers; give them back untouched
    ctx.r[3] = player; ctx.r[4] = uint32_t(id);
    g_weapon_owned(ctx);
    bool ok = ctx.r[3] != 0;
    if (ok) {
        ctx.r[3] = player; ctx.r[4] = uint32_t(id);
        g_weapon_has_ammo(ctx);
        ok = ctx.r[3] != 0;
    }
    ctx = saved;
    return ok;
}

void weapon_keys(PPCContext& ctx, uint32_t player, int port) {
    if(port<0 || port>=8)return;
    int& g_weapon_target=g_weapon_targets[port];
    int64_t& g_weapon_deadline=g_weapon_deadlines[port];
    int cur = int(mem_read32(player + 0x5DC)), sel = int(mem_read32(player + 0x5F8));
    if (cur < 0 || cur > 11 || sel < 0 || sel > 11) return;
    int group = g_netplay_active?std::exchange(g_coop_weapon_groups[port],-1):g_weapon_req.exchange(-1);
    if (group >= 0 && group < 9 && (g_netplay_active || now_ms() - g_weapon_req_ms.load() < 500)) {
        const int* g = kWeaponGroups[group];
        int n = g[1] < 0 ? 1 : 2, from = 0, base = -1;
        for (int i = 0; i < n; i++) if (g[i] == sel || (base < 0 && g[i] == cur)) { base = g[i]; from = i + 1; }
        int target = -1;
        for (int k = 0; k < n && target < 0; k++) {
            int id = g[(from + k) % n];
            if (id != base && weapon_available(ctx, player, id)) target = id;
        }
        if(getenv("SSNE_TEST_HIDDEN"))log_message("[coop-weapon-group-request] slot="+std::to_string(port)+" group="+std::to_string(group)+" current="+std::to_string(cur)+" selected="+std::to_string(sel)+" target="+std::to_string(target));
        if (target >= 0 && target != cur) {
            if (sel != cur) mem_write32(player + 0x5F8, uint32_t(target));  // a switch is under way: retarget it
            else { inject_buttons(PAD_X, 60, port); g_weapon_target = target; g_weapon_deadline = now_ms() + 500; }
        }
    }
    if (g_weapon_target >= 0) {
        if (sel != cur) {
            mem_write32(player + 0x5F8, uint32_t(g_weapon_target));
            if(getenv("SSNE_TEST_HIDDEN"))log_message("[coop-weapon-group-applied] slot="+std::to_string(port)+" target="+std::to_string(g_weapon_target));
            g_weapon_target = -1;
        }
        else if (now_ms() > g_weapon_deadline) g_weapon_target = -1;
    }
}

uint32_t g_port_player[4] = {};  // the player each controller port played in its last update (cheats)
int64_t g_port_player_ms[4] = {};

void hle_player_update(PPCContext& ctx) {
    uint32_t player = ctx.r[3];
    bool valid = player >= 0x80000000u && player < 0x81800000u;
    if (valid) { g_port_player[player_port(player)] = player; g_port_player_ms[player_port(player)] = now_ms(); }
    // A physics step runs in this update (see latch_jump) unless its accumulator ends up at exactly its old value
    // plus the frame time (fixed-step mode without a whole step; the variable-step mode always steps).
    float acc = valid ? u2f(mem_read32(player + 0x140)) : 0.f, dt = u2f(mem_read32(ctx.r[13] + 0xFFFF85F8u));
    uint32_t controller = valid ? mem_read32(player + 0x344) : 0;
    const int port = valid ? player_port(player) : 0;
    float step = 0;
    if (valid) {
        uint32_t step_src = mem_read32(player + 0x23C);
        if (step_src >= 0x80000000u && step_src < 0x81800000u) step = u2f(mem_read32(step_src + 0x24));
        if (!(step > 0 && step < 0.1f)) step = 0;
    }
    if (g_switch_boost[port] > 0 && step > 0) {  // the frames after a controller switch step like 60 fps frames (below)
        g_switch_boost[port]--;
        if (acc + dt < 2 * step) { acc = 2 * step - dt; mem_write32(player + 0x140, f2u(acc)); }
    }
    PPCFunc* slot = ssne_hook_slot(g_ver->player_update);
    *slot = nullptr;
    g_orig_player_update(ctx);
    *slot = &hle_player_update;
    if (!valid) return;
    bool stepped = mem_read32(player + 0x140) != f2u(acc + dt);
    if (stepped) g_jump_latched_ms[port] = -1;
    // Jumping out of water switches from the swim controller to the walking one: in the next frame it sets Sam's
    // upward speed, in the one after his physics steps move him up, and after every frame a water check switches
    // back to swimming while he is still too deep. At 60 fps that second frame runs two steps (1/60 s) and clears the
    // depth; above 120 fps it ran none or one, the check switched back, and every attempt failed. So after a switch
    // at frame times shorter than 3/4 of 1/60 s, the next two frames run two steps each, as at 60 fps (brought
    // forward: the accumulator goes negative and keeps the average rate).
    if (step > 0 && dt < 0.75f / 60 && mem_read32(player + 0x344) != controller) g_switch_boost[port] = 2;
    g_player_seen = true;
    const auto vehicle=mem_read32(player+0x6C4);
    g_vehicle_input.observe(port,vehicle_actor_driving(player,g_netplay_active.load()),now_ms());
    if(!g_netplay_active)publish_vehicle_presentation();
    if (!(vehicle>=0x80000000u && vehicle<0x81800000u) && g_weapon_owned && g_weapon_has_ammo && port == g_kbm_port.load()) weapon_keys(ctx, player, port);
}

void install_vehicle_detector() {
    const SSNEFuncEntry* f = ssne_find_func(g_ver->player_update);
    bool ok = f;
    for (int k = 0; k < 6 && ok; k++) ok = mem_read32(g_ver->player_update + uint32_t(k) * 4) == kPlayerUpdateSig[k];
    if (!ok) { fprintf(stdout, "[ssne] vehicles: player update not found (different game build?)\n"); return; }
    g_orig_player_update = f->func;
    ssne_install_hook(g_ver->player_update, hle_player_update);
    g_control_hooks++;
    const SSNEFuncEntry* own = ssne_find_func(kWeaponOwned);
    const SSNEFuncEntry* ammo = ssne_find_func(kWeaponHasAmmo);
    bool wk = own && ammo;
    for (int k = 0; k < 6 && wk; k++)
        wk = mem_read32(kWeaponOwned + uint32_t(k) * 4) == kWeaponOwnedSig[k] &&
             mem_read32(kWeaponHasAmmo + uint32_t(k) * 4) == kWeaponHasAmmoSig[k];
    if (wk) { g_weapon_owned = own->func; g_weapon_has_ammo = ammo->func; }
    else fprintf(stdout, "[ssne] weapon keys: weapon functions not found (different game build?)\n");
}

bool in_vehicle() { return g_vehicle_input.any(now_ms()); }
} // namespace

// ---- frame rate limit. The game measures real frame time (median of the last 5 frames, 8015544C) and
// CcEngine::Flip waits for the next VI retrace when no retrace happened since the previous flip, capping it at
// 59.94 fps. With a different limit that wait is skipped and frames are paced on the host clock instead
// (frame_limiter_wait, called at each display copy). Limits above 60 (and unlimited) only apply while playing on
// foot (the look update runs every gameplay frame): everywhere else the game uses its own 60 fps pacing. The front
// end, the pause screen and cutscenes animate per frame (menu lines scrolled, the player model spun and the title's
// bomb pulsed twice as fast at 120 fps), and vehicles need the original time step (see in_vehicle; the look update
// doesn't run while driving).
namespace {
std::atomic<int> g_frame_limit{60};
int effective_frame_limit() {
    int lim = g_frame_limit.load();
    if (lim > 0 && lim <= 60) return lim;
    if (!mouse_aim_available()) return in_vehicle() ? 60 : lim;  // look update not found: only vehicles are known
    return mouse_aim_active() ? lim : 60;
}
PPCFunc g_orig_vi_wait = nullptr;
// CcEngine::Flip's call to VIWaitForRetrace returns here (USA 80118560)
uint32_t flip_wait_return() { return g_ver->flip_bl + 4; }
bool g_flip_wait_ok = false;

void hle_VIWaitForRetrace(PPCContext& ctx) {
    const bool variable_clock=!g_netplay_active || g_authoritative_gameplay;
    if(g_flip_wait_ok && ctx.lr==flip_wait_return() && effective_frame_limit()!=60 && variable_clock)return;
    ssne_call_original(ctx,g_ver->vi_wait,g_orig_vi_wait);
}

void install_frame_limiter() {
    const SSNEFuncEntry* f = ssne_find_func(g_ver->vi_wait);
    if (!f || !f->name || strcmp(f->name, "VIWaitForRetrace") != 0) return;
    // Check Flip's call site: bl VIWaitForRetrace.
    uint32_t insn = mem_read32(g_ver->flip_bl);
    int32_t off = int32_t(insn << 6) >> 6 & ~3;
    g_flip_wait_ok = (insn & 0xFC000003u) == 0x48000001u && uint32_t(g_ver->flip_bl + off) == g_ver->vi_wait;
    if (!g_flip_wait_ok) { fprintf(stdout, "[ssne] frame limiter: Flip not found (different game build?)\n"); return; }
    g_orig_vi_wait = f->func;
    ssne_install_hook(g_ver->vi_wait, hle_VIWaitForRetrace);
}
} // namespace

// ---- mouse aim. The player's look update (800BFCA4, once per gameplay frame, r3 = player) turns the C-stick
// into rates: yaw (+0x290, radians, wrapped with fmod 2 pi, + = right) and pitch (+0x294, radians, + = down, clamped to
// [player+0x194]->+0x24 .. [player+0x198]->+0x24 degrees). Mouse aim runs the original update (so the pads and
// the game's own logic still apply) and then adds the host's accumulated mouse rotation directly: 1:1, no
// acceleration, independent of frame rate.
namespace {
#define kLookUpdate (g_ver->look_update)
const uint32_t kLookUpdateSig[6] = {0x9421FFD0u, 0x7C0802A6u, 0xDBC10020u, 0xDBE10028u, 0x93E1001Cu, 0x90010034u};
PPCFunc g_orig_look = nullptr;
std::mutex g_aim_mutex;
double g_aim_yaw[8]{}, g_aim_pitch[8]{};
std::atomic<int64_t> g_aim_last_ms[8]{{-100000},{-100000},{-100000},{-100000},{-100000},{-100000},{-100000},{-100000}};
std::atomic<bool> g_aim_multiplayer{false};
NetplayStep g_netplay_step;
std::atomic<int> g_netplay_local_player{0};
uint32_t g_netplay_render_engine = 0, g_netplay_render_sda = 0;
PPCFunc g_netplay_render_original = nullptr;
PPCFunc g_netplay_list_render=nullptr, g_netplay_view_create=nullptr;
int g_netplay_list_owner=-1;
unsigned g_netplay_count=2;
uint32_t g_netplay_players[8]{};
uint32_t g_netplay_player_types[8]{};
std::set<uint32_t> g_netplay_character_guis;
PPCFunc g_netplay_gui_create = nullptr, g_netplay_gui_assign = nullptr, g_netplay_gui_destroy = nullptr;
PPCFunc g_netplay_random_init = nullptr;
PPCFunc g_netplay_random_next = nullptr;
std::map<uint32_t, std::array<uint32_t, 33>> g_netplay_random_streams;

void netplay_gui_original(uint32_t address, PPCFunc original, PPCFunc hook, PPCContext& ctx) {
    ssne_call_original(ctx, address, original);
}
void hle_netplay_gui_create(PPCContext& ctx) {
    // This caller creates the four independent character-selection panels.
    const bool character_panel = ctx.lr == game_address(0x800FEDD4u);
    netplay_gui_original(game_address(0x800F540C), g_netplay_gui_create, hle_netplay_gui_create, ctx);
    if (character_panel) g_netplay_character_guis.insert(ctx.r[3]);
    if(getenv("SSNE_NATIVE_PARTY_TRACE")) {char line[120];snprintf(line,sizeof(line),"[party-gui-create] gui=%08X lr=%08X character=%d",ctx.r[3],ctx.lr,character_panel);log_message(line);}
}
void hle_netplay_gui_assign(PPCContext& ctx) {
    // Preserve -1 (unassigned) and each character panel's individual port.
    if(getenv("SSNE_NATIVE_PARTY_TRACE")) {char line[120];snprintf(line,sizeof(line),"[party-gui-assign] gui=%08X lr=%08X port=%d character=%d",ctx.r[3],ctx.lr,int32_t(ctx.r[4]),int(g_netplay_character_guis.count(ctx.r[3])));log_message(line);}
    const bool local_replica_gui=g_authoritative_gameplay && replica_simulation() && int32_t(ctx.r[4])==g_netplay_local_player.load();
    if (int32_t(ctx.r[4]) > 0 && !g_netplay_character_guis.count(ctx.r[3]) && !local_replica_gui) ctx.r[4] = 0;
    const bool prepare_party=g_netplay_count>2 && ctx.r[4]>=2 && g_netplay_character_guis.count(ctx.r[3]);
    netplay_gui_original(game_address(0x800F56A4), g_netplay_gui_assign, hle_netplay_gui_assign, ctx);
    if(prepare_party)native_party_prepare_lobby(ctx);
}
void hle_netplay_gui_destroy(PPCContext& ctx) {
    g_netplay_character_guis.erase(ctx.r[3]);
    netplay_gui_original(game_address(0x800F5578), g_netplay_gui_destroy, hle_netplay_gui_destroy, ctx);
}
void hle_netplay_random_init(PPCContext& ctx) {
    // The original's nonzero flag seeds gameplay RNG from OSGetTick. Two
    // peers have different local clocks even when their inputs are identical.
    // Select its built-in deterministic seed path only for native netplay.
    ctx.r[4] = 0;
    netplay_gui_original(game_address(0x8015A504), g_netplay_random_init, hle_netplay_random_init, ctx);
}
void hle_netplay_random_next(PPCContext& ctx) {
    // Rendering (including per-camera effects) and AI share this generator.
    // Give each call site its own stream so local visuals cannot advance AI RNG.
    const uint32_t generator = ctx.r[3];
    if (generator != game_address(0x802AB3DCu)) {
        netplay_gui_original(game_address(0x8015A54C), g_netplay_random_next, hle_netplay_random_next, ctx);
        return;
    }
    std::array<uint32_t, 33> saved;
    for (uint32_t i = 0; i < saved.size(); ++i) saved[i] = mem_read32(generator + i * 4);
    auto [entry, inserted] = g_netplay_random_streams.try_emplace(ctx.lr);
    if (inserted) {
        PPCContext seed = ctx;
        seed.r[4] = ctx.lr; // stable, distinct seed for each original call site
        ssne_call(seed, game_address(0x8015A40C));
    } else {
        for (uint32_t i = 0; i < saved.size(); ++i) mem_write32(generator + i * 4, entry->second[i]);
    }
    netplay_gui_original(game_address(0x8015A54C), g_netplay_random_next, hle_netplay_random_next, ctx);
    for (uint32_t i = 0; i < saved.size(); ++i) {
        entry->second[i] = mem_read32(generator + i * 4);
        mem_write32(generator + i * 4, saved[i]);
    }
}
bool g_netplay_pad_refresh = false;
PPCFunc g_netplay_pad_clamp = nullptr;
void remember_clamped_pad(uint32_t status);
void hle_netplay_pad_clamp(PPCContext& ctx) {
    // Retrace timing is local. A second clamp of the shared cache can change
    // diagonal movement while the simulation is already consuming this tick.
    if (ctx.r[3] == game_address(0x8028E824u) && !g_netplay_pad_refresh) return;
    const auto status=ctx.r[3];
    netplay_gui_original(game_address(0x801C0BAC), g_netplay_pad_clamp, hle_netplay_pad_clamp, ctx);
    // Dynamic Host activation replaces the offline clamp hook. Keep its raw
    // stick cache current: the game's processing hook still restores this cache
    // before rescaling, and otherwise replays the last offline menu position.
    remember_clamped_pad(status);
}
bool g_host_information_z_consumed=false; // guest-thread-owned, until key release
void hle_netplay_pad_read(PPCContext& ctx) {
    // PADRead normally returns PAD_ERR_TRANSFER while asynchronous SI work is
    // pending. Local retrace/transfer timing is not part of the agreed inputs.
    // Netplay's two virtual pads are always present and sampled at the boundary.
    const uint32_t output = ctx.r[3];
    if (output == game_address(0x8028E824u) && !g_netplay_pad_refresh) {
        ctx.r[3] = 0xFFFFFFFFu << (32-std::min(g_netplay_count,4u));
        return;
    }
    for (int port = 0; port < 4; ++port) {
        const uint32_t status = output + uint32_t(port) * 12;
        for (uint32_t i = 0; i < 12; ++i) mem_write8(status + i, 0);
        if (unsigned(port) >= g_netplay_count) {
            mem_write8(status + 10, 0xFF); // PAD_ERR_NO_CONTROLLER
            continue;
        }
        auto pad = get_pad(port);
        // The replica's local pause handler consumes START. Passing that same
        // press into the newly opened native GUI immediately closes it again.
        if(g_authoritative_gameplay && mem_read32(game_address(0x802DB0E0))==4 && port==g_netplay_local_player.load()){
            if(replica_simulation())pad.buttons&=~(PAD_START|PAD_Z);
            else if(g_host_information_z_consumed)pad.buttons&=~PAD_Z;
        }
        mem_write16(status, pad.buttons & 0x1F7Fu);
        mem_write8(status + 2, uint8_t(int(pad.stick_x) - 128));
        mem_write8(status + 3, uint8_t(int(pad.stick_y) - 128));
        mem_write8(status + 4, uint8_t(int(pad.cstick_x) - 128));
        mem_write8(status + 5, uint8_t(int(pad.cstick_y) - 128));
        mem_write8(status + 6, uint8_t(std::max(0, int(pad.trigger_l) - 31)));
        mem_write8(status + 7, uint8_t(std::max(0, int(pad.trigger_r) - 31)));
    }
    ctx.r[3] = 0xFFFFFFFFu << (32-std::min(g_netplay_count,4u)); // connected pads with rumble support
}
GameFrameObserver g_game_tick_observer;
std::function<void(PPCContext&)> g_game_render_begin,g_game_render_end;
uint64_t g_game_tick_number=0;
PPCFunc g_original_tick=nullptr;
void observe_game_tick(PPCContext& ctx) { if(g_game_tick_observer)g_game_tick_observer(ctx,++g_game_tick_number); }
void hle_game_tick(PPCContext& ctx){observe_game_tick(ctx);ssne_call_original(ctx,game_address(0x8015544Cu),g_original_tick);}
uint64_t g_netplay_tick = 0;
std::atomic<bool> g_netplay_world_ready{false};
std::array<std::string, 32> g_netplay_history;
void refresh_network_pads(PPCContext& ctx) {
    PPCContext pads=ctx; pads.msr&=~0x8000u;
    g_netplay_pad_refresh=true;
    pads.r[3]=game_address(0x8028E824);
    ssne_call(pads,game_address(0x801C193C));
    pads.r[3]=game_address(0x8028E824);
    ssne_call(pads,game_address(0x801C0BAC));
    g_netplay_pad_refresh=false;
}
PPCFunc g_authoritative_pause_original=nullptr,g_authoritative_netricsa_original=nullptr;
PPCFunc g_authoritative_control_gate_original=nullptr;
PPCFunc g_authoritative_player_start_original=nullptr;
void hle_authoritative_player_start(PPCContext& ctx) {
    const auto actor=ctx.r[3];
    if(!native_party_prepare_spawn(ctx,actor,ctx.r[4]))return;
    ssne_call_original(ctx,0x800B2E5C,g_authoritative_player_start_original);
    native_party_spawn_completed(actor);
}
PPCFunc g_authoritative_spawn_telefrag_original=nullptr;
void hle_authoritative_spawn_telefrag(PPCContext& ctx) {
    const auto actor=ctx.r[3],position=ctx.r[4];
    if(replica_simulation())return;
    if(native_party_extended() && mem_read32(game_address(0x802DB1BC))==1 && actor>=0x80000000u && actor<=guest_ram_end()-0x790 && position>=0x80000000u && position<=guest_ram_end()-12 && mem_read32(actor+0x48)==game_address(0x80212268u)) {
        for(unsigned slot=0;slot<g_netplay_count;++slot) {
            const auto other=mem_read32(native_party_registry()+slot*4);
            if(other==actor || other<0x80000000u || other>guest_ram_end()-0x790 || mem_read32(other+0x48)!=game_address(0x80212268u) || u2f(mem_read32(other+0xCC))<=0)continue;
            PPCContext check=ctx;check.msr&=~0x8000u;check.r[1]-=0x180;mem_write32(check.r[1],ctx.r[1]);check.r[3]=actor;check.r[4]=position;check.r[5]=other;
            ssne_call(check,0x800A3408);
            if(check.r[3])fatal("Native cooperative spawn overlaps a living teammate; placement needs a validated alternative.");
        }
        log_message("[coop-spawn] native teammate-overlap check passed for slot="+std::to_string(mem_read32(actor+0x17C)));
    }
    ssne_call_original(ctx,0x800A369C,g_authoritative_spawn_telefrag_original);
}
PPCFunc g_authoritative_player_death_original=nullptr;
void hle_authoritative_player_death(PPCContext& ctx) {
    const auto actor=ctx.r[3],info=ctx.r[4];
    if(actor>=0x80000000u && actor<=guest_ram_end()-0x790 && mem_read32(actor+0x48)==game_address(0x80212268u)) {
        std::string event="[coop-player-death] slot="+std::to_string(mem_read32(actor+0x17C))+" health="+std::to_string(u2f(mem_read32(actor+0xCC)))+" caller="+std::to_string(ctx.lr)+" info="+std::to_string(info);
        if(info>=0x80000000u && info<=guest_ram_end()-16)event+=" type="+std::to_string(mem_read32(info+4))+" source="+std::to_string(mem_read32(info+8))+" flags="+std::to_string(mem_read32(info+12));
        log_message(event);
        uint32_t stack=ctx.r[1];for(unsigned i=0;i<6 && stack>=0x80000000u && stack<=guest_ram_end()-8;++i){auto previous=mem_read32(stack);if(previous<=stack || previous>guest_ram_end()-8)break;log_message("[coop-death-stack] return="+std::to_string(mem_read32(previous+4)));stack=previous;}
    }
    ssne_call_original(ctx,0x800B3510,g_authoritative_player_death_original);
}
PPCFunc g_authoritative_movement_mode_original=nullptr;
void hle_authoritative_movement_mode(PPCContext& ctx) {
    const auto actor=ctx.r[3];const auto mode=ctx.r[4];const auto caller=ctx.lr;
    if(actor>=0x80000000u && actor<=guest_ram_end()-0x790 && mem_read32(actor+0x48)==game_address(0x80212268u)) {
        log_message("[coop-movement-mode] slot="+std::to_string(mem_read32(actor+0x17C))+" mode="+std::to_string(mode)+" caller="+std::to_string(caller));
        uint32_t stack=ctx.r[1];for(unsigned i=0;i<4 && stack>=0x80000000u && stack<=guest_ram_end()-8;++i){auto previous=mem_read32(stack);if(previous<=stack || previous>guest_ram_end()-8)break;log_message("[coop-mode-stack] return="+std::to_string(mem_read32(previous+4)));stack=previous;}
    }
    ssne_call_original(ctx,0x800B5A78,g_authoritative_movement_mode_original);
}
PPCFunc g_authoritative_player_collision_original=nullptr;
void hle_authoritative_player_collision(PPCContext& ctx){
    // 800A92A8 passes ground motion and an additional displacement separately.
    // An exactly stationary sweep can make the original solver return an invalid
    // distant position instead of this player's position (Rlevel1_4a reproduction).
    // Retain the native solver/contact/material handling, but give a degenerate
    // ground sweep a 1-mm probe along its existing gravity direction.
    const auto actor=ctx.r[4],motion=ctx.r[8],extra=ctx.r[9],position=ctx.r[10],direction=ctx.r[6];
    auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && p<=guest_ram_end()-n;};
    bool stationary=ctx.lr==0x800A92A8u && readable(actor,0x790) && mem_read32(actor+0x48)==game_address(0x80212268u) &&
        readable(motion,12) && readable(extra,12) && readable(position,12) && readable(direction,12);
    std::array<uint32_t,3> saved{};std::array<float,3> step{},down{};double length2=0;
    if(stationary)for(unsigned i=0;i<3;++i){
        saved[i]=mem_read32(motion+4*i);step[i]=u2f(saved[i]);down[i]=u2f(mem_read32(direction+4*i));
        const auto point=u2f(mem_read32(position+4*i)),offset=u2f(mem_read32(extra+4*i));
        const float combined=step[i]+offset;
        stationary &= std::isfinite(point) && std::isfinite(combined) && std::isfinite(down[i]) && float(point+combined)==point;
        length2+=double(down[i])*down[i];
    }
    stationary &= length2>.5 && length2<1.5;
    struct RestoreMotion {uint32_t pointer;std::array<uint32_t,3> words;bool active;~RestoreMotion(){if(active)for(unsigned i=0;i<3;++i)mem_write32(pointer+4*i,words[i]);}} restore{motion,saved,stationary};
    if(stationary){const float distance=.001f/float(std::sqrt(length2));for(unsigned i=0;i<3;++i)mem_write32(motion+4*i,f2u(step[i]+down[i]*distance));}
    ssne_call_original(ctx,0x800C04ACu,g_authoritative_player_collision_original);
}
PPCFunc g_authoritative_player_update_original=nullptr;
void hle_authoritative_player_update(PPCContext& ctx) {
    const auto actor=ctx.r[3];
    if(!native_party_actor_occupied(actor)){static std::array<uint64_t,8> skipped{};const auto slot=mem_read32(actor+0x17C);if(slot<8 && ++skipped[slot]%120==0)log_message("[coop-reserved-player-update-skipped] slot="+std::to_string(slot)+" skips="+std::to_string(skipped[slot]));return;}
    if(native_party_retry_spawn(ctx,actor))return;auto slot=mem_read32(actor+0x17C);static std::array<uint64_t,8> updates{};
    if(slot<8 && ++updates[slot]%120==0)log_message("[coop-player-dispatch] slot="+std::to_string(slot)+" updates="+std::to_string(updates[slot])+" mode="+std::to_string(mem_read32(actor+0x370))+" flags="+std::to_string(mem_read32(actor+0x1C))+" frozen="+std::to_string(mem_read32(actor+0x144)));
    std::array<float,3> idle_before;for(unsigned i=0;i<3;++i)idle_before[i]=u2f(mem_read32(actor+0x20+i*4));
    ssne_call_original(ctx,0x800BEEC8,g_authoritative_player_update_original);
    native_party_stabilize_idle(ctx,actor,idle_before);
    if(slot<8 && actor>=0x80000000u && actor<=guest_ram_end()-0x790){
        g_player_seen=true;
        const auto vehicle=mem_read32(actor+0x6C4);
        g_vehicle_input.observe(int(slot),vehicle_actor_driving(actor,true),now_ms());
        if(!(vehicle>=0x80000000u && vehicle<guest_ram_end()) && g_weapon_owned && g_weapon_has_ammo)weapon_keys(ctx,actor,int(slot));
    }
}
PPCFunc g_authoritative_freeze_original=nullptr,g_authoritative_release_original=nullptr;
void trace_player_freeze(PPCContext& ctx,bool frozen,PPCFunc original,uint32_t function) {
    const auto actor=ctx.r[3];const auto caller=ctx.lr;
    if(actor>=0x80000000u && actor<=guest_ram_end()-0x790 && mem_read32(actor+0x48)==game_address(0x80212268u)) {
        log_message("[coop-player-freeze] slot="+std::to_string(mem_read32(actor+0x17C))+" frozen="+std::to_string(frozen)+" caller="+std::to_string(caller)+" reason="+std::to_string(ctx.r[4]));
        uint32_t stack=ctx.r[1];
        for(unsigned i=0;i<4 && stack>=0x80000000u && stack<=guest_ram_end()-8;++i){auto previous=mem_read32(stack);if(previous<=stack || previous>guest_ram_end()-8)break;log_message("[coop-freeze-stack] return="+std::to_string(mem_read32(previous+4)));stack=previous;}
    }
    ssne_call_original(ctx,function,original);
    if(actor>=0x80000000u && actor<=guest_ram_end()-0x790)log_message("[coop-freeze-result] slot="+std::to_string(mem_read32(actor+0x17C))+" mode="+std::to_string(mem_read32(actor+0x370))+" flags="+std::to_string(mem_read32(actor+0x1C)));
}
void hle_player_freeze(PPCContext& ctx){trace_player_freeze(ctx,true,g_authoritative_freeze_original,0x800B31B8);}
void hle_player_release(PPCContext& ctx){trace_player_freeze(ctx,false,g_authoritative_release_original,0x800B3398);}
void hle_authoritative_control_gate(PPCContext& ctx) {
    const auto actor=ctx.r[3];const auto caller=ctx.lr;
    ssne_call_original(ctx,0x800C4EEC,g_authoritative_control_gate_original);
    if(actor>=0x80000000u && actor<=guest_ram_end()-0x790 && mem_read32(actor+0x48)==game_address(0x80212268u)) {
        const auto slot=mem_read32(actor+0x17C);static std::array<uint64_t,8> samples{};
        if(slot<8 && ++samples[slot]%120==0)log_message("[coop-control-gate] slot="+std::to_string(slot)+" samples="+std::to_string(samples[slot])+" blocked="+std::to_string(ctx.r[3])+" caller="+std::to_string(caller));
    }
}
thread_local unsigned g_authoritative_netricsa_depth=0;
uint32_t g_authoritative_information_gui=0; // guest-thread-owned
std::atomic<bool> g_authoritative_information_open{false},g_replica_control_active{false};
uint32_t g_authoritative_information_object=0;
std::map<unsigned,uint32_t> g_information_cameras;
PPCFunc g_information_camera_setter=nullptr;
bool information_camera(unsigned slot,uint32_t engine=0){
    if(!engine)engine=g_netplay_render_engine;
    if(!g_authoritative_information_object || !engine || g_authoritative_information_object>guest_ram_end()-0xA4 || mem_read32(g_authoritative_information_object+0x48)!=0x80219030u)return false;
    auto it=g_information_cameras.find(slot);if(it==g_information_cameras.end())return false;
    const auto view=native_party_view_engine(engine)+0xDC+slot*76;
    return mem_read32(g_authoritative_information_object+0x6C)!=0 && mem_read32(view)==it->second;
}
void hle_information_camera_set(PPCContext& ctx){
    const auto engine=ctx.r[3],slot=native_party_view_slot(ctx.r[4]),camera=ctx.r[5],caller=ctx.lr;
    ssne_call_original(ctx,0x8011A1F8u,g_information_camera_setter);
    if((g_authoritative_netricsa_depth || (g_authoritative_information_open && caller>=0x800F0000u && caller<0x80104000u)) && slot<32 && camera &&
       mem_read32(native_party_view_engine(engine)+0xDC+slot*76)==camera)g_information_cameras[slot]=camera;
}
std::atomic<bool> g_replica_pause_menu_open{false};
void update_replica_pause_menu(PPCContext& ctx,uint32_t player,unsigned slot,bool valid){
    if(native_replica_results_presenting()){g_replica_pause_menu_open=false;return;}
    if(g_authoritative_information_open){g_replica_pause_menu_open=false;return;}
    static uint32_t previous_player=0;static bool start_down=false;
    const bool down=slot<8 && (get_pad(int(slot)).buttons&PAD_START)!=0;
    const bool pressed=down && !start_down;start_down=down;
    if(!valid || !replica_simulation()){g_replica_pause_menu_open=false;previous_player=0;return;}
    if(previous_player!=player){previous_player=player;start_down=down;}
    const auto gui=mem_read32(player+0x71C);
    if(gui<0x80000000u || gui>guest_ram_end()-0x104u){g_replica_pause_menu_open=false;return;}
    if(int16_t(mem_read16(gui+0x84))!=int(slot)){
        if(!ssne_find_func(0x800F56E0u))fatal("Original pause GUI controller binding unavailable");
        auto call=ctx;call.r[1]-=0x200;mem_write32(call.r[1],ctx.r[1]);call.r[3]=gui;call.r[4]=slot;ssne_call(call,0x800F56E0u);
    }
    const bool open=(mem_read32(ctx.r[13]-0x765Cu)&(1u<<slot))!=0;
    if(pressed && !native_movie_presenting() && !g_authoritative_information_open){
        const auto function=open?0x800F9AACu:0x800F9978u;
        if(!ssne_find_func(function) || mem_read32(function)!=(open?0x9421FFF0u:0x9421FFE8u))fatal("Original local pause menu unavailable");
        auto call=ctx;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],ctx.r[1]);
        call.r[3]=gui;call.r[4]=open?0u:0x129u;call.r[5]=0x134u;call.r[6]=ctx.r[13]-0x53A0u;
        // Exactly the player's native pause action (800BF764), without its
        // movement, weapon, damage or world update. Only this process pauses.
        ssne_call(call,function);
        log_message(std::string("[coop-client-local-menu] open=")+(open?"0":"1")+" slot="+std::to_string(slot));
    }
    g_replica_pause_menu_open=(mem_read32(ctx.r[13]-0x765Cu)&(1u<<slot))!=0;
}
void hle_authoritative_netricsa(PPCContext& ctx) {
    const auto object=ctx.r[3];
    if(object>=0x80000000u && object<=guest_ram_end()-0x98) {
        auto player=mem_read32(object+0x90),gui=mem_read32(object+0x94);
        if(player>=0x80000000u && player<=guest_ram_end()-0x790u && mem_read32(player+0x48)==game_address(0x80212268u)) {
            const auto owner=mem_read32(player+0x17C);
            if(owner!=g_netplay_local_player.load()) {
                log_message("[coop-netricsa] remote player's information panel stays off the local view");return;
            }
            g_authoritative_information_gui=gui;
            if(g_authoritative_information_object!=object)g_information_cameras.clear();
            g_authoritative_information_object=object;
        }
        char trace[180];snprintf(trace,sizeof(trace),"[coop-netricsa-owner] object=%08X player=%08X slot=%u gui=%08X gui_port=%d",object,player,player>=0x80000000u && player<=guest_ram_end()-0x180?mem_read32(player+0x17C):999,gui,gui>=0x80000000u && gui<=guest_ram_end()-0x86?int(int16_t(mem_read16(gui+0x84))):-99);log_message(trace);
    }
    struct InformationScope {InformationScope(){++g_authoritative_netricsa_depth;}~InformationScope(){--g_authoritative_netricsa_depth;}} scope;
    ssne_call_original(ctx,0x80101F4C,g_authoritative_netricsa_original);
}
void hle_authoritative_pause(PPCContext& ctx) {
    // Netricsa is a player's information panel, not a shared campaign cutscene.
    // Its normal single-player pause must not freeze every connected player.
    // Other native pause requests still use the original scene/audio handler.
    if(ctx.r[3] && g_authoritative_netricsa_depth && mem_read32(game_address(0x802DB0E0))==4 && mem_read32(game_address(0x802DB1BC))==1) {
        log_message("[coop-netricsa] information panel opened without pausing the shared world");return;
    }
    char trace[180];snprintf(trace,sizeof(trace),"[coop-native-pause] enabled=%u reason=%u caller=%08X",ctx.r[3],ctx.r[4],ctx.lr);
    log_message(trace);
    snprintf(trace,sizeof(trace),"[coop-pause-origin] object=%08X argument=%08X bank=%08X secondary=%08X",ctx.r[29],ctx.r[26],mem_read32(0x802ABA9C),native_party_second_bank());log_message(trace);
    uint32_t stack=ctx.r[1];
    for(unsigned i=0;i<8 && stack>=0x80000000u && stack<=guest_ram_end()-8;++i) {
        auto previous=mem_read32(stack);if(previous<=stack || previous>guest_ram_end()-8)break;
        snprintf(trace,sizeof(trace),"[coop-pause-stack] frame=%u sp=%08X return=%08X",i,stack,mem_read32(previous+4));log_message(trace);stack=previous;
    }
    for(unsigned slot=0;slot<g_netplay_count;++slot) {
        const auto bank=slot<4?mem_read32(0x802ABA9C):native_party_second_bank();
        const auto status=slot<4?game_address(0x8028E824u)+slot*12:native_party_second_status()+(slot-4)*12;
        const auto input=get_pad(slot);
        snprintf(trace,sizeof(trace),"[coop-pause-pad] slot=%u connected=%u status=%04X error=%d requested=%04X",slot,bank?mem_read32(bank+(slot%4)*0x70C):0,mem_read16(status),int(int8_t(mem_read8(status+10))),input.buttons);log_message(trace);
    }
    ssne_call_original(ctx,0x80155BC8,g_authoritative_pause_original);
}
struct CoopHookTimer {
    ssne::perf::Scope performance;
    std::string name;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    explicit CoopHookTimer(std::string n):performance(n),name(std::move(n)){}
    ~CoopHookTimer(){if(!getenv("SSNE_COOP_PROFILE"))return;
        struct Sample{double ms=0;unsigned count=0;};static std::map<std::string,Sample> samples;auto& sample=samples[name];
        sample.ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(++sample.count>=120){log_message("[coop-hook-profile] stage="+std::string(name)+" average_ms="+std::to_string(sample.ms/sample.count));sample={};}
    }
};
PPCFunc g_original_vehicle_physics=nullptr,g_original_vehicle_steering=nullptr;
void hle_authoritative_vehicle_steering(PPCContext& ctx) {
    const auto car=ctx.r[3];
    if(car>=0x80000000u && car<=guest_ram_end()-0x458 && mem_read32(car+0x48)==0x80225568u) {
        const auto driver=mem_read32(car+0x160);
        if(driver>=0x80000000u && driver<=guest_ram_end()-0x790 &&
           mem_read32(driver+0x340)==3 && mem_read32(driver+0x6C4)==car) {
            const auto input=mem_read32(driver+0x178);
            if(input<native_party_capacity()) {
                const auto bank=mem_read32(0x802ABA9Cu)+native_party_controller_offset(input);
                if(bank>=0x80000000u && bank<=guest_ram_end()-0x70C) {
                    // Read the already sampled movement actions, including the
                    // remote driver's bindings and native analog deadzone.
                    // The console controller uses C-stick actions for steering;
                    // PC A/D (left stick X) previously reached an unused axis.
                    const float turn=u2f(mem_read32(bank+0x98))-u2f(mem_read32(bank+0x7C));
                    if(std::isfinite(turn) && turn!=0.f)ctx.ps0[1]=ctx.ps1[1]=std::clamp(turn,-1.f,1.f);
                }
            }
        }
    }
    ssne_call_original(ctx,0x80174D48u,g_original_vehicle_steering);
}
void hle_authoritative_vehicle_physics(PPCContext& ctx) {
    const auto object=ctx.r[3],clock=ctx.r[13]-0x7A08u;
    if(object<0x80000000u || object>guest_ram_end()-0x458 || mem_read32(object+0x48)!=0x80225568u){ssne_call_original(ctx,0x80170948,g_original_vehicle_physics);return;}
    const float dt=u2f(mem_read32(clock));
    // The original rounds dt/(1/60) independently on every rendered frame.
    // Split THIS elapsed interval into equal steps no larger than 1/60 instead.
    // Keep all native suspension, steering, collision and integration routines;
    // the constant is used only inside80170948 on this same guest thread.
    // A scoped replacement also covers its nested physics calls and is restored
    // before another object, render, checkpoint or ordinary clock can run.
    if(!(dt>0))return; // original speed calculation divides by dt.
    if(!std::isfinite(dt) || dt>.25f)fatal("Invalid authoritative vehicle timestep");
    const auto steps=std::max(1u,unsigned(std::ceil(double(dt)*60.-1e-6)));
    struct StepScope {uint32_t saved=mem_read32(0x8022353Cu);explicit StepScope(float step){mem_write32(0x8022353Cu,f2u(step));}~StepScope(){mem_write32(0x8022353Cu,saved);}} step(dt/steps);
    ssne_call_original(ctx,0x80170948,g_original_vehicle_physics);
}
void hle_authoritative_time_step(PPCContext& ctx) {
    CoopHookTimer timer("time_step");
    if(perf::enabled.load(std::memory_order_relaxed)){static auto previous=perf::Clock::now();const auto now=perf::Clock::now();perf::record(perf::index("update_interval"),now-previous);previous=now;}
    observe_game_tick(ctx);
    const unsigned slot=g_netplay_local_player.load();
    const auto player=slot<g_netplay_count?mem_read32(native_party_registry()+slot*4):0;
    const bool valid=player>=0x80000000u && player<=guest_ram_end()-0x790u && mem_read32(player+0x48)==game_address(0x80212268u);
    g_netplay_world_ready=valid && mem_read32(game_address(0x802DB0E0u))==4;
    // The frame observer above has applied replica snapshots. Publish the local
    // received driver too: replica actors do not run the authoritative update.
    g_vehicle_input.observe(int(slot),valid && g_netplay_world_ready && vehicle_actor_driving(player,true),now_ms());
    publish_vehicle_presentation();
    const auto information=valid?mem_read32(player+0x720):0;
    g_authoritative_information_open=information>=0x80000000u && information<=guest_ram_end()-0xA4 && mem_read32(information+0x90)==player && mem_read32(information+0x6C)!=0;
    // Consume the host's close press before sampling native actions. Otherwise
    // the same Z edge closes here and reopens in the ordinary player update.
    // Keep it consumed while held; a new press on a closed panel still opens
    // through the host's original player action.
    if(!valid || !g_netplay_world_ready || replica_simulation() || !(get_pad(int(slot)).buttons&PAD_Z))g_host_information_z_consumed=false;
    else if(g_authoritative_information_open)g_host_information_z_consumed=true;
    refresh_network_pads(ctx);
    update_replica_pause_menu(ctx,player,slot,valid && g_netplay_world_ready);
    // Opening local information does not pause the shared host world. Thus
    // the host cannot depend on the original paused-menu input dispatch to
    // receive Z/Tab. Both roles close through the original deferred GUI path;
    // only replicas need an explicit open action (the host has its player tick).
    if(valid && g_netplay_world_ready){
        static uint32_t previous=0;static bool z_down=false;
        const bool down=(get_pad(int(slot)).buttons&PAD_Z)!=0;
        const bool pressed=previous==player && down && !z_down;
        previous=player;z_down=down;
        const auto info=mem_read32(player+0x720);
        if(pressed && !g_replica_pause_menu_open && !native_movie_presenting() && !native_replica_results_presenting() && info>=0x80000000u && info<=guest_ram_end()-0xA4u && mem_read32(info+0x48)==0x80219030u){
            if(g_authoritative_information_open){
                auto call=ctx;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],ctx.r[1]);
                // Original information close: fade/remove the GUI first. Its
                // 8010236C callback releases page textures after removal.
                // Immediate 801020F8 freed textures still referenced by the
                // next GUI update, corrupting its virtual texture calls.
                call.r[3]=mem_read32(info+0x94);ssne_call(call,0x801022ECu);
                log_message("[coop-local-netricsa-close] slot="+std::to_string(slot)+" replica="+std::to_string(replica_simulation())+" deferred_resources=1");
            }else if(replica_simulation()){
            // Exact information-panel action from800BF764; no player/world tick.
            auto call=ctx;call.msr&=~0x8000u;call.r[1]-=0x200;
            mem_write32(call.r[1],ctx.r[1]);
            // Received inventory bypasses the native pickup notifications.
            // Rebuild only pages for weapons this player actually owns, using
            // the original information-only event and local weapon instances.
            for(unsigned weapon=0;weapon<12;++weapon){
                const auto table=player+0x4BC+weapon*24,instance=mem_read32(table);
                if(!mem_read32(table+16) || instance<0x80000000u || instance>guest_ram_end()-0x228)continue;
                call=ctx;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],ctx.r[1]);
                mem_write32(call.r[1]+0x80,15);mem_write32(call.r[1]+0x84,player);mem_write32(call.r[1]+0x88,instance);
                call.r[3]=info;call.r[4]=call.r[1]+0x80;ssne_call(call,0x80102CF0u);
            }
            call=ctx;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],ctx.r[1]);call.r[3]=info;call.r[4]=0;
            ssne_call(call,0x80101F4Cu);
            log_message("[coop-client-netricsa] slot="+std::to_string(slot));
            }
        }
    }
    g_replica_control_active=replica_simulation() && g_netplay_world_ready && !g_authoritative_information_open && !g_replica_pause_menu_open && u2f(mem_read32(player+0xCC))>0;
    // The server owns gameplay; independent clients no longer require a
    // lockstep dt. Use the original bounded/smoothed wall-clock delta so slow
    // rendering cannot slow the entire world's clock.
    ssne_call_original(ctx,game_address(0x8015544Cu),g_original_tick);
    if(getenv("SSNE_COOP_PROFILE")){
        static auto began=std::chrono::steady_clock::now();static double simulated=0;static unsigned count=0;
        simulated+=ctx.ps0[1];if(++count>=120){const auto now=std::chrono::steady_clock::now();log_message("[coop-clock] simulated_seconds="+std::to_string(simulated)+" wall_seconds="+std::to_string(std::chrono::duration<double>(now-began).count()));began=now;simulated=0;count=0;}
    }
}
void hle_netplay_time_step(PPCContext& ctx) {
    observe_game_tick(ctx);
    uint64_t checksum = native_party_checksum(netplay_world_checksum());
    int player_slot = 0;
    int valid_players = 0;
    for (uint32_t player : g_netplay_players) {
        const uint32_t player_type = g_netplay_player_types[player_slot++];
        bool valid = player >= 0x80000000u && player < (guest_ram_end()-0x360u) && mem_read32(player + 0x48) == player_type;
        checksum ^= valid; checksum *= 1099511628211ull;
        if (!valid) continue;
        ++valid_players;
        for (uint32_t offset : {0x20u, 0x24u, 0x28u, 0x290u, 0x294u}) {
            float v = u2f(mem_read32(player + offset));
            uint32_t q = std::isfinite(v) && std::abs(v) < 1000000.f
                ? uint32_t(int32_t(std::round(double(v) * 10000.))) : 0xFFFFFFFFu;
            checksum ^= q; checksum *= 1099511628211ull;
        }
    }
    if(getenv("SSNE_TRACE_MODS")) {
        if(g_netplay_tick%120==0)log_message("[netplay-alive] tick="+std::to_string(g_netplay_tick+1)+" enemies="+std::to_string(netplay_bot_enemies().size()));
        static std::array<std::array<uint32_t,13>,8> previous_inventory{};
        for(unsigned port=0;port<g_netplay_count;++port) {
            auto player=g_netplay_players[port];
            if(player<0x80000000u || player>=(guest_ram_end()-0x790u) || mem_read32(player+0x48)!=g_netplay_player_types[port])continue;
            std::array<uint32_t,13> inventory{};inventory[0]=player;
            for(unsigned slot=0;slot<12;++slot)inventory[slot+1]=mem_read32(player+0x4BC+slot*24+12);
            if(inventory==previous_inventory[port])continue;
            previous_inventory[port]=inventory;
            std::string line="[netplay-inventory] tick="+std::to_string(g_netplay_tick+1)+" port="+std::to_string(port)+" counts=";
            for(unsigned slot=1;slot<13;++slot) {if(slot>1)line+=",";line+=std::to_string(inventory[slot]);}
            log_message(line);
        }
    }
    if(getenv("SSNE_NATIVE_PARTY_TRACE") && g_netplay_tick%120==0) {
        for(unsigned slot=0;slot<g_netplay_count;++slot) {
            auto actor=g_netplay_players[slot];if(!actor)continue;
            log_message("[party-status] slot="+std::to_string(slot)+" hp="+std::to_string(u2f(mem_read32(actor+0xCC)))+" respawn="+std::to_string(mem_read32(actor+0x3AC))+" timer="+std::to_string(u2f(mem_read32(actor+0x3B4)))+" state="+std::to_string(mem_read32(actor+0x148))+" controller="+std::to_string(mem_read32(actor+0x178))+" ui="+std::to_string(mem_read32(mem_read32(actor+0x374)+0x3E0))+" pos="+std::to_string(u2f(mem_read32(actor+0x20)))+","+std::to_string(u2f(mem_read32(actor+0x28))));
        }
    }
    unsigned mode = mem_read32(game_address(0x802DB1BC));
    // Menu input/scene transitions are not covered by the player checksum.
    // Record changes at the shared boundary, so different peers can be compared
    // without relying on renderer frame numbers or wall-clock timestamps.
    uint32_t scene = mem_read32(game_address(0x802DB0E0u));
    uint32_t pending = mem_read32(game_address(0x802DB0E4u));
    uint32_t buttons = uint32_t(mem_read16(game_address(0x8028E824))) << 16 | mem_read16(game_address(0x8028E830));
    uint32_t errors = uint32_t(mem_read8(game_address(0x8028E82E))) << 8 | mem_read8(game_address(0x8028E83A));
    static std::array<uint32_t,5> previous_menu{~0u,~0u,~0u,~0u,~0u};
    std::array<uint32_t,5> menu{scene,pending,buttons,errors,mode};
    if (menu != previous_menu) {
        char detail[200];
        snprintf(detail,sizeof(detail),"[netplay-menu] tick=%llu scene=%u pending=%u mode=%u buttons=%08X errors=%04X",
            (unsigned long long)(g_netplay_tick+1),scene,pending,mode,buttons,errors);
        log_message(detail);
        previous_menu = menu;
    }
    {
        char detail[320];
        snprintf(detail,sizeof(detail),"[netplay-state] tick=%llu hash=%llu mode=%u players=%d p0=%08X,%08X,%08X,%08X,%08X p1=%08X,%08X,%08X,%08X,%08X",
            (unsigned long long)(g_netplay_tick+1),(unsigned long long)checksum,mode,valid_players,
            (g_netplay_players[0] ? mem_read32(g_netplay_players[0]+0x20) : 0),(g_netplay_players[0] ? mem_read32(g_netplay_players[0]+0x24) : 0),(g_netplay_players[0] ? mem_read32(g_netplay_players[0]+0x28) : 0),(g_netplay_players[0] ? mem_read32(g_netplay_players[0]+0x290) : 0),(g_netplay_players[0] ? mem_read32(g_netplay_players[0]+0x294) : 0),
            (g_netplay_players[1] ? mem_read32(g_netplay_players[1]+0x20) : 0),(g_netplay_players[1] ? mem_read32(g_netplay_players[1]+0x24) : 0),(g_netplay_players[1] ? mem_read32(g_netplay_players[1]+0x28) : 0),(g_netplay_players[1] ? mem_read32(g_netplay_players[1]+0x290) : 0),(g_netplay_players[1] ? mem_read32(g_netplay_players[1]+0x294) : 0));
        g_netplay_history[(g_netplay_tick+1) % g_netplay_history.size()] = detail;
        if (getenv("SSNE_NETPLAY_TRACE")) log_message(detail);
    }
    bool ready = valid_players >= ((mode >= 1 && mode <= 4) ? int(g_netplay_count) : 1), wait_for_world = false;
    g_netplay_world_ready = ready && scene == 4;
    const int local=g_netplay_local_player.load();
    const auto local_actor=local>=0 && unsigned(local)<g_netplay_count?g_netplay_players[local]:0;
    g_vehicle_input.observe(local,g_netplay_world_ready && vehicle_actor_driving(local_actor,true),now_ms());
    publish_vehicle_presentation();
    if (!g_netplay_step(++g_netplay_tick, checksum, ready, wait_for_world)) {
        netplay_log_world_state();
        for (uint64_t t = g_netplay_tick > 31 ? g_netplay_tick - 31 : 1; t <= g_netplay_tick; ++t) {
            auto& state = g_netplay_history[t % g_netplay_history.size()];
            if (!state.empty()) log_message(state);
        }
        char message[240];
        snprintf(message,sizeof(message),"[netplay] tick=%llu mode=%u players=%d world=%llu player0=%08X player1=%08X",(unsigned long long)g_netplay_tick,mode,valid_players,(unsigned long long)netplay_world_checksum(),g_netplay_players[0],g_netplay_players[1]);
        log_message(message);
        fatal("Netplay stopped: connection or simulation mismatch. See netplay status.");
    }
    // The normal VI callback samples into CcEngine's cached PADStatus array.
    // Refresh that cache at the agreed boundary as well, so retrace timing on
    // different PCs cannot make one guest consume the previous tick's input.
    PPCContext pads = ctx;
    // A pending VI callback also reads and clamps this same array. Do not
    // let it run between our PADRead and PADClamp, or the sticks are clamped
    // twice on just one peer (different local interrupt timing).
    pads.msr &= ~0x8000u;
    g_netplay_pad_refresh = true;
    pads.r[3] = game_address(0x8028E824);
    ssne_call(pads, game_address(0x801C193C)); // PADRead
    pads.r[3] = game_address(0x8028E824);
    ssne_call(pads, game_address(0x801C0BAC)); // PADClamp
    g_netplay_pad_refresh = false;
    // Hold only the initialized world. The loading peer must retain a full
    // time step: its first world update can occur later in this same iteration,
    // before the next readiness sample. Returning zero there skips its initial
    // gravity step and permanently diverges from the already-loaded peer.
    ctx.ps0[1] = ctx.ps1[1] = wait_for_world && ready ? 0. : double(float(1. / 59.94));
}

void hle_look_update(PPCContext& ctx) {
    uint32_t player = ctx.r[3];
    if(g_authoritative_gameplay && player>=0x80000000u && player<=guest_ram_end()-0x790 && mem_read32(player+0x48)==game_address(0x80212268u)) {
        auto slot=mem_read32(player+0x17C);
        static std::array<uint64_t,8> updates{};
        if(slot<8 && ++updates[slot]%120==0)log_message("[coop-player-look] slot="+std::to_string(slot)+" updates="+std::to_string(updates[slot])+" caller="+std::to_string(ctx.lr));
    }
    // The local-player update indexes its controller state using player+0x178.
    // AI look updates call the same function from other sites and must not
    // consume another player's mouse motion.
    int port = player>=0x80000000u && player<=guest_ram_end()-0x790 ? player_port(player) : -1;
    if (g_aim_multiplayer) {
        port = (ctx.lr == 0x800A8660 || ctx.lr == 0x800A9FF4 || ctx.lr == 0x800BF410) && player >= 0x80000000u && player < (guest_ram_end()-0x360u)
            ? int(mem_read32(player + 0x178)) : -1;
    }
    if (getenv("SSNE_AIM_TRACE") && player >= 0x80000000u && player < (guest_ram_end()-0x360u)) {
        static std::set<uint64_t> callers;
        if (callers.insert(uint64_t(player) << 32 | ctx.lr).second) {
            char message[128]; snprintf(message, sizeof(message), "[aim] mapping caller=%08X player=%08X slot=%u", ctx.lr, player, mem_read32(player + 0x178));
            log_message(message);
        }
    }
    ssne_call_original(ctx, kLookUpdate, g_orig_look);
    if(!g_netplay_active && port!=g_kbm_port.load())return;
    if (getenv("SSNE_AIM_TRACE") && port >= 0 && port < 8) {
        static std::set<uint32_t> seen;
        if (seen.insert(player).second) log_message("[aim] local player " + std::to_string(port) + " discovered");
    }
    if (port < 0 || port >= 8) return;
    if (g_netplay_active && unsigned(port) < g_netplay_count) { g_netplay_players[port] = player; g_netplay_player_types[port] = mem_read32(player + 0x48); }
    double dyaw, dpitch;
    {
        std::lock_guard<std::mutex> lk(g_aim_mutex);
        bool fresh = bool(g_netplay_active) || now_ms() - g_aim_last_ms[port].load() < 200;
        dyaw = fresh ? g_aim_yaw[port] : 0; dpitch = fresh ? g_aim_pitch[port] : 0;
        g_aim_yaw[port] = g_aim_pitch[port] = 0;
    }
    g_aim_last_ms[port] = now_ms();
    if (!g_netplay_active || port == 0) mod_tick(ctx, player);
    if (dyaw == 0 && dpitch == 0) return;
    // Zoomed in (the sniper rifle narrows the FOV), the same turn moves the image further: the mouse turns by the
    // zoom (ratio of the tangents), so it moves the crosshair over the image at the same speed at any zoom.
    if (float cam = player_zoom_fov(player); cam > 1.0f) {
        constexpr double h = 0.5 * 0.017453292519943295;
        double zoom = std::tan(h * cam) / std::tan(h * kGameFov);
        dyaw *= zoom;
        dpitch *= zoom;
    }
    if (player < 0x80000000u || player >= guest_ram_end()) return;
    constexpr double kTwoPi = 6.283185307179586;
    double yaw = std::fmod(double(u2f(mem_read32(player + 0x290))) + dyaw, kTwoPi);
    mem_write32(player + 0x290, f2u(float(yaw)));
    uint32_t lo_obj = mem_read32(player + 0x194), hi_obj = mem_read32(player + 0x198);
    if (lo_obj < 0x80000000u || lo_obj >= guest_ram_end() || hi_obj < 0x80000000u || hi_obj >= guest_ram_end()) return;
    constexpr float kDeg = 0.017453292f;
    float lo = u2f(mem_read32(lo_obj + 0x24)) * kDeg, hi = u2f(mem_read32(hi_obj + 0x24)) * kDeg;
    if (lo > hi) std::swap(lo, hi);
    // The game's pitch grows downward (C-stick up -> look down unless the game's invert option is set).
    float pitch = std::clamp(u2f(mem_read32(player + 0x294)) - float(dpitch), lo, hi);
    mem_write32(player + 0x294, f2u(pitch));
    if (getenv("SSNE_AIM_TRACE")) {
        char message[160];
        snprintf(message, sizeof(message), "[aim] controller=%d player=%08X delta=%.6f,%.6f yaw=%.6f pitch=%.6f", port, player, dyaw, dpitch, yaw, pitch);
        log_message(message);
    }
}

// Render every canonical guest camera at single-player dimensions. Only the
// local camera's GX draws are recorded. All peers still execute identical
// guest callbacks (including render RNG and effects), so selecting a camera
// cannot change the shared simulation. Restore layout before returning.
void hle_netplay_view_create(PPCContext& ctx) {
    // Every simulation creates identical full-screen view/HUD geometry.
    const auto party_view=ctx.r[3],party_slot=ctx.r[7];
    ctx.r[5]=0;
    ssne_call_original(ctx, 0x800B7D84u, g_netplay_view_create);
    native_party_bind_view(party_view,party_slot);
}
// Observational PAL probes for original RenderViewport subcalls. Address labels
// deliberately avoid guessing semantics. Never replace an existing functional hook;
// later functional hook installation may replace a probe without changing gameplay.
template<uint32_t Address, perf::detail::Stage Stage> void perf_native_probe(PPCContext& ctx){
    perf::detail::Scope timer(Stage);
    static const auto original=ssne_find_func(Address)->func;
    ssne_call_original(ctx,Address,original);
}
void install_perf_native_probes(){
    if(!perf::detail::period.load() || mem_read32(0x80000000u)!=0x47334250u)return;
    unsigned installed=0;
    for(const auto& [address,fn]:{
        std::pair<uint32_t,PPCFunc>{0x801493D0,perf_native_probe<0x801493D0,perf::detail::Native493d0>},
        {0x8012203C,perf_native_probe<0x8012203C,perf::detail::Native2203c>},
        {0x801266C8,perf_native_probe<0x801266C8,perf::detail::Native266c8>},
        {0x80142D94,perf_native_probe<0x80142D94,perf::detail::Native42d94>},
        {0x80143308,perf_native_probe<0x80143308,perf::detail::Native43308>},
        {0x8014CC6C,perf_native_probe<0x8014CC6C,perf::detail::Native4cc6c>},
        {0x8011818C,perf_native_probe<0x8011818C,perf::detail::Native1818c>}}){
        if(ssne_find_func(address) && !*ssne_hook_slot(address)){ssne_install_hook(address,fn);++installed;}
    }
    log_message("[performance-detail] original PAL render subcall probes="+std::to_string(installed)+"; existing hooks preserved");
}

void hle_netplay_list_render(PPCContext& ctx) {
    gx_sync();
    struct GxScopeDrain { ~GxScopeDrain(){ gx_sync(); } } gx_scope_drain;
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::ListDraw);
    struct OwnerScope {
        int previous=g_netplay_list_owner,previous_native=native_party_owner();
        ~OwnerScope(){gx_sync();g_netplay_list_owner=previous;native_party_owner_set(previous_native);}
    } scope;
    if(g_netplay_render_engine)for(unsigned port=0;port<g_netplay_count;++port) {
        auto player=g_netplay_players[port];if(!player)continue;
        auto view=mem_read32(player+0x374);if(view<0x80000000 || view>(guest_ram_end()-0x1000u))continue;
        for(unsigned off:{0u,4u,8u})if(mem_read32(view+off)==ctx.r[3]) {
            g_netplay_list_owner=int(port);
        }
    }
    if(g_authoritative_gameplay && g_netplay_render_engine && g_netplay_list_owner>=0 &&
       g_netplay_list_owner!=g_netplay_local_player)return;
    // HUD callbacks issue canonical view indices too. Keep native view
    // relocation in the same player scope as GPU draw filtering.
    if(native_party_extended() && g_netplay_list_owner>=0)native_party_owner_set(g_netplay_list_owner);
    detail::NativeHudListScope hud_scope(g_native_hud_list_observer);
    ssne_call_original(ctx, game_address(0x8011E5F4u), g_netplay_list_render);
}

// Culling uses the guest projection, while local widescreen/FOV is applied
// later by the GPU. Deterministic netplay shares agreed extents; authoritative
// coop has independent local rendering and never receives those legacy inputs.
// In coop match the local GPU's aspect and tangent FOV scale instead. Restore
// the projection immediately: weapons, projected textures and GX stay canonical.
float g_netplay_visibility_x=1.f, g_netplay_visibility_y=1.f;
std::array<float,2> authoritative_visibility() {
    const float target=g_fov.load();
    const float scale=target>0.f?std::tan(target*.00872664626f)/std::tan(kGameFov*.00872664626f):1.f;
    return {std::max(1.f,g_widescreen.load()*scale),std::max(1.f,scale)};
}
PPCFunc g_netplay_frustum_original=nullptr;
void hle_netplay_frustum(PPCContext& ctx) {
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::Frustum);
    const auto projection=game_address(0x80291BE0u);
    const auto x=mem_read32(projection), y=mem_read32(projection+0x14);
    struct Restore {
        uint32_t p,x,y;
        ~Restore(){mem_write32(p,x);mem_write32(p+0x14,y);}
    } restore{projection,x,y};
    if(ctx.lr==game_address(0x80117B9Cu)) { // RenderViewport's frustum setup
        float horizontal=g_netplay_visibility_x,vertical=g_netplay_visibility_y;
        if(g_authoritative_gameplay) {
            const auto extent=authoritative_visibility();
            horizontal=extent[0];vertical=extent[1];
        }
        mem_write32(projection,f2u(u2f(x)/horizontal));
        mem_write32(projection+0x14,f2u(u2f(y)/vertical));
    }
    ssne_call_original(ctx,game_address(0x8014D63Cu),g_netplay_frustum_original);
}

// PAL world visibility projects room portals into screen rectangles after the
// frustum setup. Correct its PRIVATE GXGetProjectionv copy too; otherwise a
// visible doorway in a widened side strip still rejects the entire next room.
// No GX state, render projection, viewport or camera matrix is changed.
PPCFunc g_authoritative_visibility_projection_original=nullptr;
void hle_authoritative_visibility_projection(PPCContext& ctx) {
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::VisibilityProjection);
    const auto output=ctx.r[3],caller=ctx.lr;
    ssne_call_original(ctx,0x801DADACu,g_authoritative_visibility_projection_original);
    if(caller!=0x80149474u || output!=0x80291F64u || mem_read32(output)!=f2u(0.f))return;
    const auto extent=authoritative_visibility();
    mem_write32(output+4,f2u(u2f(mem_read32(output+4))/extent[0]));
    mem_write32(output+12,f2u(u2f(mem_read32(output+12))/extent[1]));
}

PPCFunc g_authoritative_viewport_original=nullptr;
PPCFunc g_authoritative_proxy_draw_original=nullptr;
void hle_authoritative_proxy_draw(PPCContext& ctx){
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::ModelDraw);
    // Original parent callbacks still need these local attachment objects,
    // while the independently received proxies carry the complete host pose.
    // Filter the old proxy at its real draw entry; +2D is a culling override,
    // not a reliable way to disable a normally culled native model.
    if(replica_suppressed_npc_proxy(ctx.r[3])){ctx.r[3]=0;return;}
    ssne_call_original(ctx,0x8013D4D0u,g_authoritative_proxy_draw_original);
}
void hle_authoritative_viewport(PPCContext& ctx) {
    gx_sync();
    struct GxScopeDrain { ~GxScopeDrain(){ gx_sync(); } } gx_scope_drain;
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::Viewport);
    CoopHookTimer timer("viewport_"+std::to_string(ctx.r[4]));
    if(g_netplay_render_engine && getenv("SSNE_COOP_RENDER_TRACE")) {
        static std::array<uint64_t,32> visits{};
        static std::array<uint32_t,32> seen_cameras{};
        const auto slot=ctx.r[4];
        const auto view=slot<32?native_party_view_engine(ctx.r[3])+0xDC+slot*76:0;
        const auto camera=view?mem_read32(view):0;
        const bool changed=slot<32 && seen_cameras[slot]!=camera;
        if(slot<32)seen_cameras[slot]=camera;
        if(slot<32 && (changed || ++visits[slot]%120==0) && !netplay_render_discard()) {
            const auto player_slot=unsigned(g_netplay_local_player.load());
            const auto actor=player_slot<g_netplay_count?mem_read32(native_party_registry()+player_slot*4):0;
            char line[280];snprintf(line,sizeof(line),"[coop-render-view] local=%u view=%u camera=%08X camera_type=%08X camera_owner=%08X expected_camera=%08X model_mask=%08X",player_slot,slot,camera,camera>=0x80000000u && camera<guest_ram_end()-8?mem_read32(camera):0,camera>=0x80000000u && camera<guest_ram_end()-8?mem_read32(camera+4):0,actor>=0x80000000u && actor<guest_ram_end()-0x370?mem_read32(actor+0x368):0,mem_read32(view+0x1C));log_message(line);
        }
    }
    ssne_call_original(ctx,0x801179BC,g_authoritative_viewport_original);
}
void hle_netplay_render(PPCContext& ctx) {
    gx_sync();
    struct GxScopeDrain { ~GxScopeDrain(){ gx_sync(); } } gx_scope_drain;
    ssne::perf::detail::Frame perf_frame(ssne::perf::detail::GuestRender);
    CoopHookTimer timer("native_render");
    struct HudEdgeScope {
        PPCContext context;
        struct Position{uint32_t widget,x,y;};
        std::vector<Position> saved;
        void position(uint32_t widget,uint32_t x,uint32_t y){
            // Pure native GUI positioning, using this process's widgets.
            const auto low=context.r[1]-0x1000;
            std::array<uint8_t,0x1000> stack{};
            // Constructor validated this contiguous RAM range. A bytewise
            // guest/MMIO translation is unnecessary for temporary stack data.
            std::memcpy(stack.data(),ssne::host_ptr(low),stack.size());
            auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;
            mem_write32(call.r[1],context.r[1]);mem_write32(call.r[1]+0x80,x);mem_write32(call.r[1]+0x84,y);
            call.r[3]=widget;call.r[4]=call.r[1]+0x80;ssne_call(call,0x800AB728u);
            std::memcpy(ssne::host_ptr(low),stack.data(),stack.size());
        }
        explicit HudEdgeScope(PPCContext& c):context(c){
            const float wide=g_widescreen.load();
            if(!gx::hud_edges() || wide<=1.f || wide>4.f || native_movie_presenting() || native_cutscene_presenting() || g_replica_pause_menu_open || !ssne_find_func(0x800AB728u) || c.r[1]<0x80005000u || c.r[1]>guest_ram_end())return;
            const unsigned slot=g_netplay_local_player.load();
            if(slot>=g_netplay_count)return;
            const auto actor=mem_read32(native_party_registry()+slot*4);
            if(actor<0x80000000u || actor>guest_ram_end()-0x790)return;
            const auto hud=mem_read32(actor+0x374);
            if(hud<0x80000000u || hud>guest_ram_end()-0x534)return;
            const auto root=mem_read32(hud+0x14);
            auto shift=[&](uint32_t widget,unsigned group=0){
                if(widget<0x80000000u || widget>guest_ram_end()-0x38)return;
                const auto base=mem_read32(widget);
                if(base<0x80000000u || base>guest_ram_end()-0x20 || mem_read32(base+0x14)!=(group==1?mem_read32(hud+0xC):group==2?mem_read32(hud+0x18):root))return;
                const auto anchor=mem_read8(base+0x1C);
                if(group==1?anchor!=7:group==2?anchor!=6:(anchor!=2 && anchor!=3))return;
                const auto x=mem_read32(widget+4),y=mem_read32(widget+8);
                if(!std::isfinite(u2f(x)) || !std::isfinite(u2f(y)))return;
                saved.push_back({widget,x,y});position(widget,f2u(u2f(x)+((anchor==2 || anchor==6)?-1.f:1.f)*320.f*(wide-1.f)),y);
            };
            for(unsigned off:{0x84u,0x88u,0x8Cu,0x90u,0x94u,0x98u,0x9Cu,0xA0u,0x3DCu})shift(mem_read32(hud+off));
            for(unsigned i=0;i<12;++i)for(unsigned child=0;child<2;++child)shift(mem_read32(hud+0x22C+i*36+child*4));
            for(unsigned i=0;i<12;++i)for(unsigned child=0;child<2;++child)shift(mem_read32(hud+0x3F0+i*16+child*4),true);
            // The fuel gauge shares the original six-slot status rail with
            // power-ups. Icon and fill are siblings under hud+18, anchor6.
            // Move every active pair together, then restore after rendering.
            for(unsigned i=0;i<6;++i)if(mem_read32(hud+0x170+i*24)){
                shift(mem_read32(hud+0x17C+i*24),2);
                shift(mem_read32(hud+0x180+i*24),2);
            }
        }
        ~HudEdgeScope(){for(const auto& p:saved)position(p.widget,p.x,p.y);}
    };
    struct LayoutScope {
        uint32_t engine, previous_engine, previous_sda;
        std::array<std::array<uint32_t, 6>, 24> saved{};
        std::array<bool,24> changed{};
        unsigned count()const{return 2*g_netplay_count;}
        unsigned index(unsigned slot)const {
            if(native_party_extended())return slot<g_netplay_count?slot:slot<2*g_netplay_count?slot+8-g_netplay_count:slot+16-2*g_netplay_count;
            return slot<g_netplay_count?slot:slot+6-g_netplay_count;
        }
        LayoutScope(PPCContext& c) : engine(c.r[3]), previous_engine(g_netplay_render_engine),
            previous_sda(g_netplay_render_sda) {
            for (unsigned slot = 0; slot < count(); ++slot) {
                if(information_camera(index(slot),engine))continue;
                changed[slot]=true;
                uint32_t view = native_party_view_engine(engine) + 0xDC + index(slot)*76;
                for (unsigned i=0; i<6; ++i) saved[slot][i] = mem_read32(view + 4 + i*4);
                for (unsigned i=0; i<4; ++i)
                    mem_write32(view+4+i*4, f2u(0.f));
                mem_write32(view+0x14, 0); // absolute upper-left coordinate
                mem_write32(view+0x18, 3); // anchor at the full screen lower-right
            }
            g_netplay_render_engine=engine; g_netplay_render_sda=c.r[13];
        }
        ~LayoutScope() {
            gx_sync();
            for (unsigned slot=0; slot<count(); ++slot)
                if(changed[slot])
                for (unsigned i=0; i<6; ++i)
                    mem_write32(native_party_view_engine(engine)+0xDC+index(slot)*76+4+i*4, saved[slot][i]);
            g_netplay_render_engine=previous_engine; g_netplay_render_sda=previous_sda;
        }
    };
    if (frontend_presenting() || !g_netplay_world_ready || ctx.r[3]<0x80000000u || ctx.r[3]>(guest_ram_end()-0x1000u)) {
        ssne_call_original(ctx, game_address(0x80117434), g_netplay_render_original);
        return;
    }
    if(native_replica_results_presenting()){
        // Use the original menu projection/scaling, just as the host does
        // after retiring its gameplay views. Keep only shared GUI cameras.
        ctx.r[4]=native_party_extended()?0xFF000000u:(mem_read32(0x802ABAD0u)&~0xFu);
        ssne_call_original(ctx,game_address(0x80117434),g_netplay_render_original);return;
    }
    LayoutScope scope(ctx);
    HudEdgeScope hud_edges(ctx);
    struct ClientPresentationScope {
        PPCContext& context;
        explicit ClientPresentationScope(PPCContext& c):context(c){if(g_game_render_begin)g_game_render_begin(c);}
        ~ClientPresentationScope(){gx_sync();if(g_game_render_end)g_game_render_end(context);}
    } client_presentation(ctx);
    // Zero is the original movie path: world, weapons and HUD are disabled.
    // Expanding that mask to every party viewport draws gameplay behind Bink.
    if(native_party_extended() && ctx.r[4] && !native_movie_presenting())ctx.r[4]=0xFFFFFFFFu;
    if(native_movie_presenting())ctx.r[4]=0;
    if(g_authoritative_gameplay){
        // Simulation is host-owned. Remote views must not traverse and draw
        // the whole world only to discard their GX commands afterwards.
        const unsigned local=g_netplay_local_player.load();
        const uint32_t shared=native_party_extended()?0xFF000000u:0xC030u;
        const uint32_t own=local<g_netplay_count ? (1u<<local)|(1u<<(local+(native_party_extended()?8:6)))|(1u<<(local+(native_party_extended()?16:10))) : 0u;
        uint32_t information=0;for(const auto& [slot,camera]:g_information_cameras)if(information_camera(slot))information|=1u<<slot;
        ctx.r[4]&=shared|information|(native_cutscene_presenting()?0u:own);
    }
    ssne_call_original(ctx, game_address(0x80117434), g_netplay_render_original);
}

void install_mouse_aim() {
    const SSNEFuncEntry* f = ssne_find_func(kLookUpdate);
    if (!f) return;
    for (int k = 0; k < 6; k++)
        if (mem_read32(kLookUpdate + uint32_t(k) * 4) != kLookUpdateSig[k]) {
            fprintf(stdout, "[ssne] mouse aim: look update not found (different game build?)\n");
            return;
        }
    g_orig_look = f->func;
    ssne_install_hook(kLookUpdate, hle_look_update);
}
} // namespace


// ---- analog stick fix. The game reads the controllers once per video retrace (VI callback: PADRead + PADClamp
// into a PADStatus array) and its controller processing (8014F83C, once per game frame) rescales the clamped
// octagon back to a square *in place* in that array: x *= 72 / ((1 - |y|/40) * 32 + 40) for y <= 40 (other formula
// above 40; the C-stick uses 59/31/28). On the GameCube the game ticks exactly once per retrace, so every status is
// processed once. When a status is processed twice or more (game frames faster than retraces: frame limit above
// 60, or a frame landing just before a retrace), the already-scaled values are scaled again, and with both axes
// negative the denominator goes negative: down-left goes (-40,-40) -> (-72,-72) -> (-128,-128) -> (127,127), i.e.
// the stick flips to up-right and the player stops/jitters (the other diagonals use the formula that stays
// positive). Fix: keep the stick bytes as PADClamp left them and put them back before each processing pass, so
// every pass sees the raw input, as on hardware.
namespace {
const uint32_t kPadClampSig[6] = {0x7C0802A6u, 0x3C808023u, 0x90010004u, 0x9421FFE0u, 0x93E1001Cu, 0x93C10018u};
const uint32_t kPadProcessSig[6] = {0x9421FF28u, 0x7C0802A6u, 0xDA010058u, 0xDA210060u, 0xDA410068u, 0xDA610070u};
PPCFunc g_orig_pad_clamp = nullptr, g_orig_pad_process = nullptr;
uint32_t g_pad_status = 0;    // the PADStatus array (4 x 12 bytes) PADClamp last worked on
uint8_t g_pad_sticks[4][4];   // its stick bytes (x, y, sub x, sub y) right after PADClamp
bool g_pad_fix_trace = false;
int g_pad_passes = 0;         // processing passes since the last PADClamp (trace)

// ---- jump at high frame rates. The controller processing turns the pads into actions, records of 28 bytes at
// controller + 0x70C * port + 12 + 28 * n: value (float), pressed this pass (+4), held (+8), ..., held time (+0x14);
// jump is action 0. Each player (split screen: up to four) is latched on its own port. The player's update (800BEEC8) runs the movement controller (player+0x344) in fixed steps of 1/120 s
// (player+0x23C -> +0x24): the frame time goes into an accumulator (player+0x140), and the physics method (slot 0x34)
// runs once per whole step, i.e. twice per game frame at 60 fps but only every second to fourth frame above 120 fps.
// The jump is taken there, from the "pressed" flag, which lasts one processing pass, i.e. one game frame: a press in
// a frame without a physics step was lost (1 in 10 at 144 fps, 5 in 10 at ~280 fps), and jumping out of water, which
// takes a press at the edge, failed. The flag is kept set until the player update has run a physics step, in frames
// the player controls Sam only (no physics runs in the menus), and never longer than 50 ms. (Leaving the water also
// needed the first step after a controller switch to come early: hle_player_update.)
constexpr uint32_t kJumpAction = 0;
constexpr uint32_t kPortBlock = 0x70C;  // the controller processing's data per controller port (actions at +12)

void latch_jump(uint32_t controller) {
    if (controller < 0x80000000u || controller >= 0x817FF000u - 3 * kPortBlock) return;
    for (int port = 0; port < 4; port++) {
        uint32_t pressed = controller + port * kPortBlock + 12 + kJumpAction * 28 + 4;
        int64_t& latched = g_jump_latched_ms[port];
        if (mem_read32(pressed)) { latched = now_ms(); continue; }
        if (latched < 0) continue;
        if (!g_player_control.load() || now_ms() - latched > 50) { latched = -1; continue; }
        mem_write32(pressed, 1);
    }
}

void remember_clamped_pad(uint32_t status) {
    if (status < 0x80000000u || status >= (guest_ram_end()-0x30u)) { g_pad_status = 0; return; }
    for (uint32_t ch = 0; ch < 4; ch++)
        for (uint32_t k = 0; k < 4; k++) g_pad_sticks[ch][k] = mem_read8(status + 12 * ch + 2 + k);
    g_pad_status = status;
}
void hle_pad_clamp(PPCContext& ctx) {
    uint32_t status = ctx.r[3];
    ssne_call_original(ctx, g_ver->pad_clamp, g_orig_pad_clamp);
    remember_clamped_pad(status);
    if (g_pad_fix_trace && g_pad_passes != 1)
        fprintf(stdout, "[ssne] pad status processed %d times since the previous read\n", g_pad_passes);
    g_pad_passes = 0;
}

void hle_pad_process(PPCContext& ctx) {
    if (g_pad_status) {
        for (uint32_t ch = 0; ch < 4; ch++)
            for (uint32_t k = 0; k < 4; k++) mem_write8(g_pad_status + 12 * ch + 2 + k, g_pad_sticks[ch][k]);
        g_pad_passes++;
    }
    const auto party_bank=ctx.r[3];
    const double party_dt=ctx.ps0[1];
    const bool trace_fire=getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES") && g_authoritative_gameplay && native_party_extended() && get_pad(0).trigger_r>128;
    static unsigned fire_samples=0;
    const bool trace_sample=trace_fire && ++fire_samples%30==1;
    auto trace_bank=[&](const char* stage){if(!trace_sample)return;const auto status=mem_read32(0x802ABAD8),actor=mem_read32(native_party_registry());std::string message="[coop-native-fire-path] stage="+std::string(stage)+" bank="+std::to_string(party_bank)+" status="+std::to_string(status)+" buttons="+std::to_string(mem_read16(status))+" trigger="+std::to_string(mem_read8(status+7))+" information="+std::to_string(bool(g_authoritative_information_open))+" actor="+std::to_string(actor);for(unsigned action=0;action<20;++action)message+=" a"+std::to_string(action)+"="+std::to_string(u2f(mem_read32(party_bank+0xC+28*action)));log_message(message);};
    trace_bank("before");
    ssne_call_original(ctx, g_ver->pad_process, g_orig_pad_process);
    trace_bank("first");
    if(g_netplay_active && native_party_extended() && native_party_second_bank()) {
        native_party_sample_second();
        PPCContext second=ctx;second.msr&=~0x8000u;second.r[3]=native_party_second_status();
        ssne_call_original(second,g_ver->pad_clamp,g_orig_pad_clamp);
        const auto previous=mem_read32(0x802ABAD8);
        struct Restore {uint32_t previous;~Restore(){mem_write32(0x802ABAD8,previous);}} restore{previous};
        mem_write32(0x802ABAD8,native_party_second_status());
        second=ctx;second.msr&=~0x8000u;second.r[3]=native_party_second_bank();second.ps0[1]=second.ps1[1]=party_dt;
        ssne_call_original(second,g_ver->pad_process,g_orig_pad_process);
    }
    trace_bank("second");

    if(g_authoritative_gameplay) {
        static uint64_t samples=0;
        if(++samples%120==0)for(unsigned slot=0;slot<g_netplay_count;++slot) {
            const auto controller=slot<4?party_bank+slot*0x70C:native_party_second_bank()+(slot-4)*0x70C;
            const auto actor=mem_read32(native_party_registry()+slot*4);
            std::string trace="[coop-native-actions] sample="+std::to_string(samples)+" slot="+std::to_string(slot)+" active="+std::to_string(mem_read32(controller))+" flags="+std::to_string(mem_read32(controller+4))+" actor="+std::to_string(actor);
            for(unsigned action=0;action<8;++action)trace+=" a"+std::to_string(action)+"="+std::to_string(u2f(mem_read32(controller+0xC+28*action)));
            if(actor>=0x80000000u && actor<=guest_ram_end()-0x790)trace+=" input="+std::to_string(mem_read32(actor+0x178))+" state="+std::to_string(mem_read32(actor+0x148))+" respawn="+std::to_string(mem_read32(actor+0x3AC));
            log_message(trace);
        }
    }
    if(getenv("SSNE_NATIVE_PARTY_TRACE") && g_netplay_count>2 && g_netplay_tick%120==0) {
      std::string line="[party-controller] count="+std::to_string(g_netplay_count);
      auto statuses=mem_read32(0x802ABAD8);
      for(unsigned i=0;i<4;++i)line+=" pad="+std::to_string(mem_read16(statuses+12*i))+"/"+std::to_string(int8_t(mem_read8(statuses+12*i+10)))+" controller="+std::to_string(mem_read32(party_bank+i*0x70C))+"/"+std::to_string(mem_read32(party_bank+i*0x70C+4));
      log_message(line);
    }
    if(!g_netplay_active)latch_jump(party_bank);
}

bool code_matches(uint32_t addr, const uint32_t sig[6]) {
    for (int k = 0; k < 6; k++)
        if (mem_read32(addr + uint32_t(k) * 4) != sig[k]) return false;
    return true;
}

// ---- swim strokes / steps at high frame rates. 800B78BC(counter, f1 = distance moved this frame) adds the distance
// up and plays the next stroke sound (alternating sides) per stroke length; in a frame that moved less than a
// threshold (0.02, a constant at 80212748) it resets the counter to a full stroke, so the next frame that moves plays
// a stroke at once. Above 120 fps many frames don't move Sam at all (his position changes in the 120 Hz physics
// steps), and the rest move little, so the counter kept being reset and the strokes came several times as often (the
// loud, doubled water sounds: 34 strokes in 10 s instead of 5). So the threshold is scaled to the frame time (0.02
// per 1/60 s, the same speed), and a frame under it within 50 ms of the last movement doesn't touch the counter (no
// reset; its movement is counted with the next frame's); a real stop still resets it.
const uint32_t kStepCounterSig[6] = {0x9421FFE8u, 0x7C0802A6u, 0xDBE10010u, 0x93E1000Cu, 0x9001001Cu, 0x7C7F1B78u};
constexpr uint32_t kStepCounter = 0x800B78BC;
PPCFunc g_orig_step_counter = nullptr;
uint32_t g_step_threshold = 0;  // address of the threshold constant (from the function's lis / lfs)
uint32_t g_step_counter_obj = 0;  // the counter that last moved, and when
int64_t g_step_moved_ms = 0;

void hle_step_counter(PPCContext& ctx) {
    float dt = u2f(mem_read32(ctx.r[13] + 0xFFFF85F8u));
    uint32_t saved = mem_read32(g_step_threshold);
    bool scale = dt > 0 && dt < 1.0f / 60;
    if (scale) {
        float threshold = u2f(saved) * dt * 60;
        if (ctx.ps0[1] > threshold) {
            g_step_counter_obj = ctx.r[3];
            g_step_moved_ms = now_ms();
        } else if (ctx.r[3] == g_step_counter_obj && now_ms() - g_step_moved_ms < 50) {
            return;
        }
        mem_write32(g_step_threshold, f2u(threshold));
    }
    PPCFunc* slot = ssne_hook_slot(kStepCounter);
    *slot = nullptr;
    g_orig_step_counter(ctx);
    *slot = &hle_step_counter;
    if (scale) mem_write32(g_step_threshold, saved);
}

void install_step_counter_fix() {
    const SSNEFuncEntry* f = ssne_find_func(kStepCounter);
    uint32_t lis = mem_read32(kStepCounter + 0x18), lfs = mem_read32(kStepCounter + 0x20);
    if (!f || !code_matches(kStepCounter, kStepCounterSig) || (lis & 0xFFFF0000u) != 0x3D200000u ||
        (lfs & 0xFFFF0000u) != 0xC1A90000u) {
        fprintf(stdout, "[ssne] swim stroke fix: step counter not found (different game build?)\n");
        return;
    }
    g_step_threshold = ((lis & 0xFFFFu) << 16) + uint32_t(int32_t(int16_t(lfs & 0xFFFFu)));
    g_orig_step_counter = f->func;
    ssne_install_hook(kStepCounter, hle_step_counter);
}

// ---- vehicle physics at frame rates other than 60. The update of the drivable vehicles (801709A0, GenericVehicle:
// the jeeps) runs int(frame time / (1/60) + 0.5) physics steps of 1/60 s per frame: none in frames shorter than
// 1/120 s, one per frame between 60 and 120 fps (time running fast). While driving, the frame rate is held at 60
// (in_vehicle), but a vehicle nobody drives is updated at any rate: above 120 fps the jeep dropped in after the first
// fight of Splashdown in Xifengkou never landed (only its wheels were drawn, and it couldn't be entered) until a menu
// (60 fps) or a long frame ran a step. Each vehicle now runs the steps its accumulated time is due, rounded with the
// remainder carried: the frame time it sees is that many steps (0 in most frames above 120 fps). At 60 fps that is
// one step per frame, as before.
const uint32_t kVehicleUpdateSig[6] = {0x9421FF08u, 0x7C0802A6u, 0xDBE100F0u, 0xBDE100ACu, 0x900100FCu, 0x7C7F1B78u};
constexpr uint32_t kVehicleUpdate = 0x801709A0, kVehicleStep = 0x802235BC, kVehicleRound = 0x802235C0;
PPCFunc g_orig_vehicle_update = nullptr;
std::unordered_map<uint32_t, float> g_vehicle_time;  // per vehicle: time not yet stepped (in steps, -0.5 .. 0.5)

void hle_vehicle_update(PPCContext& ctx) {
    uint32_t dt_addr = ctx.r[13] + 0xFFFF85F8u, dt_bits = mem_read32(dt_addr);
    float step = u2f(mem_read32(kVehicleStep)), dt = u2f(dt_bits);
    bool adjust = dt > 0 && dt < 0.25f;
    uint32_t steps_bits = 0;
    if (adjust) {
        if (g_vehicle_time.size() > 64) g_vehicle_time.clear();  // vehicles of earlier levels
        float& due = g_vehicle_time[ctx.r[3]];
        due += dt / step;
        float n = std::floor(due + 0.5f);
        due -= n;
        steps_bits = f2u(n * step);
        mem_write32(dt_addr, steps_bits);
    }
    PPCFunc* slot = ssne_hook_slot(kVehicleUpdate);
    *slot = nullptr;
    g_orig_vehicle_update(ctx);
    *slot = &hle_vehicle_update;
    if (adjust && mem_read32(dt_addr) == steps_bits) mem_write32(dt_addr, dt_bits);
}

void install_vehicle_step_fix() {
    const SSNEFuncEntry* f = ssne_find_func(kVehicleUpdate);
    if (!f || !code_matches(kVehicleUpdate, kVehicleUpdateSig) || u2f(mem_read32(kVehicleStep)) != 1.0f / 60 ||
        u2f(mem_read32(kVehicleRound)) != 0.5f) {
        fprintf(stdout, "[ssne] vehicle step fix: vehicle update not found (different game build?)\n");
        return;
    }
    g_orig_vehicle_update = f->func;
    ssne_install_hook(kVehicleUpdate, hle_vehicle_update);
}

// ---- landing above 60 fps. An object's physics update (800914D0, enemies and other objects, not the player) sets
// its "on the ground" flag (+0x1AC) in the frames it touches the ground and clears it in the others. Enemy AI thinks
// on timers (8009C5B0: intervals of 1/60, 0.1, 0.15 s) and reads the flag when it does: a body knocked back by
// gunfire (the cThrow directive, 8000B0F4) ends when it has landed. Above 60 fps there are more physics frames
// between two thinks, and a knocked-back body that pressed into the ground at each think touched it in the frames
// right after and not in the one before the next think: the check never saw it, and the enemy stood frozen until a
// long frame (a menu, a hitch) let one through (frogs in the Temple of Neptune; any enemy that is knocked back). Above
// 60 fps the flag now stays set for 1/60 s plus a frame after the last contact: at most a 60 fps frame later, so the
// next think sees the contact as it would at 60 fps (knock-backs end after ~1.5 s at any frame rate).
const uint32_t kObjectPhysicsSig[6] = {0x9421FFA0u, 0x7C0802A6u, 0xDBE10058u, 0xBF610044u, 0x90010064u, 0x7C7F1B78u};
constexpr uint32_t kObjectPhysics = 0x800914D0;
PPCFunc g_orig_object_physics = nullptr;
std::unordered_map<uint32_t, float> g_since_landed;  // per object: time since it last touched the ground

void hle_object_physics(PPCContext& ctx) {
    uint32_t obj = ctx.r[3];
    float dt = u2f(mem_read32(ctx.r[13] + 0xFFFF85F8u));
    uint32_t before = mem_read32(obj + 0x1AC);
    PPCFunc* slot = ssne_hook_slot(kObjectPhysics);
    *slot = nullptr;
    g_orig_object_physics(ctx);
    *slot = &hle_object_physics;
    if (!(dt > 0 && dt < 0.95f / 60)) {  // 60 fps or slower: as the game does
        if (!g_since_landed.empty()) g_since_landed.clear();
        return;
    }
    if (mem_read32(obj + 0x1AC) == 1) {
        if (g_since_landed.size() > 4096) g_since_landed.clear();
        g_since_landed[obj] = 0;
        return;
    }
    auto it = g_since_landed.find(obj);
    if (it == g_since_landed.end()) return;
    it->second += dt;
    if (before == 1 && mem_read32(obj + 0x1AC) == 0 && it->second < 1.0f / 60 + dt) mem_write32(obj + 0x1AC, 1);
    else if (it->second > 1.0f) g_since_landed.erase(it);
}

void install_landing_fix() {
    const SSNEFuncEntry* f = ssne_find_func(kObjectPhysics);
    if (!f || !code_matches(kObjectPhysics, kObjectPhysicsSig)) {
        fprintf(stdout, "[ssne] landing fix: object physics not found (different game build?)\n");
        return;
    }
    g_orig_object_physics = f->func;
    ssne_install_hook(kObjectPhysics, hle_object_physics);
}

// ---- 2-player split screen. Each match, 80166D90 picks a screen layout for every player from the number of players,
// SystemSetup.ScreenMode and the TwoPlayerScreenSplitMode / ThreePlayerScreenSplitMode settings (base.cfg), each
// read through the config lookup 80113770(config, name) (value at +0x24 of what it returns). Two players get the top
// and bottom halves (mode 0) or the left and right halves (mode 1); the game's options don't offer it. The lookup for
// the 2-player mode (called from 80167098) answers with the port's choice instead (set_split_two).
const uint32_t kConfigLookupSig[6] = {0x9421FFF0u, 0x7C0802A6u, 0xBFC10008u, 0x90010014u, 0x7CBE2B78u, 0x4BFFFDD5u};
const uint32_t kSplitLayoutSig[6] = {0x9421FFB8u, 0x7C0802A6u, 0xDBE10040u, 0xBEC10018u, 0x9001004Cu, 0x480004F9u};
constexpr uint32_t kConfigLookup = 0x80113770, kSplitLayout = 0x80166D90, kTwoSplitLookupReturn = 0x8016709C;
PPCFunc g_orig_config_lookup = nullptr;
std::atomic<int> g_split_two{-1};  // -1: the game's setting

void hle_config_lookup(PPCContext& ctx) {
    uint32_t lr = ctx.lr;
    PPCFunc* slot = ssne_hook_slot(kConfigLookup);
    *slot = nullptr;
    g_orig_config_lookup(ctx);
    *slot = &hle_config_lookup;
    int mode = g_split_two.load();
    if (lr == kTwoSplitLookupReturn && mode >= 0 && ctx.r[3] >= 0x80000000u && ctx.r[3] < 0x817FFFD0u)
        mem_write32(ctx.r[3] + 0x24, uint32_t(mode));
}

void install_split_choice() {
    const SSNEFuncEntry* f = ssne_find_func(kConfigLookup);
    if (!f || !code_matches(kConfigLookup, kConfigLookupSig) || !code_matches(kSplitLayout, kSplitLayoutSig)) {
        fprintf(stdout, "[ssne] split screen: layout choice not found (different game build?)\n");
        return;
    }
    g_orig_config_lookup = f->func;
    ssne_install_hook(kConfigLookup, hle_config_lookup);
}

// ---- development: fixed frame time. 8015544C measures the frame time the game advances by (seconds, in f1).
// SSNE_FIXED_FPS=N makes every gameplay frame exactly 1/N s, however fast the host runs (repeatable frame-rate tests
// on a busy machine; menus keep their own timing); game_time() adds up the frame times the game was given. The
// controllers are still read per host retrace, so a short press (the weapon keys' X tap) can be missed at low N.
const uint32_t kFrameTimeSig[6] = {0x9421FFD8u, 0x7C0802A6u, 0x9001002Cu, 0x480641FDu, 0x812D9538u, 0x7C601B78u};
constexpr uint32_t kFrameTime = 0x8015544C;
PPCFunc g_orig_frame_time = nullptr;
double g_fixed_dt = 0;
std::atomic<double> g_game_time{0};

void hle_frame_time(PPCContext& ctx) {
    PPCFunc* slot = ssne_hook_slot(kFrameTime);
    *slot = nullptr;
    g_orig_frame_time(ctx);
    *slot = &hle_frame_time;
    if (g_fixed_dt > 0 && player_control_active()) ctx.ps0[1] = ctx.ps1[1] = g_fixed_dt;  // gameplay only
    g_game_time = g_game_time.load() + ctx.ps0[1];
}

void install_frame_time() {
    const SSNEFuncEntry* f = ssne_find_func(kFrameTime);
    if (!f || !code_matches(kFrameTime, kFrameTimeSig)) return;
    if (const char* e = getenv("SSNE_FIXED_FPS")) g_fixed_dt = atof(e) > 0 ? 1.0 / atof(e) : 0;
    g_orig_frame_time = f->func;
    ssne_install_hook(kFrameTime, hle_frame_time);
}

// ---- cheats (issue #19, F12 > Cheats). The game has its own debug cheats (base.cfg Cheats.GiveInvulnerability,
// GiveInfiniteAmmo, GiveAllWeapons; also unlocked by collecting treasure): copied into three globals when a player
// is created, and gameplay checks those globals (800B366C damage, 800B7630 ammo, 800C6170 "has weapon"). The
// cheats set them every frame, in single player only (game mode 0; co-op is 1, Deathmatch, Hold the Flag and Pass the
// Bomb 2-4, where the game's own setters 800B6660... refuse them). Save states get the game's own values: the cheats
// are lifted around the save / load at the frame flip and set again after it.
// Speed scales the movement settings (ground, air, skates), read live through their config entries (value at +0x24),
// which the player keeps (player+0x1E0, 0x1F0, 0x1E8, set up in 800B12BC). Unlock all answers the unlock check
// 800C32F8(name) (treasures collected >= UnlockTable.<name>: the 10 lost levels, the gallery and the treasure
// cheats) with yes, so nothing is written to the memory card.
// All weapons and infinite ammo also fill every ammo type once per player (as the game's infinite ammo setter
// 800B6698 does; again when the game sets up the player's cheats, 800B2418: a new level or a respawn), so the new
// weapons can be picked: the ammo inventory (player+0x5E8) holds 17 counts, a second copy at +0x24, maxima at +0xB4.
// Infinite ammo needs the inventory's own flag too (+0xD8, also set by 800B6698), lifted like the globals.
// The game's invulnerability only keeps enemy attacks off health (armor still takes them, and falls, lava and
// drowning still hurt), so invulnerable also skips the player's damage method 800B366C(player, damage) as its own
// early exit does: nothing changes and it returns the health (player+0xCC, f1).
const uint32_t kCheatSetterSig[6] = {0x812D961Cu, 0x3929FFFEu, 0x28090002u, 0x41810008u, 0x38600000u, 0x906D86E0u};
const uint32_t kUnlockedSig[6] = {0x9421FFE8u, 0x7C0802A6u, 0xBF810008u, 0x9001001Cu, 0x3D20802Bu, 0x7C7C1B78u};
const uint32_t kPlayerCheatsSig[6] = {0x9421FFE8u, 0x7C0802A6u, 0xBFA1000Cu, 0x9001001Cu, 0x7C7D1B78u, 0x3D40802Bu};
const uint32_t kDamageSig[6] = {0x9421FF80u, 0x7C0802A6u, 0xDBC10070u, 0xDBE10078u, 0xBF210054u, 0x90010084u};
constexpr uint32_t kCheatSetter = 0x800B6660, kUnlocked = 0x800C32F8, kPlayerCheats = 0x800B2418;
constexpr uint32_t kDamage = 0x800B366C;
constexpr uint32_t kCheatGlobals[3] = {0x802DA284, 0x802DA288, 0x802DA280};  // invulnerable, infinite ammo, all weapons
constexpr uint32_t kGameMode = 0x802DB1BC;
// GroundMovementSpeed, AirMovementSpeed, SkatesMovementSpeed (speed); MinJumpHeight, MaxJumpHeight (jump)
struct ScaledSetting { uint32_t offset; float stock, written; bool jump; };
ScaledSetting g_scaled_settings[5] = {{0x1E0, 275, 275, false}, {0x1F0, 20, 20, false}, {0x1E8, 70, 70, false},
                                      {0x208, 1.2f, 1.2f, true}, {0x20C, 3.2f, 3.2f, true}};
// Weapon damage: every fire mode of the player's weapons (player+0x4BC + 0x18 * id: the weapon; its modes, 60 bytes
// each, at [weapon+0x1D0], their number at +0x1BC) holds MinDamage, MaxDamage and DamagePerSec (+0x10, +0x14, +0x28)
// copied from base.cfg when the weapon is made (8009F54C). Scaled there; the game's values are kept per mode.
struct DamageMode { float stock[3], written[3]; };
std::unordered_map<uint32_t, DamageMode> g_damage_modes;
constexpr uint32_t kDamageFields[3] = {0x10, 0x14, 0x28};
// Flying: the game's own debug flight (800BEEC8): with PuzzleConfig.AllowFlyCheat (entry at player+0x358, value at
// +0x1C) on, player+0x35C set and flags 0xC000 at player+0x1C (no gravity, no collision), the sticks fly Sam.
uint32_t g_fly_player = 0, g_fly_saved_allow = 0;
std::atomic<bool> g_flying{false};
std::atomic<uint32_t> g_cheat_bits{0};  // 1 invulnerable, 2 infinite ammo, 4 all weapons, 8 unlock all, 16 fly
std::atomic<float> g_cheat_speed{1}, g_cheat_jump{1}, g_cheat_damage{1};
std::atomic<bool> g_cheats_ok{false}, g_cheats_on{false};
PPCFunc g_orig_unlocked = nullptr, g_orig_player_cheats = nullptr, g_orig_damage = nullptr;
uint32_t g_ammo_filled[4] = {};  // per port: the player whose ammo the cheats filled
uint32_t g_inf_inv[4] = {}, g_inf_saved[4] = {};  // per port: ammo inventory whose infinite flag (+0xD8) is set
bool g_cheat_forced[3] = {};
uint32_t g_cheat_saved[3] = {};
bool g_scaled_forced = false;

bool cheat_mode_allowed() { return !g_netplay_active && mem_read32(game_address(kGameMode)) == 0; }  // single player

// Sets the cheats in effect (lift = false) or gives the game back its own values (lift = true). Game thread.
void cheats_frame(bool lift) {
    if (!g_cheats_ok.load()) return;
    uint32_t bits = g_cheat_bits.load();
    bool allowed = !lift && cheat_mode_allowed();
    for (int i = 0; i < 3; i++) {
        bool want = allowed && (bits >> i & 1);
        uint32_t cur = mem_read32(game_address(kCheatGlobals[i]));
        if (want) {
            if (!g_cheat_forced[i]) g_cheat_saved[i] = cur;
            else if (cur != 1) g_cheat_saved[i] = cur;  // the game set it since (a player was created)
            g_cheat_forced[i] = true;
            mem_write32(game_address(kCheatGlobals[i]), 1);
        } else if (g_cheat_forced[i]) {
            if (cur == 1) mem_write32(game_address(kCheatGlobals[i]), g_cheat_saved[i]);
            g_cheat_forced[i] = false;
        }
    }
    for (int p = 0; p < 4; p++) {
        uint32_t player = g_port_player[p];
        bool recent = player && now_ms() - g_port_player_ms[p] <= 1000;
        uint32_t inv = player ? mem_read32(player + 0x5E8) : 0;
        if (inv < 0x80000000u || inv >= (guest_ram_end() - 0x100u)) inv = 0;
        if (g_inf_inv[p] && (g_inf_inv[p] != inv || !allowed || !(bits & 2))) {  // give the game back its flag
            if (g_inf_inv[p] == inv && mem_read32(inv + 0xD8) == 1) mem_write32(inv + 0xD8, g_inf_saved[p]);
            g_inf_inv[p] = 0;
        }
        if (allowed && (bits & 2) && recent && inv) {
            uint32_t cur = mem_read32(inv + 0xD8);
            if (g_inf_inv[p] != inv || cur != 1) g_inf_saved[p] = cur;
            g_inf_inv[p] = inv;
            mem_write32(inv + 0xD8, 1);
        }
        if (!allowed || !(bits & 6)) { if (!lift) g_ammo_filled[p] = 0; continue; }
        if (!recent || !inv || g_ammo_filled[p] == player) continue;
        for (uint32_t t = 0; t < 18; t++) {
            uint16_t max = mem_read16(inv + 0xB4 + 2 * t);
            mem_write16(inv + 2 * t, max);
            mem_write16(inv + 0x24 + 2 * t, max);
        }
        g_ammo_filled[p] = player;
    }
    float speed = allowed ? g_cheat_speed.load() : 1.0f, jump = allowed ? g_cheat_jump.load() : 1.0f;
    float damage = allowed ? g_cheat_damage.load() : 1.0f;
    uint32_t player = 0;
    for (int p = 0; p < 4 && !player; p++)
        if (g_port_player[p] && now_ms() - g_port_player_ms[p] <= 1000) player = g_port_player[p];
    if ((speed != 1.0f || jump != 1.0f || g_scaled_forced) && player) {
        for (auto& st : g_scaled_settings) {
            uint32_t n = mem_read32(player + st.offset);
            if (n < 0x80000000u || n >= (guest_ram_end() - 0x30u)) continue;
            float cur = u2f(mem_read32(n + 0x24));
            if (cur != st.stock && cur != st.written) continue;  // not that setting
            st.written = st.stock * (st.jump ? jump : speed);
            put_f(n + 0x24, st.written);
        }
        g_scaled_forced = speed != 1.0f || jump != 1.0f;
    }
    // weapon damage: the game's values back first (lifted, or the factor changed), then scaled again
    for (auto it = g_damage_modes.begin(); it != g_damage_modes.end();) {
        bool ours = true;
        for (int f = 0; f < 3; f++) ours &= u2f(mem_read32(it->first + kDamageFields[f])) == it->second.written[f];
        if (ours) for (int f = 0; f < 3; f++) put_f(it->first + kDamageFields[f], it->second.stock[f]);
        it = ours && damage != 1.0f ? std::next(it) : g_damage_modes.erase(it);
    }
    if (damage != 1.0f && player) {
        for (uint32_t id = 0; id < 12; id++) {
            uint32_t w = mem_read32(player + 0x4BC + 0x18 * id);
            if (w < 0x80000000u || w >= (guest_ram_end() - 0x200u)) continue;
            uint32_t modes = mem_read32(w + 0x1D0), count = mem_read32(w + 0x1BC);
            if (modes < 0x80000000u || modes >= (guest_ram_end() - 0x200u) || count < 1 || count > 3) continue;
            for (uint32_t m = 0; m < count; m++) {
                uint32_t a = modes + 60 * m;
                DamageMode& dm = g_damage_modes[a];  // new (the game's values) or put back above
                for (int f = 0; f < 3; f++) {
                    dm.stock[f] = u2f(mem_read32(a + kDamageFields[f]));
                    dm.written[f] = dm.stock[f] * damage;
                    put_f(a + kDamageFields[f], dm.written[f]);
                }
            }
        }
    }
    // flying
    bool fly = allowed && (bits & 16) && player;
    if (g_fly_player && (!fly || g_fly_player != player)) {
        uint32_t fp = g_fly_player;
        uint32_t allow = mem_read32(fp + 0x358);
        if (mem_read32(fp + 0x35C) == 1) {
            mem_write32(fp + 0x35C, 0);
            mem_write32(fp + 0x1C, mem_read32(fp + 0x1C) & ~0xC000u);
        }
        if (allow >= 0x80000000u && allow < (guest_ram_end() - 0x30u)) mem_write32(allow + 0x1C, g_fly_saved_allow);
        g_fly_player = 0;
    }
    if (fly && !g_fly_player) {
        uint32_t allow = mem_read32(player + 0x358);
        if (allow >= 0x80000000u && allow < (guest_ram_end() - 0x30u)) {
            g_fly_saved_allow = mem_read32(allow + 0x1C);
            mem_write32(allow + 0x1C, 1);
            mem_write32(player + 0x35C, 1);
            mem_write32(player + 0x1C, mem_read32(player + 0x1C) | 0xC000u);
            g_fly_player = player;
        }
    }
    g_flying = g_fly_player != 0;
    g_cheats_on = allowed && ((bits & 31) || speed != 1.0f || jump != 1.0f || damage != 1.0f);
}

void hle_unlocked(PPCContext& ctx) {
    PPCFunc* slot = ssne_hook_slot(kUnlocked);
    *slot = nullptr;
    g_orig_unlocked(ctx);
    *slot = &hle_unlocked;
    if ((g_cheat_bits.load() & 8) && cheat_mode_allowed()) ctx.r[3] = 1;
}

void hle_player_cheats(PPCContext& ctx) {
    uint32_t player = ctx.r[3];
    PPCFunc* slot = ssne_hook_slot(kPlayerCheats);
    *slot = nullptr;
    g_orig_player_cheats(ctx);
    *slot = &hle_player_cheats;
    for (uint32_t& f : g_ammo_filled) if (f == player) f = 0;
}

void hle_damage(PPCContext& ctx) {
    if (((g_cheat_bits.load() & 1) && cheat_mode_allowed()) || ((cheat_mode_allowed() || g_authoritative_gameplay) && !replica_simulation() && mod_current_options().god)) {
        ctx.ps0[1] = ctx.ps1[1] = u2f(mem_read32(ctx.r[3] + 0xCC));
        return;
    }
    PPCFunc* slot = ssne_hook_slot(kDamage);
    *slot = nullptr;
    g_orig_damage(ctx);
    *slot = &hle_damage;
}

void install_cheats() {
    uint32_t setter_sig[6];std::copy_n(kCheatSetterSig,6,setter_sig);if(mem_read32(0x80000000u)==0x47334250u)setter_sig[0]=0x812D962Cu;
    const SSNEFuncEntry* ul = ssne_find_func(kUnlocked);
    const SSNEFuncEntry* pc = ssne_find_func(kPlayerCheats);
    const SSNEFuncEntry* dm = ssne_find_func(kDamage);
    if (!ul || !pc || !dm || !code_matches(kCheatSetter, setter_sig) || !code_matches(kUnlocked, kUnlockedSig) ||
        !code_matches(kPlayerCheats, kPlayerCheatsSig) || !code_matches(kDamage, kDamageSig)) {
        fprintf(stdout, "[ssne] cheats: the game's cheat switches not found (different game build?)\n");
        return;
    }
    g_orig_unlocked = ul->func;
    ssne_install_hook(kUnlocked, hle_unlocked);
    g_orig_player_cheats = pc->func;
    ssne_install_hook(kPlayerCheats, hle_player_cheats);
    g_orig_damage = dm->func;
    ssne_install_hook(kDamage, hle_damage);
    g_cheats_ok = true;
    log_message("[cheats] verified native switches for "+std::string(g_ver->id));
}

// ---- save states: the request is carried out at the start of the game's frame flip (savestate.cpp).
const uint32_t kFlipSig[6] = {0x9421FFE8u, 0x7C0802A6u, 0xBFA1000Cu, 0x9001001Cu, 0x7C7F1B78u, 0x7C9D2378u};
PPCFunc g_orig_flip = nullptr;
GameFrameObserver g_game_frame_observer;
uint64_t g_game_frame_number=0;
void hle_flip(PPCContext& ctx) {
    end_player_frame();
    cheats_frame(true);
    perf::detail::Frame perf_frame(perf::detail::GuestFlip);
    CoopHookTimer timer("flip");
    if(g_game_frame_observer)g_game_frame_observer(ctx,++g_game_frame_number);
    if(g_authoritative_gameplay)frame_limit_wait_impl(effective_frame_limit());
    savestate_checkpoint_probe(ctx);
    if(!g_netplay_active)savestate_safe_point(ctx);
    ssne_call_original(ctx, g_ver->flip, g_orig_flip);
    cheats_frame(false);
}
void install_savestates() {
    const SSNEFuncEntry* f = ssne_find_func(g_ver->flip);
    bool ok = f;
    for (int k = 0; k < 6 && ok; k++) ok = mem_read32(g_ver->flip + uint32_t(k) * 4) == kFlipSig[k];
    if (!ok) { fprintf(stdout, "[ssne] save states: frame flip not found (different game build?)\n"); return; }
    g_orig_flip = f->func;
    ssne_install_hook(g_ver->flip, hle_flip);
    g_control_hooks++;
}

void install_pad_fix() {
    if (getenv("SSNE_NO_PAD_FIX")) return;  // A/B testing: the game's original behavior
    g_pad_fix_trace = getenv("SSNE_TRACE_PAD_FIX") != nullptr;
    const SSNEFuncEntry* c = ssne_find_func(g_ver->pad_clamp);
    const SSNEFuncEntry* p = ssne_find_func(g_ver->pad_process);
    if (!c || !p || !code_matches(g_ver->pad_clamp, kPadClampSig) || !code_matches(g_ver->pad_process, kPadProcessSig)) {
        fprintf(stdout, "[ssne] stick fix: controller code not found (different game build?)\n");
        return;
    }
    g_orig_pad_clamp = c->func;
    g_orig_pad_process = p->func;
    ssne_install_hook(g_ver->pad_clamp, hle_pad_clamp);
    ssne_install_hook(g_ver->pad_process, hle_pad_process);
}
} // namespace

void mouse_aim_add(double yaw, double pitch) { mouse_aim_add(g_kbm_port.load(), yaw, pitch); }
void mouse_aim_add(int port, double yaw, double pitch) {
    if (port < 0 || port >= 4 || !std::isfinite(yaw) || !std::isfinite(pitch)) return;
    std::lock_guard<std::mutex> lk(g_aim_mutex);
    // Discard motion during menus, loading and pause rather than replaying it.
    if (now_ms() - g_aim_last_ms[port].load() >= 200) {
        g_aim_yaw[port] = g_aim_pitch[port] = 0; return;
    }
    g_aim_yaw[port] += yaw;
    g_aim_pitch[port] += pitch;
}
void mouse_aim_set_multiplayer(bool enabled) { g_aim_multiplayer = enabled; }
void set_netplay_step(NetplayStep step) {reset_vehicle_input(bool(step));g_authoritative_gameplay=false;g_netplay_active = bool(step); g_netplay_step = std::move(step); }
namespace {void switch_authoritative_sdk(bool);}
void set_authoritative_gameplay(bool enabled) {reset_vehicle_input(enabled);g_netplay_step={};g_authoritative_gameplay=enabled;g_netplay_active=enabled;switch_authoritative_sdk(enabled);}
void netplay_filter_client_pad(PadState& pad,int port) {
    if (!g_netplay_active) return;
    bool character_selection = false;
    for (auto gui : g_netplay_character_guis) {
        if (gui >= 0x80000000u && gui <= (guest_ram_end()-0x100u) &&
            !mem_read32(gui + 0x14) && (mem_read32(gui + 0x6C) ||
             (g_netplay_count>2 && int16_t(mem_read16(gui+0x84))==port))) {
            character_selection = true;
            break;
        }
    }
    if(getenv("SSNE_NATIVE_PARTY_TRACE") && port>=2 && pad.buttons) {
      std::string line="[party-menu-input] port="+std::to_string(port)+" buttons="+std::to_string(pad.buttons)+" allowed="+std::to_string(character_selection);
      for(auto gui:g_netplay_character_guis)if(gui>=0x80000000u && gui<(guest_ram_end()-0x100u))line+=" gui="+std::to_string(gui)+"/"+std::to_string(mem_read32(gui+0x14))+"/"+std::to_string(mem_read32(gui+0x6C))+"/"+std::to_string(int16_t(mem_read16(gui+0x84)));
      log_message(line);
    }
    if (character_selection) return;
    if(getenv("SSNE_NATIVE_PARTY_TRACE") && pad.stick_y!=128)log_message("[party-stick] slot="+std::to_string(port)+" actor="+std::to_string(g_netplay_players[port])+" stick="+std::to_string(pad.stick_y));
    const auto player = g_netplay_players[port];
    if(getenv("SSNE_NATIVE_PARTY_TRACE") && pad.stick_y!=128) { auto bank=mem_read32(0x802ABA9C)+native_party_controller_offset(port);std::string line="[party-actions] slot="+std::to_string(port)+" valid="+std::to_string(mem_read32(player+0x48)==g_netplay_player_types[port]);for(unsigned j=0;j<4;++j)line+=" action="+std::to_string(u2f(mem_read32(bank+0xC+j*28)));log_message(line); }
    const bool gameplay = player >= 0x80000000u && player <= (guest_ram_end()-0x360u) &&
        mem_read32(player + 0x48) == g_netplay_player_types[port];
    if (!gameplay) pad = {};
    else pad.buttons &= ~uint16_t(0x1000); // START opens a shared pause menu.
}
void netplay_set_visibility(float horizontal,float vertical) {
    g_netplay_visibility_x=std::clamp(horizontal,1.f,5.f);
    g_netplay_visibility_y=std::clamp(vertical,1.f,3.f);
}
void netplay_set_roster(unsigned count) { g_netplay_count=std::clamp(count,1u,8u); native_party_configure(g_netplay_count); }
void netplay_set_local_player(int port) { g_netplay_local_player = port; }
int netplay_local_player() {return g_netplay_local_player.load();}
bool netplay_render_local() { return g_netplay_render_engine != 0 && !native_movie_presenting(); }
bool netplay_render_ui() { return g_netplay_render_engine && g_netplay_list_owner >= 0; }
bool netplay_render_gui_camera() {
    if(!g_netplay_render_engine)return false;
    const auto view=int32_t(mem_read32(g_netplay_render_sda-0x6D94));
    if(view>=0 && information_camera(unsigned(view)))return true;
    return native_party_extended() ? (view>=16 && view<24) || view==26 || view==27
                                   : (view>=10 && view<14) || view==14 || view==15;
}
bool netplay_render_world_camera(){return g_netplay_render_engine && int32_t(mem_read32(g_netplay_render_sda-0x6D94))==g_netplay_local_player.load();}
bool netplay_render_discard() {
    if (!g_netplay_render_engine || native_movie_presenting()) return false;
    // A shared movie has no player-owned viewport; its original draw stream
    // must reach every peer, including clients whose local slot is not zero.
    // Diagnostic-only draw isolation: native callbacks still execute.
    // Diagnostic switches are process-start configuration, as in the other
    // renderer probes. Querying the environment for every HUD primitive scans
    // the Windows environment block thousands of times per frame.
    static const bool suppress_hud=getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_SUPPRESS_HUD");
    if(g_netplay_list_owner>=0 && suppress_hud)return true;

    const int slot = int32_t(mem_read32(g_netplay_render_sda - 0x6D94));
    if(slot>=0 && information_camera(unsigned(slot)))return false;
    if(native_cutscene_presenting())return slot!=int(native_party_extended()?24:4) || g_netplay_list_owner>=0;
    const int owner = native_party_extended() && slot>=0 && slot<24?slot%8:slot>=6 && slot<6+int(std::min(g_netplay_count,4u)) ? slot-6 : slot>=0 && slot<int(std::min(g_netplay_count,4u)) ? slot : -1;
    return (owner >= 0 && owner != g_netplay_local_player) || (g_netplay_list_owner >= 0 && g_netplay_list_owner != g_netplay_local_player);
}
void netplay_reset_world() {
    g_netplay_world_ready = false;
    reset_vehicle_input(g_netplay_active.load());
    for (int port=0;port<8;++port) { g_netplay_players[port]=0; g_netplay_player_types[port]=0; }
}
void netplay_register_party_players() {
    if(!g_netplay_active)return;
    const auto registry=native_party_registry();
    for(unsigned slot=0;slot<g_netplay_count;++slot) {
        auto actor=mem_read32(registry+slot*4);
        if(actor>=0x80000000u && actor<=guest_ram_end()-0x790) {
            g_netplay_players[slot]=actor;g_netplay_player_types[slot]=mem_read32(actor+0x48);
            log_message("[party-actor] slot="+std::to_string(slot)+" actor="+std::to_string(actor)+" controller="+std::to_string(mem_read32(actor+0x178))+" player="+std::to_string(mem_read32(actor+0x17C))+" pos="+std::to_string(mem_read32(actor+0x180))+" respawn="+std::to_string(mem_read32(actor+0x3AC))+" timer="+std::to_string(u2f(mem_read32(actor+0x3B4)))+" state="+std::to_string(mem_read32(actor+0x148)));
        }
    }
}
bool netplay_world_ready() { return g_netplay_world_ready.load(); }
void refresh_native_menu_input(PPCContext& ctx){if(g_authoritative_gameplay)refresh_network_pads(ctx);}
uint32_t netplay_player(int port) { return port>=0 && port<8 ? g_netplay_players[port] : 0; }
BotWorld netplay_bot_world() {
    BotWorld world;
    BotActor* actors[2]={&world.host,&world.client};
    for (int i=0;i<2;++i) {
        uint32_t player=g_netplay_players[i?g_netplay_local_player.load():0];
        if (player<0x80000000u || player>=(guest_ram_end()-0x360u) || mem_read32(player+0x48)!=g_netplay_player_types[i?g_netplay_local_player.load():0]) return {};
        actors[i]->id=player;
        for (int axis=0;axis<3;++axis) actors[i]->position[axis]=u2f(mem_read32(player+0x20+axis*4));
        actors[i]->health=u2f(mem_read32(player+0xCC));
    }
    world.yaw=u2f(mem_read32(world.client.id+0x290));
    world.pitch=u2f(mem_read32(world.client.id+0x294));
    world.enemies=netplay_bot_enemies();
    return world;
}
void netplay_mouse(int port, double yaw, double pitch) {
    if (port < 0 || unsigned(port) >= g_netplay_count) return;
    std::lock_guard<std::mutex> lk(g_aim_mutex);
    g_aim_yaw[port] = yaw; g_aim_pitch[port] = pitch;
}
bool mouse_aim_available() { return g_orig_look != nullptr; }
bool mouse_aim_active() { const int port=std::clamp(g_netplay_active?g_netplay_local_player.load():g_kbm_port.load(),0,7); return g_orig_look && (g_replica_control_active || now_ms() - g_aim_last_ms[port].load() < 200); }
bool local_information_open() {return g_authoritative_gameplay && g_authoritative_information_open;}
bool local_pause_menu_open() {return g_replica_pause_menu_open.load() || native_replica_results_presenting();}

bool player_control_active() { return (mouse_aim_active() || player_in_vehicle()) && (!g_netplay_active || (!local_pause_menu_open() && !local_information_open())); }
bool player_in_vehicle() {
    const bool network=g_netplay_active.load();
    const bool driving=g_vehicle_input.local(network,g_netplay_local_player.load(),g_kbm_port.load(),now_ms());
    return detail::native_vehicle_controls(driving,network,g_netplay_world_ready.load(),
        network && local_pause_menu_open(),network && local_information_open(),
        g_vehicle_presentation_blocked.load() || native_results_presenting());
}
void set_kbm_port(int port) { g_kbm_port=std::clamp(port,0,3); }
void weapon_select_group(int group) { g_weapon_req_ms=now_ms();g_weapon_req=group; }
void coop_weapon_group(unsigned slot,int group){if(slot<8 && group>=0 && group<9)g_coop_weapon_groups[slot]=group;}

void set_frame_limit(int fps) { g_frame_limit = fps < 0 ? 0 : fps; }
int frame_limit() { return g_frame_limit.load(); }

namespace { std::atomic<bool> g_paused{false}; }
void set_game_tick_observer(GameFrameObserver observer){g_game_tick_observer=std::move(observer);g_game_tick_number=0;}
void set_game_render_observer(std::function<void(PPCContext&)> begin,std::function<void(PPCContext&)> end){g_game_render_begin=std::move(begin);g_game_render_end=std::move(end);}
void set_native_hud_list_observer(NativeHudListObserver observer){g_native_hud_list_observer=observer;}
void set_game_frame_observer(GameFrameObserver observer){g_game_frame_observer=std::move(observer);g_game_frame_number=0;}
void set_paused(bool p) { g_paused = p; }
bool paused() { return g_paused.load(); }

void frame_limiter_wait() {
    while (g_paused.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if(g_netplay_active)return;
    int lim = effective_frame_limit();
    if (lim == 60 || lim <= 0 || !g_flip_wait_ok) return;
    frame_limit_wait_impl(lim);
}

void frame_limit_wait_impl(int lim){
    if(lim<=0 || lim==60 || !g_flip_wait_ok)return;
    using clk = std::chrono::steady_clock;
    static clk::time_point next = clk::now();
    auto period = std::chrono::nanoseconds(1000000000LL / lim);
    auto now = clk::now();
    if (next < now - period * 2) next = now;  // fell behind (slow frame / loading): don't try to catch up
    if (now < next) {
        auto spin_from = next - std::chrono::microseconds(1500);
        if (now < spin_from) std::this_thread::sleep_until(spin_from);
        while (clk::now() < next) std::this_thread::yield();
    }
    next += period;
}

#include "native_skinning.inc"
#include "native_render_paths.inc"
#include "native_model_update.inc"
#include "native_render_list.inc"
#include "native_animation_pose.inc"
#include "native_linear_math.inc"
#include "native_sphere_query.inc"
namespace native_collision_module {void install();}

namespace {
bool sdk_hooks_ready=false,dynamic_authority=false;
struct AuthorityHook {uint32_t address;PPCFunc offline,network;};
std::vector<AuthorityHook> authority_hooks;
void install_network_sdk_hooks(){

        g_netplay_random_streams.clear();
        if (!ssne_find_func(game_address(0x8015544C)) || mem_read32(game_address(0x8015544C)) != 0x9421FFD8u)
            fatal("Netplay requires the supported game revision.");
        if(g_authoritative_gameplay){
            const auto* tick=ssne_find_func(game_address(0x8015544Cu));
            if(!tick || mem_read32(tick->address)!=0x9421FFD8u)fatal("Unsupported authoritative native game clock.");
            g_original_tick=tick->func;
        }
        ssne_install_hook(game_address(0x8015544C), g_authoritative_gameplay?hle_authoritative_time_step:hle_netplay_time_step);
        if(g_authoritative_gameplay){
            const auto* vehicle=ssne_find_func(0x80170948u);
            if(!vehicle || mem_read32(vehicle->address)!=0x9421FF08u)fatal("Unsupported native vehicle physics revision");
            g_original_vehicle_physics=vehicle->func;ssne_install_hook(vehicle->address,hle_authoritative_vehicle_physics);
            const auto* steering=ssne_find_func(0x80174D48u);
            if(!steering || mem_read32(steering->address)!=0x812301F4u || mem_read32(steering->address+4)!=0xD0290008u)fatal("Unsupported native vehicle steering revision");
            g_original_vehicle_steering=steering->func;ssne_install_hook(steering->address,hle_authoritative_vehicle_steering);
        }
        const uint32_t render = game_address(0x80117434);
        const auto* render_entry = ssne_find_func(render);
        if (!render_entry || mem_read32(render) != 0x9421FF48u)
            fatal("Netplay local-camera renderer does not support this game revision.");
        for (auto item : {std::pair<uint32_t,PPCFunc>{game_address(0x8011E5F4u),hle_netplay_list_render},
                          std::pair<uint32_t,PPCFunc>{0x800B7D84u,hle_netplay_view_create}}) {
            auto entry=ssne_find_func(item.first);
            const uint32_t signature=item.first==0x800B7D84u?0x9421FEC0u:0x9421FA38u;
            if(!entry || mem_read32(item.first)!=signature)fatal("Native player render-list revision is unsupported.");
            if(item.first==0x800B7D84u)g_netplay_view_create=entry->func;else g_netplay_list_render=entry->func;
            ssne_install_hook(item.first,item.second);
        }
        const auto frustum=game_address(0x8014D63Cu);
        auto frustum_entry=ssne_find_func(frustum);
        if(!frustum_entry || mem_read32(frustum)!=0x9421FF18u)
            fatal("Netplay frustum hook does not match this game build.");
        g_netplay_frustum_original=frustum_entry->func;
        ssne_install_hook(frustum,hle_netplay_frustum);
        g_netplay_render_original=render_entry->func;
        ssne_install_hook(render, hle_netplay_render);
        if(g_authoritative_gameplay && mem_read32(0x80000000u)==0x47334250u) {
            const auto* projection_copy=ssne_find_func(0x801DADACu);
            if(!projection_copy || mem_read32(projection_copy->address)!=0x9421FFE8u)fatal("Native portal visibility projection revision unsupported.");
            g_authoritative_visibility_projection_original=projection_copy->func;
            ssne_install_hook(projection_copy->address,hle_authoritative_visibility_projection);
            const auto* viewport=ssne_find_func(0x801179BC);
            if(!viewport || mem_read32(viewport->address)!=0x9421FF80u)fatal("Native viewport diagnostic revision unsupported.");
            g_authoritative_viewport_original=viewport->func;ssne_install_hook(viewport->address,hle_authoritative_viewport);
            const auto* proxy_draw=ssne_find_func(0x8013D4D0u);
            if(!proxy_draw || mem_read32(proxy_draw->address)!=0x9421FF80u)fatal("Native render proxy revision unsupported.");
            g_authoritative_proxy_draw_original=proxy_draw->func;ssne_install_hook(proxy_draw->address,hle_authoritative_proxy_draw);

        }
        if (!ssne_find_func(game_address(0x801C193C)) || mem_read32(game_address(0x801C193C)) != 0x7C0802A6u)
            fatal("Netplay requires the supported PADRead revision.");
        ssne_install_hook(game_address(0x801C193C), hle_netplay_pad_read);
        const auto clamp = ssne_find_func(game_address(0x801C0BAC));
        if (!clamp) fatal("Netplay requires the supported PADClamp revision.");
        g_netplay_pad_clamp = clamp->func;
        ssne_install_hook(game_address(0x801C0BAC), hle_netplay_pad_clamp);
        struct MenuHook { uint32_t address, signature; PPCFunc hook; PPCFunc* original; };
        for (auto h : {MenuHook{game_address(0x800F540C), 0x9421FFE8u, hle_netplay_gui_create, &g_netplay_gui_create},
                       MenuHook{game_address(0x800F56A4), 0x38000000u, hle_netplay_gui_assign, &g_netplay_gui_assign},
                       MenuHook{game_address(0x800F5578), 0x9421FFF0u, hle_netplay_gui_destroy, &g_netplay_gui_destroy},
                       MenuHook{game_address(0x8015A504), 0x9421FFF0u, hle_netplay_random_init, &g_netplay_random_init},
                       MenuHook{game_address(0x8015A54C), 0x7C681B78u, hle_netplay_random_next, &g_netplay_random_next}}) {
            auto entry = ssne_find_func(h.address);
            if (!entry || mem_read32(h.address) != h.signature) fatal("Netplay requires the supported game revision.");
            // The native frontend also owns GUI lifetime state. Preserve that
            // existing destructor participant when enabling network routing.
            const auto previous=*ssne_hook_slot(h.address);
            *h.original=(h.hook==hle_netplay_gui_destroy && previous && previous!=h.hook)?previous:entry->func;
            ssne_install_hook(h.address, h.hook);
        }

}
void install_authoritative_game_hooks(){
    if(mem_read32(0x80000000u)!=0x47334250u)return;

            native_party_install_occupancy_hooks();
            install_native_results_observer();
            install_native_cutscene_gate();
            install_native_music_observer();
            const auto* freeze=ssne_find_func(0x800B31B8);const auto* release=ssne_find_func(0x800B3398);
            if(!freeze || !release || mem_read32(freeze->address)!=0x9421FF78u || mem_read32(release->address)!=0x9421FFF0u)fatal("Unsupported native player camera-control transition.");
            g_authoritative_freeze_original=freeze->func;g_authoritative_release_original=release->func;
            ssne_install_hook(freeze->address,hle_player_freeze);ssne_install_hook(release->address,hle_player_release);
            const auto* update=ssne_find_func(0x800BEEC8);
            if(!update || mem_read32(update->address)!=0x9421FF18u)fatal("Unsupported native player update dispatch.");
            g_authoritative_player_update_original=update->func;ssne_install_hook(update->address,hle_authoritative_player_update);
            const auto* collision=ssne_find_func(0x800C04ACu);
            if(!collision || mem_read32(collision->address)!=0x9421FE98u)fatal("Unsupported native player collision routine");
            g_authoritative_player_collision_original=collision->func;ssne_install_hook(collision->address,hle_authoritative_player_collision);
            const auto* movement=ssne_find_func(0x800B5A78);
            if(!movement || mem_read32(movement->address)!=0x9421FFE8u)fatal("Unsupported native player movement mode.");
            g_authoritative_movement_mode_original=movement->func;ssne_install_hook(movement->address,hle_authoritative_movement_mode);
            const auto* death=ssne_find_func(0x800B3510);
            if(!death || mem_read32(death->address)!=0x9421FFF0u)fatal("Unsupported native player death transition.");
            g_authoritative_player_death_original=death->func;ssne_install_hook(death->address,hle_authoritative_player_death);
            const auto* start=ssne_find_func(0x800B2E5C);
            if(!start || mem_read32(start->address)!=0x9421FFB0u)fatal("Unsupported native player spawn entry.");
            g_authoritative_player_start_original=start->func;ssne_install_hook(start->address,hle_authoritative_player_start);
            const auto* telefrag=ssne_find_func(0x800A369C);
            if(!telefrag || mem_read32(telefrag->address)!=0x9421FFD0u)fatal("Unsupported native cooperative spawn query.");
            g_authoritative_spawn_telefrag_original=telefrag->func;ssne_install_hook(telefrag->address,hle_authoritative_spawn_telefrag);
            const auto* gate=ssne_find_func(0x800C4EEC);
            if(!gate || mem_read32(gate->address)!=0x81230374u)fatal("Unsupported native player control gate.");
            g_authoritative_control_gate_original=gate->func;ssne_install_hook(gate->address,hle_authoritative_control_gate);
            const auto* info=ssne_find_func(0x80101F4C);
            if(!info || mem_read32(info->address)!=0x9421FFD8u)fatal("Unsupported native Netricsa information panel.");
            g_authoritative_netricsa_original=info->func;ssne_install_hook(info->address,hle_authoritative_netricsa);
            const auto* camera_setter=ssne_find_func(0x8011A1F8u);
            if(!camera_setter || mem_read32(camera_setter->address)!=0x1C84004Cu)fatal("Unsupported native widget camera setter.");
            g_information_camera_setter=camera_setter->func;ssne_install_hook(camera_setter->address,hle_information_camera_set);
            const auto* pause=ssne_find_func(0x80155BC8);
            if(pause && mem_read32(pause->address)==0x9421FFF8u){g_authoritative_pause_original=pause->func;ssne_install_hook(pause->address,hle_authoritative_pause);}

}
void switch_authoritative_sdk(bool enabled){
    if(!sdk_hooks_ready || dynamic_authority==enabled)return;
    if(enabled && authority_hooks.empty()){
        std::vector<PPCFunc> before;before.reserve(g_ssne_func_count);
        for(size_t i=0;i<g_ssne_func_count;++i)before.push_back(*ssne_hook_slot(g_ssne_funcs[i].address));
        try{
            install_network_sdk_hooks();install_authoritative_game_hooks();
            const auto* process=ssne_find_func(g_ver->pad_process);
            if(!process || !code_matches(g_ver->pad_process,kPadProcessSig))fatal("Unsupported native party input processor");
            g_orig_pad_process=process->func;g_orig_pad_clamp=g_netplay_pad_clamp;ssne_install_hook(g_ver->pad_process,hle_pad_process);
            for(size_t i=0;i<g_ssne_func_count;++i){const auto address=g_ssne_funcs[i].address;const auto after=*ssne_hook_slot(address);if(before[i]!=after)authority_hooks.push_back({address,before[i],after});}
        }catch(...){for(size_t i=0;i<g_ssne_func_count;++i)ssne_install_hook(g_ssne_funcs[i].address,before[i]);authority_hooks.clear();throw;}
    }else for(const auto& hook:authority_hooks)ssne_install_hook(hook.address,enabled?hook.network:hook.offline);
    dynamic_authority=enabled;g_netplay_world_ready=false;g_replica_pause_menu_open=false;g_authoritative_information_open=false;g_replica_control_active=false;g_host_information_z_consumed=false;
    if(!enabled){g_netplay_character_guis.clear();g_netplay_random_streams.clear();}
    log_message(std::string("[native-network-runtime] active=")+(enabled?"1":"0")+" hooks="+std::to_string(authority_hooks.size()));
}
}

void install_sdk_hooks() {
    // The native setter releases its previous source/children before retaining
    // the incoming descriptor. Keep a borrowed descriptor alive across that
    // operation, including a descriptor also held by an old child source.
    if(mem_read32(0x80000000u)==0x47334250u && ssne_find_func(0x8013DEA8u) && mem_read32(0x8013DEA8u)==0x9421FFF0u &&
       mem_read32(0x8021DC50u+0x1C)==0x8012E164u && mem_read32(0x8021DC50u+0x14)==0x8012E10Cu){
        ssne_install_hook(0x8013DEA8u,[](PPCContext& c){
            const auto descriptor=c.r[4];
            const auto table=descriptor>=0x80000000u && descriptor<=guest_ram_end()-0x34?mem_read32(descriptor):0;
            const bool protect=table>=0x80000000u && table<=guest_ram_end()-0x20 && !mem_read16(table+0x10) && !mem_read16(table+0x18) && mem_read32(table+0x1C)==0x8012E164u && mem_read32(table+0x14)==0x8012E10Cu && mem_read32(descriptor+0x30)<0x40000000u && c.r[1]>=0x80000100u && c.r[1]<=guest_ram_end()-0x80;
            if(protect){auto retain=c;retain.r[3]=descriptor;ssne_call(retain,0x8012E164u);}
            ssne_call_original(c,0x8013DEA8u,ssne_find_func(0x8013DEA8u)->func);
            if(protect){
                // The setter now owns a reference, so release's original
                // positive-count branch cannot invoke a destructor. Preserve
                // the caller's guest save area around this extra native call.
                if(mem_read32(descriptor)!=table || mem_read32(descriptor+0x30)<2)fatal("Native animation descriptor lost its protected reference");
                std::array<uint8_t,0x100> stack{};const auto low=c.r[1]-0x80;
                std::memcpy(stack.data(),ssne::host_ptr(low),stack.size());
                auto release=c;release.r[3]=descriptor;ssne_call(release,0x8012E10Cu);
                const auto count=mem_read32(descriptor+0x30);
                std::memcpy(ssne::host_ptr(low),stack.data(),stack.size());
                if(descriptor+0x30>=low && descriptor+0x30<low+stack.size())mem_write32(descriptor+0x30,count);
            }
        });
    }
    select_version();
    if(g_netplay_active)install_network_sdk_hooks();

    int n = 0;
    for (size_t i = 0; i < g_ssne_func_count; i++) {
        const auto& f = g_ssne_funcs[i];
        if (!f.name || !*f.name) continue;
        for (auto& h : kHooks)
            if (!strcmp(f.name, h.name)) { ssne_install_hook(f.address, h.fn); n++; }
    }
    static const bool no_game_hooks = getenv("SSNE_NO_GAME_HOOKS") != nullptr;  // A/B testing: SDK-only hooks
    if (!no_game_hooks)
    for (const auto& h : address_hooks()) {
        bool ok = true;
        for (int k = 0; k < 6; k++) ok &= mem_read32(h.addr + uint32_t(k) * 4) == h.sig[k];
        if (ok) { ssne_install_hook(h.addr, h.fn); n++; }
        else fprintf(stdout, "[ssne] %s not found at %08X (different game build?): widescreen unavailable\n", h.name, h.addr);
    }
    fprintf(stdout, "[ssne] installed %d native SDK replacements\n", n);
    if (!no_game_hooks) {
        install_frame_limiter();
        install_vehicle_detector();
        install_mouse_aim();
        if (!g_netplay_active) install_pad_fix();
        else if(g_netplay_count>2) {
            const auto process=ssne_find_func(g_ver->pad_process);
            if(!process || !code_matches(g_ver->pad_process,kPadProcessSig))fatal("Unsupported native party input processor.");
            g_orig_pad_process=process->func;g_orig_pad_clamp=g_netplay_pad_clamp;
            ssne_install_hook(g_ver->pad_process,hle_pad_process);
        }
        if(!g_netplay_active) {
            const auto* tick=ssne_find_func(game_address(0x8015544Cu));
            if(tick && mem_read32(tick->address)==0x9421FFD8u){g_original_tick=tick->func;ssne_install_hook(tick->address,hle_game_tick);}
        }
        if(!g_netplay_active){
            install_step_counter_fix();install_vehicle_step_fix();install_landing_fix();
            install_split_choice();install_frame_time();
        }
        install_cheats();
        install_savestates();
        install_mod_hooks();
        install_replica_simulation_hooks();
        native_skinning::install();
        native_render_paths::install();
        native_model_update::install();
        native_render_list_adapter::install();
        native_animation_pose::install();
        native_linear_math::install();
        native_sphere_query::install();
        native_collision_module::install();
        native_party_install_geometry_trace();
        if(g_authoritative_gameplay)install_authoritative_game_hooks();
    }
    install_call_tracer();
    install_perf_native_probes();
    sdk_hooks_ready=true;
}

void set_cheats(const Cheats& c) {
    g_cheat_bits = uint32_t(c.invulnerable) | uint32_t(c.infinite_ammo) << 1 | uint32_t(c.all_weapons) << 2 |
                   uint32_t(c.unlock_all) << 3 | uint32_t(c.fly) << 4;
    g_cheat_speed = std::clamp(c.speed, 0.5f, 3.0f);
    g_cheat_jump = std::clamp(c.jump, 0.5f, 5.0f);
    g_cheat_damage = std::clamp(c.damage, 1.0f, 100.0f);
}
bool cheats_available() { return g_cheats_ok.load(); }
bool cheats_active() { return g_cheats_on.load(); }
bool cheat_flying() { return g_flying.load(); }

void set_fov(float degrees) { g_fov = degrees <= 0 ? 0.0f : std::clamp(degrees, 30.0f, 120.0f); }
float fov() { return g_fov.load(); }

void set_widescreen(float factor) { g_widescreen = factor < 1.0f ? 1.0f : factor; }
float widescreen() { return g_widescreen.load(); }
float widescreen_frame_factor() { return g_frame_factor.load(); }
bool widescreen_frame_zoomed() { return g_netplay_active?zoomed_in():g_frame_zoom.load(); }
double game_time() { return g_game_time.load(); }
void set_split_two(int mode) { g_split_two = mode < 0 ? -1 : std::min(mode, 1); }
bool frontend_presenting() { return mem_read32(game_address(0x802DB0E0u))==3; }

} // namespace ssne


