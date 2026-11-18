#include "map_editor.h"
#include "map_logic_icons.h"
#include "theme.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using namespace mapedit;
V3 point(const Matrix &m) { return {m[3], m[7], m[11]}; }
V3 axis(unsigned i) {
  V3 a;
  a[i] = 1;
  return a;
}
ImU32 axis_color(unsigned i) {
  const ImU32 colors[] = {IM_COL32(255, 90, 75, 255),
                          IM_COL32(100, 240, 130, 255),
                          IM_COL32(90, 165, 255, 255)};
  return colors[i];
}
} // namespace
MapEditor::~MapEditor() {
  release_capture();
  release_custom_preview();
}
void MapEditor::release_capture() {
  if (relative_mouse_ && SDL_WasInit(SDL_INIT_VIDEO))
    SDL_SetRelativeMouseMode(SDL_FALSE);
  if (SDL_WasInit(SDL_INIT_VIDEO))
    SDL_CaptureMouse(SDL_FALSE);
  relative_mouse_ = false;
  flight_active_ = false;
  navigation_gesture_ = 0;
}
void MapEditor::load_scene(bool focus) {
  document_revision_ = doc_.structure_revision();
  visual_signature_=visual_signature();
  drag3d_ = false;
  transform_drag_.active = false;
  pending_drag_object_ = -1;
  release_capture();
  scene_error_.clear();
  appearance_error_.clear();
  appearance_ = {};
  try {
    scene_ = mapedit::Scene::parse(doc_.payload());
    ++scene_revision_;
  } catch (const std::exception &e) {
    scene_ = {};
    scene_error_ = e.what();
    return;
  }
  try {
    appearance_ =
        mapedit::Appearance::load(doc_, std::filesystem::u8path(data_));
  } catch (const std::exception &e) {
    appearance_error_ = e.what();
  }
  if (!focus)
    return;
  placement_ = -1;
  palette_map_ = -1;
  donor_ = {};
  donor_level_.clear();
  donor_previews_.clear();
  int spawn = -1;
  for (size_t i = 0; i < doc_.objects().size(); ++i)
    if (doc_.objects()[i].type == 4140) {
      if (spawn < 0)
        spawn = int(i);
      if (doc_.objects()[i].name == "Player Quick Start") {
        spawn = int(i);
        break;
      }
    }
  if (spawn >= 0)
    select(spawn);
  if (selected_ >= 0)
    camera_.focus(point(doc_.matrix(selected_)), 40);
  else
    camera_.fit(scene_.bounds());
  fit_ = false;
}
void MapEditor::viewport(ImVec2 size) {
  if (view3d_)
    viewport_3d(size);
  else
    viewport_top(size);
}
void MapEditor::viewport_3d(ImVec2) {
  using namespace mapedit;
  auto reset_view = [&] {
    release_capture();
    camera_.yaw = .5f;
    camera_.pitch = .65f;
    int spawn = -1;
    for (size_t i = 0; i < doc_.objects().size(); ++i)
      if (doc_.objects()[i].type == 4140) {
        if (spawn < 0)
          spawn = int(i);
        if (doc_.objects()[i].name == "Player Quick Start") {
          spawn = int(i);
          break;
        }
      }
    if (spawn >= 0)
      camera_.focus(point(doc_.matrix(spawn)), 40);
    else
      camera_.fit(scene_.bounds());
  };
  const auto selected_type =
      selected_ >= 0 ? doc_.transform_type(size_t(selected_)) : 0u;
  auto tool = [&](const char *label, const char *id, TransformMode mode) {
    const bool available =
        selected_ >= 0 && transform_mode_available(selected_type, mode);
    ImGui::BeginDisabled(drag3d_ || !available);
    if (ImGui::Selectable(label, transform_mode_ == mode, 0, ImVec2(90, 0)))
      transform_mode_ = mode;
    hit(id);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          "%s\n%s",
          mode == TransformMode::Move ? "Move along axes or planes. W"
          : mode == TransformMode::Rotate
              ? "Rotate using the axis rings. E"
              : "Scale local axes or drag the centre for uniform scale. R",
          transform_limit_reason(selected_type));
  };
  tool("Move [W]", "tool_move", TransformMode::Move);
  ImGui::SameLine();
  tool("Rotate [E]", "tool_rotate", TransformMode::Rotate);
  ImGui::SameLine();
  tool("Scale [R]", "tool_scale", TransformMode::Scale);
  if (selected_ >= 0 &&
      !transform_mode_available(selected_type, transform_mode_))
    transform_mode_ = TransformMode::Move;
  ImGui::BeginDisabled(drag3d_ || transform_mode_ == TransformMode::Scale);
  ImGui::Checkbox("Local axes", &transform_local_);
  hit("gizmo_local");
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Move/rotate in world or local coordinates. Scale always "
                      "follows the model's local axes.");
  ImGui::SameLine();
  ImGui::Checkbox("Snap", &transform_snap_);
  hit("gizmo_snap");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Snap to the configured increment. Hold Shift while "
                      "dragging for temporary snapping.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(85);
  if (transform_mode_ == TransformMode::Move)
    ImGui::DragFloat("m##snap", &transform_grid_, .05f, .01f, 100, "%.2f");
  else if (transform_mode_ == TransformMode::Rotate)
    ImGui::DragFloat("deg##snap", &transform_angle_, 1, 1, 90, "%.0f");
  else
    ImGui::DragFloat("step##snap", &transform_scale_, .01f, .01f, 1, "%.2f");
  hit("gizmo_snap_step");
  const float view_toolbar_right =
      ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  auto next_view_control = [&](const char *label, bool checkbox = false) {
    const auto &style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(label).x +
                        (checkbox ? ImGui::GetFrameHeight() + style.ItemInnerSpacing.x
                                  : style.FramePadding.x * 2.f);
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <=
        view_toolbar_right)
      ImGui::SameLine();
  };
  ImGui::BeginDisabled(selected_ < 0 || drag3d_);
  if (button("Focus [F]", "focus_selection", selected_ >= 0 && !drag3d_)) {
    camera_.focus(point(doc_.matrix(selected_)),
                  std::min(40.f, camera_.distance));
    lesson_event(HelpEvent::Focus);
  }
  ImGui::EndDisabled();
  next_view_control("Reset [Home]");
  if (button("Reset [Home]", "reset_camera", !drag3d_))
    reset_view();
  next_view_control("Cutaway", true);
  ImGui::BeginDisabled(drag3d_);
  ImGui::Checkbox("Cutaway", &cutaway_);
  hit("cutaway");
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(
        "Hide geometry above the current orbit pivot to see inside rooms.");
  next_view_control("View options...");
  if (button("View options...", "view_options", !drag3d_)) {
    release_capture();
    ImGui::OpenPopup("3D view options");
  }
  if (ImGui::BeginPopup("3D view options")) {
    ImGui::Checkbox("Wireframe", &wireframe_);
    hit("wireframe");
    ImGui::Checkbox("X-ray icons", &xray_);
    hit("xray");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Keep entity symbols visible through walls. Disable to "
                        "respect map occlusion.");
    ImGui::Checkbox("Textures + models", &textured_);
    hit("textured");
    ImGui::Checkbox("Enemy models", &show_enemy_models_);
    hit("enemy_models");
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("Fly speed", &fly_speed_, 2, 150, "%.0f m/s");
    hit("fly_speed");
    ImGui::TextDisabled("RMB + WASD/QE to fly. Shift: faster.");
    ImGui::EndPopup();
  }
  ImGui::TextDisabled("RMB: fly | Alt+LMB: orbit | MMB: pan | Wheel: zoom");
  ImVec2 size = ImGui::GetContentRegionAvail();
  size.x = std::max(50.f, size.x);
  size.y = std::max(80.f, size.y);
  auto p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##map_view_3d", size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  hit("map_view");
  auto &io = ImGui::GetIO();
  bool hovered = ImGui::IsItemHovered();
  const bool hidden = std::getenv("SSNE_TEST_HIDDEN") &&
                      std::string(std::getenv("SSNE_TEST_HIDDEN")) == "1";
  bool navigation_started = false;
  if (hovered && !drag3d_ && !io.WantTextInput && !navigation_gesture_) {
    int gesture = ImGui::IsMouseClicked(ImGuiMouseButton_Right)    ? 1
                  : ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ? 2
                  : io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                      ? 3
                      : 0;
    if (gesture) {
      navigation_gesture_ = gesture;
      navigation_mouse_ = io.MousePos;
      navigation_started = true;
      flight_active_ = gesture == 1;
      pending_drag_object_ = -1;
      if (!hidden) {
        SDL_CaptureMouse(SDL_TRUE);
        if (gesture == 1) {
          relative_mouse_ = SDL_SetRelativeMouseMode(SDL_TRUE) == 0;
          int dx = 0, dy = 0;
          SDL_GetRelativeMouseState(&dx, &dy);
        }
      }
    }
  }
  auto window = SDL_GL_GetCurrentWindow();
  const int navigation_button =
      navigation_gesture_ == 1   ? ImGuiMouseButton_Right
      : navigation_gesture_ == 2 ? ImGuiMouseButton_Middle
                                 : ImGuiMouseButton_Left;
  if (navigation_gesture_ &&
      (!ImGui::IsMouseDown(navigation_button) ||
       (!hidden && window &&
        !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS))))
    release_capture();
  camera_.width = size.x;
  camera_.height = size.y;
  if (fit_ && !scene_.triangles().empty()) {
    camera_.fit(scene_.bounds());
    fit_ = false;
  }
  if ((hovered || navigation_gesture_) && !drag3d_) {
    const auto before_eye = camera_.eye(), before_forward = camera_.forward();
    if (hovered && io.MouseWheel && !navigation_gesture_)
      camera_.zoom(io.MouseWheel);
    if (navigation_gesture_ && !navigation_started) {
      ImVec2 delta = io.MousePos - navigation_mouse_;
      navigation_mouse_ = io.MousePos;
      if (relative_mouse_) {
        int dx = 0, dy = 0;
        SDL_GetRelativeMouseState(&dx, &dy);
        delta = ImVec2(float(dx), float(dy));
      }
      // No delta from the frame which acquired capture is used. SDL may warp
      // its cursor during relative-mode activation; ordinary clicks stay inert.
      if (std::isfinite(delta.x) && std::isfinite(delta.y)) {
        if (navigation_gesture_ == 1)
          camera_.look(delta.x, delta.y);
        else if (navigation_gesture_ == 2)
          camera_.pan(delta.x, delta.y);
        else
          camera_.orbit(delta.x, delta.y);
      }
    }
    if (flight_active_ && !navigation_started && !io.WantTextInput &&
        !io.KeyCtrl && !io.KeyAlt) {
      auto key = [](ImGuiKey k) { return ImGui::IsKeyDown(k) ? 1.f : 0.f; };
      camera_.fly(key(ImGuiKey_D) - key(ImGuiKey_A),
                  key(ImGuiKey_E) - key(ImGuiKey_Q),
                  key(ImGuiKey_W) - key(ImGuiKey_S), io.DeltaTime,
                  fly_speed_ * (io.KeyShift ? 4.f : 1.f));
    }
    if (length(camera_.eye() - before_eye) > .00001f ||
        length(camera_.forward() - before_forward) > .00001f)
      lesson_event(HelpEvent::Navigate);
    if (!io.WantTextInput && !navigation_gesture_ && !io.KeyCtrl &&
        !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F) && selected_ >= 0) {
      camera_.focus(point(doc_.matrix(selected_)),
                    std::min(40.f, camera_.distance));
      lesson_event(HelpEvent::Focus);
    }
    if (!io.WantTextInput && !navigation_gesture_ && !io.KeyCtrl &&
        !io.KeyAlt) {
      if (ImGui::IsKeyPressed(ImGuiKey_W))
        transform_mode_ = TransformMode::Move;
      if (ImGui::IsKeyPressed(ImGuiKey_E) &&
          transform_mode_available(selected_type, TransformMode::Rotate))
        transform_mode_ = TransformMode::Rotate;
      if (ImGui::IsKeyPressed(ImGuiKey_R) &&
          transform_mode_available(selected_type, TransformMode::Scale))
        transform_mode_ = TransformMode::Scale;
      if (ImGui::IsKeyPressed(ImGuiKey_Home))
        reset_view();
    }
  }
  auto dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(p, p + size, true);
  dl->AddRectFilled(p, p + size, IM_COL32(6, 10, 22, 255));
  const float cut = cutaway_ ? camera_.target.y + 6.f
                             : std::numeric_limits<float>::infinity();
  if (!scene_.triangles().empty() && scene_error_.empty()) {
    try {
      auto texture = scene_renderer_.draw(
          scene_, scene_revision_, camera_,
          int(size.x * io.DisplayFramebufferScale.x),
          int(size.y * io.DisplayFramebufferScale.y), cut, wireframe_,
          textured_ && !appearance_.vertices.empty() ? &appearance_ : nullptr,
          doc_, drag3d_ ? selected_ : -1, drag3d_preview_, show_enemy_models_,
          transform_drag_.active ? &transform_drag_.preview : nullptr);
      if (texture)
        dl->AddImage(ImTextureID(texture), p, p + size, ImVec2(0, 1),
                     ImVec2(1, 0));
    } catch (const std::exception &e) {
      scene_error_ = e.what();
    }
  }
  if (!appearance_error_.empty())
    dl->AddText(p + ImVec2(10, 30), theme::kYellow, appearance_error_.c_str());
  if (!scene_error_.empty())
    dl->AddText(p + ImVec2(10, 12), theme::kYellow, scene_error_.c_str());
  auto screen = [&](V3 v) -> std::optional<ImVec2> {
    auto q = camera_.project(v);
    if (!q)
      return {};
    return p + ImVec2(q->x, q->y);
  };
  if (hovered && io.KeyAlt) {
    if (const auto pivot = screen(camera_.target)) {
      dl->AddCircle(*pivot, 6, IM_COL32(255, 215, 90, 255), 20, 1.5f);
      dl->AddLine(*pivot - ImVec2(10, 0), *pivot + ImVec2(10, 0), IM_COL32(255, 215, 90, 255));
      dl->AddLine(*pivot - ImVec2(0, 10), *pivot + ImVec2(0, 10), IM_COL32(255, 215, 90, 255));
      dl->AddText(*pivot + ImVec2(13, 6), IM_COL32(255, 225, 150, 255), "Orbit pivot");
    }
  }
  if (const auto node=logic_graph_.find(logic_selected_);node) {
    auto centre=logic_position(*node);
    if(node->object==selected_ && transform_drag_.active)
      centre=matrix_position(transform_drag_.preview);
    const auto volume_color=logic_icon_color(logic_icon_for(node->type,node->role));
    if(const auto box=logic_box_bounds(*node,{centre.x,centre.y,centre.z})) {
      std::array<V3,8> corners;
      for(unsigned i=0;i<corners.size();++i)
        corners[i]={(i&1)?box->maximum[0]:box->minimum[0],
                    (i&2)?box->maximum[1]:box->minimum[1],
                    (i&4)?box->maximum[2]:box->minimum[2]};
      for(unsigned i=0;i<corners.size();++i)
        for(unsigned axis=0;axis<3;++axis)
          if(!(i&(1u<<axis))) {
            const auto a=screen(corners[i]),b=screen(corners[i|(1u<<axis)]);
            if(a && b)dl->AddLine(*a,*b,volume_color,1.5f);
          }
    } else if(node->type!=4050 && std::isfinite(node->radius) &&
              node->radius>0 && node->radius<100000) {
      for (unsigned plane = 0; plane < 3; ++plane) {
        std::optional<ImVec2> previous;
        for (unsigned step = 0; step <= 64; ++step) {
          float angle = step * 6.28318530718f / 64;
          auto v = centre;
          v[(plane + 1) % 3] += node->radius * std::cos(angle);
          v[(plane + 2) % 3] += node->radius * std::sin(angle);
          auto q = screen(v);
          if (q && previous)
            dl->AddLine(*previous, *q, volume_color,1.5f);
          previous = q;
        }
      }
    }
  }
  Matrix selected_matrix{};
  if (selected_ >= 0) {
    selected_matrix = transform_drag_.active ? transform_drag_.preview
                                             : doc_.matrix(selected_);
    if (drag3d_ && !transform_drag_.active) {
      selected_matrix[3] = drag3d_preview_.x;
      selected_matrix[7] = drag3d_preview_.y;
      selected_matrix[11] = drag3d_preview_.z;
    }
  }
  const auto paths =
      selected_ >= 0 && !link_mode_
          ? gizmo_paths(camera_, selected_matrix, transform_mode_,
                        transform_local_, selected_type)
          : std::vector<GizmoPath>{};
  const auto hovered_handle =
      hovered && !navigation_gesture_
          ? gizmo_pick(paths, io.MousePos.x - p.x, io.MousePos.y - p.y)
          : GizmoHandle::None;
  const bool logic_clicked = draw_logic_connections(
      dl, p, size,
      hovered && !navigation_gesture_ && hovered_handle == GizmoHandle::None);
  const bool link_picking =
      draw_link_picking(dl, p, size, hovered && !navigation_gesture_);
  struct Pick {
    int index;
    ImVec2 screen;
    float distance;
  };
  std::vector<Pick> picks;
  std::vector<LogicIconCandidate> icon_candidates;
  std::vector<ImVec2> icon_positions(doc_.objects().size());
  int icon_hover=-1;float icon_hover_distance=121.f;
  for (size_t i = 0; i < doc_.objects().size(); ++i)
    if (matches(i)) {
      V3 v = (drag3d_ && int(i) == selected_) ? drag3d_preview_
                                              : point(doc_.matrix(i));
      auto q = screen(v);
      if (!q || q->x < p.x || q->x > p.x + size.x || q->y < p.y ||
          q->y > p.y + size.y)
        continue;
      float distance = length(v - camera_.eye());
      if (!xray_) {
        auto obstruction =
            scene_.intersect({camera_.eye(), v - camera_.eye()},
                             std::max(.01f, distance - .15f), cut);
        if (obstruction)
          continue;
      }
      picks.push_back({int(i), *q, distance});
      icon_positions[i]=*q;icon_candidates.push_back({i,q->x-p.x,q->y-p.y,distance,int(i)==selected_});
      const auto dx=q->x-io.MousePos.x,dy=q->y-io.MousePos.y;const auto d=dx*dx+dy*dy;
      if(d<icon_hover_distance){icon_hover_distance=d;icon_hover=int(i);}
    }
  if(hovered)for(auto& c:icon_candidates)if(int(c.object)==icon_hover)c.priority=true;
  for(const auto i:layout_logic_icons(icon_candidates,size.x,size.y))draw_logic_icon(dl,icon_positions[i],int(i)==selected_?11.f:9.f,object_icon(i),int(i)==selected_,hovered&&int(i)==icon_hover);
  for (const auto &path : paths) {
    const bool highlighted = transform_drag_.active
                                 ? path.handle == transform_drag_.handle
                                 : path.handle == hovered_handle;
    const unsigned a = unsigned(path.handle);
    const ImU32 color = highlighted ? IM_COL32(255, 235, 100, 255)
                        : a < 3     ? axis_color(a)
                        : a == 3    ? IM_COL32(230, 205, 90, 220)
                        : a == 4    ? IM_COL32(220, 130, 240, 220)
                        : a == 5    ? IM_COL32(100, 225, 230, 220)
                                    : IM_COL32(245, 245, 245, 255);
    std::vector<ImVec2> points;
    for (const auto &q : path.points)
      points.push_back(p + ImVec2(q.x, q.y));
    if (path.filled && points.size() >= 4) {
      dl->AddConvexPolyFilled(points.data(), int(points.size() - 1),
                              (color & 0x00ffffffu) |
                                  (highlighted ? 0x77000000u : 0x33000000u));
    }
    for (size_t n = 1; n < points.size(); ++n)
      dl->AddLine(points[n - 1], points[n], color, highlighted ? 3.5f : 2.f);
    if (a < 3 && !points.empty()) {
      const auto tip = points.back();
      if (transform_mode_ == TransformMode::Move &&
          transform_mode_available(selected_type, TransformMode::Move)) {
        auto d = tip - points.front();
        float n = std::hypot(d.x, d.y);
        if (n > 3) {
          d = d * (1.f / n);
          ImVec2 side{-d.y, d.x};
          dl->AddTriangleFilled(tip, tip - d * 11 + side * 5,
                                tip - d * 11 - side * 5, color);
        }
      } else if (transform_mode_ == TransformMode::Scale)
        dl->AddRectFilled(tip - ImVec2(5, 5), tip + ImVec2(5, 5), color);
      const char *names[] = {"X", "Y", "Z"};
      auto mark = transform_mode_ == TransformMode::Rotate ? points[12] : tip;
      dl->AddText(mark + ImVec2(7, -7), color, names[a]);
      hits_.push_back({std::string("gizmo_") + char('x' + a),
                       mark - ImVec2(6, 6), mark + ImVec2(6, 6)});
    } else if (!points.empty()) {
      ImVec2 mid{};
      for (size_t n = 0; n + 1 < points.size(); ++n)
        mid = mid + points[n];
      mid = mid * (1.f / float(points.size() - 1));
      const char *ids[] = {"gizmo_xy", "gizmo_xz", "gizmo_yz", "gizmo_uniform"};
      hits_.push_back(
          {ids[std::min(3u, a - 3)], mid - ImVec2(6, 6), mid + ImVec2(6, 6)});
    }
  }
  if (selected_ >= 0)
    if (auto pivot = screen(matrix_position(selected_matrix))) {
      if (transform_mode_ != TransformMode::Scale)
        dl->AddCircle(*pivot, 5, theme::kYellow, 16, 1.5f);
      const auto &object = doc_.objects()[selected_];
      dl->AddText(*pivot + ImVec2(12, 13), theme::kYellow,
                  object.name.empty() ? category(object.type)
                                      : object.name.c_str());
    }
  const ImVec2 mouse = io.MousePos;
  Ray ray = camera_.ray(mouse.x - p.x, mouse.y - p.y);
  bool placing = placement_ >= 0;
  if (placing && hovered && !navigation_gesture_ && !io.KeyAlt) {
    auto surface =
        scene_.intersect(ray, std::numeric_limits<float>::infinity(), cut);
    if (surface) {
      auto v = surface->point;
      if (io.KeyShift) {
        v.x = std::round(v.x * 4) / 4;
        v.z = std::round(v.z * 4) / 4;
      }
      if (auto q = screen(v)) {
        dl->AddCircle(*q, 12, theme::kYellow, 24, 2);
        dl->AddLine(*q - ImVec2(17, 0), *q + ImVec2(17, 0), theme::kYellow, 2);
        dl->AddLine(*q - ImVec2(0, 17), *q + ImVec2(0, 17), theme::kYellow, 2);
        char label[128];
        std::snprintf(label, sizeof(label), "Place here: %.2f, %.2f, %.2f", v.x,
                      v.y, v.z);
        dl->AddText(*q + ImVec2(16, 16), theme::kYellow, label);
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        place_at(v);
    } else
      dl->AddText(p + ImVec2(10, 50), theme::kYellow,
                  "Point at a visible map surface to place.");
  }
  if (!logic_clicked && !link_picking && !placing && hovered && !io.KeyAlt &&
      !navigation_gesture_ && !drag3d_ &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    int chosen = -1;
    if (hovered_handle != GizmoHandle::None && selected_ >= 0) {
      if (transform_drag_.begin(transform_mode_, hovered_handle, camera_,
                                doc_.matrix(selected_), transform_local_,
                                mouse.x - p.x, mouse.y - p.y)) {
        drag3d_ = true;
        drag3d_mouse_ = mouse;
        drag3d_origin_ = drag3d_preview_ =
            matrix_position(transform_drag_.start);
        pending_drag_object_ = -1;
      }
    } else {
      float closest = 169;
      float depth = std::numeric_limits<float>::infinity();
      for (auto pick : picks) {
        float d = (pick.screen.x - mouse.x) * (pick.screen.x - mouse.x) +
                  (pick.screen.y - mouse.y) * (pick.screen.y - mouse.y);
        if (d < closest - .01f ||
            (std::abs(d - closest) < .01f && pick.distance < depth)) {
          chosen = pick.index;
          closest = d;
          depth = pick.distance;
        }
      }
      if(chosen<0 && textured_ && !appearance_.batches.empty()){
        ModelPickOptions options;options.cut_height=cut;options.show_enemies=show_enemy_models_;
        options.accept_object=[this](size_t object){return matches(object);};
        if(!xray_)if(const auto surface=scene_.intersect(ray,std::numeric_limits<float>::infinity(),cut))options.maximum=surface->distance+.05f;
        if(const auto model=pick_model(appearance_,doc_,ray,options,&model_pick_stats_))chosen=int(model->object);
      }
    }
    if (chosen >= 0) {
      select(chosen);
      if (transform_mode_ == TransformMode::Move && transform_mode_available(doc_.transform_type(chosen),TransformMode::Move)) {
        pending_drag_object_ = chosen;
        pending_drag_mouse_ = mouse;
      }
    }
  }
  if (pending_drag_object_ >= 0) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || navigation_gesture_ ||
        ImGui::IsKeyPressed(ImGuiKey_Escape))
      pending_drag_object_ = -1;
    else if (std::hypot(mouse.x - pending_drag_mouse_.x,
                        mouse.y - pending_drag_mouse_.y) > 4) {
      if (pending_drag_object_ == selected_ &&
          transform_mode_available(doc_.transform_type(selected_), TransformMode::Move)) {
        drag3d_origin_ = drag3d_preview_ = point(doc_.matrix(selected_));
        drag3d_mouse_ = pending_drag_mouse_;
        drag3d_axis_ = -1;
        drag3d_ = true;
        drag3d_surface_ = io.KeyCtrl;
        auto start_ray = camera_.ray(pending_drag_mouse_.x - p.x,
                                     pending_drag_mouse_.y - p.y);
        drag3d_anchor_ = horizontal_point(start_ray, drag3d_origin_.y)
                             .value_or(drag3d_origin_);
      }
      pending_drag_object_ = -1;
    }
  }
  if (transform_drag_.active) {
    transform_drag_.update(mouse.x - p.x, mouse.y - p.y,
                           transform_snap_ || io.KeyShift, transform_grid_,
                           transform_angle_, transform_scale_);
    drag3d_preview_ = matrix_position(transform_drag_.preview);
    char feedback[128];
    std::snprintf(feedback, sizeof(feedback), "%s: %.3f %s",
                  transform_mode_name(transform_drag_.mode),
                  transform_drag_.value,
                  transform_drag_.mode == TransformMode::Move     ? "m"
                  : transform_drag_.mode == TransformMode::Rotate ? "degrees"
                                                                  : "x");
    dl->AddRectFilled(mouse + ImVec2(14, 14), mouse + ImVec2(270, 41),
                      IM_COL32(8, 12, 22, 230), 4);
    dl->AddText(mouse + ImVec2(20, 20), theme::kYellow, feedback);
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (cancel || ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      if (!cancel && transform_drag_.preview != transform_drag_.start) {
        const auto before = doc_.matrix(selected_);
        attempt([&] { doc_.set_matrix(selected_, transform_drag_.preview); });
        if (doc_.matrix(selected_) != before)
          lesson_event(transform_drag_.mode == TransformMode::Move
                           ? HelpEvent::Move
                       : transform_drag_.mode == TransformMode::Rotate
                           ? HelpEvent::Rotate
                           : HelpEvent::Scale);
      }
      transform_drag_.active = false;
      drag3d_ = false;
      select(selected_);
    }
  } else if (drag3d_) {
    V3 moved = drag3d_origin_;
    bool valid = false;
    if (drag3d_axis_ >= 0) {
      auto parameter = axis_parameter(ray, drag3d_origin_, axis(drag3d_axis_));
      if (drag3d_has_parameter_ && parameter) {
        moved = moved + axis(drag3d_axis_) * (*parameter - drag3d_parameter_);
        valid = true;
      } else {
        auto a = screen(drag3d_origin_),
             b = screen(drag3d_origin_ + axis(drag3d_axis_));
        if (a && b) {
          float dx = b->x - a->x, dy = b->y - a->y, n = dx * dx + dy * dy;
          if (n > .01f) {
            moved = moved +
                    axis(drag3d_axis_) * (((mouse.x - drag3d_mouse_.x) * dx +
                                           (mouse.y - drag3d_mouse_.y) * dy) /
                                          n);
            valid = true;
          }
        }
      }
      if (valid && (io.KeyShift || transform_snap_))
        moved[drag3d_axis_] =
            std::round(moved[drag3d_axis_] / transform_grid_) * transform_grid_;
    } else if (io.KeyCtrl || drag3d_surface_) {
      if (auto hit = scene_.intersect(
              ray, std::numeric_limits<float>::infinity(), cut)) {
        moved = hit->point;
        valid = true;
        if (io.KeyShift || transform_snap_)
          moved = snapped(moved, transform_grid_);
      }
    } else if (auto plane = horizontal_point(ray, drag3d_origin_.y)) {
      moved = drag3d_origin_ + (*plane - drag3d_anchor_);
      valid = true;
      if (io.KeyShift || transform_snap_) {
        moved.x = std::round(moved.x / transform_grid_) * transform_grid_;
        moved.z = std::round(moved.z / transform_grid_) * transform_grid_;
      }
    }
    if (valid)
      drag3d_preview_ = moved;
    bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (cancel || ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      if (!cancel && std::hypot(mouse.x - drag3d_mouse_.x,
                                mouse.y - drag3d_mouse_.y) > 3.f) {
        auto v = drag3d_preview_;
        const auto before = doc_.matrix(selected_);
        attempt([&] { doc_.move(selected_, {v.x, v.y, v.z}); });
        if (doc_.matrix(selected_) != before)
          lesson_event(HelpEvent::Move);
      }
      drag3d_ = false;
      select(selected_);
    }
  }
  char label[128];
  std::snprintf(label, sizeof(label), "3D LEVEL | %zu surface triangles",
                scene_.triangles().size());
  dl->AddText(p + ImVec2(8, 8), IM_COL32(225, 235, 255, 255), label);
  dl->AddText(p + ImVec2(8, size.y - 36), IM_COL32(225, 235, 255, 255),
              "Click: select | W/E/R: tools | F: focus orbit pivot");
  dl->AddText(p + ImVec2(8, size.y - 18), IM_COL32(180, 200, 230, 255),
              "Shift: snap | Escape: cancel transform | Ctrl+drag: surface");
  dl->PopClipRect();
}
