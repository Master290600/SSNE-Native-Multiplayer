#include "map_document.h"
#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>

namespace mapedit {
namespace {
void need(bool v, const char *why) {
  if (!v)
    throw std::runtime_error(why);
}
uint32_t u(const std::vector<uint8_t> &b, uint64_t p) {
  need(p + 4 <= b.size(), "Connection field is outside the map");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
void write_word(std::vector<uint8_t> &b, uint64_t p, uint32_t v) {
  need(p + 4 <= b.size(), "Connection write is outside the map");
  for (unsigned i = 0; i < 4; ++i)
    b[p + i] = uint8_t(v >> (24 - 8 * i));
}
std::string name(const std::vector<uint8_t> &b, uint32_t p) {
  std::string result;
  for (unsigned i = 0; i < 256; ++i) {
    need(uint64_t(p) + i < b.size(), "Connection name is outside the map");
    if (!b[p + i])
      return result;
    result += char(b[p + i]);
  }
  throw std::runtime_error("Invalid connection entity name");
}
std::map<uint32_t, uint32_t> actors(const std::vector<uint8_t> &b) {
  std::map<uint32_t, uint32_t> out;
  std::set<uint32_t> seen;
  for (auto p = u(b, 0x40); p; p = u(b, p)) {
    need(seen.insert(p).second && seen.size() < 50000,
         "Invalid connection entity chain");
    need(uint64_t(p) + 44 <= b.size(), "Short connection entity");
    if (!u(b, p + 4))
      continue;
    need(out.emplace(u(b, p + 20), p).second,
         "Duplicate entity IDs cannot be edited safely");
  }
  return out;
}
uint32_t actor(const std::map<uint32_t, uint32_t> &ids, uint32_t id) {
  auto i = ids.find(id);
  need(i != ids.end(), "Connection entity ID does not exist");
  return i->second;
}
bool editor_area(const std::vector<uint8_t> &b, uint32_t p) {
  return u(b, p + 12) == 4030 &&
         (name(b, u(b, p + 8)).starts_with("Editor Trigger ") || name(b,u(b,p+8)).starts_with("Editor Area "));
}
bool editor_button(const std::vector<uint8_t> &b,uint32_t p) {
  if(u(b,p+12)!=4180)return false;
  const auto label=name(b,u(b,p+8));
  return label.starts_with("Editor Button ") || label.starts_with("Editor Toggle ");
}
std::optional<gc::editor::EffectKind> effect_kind(const std::vector<uint8_t>& b,uint32_t p) {
  if(u(b,p+12)!=4080)return{};const auto c=u(b,p+16);if(!c||uint64_t(c)+12>b.size())return{};const auto text=u(b,c+8);
  if(!text||uint64_t(text)+gc::editor::effect_prefix.size()>b.size()||!std::equal(gc::editor::effect_prefix.begin(),gc::editor::effect_prefix.end(),b.begin()+text))return{};
  std::string encoded;for(size_t n=0;n<=gc::editor::effect_max_bytes&&uint64_t(text)+n<b.size();++n){const auto ch=b[text+n];if(!ch){gc::editor::Effect effect;std::string error;if(gc::editor::decode_effect(encoded,effect,error))return effect.kind;return{};}encoded+=char(ch);}return{};
}
uint32_t owner(const std::vector<uint8_t> &b,
               const std::map<uint32_t, uint32_t> &ids, uint32_t p) {
  if (editor_button(b,p)) {
    auto relay=actor(ids,u(b,u(b,p+16)+4));
    need(u(b,relay+12)==4080 && name(b,u(b,relay+8)).starts_with("__SSNE_LOGIC_HELPER_ROUTER_"),"Button's private output relay is missing");
    return relay;
  }
  if (!editor_area(b, p))
    return p;
  auto relay = actor(ids, u(b, u(b, p + 16) + 8));
  need(u(b, relay + 12) == 4080 && wave_helper(4080, name(b, u(b, relay + 8))),
       "Editor trigger's private wave relay is missing");
  return relay;
}
size_t config_size(uint32_t type) {
  switch (type) {
  case 4030:
    return 16;
  case 4080:
    return 128;
  case 4050:
    return 40;
  case 4180:
    return 12;
  case 4170:
    return 28;
  case 5000:
    return 24;
  case 4100:
    return 52;
  default:
    return type >= 2000 && type < 4000 ? 56 : 0;
  }
}
uint32_t mode_field(uint32_t type, unsigned slot) {
  return type == 4080 ? 0x3c + (slot - 0x14) : 8;
}
} // namespace

bool logic_target_supports(uint32_t type, LogicAction action) {
  if (action == LogicAction::Trigger)
    return type == 4080 || type == 4040 || type == 4070 || type == 4180 ||
           type == 4120 || type == 4090 || type == 4100 || type == 4130 ||
           type == 4280 || type == 4340 || type == 4230 || type == 4160;
  if (action == LogicAction::Enable || action == LogicAction::Disable)
    return type == 4030 || type == 4050 || type == 4080 || type == 4170 ||
           type == 4180 || type == 4100 || type == 4260 ||
           type == 4270 || type == 4280 || type == 4310 || type == 4200 ||
           type == 4230 || (type == 4090 && action == LogicAction::Enable);
  return false;
}
bool logic_template_type(uint32_t type) {
  return type==4090 || type==4100 || type==4120 || type==4130 ||
         type==4310 || type==4340;
}
size_t logic_template_config_size(uint32_t type) {
  switch(type){case 4090:return 24;case 4100:return 52;case 4120:return 48;case 4130:return 32;case 4310:return 56;case 4340:return 12;default:return 0;}
}
bool Document::can_logic_target(uint32_t id,LogicAction action) const {
  if(!logic_lookup_valid_){logic_lookup_=actors(data_);logic_lookup_valid_=true;}
  const auto it=logic_lookup_.find(id);
  if(it==logic_lookup_.end()||!logic_target_supports(u(data_,it->second+12),action))return false;
  const auto p=it->second;
  if(auto effect=effect_kind(data_,p);effect&&*effect==gc::editor::EffectKind::Light)return action!=LogicAction::Trigger;
  return true;
}

std::vector<LogicSlot> Document::logic_slots(uint32_t source_id) const {
  auto ids = actors(data_);
  auto original = actor(ids, source_id);
  if(auto effect=effect_kind(data_,original);effect&&*effect==gc::editor::EffectKind::Light)return {};
  if(editor_button(data_,original) && name(data_,u(data_,original+8)).starts_with("Editor Toggle ")) {
    std::vector<LogicSlot> result;const auto button=u(data_,original+16);
    for(unsigned branch=0;branch<2;++branch) {
      const auto relay=actor(ids,u(data_,button+16+branch*4)),c=u(data_,relay+16);
      need(u(data_,relay+12)==4080,"Toggle branch controller is missing");
      for(unsigned output=0;output<9;++output) {
        const auto field=20+output*4,target=u(data_,c+field),action=u(data_,c+0x3c+output*4);
        need(target==0xffffffff || action<=2,"Unsupported toggle output action");
        LogicSlot slot;slot.source=source_id;slot.owner=u(data_,relay+20);slot.slot=(branch+1)*256+field;
        slot.label=std::string(branch?"On switched off: ":"On switched on: ")+std::to_string(output+1);slot.action_editable=true;slot.action=LogicAction(action<=2?action:0);if(target!=0xffffffff)slot.target=target;result.push_back(std::move(slot));
      }
    }
    return result;
  }
  auto p = owner(data_, ids, original), type = u(data_, p + 12),
       cfg = u(data_, p + 16);
  auto size = config_size(type);
  if (!size)
    return {};
  need(cfg && uint64_t(cfg) + size <= data_.size(),
       "Invalid connection configuration");
  std::vector<LogicSlot> slots;
  auto add = [&](unsigned field, std::string label, bool action = false) {
    auto target = u(data_, cfg + field);
    auto mode = action ? u(data_, cfg + mode_field(type, field)) : 0;
    if (target != 0xffffffff && mode > 2)
      return;
    LogicSlot slot;
    slot.source = source_id;
    slot.owner = u(data_, p + 20);
    slot.slot = field;
    slot.label = std::move(label);
    slot.action_editable = action;
    if (target != 0xffffffff)
      slot.target = target;
    slot.action =
        mode <= 2 ? static_cast<LogicAction>(mode) : LogicAction::Trigger;
    slots.push_back(std::move(slot));
  };
  switch (type) {
  case 4080:
    {
    const auto label=name(data_,u(data_,original+8));
    const auto event=editor_button(data_,original)?"On pressed: ":editor_area(data_,original)?"On player entered: ":label.starts_with("Editor Timer ")?"On timer: ":label.starts_with("Editor Counter ")?"On count reached: ":"On triggered: ";
    for (unsigned i = 0; i < 10; ++i)
      if(!(i==9 && name(data_,u(data_,original+8)).starts_with("Editor Timer ") && ids.contains(u(data_,cfg+20+i*4)) && name(data_,u(data_,ids.at(u(data_,cfg+20+i*4))+8)).starts_with("__SSNE_LOGIC_HELPER_CLOCK_")))
      add(20 + 4 * i, std::string(event)+std::to_string(i + 1), true);
    }
    break;
  case 4030:
    add(8, "Player enters area");
    break;
  case 4050:
    add(16, "Accepted entry: target 1");
    add(20, "Accepted entry: target 2");
    add(32, "Key denied: target");
    break;
  case 4180:
    add(4, "Switch interaction");
    break;
  case 4170:
    add(12, "Touch field action", true);
    break;
  case 5000:
    add(8, "On enemy death");
    break;
  case 4100:
    add(8,"On sound finished");
    break;
  default:
    add(8, "On successful collection");
    break;
  }
  return slots;
}

uint32_t Document::own_logic_config(uint32_t entity, size_t minimum) {
  auto cfg = u(data_, entity + 16);
  need(cfg, "Connection configuration is missing");
  unsigned references = 0;
  for (auto [id, p] : actors(data_))
    references += u(data_, p + 16) == cfg;
  if (references <= 1)
    return cfg;
  size_t extent = minimum;
  std::set<uint32_t> targets;
  for (auto field : relocations_)
    targets.insert(u(data_, field));
  if (auto next = targets.upper_bound(cfg);
      next != targets.end() && *next - cfg >= minimum && *next - cfg <= 256)
    extent = *next - cfg;
  auto clone = copy_block(cfg, extent);
  pointer(entity + 16, clone);
  return clone;
}

void Document::set_logic_link(uint32_t source_id, unsigned slot,
                              std::optional<uint32_t> target,
                              LogicAction action) {
  need(unsigned(action) <= 2, "Unsupported connection action");
  auto slots = logic_slots(source_id);
  auto found = std::find_if(slots.begin(), slots.end(),
                            [&](auto &s) { return s.slot == slot; });
  need(found != slots.end(),
       "This connection field is read-only or unsupported");
  need(found->action_editable || action == LogicAction::Trigger,
       "This output supports Trigger only");
  if (found->target == target &&
      (!found->action_editable || found->action == action))
    return;
  auto ids = actors(data_);
  if (target) {
    need(*target <= 32767,
         "Target ID exceeds the native signed entity ID range");
    auto p = actor(ids, *target);
    need(can_logic_target(*target,action),
         "Target ignores this action. Trigger: relay/wave/moving door/switch; "
         "Activate/Deactivate: area/door controller/relay/touch field/switch. "
         "Enemies need a wave connection");
    need(*target != found->owner && *target != source_id,
         "A connection cannot target its own controller");
    if(action==LogicAction::Trigger) {
      std::set<uint32_t> visited;
      std::function<bool(uint32_t)> reaches=[&](uint32_t id) {
        if(id==source_id || id==found->owner)return true;
        if(!visited.insert(id).second || !ids.contains(id))return false;
        const auto node=ids.at(id),kind=u(data_,node+12),c=u(data_,node+16);
        if(kind==4080 && std::bit_cast<float>(u(data_,c+0x78))>0)return false;
        for(const auto& output:logic_slots(id))if(output.target && output.action==LogicAction::Trigger && reaches(*output.target))return true;
        return false;
      };
      const auto source_actor=actor(ids,found->owner),source_config=u(data_,source_actor+16);
      const bool delayed=u(data_,source_actor+12)==4080 && std::bit_cast<float>(u(data_,source_config+0x78))>0;
      need(delayed || !reaches(*target),"This creates an immediate logic loop. Add a timer/delay before making a repeating connection");
    }
  }
  auto owner_id = found->owner;
  transaction(
      [&] {
        auto current = actors(data_);
        auto p = actor(current, owner_id), type = u(data_, p + 12);
        auto cfg = own_logic_config(p, config_size(type));
        const auto field=slot&255u;
        write_word(data_, cfg + field, target.value_or(0xffffffff));
        if (found->action_editable)
          write_word(data_, cfg + mode_field(type, field), unsigned(action));
        // Editor-created area watchers remain useful for door-only output
        // chains.
        for (auto [id, area] : current)
          if (editor_area(data_, area)) {
            auto area_cfg = u(data_, area + 16);
            if (u(data_, area_cfg + 8) != owner_id)
              continue;
            bool any = false;
            for (unsigned i = 0; i < 10; ++i)
              any |= u(data_, cfg + 20 + i * 4) != 0xffffffff;
            area_cfg = own_logic_config(area, 16);
            write_word(data_, area_cfg, any ? 1 : 0);
          }
      },
      true);
}

void Document::add_logic_link(uint32_t source, uint32_t target,
                              LogicAction action) {
  auto slots = logic_slots(source);
  need(!slots.empty(), "Selected object has no editable activation output");
  need(std::none_of(
           slots.begin(), slots.end(),
           [&](auto &s) { return s.target == target && s.action == action; }),
       "This connection already exists");
  auto slot = std::find_if(slots.begin(), slots.end(),
                           [](auto &s) { return !s.target; });
  need(slot != slots.end(), "No free output slot; select an existing "
                            "connection to replace or delete it");
  set_logic_link(source, slot->slot, target, action);
}

uint32_t Document::link_wave_enemy(uint32_t source_id, uint32_t enemy_offset,
                                   unsigned count) {
  need(count >= 1 && count <= 64, "Wave count must be between 1 and 64");
  auto enemy = std::find_if(objects_.begin(), objects_.end(), [&](auto &o) {
    return o.offset == enemy_offset && o.type == 5000 && o.added;
  });
  need(enemy != objects_.end(), "Choose a newly placed enemy for the wave");
  auto slots = logic_slots(source_id);
  auto free = std::find_if(slots.begin(), slots.end(),
                           [](auto &s) { return !s.target; });
  need(free != slots.end(), "No free trigger output slot for another wave");
  auto ids = actors(data_);
  auto enemy_id = u(data_, enemy_offset + 20);
  for (auto &s : slots)
    if (s.target && ids.contains(*s.target)) {
      auto p = ids.at(*s.target);
      need(u(data_, p + 12) != 4040 ||
               u(data_, u(data_, p + 16) + 20) != enemy_id,
           "This enemy is already connected to this trigger's wave");
    }
  auto p = actor(ids, free->owner), source_type = u(data_, p + 12);
  unsigned activations = 1;
  float delay = 0;
  if (source_type == 4080) {
    auto cfg = u(data_, p + 16);
    activations = u(data_, cfg + 0x74);
    need(activations <= 4096,
         "Wave activation cap is outside the supported range");
    for (auto &s : slots)
      if (s.target && ids.contains(*s.target)) {
        auto other = ids.at(*s.target);
        if (u(data_, other + 12) == 4040 &&
            wave_helper(4040, name(data_, u(data_, other + 8)))) {
          delay = std::bit_cast<float>(u(data_, u(data_, other + 16) + 28));
          break;
        }
      }
  }
  uint32_t spawn_id = 0;
  transaction(
      [&] {
        auto cfg = append_zero(60);
        write_word(data_, cfg, std::bit_cast<uint32_t>(.25f));
        write_word(data_, cfg + 8, count);
        // Retail EnemySpawner compares spawned < total using signed words;
        // zero is not an unlimited sentinel. Keep complete quotas below the
        // signed limit for an unlimited relay, without overflowing its total.
        write_word(data_, cfg + 12,
                   activations ? count * activations
                               : (0x7fffffffu / count) * count);
        write_word(data_, cfg + 20, enemy_id);
        write_word(data_, cfg + 28, std::bit_cast<uint32_t>(delay));
        write_word(data_, cfg + 32, 3);
        write_word(data_, cfg + 36, 0xffffffff);
        write_word(data_, cfg + 48, std::bit_cast<uint32_t>(1.f));
        auto spawn = append_entity(4040, "__SSNE_WAVE_HELPER_SPAWN_",
                                   u(data_, enemy_offset + 4), cfg);
        spawn_id = u(data_, spawn + 20);
        auto receiver_cfg = own_logic_config(p, config_size(source_type));
        const auto field=free->slot&255u;
        write_word(data_, receiver_cfg + field, spawn_id);
        if (free->action_editable)
          write_word(data_, receiver_cfg + mode_field(source_type, field),
                     0);
        auto enemy_cfg = own_logic_config(enemy_offset, 24);
        write_word(data_, enemy_cfg + 4, 1);
        for (auto [id, area] : actors(data_))
          if (editor_area(data_, area)) {
            auto c = u(data_, area + 16);
            if (u(data_, c + 8) == free->owner) {
              c = own_logic_config(area, 16);
              write_word(data_, c, 1);
            }
          }
      },
      true);
  return spawn_id;
}
} // namespace mapedit
