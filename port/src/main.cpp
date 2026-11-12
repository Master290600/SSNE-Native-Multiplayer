// ssne-port: the PC executable. Boots the recompiled game on a guest thread and shows a live
// status window (game log, GPU/disc/audio activity) while the GX renderer is being built.
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <mutex>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <utility>

#include "ssne/audio.h"
#include "ssne/exi.h"
#include "ssne/gx.h"
#include "ssne/gx_gpu.h"
#include "ssne/hw.h"
#include "ssne/input.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/scheduler.h"
#include "ssne/thread_util.h"
#include "settings.h"
#include "controller_input.h"
#include "settings_menu.h"
#include "settings_menu_i18n.h"
#include "mod_menu.h"
#include "png_write.h"
#include "game_view_geometry.h"
#include "native_menu_mouse.h"
#include "native_menu_carousel.h"
#include "native_crosshair.h"
#include "crosshair_renderer.h"

#include "ssne/performance.h"
#include "ssne/game_addresses.h"
#include "ssne/native_entity.h"
#include "ssne/native_party.h"
#include "ssne/native_results.h"
#include "ssne/native_cutscene.h"
#include "ssne/native_music.h"
#include "ssne/replica_simulation.h"
#include "ssne/native_effect.h"
#include "coop/native_vehicle.h"
#include "coop/startup_support.h"
#include "coop/startup_trace.h"
#include "coop/render_backpressure.h"
#include "coop/frame_pacing.h"
#include "netplay.h"
#include "coop/native_world.h"
#include "coop/native_configuration.h"
#include "coop/native_bootstrap.h"
#include "coop/native_session.h"
#include "coop/native_lobby.h"
#include "coop/native_frontend.h"
#include "coop/startup_save.h"
#include "test_bot.h"
#include "netplay_test_script.h"
#include "ssne/mods.h"
#include "ssne/ammo_rules.h"
#include "ssne/bot.h"


namespace {

port::netplay::Session g_net;
bool g_net_host = false, g_net_client = false;
std::atomic<bool> g_coop_host{false},g_coop_client{false};
std::recursive_mutex g_session_mutex;
bool g_native_frontend_enabled=false;
void setup_native_frontend(const std::string&);
void session_frontend_frame(PPCContext&,uint64_t);
void session_frontend_tick(PPCContext&,uint64_t);
std::unique_ptr<port::coop::AuthoritativeServer> g_coop_server;
std::unique_ptr<port::coop::AuthoritativeClient> g_coop_peer;
std::unique_ptr<port::coop::NativeSession> g_coop_engine;
std::atomic<bool> g_coop_engine_ready{false};
port::coop::savefile::Identity g_coop_save_identity;
unsigned g_coop_capacity=8;
std::string g_coop_level="Rlevel1_1";
bool g_coop_start_menu=false,g_coop_start_explicit=false;

int g_coop_start_slot=0;
std::optional<port::coop::savefile::State> g_coop_start_save;
std::atomic<unsigned> g_coop_local_player{0};
double g_coop_yaw=0,g_coop_pitch=0; // UI thread accumulates until the next input submission.
uint8_t g_coop_weapon=255;

bool network_game(){return g_net_host || g_net_client || g_coop_host || g_coop_client;}
void stop_network(){std::lock_guard lock(g_session_mutex);g_net.stop();if(g_coop_server)g_coop_server->stop();if(g_coop_peer)g_coop_peer->stop();}
std::unique_ptr<port::coop::NativeWorld> g_native_world;
unsigned g_net_players=2;
uint64_t g_presented_game_frames=0; // UI thread: freshly drawn game images actually presented
ssne::perf::Writer g_performance;
ssne::perf::Display g_performance_display;
std::atomic<uint64_t> g_game_updates{0}; // actual native time-step observer, not display copies
std::atomic<bool> g_test_bot{false};
unsigned g_initial_bots=0,g_bot_skill=2;
std::atomic<uint64_t> g_coop_command_sequence{0};
std::string g_lan_pin, g_lan_addresses;
std::mutex g_log_mutex;
std::deque<std::string> g_log;
FILE* g_log_file = nullptr;
std::string g_status = "Starting...";
std::atomic<bool> g_guest_running{false};
bool g_scroll_to_bottom = true;

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log.push_back(s);
    if (g_log.size() > 4000) g_log.pop_front();
    if (g_log_file) { fprintf(g_log_file, "%s\n", s.c_str()); fflush(g_log_file); }
    g_scroll_to_bottom = true;
}

void set_status(const std::string& s) {
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        g_status = s;
    }
    log_line("[port] " + s);
}

[[noreturn]] void throw_fatal(const char* msg) { throw std::runtime_error(msg); }

void bootstrap_native_game(PPCContext& context,uint64_t tick) {
    if(const char* level=getenv("SSNE_COOP_BOOTSTRAP_LEVEL");level && *level) {
        static bool queued=false;
        if(!queued && tick>=30 && ssne::native_entity_hooks_available() && mem_read32(ssne::game_address(0x802DB0E0))==3) {
            auto profiles=port::coop::read_native_configuration(port::coop::native_global_configuration(),"PlayerProfiles.Profile0");
            if(profiles.values.contains("PlayerProfiles.Profile0.ID")) {
                port::coop::NativeBootstrapRequest request;request.level=level;
                request.game_setup={{"GameSetup.Player1.Profile",int64_t(0)},{"GameSetup.Player1.Pos",int64_t(0)},{"GameSetup.Player2.Profile",int64_t(-1)},{"GameSetup.Player3.Profile",int64_t(-1)},{"GameSetup.Player4.Profile",int64_t(-1)}};
                std::string error;
                queued=port::coop::queue_native_coop_level(context,request,error);
                log_line("[coop-bootstrap] queued="+std::to_string(queued)+" level="+request.level+" pending="+std::to_string(mem_read32(ssne::game_address(0x802DB0E4)))+" error="+error);
            }
        }
    }
}

void capture_native_game(PPCContext& context,uint64_t tick) {
            if(g_native_world && tick%60==0) {
                auto sample=g_native_world->capture(tick);
                log_line("[coop-native] tick="+std::to_string(tick)+" epoch="+std::to_string(sample.epoch)+" scene="+std::to_string(sample.scene)+" pending="+std::to_string(mem_read32(ssne::game_address(0x802DB0E4)))+" actors="+std::to_string(sample.entities.size())+" unmapped="+std::to_string(sample.unmapped.size())+" created="+std::to_string(sample.created)+" registered="+std::to_string(sample.registered)+" removed="+std::to_string(sample.removed));
                if(sample.scene==4) {
                    if(getenv("SSNE_COOP_CAPTURE_RAM")) {
                        static bool dumped=false;
                        if(!dumped && std::any_of(sample.entities.begin(),sample.entities.end(),[](const auto& item){return item.second.kind==port::coop::EntityKind::Player;})) {
                            FILE* out=fopen("native-capture.ram","wb");
                            if(out){fwrite(ssne::g_ram,1,ssne::guest_ram_size(),out);fclose(out);}
                            out=fopen("native-capture-objects.csv","w");
                            if(out){for(const auto& [id,entity]:sample.entities)fprintf(out,"%llu,%u,%u,%u\n",(unsigned long long)id,g_native_world->local_address(id),unsigned(entity.kind),entity.definition);fclose(out);}
                            dumped=true;
                        }
                    }
                    static bool config_printed=false;
                    if(!config_printed) {
                        auto cfg=port::coop::read_native_configuration(port::coop::native_global_configuration(),"GameSetup");
                        log_line("[coop-config] count="+std::to_string(cfg.values.size())+" unsupported="+std::to_string(cfg.unsupported)+" error="+cfg.error);
                        for(const auto& [key,value]:cfg.values) {
                            std::string text;
                            if(auto n=std::get_if<int64_t>(&value))text=std::to_string(*n);
                            else if(auto f=std::get_if<float>(&value))text=std::to_string(*f);
                            else if(auto t=std::get_if<std::string>(&value))text=*t;
                            log_line("[coop-config-value] "+key+"="+text);
                        }
                        config_printed=true;
                    }
                    for(const auto& [id,entity]:sample.entities) {
                        auto p=std::get<std::array<float,3>>(entity.properties.at("position"));
                        log_line("[coop-actor] id="+std::to_string(id)+" kind="+std::to_string(unsigned(entity.kind))+" definition="+std::to_string(entity.definition)+" fields="+std::to_string(entity.properties.size())+" position="+std::to_string(p[0])+","+std::to_string(p[1])+","+std::to_string(p[2]));
                    }
                    static std::set<uint32_t> seen_classes;
                    for(const auto& unknown:sample.unmapped)if(seen_classes.insert(unknown.vtable).second)
                        log_line("[coop-unmapped] address="+std::to_string(unknown.address)+" vtable="+std::to_string(unknown.vtable));
                }
            }
}

void guest_main(std::string data_dir) {
    g_guest_running = true;
    static PPCContext ctx;
    try {
        if (!g_ssne_has_game_code) {
            set_status("This build has no recompiled game code. Run ssne-recomp on your main.dol and rebuild "
                       "with -DSSNE_RECOMPILED_DIR (see docs/BUILDING.md).");
            g_guest_running = false;
            return;
        }
        if ((g_net_host || g_net_client) && !g_net.wait_ready())
            throw std::runtime_error(g_net.status().message);
        if(g_net_host || g_net_client) {
            auto roster=g_net.status();

            ssne::netplay_set_roster(roster.player_count);
            ssne::netplay_set_local_player(int(roster.local_player));
            ssne::mod_netplay_player(int(roster.local_player));
            for(unsigned slot=0;slot<4;++slot)ssne::set_pad_connected(slot,slot<roster.player_count);
        }
        if (g_net_client && !ssne::exi_configure_memcard_snapshot(g_net.initial_card()))
            throw std::runtime_error("Cannot mount synchronized host save.");
        if(g_coop_host || g_coop_client) {
            auto status=g_coop_host?g_coop_server->status():g_coop_peer->status();
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(!status.connected && g_coop_client && std::chrono::steady_clock::now()<deadline) {
                if(status.message.starts_with("Stopped") || status.message.starts_with("Rejected"))break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));status=g_coop_peer->status();
            }
            if(!status.connected)throw std::runtime_error(status.message);
            g_coop_local_player=status.player.slot;
            ssne::netplay_set_roster(status.capacity);ssne::netplay_set_local_player(int(status.player.slot));
            ssne::mod_netplay_player(int(status.player.slot));ssne::native_party_extended_memory(true);
            for(unsigned slot=0;slot<4;++slot)ssne::set_pad_connected(slot,slot<status.capacity);
            if(g_coop_host)g_coop_engine=std::make_unique<port::coop::NativeSession>(*g_coop_server,status.capacity,g_coop_level,log_line);
            else g_coop_engine=std::make_unique<port::coop::NativeSession>(*g_coop_peer,log_line);
            if(g_coop_host)g_coop_engine->configure_saves(g_coop_save_identity);
            if(g_coop_start_save){g_coop_engine->configure_startup_save(std::move(*g_coop_start_save),g_coop_start_slot);g_coop_start_save.reset();}
            g_coop_engine_ready.store(true,std::memory_order_release);
        } else if((getenv("SSNE_COOP_CAPTURE") && *getenv("SSNE_COOP_CAPTURE")) || (getenv("SSNE_COOP_BOOTSTRAP_LEVEL") && *getenv("SSNE_COOP_BOOTSTRAP_LEVEL"))) {
            g_native_world=std::make_unique<port::coop::NativeWorld>();
            if(getenv("SSNE_COOP_BOOTSTRAP_LEVEL") && *getenv("SSNE_COOP_BOOTSTRAP_LEVEL"))ssne::native_party_extended_memory(true);
            ssne::set_game_frame_observer(capture_native_game);
            ssne::set_game_tick_observer(bootstrap_native_game);
            for(unsigned slot=0;slot<8;++slot)g_native_world->set_player_owner(slot,{slot,1});
        }
        std::string err;
        if(g_native_frontend_enabled)ssne::set_authoritative_gameplay(false);
        uint32_t entry = ssne::boot(ctx, data_dir, err);
        if (!entry) throw std::runtime_error(err);
        port::install_native_crosshair();
        if(g_native_frontend_enabled){
            setup_native_frontend(data_dir);port::coop::install_native_frontend();
            if(g_coop_host || g_coop_client)ssne::set_authoritative_gameplay(true);
            ssne::set_game_frame_observer([](PPCContext& c,uint64_t frame){port::native_crosshair_frame();session_frontend_frame(c,frame);});ssne::set_game_tick_observer(session_frontend_tick);
            ssne::set_game_render_observer([](PPCContext& c){if(g_coop_engine_ready)g_coop_engine->render_begin(c);},[](PPCContext& c){if(g_coop_engine_ready)g_coop_engine->render_end(c);});
        }
        if(!g_native_frontend_enabled) {
            if(g_native_world)ssne::set_game_frame_observer([](PPCContext& c,uint64_t frame){port::native_crosshair_frame();capture_native_game(c,frame);});
            else ssne::set_game_frame_observer([](PPCContext&,uint64_t){port::native_crosshair_frame();});
        }
        if(g_coop_host || g_coop_client)port::coop::install_native_lobby(g_coop_server.get(),g_coop_peer.get());
        set_status("Running recompiled game code");
        ssne::scheduler_run_main(ctx, entry);
        set_status("Game code returned from its entry point.");
    } catch (const std::bad_alloc&) {
        char info[256];
        snprintf(info, sizeof(info), "Stopped: memory allocation failed (guest LR=%08X, stack=%08X, r3=%08X, r4=%08X, r5=%08X)", ctx.lr, ctx.r[1], ctx.r[3], ctx.r[4], ctx.r[5]);
        set_status(info);
        stop_network();
    } catch (const std::exception& e) {
        set_status(std::string("Stopped: ") + e.what());
        stop_network();
    }
    g_guest_running = false;
}

// ---------------------------------------------------------------- game view

GLuint g_tex = 0;
int g_tex_w = 0, g_tex_h = 0;
uint64_t g_frame_seq = 0;
// UI-thread copy of exactly the image rectangle submitted by draw_game_view.
port::GameViewRect g_pointer_image;
float g_pointer_origin_x=0,g_pointer_origin_y=0;
uint64_t g_pointer_image_sequence=0,g_pointer_view_generation=0;
std::vector<uint32_t> g_pixels;

bool update_game_texture() {
    int w, h;
    if (!ssne::gx_get_frame(g_pixels, w, h, g_frame_seq)) return false;
    if (!g_tex) {
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (w != g_tex_w || h != g_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels.data());
        g_tex_w = w;
        g_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels.data());
    }
    return true;
}

bool g_gpu = false;       // GPU renderer active
// Main-loop heartbeat for the watchdog (hang diagnostics in ssne.log).
std::atomic<uint64_t> g_heartbeat{0};
std::atomic<const char*> g_loop_phase{"start"};

void watchdog() {
    uint64_t last = 0;
    int stuck = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t hb = g_heartbeat.load();
        if (hb != last) {
            if (stuck >= 5) log_line("[watchdog] main loop resumed after " + std::to_string(stuck) + " s");
            last = hb; stuck = 0; continue;
        }
        stuck++;
        if (stuck == 5 || (stuck > 5 && stuck % 30 == 0)) {
            std::string msg = "[watchdog] main loop stuck for " + std::to_string(stuck) + " s in: " + g_loop_phase.load();
            if (g_gpu) msg += " | renderer: " + ssne::gpu_renderer_phase();
            log_line(msg);
        }
    }
}
// Settings (settings.ini next to the executable, F12 menu).
port::Settings g_set;
std::string g_settings_path;
// Widescreen modes (F8 cycles): factor applied to the game's horizontal field of view in gameplay.
struct AspectMode { const char* name; float factor; };
// The last one follows the window (factor 0: its width / height, see aspect_factor).
const AspectMode kAspects[] = {{"4:3", 1.0f}, {"16:9", 4.0f / 3.0f}, {"21:9", 7.0f / 4.0f}, {"auto", 0.0f}};
constexpr int kAspectCount = 4;
// Automatic: as wide as the window (3440x1440 is 2.39:1, wider than 21:9; 16:10 narrower than 16:9), from 4:3 up to
// 32:9 (wider windows, e.g. three screens, get bars: the HUD layout and a horizontal FOV of over 140 degrees aren't
// made for more).
float aspect_factor(SDL_Window* win) {
    float f = kAspects[std::clamp(g_set.aspect, 0, kAspectCount - 1)].factor;
    if (f > 0) return f;
    int w = 0, h = 0;
    if (win) SDL_GL_GetDrawableSize(win, &w, &h);
    if (w <= 0 || h <= 0) return 4.0f / 3.0f;
    return std::clamp(float(w) / float(h) / (4.0f / 3.0f), 1.0f, 8.0f / 3.0f);
}
// Frame rate limit (F10 cycles these; the menu also offers 165 and 240): 60 = the game's own pacing.
const int kFpsModes[] = {60, 120, 144, 0};
std::string fps_mode_name(int v) { return v == 0 ? std::string("unlimited") : std::to_string(v); }
bool have_game_image() { return g_gpu ? ssne::gpu_renderer_output().texture != 0 : g_tex != 0; }

