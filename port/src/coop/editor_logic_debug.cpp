#include "editor_logic_debug.h"
#include "native_bootstrap.h"
#include "native_world.h"
#include "native_particles.h"
#include "ssne/game_addresses.h"
#include "ssne/native_entity.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include "ssne/editor_logic_effects.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace port::coop {
namespace {
using Clock = std::chrono::steady_clock;
bool readable(uint32_t p, uint64_t n) {
  return p >= 0x80000000u && n <= ssne::guest_ram_size() &&
         uint64_t(p) + n <= ssne::guest_ram_end();
}
void need(bool yes, const char *message) {
  if (!yes) throw std::runtime_error(message);
}
int64_t integer(uint32_t p) { return int32_t(mem_read32(p)); }
float scalar(uint32_t p) {
  const float value = u2f(mem_read32(p));
  need(std::isfinite(value) && std::abs(value) <= 1000000.f,
       "Native logic scalar is unavailable");
  return value;
}
bool bit(uint32_t p) {
  const auto n = mem_read32(p);
  need(n <= 1, "Native logic object is still initializing");
  return n != 0;
}
std::string quote(const std::string &s) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) {
      out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
    } else out += char(c);
  }
  return out + '"';
}
struct Source { uint32_t id = 0, type = 0, config = 0; bool ambiguous = false; };
struct Row {
  uint32_t id = 0, type = 0;
  bool observed = false, present = false;
  std::string status = "unavailable", reason;
  std::map<std::string, int64_t> ints;
  std::map<std::string, double> numbers;
};
struct Event {
  uint64_t sequence = 0, frame = 0;
  uint32_t id = 0;
  std::string kind, text;
};
struct Enemy { uint32_t vtable = 0; bool created = false, registered = false; };
struct State {
  std::filesystem::path path;
  std::string token, expected, level;
  Clock::time_point began{}, sampled{};
  uint64_t subscription = 0, epoch = 1, sequence = 0, event_sequence = 0;
  uint32_t resource = 0;
  std::vector<Source> source;
  std::map<uint32_t, Row> previous;
  std::map<uint32_t, Enemy> enemies;
  std::deque<Event> events;
  bool epoch_changed = false;
} state;

