#include "map_editor.h"
#include "map_logic_icons.h"
#include "theme.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <queue>
#include <span>
#include <unordered_map>

namespace {
using J = launcher::JSONValue;
bool json_integer(const J &v, const char *key, int64_t &n) {
  auto p = v.find(key);
  return p && p->integer(n);
}
double json_real(const J &v, const char *key) {
  auto p = v.find(key);
  if (!p)
    return NAN;
  if (auto d = std::get_if<double>(&p->value))
    return *d;
  int64_t n;
  if (p->integer(n))
    return double(n);
  return NAN;
}
ImU32 edge_color(mapedit::LogicAction a) {
  return a == mapedit::LogicAction::Enable    ? IM_COL32(100, 225, 150, 230)
         : a == mapedit::LogicAction::Disable ? IM_COL32(255, 120, 115, 230)
                                              : IM_COL32(100, 185, 255, 230);
}
float segment_distance_graph(ImVec2 p, ImVec2 a, ImVec2 b) {
  const auto d = b - a;
  const auto n = d.x * d.x + d.y * d.y;
  const auto t =
      n < .01f
          ? 0.f
          : std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / n, 0.f, 1.f);
  return std::hypot(p.x - a.x - t * d.x, p.y - a.y - t * d.y);
}
ImVec2 curve_point(ImVec2 a, ImVec2 b, float t) {
  const auto bend = std::max(45.f, std::abs(b.x - a.x) * .5f);
  const auto c = a + ImVec2(bend, 0), d = b - ImVec2(bend, 0);
  const float u = 1 - t;
  return a * (u * u * u) + c * (3 * u * u * t) + d * (3 * u * t * t) +
         b * (t * t * t);
}
float curve_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
  float result = 1e9f;
  auto last = a;
  for (unsigned n = 1; n <= 20; ++n) {
    auto at = curve_point(a, b, float(n) / 20);
    result = std::min(result, segment_distance_graph(p, last, at));
    last = at;
  }
  return result;
}
void curve(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float width) {
  const auto bend = std::max(45.f, std::abs(b.x - a.x) * .5f);
  dl->AddBezierCubic(a, a + ImVec2(bend, 0), b - ImVec2(bend, 0), b, color,
                     width);
}
bool inside(ImVec2 p, ImVec2 a, ImVec2 b) {
  return p.x >= a.x && p.y >= a.y && p.x <= b.x && p.y <= b.y;
}
bool graph_control(uint32_t t) {
  return t >= 4010 && t <= 4340 && t != 4060 && t != 4140 && t != 4150 &&
         t != 4200 && t != 4260 && t != 4300;
}
} // namespace
void MapEditor::load_graph_layout() {
  const auto key = project_.folder.empty() ? level_ : project_.folder.string();
  if (graph_project_ == key)
    return;
  graph_project_ = key;
  graph_positions_.clear();
  graph_slots_.clear();
  graph_slots_revision_ = UINT64_MAX;
  graph_selection_ = graph_link_from_ = graph_edge_source_ = UINT32_MAX;
  graph_edge_slot_ = -1;
  graph_pan_ = {40, 60};
  graph_zoom_ = 1;
  if (project_.folder.empty())
    return;
  try {
    std::ifstream in(project_.folder / "editor-logic-layout.json",
                     std::ios::binary | std::ios::ate);
    if (!in || in.tellg() > 256 * 1024 || in.tellg() < 2)
      return;
    std::string text(size_t(in.tellg()), '\0');
    in.seekg(0);
    in.read(text.data(), text.size());
    J value;
    std::string error;
    if (!launcher::parse_json(text, value, error))
      return;
    auto nodes = value.find("nodes");
    if (!nodes || !nodes->array() || nodes->array()->size() > 4096)
      return;
    for (const auto &n : *nodes->array()) {
      int64_t id;
      const double x = json_real(n, "x"), y = json_real(n, "y");
      if (json_integer(n, "id", id) && id >= 0 && id < UINT32_MAX &&
          std::isfinite(x) && std::isfinite(y) && std::abs(x) < 100000 &&
          std::abs(y) < 100000)
        graph_positions_.emplace(uint32_t(id), ImVec2(float(x), float(y)));
    }
  } catch (...) {
  }
}
void MapEditor::save_graph_layout() {
  if (project_.folder.empty())
    return;
  graph_project_ = project_.folder.string();
  J::Array nodes;
  for (const auto &[id, p] : graph_positions_)
    if (logic_graph_.find(id))
      nodes.emplace_back(J::Object{
          {"id", int64_t(id)}, {"x", double(p.x)}, {"y", double(p.y)}});
  const auto text = launcher::write_json(
      J::Object{{"schema", 1}, {"nodes", std::move(nodes)}});
  mapedit::atomic_write(project_.folder / "editor-logic-layout.json",
                        std::vector<uint8_t>(text.begin(), text.end()));
}
void MapEditor::graph_auto_layout() {
  std::unordered_map<uint32_t, unsigned> incoming, depth;
  std::unordered_map<uint32_t, std::vector<uint32_t>> outputs;
  for (const auto &n : logic_graph_.nodes)
    incoming[n.id] = 0;
  for (const auto &e : logic_graph_.edges)
    if (!e.missing && e.kind != mapedit::LogicEdgeKind::Reference) {
      outputs[e.from].push_back(e.to);
      ++incoming[e.to];
    }
  std::queue<uint32_t> ready;
  for (const auto &n : logic_graph_.nodes)
    if (!incoming[n.id]) {
      ready.push(n.id);
      depth[n.id] = 0;
    }
  while (!ready.empty()) {
    const auto id = ready.front();
    ready.pop();
    for (const auto target : outputs[id]) {
      depth[target] = std::min(8u, std::max(depth[target], depth[id] + 1));
      if (--incoming[target] == 0)
        ready.push(target);
    }
  }
  std::set<uint32_t> connected;
  for (const auto &e : logic_graph_.edges)
    if (!e.missing) {
      connected.insert(e.from);
      connected.insert(e.to);
    }
  std::array<float, 10> rows{};
  for (const auto &n : logic_graph_.nodes) {
    if (n.helper && n.type == 4080)
      continue;
    if (!graph_control(n.type) && !connected.contains(n.id) &&
        n.id != logic_selected_)
      continue;
    const auto column = std::min(9u, depth.contains(n.id) ? depth[n.id] : 9u);
    graph_positions_[n.id] = ImVec2(float(column) * 310.f, rows[column]);
    rows[column] +=
        120.f + float(std::max<size_t>(1, graph_slots_[n.id].size())) * 22.f;
  }
  graph_pan_ = {40, 60};
  graph_zoom_ = 1;
}
void MapEditor::graph_panel() {
  if (!graph_open_)
    return;
  load_graph_layout();
  const auto viewport = ImGui::GetMainViewport();
  const ImVec2 available(std::max(300.f, viewport->WorkSize.x - 32.f),
                         std::max(240.f, viewport->WorkSize.y - 32.f));
  ImGui::SetNextWindowPos(viewport->WorkPos + ImVec2(16, 16),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(
      ImVec2(std::min(1040.f, available.x), std::min(680.f, available.y)),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(300, 240), available);
  ImGui::SetNextWindowBgAlpha(1.f);
  if (!ImGui::Begin("Logic Graph", &graph_open_)) {
    ImGui::End();
    return;
  }
  if (!loaded()) {
    ImGui::TextWrapped("Open a map to view its logic graph.");
    ImGui::End();
    return;
  }
  std::optional<std::array<uint32_t, 3>> connect;
  if (graph_slots_revision_ != doc_.structure_revision()) {
    graph_slots_.clear();
    for (const auto &n : logic_graph_.nodes)
      try {
        graph_slots_.emplace(n.id, doc_.logic_slots(n.id));
      } catch (const std::exception &e) {
        graph_slots_.emplace(n.id, std::vector<mapedit::LogicSlot>{});
        message_ = std::string(
                       "A protected or invalid logic node cannot be edited: ") +
                   e.what();
      }
    graph_slots_revision_ = doc_.structure_revision();
  }
  if (graph_positions_.empty()) {
    graph_auto_layout();
    if (graph_positions_.contains(logic_selected_))
      graph_pan_ =
          ImVec2(160, 90) - graph_positions_[logic_selected_] * graph_zoom_;
  }
  if (button("Close", "graph_close")) {
    graph_open_ = false;
    ImGui::End();
    return;
  }
  ImGui::SameLine();
  if (button("Arrange nodes", "graph_arrange")) {
    graph_auto_layout();
    attempt([&] { save_graph_layout(); });
  }
  ImGui::SameLine();
  ImGui::Checkbox("Whole map", &graph_show_all_);
  hit("graph_all");
  ImGui::SameLine();
  if (button("Frame selection", "graph_focus",
             logic_selected_ != UINT32_MAX &&
                 graph_positions_.contains(logic_selected_)))
    graph_pan_ =
        ImVec2(300, 180) - graph_positions_[logic_selected_] * graph_zoom_;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110);
  const char *actions[]{"Trigger", "Enable", "Disable"};
  if (ImGui::BeginCombo("Wire action",
                        actions[std::clamp(graph_action_, 0, 2)])) {
    for (int i = 0; i < 3; ++i) {
      if (ImGui::Selectable(actions[i], graph_action_ == i))
        graph_action_ = i;
      hit(("graph_action_" + std::to_string(i)).c_str());
    }
    ImGui::EndCombo();
  }
  hit("graph_action");
  ImGui::TextDisabled(
      "Drag an output pin to an input. Click a wire to edit/remove. Middle "
      "drag: pan | wheel: zoom | Esc: cancel wire.");
  // Keep the canvas and its pins stationary when a wire drag begins.
  const float connection_top = ImGui::GetCursorPosY();
  if (graph_link_from_ != UINT32_MAX) {
    ImGui::TextColored(theme::kGold,
                       "Connecting #%u - pick an input pin or choose a target",
                       graph_link_from_);
    ImGui::SetNextItemWidth(250);
    if (ImGui::BeginCombo("Target", "Choose compatible target...")) {
      ImGui::InputTextWithHint("##graph_target_search", "Search nodes...",
                               graph_target_search_,
                               sizeof(graph_target_search_));
      hit("graph_target_search");
      std::string query = graph_target_search_;
      for (auto &c : query)
        c = char(std::tolower(static_cast<unsigned char>(c)));
      for (const auto &n : logic_graph_.nodes) {
        if ((n.helper && n.type != 4040) ||
            !doc_.can_logic_target(n.id, mapedit::LogicAction(graph_action_)))
          continue;
        const auto label = (n.helper && n.type == 4040
                                ? "Enemy wave x" + std::to_string(n.wave_count)
                            : n.name.empty() ? n.role
                                             : n.name) +
                           " #" + std::to_string(n.id);
        auto folded = label;
        for (auto &c : folded)
          c = char(std::tolower(static_cast<unsigned char>(c)));
        if (!query.empty() && folded.find(query) == std::string::npos)
          continue;
        if (ImGui::Selectable(label.c_str()))
          connect = std::array<uint32_t, 3>{graph_link_from_,
                                            uint32_t(graph_link_slot_), n.id};
        hit(("graph_target_" + std::to_string(n.id)).c_str());
      }
      ImGui::EndCombo();
    }
    hit("graph_target");
  }
  if (graph_edge_source_ != UINT32_MAX && graph_edge_slot_ >= 0) {
    const auto slots = graph_slots_[graph_edge_source_];
    auto slot = std::find_if(slots.begin(), slots.end(), [&](const auto &s) {
      return int(s.slot) == graph_edge_slot_;
    });
    if (slot == slots.end() || !slot->target) {
      graph_edge_source_ = UINT32_MAX;
      graph_edge_slot_ = -1;
    } else {
      ImGui::Text("Selected connection: #%u %s -> #%u", graph_edge_source_,
                  slot->label.c_str(), *slot->target);
      ImGui::SameLine();
      if (button("Apply action", "graph_apply_action", slot->action_editable))
        attempt([&] {
          doc_.set_logic_link(graph_edge_source_, unsigned(graph_edge_slot_),
                              slot->target,
                              mapedit::LogicAction(graph_action_));
          refresh_logic();
        });
      ImGui::SameLine();
      if (button("Delete connection", "graph_delete_wire"))
        attempt([&] {
          doc_.set_logic_link(graph_edge_source_, unsigned(graph_edge_slot_),
                              {}, slot->action);
          graph_edge_source_ = UINT32_MAX;
          graph_edge_slot_ = -1;
          refresh_logic();
        });
    }
  }
  ImGui::SetCursorPosY(connection_top + ImGui::GetTextLineHeightWithSpacing() +
                       2 * ImGui::GetFrameHeightWithSpacing());
  const auto origin = ImGui::GetCursorScreenPos();
  auto size = ImGui::GetContentRegionAvail();
  size.x = std::max(size.x, 80.f);
  size.y = std::max(size.y, 100.f);
  ImGui::InvisibleButton("##logic_canvas", size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle |
                             ImGuiButtonFlags_MouseButtonRight);
  // A release belongs to the wire gesture only if this canvas owned the
  // preceding press. Toolbar/combo releases must keep a pending output alive;
  // a real canvas drag released outside still deactivates the canvas.
  const bool canvas_left_released =
      ImGui::IsItemDeactivated() &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  hit("graph_canvas");
  const bool hovered = ImGui::IsItemHovered();
  auto &io = ImGui::GetIO();
  auto dl = ImGui::GetWindowDrawList();
  if (hovered && io.MouseWheel) {
    const auto old = graph_zoom_;
    graph_zoom_ = std::clamp(old * std::pow(1.15f, io.MouseWheel), .25f, 2.f);
    graph_pan_ = (io.MousePos - origin) -
                 (io.MousePos - origin - graph_pan_) * (graph_zoom_ / old);
  }
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    graph_pan_ += io.MouseDelta;
  if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape))
    graph_link_from_ = UINT32_MAX;
  const auto screen = [&](ImVec2 p) {
    return origin + graph_pan_ + p * graph_zoom_;
  };
  struct Box {
    const mapedit::LogicNode *node;
    ImVec2 p, size, input;
    std::span<const mapedit::LogicSlot> slots;
  };
  std::vector<Box> boxes;
  std::unordered_map<uint32_t, size_t> by_id;
  boxes.reserve(graph_positions_.size());
  by_id.reserve(graph_positions_.size());
  const auto neighbors = logic_neighborhood();
  const auto selected_node = logic_graph_.find(logic_selected_);
  const bool limit = !graph_show_all_ && selected_node &&
                     graph_control(selected_node->type) && neighbors.size() > 1;
  std::set<uint32_t> connected;
  for (const auto &e : logic_graph_.edges)
    if (!e.missing) {
      connected.insert(e.from);
      connected.insert(e.to);
    }
  for (const auto &n : logic_graph_.nodes) {
    if (n.helper && n.type == 4080)
      continue;
    if (!graph_control(n.type) && !connected.contains(n.id) &&
        n.id != logic_selected_)
      continue;
    if (limit && !neighbors.contains(n.id))
      continue;
    if (!graph_positions_.contains(n.id))
      graph_positions_[n.id] =
          ImVec2(float(graph_positions_.size() % 8) * 310.f,
                 float(graph_positions_.size() / 8) * 330.f);
    const auto &slots = graph_slots_[n.id];
    const auto p = screen(graph_positions_[n.id]);
    const auto dimensions =
        ImVec2(235, 82 + float(std::max<size_t>(1, slots.size())) * 22) *
        graph_zoom_;
    by_id[n.id] = boxes.size();
    boxes.push_back(
        {&n, p, dimensions, p + ImVec2(0, 67) * graph_zoom_, slots});
  }
  dl->PushClipRect(origin, origin + size, true);
  dl->AddRectFilled(origin, origin + size, IM_COL32(10, 14, 25, 255));
  const float grid = 32 * graph_zoom_;
  for (float x = std::fmod(graph_pan_.x, grid); x < size.x; x += grid)
    dl->AddLine(origin + ImVec2(x, 0), origin + ImVec2(x, size.y),
                IM_COL32(60, 70, 90, 65));
  for (float y = std::fmod(graph_pan_.y, grid); y < size.y; y += grid)
    dl->AddLine(origin + ImVec2(0, y), origin + ImVec2(size.x, y),
                IM_COL32(60, 70, 90, 65));
  uint32_t hovered_edge = UINT32_MAX;
  int hovered_slot = -1;
  float edge_nearest = 7;
  for (const auto &box : boxes)
    for (const auto &slot : box.slots)
      if (slot.target && by_id.contains(*slot.target)) {
        const auto row = size_t(&slot - box.slots.data());
        const auto a = box.p + ImVec2(235, 67 + float(row) * 22) * graph_zoom_;
        const auto b = boxes[by_id[*slot.target]].input;
        if (std::max(a.y, b.y) < origin.y ||
            std::min(a.y, b.y) > origin.y + size.y ||
            std::max(a.x, b.x) + 60 < origin.x ||
            std::min(a.x, b.x) - 60 > origin.x + size.x)
          continue;
        const auto distance = curve_distance(io.MousePos, a, b);
        const bool selected = graph_edge_source_ == box.node->id &&
                              graph_edge_slot_ == int(slot.slot);
        curve(dl, a, b,
              selected ? IM_COL32(255, 210, 70, 255) : edge_color(slot.action),
              selected ? 3 : 2);
        if (hovered && distance < edge_nearest) {
          edge_nearest = distance;
          hovered_edge = box.node->id;
          hovered_slot = int(slot.slot);
        }
      }
  uint32_t input_node = UINT32_MAX, output_node = UINT32_MAX,
           body_node = UINT32_MAX;
  int output_slot = -1;
  float nearest_input = 10.f, nearest_output = 10.f;
  for (const auto &box : boxes) {
    if (box.p.x + box.size.x < origin.x || box.p.y + box.size.y < origin.y ||
        box.p.x > origin.x + size.x || box.p.y > origin.y + size.y)
      continue;
    const auto &n = *box.node;
    const bool selected = n.id == logic_selected_ || n.id == graph_selection_;
    auto color =
        mapedit::logic_icon_color(mapedit::logic_icon_for(n.type, n.role));
    dl->AddRectFilled(box.p, box.p + box.size, IM_COL32(28, 34, 48, 250), 7);
    dl->AddRectFilled(box.p, box.p + ImVec2(box.size.x, 39 * graph_zoom_),
                      color, 7, ImDrawFlags_RoundCornersTop);
    dl->AddRect(box.p, box.p + box.size,
                selected ? IM_COL32(255, 215, 70, 255)
                         : IM_COL32(100, 115, 140, 240),
                7, 0, selected ? 2.5f : 1);
    mapedit::draw_logic_icon(dl, box.p + ImVec2(17, 19) * graph_zoom_,
                             8 * graph_zoom_,
                             mapedit::logic_icon_for(n.type, n.role));
    const auto name = n.helper && n.type == 4040
                          ? "Enemy wave x" + std::to_string(n.wave_count)
                      : n.name.empty() ? n.role
                                       : n.name;
    dl->PushClipRect(box.p + ImVec2(30, 0) * graph_zoom_,
                     box.p + ImVec2(230, 40) * graph_zoom_, true);
    dl->AddText(ImGui::GetFont(), 13 * graph_zoom_,
                box.p + ImVec2(32, 10) * graph_zoom_,
                IM_COL32(255, 255, 255, 255), name.c_str());
    dl->PopClipRect();
    const auto label = n.role + " #" + std::to_string(n.id);
    dl->AddText(ImGui::GetFont(), 11 * graph_zoom_,
                box.p + ImVec2(9, 44) * graph_zoom_,
                IM_COL32(180, 195, 220, 255), label.c_str());
    const bool compatible =
        graph_link_from_ == UINT32_MAX ||
        doc_.can_logic_target(n.id, mapedit::LogicAction(graph_action_));
    dl->AddCircleFilled(box.input, 5 * graph_zoom_,
                        compatible ? IM_COL32(220, 230, 245, 255)
                                   : IM_COL32(90, 95, 110, 255));
    dl->AddText(ImGui::GetFont(), 11 * graph_zoom_,
                box.input + ImVec2(9, -7) * graph_zoom_,
                IM_COL32(205, 215, 235, 255), "Input");
    if (inside(box.input, origin + ImVec2(10, 10),
               origin + size - ImVec2(10, 10)))
      hits_.push_back({"graph_input_" + std::to_string(n.id),
                       box.input - ImVec2(9, 9), box.input + ImVec2(9, 9)});
    const float input_distance = std::hypot(io.MousePos.x - box.input.x,
                                            io.MousePos.y - box.input.y);
    if (hovered && std::isfinite(input_distance) && input_distance < 10.f &&
        (input_distance < nearest_input ||
         (input_distance == nearest_input && n.id < input_node))) {
      input_node = n.id;
      nearest_input = input_distance;
    }
    for (size_t i = 0; i < box.slots.size(); ++i) {
      const auto &slot = box.slots[i];
      const auto p = box.p + ImVec2(235, 67 + float(i) * 22) * graph_zoom_;
      dl->AddCircleFilled(p, 5 * graph_zoom_,
                          slot.target ? edge_color(slot.action)
                                      : IM_COL32(135, 150, 175, 255));
      if (inside(p, origin + ImVec2(10, 10), origin + size - ImVec2(10, 10)))
        hits_.push_back({"graph_output_" + std::to_string(n.id) + "_" +
                             std::to_string(slot.slot),
                         p - ImVec2(9, 9), p + ImVec2(9, 9)});
      const auto text = slot.label.empty()
                            ? "Output " + std::to_string(slot.slot + 1)
                            : slot.label;
      dl->PushClipRect(box.p + ImVec2(65, 60) * graph_zoom_,
                       box.p + box.size - ImVec2(8, 0), true);
      dl->AddText(ImGui::GetFont(), 11 * graph_zoom_,
                  p - ImVec2(125, 7) * graph_zoom_,
                  IM_COL32(210, 220, 240, 255), text.c_str());
      dl->PopClipRect();
      const float output_distance =
          std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y);
      // At small zoom levels adjacent pins share the hit radius. Select the
      // closest center, with stable identity ties, rather than the last row.
      if (hovered && std::isfinite(output_distance) && output_distance < 10.f &&
          (output_distance < nearest_output ||
           (output_distance == nearest_output &&
            (n.id < output_node ||
             (n.id == output_node && int(slot.slot) < output_slot))))) {
        output_node = n.id;
        output_slot = int(slot.slot);
        nearest_output = output_distance;
      }
    }
    if (hovered && inside(io.MousePos, box.p,
                          box.p + ImVec2(box.size.x, 39 * graph_zoom_)))
      body_node = n.id;
    if (hovered && inside(io.MousePos, box.p, box.p + box.size) &&
        io.MousePos.y > box.p.y + 39 * graph_zoom_) {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(n.summary.c_str());
      if (const auto live = live_logic_node(n.id)) {
        if (auto status = live->find("status"); status && status->string())
          ImGui::Text("Observed: %s", status->string()->c_str());
      }
      ImGui::EndTooltip();
    }
  }
  if (graph_link_from_ != UINT32_MAX && by_id.contains(graph_link_from_)) {
    const auto &box = boxes[by_id[graph_link_from_]];
    const auto found =
        std::find_if(box.slots.begin(), box.slots.end(), [&](const auto &s) {
          return int(s.slot) == graph_link_slot_;
        });
    if (found != box.slots.end())
      curve(dl,
            box.p + ImVec2(235, 67 + float(found - box.slots.begin()) * 22) *
                        graph_zoom_,
            io.MousePos, IM_COL32(255, 215, 70, 255), 2.5f);
  }
  dl->PopClipRect();
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (input_node != UINT32_MAX && graph_link_from_ != UINT32_MAX)
      connect = std::array<uint32_t, 3>{graph_link_from_,
                                        uint32_t(graph_link_slot_), input_node};
    else if (output_node != UINT32_MAX) {
      graph_link_from_ = output_node;
      graph_link_slot_ = output_slot;
      graph_link_mouse_ = io.MousePos;
    } else if (body_node != UINT32_MAX) {
      graph_selection_ = body_node;
      select_logic(body_node, false);
      graph_drag_node_ = body_node;
      graph_drag_start_ = io.MousePos;
      graph_drag_position_ = graph_positions_[body_node];
      graph_dragging_ = true;
    } else if (hovered_edge != UINT32_MAX) {
      graph_edge_source_ = hovered_edge;
      graph_edge_slot_ = hovered_slot;
      for (const auto &s : graph_slots_[hovered_edge])
        if (int(s.slot) == hovered_slot)
          graph_action_ = int(s.action);
    } else
      graph_link_from_ = UINT32_MAX;
  }
  if (graph_dragging_) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
      graph_positions_[graph_drag_node_] =
          graph_drag_position_ +
          (io.MousePos - graph_drag_start_) / graph_zoom_;
    else {
      graph_dragging_ = false;
      attempt([&] { save_graph_layout(); });
    }
  }
  if (canvas_left_released && graph_link_from_ != UINT32_MAX) {
    if (input_node != UINT32_MAX)
      connect = std::array<uint32_t, 3>{graph_link_from_,
                                        uint32_t(graph_link_slot_), input_node};
    else if (std::hypot(io.MousePos.x - graph_link_mouse_.x,
                        io.MousePos.y - graph_link_mouse_.y) > 4)
      graph_link_from_ = UINT32_MAX;
  }
  ImGui::End();
  if (connect) {
    attempt([&] {
      doc_.set_logic_link((*connect)[0], (*connect)[1], (*connect)[2],
                          mapedit::LogicAction(graph_action_));
      refresh_logic();
    });
    graph_link_from_ = UINT32_MAX;
  }
}
launcher::JSONValue MapEditor::graph_test_state() const {
  J::Array nodes;
  for (const auto &[id, p] : graph_positions_)
    nodes.emplace_back(
        J::Object{{"id", int64_t(id)}, {"x", double(p.x)}, {"y", double(p.y)}});
  J::Array slots;
  if (logic_selected_ != UINT32_MAX)
    try {
      for (const auto &s : doc_.logic_slots(logic_selected_))
        slots.emplace_back(
            J::Object{{"slot", int64_t(s.slot)},
                      {"label", s.label},
                      {"target", s.target ? J(int64_t(*s.target)) : J()},
                      {"action", int64_t(s.action)},
                      {"action_editable", s.action_editable}});
    } catch (...) {
    }
  return J::Object{
      {"open", graph_open_},
      {"action", graph_action_},
      {"selected_slots", std::move(slots)},
      {"zoom", double(graph_zoom_)},
      {"pan", J::Array{double(graph_pan_.x), double(graph_pan_.y)}},
      {"pending_source", int64_t(graph_link_from_)},
      {"pending_slot", graph_link_slot_},
      {"selected_wire_source", int64_t(graph_edge_source_)},
      {"selected_wire_slot", graph_edge_slot_},
      {"positions", std::move(nodes)}};
}