// ---------------------------------------------------------------- automatic internal resolution (scale = 0)
// Starts at the scale that fits the window (the GameCube image is ~448 lines), then follows the measured GPU time:
// one step down when frames take too long for the frame rate target, one step up (after a quiet period) when the
// next scale would still fit comfortably. This is what keeps weaker GPUs (integrated graphics) at full speed.
struct AutoScale {
    int cur = 0;                 // scale in use
    double gpu_ms = 0;           // last measured average GPU time per frame
    double acc_ms = 0;
    int acc_frames = 0;
    uint32_t window_start = 0, hold_up_until = 0;
    bool measurable = true;
} g_auto;

// Height of the game image in the window, in pixels (4:3, or wider in widescreen gameplay).
double shown_height(SDL_Window* win) {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(win, &w, &h);
    float wide = g_gpu ? std::max(1.f, ssne::gpu_renderer_output().wide) : 1.f;
    return std::min(double(h), double(w) / (4.0 / 3.0 * wide));
}

// Native resolution (scale = -1): the image rendered at exactly the size it's shown (twice that with supersampling).
int native_height(SDL_Window* win) {
    int nh = std::max(224, int(std::lround(shown_height(win))));
    if (g_set.antialiasing == "ssaa") nh *= 2;
    return std::min(nh, 448 * 8);
}

int fit_scale(SDL_Window* win) {
    // Height of the game image in GameCube lines (~448).
    double shown_h = shown_height(win);
    int fit = std::clamp(int(shown_h / 448.0 + 0.75), 1, 6);
    if (g_set.antialiasing == "ssaa") fit = std::min(8, fit * 2);  // supersampling: twice the needed resolution
    return fit;
}

void update_auto_scale(SDL_Window* win) {
    if (!g_gpu) return;
    double ms = 0;
    int frames = 0;
    g_auto.measurable = ssne::gpu_renderer_take_gpu_time(ms, frames);
    g_auto.acc_ms += ms;
    g_auto.acc_frames += frames;
    uint32_t now = SDL_GetTicks();
    if (!g_auto.window_start) g_auto.window_start = now;
    int fit = fit_scale(win);
    if (g_set.scale > 0) g_auto.cur = g_set.scale;
    else if (g_set.scale == 0 && (g_auto.cur == 0 || g_auto.cur > fit)) g_auto.cur = fit;  // start, or the window got smaller
    // Judge once a second, or after up to 3 s when the GPU is so slow that few frames finish (it must still be
    // able to step down from there).
    uint32_t elapsed = now - g_auto.window_start;
    if ((elapsed >= 1000 && g_auto.acc_frames >= 8) || (elapsed >= 3000 && g_auto.acc_frames >= 2) || elapsed >= 6000) {
        if (g_auto.acc_frames >= 2) g_auto.gpu_ms = g_auto.acc_ms / g_auto.acc_frames;
        bool judge = g_auto.acc_frames >= 2 && !ssne::paused();
        g_auto.acc_ms = 0;
        g_auto.acc_frames = 0;
        g_auto.window_start = now;
        if (g_set.scale == 0 && judge && g_auto.measurable) {
            int target_fps = ssne::frame_limit() == 0 ? 60 : std::max(60, ssne::frame_limit());
            double budget = 1000.0 / target_fps;
            double s = g_auto.cur;
            if (g_auto.gpu_ms > budget * 0.8 && g_auto.cur > 1) {
                g_auto.cur--;
                g_auto.hold_up_until = now + 30000;  // don't bounce straight back up
                char buf[128];
                snprintf(buf, sizeof(buf), "[gpu] automatic resolution: %dx (the GPU needed %.1f ms per frame)", g_auto.cur, g_auto.gpu_ms);
                log_line(buf);
            } else if (g_auto.cur < fit && now >= g_auto.hold_up_until &&
                       g_auto.gpu_ms * ((s + 1) * (s + 1)) / (s * s) < budget * 0.55) {
                g_auto.cur++;
                g_auto.hold_up_until = now + 5000;
                log_line("[gpu] automatic resolution: " + std::to_string(g_auto.cur) + "x");
            }
        }
    }
    if (g_set.scale < 0) {  // native: follows the window (and the image's width: 4:3 front end, widescreen gameplay)
        int nh = native_height(win);
        if (ssne::gpu_renderer_native() != nh) ssne::gpu_renderer_set_native(nh);
    } else if (ssne::gpu_renderer_native() || ssne::gpu_renderer_scale() != g_auto.cur) {
        ssne::gpu_renderer_set_scale(g_auto.cur);
    }
}

void draw_game_view() {
    GLuint tex=g_tex,fill=0;
    float factor=ssne::widescreen_frame_factor();uint64_t image_sequence=g_frame_seq;
    if(g_gpu){const auto output=ssne::gpu_renderer_output();tex=output.texture;fill=output.fill;factor=output.wide;image_sequence=output.seq;}
    const ImGuiViewport* vp=ImGui::GetMainViewport();
    const auto image=tex?port::fit_game_view(vp->Pos.x,vp->Pos.y,vp->Size.x,vp->Size.y,factor):port::GameViewRect{};
    if(image.x!=g_pointer_image.x || image.y!=g_pointer_image.y || image.width!=g_pointer_image.width || image.height!=g_pointer_image.height || image.wide!=g_pointer_image.wide || vp->Pos.x!=g_pointer_origin_x || vp->Pos.y!=g_pointer_origin_y)++g_pointer_view_generation;
    g_pointer_image=image;g_pointer_origin_x=vp->Pos.x;g_pointer_origin_y=vp->Pos.y;g_pointer_image_sequence=image_sequence;
    port::set_native_menu_mouse_display(image.wide);
    if(!tex || !image.valid())return;
    const float W=vp->Size.x,H=vp->Size.y,w=image.width,h=image.height;
    const ImVec2 p0(image.x,image.y);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd*) { glDisable(GL_BLEND); }, nullptr);  // the game image is opaque
    if (fill && w < W - 1.f) {  // only the actual image, not its blurred side fill, accepts input
        float fh = W * 0.75f;
        dl->AddImage((ImTextureID)(intptr_t)fill, ImVec2(vp->Pos.x, vp->Pos.y + (H - fh) * 0.5f), ImVec2(vp->Pos.x + W, vp->Pos.y + (H + fh) * 0.5f));
    }
    dl->AddImage((ImTextureID)(intptr_t)tex, p0, ImVec2(p0.x + w, p0.y + h));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    port::draw_mod_overlay(p0.x,p0.y,w,h);
}

// Pixels per window point (display scaling on Windows, Wayland): the menu font is rasterized at the size it's shown
// so its text stays sharp, and rebuilt when the scale changes (the window moves to a screen with another scale).
float g_ui_scale = 0;
void update_ui_scale(SDL_Window* win) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    float s = ww > 0 ? std::clamp(std::round(float(dw) / ww * 4) / 4, 1.f, 3.f) : 1.f;
    if (s == g_ui_scale) return;
    bool rebuild = g_ui_scale != 0;
    g_ui_scale = s;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = std::round(13 * s);
    io.Fonts->AddFontDefault(&cfg);
    io.FontGlobalScale = 13 / cfg.SizePixels;
    if (rebuild) { ImGui_ImplOpenGL3_DestroyFontsTexture(); ImGui_ImplOpenGL3_CreateFontsTexture(); }
}

// ---------------------------------------------------------------- input
// Keyboard/mouse and gamepad bindings come from g_set (defaults in settings.cpp; rebind in the F12 menu).
// Controllers: up to four in use, in slots numbered in the order they were connected (a controller connected later
// takes the first free slot). Each slot plays the player g_set.pad_player says, the keyboard and mouse
// g_set.kbm_player (multiplayer: every player has its own controller port).
constexpr int kPadSlots = port::Settings::kPadSlots;
SDL_GameController* g_pads[kPadSlots] = {};
SDL_GameController* g_last_pad = nullptr;  // the controller used last (on-screen buttons, info)

SDL_GameController* first_pad() {
    for (SDL_GameController* c : g_pads) if (c) return c;
    return nullptr;
}
int pad_slot(SDL_JoystickID id) {
    for (int i = 0; i < kPadSlots; i++)
        if (g_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) == id) return i;
    return -1;
}
void open_pad(int device) {
    if(getenv("SSNE_TEST_HIDDEN"))return;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    if (pad_slot(id) >= 0) return;  // already open (SDL announces the controllers present at startup too)
    for (int i = 0; i < kPadSlots; i++)
        if (!g_pads[i]) {
            g_pads[i] = SDL_GameControllerOpen(device);
            if (g_pads[i]) {
                g_last_pad = g_pads[i];
                const char* n = SDL_GameControllerName(g_pads[i]);
                log_line("[port] controller " + std::to_string(i + 1) + " connected: " + (n ? n : "?"));
            }
            return;
        }
}
void close_pad(SDL_JoystickID id) {
    int i = pad_slot(id);
    if (i < 0) return;
    if (g_last_pad == g_pads[i]) g_last_pad = nullptr;
    log_line("[port] controller " + std::to_string(i + 1) + " disconnected");
    SDL_GameControllerClose(g_pads[i]);
    g_pads[i] = nullptr;
    if (!g_last_pad) g_last_pad = first_pad();
}
// The player (0-3) a controller slot plays, -1 if not used.
int slot_player(int slot) { return port::controller_player(g_set,slot,network_game()); }

// A port is plugged in when the keyboard or a connected controller plays its player (port 1 always: the game needs
// it); the game lets the players of plugged-in ports join a match.
void update_ports() {
    if(network_game())return;
    bool on[4] = {true, false, false, false};
    on[std::clamp(g_set.kbm_player - 1, 0, 3)] = true;
    for (int i = 0; i < kPadSlots; i++)
        if (g_pads[i] && slot_player(i) >= 0) on[slot_player(i)] = true;
    for (int p = 1; p < 4; p++)
        if (ssne::pad_connected(p) != on[p]) {
            ssne::set_pad_connected(p, on[p]);
            log_line("[port] player " + std::to_string(p + 1) + (on[p] ? ": controller port plugged in" : ": controller port empty"));
        }
    ssne::set_kbm_port(std::clamp(g_set.kbm_player - 1, 0, 3));
}
float g_mouse_dx = 0, g_mouse_dy = 0;
bool g_mouse_captured = false;
bool g_pointer_restore_capture=false;
Uint32 g_capture_click = 0;  // the mouse button whose click captured the mouse: only captures (update_input)

uint8_t axis_to_u8(float v) { return uint8_t(std::clamp(128.f + v * 127.f, 0.f, 255.f)); }

void release_mouse() { g_pointer_restore_capture=false;g_mouse_captured=false;if(!getenv("SSNE_TEST_HIDDEN"))SDL_SetRelativeMouseMode(SDL_FALSE); }

// Radial deadzone with the remaining range rescaled to 0..1.
void deadzone(float& x, float& y, float dz) {
    float m = std::sqrt(x * x + y * y);
    if (m <= dz || m <= 0) { x = y = 0; return; }
    float k = std::min(1.f, (m - dz) / std::max(1e-3f, 1.f - dz)) / m;
    x *= k; y *= k;
}

// Development: scripted controller input for automated runs of the real executable (same syntax as the headless
// runner's --press): SSNE_AUTOPRESS="SUP@8,A@9,A@12:0.5" holds each input at t seconds for dur (default 0.2 s).
// SSNE_VIRTUAL_PADS=N attaches N virtual controllers (SDL's own, seen like real ones); "pK:" items press SDL buttons
// (a, b, x, y, start, leftshoulder, ...) or stick directions (leftx+, lefty-, righttrigger+, ...) on virtual pad K,
// "pK:detach" unplugs it.
struct AutoPress { std::string name; double at, dur; };
std::vector<AutoPress> g_autopress;
std::chrono::steady_clock::time_point g_autopress_t0;
void load_autopress() {
    const char* env = getenv("SSNE_AUTOPRESS");
    if (!env) return;
    g_autopress_t0 = std::chrono::steady_clock::now();
    std::string spec = env;
    for (size_t pos = 0; pos < spec.size();) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t at = item.find('@'), colon = at == std::string::npos ? at : item.find(':', at);
        if (at != std::string::npos)
            g_autopress.push_back({item.substr(0, at), atof(item.substr(at + 1).c_str()),
                                   colon == std::string::npos ? 0.2 : atof(item.substr(colon + 1).c_str())});
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}
std::vector<int> g_virtual_pads;  // joystick device instance ids, by number - 1
void attach_virtual_pads() {  // they log their rumble (SSNE_VIRTUAL_PAD: one)
    const char* env = getenv("SSNE_VIRTUAL_PADS");
    int n = env ? std::clamp(atoi(env), 0, 4) : getenv("SSNE_VIRTUAL_PAD") ? 1 : 0;
    for (int i = 0; i < n; i++) {
        SDL_VirtualJoystickDesc d{};
        d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        d.naxes = SDL_CONTROLLER_AXIS_MAX;
        d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        std::string name = "Virtual pad " + std::to_string(i + 1);
        d.name = name.c_str();
        d.userdata = reinterpret_cast<void*>(intptr_t(i + 1));
        d.Rumble = [](void* u, Uint16 lo, Uint16 hi) -> int {
            static Uint16 last[5][2] = {};
            int k = int(reinterpret_cast<intptr_t>(u));
            if (lo != last[k][0] || hi != last[k][1])
                log_line("[dev] virtual pad " + std::to_string(k) + " rumble " + std::to_string(lo) + " " + std::to_string(hi));
            last[k][0] = lo; last[k][1] = hi;
            return 0;
        };
        int dev = SDL_JoystickAttachVirtualEx(&d);
        if (dev < 0) { log_line(std::string("[dev] virtual pad failed: ") + SDL_GetError()); continue; }
        SDL_JoystickOpen(dev);  // keeps it open for apply_virtual_autopress
        g_virtual_pads.push_back(SDL_JoystickGetDeviceInstanceID(dev));
    }
}
void apply_virtual_autopress() {
    if (g_virtual_pads.empty()) return;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_autopress_t0).count();
    for (size_t v = 0; v < g_virtual_pads.size(); v++) {
        SDL_Joystick* j = SDL_JoystickFromInstanceID(g_virtual_pads[v]);
        if (!j) continue;
        bool button[SDL_CONTROLLER_BUTTON_MAX] = {};
        int16_t axis[SDL_CONTROLLER_AXIS_MAX] = {};
        axis[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = axis[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = -32768;  // released (full range)
        std::string prefix = "p" + std::to_string(v + 1) + ":";
        bool detach = false;
        for (const AutoPress& a : g_autopress) {
            if (a.name.rfind(prefix, 0) != 0 || t < a.at || t >= a.at + a.dur) continue;
            std::string n = a.name.substr(prefix.size());
            if (n == "detach") { detach = true; continue; }  // unplugged
            char sign = n.empty() ? 0 : n.back();
            if (sign == '+' || sign == '-') {
                SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(n.substr(0, n.size() - 1).c_str());
                if (ax != SDL_CONTROLLER_AXIS_INVALID) axis[ax] = sign == '+' ? 32767 : -32768;
            } else {
                SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(n.c_str());
                if (b != SDL_CONTROLLER_BUTTON_INVALID) button[b] = true;
            }
        }
        if (detach) {
            for (int d = 0; d < SDL_NumJoysticks(); d++)
                if (SDL_JoystickGetDeviceInstanceID(d) == g_virtual_pads[v]) { SDL_JoystickClose(j); SDL_JoystickDetachVirtual(d); break; }
            g_virtual_pads[v] = -1;
            continue;
        }
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) SDL_JoystickSetVirtualButton(j, b, button[b]);
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) SDL_JoystickSetVirtualAxis(j, a, axis[a]);
    }
}

void apply_autopress(ssne::PadState& p) {
    if (g_autopress.empty()) return;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_autopress_t0).count();
    static const struct { const char* n; uint16_t b; } kNames[] = {
        {"A", ssne::PAD_A}, {"B", ssne::PAD_B}, {"X", ssne::PAD_X}, {"Y", ssne::PAD_Y}, {"START", ssne::PAD_START},
        {"Z", ssne::PAD_Z}, {"L", ssne::PAD_L}, {"R", ssne::PAD_R}, {"UP", ssne::PAD_UP}, {"DOWN", ssne::PAD_DOWN},
        {"LEFT", ssne::PAD_LEFT}, {"RIGHT", ssne::PAD_RIGHT}};
    for (const AutoPress& a : g_autopress) {
        if (t < a.at || t >= a.at + a.dur) continue;
        for (auto& k : kNames) if (a.name == k.n) p.buttons |= k.b;
        if (a.name == "SUP") p.stick_y = 230;
        if (a.name == "SDOWN") p.stick_y = 26;
        if (a.name == "SLEFT") p.stick_x = 26;
        if (a.name == "SRIGHT") p.stick_x = 230;
        if (a.name == "CLEFT") p.cstick_x = 26;
        if (a.name == "CRIGHT") p.cstick_x = 230;
        if (a.name == "R") p.trigger_r = 255;
        if (a.name.rfind("RAW", 0) == 0) {  // RAW<x>_<y>: main stick at raw bytes (128 = center)
            int x = 128, y = 128;
            sscanf(a.name.c_str() + 3, "%d_%d", &x, &y);
            p.stick_x = uint8_t(x); p.stick_y = uint8_t(y);
        }
    }
}

