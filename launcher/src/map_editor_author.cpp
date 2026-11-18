#include "map_editor.h"
#include "theme.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
uint32_t word(const mapedit::Document &doc, uint32_t p) {
  const auto &b = doc.payload();
  if (uint64_t(p) + 4 > b.size())
    throw std::runtime_error("Logic field outside map");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
const char *explanation(mapedit::LogicKind kind) {
  using K = mapedit::LogicKind;
  switch (kind) {
  case K::Area:
    return "A player entering this area sends its outputs. Re-entering can "
           "send another event. Connect several actions or enemy waves.";
  case K::Relay:
    return "One incoming Trigger sends all configured outputs. Outputs can "
           "trigger, activate or deactivate receivers.";
  case K::Counter:
    return "Wait for the configured number of incoming signals, then send "
           "outputs. Signals count events; they do not require distinct "
           "sources.";
  case K::Timer:
    return "Delay incoming triggers. Repeating timers schedule subsequent "
           "ticks through native relays. Disable the timer to stop it.";
  case K::Gate:
    return "Starts disabled. Another node must Activate this gate before "
           "incoming triggers are forwarded. Deactivate closes it again.";
  case K::KeyBranch:
    return "When a player enters, check the selected native key. Accepted and "
           "denied events use separate output pins. The original game consumes "
           "an accepted key.";
  case K::Button:
    return "A visible native button that the player activates with Use. "
           "Supports multiple outputs, delay and one-time or reusable "
           "interaction.";
  case K::Toggle:
    return "A reusable native button with separate switched-on and "
           "switched-off output pins. Its native gate controllers retain the "
           "state.";
  case K::Light:
    return "A native point light. Set its colour and radius. Activate turns it "
           "on; Deactivate turns it off. Use a timer or relay when a delayed "
           "change is needed.";
  case K::Explosion:
    return "A native explosion with visible effects and radial damage. Trigger "
           "fires it after its configured delay. Activate/Deactivate arms or "
           "disarms it.";
  }
  return "";
}
mapedit::LogicSpec defaults(mapedit::LogicKind kind) {
  mapedit::LogicSpec s;
  s.kind = kind;
  s.name = mapedit::logic_kind_name(kind);
  s.delay = kind == mapedit::LogicKind::Timer ? 1.f : 0.f;
  s.enabled = kind != mapedit::LogicKind::Gate;
  s.effect.kind = kind == mapedit::LogicKind::Explosion
                      ? gc::editor::EffectKind::Explosion
                      : gc::editor::EffectKind::Light;
  return s;
}
void effect_fields(mapedit::LogicSpec &spec, bool creation,
                   const std::function<void(const char *)> &hit) {
  const auto prefix =
      creation ? std::string("logic_create_") : std::string("logic_");
  ImGui::InputFloat("Effect radius (m)", &spec.effect.radius, .5f, 2, "%.2f");
  spec.effect.radius = std::clamp(spec.effect.radius, .1f, 64.f);
  hit((prefix + "effect_radius").c_str());
  if (spec.kind == mapedit::LogicKind::Light) {
    float colour[]{spec.effect.colour[0] / 255.f, spec.effect.colour[1] / 255.f,
                   spec.effect.colour[2] / 255.f};
    if (ImGui::ColorEdit3("Light colour", colour))
      for (unsigned k = 0; k < 3; ++k)
        spec.effect.colour[k] =
            uint8_t(std::clamp(colour[k] * 255.f + .5f, 0.f, 255.f));
    hit((prefix + "effect_colour").c_str());
    ImGui::TextDisabled("Up to 8 authored lights per map.");
  } else {
    ImGui::InputFloat("Explosion damage", &spec.effect.damage, 5, 25, "%.1f");
    spec.effect.damage = std::clamp(spec.effect.damage, 0.f, 10000.f);
    hit((prefix + "effect_damage").c_str());
    ImGui::InputFloat("Visual scale", &spec.effect.scale, .1f, .5f, "%.2f");
    spec.effect.scale = std::clamp(spec.effect.scale, .1f, 8.f);
    hit((prefix + "effect_scale").c_str());
    int style = int(spec.effect.style);
    const char *variants[] = {"Effect variant 1", "Effect variant 2",
                              "Effect variant 3", "Effect variant 4",
                              "Effect variant 5"};
    if (ImGui::Combo("Explosion effect", &style, variants, 5))
      spec.effect.style = uint32_t(style);
    hit((prefix + "effect_style").c_str());
  }
}
} // namespace

