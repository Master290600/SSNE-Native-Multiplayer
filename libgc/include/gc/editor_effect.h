#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gc::editor {
enum class EffectKind { Light, Explosion };
struct Effect {
  EffectKind kind = EffectKind::Light;
  float radius = 8, damage = 40, scale = 1;
  uint32_t style = 0;
  std::array<uint8_t, 4> colour{255, 220, 170, 255};
  bool operator==(const Effect &) const = default;
};
inline constexpr std::string_view effect_prefix = "SSNE_EDITOR_EFFECT_V1:";
inline constexpr size_t effect_max_bytes = 160;
inline constexpr unsigned effect_max_lights = 8;
inline constexpr unsigned effect_max_helpers = 256;
// Stored in the existing relocated NativeTrigger cfg+8 action string. Native
// maps, project snapshots and multiplayer packages retain it inside the SSW.
bool is_effect(std::string_view);
bool valid_effect(const Effect &, std::string &error);
std::string encode_effect(const Effect &);
bool decode_effect(std::string_view, Effect &, std::string &error);
} // namespace gc::editor