// Rumble: the game switches the GameCube controller's motor on and off (bursts of 0.2-0.7 s while taking damage,
// explosions...). Mirror that on the gamepad of the player whose port it is; SDL's rumble call takes a duration, so an active motor is refreshed
// before it runs out and stopped explicitly when the game stops it, the menu opens or rumble is turned off.
void update_rumble() {
    if(getenv("SSNE_TEST_HIDDEN"))return;
    static bool on[kPadSlots] = {};
    static uint32_t refreshed[kPadSlots] = {}, test_until = 0;
    uint32_t now = SDL_GetTicks();
    if (port::menu_take_rumble_test()) test_until = now + 400;
    const bool allowed = g_set.rumble && !port::menu_open() && !ssne::paused();
    for (int i = 0; i < kPadSlots; i++) {
        SDL_GameController* c = g_pads[i];
        if (!c) { on[i] = false; continue; }
        int player = network_game()?int(g_coop_local_player.load()):slot_player(i);
        bool want = (now < test_until) || (allowed && player >= 0 && ssne::pad_motor(player) == 1);
        if (want && (!on[i] || now - refreshed[i] > 500)) {
            float k = std::clamp(g_set.rumble_strength, 0.1f, 1.f);
            // Low-frequency (heavy) motor carries most of it, like the GameCube's single eccentric motor.
            SDL_GameControllerRumble(c, Uint16(0xFFFF * k), Uint16(0x9000 * k), 1000);
            on[i] = true;
            refreshed[i] = now;
        } else if (!want && on[i]) {
            SDL_GameControllerRumble(c, 0, 0, 0);
            on[i] = false;
        }
    }
}

// Mouse wheel bindings: every notch becomes one press of the bound button, held long enough for the game to see it
// (it reads the pads once per frame) with a short release in between, so fast scrolling still counts each notch.
bool g_focused = true;  // the game window has keyboard focus (pause in the background)

// On-screen button prompts: Xbox / PlayStation glyphs for the pad buttons bound to the GameCube controls (runtime
// repaints the game's button glyphs). GameCube A has two meanings with the Xbox layout: in menus and cutscenes the
// jump button selects (see update_input), in gameplay A is "use".
uint8_t prompt_glyph(const port::Bind& b, bool ps) {
    if (b.type == port::Bind::PAD_AXIS)
        return b.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? (ps ? ssne::PS_L2 : ssne::XB_LT)
             : b.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? (ps ? ssne::PS_R2 : ssne::XB_RT) : 0;
    if (b.type != port::Bind::PAD_BUTTON) return 0;
    switch (b.code) {
    case SDL_CONTROLLER_BUTTON_A: return ps ? ssne::PS_CROSS : ssne::XB_A;
    case SDL_CONTROLLER_BUTTON_B: return ps ? ssne::PS_CIRCLE : ssne::XB_B;
    case SDL_CONTROLLER_BUTTON_X: return ps ? ssne::PS_SQUARE : ssne::XB_X;
    case SDL_CONTROLLER_BUTTON_Y: return ps ? ssne::PS_TRIANGLE : ssne::XB_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ps ? ssne::PS_L1 : ssne::XB_LB;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ps ? ssne::PS_R1 : ssne::XB_RB;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ps ? ssne::PS_L3 : ssne::XB_LS;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ps ? ssne::PS_R3 : ssne::XB_RS;
    case SDL_CONTROLLER_BUTTON_BACK: return ps ? ssne::PS_SHARE : ssne::XB_VIEW;
    case SDL_CONTROLLER_BUTTON_START: return ps ? ssne::PS_OPTIONS : ssne::XB_MENU;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return ssne::GLYPH_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ssne::GLYPH_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ssne::GLYPH_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ssne::GLYPH_DPAD_RIGHT;
    }
    return 0;
}

// Keyboard / mouse glyph of a binding: the keycap of a key, or the mouse button / wheel (0 = none).
uint8_t kbm_glyph(const port::Bind& b) {
    switch (b.type) {
    case port::Bind::KEY: return ssne::prompt_key_glyph(SDL_GetScancodeName(SDL_Scancode(b.code)));
    case port::Bind::WHEEL: return b.code > 0 ? ssne::MS_WHEEL_UP : ssne::MS_WHEEL_DOWN;
    case port::Bind::MOUSE:
        switch (b.code) {
        case SDL_BUTTON_LEFT: return ssne::MS_LEFT;
        case SDL_BUTTON_RIGHT: return ssne::MS_RIGHT;
        case SDL_BUTTON_MIDDLE: return ssne::MS_MIDDLE;
        case SDL_BUTTON_X1: return ssne::MS_BACK;
        case SDL_BUTTON_X2: return ssne::MS_FORWARD;
        }
        return 0;
    default: return 0;
    }
}

bool g_input_pad = false;  // the last input came from the gamepad ("automatic" on-screen buttons follow it)

// Menus need keys for X and Y (Add / Delete Player in the profile screens): the mouse selects, goes back and scrolls
// there instead of its bindings. When neither has a key (their keys rebound to other controls), Insert and Delete
// press them in the menus, unless those keys are bound to something else; the menu prompts show them.
SDL_Scancode menu_fallback_key(int act) {
    SDL_Scancode key = act == port::ACT_X ? SDL_SCANCODE_INSERT : act == port::ACT_Y ? SDL_SCANCODE_DELETE : SDL_SCANCODE_UNKNOWN;
    if (key == SDL_SCANCODE_UNKNOWN) return key;
    for (const port::Bind& b : g_set.kb[act]) if (b.type == port::Bind::KEY) return SDL_SCANCODE_UNKNOWN;
    for (const auto& slots : g_set.kb)
        for (const port::Bind& b : slots) if (b == port::Bind::key(key)) return SDL_SCANCODE_UNKNOWN;
    return key;
}

void update_prompts() {
    uint8_t menu[7] = {}, control[7] = {};
    static const int kSlots[7] = {port::ACT_A, port::ACT_B, port::ACT_X, port::ACT_Y, port::ACT_Z, port::ACT_L, port::ACT_R};
    std::string style = g_set.prompts;
    if (style == "auto") {
        style = "keyboard";
        if (g_last_pad && g_input_pad) {
            SDL_GameControllerType t = SDL_GameControllerGetType(g_last_pad);
            style = (t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS5) ? "playstation" : "xbox";
        }
    }
    if (style == "keyboard") {
        // Gameplay: each control's first binding with a glyph. Menus, the pause screen and cutscenes only take keys
        // (the mouse clicks, goes back and scrolls there, see update_input): A is a jump key (they select there) and
        // B is Esc when it pauses (it goes back there).
        auto first = [](const port::Bind* b, bool keys_only) {
            for (int k = 0; k < port::Settings::kKbSlots; k++)
                if (uint8_t g = (keys_only && b[k].type != port::Bind::KEY) ? 0 : kbm_glyph(b[k])) return g;
            return uint8_t(0);
        };
        for (int s = 0; s < 7; s++) {
            control[s] = first(g_set.kb[kSlots[s]], false);
            menu[s] = first(g_set.kb[kSlots[s]], true);
            if (SDL_Scancode f = menu_fallback_key(kSlots[s])) menu[s] = kbm_glyph(port::Bind::key(f));
        }
        if (uint8_t g = first(g_set.kb[port::ACT_L], true)) menu[0] = g;
        for (const port::Bind& b : g_set.kb[port::ACT_START])
            if (b == port::Bind::key(SDL_SCANCODE_ESCAPE)) menu[1] = kbm_glyph(b);
    } else if (style != "gamecube") {
        bool ps = style == "playstation";
        for (int s = 0; s < 7; s++) menu[s] = control[s] = prompt_glyph(g_set.pad[kSlots[s]], ps);
        if (g_set.pad[port::ACT_L].type == port::Bind::PAD_BUTTON) menu[0] = prompt_glyph(g_set.pad[port::ACT_L], ps);
    }
    ssne::set_button_prompts(menu, control);
}

struct WheelPulse {
    int pending = 0;   // notches not yet turned into presses
    float t = 0;       // time left in the current phase
    int phase = 0;     // 0 idle, 1 pressed, 2 released gap
    bool step(float dt) {  // advances; true while the button is pressed
        if (phase != 0) t -= dt;
        if (phase == 1 && t <= 0) { phase = 2; t = 0.05f; }
        if (phase == 2 && t <= 0) phase = 0;
        if (phase == 0 && pending > 0) { pending--; phase = 1; t = 0.1f; }
        return phase == 1;
    }
};
WheelPulse g_wheel[2];  // [0] down, [1] up

// One player's controls, gathered from the devices that play it (keyboard / mouse and controllers add up).
using PlayerInput=port::PlayerInput;
void pad_input(SDL_GameController* c,PlayerInput& in,bool control,bool held_jump){port::sample_controller(g_set,c,in,control,held_jump);}
ssne::PadState to_pad(const PlayerInput& in){return port::map_player_input(g_set,in);}

bool g_test_ui_shot=false;
void apply_menu_mods(ssne::ModOptions options){if(!network_game() || g_coop_host)ssne::mod_options(options);}
void spawn_menu_mod(unsigned id,int count,float distance){if(!network_game() || g_coop_host)ssne::mod_spawn(id,count,distance);}
ssne::PadState scripted_pad(const ssne::PadState& live) {
    // Scripted inputs belong to the isolated test harness. Normal launches must
    // preserve SDL keyboard, mouse and controller input even with a stale script.
    if (!getenv("SSNE_TEST_HIDDEN")) return live;
    const char* script = getenv("SSNE_PAD_SCRIPT");
    std::string script_file_contents;
    if(const auto path=getenv("SSNE_PAD_SCRIPT_FILE")){
        std::ifstream file(std::filesystem::u8path(path),std::ios::binary);
        script_file_contents.assign(std::istreambuf_iterator<char>(file),{});
        script=script_file_contents.c_str();
    }
    if (!script) return live;
    ssne::PadState out;
    float now = SDL_GetTicks() / 1000.f;
    std::string spec(script);
    for (size_t pos = 0; pos < spec.size();) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char name[24]{}; float at = 0, duration = 0.2f;
        if (sscanf(item.c_str(), "%23[^@]@%f:%f", name, &at, &duration) >= 2 && now >= at && now < at + duration) {
            if(!strcmp(name,"UISHOT")){static std::vector<size_t> sent;if(std::find(sent.begin(),sent.end(),pos)==sent.end()){g_test_ui_shot=true;sent.push_back(pos);}}
            if(!strncmp(name,"MODTAB",6) && name[6]>='0' && name[6]<='4')port::mod_menu_select_tab(name[6]-'0');
            if(!strcmp(name,"MODON") || !strcmp(name,"MODOFF")){auto options=ssne::mod_current_options();options.god=options.ammo=options.weapons=options.no_enemies=options.rapid_fire=!strcmp(name,"MODON");apply_menu_mods(options);}
            if(!strcmp(name,"ESPON") || !strcmp(name,"ESPOFF")){auto options=ssne::mod_current_options();options.esp=options.bone_esp=!strcmp(name,"ESPON");ssne::mod_local_options(options);}
            if(!strcmp(name,"AIMON") || !strcmp(name,"AIMOFF")){auto options=ssne::mod_current_options();options.aimbot=!strcmp(name,"AIMON");options.aim_hold=false;options.aim_point=3;ssne::mod_local_options(options);static std::vector<size_t> traced;if(std::find(traced.begin(),traced.end(),pos)==traced.end()){traced.push_back(pos);const auto current=ssne::mod_current_options();log_line("[mod-input-test] action="+std::string(name)+" client="+std::to_string(g_coop_client)+" aimbot="+std::to_string(current.aimbot)+" point="+std::to_string(current.aim_point));}}
            if(!strcmp(name,"GODON")){auto options=ssne::mod_current_options();options.god=true;apply_menu_mods(options);}
            if(!strncmp(name,"WEAPON",6) && name[6]>='1' && name[6]<='9'){
                static std::vector<size_t> weapon_sent;
                if(std::find(weapon_sent.begin(),weapon_sent.end(),pos)==weapon_sent.end()){
                    g_coop_weapon=uint8_t(16+name[6]-'1');weapon_sent.push_back(pos);
                }
            }
            if (!strcmp(name,"QUIT") || !strcmp(name,"ENTER") || !strcmp(name,"SHOT") || !strcmp(name,"F6") || !strcmp(name,"F7") || !strcmp(name,"F8") || !strcmp(name,"SAVE") || !strcmp(name,"LOAD") || !strcmp(name,"F10") || !strcmp(name,"F12") || !strcmp(name,"MODS")) {
                static std::vector<size_t> sent;
                if (std::find(sent.begin(), sent.end(), pos) == sent.end()) {
                    SDL_Event event{}; event.type = SDL_KEYDOWN;
                    event.key.keysym.scancode = !strcmp(name,"ENTER") ? SDL_SCANCODE_RETURN : !strcmp(name,"SHOT") ? SDL_SCANCODE_PRINTSCREEN : !strcmp(name,"F6") ? SDL_SCANCODE_F6 : !strcmp(name,"F7") ? SDL_SCANCODE_F7 : !strcmp(name,"F10") ? SDL_SCANCODE_F10 : SDL_SCANCODE_F12;
                    if(!strcmp(name,"MODS"))event.key.keysym.scancode=SDL_SCANCODE_INSERT;
                    if(!strcmp(name,"F8"))event.key.keysym.scancode=SDL_SCANCODE_F8;
                    if(!strcmp(name,"SAVE"))event.key.keysym.scancode=SDL_SCANCODE_F5;
                    if(!strcmp(name,"LOAD"))event.key.keysym.scancode=SDL_SCANCODE_F9;
                    if(!strcmp(name,"QUIT"))event.type=SDL_QUIT;
                    if(!strcmp(name,"SHOT"))event.type=SDL_KEYUP; // Windows Print Screen path
                    SDL_PushEvent(&event); sent.push_back(pos);
                }
            }
            if (!strcmp(name,"KEY_ENTER") && getenv("SSNE_TEST_DEVICE_INPUT")) out.buttons |= ssne::PAD_Z;
            if (!strcmp(name,"KEY_TAB") && getenv("SSNE_TEST_DEVICE_INPUT")) out.buttons |= ssne::PAD_L;
            if (!strcmp(name,"A")) out.buttons |= ssne::PAD_A;
            if (!strcmp(name,"B")) out.buttons |= ssne::PAD_B;
            if (!strcmp(name,"X")) out.buttons |= ssne::PAD_X;
            if (!strcmp(name,"Y")) out.buttons |= ssne::PAD_Y;
            if (!strcmp(name,"Z")) out.buttons |= ssne::PAD_Z;
            if (!strcmp(name,"START")) out.buttons |= ssne::PAD_START;
            if (!strcmp(name,"UP")) out.buttons |= ssne::PAD_UP;
            if (!strcmp(name,"DOWN")) out.buttons |= ssne::PAD_DOWN;
            if (!strcmp(name,"LEFT")) out.buttons |= ssne::PAD_LEFT;
            if (!strcmp(name,"RIGHT")) out.buttons |= ssne::PAD_RIGHT;
            if (!strcmp(name,"SUP")) out.stick_y = 255;
            if (!strcmp(name,"SLEFT")) out.stick_x = 0;
            if (!strcmp(name,"SRIGHT")) out.stick_x = 255;
            if (!strcmp(name,"CLEFT")) out.cstick_x = 0;
            if (!strcmp(name,"CRIGHT")) out.cstick_x = 255;
            if (!strcmp(name,"CUP")) out.cstick_y = 255;
            if (!strcmp(name,"CDOWN")) out.cstick_y = 0;
            if (!strcmp(name,"FIRE")) { out.trigger_r = 255; out.buttons |= ssne::PAD_R; }
            if (!strcmp(name,"FIRE_SOFT")) out.trigger_r = 180;
            if (!strcmp(name,"FIRE_LEFT")) out.trigger_l = 180;
            if (!strcmp(name,"SDOWN")) out.stick_y = 0;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}
#include "native_mouse_input.inc"

void feed_local_pad(const ssne::PadState& live,int offline_port=0) {
    std::lock_guard session_lock(g_session_mutex);

    auto p=(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_TEST_DEVICE_INPUT"))?live:scripted_pad(live);
    p=port::coop::native_results_input(p,ssne::native_results_presenting());
    p.buttons|=take_native_menu_pad_pulse();
    if(port::menu_open() || port::mod_menu_open())p={};
    if(port::coop::native_frontend_input(p)){g_coop_yaw=g_coop_pitch=0;if(g_coop_host)g_coop_server->submit_host_input({});else if(g_coop_client)g_coop_peer->submit_input({});ssne::set_pad(0,{});if(offline_port)ssne::set_pad(offline_port,{});if(g_coop_local_player.load()>0)ssne::set_pad(g_coop_local_player.load(),{});return;}
    // Track edges across scenes: a confirmation held while leaving credits
    // must be released before it can start a new campaign in the lobby.
    static bool start_down=false;
    const bool down=(p.buttons&(ssne::PAD_A|ssne::PAD_START))!=0;
    const bool start_pressed=down && !start_down;
    start_down=down;
    if(g_coop_host)p.buttons=port::coop::filter_native_lobby_confirmation(p.buttons);
    if(port::coop::native_lobby_visible()){
        if(g_coop_host && start_pressed && g_coop_engine_ready.load(std::memory_order_acquire))g_coop_engine->request_lobby_campaign(false);
        g_coop_yaw=g_coop_pitch=0;
        auto choice=port::coop::native_lobby_choice(p);
        if(g_coop_host)g_coop_server->submit_host_input(choice);else if(g_coop_peer)g_coop_peer->submit_input(choice);
        return;
    }
    if(g_coop_host || g_coop_client) {
        if(g_coop_host && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_FIRE_READY") && ssne::native_test_fire_requested()){p.trigger_r=255;p.buttons|=ssne::PAD_R;}
        static bool skip_down=false;
        const auto vote=g_coop_host?g_coop_server->skip_vote():g_coop_peer->latest_world()->vote;
        // Hidden native tests inject an ordinary A pulse relative to the received
        // vote, avoiding machine-speed-dependent wall-clock script scheduling.
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_SKIP_DELAY_MS")) {
            static uint64_t seen_vote=0;static uint32_t seen_at=0;
            if(seen_vote!=vote.cutscene){seen_vote=vote.cutscene;seen_at=SDL_GetTicks();}
            const auto delay=unsigned(std::max(0,atoi(getenv("SSNE_COOP_TEST_SKIP_DELAY_MS"))));
            const auto elapsed=SDL_GetTicks()-seen_at;
            const auto first_vote=getenv("SSNE_TEST_SKIP_FIRST_VOTE")?1u:2u;
            if(vote.cutscene>=first_vote && elapsed>=delay && elapsed<delay+200)p.buttons|=ssne::PAD_A;
        }
        const bool down=(p.buttons&ssne::PAD_A)!=0;
        if(vote.cutscene && down && !skip_down) {
            if(g_coop_host){const auto result=g_coop_server->host_skip(++g_coop_command_sequence,vote.cutscene);if(getenv("SSNE_TEST_HIDDEN"))log_line("[coop-test-host-vote] scene="+std::to_string(vote.cutscene)+" result="+std::to_string(int(result)));}
            else g_coop_peer->vote_skip(vote.cutscene);
        }
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_AFK_VOTE_MASK")){
            static uint64_t seen=0;static uint32_t began=0;static bool sent=false;
            if(seen!=vote.cutscene){seen=vote.cutscene;began=SDL_GetTicks();sent=false;}
            const auto ns=g_coop_host?g_coop_server->status():g_coop_peer->status();
            const auto mask=std::strtoul(getenv("SSNE_COOP_TEST_AFK_VOTE_MASK"),nullptr,0);
            const auto delay=getenv("SSNE_COOP_TEST_AFK_VOTE_MS")?unsigned(std::max(0,atoi(getenv("SSNE_COOP_TEST_AFK_VOTE_MS")))):4500u;
            if(!sent && vote.cutscene>=2 && ns.player.slot<port::coop::max_players && (mask&(1u<<ns.player.slot)) && SDL_GetTicks()-began>=delay){
                sent=true;const bool afk=!getenv("SSNE_COOP_TEST_AFK_VOTE_RETURN");
                if(g_coop_host)g_coop_server->host_afk(++g_coop_command_sequence,afk);else g_coop_peer->set_afk(afk);
            }
        }
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TEST_AFK")) {
            const auto people=g_coop_host?g_coop_server->participants():g_coop_peer->latest_world()->players;
            const auto ns=g_coop_host?g_coop_server->status():g_coop_peer->status();
            static uint32_t ready_at=0;static bool sent=false,returned=false;
            const auto afk_mask=getenv("SSNE_COOP_TEST_AFK_MASK")?std::strtoul(getenv("SSNE_COOP_TEST_AFK_MASK"),nullptr,0):255u;
            if(ns.player.slot<port::coop::max_players && (afk_mask&(1u<<ns.player.slot)) && people[ns.player.slot].ready){
                if(!ready_at)ready_at=SDL_GetTicks();const auto elapsed=SDL_GetTicks()-ready_at;
                const auto delay=getenv("SSNE_COOP_TEST_AFK_DELAY_MS")?std::clamp(atoi(getenv("SSNE_COOP_TEST_AFK_DELAY_MS")),5000,120000):5000;
                if(!sent && elapsed>uint32_t(delay)){sent=true;if(g_coop_host)g_coop_server->host_afk(++g_coop_command_sequence,true);else g_coop_peer->set_afk(true);}
                const auto return_ms=getenv("SSNE_COOP_TEST_AFK_RETURN_MS")?std::clamp(atoi(getenv("SSNE_COOP_TEST_AFK_RETURN_MS")),10000,300000):35000;
                if(sent && !returned && elapsed>uint32_t(return_ms)){returned=true;if(g_coop_host)g_coop_server->host_afk(++g_coop_command_sequence,false);else g_coop_peer->set_afk(false);}
            }
        }
        skip_down=down;
        port::coop::ControlInput input;input.pad=p;
        input.weapon=std::exchange(g_coop_weapon,uint8_t(255));
        if(g_coop_engine_ready.load(std::memory_order_acquire))input.preferences=g_coop_engine->local_preferences();
        input.yaw=int32_t(std::round(std::clamp(g_coop_yaw,-2.,2.)*1000000.));
        input.pitch=int32_t(std::round(std::clamp(g_coop_pitch,-2.,2.)*1000000.));
        g_coop_yaw=g_coop_pitch=0;
        if(g_coop_host)g_coop_server->submit_host_input(input);
        else {if(ssne::local_pause_menu_open() || ssne::local_information_open()){input.pad={};input.yaw=input.pitch=0;}g_coop_peer->submit_input(input);ssne::set_pad(g_coop_local_player.load(),p);}
    } else if(g_net_host || g_net_client) { if(!(g_net_client && g_test_bot && ssne::netplay_world_ready())) g_net.submit_pad(p); }
    else ssne::set_pad(offline_port,p);
}


