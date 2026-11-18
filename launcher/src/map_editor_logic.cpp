#include "map_editor.h"
#include "theme.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace {
using J = launcher::JSONValue;
std::string text(const J &v, const char *key) {
  auto p = v.find(key);
  return p && p->string() ? *p->string() : std::string{};
}
int64_t number(const J &v, const char *key, int64_t fallback = -1) {
  int64_t out;
  auto p = v.find(key);
  return p && p->integer(out) ? out : fallback;
}
bool flag(const J &v, const char *key) {
  bool out = false;
  auto p = v.find(key);
  return p && p->boolean(out) && out;
}
std::string lower(std::string s) {
  for (char &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string display_name(const mapedit::LogicNode &node) {
  if (node.helper)
    return std::string(node.name.starts_with("__SSNE_LOGIC_HELPER_")?"Logic controller #":node.type == 4080 ? "Wave controller #"
                                         : "Wave spawn point #") +
           std::to_string(node.id);
  return node.name.empty() ? node.role + " #" + std::to_string(node.id)
                           : node.name;
}
ImU32 state_color(const J *sample) {
  if (!sample || !flag(*sample, "observed"))
    return IM_COL32(145, 160, 185, 220);
  auto s = lower(text(*sample, "status"));
  if (s.find("unknown") != std::string::npos ||
      s.find("unsupported") != std::string::npos)
    return IM_COL32(145, 160, 185, 220);
  if (s.find("locked") != std::string::npos ||
      s.find("closed") != std::string::npos ||
      s.find("disabled") != std::string::npos)
    return IM_COL32(255, 120, 100, 235);
  if (s.find("complete") != std::string::npos ||
      s.find("exhaust") != std::string::npos || s == "open" ||
      s == "triggered" || s == "finished")
    return IM_COL32(105, 235, 145, 235);
  return IM_COL32(255, 200, 70, 235);
}
float distance(ImVec2 p, ImVec2 a, ImVec2 b) {
  float dx = b.x - a.x, dy = b.y - a.y, n = dx * dx + dy * dy;
  if (n < .001f)
    return std::hypot(p.x - a.x, p.y - a.y);
  float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / n, 0.f, 1.f);
  return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}
std::string scalar(const J &v) {
  if (auto d = std::get_if<double>(&v.value)) {
    char s[48];
    std::snprintf(s, sizeof(s), "%.3f", *d);
    return s;
  }
  if (v.string())
    return *v.string();
  return launcher::write_json(v);
}
} // namespace

