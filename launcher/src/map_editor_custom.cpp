#include "../../port/src/crosshair_image.h"
#include "../../port/src/png_write.h"
#include "gc/custom_levels.h"
#include "map_editor.h"
#include "platform.h"
#include "theme.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using J = launcher::JSONValue;
std::vector<uint8_t> bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 0 || in.tellg() > 4 * 1024 * 1024)
    throw std::runtime_error(
        "Preview or metadata file is missing or exceeds 4 MiB.");
  std::vector<uint8_t> result(size_t(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(result.data()), result.size());
  if (!in)
    throw std::runtime_error("Cannot read preview or metadata.");
  return result;
}
std::string field(const J &j, const char *name) {
  const auto p = j.find(name);
  return p && p->string() ? *p->string() : std::string{};
}
std::string utf8(const fs::path &p) {
  const auto v = p.u8string();
  return {v.begin(), v.end()};
}
} // namespace

void MapEditor::release_custom_preview() {
  if (custom_preview_texture_ && SDL_GL_GetCurrentContext())
    glDeleteTextures(1, &custom_preview_texture_);
  custom_preview_texture_ = custom_preview_width_ = custom_preview_height_ = 0;
}
void MapEditor::load_custom_metadata() {
  release_custom_preview();
  custom_preview_path_.clear();
  custom_png_input_[0] = 0;
  custom_export_alias_.clear();
  custom_export_folder_.clear();
  custom_modes_ = gc::custom::kDefaultModes;
  const auto name =
      project_.folder.empty() ? std::string(name_) : project_.title;
  std::snprintf(custom_title_, sizeof(custom_title_), "%s", name.c_str());
  std::snprintf(custom_id_, sizeof(custom_id_), "%s", name.c_str());
  if (project_.folder.empty())
    return;
  const auto path = project_.folder / "custom-level-editor.json";
  if (!fs::is_regular_file(path) || fs::is_symlink(path))
    return;
  try {
    const auto raw = bytes(path);
    J j;
    std::string error;
    if (!launcher::parse_json(std::string(raw.begin(), raw.end()), j, error))
      return;
    if (const auto id = field(j, "id"); gc::custom::valid_id(id))
      std::snprintf(custom_id_, sizeof(custom_id_), "%s", id.c_str());
    if (const auto title = field(j, "title");
        !title.empty() && title.size() < sizeof(custom_title_))
      std::snprintf(custom_title_, sizeof(custom_title_), "%s", title.c_str());
    int64_t modes = 0;
    if (auto value = j.find("modes");
        value && value->integer(modes) && modes > 0 && modes <= 63)
      custom_modes_ = uint32_t(modes);
    if (field(j, "preview") == "custom-preview.png" &&
        fs::is_regular_file(project_.folder / "custom-preview.png"))
      import_custom_preview(utf8(project_.folder / "custom-preview.png"));
  } catch (const std::exception &e) {
    message_ = std::string("Custom preview was not loaded: ") + e.what();
  }
}
void MapEditor::save_custom_metadata() {
  if (project_.folder.empty())
    return;
  const auto text = launcher::write_json(
      J::Object{{"schema", 1},
                {"id", custom_id_},
                {"title", custom_title_},
                {"modes", int64_t(custom_modes_)},
                {"preview",
                 custom_preview_path_.empty() ? "" : "custom-preview.png"}},
      true);
  mapedit::atomic_write(project_.folder / "custom-level-editor.json",
                        {text.begin(), text.end()});
}
void MapEditor::import_custom_preview(const std::string &source) {
  const auto path = fs::u8path(source);
  if (fs::is_symlink(path))
    throw std::runtime_error("Choose a regular PNG file.");
  port::CrosshairImage image;
  std::string error;
  if (!port::load_crosshair_png(path, image, error))
    throw std::runtime_error(error);
  if (project_.folder.empty())
    save();
  const auto target = project_.folder / "custom-preview.png";
  const auto raw = bytes(path);
  if (fs::weakly_canonical(path) != fs::weakly_canonical(target))
    mapedit::atomic_write(target, raw);
  release_custom_preview();
  GLint binding = 0, alignment = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  glGenTextures(1, &custom_preview_texture_);
  glBindTexture(GL_TEXTURE_2D, custom_preview_texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
  glBindTexture(GL_TEXTURE_2D, GLuint(binding));
  custom_preview_width_ = image.width;
  custom_preview_height_ = image.height;
  custom_preview_path_ = utf8(target);
  std::snprintf(custom_png_input_, sizeof(custom_png_input_), "%s",
                custom_preview_path_.c_str());
  save_custom_metadata();
}
void MapEditor::capture_custom_preview() {
  if (!loaded() || scene_.triangles().empty())
    throw std::runtime_error("Open a map with a 3D scene first.");
  if (project_.folder.empty())
    save();
  auto camera = camera_;
  camera.width = camera.height = 512;
  const auto texture = scene_renderer_.draw(
      scene_, scene_revision_, camera, 512, 512,
      cutaway_ ? camera.target.y + 6 : std::numeric_limits<float>::infinity(),
      false, appearance_.vertices.empty() ? nullptr : &appearance_, doc_, -1,
      {}, show_enemy_models_);
  if (!texture)
    throw std::runtime_error("The 3D preview could not be captured.");
  std::vector<uint32_t> pixels(512 * 512);
  GLint binding = 0, alignment = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
  glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  glBindTexture(GL_TEXTURE_2D, GLuint(binding));
  for (unsigned y = 0; y < 256; ++y)
    for (unsigned x = 0; x < 512; ++x)
      std::swap(pixels[y * 512 + x], pixels[(511 - y) * 512 + x]);
  const auto pending = project_.folder / "custom-preview-capture.png";
  if (!port::write_png(utf8(pending), pixels.data(), 512, 512))
    throw std::runtime_error("Cannot save the captured preview.");
  import_custom_preview(utf8(pending));
  message_ = "Current 3D view captured as the custom level preview.";
}
void MapEditor::export_custom_level() {
  save();
  const auto exported = gc::custom::write_package(
      fs::u8path(base_) / "CustomLevels", custom_id_, custom_title_, level_,
      doc_.export_bytes(),
      custom_preview_path_.empty() ? fs::path{}
                                   : fs::u8path(custom_preview_path_),
      custom_modes_);
  save_custom_metadata();
  custom_export_alias_ = exported.alias;
  custom_export_folder_ = utf8(exported.folder);
  message_ = "Custom level added: " + exported.title +
             ". Start Play from the normal launcher and open Custom Level in "
             "the game's level selection.";
}
void MapEditor::custom_toolbar() {
  ImGui::SameLine();
  if (button("Custom level...", "custom_level",
             loaded() && !test_future_.valid() && !drag3d_ && !flight_active_))
    ImGui::OpenPopup("Custom level in game");
}
void MapEditor::custom_modal() {
  ImGui::SetNextWindowSize(ImVec2(640, 570), ImGuiCond_Appearing);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(.035f, .045f, .105f, 1));
  if (!ImGui::BeginPopupModal("Custom level in game", nullptr,
                              ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PopStyleColor();
    return;
  }
  ImGui::TextWrapped(
      "Add this map to the game's Custom Level category. Originals stay "
      "available. Changes appear when the game is started again.");
  ImGui::TextUnformatted("Display name");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##custom_title", custom_title_, sizeof(custom_title_));
  hit("custom_title");
  ImGui::SetNextItemWidth(240);
  ImGui::InputText("Package ID", custom_id_, sizeof(custom_id_));
  hit("custom_id");
  ImGui::TextDisabled(
      "Package ID identifies your copy; reuse it to update that level.");
  if (button("Add / update in game", "custom_export", custom_modes_ != 0))
    attempt([&] { export_custom_level(); });
  ImGui::SameLine();
  if (button("Close", "custom_close"))
    ImGui::CloseCurrentPopup();
  if (!custom_export_alias_.empty())
    ImGui::TextWrapped("Ready in game: %s", custom_title_);
  ImGui::Separator();
  const auto begin = ImGui::GetCursorPos();
  if (custom_preview_texture_) {
    const float aspect =
        float(custom_preview_width_) / std::max(1u, custom_preview_height_);
    const ImVec2 size =
        aspect > 1 ? ImVec2(160, 160 / aspect) : ImVec2(160 * aspect, 160);
    ImGui::Image(ImTextureID(custom_preview_texture_), size);
  } else
    ImGui::Dummy(ImVec2(160, 135));
  ImGui::SetCursorPos(begin + ImVec2(185, 0));
  ImGui::BeginGroup();
  ImGui::TextColored(theme::kGold, "Preview image");
  if (button("Choose PNG...", "custom_preview_browse"))
    attempt([&] {
      auto path = platform::open_png_dialog("Select custom level preview");
      if (!path.empty())
        import_custom_preview(path);
    });
  if (button("Use current 3D view", "custom_preview_capture"))
    attempt([&] { capture_custom_preview(); });
  if (button("Use original preview", "custom_preview_original")) {
    release_custom_preview();
    custom_preview_path_.clear();
    custom_png_input_[0] = 0;
  }
  ImGui::TextDisabled("PNG, up to 1024 x 1024 and 4 MiB.");
  ImGui::EndGroup();
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), begin.y + 168));
  ImGui::SetNextItemWidth(420);
  ImGui::InputTextWithHint("##custom_png_path", "Or paste a PNG path...",
                           custom_png_input_, sizeof(custom_png_input_));
  hit("custom_png_path");
  ImGui::SameLine();
  if (button("Load PNG", "custom_preview_load"))
    attempt([&] { import_custom_preview(custom_png_input_); });
  ImGui::TextUnformatted("Show in these modes:");
  const char *modes[]{"Single player", "Coop",          "Deathmatch",
                      "Hold the Flag", "Pass the Bomb", "Survival"};
  for (unsigned i = 0; i < 6; ++i) {
    if (i % 3)
      ImGui::SameLine();
    bool enabled = (custom_modes_ & (1u << i)) != 0;
    if (ImGui::Checkbox(modes[i], &enabled))
      custom_modes_ =
          enabled ? custom_modes_ | (1u << i) : custom_modes_ & ~(1u << i);
    hit(("custom_mode_" + std::to_string(i)).c_str());
  }
  ImGui::Separator();
  if (is_error_ && !message_.empty())
    ImGui::TextWrapped("%s", message_.c_str());
  ImGui::EndPopup();
  ImGui::PopStyleColor();
}
J MapEditor::custom_test_state() const {
  return J::Object{{"id", custom_id_},
                   {"title", custom_title_},
                   {"modes", int64_t(custom_modes_)},
                   {"preview", custom_preview_path_},
                   {"preview_width", int64_t(custom_preview_width_)},
                   {"preview_height", int64_t(custom_preview_height_)},
                   {"alias", custom_export_alias_},
                   {"folder", custom_export_folder_}};
}
