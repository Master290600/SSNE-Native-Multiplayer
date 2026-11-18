#include "map_editor.h"
#include "theme.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace {
using J = launcher::JSONValue;
std::string label(const mapedit::LogicNode &n) {
  const auto name = n.helper ? std::string(n.type == 4040 ? "Wave spawn point"
                                                          : "Wave controller")
                             : n.name;
  return std::to_string(n.id) + "  " + (name.empty() ? n.role : name);
}
std::string lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool source_type(const mapedit::LogicNode &n) {
  return n.supported && (n.type == 4030 || n.type == 4080 || n.type == 4050 ||
                         n.type == 4180 || n.type == 4170 || n.type == 5000 ||
                         n.type == 4100 || (n.type >= 2000 && n.type < 4000));
}
bool target_type(const mapedit::LogicNode &n, const mapedit::Document &doc) {
  if (n.type == 5000)
    return n.object >= 0 && size_t(n.object) < doc.objects().size() &&
           doc.objects()[size_t(n.object)].added;
  return n.supported && (doc.can_logic_target(n.id,mapedit::LogicAction::Trigger) ||
                         doc.can_logic_target(n.id,mapedit::LogicAction::Enable) ||
                         doc.can_logic_target(n.id,mapedit::LogicAction::Disable));
}
} // namespace

