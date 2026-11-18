#pragma once
#include "map_document.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mapedit {
// These are authored connections, not a claim that a particular live event has
// fired. Reference edges do not send a trigger and must not be counted as
// gates.
enum class LogicEdgeKind {
  Trigger,
  Enable,
  Disable,
  Spawn,
  Collected,
  DoorEnter,
  DoorLeave, // Reserved: the native DoorController does not emit a leave event.
  DoorDenied,
  Touch,
  Reference,
  Unknown
};
struct LogicNode {
  uint32_t id = 0, offset = 0, type = 0;
  int object = -1;
  std::string name, role, summary;
  std::array<float, 3> position{};
  bool helper = false, supported = false, initially_enabled = true;
  int threshold = 0, activation_limit = 0;
  float delay = 0, radius = 0;
  // Native4050 uses full dimensions, extending +X/+Y/-Z from its anchor.
  // Its predicate ignores the transform's orientation and scale.
  float box_width = 0, box_height = 0;
  unsigned wave_count = 0, total_spawn_count = 0;
  int key_index = -1;
  bool repeat_counter = false;
};
struct LogicBoxBounds {
  std::array<float,3> minimum{}, maximum{};
};
// Bounded, positive native4050 dimensions only. The supplied anchor lets the
// viewport show a pending translation without changing the authored graph.
std::optional<LogicBoxBounds>
logic_box_bounds(const LogicNode&, const std::array<float,3>& anchor);
struct LogicEdge {
  uint32_t from = 0, to = 0;
  LogicEdgeKind kind = LogicEdgeKind::Unknown;
  std::string label;
  bool missing = false;
  int slot = -1;
  bool editable = false;
};
struct LogicGraph {
  std::vector<LogicNode> nodes;
  std::vector<LogicEdge> edges;
  std::vector<std::string> warnings;
  const LogicNode *find(uint32_t id) const;
};
// Decode proven original actor configuration fields from an unpacked map.
// Includes native helper entities hidden by Document::objects(). Parsing is
// strictly read-only; unsupported/custom action code is never guessed.
LogicGraph build_logic_graph(const Document &);
const char *logic_edge_name(LogicEdgeKind);
} // namespace mapedit
