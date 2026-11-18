#pragma once
#include "launcher_json.h"
#include "map_appearance.h"
#include "map_document.h"
#include "map_gl.h"
#include "map_gizmo.h"
#include "map_help.h"
#include "map_logic_icons.h"
#include "map_model_pick.h"
#include "map_logic.h"
#include "map_scene.h"
#include <chrono>
#include <functional>
#include <future>
#include <imgui.h>
#include <limits>
#include <map>

class MapEditor {
public:
  ~MapEditor();
  void configure(std::string base, std::string data, std::string game);
  bool draw(); // false after Back, including unsaved-change confirmation
  launcher::JSONValue test_state() const;
  bool loaded() const { return !doc_.objects().empty(); }

private:
  struct Hit {
    std::string id;
    ImVec2 a, b;
  };
  mapedit::Document doc_;
  mapedit::Project project_;
  mapedit::Scene scene_;
  mapedit::Appearance appearance_;
  mapedit::Camera camera_;
  mapedit::SceneRenderer scene_renderer_;
  uint64_t scene_revision_ = 0;
  std::vector<uint32_t> visual_signature_;
  std::vector<uint32_t> visual_signature()const;
  std::string scene_error_;
  std::string appearance_error_;
  bool textured_ = true;
  bool show_enemy_models_ = true;
  float fly_speed_ = 20;
  bool flight_active_ = false, relative_mouse_ = false;
  mapedit::TransformMode transform_mode_=mapedit::TransformMode::Move;
  bool transform_local_=false,transform_snap_=false;
  float transform_grid_=.25f,transform_angle_=15.f,transform_scale_=.1f;
  mapedit::GizmoDrag transform_drag_;
  int navigation_gesture_=0;
  ImVec2 navigation_mouse_{};
  int pending_drag_object_=-1;
  ImVec2 pending_drag_mouse_{};
  void release_capture();
  bool view3d_ = true, wireframe_ = false, cutaway_ = true, xray_ = true;
  bool drag3d_ = false, drag3d_surface_ = false, drag3d_has_parameter_ = false;
  int drag3d_axis_ = -1;
  mapedit::V3 drag3d_origin_, drag3d_preview_, drag3d_anchor_;
  ImVec2 drag3d_mouse_{};
  float drag3d_parameter_ = 0;
  std::string base_, data_, game_, message_, level_;
  std::vector<std::string> levels_, projects_;
  std::vector<Hit> hits_;
  uint64_t icon_revision_=UINT64_MAX;
  std::vector<mapedit::LogicIcon> object_icons_;
  std::vector<bool> object_helpers_;
  mapedit::ModelPickStats model_pick_stats_;
  void refresh_object_icons();
  mapedit::LogicIcon object_icon(size_t)const;
  int level_index_ = 0, project_index_ = 0, selected_ = -1, filter_ = 0;
  char search_[128]{}, name_[49] = "My_Map";
  float position_[3]{}, turn_ = 0, zoom_ = 1, centre_x_ = 0, centre_z_ = 0;
  bool close_requested_ = false, fit_ = true, dragging_ = false,
       is_error_ = false;
  ImVec2 drag_mouse_{};
  mapedit::Matrix drag_matrix_{};
  std::function<void()> pending_;
  std::future<std::string> test_future_;
  void refresh();
  void select(int);
  void fit_view(ImVec2);
  void attempt(const std::function<void()> &);
  void request(const std::function<void()> &);
  void open_level();
  void open_project();
  void save();
  void test(bool survival = false, bool logic = false);
  void wave_properties();
  void logic_properties();
  void logic_creation_popup();
  mapedit::LogicSpec logic_draft_;
  mapedit::LogicSpec logic_create_draft_;
  char logic_create_name_[96]="New logic";
  uint32_t logic_draft_id_=UINT32_MAX;
  uint64_t logic_draft_revision_=UINT64_MAX;
  int logic_create_type_=0;
  char logic_name_[96]{};
  mapedit::LogicGraph logic_graph_;
  uint64_t logic_revision_ = std::numeric_limits<uint64_t>::max();
  bool connections_ = true, logic_window_ = false, logic_follow_ = true;
  uint32_t logic_selected_ = UINT32_MAX;
  char logic_search_[128]{};
  int logic_filter_ = 0;
  size_t logic_drawn_ = 0;
  launcher::JSONValue logic_live_;
  std::string logic_token_, logic_file_, logic_project_, logic_export_,
      saved_export_, logic_error_;
  int64_t logic_sequence_ = -1;
  std::chrono::steady_clock::time_point logic_poll_{}, logic_received_{};
  bool logic_fresh_ = false, logic_stream_ready_ = false;
  void refresh_logic();
  void poll_logic();
  void logic_toolbar();
  void logic_panel();
  void select_logic(uint32_t, bool focus = false);
  mapedit::V3 logic_position(const mapedit::LogicNode &) const;
  std::set<uint32_t> logic_neighborhood() const;
  const launcher::JSONValue *live_logic_node(uint32_t) const;
  const launcher::JSONValue *recorded_logic_node(uint32_t) const;
  bool logic_sample_current() const;
  bool draw_logic_connections(ImDrawList *, ImVec2, ImVec2, bool hovered);
  launcher::JSONValue logic_test_state() const;
  bool link_mode_ = false;
  int link_pick_ = 0, link_slot_ = -1, link_action_ = 0, link_enemy_count_ = 3;
  uint32_t link_source_ = UINT32_MAX, link_target_ = UINT32_MAX;
  uint64_t link_revision_ = std::numeric_limits<uint64_t>::max();
  std::vector<uint32_t> link_pick_candidates_;
  char link_search_[96]{};
  void reset_link_edit();
  void sync_link_edit();
  void choose_link_node(uint32_t);
  void choose_link_edge(const mapedit::LogicEdge &);
  void link_properties();
  void apply_link(bool remove = false);
  bool draw_link_picking(ImDrawList *, ImVec2, ImVec2, bool);
  launcher::JSONValue link_test_state() const;
  char custom_title_[129]{}, custom_id_[49]{}, custom_png_input_[1024]{};
  uint32_t custom_modes_ = 35, custom_preview_texture_ = 0;
  unsigned custom_preview_width_ = 0, custom_preview_height_ = 0;
  std::string custom_preview_path_, custom_export_alias_, custom_export_folder_;
  void custom_toolbar();
  void custom_modal();
  void load_custom_metadata();
  void save_custom_metadata();
  void import_custom_preview(const std::string &);
  void capture_custom_preview();
  void release_custom_preview();
  void export_custom_level();
  launcher::JSONValue custom_test_state() const;
  mapedit::Tutorial tutorial_;
  mapedit::HelpEvents help_events_{};
  bool help_open_=false, help_loaded_=false, help_matrix_valid_=false,
       help_camera_valid_=false,help_practice_requested_=false;
  unsigned help_language_=1;
  char help_search_[128]{};
  int help_topic_=0, help_matrix_object_=-1;
  mapedit::Matrix help_last_matrix_{};
  mapedit::V3 help_last_eye_{};
  float help_last_yaw_=0,help_last_pitch_=0;
  uint64_t help_logic_revision_=UINT64_MAX;
  std::set<uint32_t> help_logic_ids_;
  std::string help_logic_signature_;
  size_t help_link_count_=0;
  void help_toolbar();
  void help_panel();
  void observe_help();
  void load_help_preferences();
  void save_help_preferences();
  void start_practice();
  void lesson_event(mapedit::HelpEvent e){++help_events_[unsigned(e)];}
  launcher::JSONValue help_test_state()const;
  bool graph_open_=false,graph_dragging_=false;
  bool graph_show_all_=false;
  ImVec2 graph_pan_{40,60};
  float graph_zoom_=1;
  uint32_t graph_link_from_=UINT32_MAX,graph_selection_=UINT32_MAX;
  int graph_link_slot_=-1,graph_action_=0;
  uint32_t graph_drag_node_=UINT32_MAX,graph_edge_source_=UINT32_MAX;
  int graph_edge_slot_=-1;
  char graph_target_search_[96]{};
  ImVec2 graph_drag_start_{},graph_drag_position_{},graph_link_mouse_{};
  std::map<uint32_t,ImVec2> graph_positions_;
  std::map<uint32_t,std::vector<mapedit::LogicSlot>> graph_slots_;
  uint64_t graph_slots_revision_=UINT64_MAX;
  std::string graph_project_;
  void graph_panel();
  void graph_auto_layout();
  void load_graph_layout();
  void save_graph_layout();
  launcher::JSONValue graph_test_state()const;
  mapedit::WaveTrigger wave_draft_;
  bool wave_selected_ = false;
  void viewport(ImVec2);
  void load_scene(bool focus = true);
  uint64_t document_revision_ = 0;
  int placement_ = -1, palette_kind_ = 0;
  int palette_map_ = -1;
  bool placement_foreign_ = false;
  mapedit::Document donor_;
  std::string donor_level_;
  std::vector<mapedit::Appearance::Preview> donor_previews_;
  char palette_search_[128]{};
  void structure_tools();
  void structure_popups();
  void sync_structure();
  void place_at(mapedit::V3);
  void duplicate_selected();
  void viewport_3d(ImVec2);
  void viewport_top(ImVec2);
  void hit(const char *id);
  bool button(const char *, const char *, bool enabled = true);
  bool matches(size_t) const;
};
