#include "map_editor.h"
#include "map_logic_icons.h"
#include "gc/sha256.h"
#include "platform.h"
#include "theme.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
namespace fs = std::filesystem;
namespace {
std::string lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
ImU32 color(uint32_t type) {
  if (type == 4140)
    return IM_COL32(95, 235, 130, 255);
  if (type == 4040 || type == 5000)
    return IM_COL32(255, 105, 90, 255);
  if (type == 3100 || type == 3200 || type == 3210)
    return IM_COL32(110, 200, 255, 255);
  if (type == 2100 || type == 2200)
    return IM_COL32(240, 130, 240, 255);
  return IM_COL32(205, 195, 120, 220);
}
} // namespace
void MapEditor::configure(std::string base, std::string data,
                          std::string game) {
  close_requested_ = false;
  base_ = std::move(base);
  data_ = std::move(data);
  game_ = std::move(game);
  attempt([&] { refresh(); });
}
void MapEditor::attempt(const std::function<void()> &f) {
  try {
    f();
    is_error_ = false;
  } catch (const std::exception &e) {
    message_ = e.what();
    is_error_ = true;
  }
}
void MapEditor::refresh() {
  levels_.clear();
  projects_.clear();
  const auto root = fs::u8path(data_) / "files/Levels";
  if (fs::is_directory(root))
    for (const auto &e : fs::directory_iterator(root))
      if (e.is_regular_file() && lower(e.path().extension().string()) == ".ssw")
        levels_.push_back(e.path().stem().string());
  std::sort(levels_.begin(), levels_.end());
  const auto projects = fs::u8path(base_) / "MapProjects";
  if (fs::is_directory(projects))
    for (const auto &e : fs::directory_iterator(projects))
      if (e.is_directory() && !e.is_symlink() &&
          fs::is_regular_file(e.path() / "project.json"))
        projects_.push_back(e.path().filename().string());
  std::sort(projects_.begin(), projects_.end());
  level_index_ =
      std::clamp(level_index_, 0, std::max(0, int(levels_.size()) - 1));
  project_index_ =
      std::clamp(project_index_, 0, std::max(0, int(projects_.size()) - 1));
}
void MapEditor::hit(const char *id) {
  if (ImGui::IsItemVisible())
    hits_.push_back({id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
}
bool MapEditor::button(const char *label, const char *id, bool enabled) {
  ImGui::BeginDisabled(!enabled);
  bool clicked = ImGui::Button(label);
  hit(id);
  ImGui::EndDisabled();
  if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
    const auto tip=mapedit::editor_tool_help(id,help_language_);
    if(!tip.empty()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(ImGui::GetFontSize()*28);ImGui::TextUnformatted(tip.data(),tip.data()+tip.size());ImGui::PopTextWrapPos();ImGui::EndTooltip();}
  }
  return clicked;
}
void MapEditor::select(int index) {
  const auto before=selected_;
  drag3d_ = false;
  index = index >= 0 && !doc_.objects().empty()
              ? std::min(index, int(doc_.objects().size()) - 1)
              : -1;
  selected_ = index;
  if(index>=0 && before!=index)lesson_event(mapedit::HelpEvent::Select);
  logic_selected_ = UINT32_MAX;
  if (index >= 0) {
    auto at = doc_.objects()[index].offset + 20;
    const auto &b = doc_.payload();
    logic_selected_ = uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 |
                      uint32_t(b[at + 2]) << 8 | b[at + 3];
  }
  turn_ = 0;
  auto wave =
      doc_.wave_trigger(index < 0 ? doc_.objects().size() : size_t(index));
  wave_selected_ = bool(wave);
  if (wave)
    wave_draft_ = *wave;
  if (index >= 0) {
    auto m = doc_.matrix(size_t(index));
    position_[0] = m[3];
    position_[1] = m[7];
    position_[2] = m[11];
  }
}
void MapEditor::refresh_object_icons(){
  if(icon_revision_==doc_.structure_revision()&&object_icons_.size()==doc_.objects().size())return;
  object_icons_.clear();object_icons_.reserve(doc_.objects().size());
  object_helpers_.clear();object_helpers_.reserve(doc_.objects().size());
  for(const auto&o:doc_.objects())object_icons_.push_back(mapedit::logic_icon_for(o.type));
  for(size_t i=0;i<doc_.objects().size();++i)object_helpers_.push_back(doc_.is_logic_visual(i));
  for(const auto&n:logic_graph_.nodes)if(n.object>=0&&size_t(n.object)<object_icons_.size())object_icons_[size_t(n.object)]=mapedit::logic_icon_for(n.type,n.role);
  icon_revision_=doc_.structure_revision();
}
mapedit::LogicIcon MapEditor::object_icon(size_t i)const{return i<object_icons_.size()?object_icons_[i]:mapedit::logic_icon_for(doc_.objects()[i].type);}
void MapEditor::request(const std::function<void()> &action) {
  if (doc_.changed()) {
    pending_ = action;
    ImGui::OpenPopup("Unsaved map changes");
  } else
    attempt(action);
}
void MapEditor::open_level() {
  if (levels_.empty())
    return;
  mapedit::Document next;
  next.load(fs::u8path(data_) / "files/Levels" /
            (levels_[level_index_] + ".ssw"));
  doc_ = std::move(next);
  icon_revision_=UINT64_MAX;graph_project_.clear();graph_positions_.clear();graph_slots_revision_=UINT64_MAX;
  reset_link_edit();
  logic_revision_ = std::numeric_limits<uint64_t>::max();
  level_ = levels_[level_index_];
  project_ = {};
  std::snprintf(name_, sizeof(name_), "%s_edit", level_.c_str());
  select(0);
  load_scene();
  load_custom_metadata();
  help_matrix_valid_=help_camera_valid_=false;help_logic_revision_=UINT64_MAX;
  lesson_event(mapedit::HelpEvent::Loaded);
  message_ =
      "Original opened read-only. Save a project to create your own map copy.";
}
void MapEditor::open_project() {
  if (projects_.empty())
    return;
  mapedit::Document next;
  auto project = mapedit::Project::open(fs::u8path(base_) / "MapProjects" /
                                            projects_[project_index_],
                                        fs::u8path(data_), next);
  doc_ = std::move(next);
  icon_revision_=UINT64_MAX;graph_project_.clear();graph_positions_.clear();graph_slots_revision_=UINT64_MAX;
  reset_link_edit();
  logic_revision_ = std::numeric_limits<uint64_t>::max();
  project_ = std::move(project);
  level_ = project_.level;
  std::snprintf(name_, sizeof(name_), "%s", project_.title.c_str());
  select(0);
  load_scene();
  load_custom_metadata();
  help_matrix_valid_=help_camera_valid_=false;help_logic_revision_=UINT64_MAX;
  lesson_event(mapedit::HelpEvent::Loaded);
  message_ = "Project opened. Original game maps are unchanged.";
}
void MapEditor::save() {
  if (!loaded())
    return;
  if (project_.folder.empty())
    project_ = mapedit::Project::create(fs::u8path(base_) / "MapProjects",
                                        name_, level_);
  project_.save(doc_);
  save_graph_layout();
  lesson_event(mapedit::HelpEvent::Save);
  saved_export_ = gc::sha256_file(
      (project_.folder / "game_data/files/Levels" / (level_ + ".ssw"))
          .string());
  refresh();
  message_ = "Saved " + project_.title + ". Map copy: MapProjects/" +
             project_.folder.filename().string() + "/game_data/files/Levels/" +
             level_ + ".ssw";
}
void MapEditor::test(bool survival, bool logic) {
  save();
  auto project = project_;
  auto data = fs::u8path(data_);
  auto game = fs::u8path(game_);
  auto card = fs::u8path(base_) / "Multiplayer/saves/Host-MemoryCardA.EUR.raw";
  if (!fs::is_regular_file(card))
    card = fs::u8path(base_) / "saves/MemoryCardA.EUR.raw";
  std::string debug_file, debug_token;
  if (logic) {
    std::random_device entropy;
    debug_token =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(entropy());
    debug_file =
        (project.folder / "test" / ("editor-logic-" + debug_token + ".json"))
            .string();
    logic_token_ = debug_token;
    logic_file_ = debug_file;
    logic_project_ = project.folder.string();
    logic_export_ = saved_export_;
    logic_sequence_ = -1;
    logic_live_ = {};
    logic_fresh_ = logic_stream_ready_ = false;
    logic_error_.clear();
    logic_received_ = {};
    logic_window_ = true;
  }
  message_ = "Preparing a separate test copy...";
  test_future_ = std::async(std::launch::async, [project, data, game, card,
                                                 survival, logic, debug_file,
                                                 debug_token] {
    auto exe = survival ? project.prepare_survival_test(data, game, card)
                        : project.prepare_test(data, game, card);
    auto run = exe.parent_path();
    std::string error;
    std::vector<std::string> args{
        "--data", (project.folder / "game_data").string(), "--memcard",
        (run / "MemoryCardA.EUR.raw").string()};
    if (logic) {
      args.insert(args.end(),
                  {"--editor-debug", debug_file, "--editor-debug-token",
                   debug_token, "--lan-port", "0"});
      if (!survival)
        args.insert(args.end(), {"--coop-host", "--coop-level", project.level});
    }
    if (survival) {
      args.insert(args.end(), {"--survival", "--coop-level", project.level,
                               "--survival-project-resources"});
    }
    if (!platform::launch(exe.string(), args, run.string(), error))
      throw std::runtime_error("Map test could not start: " + error);
    if (logic)
      return std::string(
          "Logic test started on this project. Live observations appear in "
          "Logic / Test; game input stays in the game window.");
    if (survival)
      return std::string(
          "Survival starts on this project map after choosing a game "
          "profile. Other players need the same edited project data.");
    return "Test game started with this project's map copy. Select " +
           project.level + " in the original game menus.";
  });
}

bool MapEditor::matches(size_t i) const {
  if(i<object_helpers_.size()&&object_helpers_[i])return false;
  auto &o = doc_.objects()[i];
  if (filter_ == 1 && o.type != 4140)
    return false;
  if (filter_ == 2 && o.type != 4040 && o.type != 5000)
    return false;
  if (filter_ == 3 && o.type != 3100 && o.type != 3200 && o.type != 3210 &&
      o.type != 2100 && o.type != 2200)
    return false;
  if(filter_==4 && !(o.type>=4010&&o.type<=4340&&o.type!=4140&&o.type!=4200&&o.type!=4300))return false;
  if(filter_==5 && o.type!=4200)return false;
  if(filter_==6 && o.type!=4260)return false;
  auto query = lower(search_);
  return query.empty() || lower(o.name).find(query) != std::string::npos ||
         lower(mapedit::category(o.type)).find(query) != std::string::npos;
}
void MapEditor::fit_view(ImVec2 size) {
  float minx = 0, maxx = 0, minz = 0, maxz = 0;
  bool first = true;
  for (size_t i = 0; i < doc_.objects().size(); ++i)
    if (matches(i)) {
      auto m = doc_.matrix(i);
      if (first) {
        minx = maxx = m[3];
        minz = maxz = m[11];
        first = false;
      } else {
        minx = std::min(minx, m[3]);
        maxx = std::max(maxx, m[3]);
        minz = std::min(minz, m[11]);
        maxz = std::max(maxz, m[11]);
      }
    }
  centre_x_ = (minx + maxx) * .5f;
  centre_z_ = (minz + maxz) * .5f;
  zoom_ = std::clamp(std::min((size.x - 40) / std::max(10.f, maxx - minx),
                              (size.y - 60) / std::max(10.f, maxz - minz)),
                     .02f, 100.f);
  fit_ = false;
}
void MapEditor::viewport_top(ImVec2 size) {
  size.x = std::max(size.x, 50.f);
  size.y = std::max(size.y, 80.f);
  auto p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##map_view", size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle |
                             ImGuiButtonFlags_MouseButtonRight);
  hit("map_view");
  auto dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(p, p + size, true);
  dl->AddRectFilled(p, p + size, IM_COL32(8, 13, 35, 245));
  if (fit_)
    fit_view(size);
  auto centre = p + size * .5f;
  auto screen = [&](float x, float z) {
    return ImVec2(centre.x + (x - centre_x_) * zoom_,
                  centre.y - (z - centre_z_) * zoom_);
  };
  auto &io = ImGui::GetIO();
  bool hovered = ImGui::IsItemHovered();
  if (hovered && io.MouseWheel != 0) {
    float old = zoom_;
    zoom_ = std::clamp(zoom_ * std::pow(1.2f, io.MouseWheel), .02f, 100.f);
    centre_x_ += (io.MousePos.x - centre.x) * (1.f / old - 1.f / zoom_);
    centre_z_ -= (io.MousePos.y - centre.y) * (1.f / old - 1.f / zoom_);
  }
  if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                  ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
    centre_x_ -= io.MouseDelta.x / zoom_;
    centre_z_ += io.MouseDelta.y / zoom_;
  }
  float grid = std::pow(10.f, std::ceil(std::log10(30.f / zoom_)));
  for (float x = std::floor((centre_x_ - size.x / (2 * zoom_)) / grid) * grid;
       x < centre_x_ + size.x / (2 * zoom_); x += grid) {
    auto q = screen(x, 0);
    dl->AddLine(ImVec2(q.x, p.y), ImVec2(q.x, p.y + size.y),
                IM_COL32(65, 85, 130, 75));
  }
  for (float z = std::floor((centre_z_ - size.y / (2 * zoom_)) / grid) * grid;
       z < centre_z_ + size.y / (2 * zoom_); z += grid) {
    auto q = screen(0, z);
    dl->AddLine(ImVec2(p.x, q.y), ImVec2(p.x + size.x, q.y),
                IM_COL32(65, 85, 130, 75));
  }
  auto origin = screen(0, 0);
  dl->AddLine(ImVec2(origin.x, p.y), ImVec2(origin.x, p.y + size.y),
              IM_COL32(110, 160, 225, 150));
  dl->AddLine(ImVec2(p.x, origin.y), ImVec2(p.x + size.x, origin.y),
              IM_COL32(110, 160, 225, 150));
  const bool logic_clicked = draw_logic_connections(dl, p, size, hovered);
  const bool link_picking = draw_link_picking(dl, p, size, hovered);
  if(const auto node=logic_graph_.find(logic_selected_);node) {
    auto pos=logic_position(*node);
    if(node->object==selected_ && dragging_) {
      pos.x=drag_matrix_[3]+(io.MousePos.x-drag_mouse_.x)/zoom_;
      pos.z=drag_matrix_[11]-(io.MousePos.y-drag_mouse_.y)/zoom_;
    }
    const auto volume_color=mapedit::logic_icon_color(mapedit::logic_icon_for(node->type,node->role));
    if(const auto box=mapedit::logic_box_bounds(*node,{pos.x,pos.y,pos.z}))
      dl->AddRect(screen(box->minimum[0],box->maximum[2]),
                  screen(box->maximum[0],box->minimum[2]),volume_color,0,0,1.5f);
    else if(node->type!=4050 && std::isfinite(node->radius) &&
            node->radius>0 && node->radius<100000)
      dl->AddCircle(screen(pos.x,pos.z),node->radius*zoom_,volume_color,64,1.5f);
  }
  int nearest = -1;
  float best = 100;
  std::vector<mapedit::LogicIconCandidate> icon_candidates;
  std::vector<ImVec2> icon_positions(doc_.objects().size());
  for (size_t i = 0; i < doc_.objects().size(); ++i)
    if (matches(i)) {
      auto m = doc_.matrix(i);
      auto q = screen(m[3], m[11]);
      if (int(i) == selected_ && dragging_)
        q = screen(drag_matrix_[3] + (io.MousePos.x - drag_mouse_.x) / zoom_,
                   drag_matrix_[11] - (io.MousePos.y - drag_mouse_.y) / zoom_);
      float d = (q.x - io.MousePos.x) * (q.x - io.MousePos.x) +
                (q.y - io.MousePos.y) * (q.y - io.MousePos.y);
      if (d < best) {
        best = d;
        nearest = int(i);
      }
      auto c = color(doc_.objects()[i].type);
      icon_candidates.push_back({i,q.x-p.x,q.y-p.y,1,false});
      icon_positions[i]=q;
      if (int(i) == selected_) {
        dl->AddCircle(q, 9, theme::kYellow, 20, 2);
        dl->AddText(q + ImVec2(12, -10), theme::kYellow,
                    doc_.objects()[i].name.empty()
                        ? mapedit::category(doc_.objects()[i].type)
                        : doc_.objects()[i].name.c_str());
      }
    }
  for(auto& candidate:icon_candidates)candidate.priority=int(candidate.object)==selected_||(hovered&&int(candidate.object)==nearest);
  const auto visible_icons=mapedit::layout_logic_icons(icon_candidates,size.x,size.y,18);
  for(const auto i:visible_icons){const auto q=icon_positions[i];mapedit::draw_logic_icon(dl,q,int(i)==selected_?9.f:6.f,object_icon(i),int(i)==selected_,hovered&&int(i)==nearest);}
  bool placing = placement_ >= 0;
  if (placing && hovered) {
    auto v = mapedit::V3{centre_x_ + (io.MousePos.x - centre.x) / zoom_,
                         camera_.target.y,
                         centre_z_ - (io.MousePos.y - centre.y) / zoom_};
    if (io.KeyShift)
      v = mapedit::snapped(v, .25f);
    dl->AddCircle(io.MousePos, 12, theme::kYellow, 24, 2);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      place_at(v);
  }
  if (!logic_clicked && !link_picking && !placing && hovered && nearest >= 0 &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    select(nearest);
    drag_matrix_ = doc_.matrix(nearest);
    drag_mouse_ = io.MousePos;
    dragging_ = true;
  }
  if (dragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (std::hypot(io.MousePos.x - drag_mouse_.x,
                   io.MousePos.y - drag_mouse_.y) > 3.f) {
      auto m = drag_matrix_;
      m[3] += (io.MousePos.x - drag_mouse_.x) / zoom_;
      m[11] -= (io.MousePos.y - drag_mouse_.y) / zoom_;
      attempt([&] {
        doc_.set_matrix(selected_, m);
        select(selected_);
      });
    }
    dragging_ = false;
  }
  if (hovered && nearest >= 0 && !dragging_) {
    ImGui::BeginTooltip();
    auto &o = doc_.objects()[nearest];
    ImGui::Text("%s", o.name.empty() ? "Unnamed object" : o.name.c_str());
    ImGui::TextDisabled("%s", mapedit::category(o.type));
    ImGui::EndTooltip();
  }
  dl->AddText(
      p + ImVec2(10, 10), IM_COL32(205, 220, 250, 255),
      "TOP VIEW  X / Z  |  Object positions; level geometry is not shown");
  dl->PopClipRect();
}
bool MapEditor::draw() {
  if (test_future_.valid() && test_future_.wait_for(std::chrono::seconds(0)) ==
                                  std::future_status::ready)
    attempt([&] { message_ = test_future_.get();lesson_event(mapedit::HelpEvent::Test); });
  hits_.clear();
  refresh_logic();
  refresh_object_icons();
  poll_logic();
  bool stay = true;
  auto vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("##map_editor", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings);
  if(help_practice_requested_){help_practice_requested_=false;request([this]{start_practice();});}
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kGold);
  ImGui::TextUnformatted("SERIOUS SAM - OBJECT & SPAWN EDITOR");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("Local preview 0.13 - Scene & Logic tools");
  ImGui::SameLine(ImGui::GetWindowWidth() - 95);
  if (button("Back", "back", !test_future_.valid()))
    request([this] {
      reset_link_edit();
      doc_ = {};
      release_capture();
      scene_ = {};
      appearance_ = {};
      appearance_error_.clear();
      scene_error_.clear();
      drag3d_ = false;
      project_ = {};
      level_.clear();
      selected_ = -1;
      close_requested_ = true;
    });
  ImGui::Separator();
  ImGui::SetNextItemWidth(180);
  if (ImGui::BeginCombo("##level", levels_.empty()
                                       ? "Install game first"
                                       : levels_[level_index_].c_str())) {
    for (int i = 0; i < int(levels_.size()); ++i) {
      if (ImGui::Selectable(levels_[i].c_str(), i == level_index_))
        level_index_ = i;
      hit(("level_" + levels_[i]).c_str());
    }
    ImGui::EndCombo();
  }
  hit("level");
  ImGui::SameLine();
  if (button("Open original", "open_original",
             !levels_.empty() && !test_future_.valid()))
    request([&] { open_level(); });
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140);
  if (ImGui::BeginCombo("##project", projects_.empty()
                                         ? "Saved projects"
                                         : projects_[project_index_].c_str())) {
    for (int i = 0; i < int(projects_.size()); ++i)
      if (ImGui::Selectable(projects_[i].c_str(), i == project_index_))
        project_index_ = i;
    ImGui::EndCombo();
  }
  hit("projects");
  ImGui::SameLine();
  if (button("Open project", "open_project",
             !projects_.empty() && !test_future_.valid()))
    request([&] { open_project(); });
  ImGui::SameLine();
  if (button("Refresh", "refresh"))
    attempt([&] { refresh(); });
  ImGui::SetNextItemWidth(150);
  ImGui::BeginDisabled(!project_.folder.empty());
  ImGui::InputText("Project name", name_, sizeof(name_));
  hit("project_name");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Save project", "save", loaded() && !test_future_.valid()))
    attempt([&] { save(); });
  ImGui::SameLine();
  if (button("Test in game", "test",
             loaded() && !test_future_.valid() &&
                 fs::is_regular_file(fs::u8path(game_))))
    attempt([&] { test(); });
  ImGui::SameLine();
  if (button("Undo", "undo", doc_.can_undo() && !drag3d_)) {
    doc_.undo();
    select(selected_);
  }
  ImGui::SameLine();
  if (button("Redo", "redo", doc_.can_redo() && !drag3d_)) {
    doc_.redo();
    select(selected_);
  }
  ImGui::SameLine();
  if (button("Fit view", "fit", loaded()))
    fit_ = true;
  ImGui::SameLine();
  if (ImGui::Checkbox("3D view", &view3d_)) {
    drag3d_ = false;
    transform_drag_.active=false;pending_drag_object_=-1;release_capture();
    fit_ = !view3d_;
  }
  hit("view3d");
  if (!drag3d_ && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z) && doc_.undo())
      select(selected_);
    if (ImGui::IsKeyPressed(ImGuiKey_Y) && doc_.redo())
      select(selected_);
    if (ImGui::IsKeyPressed(ImGuiKey_S) && loaded() && !test_future_.valid())
      attempt([&] { save(); });
  }
  structure_tools();
  sync_structure();
  refresh_logic();
  sync_link_edit();
  refresh_object_icons();
  logic_toolbar();
  custom_toolbar();
  help_toolbar();
  ImGui::TextDisabled("%s | %zu objects | %zu edited%s",
                      level_.empty() ? "No map loaded" : level_.c_str(),
                      doc_.objects().size(), doc_.edited_objects(),
                      doc_.changed() ? " | UNSAVED" : "");
  if (!message_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          is_error_ ? theme::kBad : theme::kGood);
    ImGui::TextWrapped("%s", message_.c_str());
    ImGui::PopStyleColor();
  }
  float available = ImGui::GetContentRegionAvail().y - 32;
  ImGui::BeginChild("objects", ImVec2(260, available), ImGuiChildFlags_Borders);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##search", "Search objects...", search_,
                           sizeof(search_));
  hit("search");
  const char *filters[] = {"All objects", "Player spawns", "Enemies / spawners",
                           "Weapons / supplies","Logic / triggers","Scene props","Arrow markers"};
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##filter", &filter_, filters, 7))
    fit_ = !view3d_;
  hit("filter");
  ImGui::Separator();
  ImGui::BeginChild("list");
  for (size_t i = 0; i < doc_.objects().size(); ++i)
    if (matches(i)) {
      auto &o = doc_.objects()[i];
      ImGui::PushID(int(i));
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::ColorConvertU32ToFloat4(color(o.type)));
      std::string label = o.name.empty() ? mapedit::category(o.type) : o.name;
      auto icon=object_icon(i);
      if(icon!=mapedit::LogicIcon::Object){auto at=ImGui::GetCursorScreenPos();mapedit::draw_logic_icon(ImGui::GetWindowDrawList(),at+ImVec2(9,ImGui::GetTextLineHeight()*.5f),7,icon,selected_==int(i));ImGui::Dummy(ImVec2(20,ImGui::GetTextLineHeight()));ImGui::SameLine(0,2);}
      if (ImGui::Selectable(label.c_str(), selected_ == int(i)))
        select(int(i));
      hit(("object_" + std::to_string(i)).c_str());
      ImGui::PopStyleColor();
      ImGui::PopID();
    }
  ImGui::EndChild();
  ImGui::EndChild();
  ImGui::SameLine();
  float prop = 245;
  float width = std::max(100.f, ImGui::GetContentRegionAvail().x - prop - 8);
  ImGui::BeginChild("view", ImVec2(width, available), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  viewport(ImGui::GetContentRegionAvail());
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("properties", ImVec2(0, available),
                    ImGuiChildFlags_Borders);
  if (link_mode_)
    link_properties();
  else if (selected_ >= 0) {
    auto &o = doc_.objects()[selected_];
    ImGui::TextColored(theme::kGold, "Selected object");
    ImGui::TextWrapped("%s", o.name.empty() ? "Unnamed" : o.name.c_str());
    ImGui::TextDisabled("%s", mapedit::category(o.type));
    ImGui::Separator();
    for (unsigned k = 0; k < 3; ++k) {
      const char *label = k == 0 ? "X" : k == 1 ? "Y (height)" : "Z";
      ImGui::SetNextItemWidth(145);
      ImGui::InputFloat(label, position_ + k, .25f, 1.f, "%.3f");
      hit(k == 0 ? "position_x" : k == 1 ? "position_y" : "position_z");
    }
    if (button("Apply position", "apply_position",mapedit::transform_mode_available(doc_.transform_type(size_t(selected_)),mapedit::TransformMode::Move)))
      attempt([&] {
        doc_.move(selected_, {position_[0], position_[1], position_[2]});
        select(selected_);
      });
    ImGui::SetNextItemWidth(145);
    ImGui::InputFloat("Turn (degrees)", &turn_, 15, 90, "%.1f");
    hit("turn");
    if (button("Rotate around Y", "rotate",mapedit::transform_mode_available(doc_.transform_type(size_t(selected_)),mapedit::TransformMode::Rotate)))
      attempt([&] {
        doc_.rotate_y(selected_, turn_);
        select(selected_);
      });
    if (button("Reset object", "reset"))
      attempt([&] {
        doc_.set_matrix(selected_, doc_.original_matrix(selected_));
        select(selected_);
      });
    if (button("Focus view", "focus")) {
      auto m = doc_.matrix(selected_);
      if (view3d_)
        camera_.focus({m[3], m[7], m[11]});
      lesson_event(mapedit::HelpEvent::Focus);
      centre_x_ = m[3];
      centre_z_ = m[11];
      zoom_ = std::max(zoom_, 2.f);
    }
    ImGui::Separator();
    for (auto &preview : appearance_.previews)
      if (preview.object == selected_) {
        ImGui::TextWrapped("Model: %s", preview.model.empty()
                                            ? "None"
                                            : preview.model.c_str());
        if (!preview.pose.empty())
          ImGui::TextWrapped("Pose: %s (%zu bones)", preview.pose.c_str(),
                             preview.bones);
        if (!preview.error.empty())
          ImGui::TextWrapped("Preview: %s", preview.error.c_str());
        break;
      }
    ImGui::TextWrapped("W: Move | E: Rotate | R: Scale. Drag the coloured "
                       "handles; Escape cancels. The toolbar offers snapping "
                       "and World / Local axes. Unsupported native transforms "
                       "are disabled for the selected entity.");
    ImGui::TextDisabled("Hold right mouse: look + WASD. Q/E: down/up. Shift: "
                        "fast. Alt + left drag orbits the pivot; middle drag pans.\nMouse wheel "
                        "zooms; F focuses selection.");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Add objects from this map's templates, duplicate with Ctrl+D, or "
        "delete "
        "with Delete. Story scripts and the last player start are protected. "
        "original textures, objects and enemy model previews. Script-only "
        "points keep their icons.");
  } else
    ImGui::TextWrapped("Open a map and select an object.");
  if (wave_selected_ && !link_mode_)
    wave_properties();
  if(!link_mode_ && logic_selected_!=UINT32_MAX)logic_properties();
  ImGui::EndChild();
  ImGui::TextDisabled(
      "Original maps stay unchanged. Test uses a separate map copy and memory "
      "card; select the source level in the game.");
  if (ImGui::BeginPopupModal("Unsaved map changes", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Save your changes before leaving this map?");
    if (button("Save and continue", "unsaved_save")) {
      attempt([&] {
        save();
        if (pending_)
          pending_();
        pending_ = {};
      });
      if (!is_error_)
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Discard changes", "unsaved_discard")) {
      attempt([&] {
        if (pending_)
          pending_();
        pending_ = {};
      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Cancel", "unsaved_cancel")) {
      pending_ = {};
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  structure_popups();
  logic_creation_popup();
  custom_modal();
  ImGui::End();
  logic_panel();
  graph_panel();
  help_panel();
  return !close_requested_;
}
launcher::JSONValue MapEditor::test_state() const {
  using J = launcher::JSONValue;
  J::Array objects, hits;
  for (size_t i = 0; i < doc_.objects().size(); ++i) {
    auto m = doc_.matrix(i);
    auto &o = doc_.objects()[i];
    J::Array transform;for(float v:m)transform.emplace_back(double(v));
    objects.emplace_back(J::Object{
        {"index", int64_t(i)},
        {"added", o.added},
        {"offset", int64_t(o.offset)},
        {"transform",int64_t(o.transform)},
        {"can_duplicate", doc_.can_duplicate(i)},
        {"can_delete", doc_.can_erase(i)},
        {"name", o.name},
        {"type", int64_t(o.type)},
        {"transform_type",int64_t(doc_.transform_type(i))},
        {"matrix",std::move(transform)},
        {"category", mapedit::category(o.type)},
        {"position", J::Array{double(m[3]), double(m[7]), double(m[11])}}});
  }
  for (auto &r : hits_)
    hits.emplace_back(
        J::Object{{"id", r.id},
                  {"bounds", J::Array{double(r.a.x), double(r.a.y),
                                      double(r.b.x), double(r.b.y)}}});
  J::Array visual_objects, visual_matrices;
  for (auto &b : appearance_.batches)
    if (selected_>=0 && (b.object == selected_ || b.matrix_offset==doc_.objects()[size_t(selected_)].transform)) {
      auto m = appearance_.model_matrix(b, doc_, drag3d_ ? selected_ : -1,
                                        drag3d_preview_,transform_drag_.active?&transform_drag_.preview:nullptr);
      J::Array values;
      for (float v : m)
        values.emplace_back(double(v));
      visual_matrices.emplace_back(std::move(values));
      visual_objects.emplace_back(
          J::Array{double(m[3]), double(m[7]), double(m[11])});
      if (visual_objects.size() >= 8)
        break;
    }
  J::Array previews;
  for (auto &p : appearance_.previews)
    previews.emplace_back(J::Object{{"object", p.object},
                                    {"model", p.model},
                                    {"pose", p.pose},
                                    {"role", p.role},
                                    {"error", p.error},
                                    {"bones", int64_t(p.bones)}});
  J::Array waves;
  for (size_t i = 0; i < doc_.objects().size(); ++i)
    if (auto w = doc_.wave_trigger(i)) {
      J::Array rows;
      for (auto &r : w->rows)
        rows.emplace_back(J::Object{{"enemy_offset", int64_t(r.enemy_offset)},
                                    {"count", int64_t(r.count)}});
      waves.emplace_back(
          J::Object{{"object", int64_t(i)},
                    {"radius", double(w->radius)},
                    {"delay", double(w->delay)},
                    {"repeat", w->repeat},
                    {"max_activations", int64_t(w->max_activations)},
                    {"rows", std::move(rows)}});
    }
  J::Array templates;
  for (size_t i = 0; i < donor_.objects().size(); ++i) {
    auto &o = donor_.objects()[i];
    if (donor_.can_duplicate(i))
      templates.emplace_back(J::Object{
          {"index", int64_t(i)}, {"name", o.name}, {"type", int64_t(o.type)}});
  }
  auto eye = camera_.eye();
  return J::Object{
      {"view3d", view3d_},
      {"logic", logic_test_state()},
      {"help",help_test_state()},
      {"graph",graph_test_state()},
      {"custom_level", custom_test_state()},
      {"waves", std::move(waves)},
      {"placement_template", placement_},
      {"placement_foreign", placement_foreign_ && placement_ >= 0},
      {"palette_source_map", palette_map_ < 0 ? level_ : donor_level_},
      {"foreign_templates", std::move(templates)},
      {"structure_revision", int64_t(doc_.structure_revision())},
      {"scene_revision",int64_t(scene_revision_)},
      {"scene_triangles", int64_t(scene_.triangles().size())},
      {"scene_vertices", int64_t(scene_.vertices().size())},
      {"scene_error", scene_error_},
      {"textured", textured_},
      {"selected_visual_positions", std::move(visual_objects)},
      {"selected_visual_matrices", std::move(visual_matrices)},
      {"appearance_error", appearance_error_},
      {"visual_vertices", int64_t(appearance_.vertices.size())},
      {"visual_textures", int64_t(appearance_.textures.size())},
      {"visual_instances", int64_t(appearance_.instances)},
      {"visual_enemies", int64_t(appearance_.enemies)},
      {"visual_arrows",int64_t(appearance_.arrows)},
      {"model_pick",J::Object{{"batches",int64_t(model_pick_stats_.batches)},{"bounds",int64_t(model_pick_stats_.bounds_tested)},{"bounds_hit",int64_t(model_pick_stats_.bounds_hit)},{"triangles",int64_t(model_pick_stats_.triangles_tested)}}},
      {"visual_extra_objects", int64_t(appearance_.extra_objects)},
      {"enemy_models", show_enemy_models_},
      {"model_previews", std::move(previews)},
      {"visual_pickups", int64_t(appearance_.pickups)},
      {"visual_warnings", int64_t(appearance_.warnings.size())},
      {"renderer_ready", scene_renderer_.ready()},
      {"camera_eye", J::Array{double(eye.x), double(eye.y), double(eye.z)}},
      {"camera_pivot",J::Array{double(camera_.target.x),double(camera_.target.y),double(camera_.target.z)}},
      {"camera_distance",double(camera_.distance)},
      {"camera_yaw",double(camera_.yaw)},{"camera_pitch",double(camera_.pitch)},{"camera_width",double(camera_.width)},{"camera_height",double(camera_.height)},
      {"tool",int64_t(transform_mode_)},{"tool_local",transform_local_},{"tool_snap",transform_snap_},{"tool_dragging",transform_drag_.active},{"tool_handle",int64_t(transform_drag_.handle)},
      {"camera_forward",
       J::Array{double(camera_.forward().x), double(camera_.forward().y),
                double(camera_.forward().z)}},
      {"camera_right",
       J::Array{double(camera_.right().x), double(camera_.right().y),
                double(camera_.right().z)}},
      {"loaded", loaded()},
      {"test_preparing", test_future_.valid()},
      {"level", level_},
      {"selected", selected_},
      {"dirty", doc_.changed()},
      {"edited", int64_t(doc_.edited_objects())},
      {"project", project_.folder.string()},
      {"message", message_},
      {"error", is_error_},
      {"objects", std::move(objects)},
      {"controls", std::move(hits)}};
}
