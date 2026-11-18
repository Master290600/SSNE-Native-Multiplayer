#include "map_logic_icons.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace mapedit {
namespace {
bool contains(std::string_view text, std::string_view token) {
  if (token.size() > text.size())
    return false;
  for (size_t i = 0; i + token.size() <= text.size(); ++i) {
    bool same = true;
    for (size_t k = 0; k < token.size(); ++k) {
      char ch = text[i + k];
      if (ch >= 'A' && ch <= 'Z')
        ch += 'a' - 'A';
      if (ch != token[k]) {
        same = false;
        break;
      }
    }
    if (same)
      return true;
  }
  return false;
}
constexpr uint32_t color(unsigned r, unsigned g, unsigned b) {
  return r | (g << 8) | (b << 16) | 0xff000000u;
}
} // namespace
LogicIcon logic_icon_for(uint32_t type, std::string_view role) {
  // Roles come from the decoded native configuration, never from arbitrary
  // user labels. A renamed enemy cannot accidentally become a light icon.
  if (type == 4180)
    return contains(role, "toggle") ? LogicIcon::Toggle : LogicIcon::Button;
  if (type == 4080) {
    if (contains(role, "light")) return LogicIcon::Light;
    if (contains(role, "explosion")) return LogicIcon::Explosion;
    if (contains(role, "light"))
      return LogicIcon::Light;
    if (contains(role, "explosion"))
      return LogicIcon::Explosion;
    if (contains(role, "gate"))
      return LogicIcon::Gate;
    if (contains(role, "branch") || contains(role, "condition"))
      return LogicIcon::Branch;
    if (contains(role, "counter"))
      return LogicIcon::Counter;
    if (contains(role, "timer") || contains(role, "delay"))
      return LogicIcon::Timer;
    if (contains(role, "sequence"))
      return LogicIcon::Sequence;
    return LogicIcon::Relay;
  }
  switch (type) {
  case 4140:
    return LogicIcon::PlayerSpawn;
  case 5000:
    return LogicIcon::Enemy;
  case 4040:
    return LogicIcon::WaveSpawner;
  case 4030:
    return LogicIcon::AreaTrigger;
  case 4050:
    return LogicIcon::Lock;
  case 4070:
    return LogicIcon::Door;
  case 4170:
    return LogicIcon::TouchField;
  case 4060:
    return LogicIcon::PathMarker;
  case 4150:
    return LogicIcon::EnemyPath;
  case 4260:
    return LogicIcon::ArrowMarker;
  case 4160:
    return LogicIcon::Exit;
  case 4020:
    return LogicIcon::Teleport;
  case 4010:
    return LogicIcon::Copier;
  case 4100:
    return LogicIcon::Sound;
  case 4090:
    return LogicIcon::Music;
  case 4120:
    return LogicIcon::Camera;
  case 4110:
    return LogicIcon::DamageField;
  case 4130:
    return LogicIcon::Message;
  case 4190:
    return LogicIcon::Vehicle;
  case 4210:
  case 4240:
    return LogicIcon::Destructible;
  case 4220:
    return LogicIcon::BouncePad;
  case 4230:
    return LogicIcon::RollingStone;
  case 4270:
    return LogicIcon::Lock;
  case 4280:
    return LogicIcon::Teleport;
  case 4310:
    return LogicIcon::Particles;
  case 4320:
    return LogicIcon::Flag;
  case 4330:
    return LogicIcon::Statistics;
  case 4340:
    return LogicIcon::Movie;
  default:
    break;
  }
  if (type >= 2000 && type < 4000)
    return LogicIcon::Pickup;
  if (contains(role, "explosion"))
    return LogicIcon::Explosion;
  if (contains(role, "music"))
    return LogicIcon::Music;
  if (contains(role, "light"))
    return LogicIcon::Light;
  if (contains(role, "camera"))
    return LogicIcon::Camera;
  if (contains(role, "sound"))
    return LogicIcon::Sound;
  if (contains(role, "sequence"))
    return LogicIcon::Sequence;
  return LogicIcon::Object;
}
const char *logic_icon_name(LogicIcon kind) {
  constexpr std::array names{"Object",
                             "Player spawn",
                             "Enemy",
                             "Wave spawner",
                             "Pickup",
                             "Door",
                             "Area trigger",
                             "Relay",
                             "Counter",
                             "Timer / delay",
                             "Button",
                             "Toggle",
                             "Condition / branch",
                             "Camera",
                             "Sound",
                             "Music",
                             "Light",
                             "Explosion",
                             "Path marker",
                             "Arrow marker",
                             "Level exit",
                             "Teleport",
                             "Copier",
                             "Touch field",
                             "Sequence",
                             "Locked door / condition",
                             "Enemy route marker",
                             "Message",
                             "Damage field",
                             "Particles",
                             "Destructible model / brush",
                             "Vehicle",
                             "Bounce pad",
                             "Rolling stone",
                             "Multiplayer flag",
                             "Score / par watcher",
                             "Movie",
                             "Gate"};
  const auto n = static_cast<unsigned>(kind);
  return n < names.size() ? names[n] : "Object";
}
uint32_t logic_icon_color(LogicIcon kind) {
  switch (kind) {
  case LogicIcon::PlayerSpawn:
    return color(100, 240, 157);
  case LogicIcon::Enemy:
    return color(255, 119, 105);
  case LogicIcon::WaveSpawner:
    return color(255, 156, 91);
  case LogicIcon::Pickup:
    return color(112, 211, 255);
  case LogicIcon::Door:
  case LogicIcon::Lock:
    return color(225, 195, 118);
  case LogicIcon::AreaTrigger:
  case LogicIcon::TouchField:
    return color(111, 235, 203);
  case LogicIcon::Button:
  case LogicIcon::Toggle:
    return color(114, 209, 255);
  case LogicIcon::Relay:
  case LogicIcon::Counter:
  case LogicIcon::Timer:
  case LogicIcon::Branch:
  case LogicIcon::Sequence:
  case LogicIcon::Copier:
  case LogicIcon::Gate:
    return color(202, 160, 255);
  case LogicIcon::Camera:
    return color(155, 196, 255);
  case LogicIcon::Sound:
  case LogicIcon::Music:
    return color(245, 162, 231);
  case LogicIcon::Light:
    return color(255, 230, 118);
  case LogicIcon::Explosion:
    return color(255, 164, 97);
  case LogicIcon::DamageField:
    return color(255, 123, 110);
  case LogicIcon::Destructible:
  case LogicIcon::RollingStone:
    return color(232, 181, 121);
  case LogicIcon::Message:
    return color(168, 222, 255);
  case LogicIcon::Particles:
    return color(255, 169, 219);
  case LogicIcon::Vehicle:
  case LogicIcon::BouncePad:
    return color(139, 218, 218);
  case LogicIcon::Flag:
  case LogicIcon::Statistics:
    return color(255, 217, 122);
  case LogicIcon::Movie:
    return color(167, 190, 253);
  case LogicIcon::EnemyPath:
    return color(245, 174, 135);
  case LogicIcon::PathMarker:
  case LogicIcon::ArrowMarker:
    return color(255, 218, 106);
  case LogicIcon::Exit:
  case LogicIcon::Teleport:
    return color(121, 236, 178);
  default:
    return color(179, 193, 214);
  }
}
std::vector<size_t>
layout_logic_icons(std::span<const LogicIconCandidate> input, float width,
                   float height, float pixels) {
  std::vector<size_t> result, priority;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 ||
      height <= 0)
    return result;
  pixels = std::isfinite(pixels) ? std::clamp(pixels, 12.f, 64.f) : 22.f;
  struct Slot {
    size_t index;
    bool priority;
  };
  std::unordered_map<uint64_t, Slot> nearest;
  nearest.reserve(std::min(input.size(), size_t(50000)));
  for (size_t i = 0; i < input.size(); ++i) {
    const auto &v = input[i];
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.depth) ||
        v.x < 0 || v.y < 0 || v.x >= width || v.y >= height || v.depth < 0)
      continue;
    const auto x = uint32_t(std::min(double(v.x / pixels), double(UINT32_MAX)));
    const auto y = uint32_t(std::min(double(v.y / pixels), double(UINT32_MAX)));
    const auto key = uint64_t(x) | (uint64_t(y) << 32);
    auto [at, inserted] = nearest.try_emplace(key, Slot{i, v.priority});
    if (v.priority) {
      priority.push_back(v.object);
      at->second = {i, true};
    } else if (!inserted && !at->second.priority) {
      const auto &before = input[at->second.index];
      if (v.depth < before.depth ||
          (v.depth == before.depth && v.object < before.object))
        at->second.index = i;
    }
  }
  for (auto &[key, value] : nearest)
    if (!value.priority)
      result.push_back(input[value.index].object);
  std::sort(result.begin(), result.end());
  std::sort(priority.begin(), priority.end());
  result.insert(result.end(), priority.begin(), priority.end());
  return result;
}
} // namespace mapedit