void observe(const ssne::NativeEntityNotice &notice) {
  using Kind = ssne::NativeEntityNotice::Event;
  if (notice.event == Kind::LevelExit) {
    state.enemies.clear(); state.previous.clear(); state.events.clear();
    state.source.clear(); state.resource = 0; state.level.clear();
    ++state.epoch; state.epoch_changed = true;
    return;
  }
  if (notice.event == Kind::Removing) { state.enemies.erase(notice.object); return; }
  if (notice.event != Kind::CreatedActor && notice.event != Kind::Registered) return;
  if (!readable(notice.object, 0x4c)) return;
  const auto vt = mem_read32(notice.object + 0x48);
  if (!readable(vt, 4)) return;
  if (state.enemies.size() >= 32768 && !state.enemies.contains(notice.object)) return;
  auto &entry = state.enemies[notice.object];
  if (entry.vtable != vt) entry = {vt, false, false};
  if (notice.event == Kind::CreatedActor) entry.created = true;
  if (notice.event == Kind::Registered && notice.update_group != -1) entry.registered = true;
}
void event(uint32_t id, const char *kind, const std::string &text, uint64_t frame) {
  state.events.push_back({++state.event_sequence, frame, id, kind, text});
  while (state.events.size() > 256) state.events.pop_front();
}
int64_t value(const Row &r, const char *name) {
  const auto it = r.ints.find(name); return it == r.ints.end() ? 0 : it->second;
}
void changed(const Row &r, uint64_t frame) {
  const auto old = state.previous.find(r.id);
  if (old == state.previous.end()) {
    if (r.observed && r.present) event(r.id, "appeared", "Native logic state observed: " + r.status, frame);
    state.previous[r.id] = r; return;
  }
  const auto &before = old->second;
  if (r.present && !before.present) event(r.id, "appeared", "Native logic object became available", frame);
  if (!r.present && before.present) event(r.id, "retired", "Native logic object is no longer indexed", frame);
  if (r.observed && before.observed) {
    if (value(r, "activations") > value(before, "activations") ||
        (value(r, "action_has_run") && !value(before, "action_has_run")))
      event(r.id, "activated", "Observed native trigger activation / dispatch state", frame);
    if (value(r, "spawned") > value(before, "spawned"))
      event(r.id, "spawned", "Native spawner total is now " + std::to_string(value(r, "spawned")), frame);
    if (value(r, "count") != value(before, "count"))
      event(r.id, "counter", "Native counter is now " + std::to_string(value(r, "count")), frame);
    if (r.status != before.status)
      event(r.id, "state", "Observed state: " + before.status + " -> " + r.status, frame);
  }
  state.previous[r.id] = r;
}
void load_source(uint32_t resource, const std::string &level) {
  if (state.resource == resource && state.level == level) return;
  need(readable(resource, 0x80) && mem_read32(resource) == 1 &&
           mem_read32(resource + 4) == 100,
       "Original loaded level image is not available");
  if (state.resource && !state.epoch_changed) ++state.epoch;
  state.epoch_changed = false; state.previous.clear(); state.events.clear();
  state.source.clear(); state.resource = resource; state.level = level;
  std::set<uint32_t> seen;
  std::map<uint32_t, size_t> identifiers;
  for (auto node = mem_read32(resource + 0x40); node; node = mem_read32(node)) {
    need(readable(node, 44) && seen.insert(node).second && seen.size() <= 32768,
         "Invalid original loaded level entity chain");
    const auto type = mem_read32(node + 12), id = mem_read32(node + 20);
    if (type != 4030 && type != 4040 && type != 4050 && type != 4070 &&
        type != 4080 && type != 4180 && type != 4310 && type != 5000) continue;
    if (identifiers.contains(id)) {
      state.source[identifiers.at(id)].ambiguous = true;
      continue;
    }
    identifiers[id] = state.source.size();
    state.source.push_back({id, type, mem_read32(node + 16)});
  }
}
std::map<uint32_t, int64_t> living_signallers() {
  std::map<uint32_t, int64_t> result;
  for (const auto &[object, e] : state.enemies) {
    if (!e.created || !e.registered || e.vtable == 0x802121E8u ||
        !readable(object, 0xd8) || mem_read32(object + 0x48) != e.vtable) continue;
    const float health = u2f(mem_read32(object + 0xcc));
    if (!(std::isfinite(health) && health > 0 && health <= 1000000.f)) continue;
    // Original Enemy+D4 is the target looked up by game_grunt_death before
    // sending event13. It is NOT the enemy-template identity or wave owner.
    const auto target = integer(object + 0xd4);
    if (target >= 0 && target <= 32767) ++result[uint32_t(target)];
  }
  return result;
}
unsigned watched(uint32_t object) {
  // PAL WatchActors::initialize (80079CC4) writes its 8-entry pool at +54.
  const auto storage = mem_read32(object + 0x54);
  need(readable(storage, 32), "Original watched-actor array is unavailable");
  unsigned count = 0;
  for (unsigned i = 0; i < 8; ++i) count += mem_read32(storage + i * 4) != 0;
  return count;
}
Row read_row(const Source &s, const std::map<uint32_t, int64_t> &alive) {
  Row r; r.id = s.id; r.type = s.type;
  if (s.ambiguous || s.id > 32767) {
    r.reason = s.ambiguous ? "Authored identifier is duplicated; no unique live object can be attributed" : "Authored identifier is outside the native script registry range";
    return r;
  }
  if (s.type == 5000) {
    r.observed = true; r.status = "template";
    r.reason = "Authored enemy template; runtime AI controls each spawned actor";
    if (readable(s.config, 12)) r.ints["death_target_id"] = integer(s.config + 8);
    return r;
  }
  auto found = native_script_reference_address(s.id);
  if (s.id == 0) {
    // ID zero is used by some original maps. The existing helper excludes
    // zero, so validate the same original map here.
    const auto table=mem_read32(0x802DB7C4u),count=mem_read32(0x802DB7C8u),capacity=mem_read32(0x802DB7CCu);
    if(count && count<=capacity && count<=32768 && readable(table,uint64_t(count)*8) &&
       mem_read16(table)==0 && (count==1 || mem_read16(table+8)!=0)) {
      const auto object=mem_read32(table+4),vt=readable(object,0x54)?mem_read32(object+0x48):0;
      if(readable(vt,0x280) && mem_read32(vt+0x27c)==0x8007A4CCu && mem_read32(object+0x50)==0) found=object;
    }
  }
  if (!found || !*found) {
    r.status = state.previous.contains(s.id) && state.previous.at(s.id).present ? "retired" : "not-indexed";
    r.reason = "No current native script object for this ID (not loaded, excluded or removed)";
    return r;
  }
  const auto p = *found; r.present = true;
  try {
    const auto vt = mem_read32(p + 0x48);
    if (s.type == 4080) {
      need(vt == 0x80209758u && readable(p, 0xa8) && readable(s.config, 128) &&
               mem_read32(p + 0x54) == s.config, "Native trigger layout is unavailable");
      r.ints["enabled"] = bit(p + 0x8c); r.ints["requested_enabled"] = bit(p + 0x88);
      r.ints["count"] = integer(p + 0x90); r.ints["activations"] = integer(p + 0x94);
      r.numbers["timer"] = std::max(0.f, scalar(p + 0x98));
      r.ints["exhausted"] = bit(p + 0xa0); r.ints["action_has_run"] = bit(p + 0xa4);
      r.ints["counter_enabled"] = mem_read32(s.config + 0x64) != 0;
      r.ints["threshold"] = integer(s.config + 0x68);
      r.ints["activation_limit"] = integer(s.config + 0x74);
      r.ints["alive_signallers_known"] = ssne::native_entity_hooks_available();
      r.ints["alive_signallers"] = alive.contains(s.id) ? alive.at(s.id) : 0;
      const auto remaining = std::max<int64_t>(0, value(r,"threshold") - value(r,"count"));
      r.ints["remaining_signals"] = remaining;
      if (r.numbers["timer"] > 0) { r.status="delay";r.reason="Native trigger delay is still running"; }
      else if (!value(r,"enabled")) { r.status="disabled";r.reason="Native trigger is disabled"; }
      else if (value(r,"exhausted") && value(r,"action_has_run")) { r.status="finished";r.reason="Native trigger has dispatched and exhausted its counter / activation limit"; }
      else if (value(r,"counter_enabled") && remaining > 0) {
        r.status="counter-wait";r.reason="Waiting for " + std::to_string(remaining) + " more counter signals";
        if (ssne::native_entity_hooks_available()) r.reason += "; " + std::to_string(value(r,"alive_signallers")) + " living enemies currently signal this counter";
      } else if (value(r,"action_has_run")) { r.status="triggered";r.reason="Native dispatch has been observed; further signals may still be accepted"; }
      else { r.status="armed";r.reason="Waiting for a native trigger signal"; }
    } else if (s.type == 4040) {
      need(vt == 0x80204E30u && readable(p,0xa8) && readable(s.config,60) &&
               mem_read32(p+0x54)==s.config,"Native spawner layout is unavailable");
      r.ints["initial"]=bit(p+0x58);r.ints["queued"]=integer(p+0x5c);r.ints["enabled"]=bit(p+0x60);
      r.ints["spawned"]=integer(p+0x70);r.ints["wave_count"]=integer(p+0x78);
      r.ints["quota"]=integer(s.config+8);r.ints["total"]=integer(s.config+12);
      r.ints["template_id"]=integer(s.config+20);r.ints["mode"]=integer(s.config+32);
      r.numbers["configured_delay"]=scalar(s.config+28);
      const bool initial=value(r,"initial")!=0;
      // These fields are not initialized until the original BeginSpawning.
      r.numbers["delay"]=initial?0:scalar(p+0x64);r.numbers["interval"]=initial?0:scalar(p+0x68);
      r.numbers["wave_delay"]=initial?0:scalar(p+0x6c);r.ints["wave_spawned"]=initial?0:integer(p+0x74);
      r.numbers["delay_remaining"]=value(r,"enabled")?std::max(0.,r.numbers["configured_delay"]-r.numbers["delay"]):0;
      if(initial){r.status="armed";r.reason="Wave has not been started by its native trigger";}
      else if(r.numbers["delay_remaining"]>0){r.status="delay";r.reason="Wave started; native spawn delay is still running";}
      else if(value(r,"enabled")){r.status="spawning";r.reason="Native spawner is running (interval / safety backoff may postpone the next enemy)";}
      else if(value(r,"total")>0 && value(r,"spawned")>=value(r,"total")){r.status="finished";r.reason="Native spawner has emitted its configured total";}
      else if(value(r,"queued")>0){r.status="queued";r.reason="Another native wave activation is queued";}
      else{r.status="waiting-trigger";r.reason="Current wave emitted; waiting for another activation";}
    } else if (s.type == 4030 || s.type == 4050) {
      // PAL 800866B8 (WatchPlayers) / 8007B1D4 (DoorController) use +58
      // for configuration, +68 for enabled; Door's paid field is +6C.
      need(vt==(s.type==4050?0x802045E8u:0x8020A368u) && readable(p,s.type==4050?0x78:0x70) && readable(s.config,s.type==4050?40:16) &&
               mem_read32(p+0x58)==s.config,"Native area/door controller layout is unavailable");
      r.ints["enabled"]=bit(p+0x68);r.ints["watched_count"]=watched(p);
      if(s.type==4050){
        r.ints["paid"]=bit(p+0x6c);r.ints["mode"]=integer(s.config+24);r.ints["key_index"]=integer(s.config+36);
        if(!value(r,"enabled")){r.status="disabled";r.reason="Native door controller is disabled; inspect its incoming activation path";}
        else if(value(r,"mode")==1 && !value(r,"paid")){r.status="key-required";r.reason="Configured key has not yet been consumed by this controller";}
        else if(value(r,"mode")<0 || value(r,"mode")>1){r.status="unsupported-mode";r.reason="Door-controller mode is not understood by the diagnostic";}
        else{r.status=value(r,"watched_count")?"occupied":"armed";r.reason=value(r,"watched_count")?"Eligible actor is in the controller area; inspect linked receivers":"Waiting for an actor to enter the native door area";}
      }else{
        r.ints["target_id"]=integer(s.config+8);r.numbers["radius"]=scalar(s.config+4);
        r.status=!value(r,"enabled")?"disabled":value(r,"watched_count")?"occupied":"armed";
        r.reason=!value(r,"enabled")?"Native area trigger is disabled":value(r,"watched_count")?"Actors are currently watched inside this native area":"Waiting for a player to enter the native area";
      }
    } else if(s.type==4070){
      need(vt==0x80206868u && readable(p,0x13c) && readable(s.config,0x40) &&
               mem_read32(p+0x5c)==s.config,"Native moving-brush layout is unavailable");
      r.ints["active"]=bit(p+0xb0);r.ints["requested"]=bit(p+0xb4);
      r.numbers["progress"]=scalar(p+0xc0);r.ints["segment"]=integer(p+0xc8);r.ints["completed"]=bit(p+0x58);
      r.ints["movement_enabled"]=mem_read32(s.config+0x10)!=0;
      need(r.numbers["progress"]>=0&&r.numbers["progress"]<=1,"Native brush progress is invalid");
      if(value(r,"active")&&!value(r,"movement_enabled")){r.status="movement-disabled";r.reason="Movement requested, but disabled in the authored brush configuration";}
      else if(value(r,"active")||value(r,"requested")){r.status="moving";r.reason="Native brush movement is running / requested";}
      else if(!value(r,"segment")&&r.numbers["progress"]==0){r.status="initial-position";r.reason="At its authored initial position; inspect incoming conditions. Position alone does not prove whether a door is open or closed";}
      else{r.status="stopped";r.reason="Native brush is stopped at a path position; inspect linked counters, area controller and path stops";}
    }else if(s.type==4180){
      need(vt==0x80208B60u && readable(p,0x68) && readable(s.config,12) &&
               mem_read32(p+0x54)==s.config,"Native switch layout is unavailable");
      r.ints["enabled"]=bit(p+0x60);r.ints["available"]=bit(p+0x64);r.ints["target_id"]=integer(s.config+4);
      r.status=!value(r,"enabled")?"disabled":value(r,"available")?"armed":"used";
      r.reason=!value(r,"enabled")?"Native switch is disabled":value(r,"available")?"Waiting for interaction":"Native switch is no longer available for interaction";
    } else if(s.type==4310){
      need(vt==native_particles_vtable && readable(p,0x64) &&
               mem_read32(p+0x54)==s.config,"Native particle holder layout is unavailable");
      EntityState observed;std::string error;
      need(capture_native_particles(p,observed,error),error.c_str());
      r.ints["enabled"]=std::get<int64_t>(observed.properties.at("particles_enabled"));
      r.ints["particle_kind"]=std::get<int64_t>(observed.properties.at("particles_kind"));
      r.status=value(r,"enabled")?"active":"disabled";
      r.reason=value(r,"enabled")?"Native scene emitter is active":"Native scene emitter emission is disabled";
    }
    r.observed=true;
  }catch(const std::exception&e){r.status="unavailable";r.reason=e.what();}
  return r;
}
void publish(const std::string &level,unsigned scene,uint64_t frame,bool ready,
             const std::string &error,const std::vector<Row>&rows,bool authority) {
  std::ostringstream out;out.precision(9);
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-state.began).count();
  const auto effects=ssne::editor_logic_effect_stats();
  out<<"{\"schema\":1,\"token\":"<<quote(state.token)<<",\"level\":"<<quote(level)
     <<",\"expected_level\":"<<quote(state.expected)<<",\"scene\":"<<scene
     <<",\"frame\":"<<frame<<",\"sequence\":"<<++state.sequence<<",\"epoch\":"<<state.epoch
     <<",\"sample_ms\":"<<ms<<",\"authority\":"<<(authority?"true":"false")
     <<",\"ready\":"<<(ready?"true":"false")<<",\"error\":"<<quote(error)
     <<",\"events_are_observations\":true,\"enemy_lifetimes_observed\":"<<(ssne::native_entity_hooks_available()?"true":"false")
     <<",\"editor_effects\":{\"helpers\":"<<effects.helpers<<",\"lights\":"<<effects.lights<<",\"explosions\":"<<effects.explosions<<",\"rejected\":"<<effects.rejected<<"},\"nodes\":[";
  bool first=true;
  for(const auto&r:rows){if(!first)out<<',';first=false;
    out<<"{\"id\":"<<r.id<<",\"type\":"<<r.type<<",\"observed\":"<<(r.observed?"true":"false")
       <<",\"present\":"<<(r.present?"true":"false")<<",\"status\":"<<quote(r.status)<<",\"reason\":"<<quote(r.reason);
    for(const auto&[key,v]:r.ints)out<<','<<quote(key)<<':'<<v;
    for(const auto&[key,v]:r.numbers)out<<','<<quote(key)<<':'<<v;out<<'}';
  }
  out<<"],\"events\":[";first=true;
  for(const auto&e:state.events){if(!first)out<<',';first=false;out<<"{\"sequence\":"<<e.sequence<<",\"id\":"<<e.id<<",\"kind\":"<<quote(e.kind)<<",\"text\":"<<quote(e.text)<<",\"frame\":"<<e.frame<<",\"observed_change\":true}";}
  out<<"]}\n";
  auto pending=state.path;pending += ".pending";
  std::ofstream file(pending,std::ios::binary|std::ios::trunc);if(!file)return;file<<out.str();file.close();if(!file)return;