// Device conversion and scripted pads share this dispatcher. In particular,
// neutral releases must reach the frontend after the network role is cleared.
void route_local_pads(const std::array<ssne::PadState,4>& pads){
    std::lock_guard session_lock(g_session_mutex);
    const bool network=network_game();const int own=std::clamp(g_set.kbm_player-1,0,3);
    for(int port=0;port<4;++port){
        if(port==own && (network || g_native_frontend_enabled))feed_local_pad(pads[port],port);
        else if(!network)ssne::set_pad(port,pads[port]);
    }
}

void update_input(float dt) {
    if(getenv("SSNE_TEST_HIDDEN") && !getenv("SSNE_TEST_DEVICE_INPUT")){std::array<ssne::PadState,4> pads{};pads[std::clamp(g_set.kbm_player-1,0,3)]=port::menu_open()?ssne::PadState{}:scripted_pad({});route_local_pads(pads);return;}
    apply_virtual_autopress();
    update_ports();
    if (port::menu_open() || port::mod_menu_open()) {
        ssne::mod_aim_input(false,false);g_coop_yaw=g_coop_pitch=0;  // the game gets neutral controllers while the menu is open
        g_mouse_dx = g_mouse_dy = 0;
        g_wheel[0] = g_wheel[1] = WheelPulse{};
        route_local_pads({});
        return;
    }
    PlayerInput players[4];
    PlayerInput& kbm = players[std::clamp(g_set.kbm_player - 1, 0, 3)];
    float* act = kbm.act;
    // Private device snapshots exercise normal binding conversion and routing.
    // They never focus a window or change the desktop keyboard/mouse state.
    const bool test_devices=getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_TEST_DEVICE_INPUT");
    std::array<Uint8,SDL_NUM_SCANCODES> test_keys{};Uint32 test_mouse=0;
    if(test_devices){
        const auto device=scripted_pad({});
        auto key=[&](int action){for(const auto& b:g_set.kb[action])if(b.type==port::Bind::KEY && b.code>0 && b.code<SDL_NUM_SCANCODES)test_keys[b.code]=1;};
        if(device.buttons&ssne::PAD_START)test_keys[SDL_SCANCODE_ESCAPE]=1;
        if(device.buttons&ssne::PAD_Z)test_keys[SDL_SCANCODE_RETURN]=1;
        if(device.buttons&ssne::PAD_L)test_keys[SDL_SCANCODE_TAB]=1;
        if((device.buttons&ssne::PAD_UP) || device.stick_y>192)key(port::ACT_MOVE_UP);
        if((device.buttons&ssne::PAD_DOWN) || device.stick_y<64)key(port::ACT_MOVE_DOWN);
        if((device.buttons&ssne::PAD_LEFT) || device.stick_x<64)key(port::ACT_MOVE_LEFT);
        if((device.buttons&ssne::PAD_RIGHT) || device.stick_x>192)key(port::ACT_MOVE_RIGHT);
        if(device.buttons&ssne::PAD_X)key(port::ACT_X);
        if(device.buttons&ssne::PAD_Y)if(auto code=menu_fallback_key(port::ACT_Y))test_keys[code]=1;
        if((device.buttons&ssne::PAD_A) || device.trigger_r>128)test_mouse|=SDL_BUTTON(SDL_BUTTON_LEFT);
        if(device.buttons&ssne::PAD_B)test_mouse|=SDL_BUTTON(SDL_BUTTON_RIGHT);
        // Explicit raw keyboard snapshots for isolated device-route regressions.
        // These enter the normal bindings below without touching desktop input.
        if(const auto* path=std::getenv("SSNE_TEST_DEVICE_KEY_FILE")) {
            std::ifstream keys(std::filesystem::u8path(path));int code=0;
            for(unsigned n=0;n<SDL_NUM_SCANCODES && (keys>>code);++n)
                if(code>SDL_SCANCODE_UNKNOWN && code<SDL_NUM_SCANCODES)test_keys[code]=1;
        }
        static bool initialized=false;if(!initialized){g_mouse_captured=true;initialized=true;}
    }
    const Uint8* k = test_devices?test_keys.data():SDL_GetKeyboardState(nullptr);
    Uint32 presentation_buttons=g_presentation_mouse_buttons;
    if(g_presentation_pointer_active)for(unsigned i=0;i<2;++i)if(g_presentation_click[i].step(dt))presentation_buttons|=SDL_BUTTON(i?SDL_BUTTON_RIGHT:SDL_BUTTON_LEFT);
    Uint32 mb = test_devices?test_mouse:(g_presentation_pointer_active?presentation_buttons:(g_mouse_captured?SDL_GetMouseState(nullptr,nullptr):0));
    if(test_devices && getenv("SSNE_NATIVE_MOUSE_COMMAND") && g_presentation_pointer_active)mb|=presentation_buttons;
    if(g_native_pointer_active && !(test_devices && !getenv("SSNE_NATIVE_MOUSE_COMMAND")))mb=0;
    // Text events own keyboard editing; held letters must not also move native menu focus.
    bool kb_ok = !ImGui::GetIO().WantCaptureKeyboard && !port::coop::native_frontend_text_active();
    bool wheel_on[2] = {g_wheel[0].step(dt), g_wheel[1].step(dt)};
    if(g_native_pointer_active)wheel_on[0]=wheel_on[1]=false;
    // The game jumps with the (analog) L trigger and selects / skips with A. Outside player control (menus,
    // cutscenes, the pause screen) some inputs switch to menu duties:
    //  - keys and pad buttons bound to jump press A, so Space (or the Xbox layout's A button) jumps and selects,
    //  - Escape bound to Start (pause) presses B (back): it opens the pause menu in gameplay and closes it again,
    //  - the mouse works the menus instead of its bindings: left click selects (A), right click goes back (B) and
    //    the wheel moves the selection (stick up/down).
    // Those inputs, if still held when control changes, only count once released: firing or jumping as a cutscene
    // starts doesn't skip it, the Esc that paused doesn't close the pause menu, the click that starts a level
    // doesn't also fire.
    const bool control = ssne::player_control_active() && !ssne::native_results_presenting() && !g_presentation_pointer_active;
    static bool was_control = true;
    static Uint32 held_mouse = 0;
    static bool held_jump = false, held_esc = false, held_pad_jump[kPadSlots] = {};
    bool jump_key = false;
    for (const port::Bind& b : g_set.kb[port::ACT_L])
        if (b.type == port::Bind::KEY && kb_ok && b.code > 0 && b.code < SDL_NUM_SCANCODES && k[b.code]) jump_key = true;
    const bool esc = kb_ok && k[SDL_SCANCODE_ESCAPE];
    const port::Bind& pj = g_set.pad[port::ACT_L];
    bool pad_jump[kPadSlots] = {};
    for (int i = 0; i < kPadSlots; i++)
        pad_jump[i] = g_pads[i] && pj.type == port::Bind::PAD_BUTTON &&
                      SDL_GameControllerGetButton(g_pads[i], SDL_GameControllerButton(pj.code));
    if (control != was_control) {
        held_mouse = mb; held_jump = jump_key; held_esc = esc;
        for (int i = 0; i < kPadSlots; i++) held_pad_jump[i] = pad_jump[i];
    }
    was_control = control;
    held_mouse |= g_capture_click;
    g_capture_click = 0;
    held_mouse &= mb;
    held_jump &= jump_key;
    held_esc &= esc;
    for (int i = 0; i < kPadSlots; i++) held_pad_jump[i] &= pad_jump[i];
    for (int a = 0; a < port::ACT_COUNT; a++)
        for (const port::Bind& b : g_set.kb[a]) {
            if (b.type == port::Bind::KEY && kb_ok && b.code > 0 && b.code < SDL_NUM_SCANCODES && k[b.code]) {
                if (a == port::ACT_START && b.code == SDL_SCANCODE_ESCAPE) { if (!held_esc) act[control ? a : port::ACT_B] = 1; }
                else if (a == port::ACT_L) { if (!held_jump) act[control ? a : port::ACT_A] = 1; }
                else act[a] = 1;
            }
            if (b.type == port::Bind::MOUSE && control && (mb & ~held_mouse & SDL_BUTTON(b.code))) act[a] = 1;
            if (b.type == port::Bind::WHEEL && control && wheel_on[b.code > 0 ? 1 : 0]) act[a] = 1;
        }
    if(ssne::native_results_presenting() && kb_ok && (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]))act[port::ACT_A]=1;
    if (!control) {
        for (int a : {port::ACT_X, port::ACT_Y})
            if (SDL_Scancode f = menu_fallback_key(a); f && kb_ok && k[f]) act[a] = 1;
        Uint32 clicks = mb & ~held_mouse;
        if(g_native_pointer_active && !(test_devices && !getenv("SSNE_NATIVE_MOUSE_COMMAND")))clicks=0;
        if (clicks & SDL_BUTTON(SDL_BUTTON_LEFT)) act[port::ACT_A] = 1;
        if (clicks & SDL_BUTTON(SDL_BUTTON_RIGHT)) act[port::ACT_B] = 1;
        if (wheel_on[1]) kbm.ly += 1;
        if (wheel_on[0]) kbm.ly -= 1;
    }
    // Weapon wheel: holding X opens the game's weapon wheel after WeaponWheelDelay (0.25 s, base.cfg), which by
    // default pauses the game, and the wheel picks the weapon in the main stick's direction. A key / mouse hold of X
    // that starts in gameplay keeps X pressed until it is released (the pause would otherwise end player control and
    // hand the mouse buttons to the menus), and after the delay the mouse steers a virtual main stick instead of the
    // view: move towards a weapon, release the key / button to take it. Only on foot: in vehicles X does other things
    // and the main stick drives.
    static bool wheel_hold = false, x_was_down = false;
    static float wheel_t = 0, wheel_vx = 0, wheel_vy = 0;
    bool x_down = false, x_fresh = false;  // fresh: not a button that only captured the mouse / was held over
    for (const port::Bind& b : g_set.kb[port::ACT_X]) {
        if (b.type == port::Bind::KEY && kb_ok && b.code > 0 && b.code < SDL_NUM_SCANCODES && k[b.code]) x_down = x_fresh = true;
        if (b.type == port::Bind::MOUSE && (mb & SDL_BUTTON(b.code))) {
            x_down = true;
            if (!(held_mouse & SDL_BUTTON(b.code))) x_fresh = true;
        }
    }
    if (!x_down) wheel_hold = false;
    else if (!x_was_down && x_fresh && ssne::mouse_aim_active()) { wheel_hold = true; wheel_t = 0; wheel_vx = wheel_vy = 0; }
    x_was_down = x_down;
    if (wheel_hold) { act[port::ACT_X] = 1; wheel_t += dt; }
    const bool wheel_mouse = wheel_hold && wheel_t >= 0.25f && g_mouse_captured;
    if (wheel_mouse) {
        constexpr float kRadius = 100.f;  // mouse counts to full tilt; also how far back a change of mind takes
        wheel_vx += g_mouse_dx;
        wheel_vy -= g_mouse_dy;
        float len = std::hypot(wheel_vx, wheel_vy);
        if (len > kRadius) { wheel_vx *= kRadius / len; wheel_vy *= kRadius / len; len = kRadius; }
        if (len > kRadius * 0.35f) { kbm.lx += wheel_vx / len; kbm.ly += wheel_vy / len; }
        g_mouse_dx = g_mouse_dy = 0;
    }
    for (int i = 0; i < kPadSlots; i++)
        if (g_pads[i] && slot_player(i) >= 0) pad_input(g_pads[i], players[slot_player(i)], control, held_pad_jump[i]);
    kbm.ly += act[port::ACT_MOVE_UP] - act[port::ACT_MOVE_DOWN];
    // Vehicles steer with the C-stick (the main stick's X does nothing there): the strafe keys steer while driving.
    (ssne::player_in_vehicle() ? kbm.cx : kbm.lx) += act[port::ACT_MOVE_RIGHT] - act[port::ACT_MOVE_LEFT];
    kbm.cy += act[port::ACT_LOOK_UP] - act[port::ACT_LOOK_DOWN];
    kbm.cx += act[port::ACT_LOOK_RIGHT] - act[port::ACT_LOOK_LEFT];
    // Weapon groups 1-9 (number keys by default), on foot only: the runtime picks and switches the weapon.
    static bool weapon_was[9] = {};
    for (int i = 0; i < 9; i++) {
        bool on = act[port::ACT_WEAPON1 + i] >= 0.5f;
        if(on && !weapon_was[i] && ssne::player_control_active()){
            if(network_game())g_coop_weapon=uint8_t(16+i);else ssne::weapon_select_group(i);
        }
        weapon_was[i] = on;
    }
    if (g_mouse_captured && g_set.mouse_look && g_set.mouse_aim && ssne::player_control_active() && !ssne::player_in_vehicle()) {
        // Direct aim: mouse counts -> radians, applied to the player's view by the runtime (no C-stick).
        const double kr = 0.0012 * g_set.mouse_sens;
        if(network_game()){g_coop_yaw+=g_mouse_dx*kr;g_coop_pitch+=(g_set.mouse_invert_y?g_mouse_dy:-g_mouse_dy)*kr;}else ssne::mouse_aim_add(g_mouse_dx*kr,(g_set.mouse_invert_y?g_mouse_dy:-g_mouse_dy)*kr);
    } else if (g_mouse_captured && g_set.mouse_look) {
        // Mouse motion drives the C-stick (menus, or direct aim off); scaled per frame so speed doesn't depend on frame rate.
        float sens = 0.02f * g_set.mouse_sens;
        kbm.cx += std::clamp(g_mouse_dx * sens / std::max(dt * 60.f, 0.25f), -1.f, 1.f);
        float dy = std::clamp(g_mouse_dy * sens / std::max(dt * 60.f, 0.25f), -1.f, 1.f);
        kbm.cy += g_set.mouse_invert_y ? dy : -dy;
    }
    ssne::mod_aim_input((mb & SDL_BUTTON(SDL_BUTTON_RIGHT))!=0,g_mouse_captured && control && !port::mod_menu_open() && !port::menu_open() && !ImGui::GetIO().WantCaptureMouse);
    if(network_game()){double yaw=0,pitch=0;ssne::mod_take_netplay_aim(yaw,pitch);g_coop_yaw+=yaw;g_coop_pitch+=pitch;}
    g_mouse_dx = g_mouse_dy = 0;
    std::array<ssne::PadState,4> pads;
    for (int p = 0; p < 4; p++) {
        ssne::PadState ps = to_pad(players[p]);
        if (p == 0) apply_autopress(ps);
        // Flying (cheat): the game's debug flight goes down with L (jump) and up with R (fire); swapped, so jump
        // flies up and fire down (single player or the authoritative host: port 0).
        if (p == std::clamp(g_set.kbm_player-1,0,3) && ssne::cheat_flying()) {
            std::swap(ps.trigger_l, ps.trigger_r);
            uint16_t lr = ps.buttons & (ssne::PAD_L | ssne::PAD_R);
            ps.buttons &= uint16_t(~(ssne::PAD_L | ssne::PAD_R));
            if (lr & ssne::PAD_L) ps.buttons |= ssne::PAD_R;
            if (lr & ssne::PAD_R) ps.buttons |= ssne::PAD_L;
        }
        pads[p]=ps;
    }
    route_local_pads(pads);
}