void MapEditor::logic_properties() {
  if (selected_ < 0 || size_t(selected_) >= doc_.objects().size())
    return;
  size_t index = size_t(selected_);
  if (doc_.objects()[index].type == 4200) {
    const auto owner = doc_.logic_owner_index(index);
    if (owner < 0)
      return;
    index = size_t(owner);
  }
  if (!doc_.is_editor_logic(index))
    return;
  auto id = word(doc_, doc_.objects()[index].offset + 20);
  if (logic_draft_id_ != id ||
      logic_draft_revision_ != doc_.structure_revision()) {
    auto value = doc_.logic_settings(id);
    if (!value)
      return;
    logic_draft_ = *value;
    logic_draft_id_ = id;
    logic_draft_revision_ = doc_.structure_revision();
    std::snprintf(logic_name_, sizeof(logic_name_), "%s",
                  logic_draft_.name.c_str());
  }
  auto &s = logic_draft_;
  using K = mapedit::LogicKind;
  ImGui::Separator();
  ImGui::TextColored(theme::kGold, "%s / native logic",
                     mapedit::logic_kind_name(s.kind));
  ImGui::TextWrapped("%s", explanation(s.kind));
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("Name##logic", logic_name_, sizeof(logic_name_));
  hit("logic_name");
  ImGui::Checkbox("Initially enabled", &s.enabled);
  hit("logic_enabled");
  if (s.kind == K::Area || s.kind == K::KeyBranch) {
    ImGui::SetNextItemWidth(130);
    ImGui::InputFloat(s.kind == K::KeyBranch ? "Full box side length (m)"
                                             : "Radius (m)",
                      &s.radius, .25f, 1, "%.2f");
    if(s.kind == K::KeyBranch && ImGui::IsItemHovered())
      ImGui::SetTooltip("World-aligned box extending +X, +Y and -Z from its anchor.");
    s.radius = std::clamp(s.radius, .25f, 10000.f);
    hit("logic_radius");
  }
  if (s.kind == K::Area) {
    ImGui::SetNextItemWidth(130);
    ImGui::InputFloat("Poll interval (s)", &s.poll_interval, .02f, .1f, "%.2f");
    s.poll_interval = std::clamp(s.poll_interval, .02f, 10.f);
    hit("logic_poll");
  }
  if (s.kind != K::KeyBranch && s.kind != K::Light) {
    ImGui::SetNextItemWidth(130);
    ImGui::InputFloat(s.kind == K::Timer ? "Timer interval (s)"
                                         : "Output delay (s)",
                      &s.delay, .1f, 1, "%.2f");
    s.delay = std::clamp(s.delay, 0.f, 3600.f);
    hit("logic_delay");
    int cap = int(s.activation_limit);
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("Activation limit (0 = unlimited)", &cap))
      s.activation_limit = unsigned(std::clamp(cap, 0, 4096));
    hit("logic_limit");
  }
  if (s.kind == K::Counter) {
    int n = int(s.threshold);
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("Signals required", &n))
      s.threshold = unsigned(std::clamp(n, 1, 4096));
    hit("logic_threshold");
    ImGui::Checkbox("Reset count after firing", &s.repeat);
    hit("logic_repeat");
  }
  if (s.kind == K::Timer) {
    ImGui::Checkbox("Repeat timer", &s.repeat);
    hit("logic_repeat");
  }
  if (s.kind == K::Relay || s.kind == K::Counter || s.kind == K::Timer ||
      s.kind == K::Gate) {
    ImGui::Checkbox("Trigger at level start", &s.start_on_load);
    hit("logic_start");
  }
  if (s.kind == K::Button) {
    ImGui::Checkbox("Reusable button", &s.reusable);
    hit("logic_reusable");
  }
  if (s.kind == K::KeyBranch) {
    int key = s.key;
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("Required native key (0..15)", &key))
      s.key = std::clamp(key, 0, 15);
    hit("logic_key");
  }
  if (s.kind == K::Light || s.kind == K::Explosion)
    effect_fields(s, false, [&](const char *id) { hit(id); });
  if (button("Apply logic properties", "logic_apply",
             !test_future_.valid() && !drag3d_ && !flight_active_))
    attempt([&] {
      s.name = logic_name_;
      doc_.set_logic_settings(id, s);
      sync_structure();
      refresh_logic();
      logic_selected_ = id;
      logic_draft_revision_ = UINT64_MAX;
      message_ = "Native logic settings saved in the map draft. Save project "
                 "to keep them; Test logic runs the actual game.";
    });
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("One undoable edit. Values are written into native SSW "
                      "actor configurations and survive save/load/export.");
  if (s.kind != K::Light &&
      button("Connect outputs...", "logic_connect", !test_future_.valid())) {
    link_mode_ = true;
    link_source_ = id;
    link_target_ = UINT32_MAX;
    link_slot_ = -1;
    link_pick_ = 2;
    link_revision_ = doc_.structure_revision();
    connections_ = true;
    logic_selected_ = id;
    message_ = "Choose an output event and pick its receiver in the viewport "
               "or Logic graph.";
  }
}