#ifdef _WIN32
  MoveFileExW(pending.c_str(),state.path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
#else
  std::error_code ec;std::filesystem::rename(pending,state.path,ec);
#endif
}
} // namespace
void configure_editor_logic_debug(std::string output,std::string token,std::string expected) {
  if(state.subscription)ssne::stop_observing_native_entities(state.subscription);
  state=State{};
  if(output.empty())return;
  if(token.empty()||token.size()>128||expected.size()>64)
    throw std::invalid_argument("Invalid editor logic diagnostic identity");
  state.path=std::filesystem::path(std::u8string(output.begin(),output.end()));state.token=std::move(token);state.expected=std::move(expected);state.began=Clock::now();
  state.subscription=ssne::observe_native_entities(observe);
}
void sample_editor_logic_debug(PPCContext &,uint64_t frame) {
  if(state.path.empty())return;
  const auto now=Clock::now();if(state.sampled!=Clock::time_point{}&&now-state.sampled<std::chrono::milliseconds(250))return;state.sampled=now;
  std::vector<Row> rows;std::string level,error;unsigned scene=0;bool ready=false;
  const bool authority=!ssne::replica_simulation();
  try {
    need(mem_read32(0x80000000u)==0x47334250u,"Logic diagnostics support the original PAL revision only");
    scene=mem_read32(ssne::game_address(0x802DB0E0));
    if(auto current=native_active_level())level=*current;
    if(scene!=4)error="Waiting for native gameplay";
    else if(!authority)error="Replica state cannot explain the host's logic; run the editor test as host";
    else if(!state.expected.empty()&&level!=state.expected)error="A different level is running; this editor snapshot is not current";
    else{
      const auto owner=mem_read32(0x802DACC0u);need(readable(owner,0x29c),"Native level owner is unavailable");
      const auto manager=mem_read32(owner+0x298);need(readable(manager,8),"Native level image owner is unavailable");
      const auto resource=mem_read32(manager+4);load_source(resource,level);const auto alive=living_signallers();
      for(const auto&s:state.source){auto row=read_row(s,alive);changed(row,frame);rows.push_back(std::move(row));}
      ready=true;
    }
  }catch(const std::exception&e){error=e.what();}
  publish(level,scene,frame,ready,error,rows,authority);
}
} // namespace port::coop