// ---------------------------------------------------------------- audio
// The emulated AI DMA plays the game's mix in real time into a FIFO; the SDL callback drains it. The two clocks
// drift slightly, so the callback keeps the FIFO between ~40 and ~200 ms: it skips ahead when too much is queued
// and re-buffers a little after an underrun.
std::atomic<int> g_volume{100};
std::atomic<bool> g_muted{false};
SDL_AudioDeviceID g_audio_dev = 0;

void audio_callback(void*, Uint8* stream, int len) {
    static bool buffering = true;
    auto* out = reinterpret_cast<int16_t*>(stream);
    uint32_t frames = uint32_t(len / 4);
    uint32_t rate = ssne::audio_rate();
    uint32_t queued = ssne::audio_buffered();
    if (queued > rate * 200 / 1000) {  // too much latency: drop the oldest audio
        static std::vector<int16_t> sink;
        uint32_t drop = queued - rate * 80 / 1000;
        sink.resize(size_t(drop) * 2);
        ssne::audio_pull(sink.data(), drop);
        queued -= drop;
    }
    if (ssne::paused()) {  // game frozen by the settings menu: discard whatever the AI keeps producing
        static std::vector<int16_t> sink;
        sink.resize(size_t(queued) * 2);
        ssne::audio_pull(sink.data(), queued);
        memset(stream, 0, size_t(len));
        buffering = true;
        return;
    }
    if (buffering && queued < frames + rate * 40 / 1000) { memset(stream, 0, size_t(len)); return; }
    buffering = false;
    uint32_t got = ssne::audio_pull(out, frames);
    if (got < frames) buffering = true;
    int vol = g_muted ? 0 : g_volume.load();
    if (vol != 100)
        for (uint32_t i = 0; i < frames * 2; i++) out[i] = int16_t(std::clamp(int32_t(out[i]) * vol / 100, -32768, 32767));
}

void open_audio() {
    SDL_AudioSpec want{}, have{};
    want.freq = 32000;  // AX mixes at 32 kHz
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    g_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL converts to the device's format
    if (!g_audio_dev) { log_line(std::string("[audio] no audio device: ") + SDL_GetError()); return; }
    ssne::audio_set_sink_active(true);
    SDL_PauseAudioDevice(g_audio_dev, 0);
    log_line("[audio] output: " + std::string(SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?") +
             ", 32000 Hz stereo");
}

// ---------------------------------------------------------------- overlay (F1)
bool g_show_overlay = false;

// ---------------------------------------------------------------- window, screenshots

// Window position / size from settings.ini, if it is still on a connected display; else centered on the chosen
// (or first) display.
void initial_window_geometry(int& x, int& y, int& w, int& h) {
    w = g_set.win_w; h = g_set.win_h;
    int n = SDL_GetNumVideoDisplays();
    if (g_set.win_x != -1 || g_set.win_y != -1) {
        for (int d = 0; d < n; d++) {
            SDL_Rect b{};
            if (SDL_GetDisplayUsableBounds(d, &b) != 0) continue;
            // the title bar area must be on this display
            if (g_set.win_x + 40 >= b.x && g_set.win_x + 40 < b.x + b.w && g_set.win_y >= b.y - 8 && g_set.win_y < b.y + b.h - 40 &&
                (g_set.display < 0 || g_set.display == d)) {
                x = g_set.win_x; y = g_set.win_y;
                return;
            }
        }
    }
    int d = g_set.display >= 0 && g_set.display < n ? g_set.display : 0;
    x = int(SDL_WINDOWPOS_CENTERED_DISPLAY(d));
    y = int(SDL_WINDOWPOS_CENTERED_DISPLAY(d));
}

void remember_window(SDL_Window* win) {
    Uint32 f = SDL_GetWindowFlags(win);
    if (f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) return;
    g_set.win_max = (f & SDL_WINDOW_MAXIMIZED) != 0;
    if (g_set.win_max) return;  // keep the restored size and position
    SDL_GetWindowPosition(win, &g_set.win_x, &g_set.win_y);
    SDL_GetWindowSize(win, &g_set.win_w, &g_set.win_h);
}

void toast(const std::string& s);

// Screenshot of the game image (without the menus / info bar), PNG in Screenshots next to the game. The PNG is
// encoded on a worker thread so the game doesn't hitch.
void take_screenshot(const std::string& base_dir) {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (g_gpu) { if (!ssne::gpu_renderer_read_output(px, w, h)) px.clear(); }
    else if (!g_pixels.empty()) { px = g_pixels; w = g_tex_w; h = g_tex_h; }
    if (px.empty() || w <= 0 || h <= 0) { toast("No game image to capture yet"); return; }
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "SSNE-Native_%Y-%m-%d_%H-%M-%S", &lt);
    const auto test_shots=getenv("SSNE_TEST_HIDDEN")?getenv("SSNE_TEST_SCREENSHOT_DIR"):nullptr;
    std::string path=(test_shots?std::string(test_shots)+"/":base_dir+"Screenshots/")+name;
    static int same_second = 0;
    static std::string last_name;
    if (last_name == name) path += "_" + std::to_string(++same_second); else same_second = 0;
    last_name = name;
    path += ".png";
    toast("Screenshot: Screenshots/" + path.substr(path.rfind('/') + 1));
    std::thread([px = std::move(px), w, h, path] {
        if (!port::write_png(path, px.data(), w, h)) log_line("[port] could not write " + path);
    }).detach();
}

// ---------------------------------------------------------------- save states

std::string g_states_dir;
std::string g_toast;
uint32_t g_toast_until = 0;
int g_state_slot_busy = 0;  // slot of the request in flight (thumbnail after a save)

void toast(const std::string& s) {
    g_toast = s;
    g_toast_until = SDL_GetTicks() + 2500;
    log_line("[port] " + s);
}

std::string state_path(int slot) { return g_states_dir + "slot" + std::to_string(slot) + (g_coop_host || g_coop_client ? ".coop" : ".state"); }
std::string thumb_path(int slot) { return g_states_dir + "slot" + std::to_string(slot) + ".bmp"; }

bool prepare_coop_start_slot(int slot,std::string& error) {
    try{
        auto state=port::coop::savefile::read(std::filesystem::u8path(state_path(slot)),g_coop_save_identity);
        auto level=port::coop::savefile::startup_level(state);
        g_coop_start_slot=slot;g_coop_level=std::move(level);g_coop_start_save=std::move(state);
        log_line("[coop-startup-selected] slot="+std::to_string(slot)+" level="+g_coop_level);
        return true;
    }catch(const std::exception& e){error=e.what();log_line("[coop-startup-rejected] "+error);return false;}
}

std::string state_time(int slot, bool& exists) {
    uint64_t t = 0;
    exists = (g_coop_host || g_coop_client) ? port::coop::savefile::read_info(std::filesystem::u8path(state_path(slot)),t) : ssne::savestate_read_info(state_path(slot), t);
    if (!exists) return "";
    std::time_t tt = std::time_t(t);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return buf;
}

std::string state_slot_note(int slot) {
    bool exists;
    std::string when = state_time(slot, exists);
    return exists ? " (" + when + ")" : " (empty)";
}

void request_state(bool save, int slot) {
    std::lock_guard session_lock(g_session_mutex);
    if(g_coop_host || g_coop_client){
        if(!g_coop_host){toast("Only the host can save or load the cooperative session");return;}
        if(!g_coop_engine_ready.load(std::memory_order_acquire) || !have_game_image()){toast("Wait until the cooperative game is running");return;}
        std::string error;
        if(!g_coop_engine->request_save(save,slot,state_path(slot),error)){toast(error);return;}
        toast(save?"Saving cooperative session...":"Loading cooperative session...");return;
    }
    if(network_game()) {toast("Save states are unavailable in legacy netplay");return;}
    if (!g_ssne_has_game_code || !have_game_image()) { toast("Save states are available once the game is running"); return; }
    if (ssne::savestate_pending()) return;
    if (!save) {
        bool exists;
        state_time(slot, exists);
        if (!exists) { toast("Slot " + std::to_string(slot) + " is empty"); return; }
    }
    g_state_slot_busy = slot;
    ssne::savestate_request(save, state_path(slot));
}

// Thumbnail of the current game image (what the player sees), ~192 px wide, next to the state file.
void save_thumbnail(int slot) {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (g_gpu) { if (!ssne::gpu_renderer_read_output(px, w, h)) return; }
    else { px = g_pixels; w = g_tex_w; h = g_tex_h; }
    if (w <= 0 || h <= 0 || px.size() < size_t(w) * h) return;
    int tw = 192, th = std::max(1, h * tw / w);
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!sf) return;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            // box filter over the source pixels this thumbnail pixel covers
            int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw), y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
            uint32_t acc[3] = {}, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    uint32_t c = px[size_t(sy) * w + sx];
                    acc[0] += c & 255; acc[1] += (c >> 8) & 255; acc[2] += (c >> 16) & 255; n++;
                }
            uint32_t c = (acc[0] / n) | (acc[1] / n) << 8 | (acc[2] / n) << 16 | 0xFF000000u;
            memcpy((uint8_t*)sf->pixels + size_t(y) * sf->pitch + size_t(x) * 4, &c, 4);
        }
    SDL_SaveBMP(sf, thumb_path(slot).c_str());
    SDL_FreeSurface(sf);
}

void handle_state_result() {
    std::lock_guard session_lock(g_session_mutex);
    if(g_coop_host || g_coop_client){
        port::coop::NativeSession::SaveResult result;
        if(g_coop_engine_ready.load(std::memory_order_acquire) && g_coop_engine->take_save_result(result)){
            toast("Slot "+std::to_string(result.slot)+": "+result.message);
            if(result.save && result.ok)save_thumbnail(result.slot);
        }
        return;
    }
    ssne::SavestateResult res;
    if (!ssne::savestate_take_result(res)) return;
    toast("Slot " + std::to_string(g_state_slot_busy) + ": " + res.message);
    if (res.save && res.ok) save_thumbnail(g_state_slot_busy);
}

// Menu: slot dates and thumbnails (textures reloaded when the file changes).
void fill_state_slots(port::MenuInfo& mi) {
    std::lock_guard session_lock(g_session_mutex);
    static GLuint tex[5] = {};
    static std::filesystem::file_time_type stamp[5];
    mi.states_available = (!network_game() || (g_coop_host && g_coop_engine_ready.load(std::memory_order_acquire))) && g_ssne_has_game_code && have_game_image();
    mi.cheats_available = ssne::cheats_available();
    mi.states_dir = g_states_dir;
    static uint32_t last = 0;
    static port::MenuInfo::Slot cache[5];
    uint32_t now = SDL_GetTicks();
    if (now - last > 500 || last == 0) {
        last = now;
        for (int i = 0; i < 5; i++) {
            port::MenuInfo::Slot& sl = cache[i];
            sl.when = state_time(i + 1, sl.exists);
            std::error_code ec;
            auto p = std::filesystem::u8path(thumb_path(i + 1));
            auto t = std::filesystem::last_write_time(p, ec);
            if (ec || !sl.exists) { sl.tex = 0; continue; }
            if (tex[i] && t == stamp[i]) { sl.tex = tex[i]; continue; }
            SDL_Surface* bmp = SDL_LoadBMP(thumb_path(i + 1).c_str());
            SDL_Surface* rgba = bmp ? SDL_ConvertSurfaceFormat(bmp, SDL_PIXELFORMAT_ABGR8888, 0) : nullptr;
            if (bmp) SDL_FreeSurface(bmp);
            if (!rgba) { sl.tex = 0; continue; }
            if (!tex[i]) glGenTextures(1, &tex[i]);
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            sl.w = rgba->w; sl.h = rgba->h;
            SDL_FreeSurface(rgba);
            stamp[i] = t;
            sl.tex = tex[i];
        }
    }
    for (int i = 0; i < 5; i++) mi.slots[i] = cache[i];
}

// The frame rate shown is that of new game images on screen, not of frames the game produced: when the GPU falls
// behind, a pass renders two queued frames and shows only the last (the game is paced to 2 frames in flight), and the
// window is also presented for the menus without a new image (issue #10: 120 fps shown during visible slowdowns). The
// slowest frame is the longest time between two new images in the last second.
uint64_t g_shown_frames = 0;
uint32_t g_last_shown_ms = 0, g_slowest_ms = 0;
void note_shown_frame() {
    uint32_t now = SDL_GetTicks();
    if (g_last_shown_ms) g_slowest_ms = std::max(g_slowest_ms, now - g_last_shown_ms);
    g_last_shown_ms = now;
    g_shown_frames++;
}

void draw_bot_controls() {
    std::lock_guard session_lock(g_session_mutex);
    if(g_coop_host && g_coop_server){
        const auto people=g_coop_server->participants();
        ImGui::Separator();port::menu_ui::TextUnformatted("AI bots");static int skill=1;const char* skills[]={"Easy","Normal","Hard"};port::menu_ui::Combo("Bot difficulty",&skill,skills,3);
        if(port::menu_ui::Button("Add bot") && !g_coop_server->add_bot(unsigned(skill+1)))toast(port::menu_ui::text("No free player slot in the selected mode"));
        const auto mask=g_coop_server->bot_mask();
        for(unsigned slot=1;slot<people.size();++slot)if(mask&(1u<<slot)){ImGui::PushID(int(slot));port::menu_ui::Text("%s (%s)",people[slot].name.c_str(),port::menu_ui::text(skills[std::clamp(int(g_coop_server->bot_skill(slot))-1,0,2)]));ImGui::SameLine();if(port::menu_ui::SmallButton("Remove"))g_coop_server->remove_bot(people[slot].id);ImGui::PopID();}
        port::menu_ui::TextUnformatted("Bots occupy free player slots. Humans have priority.");
    }
}