void MapEditor::logic_creation_popup() {
  ImGui::SetNextWindowSize(ImVec2(590, 520), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Create logic", nullptr,
                              ImGuiWindowFlags_NoSavedSettings))
    return;
  static constexpr const char *kinds[] = {"Player area trigger",
                                          "Trigger relay / sequence",
                                          "Signal counter",
                                          "Timer",
                                          "Condition gate",
                                          "Key condition branch",
                                          "Interactive wall button",
                                          "Toggle switch",
                                          "Point light",
                                          "Explosion"};
  if (ImGui::BeginCombo("Logic element",
                        kinds[std::clamp(logic_create_type_, 0, 9)])) {
    for (int kind = 0; kind < 10; ++kind) {
      if (ImGui::Selectable(kinds[kind], logic_create_type_ == kind)) {
        logic_create_type_ = kind;
        logic_create_draft_ = defaults(mapedit::LogicKind(kind));
        std::snprintf(logic_create_name_, sizeof(logic_create_name_), "%s",
                      logic_create_draft_.name.c_str());
      }
      hit(("logic_kind_option_" + std::to_string(kind)).c_str());
    }
    ImGui::EndCombo();
  }
  hit("logic_kind");
  auto &s = logic_create_draft_;
  s.kind = mapedit::LogicKind(logic_create_type_);
  using K = mapedit::LogicKind;
  ImGui::TextWrapped("%s", explanation(s.kind));
  ImGui::Separator();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("Name", logic_create_name_, sizeof(logic_create_name_));
  hit("logic_create_name");
  if (s.kind == K::Area || s.kind == K::KeyBranch) {
    ImGui::InputFloat(s.kind == K::KeyBranch ? "Full box side length (m)"
                                             : "Radius (m)",
                      &s.radius, .5f, 2, "%.2f");
    if(s.kind == K::KeyBranch && ImGui::IsItemHovered())
      ImGui::SetTooltip("World-aligned box extending +X, +Y and -Z from its anchor.");
    s.radius = std::clamp(s.radius, .25f, 10000.f);
    hit("logic_create_radius");
  }
  if (s.kind != K::KeyBranch && s.kind != K::Light) {
    ImGui::InputFloat(s.kind == K::Timer ? "Timer interval (s)"
                                         : "Output delay (s)",
                      &s.delay, .1f, 1, "%.2f");
    s.delay = std::clamp(s.delay, 0.f, 3600.f);
    hit("logic_create_delay");
  }
  if (s.kind == K::Counter) {
    int n = int(s.threshold);
    if (ImGui::InputInt("Signals required", &n))
      s.threshold = unsigned(std::clamp(n, 1, 4096));
    hit("logic_create_threshold");
    ImGui::Checkbox("Reset count after firing", &s.repeat);
  }
  if (s.kind == K::Timer) {
    ImGui::Checkbox("Repeat timer", &s.repeat);
    hit("logic_create_repeat");
    ImGui::Checkbox("Start when level loads", &s.start_on_load);
    hit("logic_create_start");
  }
  if (s.kind == K::Button) {
    ImGui::Checkbox("Reusable", &s.reusable);
    hit("logic_create_reusable");
  }
  if (s.kind == K::KeyBranch) {
    int key = s.key;
    if (ImGui::InputInt("Required native key (0..15)", &key))
      s.key = std::clamp(key, 0, 15);
    hit("logic_create_key");
  }
  if (s.kind == K::Light || s.kind == K::Explosion) {
    ImGui::Checkbox("Initially enabled", &s.enabled);
    hit("logic_create_enabled");
    effect_fields(s, true, [&](const char *id) { hit(id); });
  }
  ImGui::TextWrapped(
      "The element is placed at the current camera pivot. Use Move/Rotate to "
      "place it precisely, then connect its output pins in the Logic graph.");
  if (s.kind == K::Button || s.kind == K::Toggle)
    ImGui::TextDisabled("Button geometry and materials are taken from your own "
                        "original game data.");
  if (button("Create element", "logic_create_apply",
             loaded() && !test_future_.valid()))
    attempt([&] {
      s.name = logic_create_name_;
      const std::array<float, 3> p{camera_.target.x, camera_.target.y + 1,
                                   camera_.target.z};
      size_t created;
      if (s.kind == K::Button || s.kind == K::Toggle) {
        mapedit::Document original;
        original.load(std::filesystem::u8path(data_) /
                      "files/Levels/Rlevel1_1.ssw");
        size_t brush = original.objects().size();
        for (size_t i = 0; i < original.objects().size(); ++i) {
          const auto &o = original.objects()[i];
          if (o.type != 4070)
            continue;
          auto target = word(original, word(original, o.offset + 16) + 36);
          if (std::any_of(original.objects().begin(), original.objects().end(),
                          [&](const auto &other) {
                            return other.type == 4180 &&
                                   word(original, other.offset + 20) == target;
                          })) {
            brush = i;
            break;
          }
        }
        if (brush == original.objects().size())
          throw std::runtime_error(
              "The original interactive button template is missing");
        created = doc_.add_button(original, brush, p, s);
      } else
        created = doc_.add_logic(p, s);
      select(int(created));
      sync_structure();
      refresh_logic();
      logic_selected_ = word(doc_, doc_.objects()[created].offset + 20);
      logic_draft_revision_ = UINT64_MAX;
      message_ = "Logic element created. Connect its named outputs to "
                 "receivers, Save project, then Test logic.";
      ImGui::CloseCurrentPopup();
    });
  ImGui::SameLine();
  if (button("Cancel", "logic_create_cancel"))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}