void MapEditor::refresh_logic() {
  if (!loaded()) {
    logic_graph_ = {};
    logic_revision_ = std::numeric_limits<uint64_t>::max();
    return;
  }
  if (logic_revision_ == doc_.structure_revision())
    return;
  attempt([&] {
    logic_graph_ = mapedit::build_logic_graph(doc_);
    logic_revision_ = doc_.structure_revision();
  });
}
mapedit::V3 MapEditor::logic_position(const mapedit::LogicNode &node) const {
  if (node.object >= 0 && size_t(node.object) < doc_.objects().size()) {
    if (drag3d_ && node.object == selected_)
      return drag3d_preview_;
    auto m = doc_.matrix(size_t(node.object));
    return {m[3], m[7], m[11]};
  }
  const auto &b = doc_.payload();
  auto word = [&](size_t p) {
    return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
           uint32_t(b[p + 2]) << 8 | b[p + 3];
  };
  if (uint64_t(node.offset) + 8 <= b.size()) {
    auto m = word(node.offset + 4);
    if (m && uint64_t(m) + 48 <= b.size())
      return {std::bit_cast<float>(word(m + 12)),
              std::bit_cast<float>(word(m + 28)),
              std::bit_cast<float>(word(m + 44))};
  }
  return {node.position[0], node.position[1], node.position[2]};
}
void MapEditor::select_logic(uint32_t id, bool focus) {
  const auto *node = logic_graph_.find(id);
  if (!node)
    return;
  if (node->object >= 0)
    select(node->object);
  logic_selected_ = id;
  if (focus) {
    auto p = logic_position(*node);
    camera_.focus(p, 30);
    centre_x_ = p.x;
    centre_z_ = p.z;
  }
}
std::set<uint32_t> MapEditor::logic_neighborhood() const {
  std::set<uint32_t> nodes;
  if (!logic_follow_) {
    for (auto &n : logic_graph_.nodes)
      nodes.insert(n.id);
    return nodes;
  }
  if (logic_selected_ == UINT32_MAX)
    return nodes;
  nodes.insert(logic_selected_);
  std::set<uint32_t> frontier{logic_selected_};
  for (unsigned step = 0; step < 4 && !frontier.empty() && nodes.size() < 1000;
       ++step) {
    std::set<uint32_t> next;
    for (auto &e : logic_graph_.edges) {
      std::optional<uint32_t> add;
      if (frontier.contains(e.from))
        add = e.to;
      else if (frontier.contains(e.to))
        add = e.from;
      if (add && !nodes.contains(*add))
        next.insert(*add);
    }
    nodes.insert(next.begin(), next.end());
    frontier = std::move(next);
  }
  return nodes;
}
void MapEditor::poll_logic() {
  auto now = std::chrono::steady_clock::now();
  logic_fresh_ =
      logic_sequence_ >= 0 && now - logic_received_ < std::chrono::seconds(3);
  if (logic_file_.empty() || now - logic_poll_ < std::chrono::milliseconds(250))
    return;
  logic_poll_ = now;
  std::error_code ec;
  auto size =
      std::filesystem::file_size(std::filesystem::u8path(logic_file_), ec);
  if (ec || size > 4 * 1024 * 1024)
    return;
  std::ifstream f(std::filesystem::u8path(logic_file_), std::ios::binary);
  if (!f)
    return;
  std::string bytes(std::istreambuf_iterator<char>(f), {});
  J next;
  std::string error;
  if (!launcher::parse_json(bytes, next, error,
                            {4 * 1024 * 1024, 65536, 100000, 20})) {
    logic_error_ =
        "Diagnostic snapshot is incomplete; waiting for the next sample.";
    return;
  }
  if (number(next, "schema") != 1 || text(next, "token") != logic_token_) {
    logic_error_ = "Ignored diagnostics from another test session.";
    logic_fresh_ = false;
    logic_stream_ready_ = false;
    return;
  }
  auto sequence = number(next, "sequence");
  if (sequence < 0) {
    logic_error_ = "Diagnostic sequence is unavailable.";
    return;
  }
  if (sequence != logic_sequence_) {
    logic_sequence_ = sequence;
    logic_received_ = now;
    logic_fresh_ = true;
  }
  logic_stream_ready_ = flag(next, "ready");
  logic_error_ = text(next, "error");
  // Preserve the last playable sample when the test returns to a menu or
  // changes levels. Freshness and stream readiness still gate LIVE results.
  if (logic_stream_ready_ || !flag(logic_live_, "ready"))
    logic_live_ = std::move(next);
}
bool MapEditor::logic_sample_current() const {
  return logic_fresh_ && logic_stream_ready_ && flag(logic_live_, "ready") &&
         text(logic_live_, "level") == level_ &&
         project_.folder.string() == logic_project_ && !doc_.changed() &&
         saved_export_ == logic_export_;
}
const J *MapEditor::live_logic_node(uint32_t id) const {
  if (!logic_sample_current())
    return nullptr;
  return recorded_logic_node(id);
}
const J *MapEditor::recorded_logic_node(uint32_t id) const {
  if (!flag(logic_live_, "ready") || text(logic_live_, "level") != level_ ||
      project_.folder.string() != logic_project_ || doc_.changed() ||
      saved_export_ != logic_export_)
    return nullptr;
  auto nodes = logic_live_.find("nodes");
  if (!nodes || !nodes->array())
    return nullptr;
  for (auto &node : *nodes->array())
    if (number(node, "id") == id)
      return &node;
  return nullptr;
}
void MapEditor::logic_toolbar() {
  if(button("Add logic...","add_logic",loaded()&&!test_future_.valid()&&!drag3d_&&!flight_active_)) {
    logic_create_type_=0;logic_create_draft_={};logic_create_draft_.kind=mapedit::LogicKind::Area;logic_create_draft_.delay=0;std::snprintf(logic_create_name_,sizeof(logic_create_name_),"Area");ImGui::OpenPopup("Create logic");
  }
  if(ImGui::IsItemHovered())ImGui::SetTooltip("Create native triggers, gates, counters, timers and interactive world buttons. Connect named outputs in the Logic graph.");
  ImGui::SameLine();
  ImGui::Checkbox("Connections", &connections_);
  hit("connections");
  ImGui::SameLine();
  if (button("Logic / Test", "logic_panel", loaded()))
    logic_window_ = !logic_window_;
  ImGui::SameLine();
  if (button("Test logic", "logic_test",
             loaded() && !test_future_.valid() && !drag3d_ && !flight_active_))
    attempt([&] { test(false, true); });
  ImGui::SameLine();
  if (ImGui::Checkbox("Edit links", &link_mode_)) {
    drag3d_ = dragging_ = false;
    placement_ = -1;
    link_pick_ = link_mode_ ? 1 : 0;
    if (link_mode_) {
      connections_ = true;
      logic_window_ = false;
    }
  }
  hit("edit_links");
  if (link_mode_ && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    link_pick_ = 0;
  if (link_mode_ && link_slot_ >= 0 && !ImGui::GetIO().WantTextInput &&
      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
      ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    apply_link(true);
  if (!logic_file_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(
        ImGui::ColorConvertU32ToFloat4(logic_sample_current()
                                           ? IM_COL32(105, 235, 145, 255)
                                           : IM_COL32(170, 180, 200, 255)),
        "%s", logic_sample_current() ? "LIVE" : "Test not live");
  }
}
bool MapEditor::draw_logic_connections(ImDrawList *dl, ImVec2 p, ImVec2 size,
                                       bool hovered) {
  logic_drawn_ = 0;
  if (!connections_)
    return false;
  auto project = [&](mapedit::V3 point) -> std::optional<mapedit::Projected> {
    if (view3d_)
      return camera_.project(point);
    return mapedit::Projected{size.x * .5f + (point.x - centre_x_) * zoom_,
                              size.y * .5f - (point.z - centre_z_) * zoom_, 1};
  };
  auto allowed = logic_neighborhood();
  auto mouse = ImGui::GetIO().MousePos;
  const mapedit::LogicEdge *hover = nullptr;
  size_t hover_index = 0;
  float nearest = 6;
  bool on_gizmo = false;
  // The viewport gives real Move/Rotate/Scale handle picking priority before
  // calling us. Do not reserve obsolete screen-space translation axes here.
  for (size_t i = 0; i < logic_graph_.edges.size() && logic_drawn_ < 1500;
       ++i) {
    auto &edge = logic_graph_.edges[i];
    if (!allowed.contains(edge.from) || !allowed.contains(edge.to))
      continue;
    auto from = logic_graph_.find(edge.from), to = logic_graph_.find(edge.to);
    if (!from || !to)
      continue;
    auto a = project(logic_position(*from)), b = project(logic_position(*to));
    if (!a || !b)
      continue;
    ImVec2 start = p + ImVec2(a->x, a->y), end = p + ImVec2(b->x, b->y);
    float length = std::hypot(end.x - start.x, end.y - start.y);
    if (length < 2)
      continue;
    ImU32 color = state_color(live_logic_node(edge.from));
    if (edge.missing)
      color = IM_COL32(255, 100, 90, 255);
    dl->AddLine(start, end, color,
                edge.from == logic_selected_ || edge.to == logic_selected_
                    ? 2.5f
                    : 1.5f);
    ImVec2 tip = start + (end - start) * .65f,
           dir = (end - start) * (1.f / length), side{-dir.y, dir.x};
    dl->AddTriangleFilled(tip, tip - dir * 8 + side * 4,
                          tip - dir * 8 - side * 4, color);
    ++logic_drawn_;
    ImVec2 mid = (start + end) * .5f;
    if (mid.x >= p.x && mid.x <= p.x + size.x && mid.y >= p.y &&
        mid.y <= p.y + size.y)
      hits_.push_back({"logic_edge_" + std::to_string(i), mid - ImVec2(6, 6),
                       mid + ImVec2(6, 6)});
    float d = distance(mouse, start, end);
    if (hovered && !flight_active_ && !on_gizmo && d < nearest &&
        std::hypot(mouse.x - start.x, mouse.y - start.y) > 12 &&
        std::hypot(mouse.x - end.x, mouse.y - end.y) > 12) {
      nearest = d;
      hover = &edge;
      hover_index = i;
    }
  }
  if (hover) {
    ImGui::BeginTooltip();
    auto a = logic_graph_.find(hover->from), b = logic_graph_.find(hover->to);
    ImGui::TextWrapped("%s -> %s", a ? display_name(*a).c_str() : "Missing",
                       b ? display_name(*b).c_str() : "Missing");
    ImGui::TextWrapped("%s", hover->label.c_str());
    ImGui::TextDisabled(link_mode_ ? "Click connection to edit its output."
                                   : "Click connection to inspect its target.");
    ImGui::EndTooltip();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl && !drag3d_ &&
        placement_ < 0) {
      if (link_mode_)
        choose_link_edge(*hover);
      else {
        select_logic(hover->to);
        logic_window_ = true;
      }
      return true;
    }
  }
  (void)hover_index;
  return false;
}
void MapEditor::logic_panel() {
  if (!logic_window_)
    return;
  ImGui::SetNextWindowSize(ImVec2(900, 580), ImGuiCond_FirstUseEver);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.035f, .045f, .105f, 1));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.045f, .055f, .12f, 1));
  if (!ImGui::Begin("Map logic / live test", &logic_window_,
                    ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    ImGui::PopStyleColor(2);
    return;
  }
  ImGui::TextWrapped("Follow the original activation links. Live colors "
                     "describe observed source states; incoming links are "
                     "possible paths, not an AND condition.");
  ImGui::Checkbox("Selected chain (up to 4 steps)", &logic_follow_);
  hit("logic_follow");
  ImGui::SameLine();
  if (button("Run logic test", "logic_run", loaded() && !test_future_.valid()))
    attempt([&] { test(false, true); });
  ImGui::SameLine();
  if (button("Test Survival + logic", "logic_survival_test",
             loaded() && !test_future_.valid()))
    attempt([&] { test(true, true); });
  ImGui::SameLine();
  if (button("Close panel", "logic_close"))
    logic_window_ = false;
  if (logic_file_.empty())
    ImGui::TextDisabled(
        "No test session. Start Test logic to inspect real game state.");
  else if (doc_.changed() || saved_export_ != logic_export_ ||
           project_.folder.string() != logic_project_)
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kYellow),
                       "Edited map differs from the running test. Start a new "
                       "test for live results.");
  else if (!logic_fresh_)
    ImGui::TextDisabled("No fresh game sample. Waiting for the test, or the "
                        "test has stopped. Old states are not shown as live.");
  else if (!logic_stream_ready_)
    ImGui::TextDisabled(
        "Waiting for the selected level to be ready in the game.");
  else
    ImGui::TextColored(theme::kGood, "LIVE: %s | frame %lld | epoch %lld",
                       text(logic_live_, "level").c_str(),
                       (long long)number(logic_live_, "frame"),
                       (long long)number(logic_live_, "epoch"));
  if (!logic_error_.empty())
    ImGui::TextWrapped("Diagnostics: %s", logic_error_.c_str());
  ImGui::SetNextItemWidth(280);
  ImGui::InputTextWithHint("##logic_search", "Find name, role or ID...",
                           logic_search_, sizeof(logic_search_));
  hit("logic_search");
  ImGui::SameLine();
  const char *filters[]{"All logic", "Triggers / counters", "Waves / templates",
                        "Doors / switches"};
  ImGui::SetNextItemWidth(220);
  ImGui::Combo("##logic_kind", &logic_filter_, filters, 4);
  hit("logic_filter");
  ImGui::Separator();
  float height = std::max(180.f, ImGui::GetContentRegionAvail().y);
  ImGui::BeginChild("logic_nodes", ImVec2(300, height),
                    ImGuiChildFlags_Borders);
  std::set<uint32_t> linked;
  for (auto &e : logic_graph_.edges) {
    linked.insert(e.from);
    linked.insert(e.to);
  }
  auto query = lower(logic_search_);
  for (auto &node : logic_graph_.nodes) {
    if (!node.supported && !linked.contains(node.id))
      continue;
    if (logic_filter_ == 1 && node.type != 4030 && node.type != 4080 &&
        node.type != 4170)
      continue;
    if (logic_filter_ == 2 && node.type != 4040 && node.type != 5000)
      continue;
    if (logic_filter_ == 3 && node.type != 4050 && node.type != 4070 &&
        node.type != 4180)
      continue;
    std::string label = std::to_string(node.id) + "  " + display_name(node);
    if (!query.empty() &&
        lower(label + " " + node.role).find(query) == std::string::npos)
      continue;
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImGui::ColorConvertU32ToFloat4(state_color(live_logic_node(node.id))));
    if (ImGui::Selectable((label + "##logic" + std::to_string(node.id)).c_str(),
                          logic_selected_ == node.id))
      select_logic(node.id);
    hit(("logic_node_" + std::to_string(node.id)).c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("logic_details", ImVec2(0, height),
                    ImGuiChildFlags_Borders);
  if (auto node = logic_graph_.find(logic_selected_)) {
    ImGui::TextColored(theme::kGold, "%s", display_name(*node).c_str());
    ImGui::TextWrapped("%s | ID %u | type %u%s", node->role.c_str(), node->id,
                       node->type, node->helper ? " | generated helper" : "");
    ImGui::TextWrapped("%s", node->summary.c_str());
    if (button("Focus node", "logic_focus"))
      select_logic(node->id, true);
    ImGui::Separator();
    const auto live = recorded_logic_node(node->id);
    if (live && flag(*live, "observed")) {
      ImGui::TextColored(
          ImGui::ColorConvertU32ToFloat4(
              state_color(logic_sample_current() ? live : nullptr)),
          "%s: %s",
          logic_sample_current() ? "Observed live" : "Recorded (not live)",
          text(*live, "status").c_str());
      ImGui::TextWrapped("%s", text(*live, "reason").c_str());
      if (node->type == 4040) {
        ImGui::TextWrapped(
            "Spawned: %lld / %lld | This wave: %lld / %lld | Queued: %lld",
            (long long)number(*live, "spawned"),
            (long long)number(*live, "total"),
            (long long)number(*live, "wave_spawned"),
            (long long)number(*live, "quota"),
            (long long)number(*live, "queued"));
        if (auto delay = live->find("delay_remaining"))
          ImGui::TextWrapped("Spawn delay remaining: %s s",
                             scalar(*delay).c_str());
      } else if (node->type == 4080 && number(*live, "counter_enabled") != 0) {
        ImGui::TextWrapped("Counter: %lld / %lld | Missing signals: %lld",
                           (long long)number(*live, "count"),
                           (long long)number(*live, "threshold"),
                           (long long)number(*live, "remaining_signals"));
      }
    } else
      ImGui::TextWrapped("No verified live state for this node. Static links "
                         "alone do not prove why a door is closed.");
    for (bool incoming : {true, false}) {
      ImGui::Separator();
      ImGui::TextUnformatted(incoming ? "Incoming activation paths"
                                      : "Outgoing links");
      bool found = false;
      for (size_t i = 0; i < logic_graph_.edges.size(); ++i) {
        auto &edge = logic_graph_.edges[i];
        if ((incoming ? edge.to : edge.from) != node->id)
          continue;
        found = true;
        auto id = incoming ? edge.from : edge.to;
        auto other = logic_graph_.find(id);
        auto label =
            edge.label + " -> " +
            (other ? display_name(*other) : "Missing ID " + std::to_string(id));
        if (ImGui::Selectable((label + "##link" + std::to_string(i) +
                               (incoming ? "in" : "out"))
                                  .c_str()))
          select_logic(id);
        hit(("logic_link_" + std::to_string(i) + (incoming ? "in" : "out"))
                .c_str());
        if (auto state = recorded_logic_node(id))
          ImGui::TextWrapped(
              "  %s%s: %s", logic_sample_current() ? "" : "Recorded: ",
              text(*state, "status").c_str(), text(*state, "reason").c_str());
      }
      if (!found)
        ImGui::TextDisabled("No known link.");
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Observed changes in this test");
    bool event_found = false;
    if (recorded_logic_node(node->id))
      if (auto events = logic_live_.find("events"); events && events->array())
        for (auto it = events->array()->rbegin(); it != events->array()->rend();
             ++it)
          if (number(*it, "id") == node->id) {
            ImGui::TextWrapped("%s: %s", text(*it, "kind").c_str(),
                               text(*it, "text").c_str());
            event_found = true;
          }
    if (!event_found)
      ImGui::TextDisabled("No observed change recorded for this node.");
    if (live && live->object() &&
        ImGui::CollapsingHeader("Raw diagnostic fields"))
      for (auto &[key, value] : *live->object()) {
        if (key == "id" || key == "type" || key == "status" ||
            key == "reason" || key == "observed" || value.array() ||
            value.object())
          continue;
        ImGui::TextWrapped("%s: %s", key.c_str(), scalar(value).c_str());
      }
  } else
    ImGui::TextWrapped(
        "Select a trigger, wave, counter or door to inspect its connections.");
  if (!logic_graph_.warnings.empty() &&
      ImGui::CollapsingHeader("Unresolved / unsupported links"))
    for (auto &warning : logic_graph_.warnings)
      ImGui::TextWrapped("%s", warning.c_str());
  ImGui::EndChild();
  ImGui::End();
  ImGui::PopStyleColor(2);
}
J MapEditor::logic_test_state() const {
  J::Array nodes, edges;
  for (auto &n : logic_graph_.nodes)
    nodes.emplace_back(J::Object{{"id", int64_t(n.id)},
                                 {"object", n.object},
                                 {"type", int64_t(n.type)},
                                 {"name", n.name},
                                 {"role", n.role},
                                 {"summary", n.summary},
                                 {"helper", n.helper}});
  for (auto &e : logic_graph_.edges)
    edges.emplace_back(J::Object{{"from", int64_t(e.from)},
                                 {"to", int64_t(e.to)},
                                 {"label", e.label},
                                 {"slot", e.slot},
                                 {"editable", e.editable},
                                 {"missing", e.missing}});
  return J::Object{{"nodes", std::move(nodes)},
                   {"edit", link_test_state()},
                   {"edges", std::move(edges)},
                   {"selected", int64_t(logic_selected_)},
                   {"connections", connections_},
                   {"panel", logic_window_},
                   {"drawn", int64_t(logic_drawn_)},
                   {"fresh", logic_fresh_},
                   {"stream_ready", logic_stream_ready_},
                   {"current", logic_sample_current()},
                   {"file", logic_file_},
                   {"token", logic_token_},
                   {"error", logic_error_},
                   {"live", logic_live_}};
}
