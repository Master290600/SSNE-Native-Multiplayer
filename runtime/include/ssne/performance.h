#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include "ssne/performance_detail.h"

namespace ssne::perf {
using Clock=std::chrono::steady_clock;
inline constexpr std::array names={"tick","frame","render_begin","render_end","capture","publish","replica_apply",
    "time_step","viewport_0","viewport_1","viewport_2","viewport_3","native_render","flip","effects",
    "capture_bindings","preflight","creation","scalars","destructibles_hints","hud","markers_brushes",
    "player_resources","weapon_models_effects","skeletons","projectiles_retirement","gpu_submit","gpu_wait","swap","present_interval","update_interval","pacing","cpu_skinning","native_draw_mode","native_material_setup","native_visibility_setup","native_projection_setup","native_skinning_dispatch","ui_frame","events_input","gpu_backpressure","present_reused","gpu_run_empty","gpu_run_single","gpu_run_multiple","gpu_batch_last_age","ui_overlay","ui_settings","ui_mod_menu","ui_gl_begin","ui_sdl_begin","ui_imgui_begin","ui_game_view","ui_submit"};
inline constexpr std::array<double,15> bounds={.25,.5,1,2,4,8,12,16.667,25,33.334,50,100,250,1000,1e9};
struct Counter {std::mutex mutex;std::atomic<uint64_t> count{0},ns{0},max_ns{0},over33{0};std::array<std::atomic<uint64_t>,bounds.size()> histogram{};};
inline std::array<Counter,names.size()> counters;
inline std::atomic<bool> enabled{false};
inline Clock::time_point origin=Clock::now();
inline std::atomic<double> snapshot_received{0};
struct Context {uint64_t frame=0,epoch=0,entities=0;unsigned scene=0,ping_ms=0;uint64_t revision=0,bytes_received=0;double interpolation_ms=0;std::string level;std::array<float,3> position{};};
struct Slow {unsigned stage;double ms,at;uint64_t frame;};
inline std::mutex context_mutex;
inline Context context;
inline std::array<Slow,32> slow{};inline unsigned slow_count=0;
inline std::string quote(std::string_view s){std::string out="\"";for(unsigned char c:s){if(c=='"' || c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';}
inline constexpr unsigned index(std::string_view name){for(unsigned i=0;i<names.size();++i)if(name==names[i])return i;return unsigned(names.size());}
inline void record(unsigned stage,Clock::duration duration){
    if(stage>=names.size() || !enabled.load(std::memory_order_relaxed))return;
    const auto n=uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()));auto& c=counters[stage];
    {std::lock_guard lock(c.mutex);
    c.ns.fetch_add(n,std::memory_order_relaxed);c.count.fetch_add(1,std::memory_order_relaxed);
    auto old=c.max_ns.load(std::memory_order_relaxed);while(old<n && !c.max_ns.compare_exchange_weak(old,n,std::memory_order_relaxed)){}
    const auto ms=n/1e6;unsigned b=0;while(b+1<bounds.size() && ms>bounds[b])++b;c.histogram[b].fetch_add(1,std::memory_order_relaxed);if(ms>33.334)c.over33.fetch_add(1,std::memory_order_relaxed);}
    const auto ms=n/1e6;if(ms>33.334){std::lock_guard lock(context_mutex);
        Slow sample{stage,ms,std::chrono::duration<double>(Clock::now()-origin).count(),context.frame};
        if(slow_count<slow.size())slow[slow_count++]=sample;
        else{auto least=std::min_element(slow.begin(),slow.end(),[](auto a,auto b){return a.ms<b.ms;});if(ms>least->ms)*least=sample;}
    }
}
struct Scope {
    unsigned stage=unsigned(names.size());Clock::time_point start{};
    explicit Scope(std::string_view name){if(enabled.load(std::memory_order_relaxed)){stage=index(name);start=Clock::now();}}
    ~Scope(){if(stage<names.size())record(stage,Clock::now()-start);}
};
struct Laps {
    bool active=enabled.load(std::memory_order_relaxed);Clock::time_point at=active?Clock::now():Clock::time_point{};
    void mark(std::string_view name){if(active){const auto now=Clock::now();record(index(name),now-at);at=now;}}
};
inline void set_context(Context c){if(!enabled.load(std::memory_order_relaxed))return;c.level.resize(std::min<size_t>(128,c.level.size()));for(auto& f:c.position)if(!std::isfinite(f))f=0;ssne::perf::detail::set_context({c.frame,c.scene,c.level,c.position});std::lock_guard lock(context_mutex);context=std::move(c);}
struct Display {double render_fps=0,native_fps=0,updates=0,gpu_ms=0,gpu_cpu_ms=0;uint64_t dropped=0,draws=0,copies=0,uploads=0,shader_programs=0;unsigned pending=0,ready_players=0;int scale=0,width=0,height=0,limit=0;float fov=0;bool focused=false,minimized=false,vsync=false,paused=false,background=false,gpu_timer_valid=false,local_ready=false;};
// One writer on the UI thread; hot scopes use bounded counters, never disk I/O.
// Each session is bounded to two 8-MiB parts. Stage spans overlap: do not sum
// time_step/frame/render/apply. GPU time is asynchronous, not a frame fence.
class Writer {
    std::string path_,role_,header_;std::ofstream file_;size_t bytes_=0;Clock::time_point last_=Clock::now();
    void write(const std::string& line){if(!file_)return;if(bytes_+line.size()+1>8*1024*1024){file_.close();const auto old=path_+".previous";std::remove(old.c_str());if(std::rename(path_.c_str(),old.c_str())!=0){enabled=false;return;}file_.open(path_,std::ios::trunc|std::ios::binary);bytes_=header_.size()+1;file_<<header_<<'\n';}file_<<line<<'\n';file_.flush();bytes_+=line.size()+1;if(!file_)enabled=false;}
public:
    bool due()const{return enabled.load(std::memory_order_relaxed) && Clock::now()-last_>=std::chrono::seconds(1);}
    bool open(std::string path,std::string role){if(file_.is_open())file_.close();file_.clear();path_=std::move(path);role_=std::move(role);file_.open(path_,std::ios::trunc|std::ios::binary);if(!file_)return false;origin=last_=Clock::now();enabled=true;detail::configure();
        header_="{\"type\":\"header\",\"schema\":2,\"role\":"+quote(role_)+",\"utc_epoch_s\":"+std::to_string(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count())+",\"detail_sampling\":\"whole operations; default 1 in 16; self times partition a root\",\"thread_cpu_note\":\"Windows kernel+user thread time; wall minus CPU includes scheduling and waits, not GPU execution\",\"stage_spans_overlap\":true,\"histogram_percentiles_are_upper_bounds\":true,\"gpu_is_asynchronous\":true,\"max_bytes_per_part\":8388608,\"parts\":2}";write(header_);return true;}
    bool sample(const Display& d,bool force=false){const auto now=Clock::now();const auto seconds=std::chrono::duration<double>(now-last_).count();if(!enabled || (!force && seconds<1))return false;last_=now;
        Context ctx;std::array<Slow,32> events;unsigned count;{std::lock_guard lock(context_mutex);ctx=context;events=slow;count=slow_count;slow_count=0;}
        std::ostringstream s;s<<std::fixed<<std::setprecision(3)<<"{\"type\":\"sample\",\"elapsed_s\":"<<std::chrono::duration<double>(now-origin).count()<<",\"window_s\":"<<seconds<<",\"role\":"<<quote(role_)<<",\"frame\":"<<ctx.frame<<",\"epoch\":"<<ctx.epoch<<",\"scene\":"<<ctx.scene<<",\"level\":"<<quote(ctx.level)<<",\"revision\":"<<ctx.revision<<",\"ping_ms\":"<<ctx.ping_ms<<",\"bytes_received_total\":"<<ctx.bytes_received<<",\"interpolation_delay_ms\":"<<ctx.interpolation_ms<<",\"last_applied_motion_age_ms\":"<<(snapshot_received.load()>0?std::max(0.,(std::chrono::duration<double>(now.time_since_epoch()).count()-snapshot_received.load())*1000.):-1.)<<",\"entities\":"<<ctx.entities<<",\"position\":["<<ctx.position[0]<<','<<ctx.position[1]<<','<<ctx.position[2]<<"],\"render_fps\":"<<d.render_fps<<",\"native_fps\":"<<d.native_fps<<",\"updates_per_s\":"<<d.updates<<",\"background_mode\":"<<d.background<<",\"gpu_timer_valid\":"<<d.gpu_timer_valid<<",\"gpu_ms_async\":"<<d.gpu_ms<<",\"gpu_submit_cpu_last_ms\":"<<d.gpu_cpu_ms<<",\"gpu_pending\":"<<d.pending<<",\"dropped_total\":"<<d.dropped<<",\"draws_total\":"<<d.draws<<",\"copies_total\":"<<d.copies<<",\"uploads_total\":"<<d.uploads<<",\"shader_programs_total\":"<<d.shader_programs<<",\"scale\":"<<d.scale<<",\"output\":["<<d.width<<','<<d.height<<"],\"fps_limit\":"<<d.limit<<",\"fov\":"<<d.fov<<",\"focused\":"<<d.focused<<",\"minimized\":"<<d.minimized<<",\"vsync\":"<<d.vsync<<",\"paused\":"<<d.paused<<",\"stages\":{";
        bool first=true;for(unsigned i=0;i<counters.size();++i){auto& c=counters[i];std::lock_guard lock(c.mutex);const auto calls=c.count.exchange(0),total=c.ns.exchange(0),max=c.max_ns.exchange(0),over=c.over33.exchange(0);uint64_t sum=0;double p95=0,p99=0;for(unsigned b=0;b<bounds.size();++b){sum+=c.histogram[b].exchange(0);if(!p95 && sum>=std::max<uint64_t>(1,(calls*95+99)/100))p95=bounds[b];if(!p99 && sum>=std::max<uint64_t>(1,(calls*99+99)/100))p99=bounds[b];}if(!calls)continue;if(!first)s<<',';first=false;s<<quote(names[i])<<":{\"calls\":"<<calls<<",\"avg_ms\":"<<total/1e6/calls<<",\"max_ms\":"<<max/1e6<<",\"p95_upper_ms\":"<<p95<<",\"p99_upper_ms\":"<<p99<<",\"over33ms\":"<<over<<'}';}
        s<<"},\"worst_spans\":[";for(unsigned i=0;i<count;++i){if(i)s<<',';s<<"{\"stage\":"<<quote(names[events[i].stage])<<",\"ms\":"<<events[i].ms<<",\"at_s\":"<<events[i].at<<",\"guest_frame\":"<<events[i].frame<<'}';}s<<"]";detail::append_json(s,quote);s<<",\"local_ready\":"<<d.local_ready<<",\"ready_players\":"<<d.ready_players<<"}";write(s.str());return true;
    }
};
}
