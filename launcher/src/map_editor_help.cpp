#include "map_editor.h"
#include "theme.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {
using J = launcher::JSONValue;
std::string lower_help(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
  return s;
}
int64_t help_number(const J &v, const char *key, int64_t fallback) {
  int64_t n;
  auto p = v.find(key);
  return p && p->integer(n) ? n : fallback;
}
float column_length(const mapedit::Matrix &m, unsigned c) {
  return std::sqrt(m[c] * m[c] + m[c + 4] * m[c + 4] + m[c + 8] * m[c + 8]);
}
bool logic_element(uint32_t t) {
  return t == 4030 || t == 4040 || t == 4050 || t == 4070 || t == 4080 ||
         t == 4100 || t == 4120 || t == 4170 || t == 4180;
}
} // namespace
void MapEditor::load_help_preferences() {
  if (help_loaded_)
    return;
  help_loaded_ = true;
  // Match the user's selected game language without changing game settings.
  std::ifstream settings(std::filesystem::u8path(base_) /
                         "Multiplayer/settings.ini");
  for (std::string line; std::getline(settings, line);) {
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    auto key = line.substr(0, eq);
    key.erase(
        std::remove_if(key.begin(), key.end(),
                       [](unsigned char c) { return c == ' ' || c == '\t'; }),
        key.end());
    if (key == "game_language") {
      std::istringstream value(line.substr(eq + 1));
      int n = -1;
      if (value >> n; n >= 0 && n <= 2)
        help_language_ = unsigned(n);
      break;
    }
  }
  try {
    std::ifstream file(std::filesystem::u8path(base_) / "map-editor-help.json",
                       std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 2 || file.tellg() > 8192)
      return;
    std::string data(size_t(file.tellg()), '\0');
    file.seekg(0);
    if (!file.read(data.data(), data.size()))
      return;
    J v;
    std::string error;
    if (!launcher::parse_json(data, v, error))
      return;
    if (help_number(v, "schema", 0) != 1)
      return;
    help_language_ = unsigned(
        std::clamp<int64_t>(help_number(v, "language", help_language_), 0, 2));
    tutorial_.step =
        int(std::clamp<int64_t>(help_number(v, "step", -1), -1, 12));
    tutorial_.completed =
        uint32_t(std::clamp<int64_t>(help_number(v, "completed", 0), 0, 4095));
    tutorial_.skipped =
        uint32_t(std::clamp<int64_t>(help_number(v, "skipped", 0), 0, 4095)) &
        ~tutorial_.completed;
    // Event counters are process-local; persisted incomplete steps require a
    // new action.
    if (tutorial_.active())
      tutorial_.baseline =
          help_events_[unsigned(mapedit::help_lessons()[tutorial_.step].event)];
  } catch (...) {
  }
}
void MapEditor::save_help_preferences() {
  try {
    auto value = J::Object{{"schema", 1},
                           {"language", int64_t(help_language_)},
                           {"step", tutorial_.step},
                           {"completed", int64_t(tutorial_.completed)},
                           {"skipped", int64_t(tutorial_.skipped)}};
    const auto bytes = launcher::write_json(value);
    mapedit::atomic_write(std::filesystem::u8path(base_) /
                              "map-editor-help.json",
                          std::vector<uint8_t>(bytes.begin(), bytes.end()));
  } catch (const std::exception &e) {
    message_ = std::string("Help preferences could not be saved: ") + e.what();
  }
}
void MapEditor::help_toolbar() {
  load_help_preferences();
  if (ImGui::IsKeyPressed(ImGuiKey_F1))
    help_open_ = !help_open_;
  if (button("Help / Tutorial (F1)", "help"))
    help_open_ = !help_open_;
  ImGui::SameLine();
  if (button("Logic graph", "logic_graph", loaded()))
    graph_open_ = !graph_open_;
  if (tutorial_.active()) {
    ImGui::SameLine();
    ImGui::TextDisabled(
        "Tutorial %d/12: %s", tutorial_.step + 1,
        mapedit::help_lessons()[tutorial_.step].title[help_language_]);
  }
}
void MapEditor::start_practice() {
  if (levels_.empty())
    throw std::runtime_error("Install the game before starting a practice map");
  const auto original = std::find(levels_.begin(), levels_.end(), "Rlevel1_1");
  level_index_ =
      original == levels_.end() ? 0 : int(original - levels_.begin());
  view3d_ = true;
  release_capture();
  open_level();
  int nearby = -1;
  float distance = std::numeric_limits<float>::infinity();
  for (size_t i = 0; i < doc_.objects().size(); ++i)
    if (doc_.objects()[i].type == 4200) {
      const auto m = doc_.matrix(i);
      const auto d =
          mapedit::length(mapedit::V3{m[3], m[7], m[11]} - camera_.target);
      if (d < distance) {
        distance = d;
        nearby = int(i);
      }
    }
  if (nearby >= 0)
    select(nearby);
  std::string practice = "Tutorial_Practice";
  unsigned suffix = 1;
  while (std::filesystem::exists(std::filesystem::u8path(base_) /
                                 "MapProjects" / practice))
    practice = "Tutorial_Practice_" + std::to_string(suffix++);
  std::snprintf(name_, sizeof(name_), "%s", practice.c_str());
  tutorial_.start(help_events_);
  lesson_event(mapedit::HelpEvent::Loaded);
  tutorial_.observe(help_events_);
  help_open_ = true;
  message_ =
      "Practice map opened in memory. Save project creates your own copy.";
  save_help_preferences();
}
void MapEditor::observe_help() {
  if (!loaded()) {
    help_matrix_valid_ = help_camera_valid_ = false;
    help_logic_revision_ = UINT64_MAX;
    return;
  }
  const auto eye = camera_.eye();
  if (help_camera_valid_ &&
      (mapedit::length(eye - help_last_eye_) > .0001f ||
       std::abs(camera_.yaw - help_last_yaw_) > .00001f ||
       std::abs(camera_.pitch - help_last_pitch_) > .00001f))
    lesson_event(mapedit::HelpEvent::Navigate);
  help_last_eye_ = eye;
  help_last_yaw_ = camera_.yaw;
  help_last_pitch_ = camera_.pitch;
  help_camera_valid_ = true;
  if (selected_ >= 0 && size_t(selected_) < doc_.objects().size()) {
    const auto m = doc_.matrix(size_t(selected_));
    if (help_matrix_valid_ && help_matrix_object_ == selected_) {
      if (std::abs(m[3] - help_last_matrix_[3]) +
              std::abs(m[7] - help_last_matrix_[7]) +
              std::abs(m[11] - help_last_matrix_[11]) >
          .0001f)
        lesson_event(mapedit::HelpEvent::Move);
      bool scale = false, rotation = false;
      for (unsigned c = 0; c < 3; ++c) {
        const auto a = column_length(m, c),
                   b = column_length(help_last_matrix_, c);
        scale |= std::abs(a - b) > .0001f;
        if (a > 1e-5f && b > 1e-5f)
          for (unsigned r = 0; r < 3; ++r)
            rotation |= std::abs(m[r * 4 + c] / a -
                                 help_last_matrix_[r * 4 + c] / b) > .0001f;
      }
      if (scale)
        lesson_event(mapedit::HelpEvent::Scale);
      if (rotation)
        lesson_event(mapedit::HelpEvent::Rotate);
    }
    help_last_matrix_ = m;
    help_matrix_object_ = selected_;
    help_matrix_valid_ = true;
  } else
    help_matrix_valid_ = false;
  if (help_logic_revision_ != doc_.structure_revision()) {
    std::set<uint32_t> ids;
    std::string signature;
    for (const auto &node : logic_graph_.nodes)
      if (logic_element(node.type)) {
        ids.insert(node.id);
        signature += std::to_string(node.id) + ":" + node.name + ":" +
                     node.summary + ":" + std::to_string(node.delay) + ":" +
                     std::to_string(node.radius) + ":" +
                     std::to_string(node.threshold) + ":" +
                     std::to_string(node.activation_limit) + ":" +
                     std::to_string(node.initially_enabled) + ":" +
                     std::to_string(node.repeat_counter) + ";";
      }
    size_t links = 0;
    for (const auto &edge : logic_graph_.edges)
      if (edge.editable && !edge.missing)
        ++links;
    if (help_logic_revision_ != UINT64_MAX) {
      if (std::any_of(ids.begin(), ids.end(), [&](uint32_t id) {
            return !help_logic_ids_.contains(id);
          }))
        lesson_event(mapedit::HelpEvent::CreateLogic);
      if (ids == help_logic_ids_ && signature != help_logic_signature_)
        lesson_event(mapedit::HelpEvent::ConfigureLogic);
      if (links > help_link_count_)
        lesson_event(mapedit::HelpEvent::Connect);
    }
    help_logic_ids_ = std::move(ids);
    help_logic_signature_ = std::move(signature);
    help_link_count_ = links;
    help_logic_revision_ = doc_.structure_revision();
  }
  if (tutorial_.observe(help_events_))
    save_help_preferences();
}
void MapEditor::help_panel() {
  observe_help();
  if (!help_open_)
    return;
  const auto viewport = ImGui::GetMainViewport();
  const ImVec2 available(std::max(300.f, viewport->WorkSize.x - 32.f),
                         std::max(240.f, viewport->WorkSize.y - 32.f));
  ImGui::SetNextWindowPos(viewport->WorkPos + ImVec2(16, 16),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(
      ImVec2(std::min(650.f, available.x), std::min(620.f, available.y)),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(300, 240), available);
  ImGui::SetNextWindowBgAlpha(1.f);
  if (!ImGui::Begin("Map Editor - Help & Tutorial", &help_open_)) {
    ImGui::End();
    return;
  }
  int language = int(help_language_);
  ImGui::SetNextItemWidth(150);
  if (ImGui::Combo("Language", &language, "English\0Deutsch\0Français\0")) {
    help_language_ = unsigned(language);
    save_help_preferences();
  }
  hit("help_language");
  const auto l = help_language_;
  const auto words = [&](const char *en, const char *de, const char *fr) {
    return l == 1 ? de : l == 2 ? fr : en;
  };
  ImGui::TextWrapped(
      "%s",
      words(
          "Learn by making real edits. No map is changed by opening this help.",
          "Lerne mit echten Bearbeitungsschritten. Das Öffnen dieser Hilfe "
          "ändert keine Karte.",
          "Apprenez en réalisant de vraies modifications. Ouvrir cette aide ne "
          "modifie aucune carte."));
  if (button(words("Start practice", "Übung starten", "Démarrer l'exercice"),
             "help_practice", !levels_.empty()))
    help_practice_requested_ = true;
  ImGui::SameLine();
  if (button(words("Use current map", "Aktuelle Karte üben",
                   "Utiliser la carte courante"),
             "help_start", loaded())) {
    tutorial_.start(help_events_);
    lesson_event(mapedit::HelpEvent::Loaded);
    tutorial_.observe(help_events_);
    save_help_preferences();
  }
  if (tutorial_.active()) {
    const auto &lesson = mapedit::help_lessons()[tutorial_.step];
    ImGui::SeparatorText(words("Interactive lesson", "Interaktive Übung",
                               "Exercice interactif"));
    ImGui::TextColored(theme::kGold, "%s", lesson.title[l]);
    ImGui::TextWrapped("%s", lesson.body[l]);
    ImGui::Spacing();
    const bool complete = (tutorial_.completed & (1u << tutorial_.step)) != 0;
    ImGui::PushStyleColor(ImGuiCol_Text,
                          complete ? theme::kGood : theme::kGold);
    ImGui::TextWrapped("%s",
                       complete ? words("Action completed", "Aktion ausgeführt",
                                        "Action effectuée")
                                : lesson.hint[l]);
    ImGui::PopStyleColor();
    if (button(words("Previous", "Zurück", "Précédent"), "help_previous",
               tutorial_.step > 0)) {
      tutorial_.previous(help_events_);
      save_help_preferences();
    }
    ImGui::SameLine();
    if (button(words("Next", "Weiter", "Suivant"), "help_next", complete)) {
      tutorial_.next(help_events_);
      save_help_preferences();
    }
    ImGui::SameLine();
    if (button(words("Skip step", "Schritt überspringen", "Ignorer l'étape"),
               "help_skip")) {
      tutorial_.next(help_events_, true);
      save_help_preferences();
    }
    ImGui::SameLine();
    if (button(words("Stop tutorial", "Übung beenden", "Arrêter l'exercice"),
               "help_stop")) {
      tutorial_.step = -1;
      save_help_preferences();
    }
    ImGui::ProgressBar(std::clamp(float(tutorial_.step) /
                                      float(mapedit::help_lessons().size()),
                                  0.f, 1.f),
                       ImVec2(-1, 0));
  } else if (tutorial_.finished()) {
    ImGui::Separator();
    ImGui::TextColored(
        theme::kGood, "%s",
        words("Tutorial finished", "Übung beendet", "Exercice terminé"));
    ImGui::TextWrapped(
        "%s", tutorial_.skipped
                  ? words("Some steps were skipped. Restart to practise them.",
                          "Einige Schritte wurden übersprungen. Du kannst sie "
                          "in einer neuen Übung nachholen.",
                          "Certaines étapes ont été ignorées. Recommencez pour "
                          "les essayer.")
                  : words("You have tried every editor workflow. Reopen this "
                          "help whenever needed.",
                          "Du hast alle Editor-Abläufe ausprobiert. Diese "
                          "Hilfe bleibt jederzeit verfügbar.",
                          "Vous avez essayé tous les outils. Cette aide reste "
                          "toujours disponible."));
  }
  ImGui::SeparatorText(words("Reference", "Nachschlagen", "Référence"));
  ImGui::InputTextWithHint(
      "##help_search",
      words("Search help...", "Hilfe durchsuchen...", "Rechercher..."),
      help_search_, sizeof(help_search_));
  hit("help_search");
  const auto query = lower_help(help_search_);
  ImGui::BeginChild("help_topics", ImVec2(200, 0), ImGuiChildFlags_Borders);
  for (size_t i = 0; i < mapedit::help_lessons().size(); ++i) {
    const auto &t = mapedit::help_lessons()[i];
    if (!query.empty() &&
        lower_help(std::string(t.title[l]) + " " + t.body[l]).find(query) ==
            std::string::npos)
      continue;
    if (ImGui::Selectable(t.title[l], help_topic_ == int(i)))
      help_topic_ = int(i);
    hit(("help_topic_" + std::to_string(i)).c_str());
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("help_reference", ImVec2(0, 0));
  const auto &topic =
      mapedit::help_lessons()[size_t(std::clamp(help_topic_, 0, 11))];
  ImGui::TextColored(theme::kGold, "%s", topic.title[l]);
  ImGui::TextWrapped("%s", topic.body[l]);
  ImGui::Spacing();
  ImGui::TextWrapped("%s", topic.hint[l]);
  ImGui::EndChild();
  ImGui::End();
}
launcher::JSONValue MapEditor::help_test_state() const {
  J::Array events;
  for (auto n : help_events_)
    events.emplace_back(int64_t(n));
  return J::Object{{"open", help_open_},
                   {"language", int64_t(help_language_)},
                   {"step", tutorial_.step},
                   {"completed", int64_t(tutorial_.completed)},
                   {"skipped", int64_t(tutorial_.skipped)},
                   {"events", std::move(events)}};
}
