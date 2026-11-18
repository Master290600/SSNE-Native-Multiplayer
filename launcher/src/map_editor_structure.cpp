#include "map_editor.h"
#include "theme.h"
#include <algorithm>
#include <cctype>
#include <set>
namespace {
std::string folded(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
} // namespace
std::vector<uint32_t> MapEditor::visual_signature()const{
  const auto& bytes=doc_.payload();
  const auto word=[&](uint64_t p){return p+4<=bytes.size()?(uint32_t(bytes[size_t(p)])<<24|uint32_t(bytes[size_t(p)+1])<<16|uint32_t(bytes[size_t(p)+2])<<8|bytes[size_t(p)+3]):0u;};
  std::vector<uint32_t> key;key.reserve(doc_.objects().size()*8);
  for(size_t i=0;i<doc_.objects().size();++i){const auto&o=doc_.objects()[i];
    if(!(o.type==4200||o.type==4260||o.type==5000||o.type==4040||(o.type>=2000&&o.type<4000)))continue;
    const auto config=word(uint64_t(o.offset)+16);
    key.insert(key.end(),{uint32_t(i),o.offset,o.type,o.transform,config});
    // Config changes conservatively invalidate meshes; matrix values remain
    // live in model_matrix and never cause a geometry/texture rebuild.
    const unsigned words=o.type==4040?15:o.type==4200?4:o.type==4260?5:6;
    for(unsigned j=0;j<words;++j)key.push_back(config?word(uint64_t(config)+j*4):0u);
  }
  return key;
}
void MapEditor::sync_structure() {
  if (document_revision_ == doc_.structure_revision())
    return;
  placement_ = -1;
  select(selected_);
  if(visual_signature_!=visual_signature())load_scene(false);
  else document_revision_=doc_.structure_revision();
}
void MapEditor::place_at(mapedit::V3 p) {
  if (placement_ < 0)
    return;
  attempt([&] {
    auto next =
        placement_foreign_
            ? doc_.import_object(donor_, size_t(placement_), {p.x, p.y, p.z})
            : doc_.duplicate(size_t(placement_), {p.x, p.y, p.z});
    placement_ = -1;
    select(int(next));
    sync_structure();
    message_ = "Object placed. Use the position fields or handles to adjust "
               "it. Undo is available.";
  });
}
void MapEditor::duplicate_selected() {
  if (selected_ < 0)
    return;
  attempt([&] {
    auto m = doc_.matrix(size_t(selected_));
    auto next = doc_.duplicate(size_t(selected_), {m[3] + 2, m[7], m[11]});
    select(int(next));
    sync_structure();
    message_ = "Duplicated selection 2 m along X. The copy has its own ID and "
               "configuration.";
  });
}
void MapEditor::structure_tools() {
  bool idle = loaded() && !link_mode_ && !drag3d_ && !dragging_ &&
              !flight_active_ && !test_future_.valid();
  if (button("Add trigger", "add_trigger", idle))
    attempt([&] {
      auto n = doc_.add_wave_trigger(
          {camera_.target.x, camera_.target.y + 1, camera_.target.z}, {});
      select(int(n));
      sync_structure();
      message_ = "Trigger created. Link newly placed enemies in its "
                 "properties, then Apply trigger.";
    });
  ImGui::SameLine();
  if (button("Add object...", "add_object", idle)) {
    placement_ = -1;
    palette_search_[0] = 0;
    ImGui::OpenPopup("Place a new object");
  }
  ImGui::SameLine();
  bool duplicate =
      idle && selected_ >= 0 && doc_.can_duplicate(size_t(selected_));
  if (button("Duplicate (Ctrl+D)", "duplicate", duplicate))
    duplicate_selected();
  ImGui::SameLine();
  bool erase = idle && selected_ >= 0 && doc_.can_erase(size_t(selected_));
  auto remove = [&] {
    attempt([&] {
      doc_.erase(size_t(selected_));
      select(-1);
      sync_structure();
      message_ = "Object deleted. Undo restores it, including its model and "
                 "references.";
    });
  };
  if (button("Delete", "delete_object", erase))
    remove();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !erase &&
      selected_ >= 0)
    ImGui::SetTooltip("Story/script objects, referenced enemy templates and "
                      "the last player start are protected.");
  if (idle && !ImGui::GetIO().WantTextInput &&
      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    if (duplicate && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_D, false))
      duplicate_selected();
    else if (erase && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
      remove();
  }
  ImGui::SameLine();
  if (button("Play Survival", "play_survival", idle))
    attempt([&] { test(true); });
  if (placement_ >= 0) {
    ImGui::SameLine();
    if (button("Cancel placement (Esc)", "cancel_placement"))
      placement_ = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      placement_ = -1;
    if (placement_ >= 0)
      ImGui::TextColored(
          ImGui::ColorConvertU32ToFloat4(theme::kYellow),
          "Click a map surface to place %s. Shift: snap. Esc: cancel.",
          (placement_foreign_ ? donor_ : doc_)
              .objects()[placement_]
              .name.c_str());
  }
}
void MapEditor::structure_popups() {
  ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Place a new object", nullptr,
                              ImGuiWindowFlags_NoSavedSettings))
    return;
  ImGui::TextWrapped("Choose a source map and template. Place active enemies, "
                     "pickups, props and native camera/sound/music/effect actions "
                     "on the target map. Required models and textures are "
                     "imported with the object.");
  ImGui::SetNextItemWidth(260);
  if (ImGui::BeginCombo("Source map", palette_map_ < 0
                                          ? ("Current: " + level_).c_str()
                                          : donor_level_.c_str())) {
    if (ImGui::Selectable(("Current: " + level_).c_str(), palette_map_ < 0))
      palette_map_ = -1;
    hit("source_map_current");
    for (size_t i = 0; i < levels_.size(); ++i)
      if (levels_[i] != level_) {
        if (ImGui::Selectable(levels_[i].c_str(), palette_map_ == int(i)))
          attempt([&] {
            mapedit::Document next;
            next.load(std::filesystem::u8path(data_) / "files/Levels" /
                      (levels_[i] + ".ssw"));
            auto previews =
                mapedit::Appearance::load(next, std::filesystem::u8path(data_))
                    .previews;
            donor_ = std::move(next);
            donor_previews_ = std::move(previews);
            donor_level_ = levels_[i];
            palette_map_ = int(i);
          });
        hit(("source_map_" + levels_[i]).c_str());
      }
    ImGui::EndCombo();
  }
  hit("palette_map");
  const auto &source = palette_map_ < 0 ? doc_ : donor_;
  const auto &previews =
      palette_map_ < 0 ? appearance_.previews : donor_previews_;
  const char *kinds[] = {"All supported templates", "Enemies",
                         "Weapons / pickups", "Props", "Player starts", "Logic / action templates"};
  ImGui::SetNextItemWidth(240);
  ImGui::Combo("Category", &palette_kind_, kinds, 6);
  hit("palette_kind");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##palette_search", "Search template or model...",
                           palette_search_, sizeof(palette_search_));
  hit("palette_search");
  ImGui::BeginChild("templates", ImVec2(0, -45), ImGuiChildFlags_Borders);
  auto query = folded(palette_search_);
  size_t available = 0;
  for (size_t i = 0; i < source.objects().size(); ++i) {
    auto &o = source.objects()[i];
    if (o.added || !source.can_duplicate(i))
      continue;
    int kind = (o.type == 5000 || o.type == 4040) ? 1
               : o.type == 4200                   ? 3
               : o.type == 4140                   ? 4
               : mapedit::logic_template_type(o.type) ? 5
                                                  : 2;
    if (palette_kind_ && kind != palette_kind_)
      continue;
    std::string model;
    for (auto &p : previews)
      if (p.object == int(i)) {
        model = p.model;
        break;
      }
    auto label =
        o.name + " | " + (kind == 1 ? "Enemy" : mapedit::category(o.type));
    if (!model.empty())
      label += " | " + model;
    if (!query.empty() && folded(label).find(query) == std::string::npos)
      continue;
    ++available;
    ImGui::PushID(int(i));
    if (ImGui::Selectable(label.c_str())) {
      placement_ = int(i);
      placement_foreign_ = palette_map_ >= 0;
      drag3d_ = false;
      message_ = "Click a map surface to place the new object; Esc cancels.";
      ImGui::CloseCurrentPopup();
    }
    hit(("template_" + std::to_string(i)).c_str());
    ImGui::PopID();
  }
  if (!available)
    ImGui::TextWrapped(
        "No matching template in this map. Try another category or search.");
  ImGui::EndChild();
  if (button("Cancel", "palette_cancel"))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void MapEditor::wave_properties() {
  ImGui::Separator();
  ImGui::TextColored(theme::kGold, "Trigger / enemy wave");
  ImGui::SetNextItemWidth(130);
  ImGui::InputFloat("Radius (m)", &wave_draft_.radius, .5f, 2, "%.2f");
  hit("trigger_radius");
  ImGui::SetNextItemWidth(130);
  ImGui::InputFloat("Delay (s)", &wave_draft_.delay, .5f, 2, "%.2f");
  hit("trigger_delay");
  ImGui::Checkbox("Repeat on re-entry", &wave_draft_.repeat);
  hit("trigger_repeat");
  if (wave_draft_.repeat) {
    int n = int(wave_draft_.max_activations);
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("Max activations", &n))
      wave_draft_.max_activations = unsigned(std::clamp(n, 2, 4096));
    hit("trigger_max");
  }
  ImGui::TextWrapped("Link enemies you placed with Add object. Linked enemies "
                     "become wave templates and appear only after entering "
                     "this area. Leave and re-enter for another activation.");
  for (size_t row = 0; row < wave_draft_.rows.size(); ++row) {
    ImGui::PushID(int(row));
    auto &r = wave_draft_.rows[row];
    auto it = std::find_if(doc_.objects().begin(), doc_.objects().end(),
                           [&](auto &o) { return o.offset == r.enemy_offset; });
    ImGui::SetNextItemWidth(195);
    if (ImGui::BeginCombo("##enemy", it == doc_.objects().end()
                                         ? "Choose enemy"
                                         : it->name.c_str())) {
      for (auto &o : doc_.objects())
        if (o.added && o.type == 5000) {
          if (ImGui::Selectable(o.name.c_str(), o.offset == r.enemy_offset))
            r.enemy_offset = o.offset;
          hit(("wave_enemy_option_" + std::to_string(o.offset)).c_str());
        }
      ImGui::EndCombo();
    }
    hit(("wave_enemy_" + std::to_string(row)).c_str());
    int n = int(r.count);
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputInt("Count", &n))
      r.count = unsigned(std::clamp(n, 1, 64));
    hit(("wave_count_" + std::to_string(row)).c_str());
    if (button("Remove point",
               ("wave_remove_" + std::to_string(row)).c_str())) {
      wave_draft_.rows.erase(wave_draft_.rows.begin() + row);
      ImGui::PopID();
      break;
    }
    ImGui::PopID();
  }
  if (button("Link enemy", "wave_add", wave_draft_.rows.size() < 10)) {
    for (auto &o : doc_.objects())
      if (o.added && o.type == 5000 &&
          std::none_of(wave_draft_.rows.begin(), wave_draft_.rows.end(),
                       [&](auto &r) { return r.enemy_offset == o.offset; })) {
        wave_draft_.rows.push_back({o.offset, 3});
        break;
      }
  }
  if (button("Apply trigger", "wave_apply"))
    attempt([&] {
      doc_.set_wave_trigger(size_t(selected_), wave_draft_);
      select(selected_);
      sync_structure();
      message_ = "Trigger and linked wave saved in the map. Test in game to "
                 "check its area and spawn positions.";
    });
}