void draw_session_overlay() {
    std::lock_guard session_lock(g_session_mutex);
    if(!g_coop_host && !g_coop_client)return;
    const auto status=g_coop_host?g_coop_server->status():g_coop_peer->status();
    const auto world=g_coop_client?g_coop_peer->latest_world():std::shared_ptr<const port::coop::WorldState>{};
    const auto vote=g_coop_host?g_coop_server->skip_vote():world?world->vote:port::coop::SkipVote{};
    if(vote.cutscene){
        const auto size=ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(size.x*.5f,size.y*.86f),ImGuiCond_Always,ImVec2(.5f,.5f));
        ImGui::SetNextWindowBgAlpha(.9f);
        ImGui::Begin("##coop-vote",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("Skip: %u / %u votes",vote.received,vote.required);
        ImGui::TextUnformatted(vote.skip_requested?"Skipping...":"Press select / jump to vote");
        ImGui::End();
    }
    if(!port::menu_open() && !g_show_overlay && status.connected)return;
    ImGui::SetNextWindowPos(ImVec2(8,52),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(.9f);
    ImGui::Begin("Multiplayer session",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%s | player %u | %u / %u connected",g_coop_host?"Host":"Client",status.player.slot+1,status.joined,status.capacity);
    ImGui::TextWrapped("%s",status.message.c_str());
    ImGui::Text("RTT: %u ms",status.ping_ms);
    if(g_coop_host)ImGui::Text("PIN: %s | TCP: %u",g_lan_pin.c_str(),status.port);
    const auto people=g_coop_host?g_coop_server->participants():world?world->players:std::array<port::coop::Participant,port::coop::max_players>{};
    const auto bot_mask=g_coop_host?g_coop_server->bot_mask():world && world->mission.contains("Session.Bots.Mask")?uint8_t(std::get<int64_t>(world->mission.at("Session.Bots.Mask"))):0;
    for(const auto& person:people)if(person.connected)ImGui::Text("%u  %s%s%s",person.id.slot+1,person.name.c_str(),person.ready?"":" (loading)",bot_mask&(1u<<person.id.slot)?" (Bot)":person.afk?" (AFK)":"");
    draw_bot_controls();
    if(status.player.slot<people.size() && people[status.player.slot].ready){
        bool afk=people[status.player.slot].afk;
        if(ImGui::Checkbox("AFK",&afk)){if(g_coop_host)g_coop_server->host_afk(++g_coop_command_sequence,afk);else g_coop_peer->set_afk(afk);}
    }
    ImGui::End();
}

void draw_overlay(const std::string& data_dir, float dt) {
    std::lock_guard session_lock(g_session_mutex);
    if(g_native_frontend_enabled && (port::coop::native_frontend_visible() || mem_read32(0x802DB7E8)==3) && !g_show_overlay && !port::menu_open())return;
    draw_session_overlay();
    static uint64_t last_shown = 0;
    static float fps_timer = 0, fps = 0;
    static uint32_t slowest = 0;
    auto rs = ssne::gx_render_stats();
    auto hw = ssne::hw_stats();
    auto st = ssne::scheduler_stats();
    fps_timer += dt;
    if (fps_timer >= 1.0f) {
        fps = float(g_shown_frames - last_shown) / fps_timer;
        last_shown = g_shown_frames;
        slowest = g_slowest_ms;
        g_slowest_ms = 0;
        fps_timer = 0;
    }

    if (g_set.top_bar == 1) {  // frame rate only
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fpsonly", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps%s%s", fps, ssne::cheats_active() ? "  [cheats]" : "", ssne::paused() ? "  [paused]" : "");
        ImGui::End();
    }
    if (g_set.top_bar == 2) {
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps (limit %s, slowest frame %u ms)   F12: settings & hotkeys   F1: log/stats   Shift+F1: smaller bar%s%s",
                    fps, fps_mode_name(ssne::frame_limit()).c_str(), slowest, ssne::cheats_active() ? "   [cheats]" : "",
                    ssne::paused() ? "   [paused]" : "");
        if (!g_gpu)  // make the (much slower) CPU renderer obvious: it explains low frame rates in bug reports
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1), "Software renderer (CPU): %s",
                               g_set.renderer == "software" ? "selected in F12 > Graphics" : "the GPU renderer could not start, see F1");
        ImGui::End();
    }
    int sh_done = 0, sh_total = 0;  // issue #18: the first start's lower frame rate explained while shaders compile
    if (g_gpu && g_set.shader_notice && have_game_image() && ssne::gpu_renderer_shader_progress(sh_done, sh_total)) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 12, vp->Pos.y + vp->Size.y - 12), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##shaders", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text("Preparing shaders  %d / %d", sh_done, sh_total);
        ImGui::TextDisabled("The frame rate can dip until this is done");
        ImGui::ProgressBar(float(sh_done) / float(std::max(1, sh_total)), ImVec2(-FLT_MIN, 6), "");
        ImGui::End();
    }
    if (SDL_GetTicks() < g_toast_until) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 56), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(g_toast.c_str());
        ImGui::End();
    }
    if (!have_game_image() || g_show_overlay) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 20, vp->Pos.y + 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("SSNE-Native - development build");
        {
            std::lock_guard<std::mutex> lk(g_log_mutex);
            ImGui::TextWrapped("%s", g_status.c_str());
        }
        ImGui::Text("Frames %llu  |  triangles %llu  |  textures %llu  |  disc reads %llu (%llu KB)  |  interrupts %llu, threads %d",
                    (unsigned long long)rs.display_copies, (unsigned long long)rs.triangles, (unsigned long long)rs.textures_decoded,
                    (unsigned long long)hw.disc_reads, (unsigned long long)(hw.disc_bytes / 1024), (unsigned long long)st.interrupts, st.threads);
        ImGui::Text("Data: %s   Recompiled functions: %zu   Controller: %s", data_dir.c_str(), g_ssne_func_count,
                    g_last_pad ? SDL_GameControllerName(g_last_pad) : "keyboard/mouse");
        {
            auto au = ssne::audio_stats();
            ImGui::Text("Audio: %s  volume %d%% (F3 mute, F4/F5 -/+)  |  %u voices  |  %u ms queued  |  %llu underruns",
                        g_audio_dev ? (g_muted ? "muted" : "on") : "no device", g_volume.load(), au.active_voices,
                        ssne::audio_buffered() * 1000 / std::max(1u, ssne::audio_rate()), (unsigned long long)au.underruns);
        }
        if (g_gpu) {
            auto gs = ssne::gpu_renderer_stats();
            auto o = ssne::gpu_renderer_output();
            ImGui::Text("Renderer: GPU %dx (%dx%d, F6/F7 to change) | %s | %.1f ms/frame, %llu draws, %llu shaders",
                        ssne::gpu_renderer_scale(), o.w, o.h, ssne::gpu_renderer_info().c_str(), gs.frame_ms,
                        (unsigned long long)gs.draws, (unsigned long long)gs.programs);
        } else {
            ImGui::Text("Renderer: software (CPU)");
        }
        {
            std::string mc = ssne::exi_memcard_path();
            ImGui::Text("Memory card A: %s", mc.empty() ? "none" : mc.c_str());
        }
        ImGui::Separator();
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lk(g_log_mutex);
            ImGuiListClipper clip;
            clip.Begin(int(g_log.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(g_log[size_t(i)].c_str());
            if (g_scroll_to_bottom) { ImGui::SetScrollHereY(1.0f); g_scroll_to_bottom = false; }
        }
        ImGui::EndChild();
        ImGui::End();
    }
}

// Applies g_set to the running game/renderer/window. `win` may be null before the window exists.
// 2-player split screen: automatic is left / right in widescreen (two tall halves instead of two flat strips).
void apply_split(SDL_Window* win) {
    int mode = g_set.split_two == 0 ? (aspect_factor(win) > 1.2f ? 1 : 0) : g_set.split_two - 1;
    ssne::set_split_two(mode);
}

void apply_settings(SDL_Window* win) {
    if(getenv("SSNE_TEST_HIDDEN")){g_set.fullscreen=false;g_set.display=-1;}
    {
        ssne::Cheats c;
        // Host party-wide switches have one owner, shared with the Insert menu.
        c.invulnerable = !network_game() && g_set.cheat_invulnerable;
        c.infinite_ammo = !network_game() && g_set.cheat_infinite_ammo;
        c.all_weapons = !network_game() && g_set.cheat_all_weapons;
        c.authoritative_host = g_coop_host;
        c.unlock_all = g_set.cheat_unlock_all;
        c.speed = g_set.cheat_speed / 100.0f;
        c.fly = g_set.cheat_fly;
        c.jump = g_set.cheat_jump / 100.0f;
        c.damage = g_set.cheat_damage / 100.0f;
        ssne::set_cheats(c);
    }
    ssne::set_widescreen(aspect_factor(win));
    apply_split(win);
    update_ports();
    ssne::set_hud_edges(g_set.hud_edges);
    ssne::gpu_renderer_set_side_fill(g_set.side_fill);
    {
        const std::string& aa = g_set.antialiasing;
        int msaa = aa == "msaa2" ? 2 : aa == "msaa4" ? 4 : aa == "msaa8" ? 8 : 1;
        ssne::gpu_renderer_set_antialiasing(msaa, aa == "fxaa", aa != "off");
    }
    ssne::gpu_renderer_set_anisotropy(g_set.anisotropy);
    ssne::set_frame_limit(g_set.fps_limit);
    ssne::set_fov(float(g_set.fov));
    ssne::set_skip_intros(g_set.skip_intros);
    g_volume = g_set.volume;
    g_muted = g_set.muted;
    ssne::audio_set_hq(g_set.audio_hq);
    ssne::audio_set_mix(g_set.music_volume, g_set.sfx_volume);
    if (win) {
        SDL_GL_SetSwapInterval(g_set.vsync ? 1 : 0);
        bool fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
        // Monitor choice: fullscreen goes to the display the window is on, so move the window there first.
        static int applied_display = -2;
        int want = g_set.display < SDL_GetNumVideoDisplays() ? g_set.display : -1;
        bool display_changed = want != applied_display;
        applied_display = want;
        if (want >= 0 && SDL_GetWindowDisplayIndex(win) != want && (display_changed || g_set.fullscreen != fs)) {
            if (fs) { SDL_SetWindowFullscreen(win, 0); fs = false; }
            SDL_SetWindowPosition(win, int(SDL_WINDOWPOS_CENTERED_DISPLAY(want)), int(SDL_WINDOWPOS_CENTERED_DISPLAY(want)));
        }
        if (fs != g_set.fullscreen) SDL_SetWindowFullscreen(win, g_set.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (g_gpu && g_set.scale > 0 && (ssne::gpu_renderer_native() || ssne::gpu_renderer_scale() != g_set.scale))
            ssne::gpu_renderer_set_scale(g_set.scale);
    }
}

void save_now() {
    if (!port::save_settings(g_set, g_settings_path)) log_line("[port] could not save " + g_settings_path);
}

#include "coop/native_runtime.inc"

} // namespace

int main(int argc, char** argv) {
    std::string data_dir;
    bool headless = false;
    std::string lan_address,coop_player_name;
    unsigned player_skin=0,enemy_multiplier=1,ammo_capacity=0,ammo_pickup_multiplier=1;
    uint16_t g_lan_port=27341;
    std::string memcard;  // default: saves/MemoryCardA.<USA|EUR>.raw next to the executable (by the disc's region)
    std::string base_dir;
    {
        char* base = SDL_GetBasePath();
        base_dir = base ? base : "";
        SDL_free(base);
    }
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        if(a=="--player-skin"){
            if(i+1>=argc){fprintf(stderr,"Missing player skin.\n");return 1;}
            char* end=nullptr;const auto value=std::strtol(argv[++i],&end,10);
            if(!argv[i][0] || !end || *end || value<0 || value>10){fprintf(stderr,"Player skin must be 0..10.\n");return 1;}player_skin=unsigned(value);
        }
        else if(a=="--ammo-capacity"){
            if(i+1>=argc){fprintf(stderr,"Missing ammo capacity.\n");return 1;}
            const std::string value=argv[++i];
            if(value=="normal")ammo_capacity=0;else if(value=="double")ammo_capacity=1;else if(value=="unlimited")ammo_capacity=2;else if(value=="1")ammo_capacity=0;else if(value=="2")ammo_capacity=1;else if(value=="3" || value=="4" || value=="5")ammo_capacity=unsigned(value[0]-'0');else{fprintf(stderr,"Ammo capacity must be normal, double, 1..5 or unlimited.\n");return 1;}
        }
        else if(a=="--ammo-pickup-multiplier"){
            if(i+1>=argc || (std::string(argv[i+1]).size()!=1 || argv[i+1][0]<'1' || argv[i+1][0]>'5')){fprintf(stderr,"Ammo pickup multiplier must be 1..5.\n");return 1;}
            ammo_pickup_multiplier=unsigned(argv[++i][0]-'0');
        }
        else if(a=="--enemy-multiplier"){
            if(i+1>=argc || (std::string(argv[i+1]).size()!=1 || argv[i+1][0]<'1' || argv[i+1][0]>'5')){fprintf(stderr,"Enemy multiplier must be 1..5.\n");return 1;}
            enemy_multiplier=unsigned(argv[++i][0]-'0');
        }
        else if(a=="--coop-host")g_coop_host=true;
        else if(a=="--coop-join" && i+1<argc){g_coop_client=true;lan_address=argv[++i];}
        else if(a=="--player-name" && i+1<argc){
            coop_player_name=argv[++i];
            if(coop_player_name.empty() || coop_player_name.size()>64 || coop_player_name.find_first_not_of(' ')==std::string::npos ||
               std::any_of(coop_player_name.begin(),coop_player_name.end(),[](unsigned char c){return c<32 || c==127;})){
                fprintf(stderr,"Player name must contain1..64UTF8bytes without control characters.\n");return 1;
            }
        }
        else if(a=="--coop-capacity" && i+1<argc)g_coop_capacity=std::clamp(atoi(argv[++i]),1,8);
        else if(a=="--coop-level" && i+1<argc){g_coop_level=argv[++i];g_coop_start_explicit=true;}
        else if(a=="--coop-load-slot" && i+1<argc){
            char* end=nullptr;const auto n=std::strtol(argv[++i],&end,10);
            if(!end || *end || n<1 || n>5){fprintf(stderr,"Cooperative save slot must be 1..5.\n");return 1;}
            g_coop_start_slot=int(n);g_coop_start_explicit=true;
        }
        else if(a=="--net-host") g_net_host=true;
        else if(a=="--net-join" && i+1<argc) {g_net_client=true;lan_address=argv[++i];}
        else if(a=="--lan-pin" && i+1<argc) g_lan_pin=argv[++i];
        else if(a=="--lan-port" && i+1<argc) g_lan_port=uint16_t(atoi(argv[++i]));
        else if(a=="--net-players" && i+1<argc) g_net_players=std::clamp(atoi(argv[++i]),2,8);
        else if(a=="--test-bot") g_test_bot=true;
        else if(a=="--bots" && i+1<argc)g_initial_bots=std::clamp(std::atoi(argv[++i]),0,7);
        else if(a=="--bot-skill" && i+1<argc)g_bot_skill=std::clamp(std::atoi(argv[++i]),1,3);
    }
    if(enemy_multiplier!=1 && !g_coop_host){fprintf(stderr,"Only a cooperative host can multiply enemies.\n");return 1;}
    if((ammo_capacity || ammo_pickup_multiplier!=1) && !g_coop_host){fprintf(stderr,"Only a cooperative host can choose ammunition rules.\n");return 1;}
    ssne::set_ammo_rules(g_coop_host?ammo_capacity:0,g_coop_host?ammo_pickup_multiplier:1);
    ssne::set_enemy_multiplier(g_coop_host?enemy_multiplier:1);
    if((g_net_host || g_coop_host) && g_lan_pin.empty()) g_lan_pin=std::to_string(100000+(SDL_GetTicks64()+std::chrono::steady_clock::now().time_since_epoch().count())%900000);
    g_settings_path = base_dir + ((network_game())?"settings-netplay.ini":"settings.ini");

    bool have_file = port::load_settings(g_set, g_settings_path);
    if(!have_file && !network_game())have_file=port::load_settings(g_set,base_dir+"settings-netplay.ini");
    // Freeze the native language request before the guest thread starts.
    port::coop::configure_native_startup_language(g_set.game_language);
    // Command-line options override settings.ini for this run (and are saved if the menu changes anything).
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--headless") headless = true;
        else if (a == "--fullscreen") g_set.fullscreen = true;
        else if (a == "--volume" && i + 1 < argc) g_set.volume = std::clamp(atoi(argv[++i]), 0, 200);
        else if (a == "--mute") g_set.muted = true;
        else if (a == "--memcard" && i + 1 < argc) memcard = argv[++i];
        else if (a == "--no-memcard") memcard = "-";
        else if (a == "--renderer" && i + 1 < argc) g_set.renderer = argv[++i];
        else if (a == "--aspect" && i + 1 < argc) {
            std::string v = argv[++i];
            for (int k = 0; k < kAspectCount; k++) if (v == kAspects[k].name) g_set.aspect = k;
        }
        else if (a == "--scale" && i + 1 < argc) {  // 0 = automatic, "native" (or -1) = native resolution
            std::string v = argv[++i];
            g_set.scale = v == "native" ? -1 : std::clamp(atoi(v.c_str()), -1, 8);
        }
        else if (a == "--fov" && i + 1 < argc) g_set.fov = std::clamp(atoi(argv[++i]), 45, 110);
        else if (a == "--fps" && i + 1 < argc) {
            std::string v = argv[++i];
            g_set.fps_limit = v == "unlimited" ? 0 : std::max(0, atoi(v.c_str()));
            if (g_set.fps_limit != 0) g_set.fps_limit = std::max(g_set.fps_limit, port::kMinFpsLimit);
        }
    }
    data_dir=port::startup::data_directory(base_dir,data_dir);
    {std::ifstream boot(std::filesystem::u8path(data_dir+"/sys/boot.bin"),std::ios::binary);char id[6]{};boot.read(id,6);g_native_frontend_enabled=boot && std::string(id,6)=="G3BP9G";}
    if(g_native_frontend_enabled)ssne::native_party_extended_memory(true);
    if(g_net_host || g_net_client){fprintf(stderr,"Use --coop-host or --coop-join with this integration build.\n");return 1;}
    ssne::set_fatal_handler(throw_fatal);
    if (memcard.empty()) {
        // One card per region, like Dolphin (a PAL save isn't a USA save): the disc ID's 4th letter is the region.
        std::string region = "USA";
        if (FILE* bf = fopen((data_dir + "/sys/boot.bin").c_str(), "rb")) {
            char id[4] = {};
            if (fread(id, 1, 4, bf) == 4 && (id[3] == 'P' || id[3] == 'D' || id[3] == 'F')) region = "EUR";
            else if (id[3] == 'J') region = "JAP";
            fclose(bf);
        }
        const auto existing=base_dir+"saves/Host-MemoryCardA."+region+".raw";
        memcard = g_native_frontend_enabled && std::filesystem::exists(std::filesystem::u8path(existing))?existing:base_dir + "saves/MemoryCardA." + region + ".raw";
    }
    if(g_coop_client){if(!ssne::exi_configure_volatile_memcard(memcard=="-"?std::string():memcard))return 1;}
    else ssne::exi_configure_memcard(memcard=="-"?std::string():memcard);
    {   // save states: states/<disc ID>/slotN.state (+ .bmp thumbnail)
        std::string id = "unknown";
        if (FILE* bf = fopen((data_dir + "/sys/boot.bin").c_str(), "rb")) {
            char b[7] = {};
            if (fread(b, 1, 6, bf) == 6) id = b;
            fclose(bf);
        }
        g_states_dir = base_dir + "states/" + id + "/";
        if(network_game())g_states_dir+="coop/";
    }
    if(g_coop_host || g_coop_client) {
        if(g_net_host || g_net_client || (g_coop_host && g_coop_client)){fprintf(stderr,"Select exactly one network role.\n");return 1;}
        const auto fingerprint=port::netplay::content_hash(data_dir+"/sys/main.dol");
        if(!g_ssne_has_game_code || !fingerprint){fprintf(stderr,"Coop requires local game code and data.\n");return 1;}
        if(g_coop_host){
            try{g_coop_save_identity=port::coop::savefile::identify(std::filesystem::u8path(data_dir));}
            catch(const std::exception& e){fprintf(stderr,"Cannot initialize cooperative saves: %s\n",e.what());return 1;}
            std::ifstream boot(std::filesystem::u8path(data_dir+"/sys/boot.bin"),std::ios::binary);
            unsigned char id[4]{};boot.read(reinterpret_cast<char*>(id),4);
            if(!boot){fprintf(stderr,"Cannot read cooperative game identity.\n");return 1;}
            for(auto c:id)g_coop_save_identity.game=(g_coop_save_identity.game<<8)|c;
            g_coop_save_identity.ram=0x04000000;g_coop_save_identity.capacity=g_coop_capacity;
            if(g_coop_start_slot){std::string error;if(!prepare_coop_start_slot(g_coop_start_slot,error)){fprintf(stderr,"Cannot start saved cooperative session: %s\n",error.c_str());return 1;}}
            g_coop_start_menu=false;
            if(!g_coop_start_explicit && !headless)g_coop_level="lobby";
        }
        else if(g_coop_start_slot){fprintf(stderr,"Only a host can select a cooperative save.\n");return 1;}
        bool started=false;
        if(g_coop_host){g_coop_server=std::make_unique<port::coop::AuthoritativeServer>(g_coop_capacity);started=g_coop_server->start(g_lan_port,g_lan_pin,fingerprint,coop_player_name.empty()?"Host":coop_player_name,player_skin);if(started)for(unsigned i=0;i<g_initial_bots;++i)g_coop_server->add_bot(g_bot_skill);g_lan_addresses=port::netplay::local_ipv4_addresses();}
        else {g_coop_peer=std::make_unique<port::coop::AuthoritativeClient>();started=g_coop_peer->start(lan_address,g_lan_port,g_lan_pin,fingerprint,coop_player_name.empty()?"Player":coop_player_name,player_skin);}
        if(!started){fprintf(stderr,"Cannot start authoritative coop connection.\n");return 1;}
        ssne::set_authoritative_gameplay(true);ssne::gpu_set_nonblocking_frames(true);ssne::mouse_aim_set_multiplayer(true);
        if(g_coop_host && getenv("SSNE_TEST_HIDDEN")) {
            if(const char* flags=getenv("SSNE_NETPLAY_MOD_FLAGS"))ssne::mod_shared_options(uint32_t(atoi(flags)),3.f);
        }
        set_status("Development coop: native engine integration in progress");
    }
    apply_settings(nullptr);