void MapEditor::reset_link_edit() {
  link_mode_ = false;
  link_pick_ = 0;
  link_slot_ = -1;
  link_source_ = link_target_ = UINT32_MAX;
  link_revision_ = std::numeric_limits<uint64_t>::max();
  link_pick_candidates_.clear();
}
void MapEditor::sync_link_edit() {
  if (link_revision_ == doc_.structure_revision())
    return;
  link_revision_ = doc_.structure_revision();
  if (!logic_graph_.find(link_source_)) {
    link_source_ = UINT32_MAX;
    link_slot_ = -1;
  }
  if (!logic_graph_.find(link_target_))
    link_target_ = UINT32_MAX;
  if (link_slot_ >= 0 && link_source_ != UINT32_MAX) {
    const auto slots = doc_.logic_slots(link_source_);
    const auto it = std::find_if(slots.begin(), slots.end(), [&](auto &s) {
      return int(s.slot) == link_slot_;
    });
    if (it == slots.end())
      link_slot_ = -1;
    else {
      link_target_ = it->target.value_or(UINT32_MAX);
      link_action_ = int(it->action);
    }
  }
  link_pick_candidates_.clear();
}
void MapEditor::choose_link_node(uint32_t id) {
  const auto *n = logic_graph_.find(id);
  if (!n)
    return;
  if (link_pick_ == 1) {
    if (!source_type(*n) || doc_.logic_slots(id).empty()) {
      message_ = "This node has no supported editable activation output.";
      is_error_ = true;
      return;
    }
    link_source_ = id;
    link_target_ = UINT32_MAX;
    link_slot_ = -1;
    link_action_ = 0;
    link_pick_ = 2;
    select_logic(id);
    message_ = "Source selected. Pick a wave or door, then Add connection.";
  } else if (link_pick_ == 2) {
    if (!target_type(*n, doc_))
      return;
    link_target_ = id;
    if (n->type == 5000)
      link_action_ = 0;
    else if (!mapedit::logic_target_supports(
                 n->type, static_cast<mapedit::LogicAction>(link_action_))) {
      const auto slots = doc_.logic_slots(link_source_);
      const auto chosen =
          std::find_if(slots.begin(), slots.end(), [&](const auto &s) {
            return int(s.slot) == link_slot_;
          });
      const bool flexible =
          chosen != slots.end()
              ? chosen->action_editable
              : !slots.empty() && slots.front().action_editable;
      if (flexible)
        for (int action = 0; action < 3; ++action)
          if (mapedit::logic_target_supports(
                  n->type, static_cast<mapedit::LogicAction>(action))) {
            link_action_ = action;
            break;
          }
    }
    link_pick_ = 0;
    message_ =
        "Target selected. Review the action, then add or apply the connection.";
  } else
    select_logic(id);
  is_error_ = false;
}
void MapEditor::choose_link_edge(const mapedit::LogicEdge &edge) {
  // Copy slot identity before any selection or graph refresh. Equal endpoints
  // can belong to different original fields and must remain distinct.
  const auto source = edge.from, target = edge.to;
  const auto slot = edge.slot;
  if (!edge.editable || slot < 0) {
    message_ = "This internal/template/reference link is read-only. Select an "
               "activation output to edit.";
    is_error_ = false;
    select_logic(target);
    return;
  }
  link_source_ = source;
  link_target_ = target;
  link_slot_ = slot;
  link_pick_ = 0;
  const auto slots = doc_.logic_slots(source);
  for (const auto &s : slots)
    if (s.slot == unsigned(slot))
      link_action_ = int(s.action);
  select_logic(source);
  logic_window_ = false;
  message_ = "Connection selected. Pick a new target, change its action, or "
             "delete this exact output.";
  is_error_ = false;
}
void MapEditor::apply_link(bool remove) {
  attempt([&] {
    const auto source = link_source_;
    const auto *target = logic_graph_.find(link_target_);
    if (source == UINT32_MAX)
      throw std::runtime_error("Pick a source first.");
    const auto action = static_cast<mapedit::LogicAction>(link_action_);
    if (remove) {
      if (link_slot_ < 0)
        throw std::runtime_error("Select an existing connection first.");
      doc_.set_logic_link(source, unsigned(link_slot_), {}, action);
      link_slot_ = -1;
      link_target_ = UINT32_MAX;
    } else {
      if (!target)
        throw std::runtime_error("Pick a target first.");
      if (target->type == 5000) {
        if (link_slot_ >= 0 || action != mapedit::LogicAction::Trigger)
          throw std::runtime_error(
              "Use a new Trigger connection to add an enemy wave; existing "
              "wave templates stay protected.");
        if (target->object < 0)
          throw std::runtime_error(
              "Choose a newly placed enemy for this wave.");
        const auto offset = doc_.objects().at(size_t(target->object)).offset;
        link_target_ =
            doc_.link_wave_enemy(source, offset, unsigned(link_enemy_count_));
      } else if (link_slot_ >= 0) {
        doc_.set_logic_link(source, unsigned(link_slot_), link_target_, action);
      } else
        doc_.add_logic_link(source, link_target_, action);
      if (link_slot_ < 0)
        for (const auto &s : doc_.logic_slots(source))
          if (s.target == link_target_ && s.action == action) {
            link_slot_ = int(s.slot);
            break;
          }
    }
    // No cached node/edge reference survives a structural transaction.
    sync_structure();
    refresh_logic();
    select_logic(source);
    link_revision_ = doc_.structure_revision();
    link_pick_ = 0;
    message_ = remove ? "Connection deleted. Undo restores it."
                      : "Connection saved in the map draft. Undo is available; "
                        "Save project keeps it.";
  });
}
bool MapEditor::draw_link_picking(ImDrawList *dl, ImVec2 p, ImVec2 size,
                                  bool hovered) {
  if (!link_mode_)
    return false;
  auto project = [&](mapedit::V3 v) -> std::optional<ImVec2> {
    if (view3d_) {
      auto q = camera_.project(v);
      if (!q)
        return {};
      return p + ImVec2(q->x, q->y);
    }
    return p + ImVec2(size.x * .5f + (v.x - centre_x_) * zoom_,
                      size.y * .5f - (v.z - centre_z_) * zoom_);
  };
  const auto mouse = ImGui::GetIO().MousePos;
  std::vector<std::pair<uint32_t, ImVec2>> candidates;
  for (const auto &n : logic_graph_.nodes) {
    if (link_pick_ == 1 ? !source_type(n) : !target_type(n, doc_))
      continue;
    auto q = project(logic_position(n));
    if (!q || q->x < p.x + 10 || q->x > p.x + size.x - 10 || q->y < p.y + 10 ||
        q->y > p.y + size.y - 10)
      continue;
    auto icon = *q;
    if (n.helper)
      icon = icon + ImVec2(n.type == 4040 ? 13.f : -13.f, -14.f);
    const auto color = n.id == link_source_   ? IM_COL32(255, 205, 60, 255)
                       : n.id == link_target_ ? IM_COL32(100, 245, 160, 255)
                                              : IM_COL32(100, 205, 255, 230);
    if (n.helper) {
      dl->AddLine(*q, icon, color);
      dl->AddQuadFilled(icon + ImVec2(0, -6), icon + ImVec2(6, 0),
                        icon + ImVec2(0, 6), icon + ImVec2(-6, 0), color);
    } else
      dl->AddCircle(icon, 7, color, 16, 2);
    hits_.push_back({"logic_node_pick_" + std::to_string(n.id),
                     icon - ImVec2(7, 7), icon + ImVec2(7, 7)});
    if (std::hypot(mouse.x - icon.x, mouse.y - icon.y) < 10)
      candidates.emplace_back(n.id, icon);
    if (n.id == link_source_ || n.id == link_target_)
      dl->AddText(icon + ImVec2(9, -19), color, label(n).c_str());
  }
  if (auto n = logic_graph_.find(link_source_))
    if (auto start = project(logic_position(*n))) {
      std::optional<ImVec2> end;
      if (link_pick_ == 2 && hovered)
        end = mouse;
      else if (auto target = logic_graph_.find(link_target_))
        end = project(logic_position(*target));
      if (end) {
        dl->AddLine(*start, *end, IM_COL32(100, 245, 160, 230), 3);
        dl->AddCircle(*end, 10, IM_COL32(100, 245, 160, 255), 24, 2);
      }
    }
  const char *hint =
      link_pick_ == 1   ? "LINKS: click source | Esc: cancel picking"
      : link_pick_ == 2 ? "LINKS: click target | Add / Apply in the right panel"
                        : "LINKS: click a connection to edit | RMB + WASD: fly";
  dl->AddRectFilled(p + ImVec2(4, 26), p + ImVec2(size.x - 4, 48),
                    IM_COL32(6, 10, 22, 225));
  dl->AddText(p + ImVec2(8, 29), theme::kYellow, hint);
  if (hovered && !flight_active_ && !ImGui::GetIO().KeyAlt &&
      !ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (candidates.size() == 1)
      choose_link_node(candidates.front().first);
    else if (!candidates.empty()) {
      link_pick_candidates_.clear();
      for (auto [id, position] : candidates)
        link_pick_candidates_.push_back(id);
      ImGui::OpenPopup("Choose logic node");
    }
  }
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(.035f, .045f, .105f, 1));
  if (ImGui::BeginPopup("Choose logic node")) {
    for (auto id : link_pick_candidates_)
      if (auto n = logic_graph_.find(id)) {
        if (ImGui::Selectable(label(*n).c_str()))
          choose_link_node(id);
        hit(("logic_pick_option_" + std::to_string(id)).c_str());
      }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor();
  return true;
}
void MapEditor::link_properties() {
  sync_link_edit();
  const auto slots = link_source_ == UINT32_MAX
                         ? std::vector<mapedit::LogicSlot>{}
                         : doc_.logic_slots(link_source_);
  const mapedit::LogicSlot *chosen = nullptr;
  for (const auto &slot : slots)
    if (int(slot.slot) == link_slot_)
      chosen = &slot;
  const auto target = logic_graph_.find(link_target_);
  const bool flexible = chosen
                            ? chosen->action_editable
                            : !slots.empty() && slots.front().action_editable;
  const auto accepts = [&](int action) {
    return !target ||
           (target->type == 5000
                ? action == 0
                : doc_.can_logic_target(
                      target->id, static_cast<mapedit::LogicAction>(action)));
  };
  const bool idle = !drag3d_ && !flight_active_ && !test_future_.valid();
  ImGui::TextColored(theme::kGold, "Connection editor");
  ImGui::TextWrapped("Pick source > target. Click an arrow to edit.");
  ImGui::Text(
      "Source: %s  >  Target: %s",
      link_source_ == UINT32_MAX ? "?" : std::to_string(link_source_).c_str(),
      link_target_ == UINT32_MAX ? "?" : std::to_string(link_target_).c_str());
  ImGui::SetNextItemWidth(-1);
  const char *actions[]{"Trigger", "Activate", "Deactivate"};
  ImGui::BeginDisabled(!flexible);
  if (ImGui::BeginCombo("##link_action",
                        actions[std::clamp(link_action_, 0, 2)])) {
    for (int i = 0; i < 3; ++i) {
      ImGui::BeginDisabled(!accepts(i));
      if (ImGui::Selectable(actions[i], link_action_ == i))
        link_action_ = i;
      hit(("link_action_" + std::to_string(i)).c_str());
      ImGui::EndDisabled();
    }
    ImGui::EndCombo();
  }
  hit("link_action");
  ImGui::EndDisabled();
  if (button(link_slot_ < 0 ? "Add link" : "Apply link",
             link_slot_ < 0 ? "link_add" : "link_apply",
             idle && link_source_ != UINT32_MAX && target &&
                 accepts(link_action_))) {
    apply_link();
    return;
  }
  ImGui::SameLine();
  if (button("Delete link", "link_remove", idle && chosen && chosen->target)) {
    apply_link(true);
    return;
  }
  if (button("Pick source", "link_pick_source", idle)) {
    link_pick_ = 1;
    logic_window_ = false;
    placement_ = -1;
  }
  ImGui::SameLine();
  if (button("Pick target", "link_pick_target",
             idle && link_source_ != UINT32_MAX)) {
    link_pick_ = 2;
    logic_window_ = false;
    placement_ = -1;
  }
  if (button("New link", "link_new", idle)) {
    link_slot_ = -1;
    link_pick_ = link_source_ == UINT32_MAX ? 1 : 2;
  }
  ImGui::SameLine();
  if (button("Cancel pick", "link_cancel_pick", link_pick_ != 0))
    link_pick_ = 0;
  if (!accepts(link_action_))
    ImGui::TextWrapped("This target does not receive that action. Select a "
                       "supported action or another target.");
  ImGui::Separator();
  auto combo = [&](bool source) {
    auto &id = source ? link_source_ : link_target_;
    auto n = logic_graph_.find(id);
    ImGui::TextUnformatted(source ? "Source" : "Target");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(source ? "##link_source" : "##link_target",
                          n ? label(*n).c_str() : "Choose...")) {
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##find_link_node", "Find name or ID...",
                               link_search_, sizeof(link_search_));
      const auto query = lower(link_search_);
      for (const auto &candidate : logic_graph_.nodes) {
        if (source ? !source_type(candidate) : !target_type(candidate, doc_))
          continue;
        if (!query.empty() &&
            lower(label(candidate)).find(query) == std::string::npos)
          continue;
        if (ImGui::Selectable(label(candidate).c_str(), id == candidate.id)) {
          link_pick_ = source ? 1 : 2;
          choose_link_node(candidate.id);
        }
        hit(((source ? "link_source_" : "link_target_") +
             std::to_string(candidate.id))
                .c_str());
      }
      ImGui::EndCombo();
    }
    hit(source ? "link_source_combo" : "link_target_combo");
    if (n && button(source ? "Focus source" : "Focus target",
                    source ? "link_focus_source" : "link_focus_target"))
      select_logic(id, true);
  };
  combo(true);
  combo(false);
  ImGui::TextUnformatted("Output");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##link_slot", chosen
                                           ? chosen->label.c_str()
                                           : "New connection (empty output)")) {
    if (ImGui::Selectable("New connection (empty output)", link_slot_ < 0))
      link_slot_ = -1;
    hit("link_slot_new");
    for (const auto &slot : slots) {
      const auto name =
          slot.label +
          (slot.target ? " -> " + std::to_string(*slot.target) : " (empty)");
      if (ImGui::Selectable(name.c_str(), int(slot.slot) == link_slot_)) {
        link_slot_ = int(slot.slot);
        link_target_ = slot.target.value_or(UINT32_MAX);
        link_action_ = int(slot.action);
        link_pick_ = 0;
      }
      hit(("link_slot_" + std::to_string(slot.slot)).c_str());
    }
    ImGui::EndCombo();
  }
  hit("link_slot_combo");
  if (target && target->type == 5000) {
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputInt("Wave count", &link_enemy_count_))
      link_enemy_count_ = std::clamp(link_enemy_count_, 1, 64);
    hit("link_enemy_count");
    ImGui::TextWrapped("The newly placed enemy becomes a real wave template.");
  }
  ImGui::Separator();
  ImGui::TextWrapped(
      "Trigger requests door motion or a wave. Activate / Deactivate controls "
      "supported area, relay and switch actors. Enemy death fires per enemy; a "
      "whole wave needs a counter. Internal and model references stay "
      "protected.");
}
J MapEditor::link_test_state() const {
  J::Array slots;
  if (link_source_ != UINT32_MAX)
    for (const auto &s : doc_.logic_slots(link_source_))
      slots.emplace_back(
          J::Object{{"source", int64_t(s.source)},
                    {"owner", int64_t(s.owner)},
                    {"slot", int64_t(s.slot)},
                    {"label", s.label},
                    {"target", s.target ? J(int64_t(*s.target)) : J()},
                    {"action", int(s.action)},
                    {"action_editable", s.action_editable}});
  return J::Object{{"mode", link_mode_},
                   {"pick", link_pick_ == 1   ? "source"
                            : link_pick_ == 2 ? "target"
                                              : ""},
                   {"source", int64_t(link_source_)},
                   {"target", int64_t(link_target_)},
                   {"slot", link_slot_},
                   {"action", link_action_},
                   {"slots", std::move(slots)}};
}
