#include "map_logic.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mapedit {
std::optional<LogicBoxBounds>
logic_box_bounds(const LogicNode& node, const std::array<float,3>& anchor) {
  if(node.type!=4050 || !std::isfinite(node.box_width) ||
     !std::isfinite(node.box_height) || node.box_width<=0 ||
     node.box_height<=0 || node.box_width>10000 || node.box_height>10000)
    return {};
  for(float value:anchor)
    if(!std::isfinite(value) || std::abs(value)>1000000)return {};
  return LogicBoxBounds{{anchor[0],anchor[1],anchor[2]-node.box_width},
                        {anchor[0]+node.box_width,anchor[1]+node.box_height,anchor[2]}};
}
namespace {
struct Read {
  const std::vector<uint8_t> &b;
  void range(uint64_t p, uint64_t n) const {
    if (p > b.size() || n > b.size() - p)
      throw std::runtime_error("Logic field outside map payload");
  }
  uint32_t u(uint64_t p) const {
    range(p, 4);
    return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
           uint32_t(b[p + 2]) << 8 | b[p + 3];
  }
  int32_t i(uint64_t p) const { return std::bit_cast<int32_t>(u(p)); }
  int32_t h(uint64_t p) const {
    range(p, 2);
    return std::bit_cast<int16_t>(uint16_t(uint16_t(b[p]) << 8 | b[p + 1]));
  }
  float f(uint64_t p) const {
    auto v = std::bit_cast<float>(u(p));
    if (!std::isfinite(v) || std::abs(v) > 1e8f)
      throw std::runtime_error("Invalid logic scalar");
    return v;
  }
  std::string text(uint32_t p) const {
    if (!p)
      return "Unnamed";
    std::string out;
    for (unsigned i = 0; i < 1024; ++i) {
      range(uint64_t(p) + i, 1);
      const auto c = b[p + i];
      if (!c)
        return out.empty() ? "Unnamed" : out;
      out += c >= 32 && c != 127 ? char(c) : '?';
    }
    throw std::runtime_error("Unterminated logic name");
  }
};
std::string number(float f) {
  std::ostringstream s;
  s << std::setprecision(4) << f;
  return s.str();
}
const char *spawner_mode(int n) {
  switch (n) {
  case 0:
    return "continuous";
  case 1:
    return "waves";
  case 3:
    return "triggered waves";
  case 5:
    return "accelerating waves";
  default:
    return "unsupported native mode";
  }
}
LogicEdgeKind signal(int mode) {
  switch (mode) {
  case 0:
    return LogicEdgeKind::Trigger;
  case 1:
    return LogicEdgeKind::Enable;
  case 2:
    return LogicEdgeKind::Disable;
  default:
    return LogicEdgeKind::Unknown;
  }
}
} // namespace
const LogicNode *LogicGraph::find(uint32_t id) const {
  auto p = std::find_if(nodes.begin(), nodes.end(),
                        [id](const auto &n) { return n.id == id; });
  return p == nodes.end() ? nullptr : &*p;
}
const char *logic_edge_name(LogicEdgeKind kind) {
  switch (kind) {
  case LogicEdgeKind::Trigger:
    return "Trigger";
  case LogicEdgeKind::Enable:
    return "Activate";
  case LogicEdgeKind::Disable:
    return "Deactivate";
  case LogicEdgeKind::Spawn:
    return "Spawn template";
  case LogicEdgeKind::Collected:
    return "On collection: trigger";
  case LogicEdgeKind::DoorEnter:
    return "On accepted entry: trigger";
  case LogicEdgeKind::DoorLeave:
    return "On leave";
  case LogicEdgeKind::DoorDenied:
    return "On key denied: trigger";
  case LogicEdgeKind::Touch:
    return "On touch";
  case LogicEdgeKind::Reference:
    return "Reference only";
  default:
    return "Unsupported action";
  }
}
LogicGraph build_logic_graph(const Document &doc) {
  const Read r{doc.payload()};
  r.range(0, 0x44);
  if (r.u(0) != 1 || r.u(4) != 100)
    throw std::runtime_error("Unsupported logic map header");
  LogicGraph out;
  std::map<uint32_t, int> objects;
  for (size_t i = 0; i < doc.objects().size(); ++i)
    objects.emplace(doc.objects()[i].offset, int(i));
  std::set<uint32_t> seen, ids;
  for (auto at = r.u(0x40); at; at = r.u(at)) {
    r.range(at, 24);
    if (!seen.insert(at).second || seen.size() > 50000)
      throw std::runtime_error("Invalid or cyclic logic entity list");
    // Global metadata (notably classes5020/6002) is stored in the same chain,
    // but has no placement matrix or registered actor ID. Its repeated zero
    // padding must not shadow a real authored Trigger/Player/etc. with ID0.
    const auto transform = r.u(at + 4);
    if (!transform && !objects.contains(at))
      continue;
    LogicNode n;
    n.offset = at;
    n.id = r.u(at + 20);
    n.type = r.u(at + 12);
    n.name = r.text(r.u(at + 8));
    if (!ids.insert(n.id).second)
      out.warnings.push_back("Duplicate authored entity ID " +
                             std::to_string(n.id));
    if (auto i = objects.find(at); i != objects.end())
      n.object = i->second;
    if (transform) {
      r.range(transform, 48);
      n.position = {r.f(transform + 12), r.f(transform + 28),
                    r.f(transform + 44)};
    }
    n.helper = wave_helper(n.type, n.name);
    n.role = category(n.type);
    n.summary = "No decoded control links for this entity type. Native actions "
                "may add behavior.";
    out.nodes.push_back(std::move(n));
  }
  auto edge = [&](const LogicNode &from, int32_t to, LogicEdgeKind kind,
                  std::string label, int slot = -1) {
    if (to < 0)
      return;
    const bool missing = !ids.contains(uint32_t(to));
    out.edges.push_back(
        {from.id, uint32_t(to), kind, std::move(label), missing, slot,
         slot >= 0 && kind != LogicEdgeKind::Unknown &&
             !(from.type == 4030 && from.name.starts_with("Editor Trigger "))});
    if (missing)
      out.warnings.push_back("Entity " + std::to_string(from.id) + " (" +
                             from.name + ") references missing ID " +
                             std::to_string(to));
  };
  for (auto &n : out.nodes) {
    const auto c = r.u(n.offset + 16);
    if (!c)
      continue;
    try {
      auto w = [&](uint32_t p) { return r.i(uint64_t(c) + p); };
      auto f = [&](uint32_t p) { return r.f(uint64_t(c) + p); };
      switch (n.type) {
      case 4030: // NativeSceneGameWatchPlayers, cfg16.
        n.supported = true;
        n.role = "Area trigger";
        n.initially_enabled = w(0) != 0;
        n.radius = f(4);
        if(n.name.starts_with("Editor Area ")) {
          for(const auto& slot:doc.logic_slots(n.id))if(slot.target)edge(n,int32_t(*slot.target),signal(unsigned(slot.action)),slot.label,int(slot.slot));
          if(auto spec=doc.logic_settings(n.id))n.delay=spec->delay;
        } else edge(n, w(8), LogicEdgeKind::Trigger, "Player enters area", 8);
        n.summary =
            "Radius " + number(n.radius) + " m; poll " +
            number(std::max(.1f, f(12))) + " s; " +
            (n.initially_enabled ? "initially enabled" : "initially disabled") +
            ". Fires once per player's entry; re-entry can fire again.";
        break;
      case 4080: { // NativeSceneGameControlsFF0, cfg128.
        n.supported = true;
        n.initially_enabled = w(0) != 0;
        n.delay = f(0x78);
        const bool counter = w(0x64) != 0;
        n.threshold = counter ? w(0x68) : 0;
        n.repeat_counter = w(0x6c) != 0;
        n.activation_limit = w(0x74);
        n.role = counter ? "Counter relay" : "Trigger relay";
        std::optional<LogicSpec> effect_spec;
        const auto effect_text=uint32_t(w(8));
        if(effect_text && uint64_t(effect_text)+gc::editor::effect_prefix.size()<=r.b.size() && std::equal(gc::editor::effect_prefix.begin(),gc::editor::effect_prefix.end(),r.b.begin()+effect_text))effect_spec=doc.logic_settings(n.id);
        if(effect_spec && effect_spec->kind==LogicKind::Light) {
          n.role="Point light";n.summary="Activate turns the point light on; Deactivate turns it off. Its colour/radius are authored in the map. Light inputs are immediate; use an external timer for delayed changes.";
          n.radius=effect_spec->effect.radius;
          break;
        }
        if(effect_spec && effect_spec->kind==LogicKind::Explosion) {
          n.role="Explosion";
          n.radius=effect_spec->effect.radius;
        }
        if(n.name.starts_with("Editor Timer "))n.role="Timer";
        if(n.name.starts_with("Editor Gate "))n.role="Condition gate";
        if(n.name.starts_with("Editor Timer ")) {
          for(const auto& slot:doc.logic_slots(n.id))if(slot.target)edge(n,int32_t(*slot.target),signal(unsigned(slot.action)),slot.label,int(slot.slot));
        } else for (unsigned i = 0; i < 10; ++i) {
          const auto mode = w(0x3c + i * 4);
          auto k = signal(mode);
          edge(n, w(0x14 + i * 4), k,
               std::string(logic_edge_name(k)) + " target " +
                   std::to_string(i + 1),
               int(0x14 + i * 4));
        }
        n.summary = std::string(n.initially_enabled ? "Initially enabled"
                                                    : "Initially disabled") +
                    "; delay " + number(n.delay) + " s; " +
                    (w(4) ? "requests itself at initialization; " : "");
        if (counter)
          n.summary +=
              "requires " + std::to_string(n.threshold) + " received triggers" +
              (n.repeat_counter ? " per repeating counter cycle; " : "; ");
        n.summary +=
            n.activation_limit > 0
                ? "activation cap " + std::to_string(n.activation_limit) + "."
                : "no configured activation cap.";
        n.summary +=
            " Counter inputs are events, not necessarily enemy deaths.";
        if(effect_spec && effect_spec->kind==LogicKind::Explosion)n.summary="Trigger emits the configured native explosion, visible effects and radial damage after its native delay. Activate/Deactivate arms or disarms this controller. "+n.summary;
        break;
      }
      case 4040: { // NativeGameEnemySpawnerActor, cfg60.
        n.supported = true;
        n.role = "Wave spawner";
        n.initially_enabled = false;
        n.wave_count = w(8) > 0 ? unsigned(w(8)) : 0;
        n.total_spawn_count = w(12) > 0 ? unsigned(w(12)) : 0;
        n.delay = f(0x1c);
        edge(n, w(0x14), LogicEdgeKind::Spawn, "Enemy template");
        if (w(0x24) > 0)
          edge(n, w(0x24), LogicEdgeKind::Reference,
               "Patrol/action target (reference)");
        n.summary = std::string(spawner_mode(w(0x20))) + "; " +
                    std::to_string(n.wave_count) + " per wave; " +
                    std::to_string(n.total_spawn_count) +
                    " total; initial delay " + number(n.delay) +
                    " s; spawn interval " + number(f(0)) + " s; wave pause " +
                    number(f(4)) +
                    " s. Waiting for a native trigger at initialization.";
        break;
      }
      case 4050: { // DoorController derives WatchPlayers; cfg40.
        n.supported = true;
        n.role = "Door controller";
        if(n.name.starts_with("Editor Key branch "))n.role="Key condition branch";
        n.initially_enabled = w(0) != 0;
        n.box_width = f(8);
        n.box_height = f(4);
        n.key_index = w(0x24);
        edge(n, w(0x10), LogicEdgeKind::DoorEnter, "Accepted entry: target 1",
             16);
        edge(n, w(0x14), LogicEdgeKind::DoorEnter, "Accepted entry: target 2",
             20);
        edge(n, w(0x20), LogicEdgeKind::DoorDenied, "Key denied: target", 32);
        n.summary = std::string(n.initially_enabled ? "Initially enabled"
                                                    : "Initially disabled") +
                    "; entry box " + number(f(8)) + " m wide, " + number(f(4)) +
                    " m high; " + (w(12) ? "players only; " : "");
        if (w(24) == 1)
          n.summary += "requires key index " + std::to_string(n.key_index) +
                       " (pickup kind " + std::to_string(n.key_index + 46) +
                       "); key is consumed once.";
        else if (w(24) == 0)
          n.summary += "no key requirement.";
        else
          n.summary += "unsupported entry mode " + std::to_string(w(24)) +
                       "; its condition is not inferred.";
        break;
      }
      case 4070: { // NativeMovingBrushConfigurations, cfg0x3e.
        n.supported = true;
        n.role = "Moving door / brush";
        n.initially_enabled = w(0x10) != 0;
        if (w(0x24) > 0)
          edge(n, w(0x24), LogicEdgeKind::Reference,
               "Interaction switch (reference)");
        const auto target = r.h(uint64_t(c) + 0x3c);
        if (target > 0)
          edge(n, target, LogicEdgeKind::Trigger, "On damage: trigger");
        n.summary =
            std::string(n.initially_enabled ? "Movement enabled"
                                            : "Movement disabled") +
            "; " +
            (w(0) ? "initial motion requested; " : "waits for request; ") +
            "duration " + number(f(4)) + " s; pause " + number(f(8)) + " s; " +
            (w(0x18) ? "player touch can request motion. " : "") +
            "Motion follows native path nodes; arbitrary incoming links are "
            "not all mandatory conditions.";
        break;
      }
      case 4180: // NativeBrushSwitchActor, cfg12.
        n.supported = true;
        n.role = "Switch";
        n.initially_enabled = w(0) != 0;
        if(n.name.starts_with("Editor Button ") || n.name.starts_with("Editor Toggle ")) {
          n.role=n.name.starts_with("Editor Toggle ")?"Toggle switch":"Button";
          for(const auto& slot:doc.logic_slots(n.id))if(slot.target)edge(n,int32_t(*slot.target),signal(unsigned(slot.action)),slot.label,int(slot.slot));
          if(auto spec=doc.logic_settings(n.id))n.delay=spec->delay;
        } else edge(n, w(4), LogicEdgeKind::Trigger, "On switch interaction", 4);
        n.summary = std::string(n.initially_enabled ? "Initially enabled"
                                                    : "Initially disabled") +
                    "; " + (w(8) ? "reusable." : "one successful use.");
        break;
      case 4170: { // NativeGameTouchFieldActor, cfg28.
        n.supported = true;
        n.role = "Touch field";
        n.initially_enabled = w(0) != 0;
        auto kind = signal(w(8));
        edge(n, w(12), kind, "On touch: " + std::string(logic_edge_name(kind)),
             12);
        n.summary = std::string(n.initially_enabled ? "Initially enabled"
                                                    : "Initially disabled") +
                    "; " + (w(4) ? "players only; " : "") +
                    "disables itself after a valid touch. Native collision "
                    "field index " +
                    std::to_string(w(16)) + ".";
        break;
      }
      case 4020: // NativeSceneGameWatchPlayers teleport cfg16.
        n.supported = true;
        n.role = "Teleport";
        n.initially_enabled = w(0) != 0;
        edge(n, w(12), LogicEdgeKind::Reference,
             "Teleport destination (reference)");
        n.summary = "Teleport area " + number(f(4)) + " m wide, " +
                    number(f(8)) +
                    " m high. This reference moves the player; it is not a "
                    "trigger signal.";
        break;
      case 5000:
        n.supported = true;
        n.role = "Enemy template";
        n.initially_enabled = w(4) == 0;
        edge(n, w(8), LogicEdgeKind::Trigger, "On death: trigger", 8);
        if (w(8) >= 0 && uint32_t(w(8)) == n.id) {
          out.warnings.push_back("Entity " + std::to_string(n.id) + " (" +
                                 n.name +
                                 "): Enemy death target points to itself; no "
                                 "separate completion counter is signalled.");
        }
        n.summary =
            std::string(n.initially_enabled ? "Active authored enemy"
                                            : "Preload-only enemy template") +
            ". Native death sends a trigger to its configured target; "
            "additional AI/custom actions are not inferred from distance or "
            "name.";
        break;
      case 4120:
        n.supported=true;n.role="Camera path";
        n.summary="Trigger starts this original native camera path. Its native camera configuration and path remain in the map.";
        break;
      case 4090:
        n.supported=true;n.role="Music action";
        n.summary="Trigger or Activate plays the configured native music cue. Deactivate is not implemented by this native class.";
        break;
      case 4100:
        n.supported=true;n.role="Sound action";
        edge(n,w(8),LogicEdgeKind::Trigger,"On sound finished",8);
        n.summary="Trigger or Activate plays the native sound; Deactivate stops it. Uses the configured sound asset and spatial settings.";
        break;
      case 4130:
        n.supported=true;n.role="Message / sequence event";
        n.summary="Native message holder accepts Trigger, Activate and Deactivate. Its original localized message/action data remains unchanged.";
        break;
      case 4310:
        n.supported=true;n.role="Particle effect";
        n.summary="Activate creates the native particle effect; Deactivate removes it. Trigger is ignored by this original class and cannot be assigned.";
        break;
      case 4260:
        n.supported=true;n.role="Arrow marker";
        n.summary="Activate shows the native arrow marker; Deactivate hides it. This is also a directional visual marker in the editor.";
        break;
      case 4270:
        n.supported=true;n.role="Lockdown gate";
        n.summary="Activate and Deactivate control the original lockdown. Trigger is ignored and is not offered.";
        break;
      case 4280:
        n.supported=true;n.role="Warp players";
        n.summary="Trigger starts the original player warp; Activate and Deactivate control its enabled state.";
        break;
      case 4340:
        n.supported=true;n.role="Movie / sequence";
        n.summary="Trigger starts the original movie identified by this actor. Activate/Deactivate are not native movie actions.";
        break;
      case 4230:
        n.supported=true;n.role="Rolling stone";
        n.summary="Trigger or Activate starts the original rolling stone; Deactivate stops it.";
        break;
      case 4160:
        n.supported=true;n.role="Level transition";
        n.summary="Trigger requests the configured native level transition. Host authority handles multiplayer level changes.";
        break;
      case 4200:
        n.supported=true;n.role="Model / visibility";
        n.summary="Activate and Deactivate control this native model holder. They change model visibility, not illumination.";
        break;
      case 4110:
        n.role="Targeted damage";
        n.summary="Native damage can require a non-null activating actor. Generic timer/relay links are rejected to prevent invalid native calls.";
        break;
      default:
        if (n.type >= 2000 && n.type < 4000) {
          n.supported = true;
          n.role = "Pickup";
          edge(n, w(8), LogicEdgeKind::Collected, "On successful collection",
               8);
          n.summary = std::string(w(4) ? "Respawning pickup."
                                       : "Non-respawning pickup.") +
                      " Its target is triggered only after inventory accepts "
                      "the collection.";
        }
        break;
      }
    } catch (const std::exception &e) {
      n.supported = false;
      n.summary =
          "Invalid/unsupported control configuration: " + std::string(e.what());
      out.warnings.push_back("Entity " + std::to_string(n.id) + ": " +
                             n.summary);
      out.edges.erase(
          std::remove_if(out.edges.begin(), out.edges.end(),
                         [&](const auto &e) { return e.from == n.id; }),
          out.edges.end());
    }
  }
  out.warnings.push_back(
      "Connections decode known authored control fields only. Native actions, "
      "AI, dynamic relations and custom scripts can add dependencies; a static "
      "incoming link alone does not prove why a door is blocked.");
  return out;
}
} // namespace mapedit
