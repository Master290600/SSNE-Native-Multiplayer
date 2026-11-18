#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

struct ImDrawList;
struct ImVec2;
namespace mapedit {
// Semantic editor billboards. Native game meshes remain independent of these
// helpers; no icon textures, font glyphs or original game assets are required.
enum class LogicIcon {
  Object,
  PlayerSpawn,
  Enemy,
  WaveSpawner,
  Pickup,
  Door,
  AreaTrigger,
  Relay,
  Counter,
  Timer,
  Button,
  Toggle,
  Branch,
  Camera,
  Sound,
  Music,
  Light,
  Explosion,
  PathMarker,
  ArrowMarker,
  Exit,
  Teleport,
  Copier,
  TouchField,
  Sequence,
  Lock,
  EnemyPath,
  Message,
  DamageField,
  Particles,
  Destructible,
  Vehicle,
  BouncePad,
  RollingStone,
  Flag,
  Statistics,
  Movie,
  Gate,
  Count
};
LogicIcon logic_icon_for(uint32_t native_type, std::string_view role = {});
const char *logic_icon_name(LogicIcon);
uint32_t logic_icon_color(LogicIcon);
struct LogicIconCandidate {
  size_t object = 0;
  float x = 0, y = 0, depth = 0; // Viewport-local pixels; positive distance.
  bool priority = false;         // Selected / hovered / explicitly linked.
};
// Keep every priority helper and the closest ordinary helper per screen cell.
// Returns object IDs in a stable order, priority helpers last. Picking remains
// independent: hidden overlapping helpers are still available in the outliner.
std::vector<size_t> layout_logic_icons(std::span<const LogicIconCandidate>,
                                       float width, float height,
                                       float cell_pixels = 22);
// Radius is in logical screen pixels, independent of camera distance. Drawing
// is clipped by the caller's viewport. Selected/hovered outlines stay legible
// on bright terrain without replacing or hiding the underlying object model.
void draw_logic_icon(ImDrawList *, ImVec2 center, float radius, LogicIcon,
                     bool selected = false, bool hovered = false);
} // namespace mapedit