#ifdef _WIN32
    timeBeginPeriod(1);  // 1 ms sleeps for the audio/VI/alarm timers (default is ~15.6 ms)
#endif
    if(g_coop_host || g_coop_client){const auto path=getenv("SSNE_PERF_LOG");const std::string output=path?path:g_coop_host?"performance-host.jsonl":"performance-client.jsonl";
        if(!g_performance.open(output,g_coop_host?"host":"client"))fprintf(stderr,"Performance log could not be opened: %s\n",output.c_str());}
    g_log_file = fopen(g_coop_host?"ssne-coop-host.log":g_coop_client?"ssne-coop-client.log":g_net_host?"ssne-net-host.log":g_net_client?"ssne-net-client.log":"ssne.log", "w");

    ssne::set_log_sink([](const char* line) { log_line(line); });
    log_line(have_file ? "[port] settings: " + g_settings_path : "[port] settings: defaults (" + g_settings_path + " will be created)");
    if (!have_file) save_now();

    if (headless) {
        guest_main(data_dir);
        return 0;
    }

#ifdef _WIN32
    // Windows display scaling: SDL works in scaled points and the window gets all its pixels. Unaware of it, the game
    // was rendered into a window of 1/scale the size and stretched (1280x720 on a 4K screen at 300%, issue #7).
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
#endif
    // Rumble on PlayStation controllers goes through SDL's HIDAPI drivers, which keep it off unless asked.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if (SDL_Init(SDL_INIT_VIDEO | (getenv("SSNE_TEST_HIDDEN")?0:SDL_INIT_GAMECONTROLLER)) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if(!getenv("SSNE_TEST_HIDDEN"))attach_virtual_pads();  // development (SSNE_VIRTUAL_PADS)
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)  // no audio device: play silently
        log_line(std::string("[audio] audio unavailable: ") + SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    int wx, wy, ww, wh;
    initial_window_geometry(wx, wy, ww, wh);
    const bool hidden_test=getenv("SSNE_TEST_HIDDEN")!=nullptr;
    if(hidden_test){g_set.fullscreen=false;g_set.win_max=false;}
    SDL_Window* win = SDL_CreateWindow("Serious Sam: Next Encounter Native", wx, wy, ww, wh,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (hidden_test?SDL_WINDOW_HIDDEN:0) |
                                           (g_set.win_max ? SDL_WINDOW_MAXIMIZED : 0));
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    bool core = gl != nullptr;
    if (!gl) {  // no GL 3.3 core: fall back to a basic context and the software renderer
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        gl = SDL_GL_CreateContext(win);
        std::string have = "none";
        if (gl) {
            const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            have = std::string(v ? v : "?") + " on " + (r ? r : "?");
        }
        log_line("[gpu] OpenGL 3.3 core not available (driver offers " + have + "); using the software renderer. "
                 "Updating the graphics driver usually fixes this.");
    }
    if (!gl) { fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError()); return 1; }
    if (core && g_set.renderer != "software") {
        std::string err;
        {
            std::string dir = base_dir + "shadercache";
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
            if (!ec) ssne::gpu_renderer_set_shader_cache(dir);
        }
        int start_scale = g_set.scale > 0 ? g_set.scale : fit_scale(win);
        if (ssne::gpu_renderer_init(SDL_GL_GetProcAddress, start_scale, err)) {
            g_gpu = true;
            g_auto.cur = start_scale;
            ssne::gx_use_gpu(true);
            if (g_set.scale < 0) ssne::gpu_renderer_set_native(native_height(win));
            log_line("[gpu] " + ssne::gpu_renderer_info() + ", internal resolution " +
                     (g_set.scale < 0 ? "native (" + std::to_string(native_height(win)) + " lines)"
                                      : std::to_string(start_scale) + "x" + (g_set.scale ? "" : " (automatic)")));
        } else {
            log_line("[gpu] GPU renderer unavailable (" + err + "); using the software renderer");
        }
    }
    ssne::gpu_renderer_set_player_views(network_game());
    if(hidden_test)log_line("[input] hidden test: real keyboard/mouse/controller sampling and mouse capture disabled");
    ssne::gx_set_threaded(g_set.gx_thread);
    ssne::gx_set_gpu_vertex(g_set.gpu_vertex);
    log_line(g_set.gx_thread && !getenv("SSNE_GX_SYNC") ? "[gx] graphics commands on their own thread"
                                                        : "[gx] graphics commands on the game's thread (gx_thread = 0)");
    apply_settings(win);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // the settings menu works with a controller
    ImGui::StyleColorsDark();
    update_ui_scale(win);
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init(core ? "#version 330 core" : "#version 130");

    port::initialize_crosshair_renderer(base_dir);
    port::update_crosshair_renderer(g_set);
    SDL_StartTextInput();
    open_audio();
    std::thread(watchdog).detach();
    ssne::spawn_guest_thread([data_dir] { guest_main(data_dir); });  // never returns (game loop)

    load_autopress();
    if (const char* t = getenv("SSNE_OPEN_MENU")) {  // development: open the menu on a tab at startup (screenshots)
        port::menu_set_open(true);
        port::menu_select_tab(atoi(t));
    }
    auto toggle_menu = [&] {
        port::mod_menu_set_open(false);
        port::menu_set_open(!port::menu_open());
        if(hidden_test)log_line(std::string("[settings-test] open=")+(port::menu_open()?"1":"0"));
        if (port::menu_open()) release_mouse();
        else save_now();
    };
    port::FrameRateSample performance_rates;
    auto sample_performance=[&](bool background){
        std::lock_guard session_lock(g_session_mutex);
        if(!g_performance.due())return;
        const auto now=std::chrono::steady_clock::now();const auto native=ssne::gx_render_stats();
        performance_rates.update(now,native.display_copies,g_presented_game_frames,g_game_updates.load(std::memory_order_relaxed));
        auto& d=g_performance_display;d.render_fps=performance_rates.renders;d.native_fps=performance_rates.ticks;d.updates=performance_rates.updates;
        d.local_ready=false;d.ready_players=0;
        if(g_coop_host || g_coop_client){
            const auto status=g_coop_host?g_coop_server->status():g_coop_peer->status();
            const auto people=g_coop_host?g_coop_server->participants():g_coop_peer->latest_world()->players;
            for(const auto& player:people)if(player.connected && player.ready)++d.ready_players;
            d.local_ready=status.player.slot<people.size() && people[status.player.slot].connected && people[status.player.slot].ready;
        }
        const auto gpu=ssne::gpu_renderer_stats();const auto output=ssne::gpu_renderer_output();
        d.gpu_ms=g_auto.gpu_ms;d.gpu_timer_valid=g_auto.gpu_ms>0;d.gpu_cpu_ms=gpu.frame_ms;d.pending=0;d.dropped=ssne::gpu_dropped_frames();d.draws=gpu.draws;d.copies=gpu.tex_copies;d.uploads=gpu.uploads;d.shader_programs=gpu.programs;
        d.width=output.w;d.height=output.h;d.scale=ssne::gpu_renderer_scale();d.limit=g_set.fps_limit;d.fov=g_set.fov;d.vsync=g_set.vsync;d.paused=ssne::paused();d.focused=(SDL_GetWindowFlags(win)&SDL_WINDOW_INPUT_FOCUS)!=0;d.minimized=(SDL_GetWindowFlags(win)&SDL_WINDOW_MINIMIZED)!=0;d.background=background;
        g_performance.sample(d);
    };

    bool quit = false;
    uint32_t last = SDL_GetTicks();
    bool crosshair_reload=false;
    double swap_ms = 100;  // how long the last SwapWindow blocked (vsync); ~0 when it doesn't wait
    while (!quit) {
        sync_native_mouse_mode();
        SDL_Event e;
        bool changed = false;  // settings changed by hotkeys/menu this frame
        while (SDL_PollEvent(&e)) {
            if (port::menu_handle_event(e, g_set, changed)) continue;  // rebinding in progress
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL) g_input_pad = false;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN || (e.type == SDL_CONTROLLERAXISMOTION && std::abs(e.caxis.value) > 16000)) {
                g_input_pad = true;
                int slot = pad_slot(e.type == SDL_CONTROLLERBUTTONDOWN ? e.cbutton.which : e.caxis.which);
                if (slot >= 0) g_last_pad = g_pads[slot];
            }
            ImGui_ImplSDL2_ProcessEvent(&e);
            if(handle_native_mouse_event(e))continue;
            switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYUP:
                // Print Screen: Windows only delivers its key-up.
                if (e.key.keysym.scancode == SDL_SCANCODE_PRINTSCREEN) take_screenshot(base_dir);
                break;
            case SDL_TEXTINPUT: port::coop::native_frontend_text_input(e.text.text);break;
            case SDL_KEYDOWN: {
                std::lock_guard session_lock(g_session_mutex);
                SDL_Scancode sc = e.key.keysym.scancode;
                if(port::coop::native_frontend_text_active() && sc!=SDL_SCANCODE_F12){
                    if((e.key.keysym.mod&KMOD_CTRL) && sc==SDL_SCANCODE_A){port::coop::native_frontend_edit_key(5);break;}
                    if((e.key.keysym.mod&KMOD_CTRL) && sc==SDL_SCANCODE_V){if(char* clipboard=SDL_GetClipboardText()){port::coop::native_frontend_text_input(clipboard);SDL_free(clipboard);}break;}
                    const unsigned edit_key=(sc==SDL_SCANCODE_RETURN || sc==SDL_SCANCODE_KP_ENTER)?1:sc==SDL_SCANCODE_ESCAPE?2:sc==SDL_SCANCODE_BACKSPACE?3:sc==SDL_SCANCODE_DELETE?4:0;
                    if(edit_key)port::coop::native_frontend_edit_key(edit_key);break;
                }
                if (sc == SDL_SCANCODE_F12 && (e.key.keysym.mod & KMOD_SHIFT)) { if (!e.key.repeat) take_screenshot(base_dir); break; }
                if (sc == SDL_SCANCODE_F12 && !e.key.repeat) { toggle_menu(); break; }
                if(sc==SDL_SCANCODE_INSERT && !e.key.repeat){port::mod_menu_set_open(!port::mod_menu_open());release_mouse();if(getenv("SSNE_TEST_HIDDEN"))log_line("[mod-menu-test] open="+std::to_string(port::mod_menu_open()));break;}
                if(sc==SDL_SCANCODE_ESCAPE && port::mod_menu_open()){port::mod_menu_set_open(false);break;}
                if (sc == SDL_SCANCODE_ESCAPE && port::menu_open()) { toggle_menu(); break; }
                if (sc == SDL_SCANCODE_F1 && !e.key.repeat) {
                    if (e.key.keysym.mod & KMOD_SHIFT) { g_set.top_bar = (g_set.top_bar + 2) % 3; changed = true; }
                    else g_show_overlay = !g_show_overlay;
                }
                if (sc == SDL_SCANCODE_F3) { g_set.muted = !g_set.muted; changed = true; }
                if (sc == SDL_SCANCODE_F10) {
                    int k = -1;
                    for (int j = 0; j < 4; j++) if (kFpsModes[j] == g_set.fps_limit) k = j;
                    g_set.fps_limit = kFpsModes[(k + 1) % 4];
                    changed = true;
                    log_line("[port] frame rate limit: " + fps_mode_name(g_set.fps_limit));
                }
                if (sc == SDL_SCANCODE_F8) {
                    g_set.aspect = (g_set.aspect + 1) % kAspectCount;
                    changed = true;
                    log_line(std::string("[port] aspect ratio: ") + kAspects[g_set.aspect].name);
                }
                if (sc == SDL_SCANCODE_F5 && !e.key.repeat) {
                    if (e.key.keysym.mod & KMOD_SHIFT) {
                        g_set.state_slot = g_set.state_slot % 5 + 1;
                        changed = true;
                        toast("Save state slot " + std::to_string(g_set.state_slot) + state_slot_note(g_set.state_slot));
                    } else request_state(true, g_set.state_slot);
                }
                if (sc == SDL_SCANCODE_F9 && !e.key.repeat && !(e.key.keysym.mod & KMOD_SHIFT)) request_state(false, g_set.state_slot);
                if (g_gpu && sc == SDL_SCANCODE_F9 && (e.key.keysym.mod & KMOD_SHIFT)) {
                    std::string dir = base_dir + "debug";
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
                    ssne::gpu_renderer_request_dump(dir);
                    log_line("[gpu] F9: capturing the next frame to " + dir);
                }
                // F6/F7 step from the resolution in use (leaving automatic resolution if it was on).
                if (g_gpu && sc == SDL_SCANCODE_F6) { g_set.scale = std::max(1, ssne::gpu_renderer_scale() - 1); changed = true; }
                if (g_gpu && sc == SDL_SCANCODE_F7) { g_set.scale = std::min(8, ssne::gpu_renderer_scale() + 1); changed = true; }
                if (sc == SDL_SCANCODE_F4) {
                    if (e.key.keysym.mod & KMOD_SHIFT) g_set.volume = std::min(200, g_set.volume + 10);
                    else g_set.volume = std::max(0, g_set.volume - 10);
                    changed = true;
                    toast("Volume " + std::to_string(g_set.volume) + "%");
                }
                if (sc == SDL_SCANCODE_F2) release_mouse();
                if(!port::menu_open() && port::coop::native_lobby_visible() && sc==SDL_SCANCODE_C){
                    if(!e.key.repeat && g_coop_host && g_coop_engine_ready.load(std::memory_order_acquire))g_coop_engine->request_lobby_campaign(true);
                    break;
                }
                if(!port::menu_open() && sc==SDL_SCANCODE_RETURN && !(e.key.keysym.mod & KMOD_ALT) && (g_coop_host || g_coop_client)){
                    const auto world=g_coop_client?g_coop_peer->latest_world():nullptr;
                    const auto vote=g_coop_host?g_coop_server->skip_vote():world?world->vote:port::coop::SkipVote{};
                    if(vote.cutscene){
                        if(!e.key.repeat){if(g_coop_host)g_coop_server->host_skip(++g_coop_command_sequence,vote.cutscene);else g_coop_peer->vote_skip(vote.cutscene);}
                        break;
                    }
                    if(port::coop::native_lobby_visible()){
                        if(!e.key.repeat && g_coop_host && g_coop_engine_ready.load(std::memory_order_acquire))g_coop_engine->request_lobby_campaign(false);
                        break;
                    }
                }
                if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    g_set.fullscreen = !g_set.fullscreen;
                    changed = true;
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (!hidden_test && !g_mouse_captured && !port::menu_open() && !port::mod_menu_open() && ssne::player_control_active() && !port::menu_mouse_wants_pointer() && g_set.mouse_look && !ImGui::GetIO().WantCaptureMouse &&
                    have_game_image()) {
                    g_mouse_captured = true;
                    g_capture_click = SDL_BUTTON(e.button.button);
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                }
                break;
            case SDL_MOUSEMOTION:
                if (g_mouse_captured) { g_mouse_dx += float(e.motion.xrel); g_mouse_dy += float(e.motion.yrel); }
                break;
            case SDL_MOUSEWHEEL:
                if (!port::menu_open() && !port::mod_menu_open() && e.wheel.y != 0) {
                    int dy = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                    WheelPulse& w = g_wheel[dy > 0 ? 1 : 0];
                    w.pending = std::min(w.pending + std::abs(dy), 4);  // don't queue a long burst
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) toggle_menu();
                break;
            case SDL_CONTROLLERDEVICEADDED:  // which: device index
                open_pad(e.cdevice.which);
                g_input_pad = g_last_pad != nullptr;
                break;
            case SDL_CONTROLLERDEVICEREMOVED:  // which: instance id
                close_pad(e.cdevice.which);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { cancel_native_mouse();release_mouse();g_focused=false; }
                if (e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_MAXIMIZED || e.window.event == SDL_WINDOWEVENT_RESTORED)
                    remember_window(win);
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {  // automatic aspect
                    cancel_native_mouse();g_pointer_image={};++g_pointer_view_generation;
                    ssne::set_widescreen(aspect_factor(win));
                    apply_split(win);
                }
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_focused = true;
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {cancel_native_mouse();release_mouse();g_focused=false;}
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
                break;
            default: break;
            }
        }
        uint32_t now = SDL_GetTicks();
        float dt = float(now - last) / 1000.0f;
        last = now;
        sync_native_mouse_mode();
        native_mouse_test_command();
        if(g_crosshair_test_settings_request>=0) {
            const bool wanted=g_crosshair_test_settings_request!=0;g_crosshair_test_settings_request=-1;
            if(wanted!=port::menu_open())toggle_menu();
            if(wanted)port::menu_select_tab(0);
        }
        update_input(dt);
        write_native_mouse_input_state();
        {   // development, for automated runs: SSNE_LOAD_STATE_AT=T loads the save state slot (state_slot) at T seconds,
            // SSNE_AUTOSHOT=T1,T2,... saves screenshots (Screenshots/) at those times
            static const auto t_start = std::chrono::steady_clock::now();
            static double load_at = getenv("SSNE_LOAD_STATE_AT") ? atof(getenv("SSNE_LOAD_STATE_AT")) : -1;
            static std::vector<double> shots = [] {
                std::vector<double> v;
                if (const char* e = getenv("SSNE_AUTOSHOT"))
                    for (const char* p = e; *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : p + strlen(p)) v.push_back(atof(p));
                return v;
            }();
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            if (load_at >= 0 && el >= load_at) { request_state(false, g_set.state_slot); load_at = -1; }
            for (double& t : shots)
                if (t >= 0 && el >= t) { take_screenshot(base_dir); t = -1; }
        }
        update_rumble();
        bool background = !g_focused && g_set.pause_unfocused;
        // A save / load requested from the menu needs the game to reach its next frame: don't hold it meanwhile.
        ssne::set_paused(!network_game() && ((port::menu_open() && g_set.pause_in_menu && !ssne::savestate_pending()) || background));
        handle_state_result();
        if (background) SDL_Delay(15);  // nothing to show: keep the CPU/GPU mostly idle
        sample_performance(false);
        update_prompts();
        g_heartbeat++;
        bool new_frame = false;
        if (g_gpu) {
            g_loop_phase = "running GPU frames";
            new_frame = ssne::gpu_renderer_run(2);
            if (!new_frame && swap_ms < 2.0) {
                // Vsync off (or not honored by the driver): instead of spinning a CPU core on UI redraws the game
                // thread could use, sleep until the game queues its next frame (the UI still redraws >= 200 Hz).
                g_loop_phase = "waiting for a frame";
                if (ssne::gpu_renderer_wait(5)) new_frame = ssne::gpu_renderer_run(2);
            }
            update_auto_scale(win);
            static uint32_t next_stats = SDL_GetTicks() + 10000;
            if (SDL_GetTicks() >= next_stats) {  // periodic renderer stats in ssne.log (diagnostics)
                next_stats += 10000;
                auto gs = ssne::gpu_renderer_stats();
                auto o = ssne::gpu_renderer_output();
                char buf[256];
                snprintf(buf, sizeof(buf), "[gpu] frames %llu (dropped %llu), draws %llu, copies %llu, uploads %llu, shaders %llu (%llu from cache), render targets %llu, last %.1f ms, worst %.1f ms, GPU %.1f ms/frame, output %dx%d (%dx%s)",
                         (unsigned long long)gs.frames, (unsigned long long)ssne::gpu_dropped_frames(), (unsigned long long)gs.draws,
                         (unsigned long long)gs.tex_copies, (unsigned long long)gs.uploads, (unsigned long long)gs.programs,
                         (unsigned long long)gs.cached_programs,
                         (unsigned long long)gs.render_targets, gs.frame_ms, ssne::gpu_renderer_take_max_frame_ms(), g_auto.gpu_ms,
                         o.w, o.h, ssne::gpu_renderer_scale(), g_set.scale < 0 ? ", native" : g_set.scale ? "" : ", automatic");
                log_line(buf);
            }
        }
        else {
            new_frame = update_game_texture();
            if (!new_frame && swap_ms < 2.0) { SDL_Delay(1); new_frame = update_game_texture(); }
        }
        if (new_frame) {note_shown_frame();++g_presented_game_frames;}
        g_loop_phase = "UI";
        update_ui_scale(win);
        port::update_crosshair_renderer(g_set,std::exchange(crosshair_reload,false));
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        flush_crosshair_test_imgui();
        ImGui::NewFrame();
        draw_game_view();
        port::menu_ui::set_language(g_set.game_language>=0?unsigned(g_set.game_language):port::native_crosshair_language());
        draw_overlay(data_dir, dt);
        {
            port::MenuInfo mi;
            mi.network=network_game();mi.network_host=g_coop_host;mi.bot_controls=g_coop_host?draw_bot_controls:nullptr;
            mi.gpu_active = g_gpu;
            if (g_gpu) {
                mi.renderer_info = ssne::gpu_renderer_info();
                auto o = ssne::gpu_renderer_output();
                mi.out_w = o.w; mi.out_h = o.h;
                mi.current_scale = ssne::gpu_renderer_scale();
                mi.native_h = ssne::gpu_renderer_native();
                mi.msaa_active = ssne::gpu_renderer_msaa();
                mi.aniso_active = ssne::gpu_renderer_anisotropy();
                mi.gpu_ms = float(g_auto.gpu_ms);
            }
            mi.display_synced = ssne::vi_host_sync();
            if (SDL_GameController* c = first_pad()) mi.pad_name = SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "controller";
            for (int i = 0; i < kPadSlots; i++)
                mi.pads[i] = g_pads[i] ? (SDL_GameControllerName(g_pads[i]) ? SDL_GameControllerName(g_pads[i]) : "controller") : "";
            mi.settings_path = g_settings_path;
            port::fill_crosshair_menu_info(mi);
            for (int d = 0; d < SDL_GetNumVideoDisplays(); d++) {
                const char* n = SDL_GetDisplayName(d);
                SDL_DisplayMode m{};  // in pixels (the bounds are in scaled points with display scaling)
                SDL_GetDesktopDisplayMode(d, &m);
                mi.displays.push_back(std::string(n ? n : "Display") + "  (" + std::to_string(m.w) + "x" + std::to_string(m.h) + ")");
            }
            fill_state_slots(mi);
            if(mi.network) {
                const auto shared=ssne::mod_current_options();
                g_set.cheat_invulnerable=shared.god;g_set.cheat_infinite_ammo=shared.ammo;g_set.cheat_all_weapons=shared.weapons;
            }
            const std::array<bool,3> shared_before{g_set.cheat_invulnerable,g_set.cheat_infinite_ammo,g_set.cheat_all_weapons};
            bool was_open = port::menu_open();
            changed |= port::draw_menu(g_set, mi);
            if(mi.network_host && shared_before!=std::array<bool,3>{g_set.cheat_invulnerable,g_set.cheat_infinite_ammo,g_set.cheat_all_weapons}) {
                auto shared=ssne::mod_current_options();
                shared.god=g_set.cheat_invulnerable;shared.ammo=g_set.cheat_infinite_ammo;shared.weapons=g_set.cheat_all_weapons;
                apply_menu_mods(shared);
            }
            crosshair_reload|=port::menu_take_crosshair_reload();
            if (port::menu_take_exit_request()) quit = true;
            { int slot; bool save; if (port::menu_take_state_request(slot, save)) request_state(save, slot); }
            if (was_open && !port::menu_open()) save_now();  // closed with the window's X button
        }
        {
            port::ModMenuControls controls;controls.netplay=network_game();controls.host=g_coop_host;
            controls.requested=ssne::mod_current_options();
            controls.apply=[](ssne::ModOptions options){apply_menu_mods(options);};
            controls.apply_local=[](ssne::ModOptions options){ssne::mod_local_options(options);};
            controls.spawn=[](unsigned id,int count,float distance){spawn_menu_mod(id,count,distance);};
            port::draw_mod_menu(controls);
        }
        if (changed) { apply_settings(win); save_now(); }
        ImGui::Render();
        // Present a new game image, the menus / log / messages, and every vblank in display sync (the game ticks with
        // it); otherwise only every 100 ms (the info bar), so that frame rate tools see the game's real rate.
        static uint32_t last_present = 0;
        bool present = new_frame || port::menu_open() || port::mod_menu_open() || ssne::paused() || g_show_overlay || SDL_GetTicks() < g_toast_until ||
                       !have_game_image() || ssne::vi_host_sync() || (g_set.sync_display && g_set.vsync && g_set.fps_limit == 60) ||
                       SDL_GetTicks() - last_present >= 100;
        if (!present) {
            swap_ms = 0;  // nothing waited: the next pass waits for a game frame instead
            g_loop_phase = "events";
            continue;
        }
        last_present = SDL_GetTicks();
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if(hidden_test && g_test_ui_shot){
            g_test_ui_shot=false;static unsigned sequence=0;
            if(const auto folder=getenv("SSNE_TEST_SCREENSHOT_DIR")){
                std::vector<uint32_t> pixels(size_t(w)*h),flipped(pixels.size());
                glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                for(int y=0;y<h;++y)std::copy_n(pixels.data()+size_t(h-1-y)*w,w,flipped.data()+size_t(y)*w);
                port::write_png(std::string(folder)+"/ui-"+std::to_string(++sequence)+".png",flipped.data(),w,h);
                const auto output=ssne::gpu_renderer_output();
                std::ofstream metadata(std::string(folder)+"/ui-"+std::to_string(sequence)+".json");
                metadata<<"{\"output\":["<<output.w<<','<<output.h<<"],\"wide\":"<<output.wide<<",\"scale\":"<<ssne::gpu_renderer_scale()<<",\"display_copies\":"<<ssne::gx_render_stats().display_copies<<"}";
                const auto mod=ssne::mod_state();log_line("[mod-menu-test] active="+std::to_string(mod.active)+" god="+std::to_string(mod.options.god)+" ammo="+std::to_string(mod.options.ammo)+" rapid="+std::to_string(mod.options.rapid_fire));
            }
        }
        if (const char* shot = getenv("SSNE_SCREENSHOT")) {  // development: save the window after ~3 s and exit
            static uint32_t at=SDL_GetTicks()+(getenv("SSNE_SCREENSHOT_MS")?uint32_t(atoi(getenv("SSNE_SCREENSHOT_MS"))):3000);
            if (SDL_GetTicks() >= at) {
                std::vector<uint8_t> px(size_t(w) * h * 4);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
                for (int y = 0; y < h; y++) memcpy((uint8_t*)sf->pixels + size_t(y) * sf->pitch, &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
                SDL_SaveBMP(sf, shot);
                SDL_FreeSurface(sf);
                quit = true;
            }
        }
        g_loop_phase = "present (SwapWindow)";
        auto swap_t0 = std::chrono::steady_clock::now();
        SDL_GL_SwapWindow(win);
        auto swap_t1 = std::chrono::steady_clock::now();
        swap_ms = std::chrono::duration<double, std::milli>(swap_t1 - swap_t0).count();
        {   // Display sync: the swap returned at a vertical blank; tick the game's VI with it when the display runs at
            // ~60 Hz and every swap waits for vsync (checked continuously; any irregularity falls back to the timer).
            static auto last_swap = swap_t1;
            static int good = 0, bad = 0;
            double period = std::chrono::duration<double, std::milli>(swap_t1 - last_swap).count();
            last_swap = swap_t1;
            bool eligible = !network_game() && g_set.sync_display && g_set.vsync && g_set.fps_limit == 60;
            bool regular = period > 15.4 && period < 17.9 && swap_ms > 0.3;
            if (!ssne::paused()) {  // the settings menu pauses the game: keep the current state meanwhile
                if (eligible && regular) { good++; bad = 0; } else { bad++; good = 0; }
            }
            bool on = ssne::vi_host_sync();
            if (!on && eligible && good >= 60) { ssne::set_vi_host_sync(true); log_line("[port] display sync on: the game ticks with the display"); }
            else if (on && (!eligible || bad >= 3)) {
                ssne::set_vi_host_sync(false);
                log_line(eligible ? "[port] display sync paused (irregular display timing), using the game clock" : "[port] display sync off");
            }
            if (ssne::vi_host_sync()) ssne::vi_host_vblank();
        }
        g_loop_phase = "events";
    }
    // Exit. Give the display back first (leave fullscreen, destroy the GL context and window) so nothing of the
    // game stays on screen even if a later step stalls, save, and end the process from a watchdog if shutting the
    // game threads down takes too long.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::_Exit(0);
    }).detach();
    for (SDL_GameController* c : g_pads) if (c) SDL_GameControllerRumble(c, 0, 0, 0);  // don't leave them vibrating
    save_now();
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_SetWindowFullscreen(win, 0);
    SDL_HideWindow(win);
    port::shutdown_crosshair_renderer();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    SDL_GL_MakeCurrent(win, nullptr);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    stop_network();
    ssne::set_paused(false);
    ssne::scheduler_shutdown();
    log_line("[port] exit");
    if (g_log_file) fclose(g_log_file);
    std::_Exit(0);  // guest threads may still be inside game code
}
