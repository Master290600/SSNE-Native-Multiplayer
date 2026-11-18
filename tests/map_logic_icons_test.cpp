#include "map_logic_icons.h"
#include <cmath>
#include <imgui.h>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
using namespace mapedit;
unsigned checks = 0;
void check(bool v, const char *reason) {
  ++checks;
  if (!v)
    throw std::runtime_error(reason);
}
int main() {
  try {
    check(logic_icon_for(4260) == LogicIcon::ArrowMarker,
          "Native Arrow4260 misclassified");
    check(logic_icon_for(4150) == LogicIcon::EnemyPath &&
              logic_icon_for(4060) == LogicIcon::PathMarker,
          "PathMarker4150 confused with Arrow");
    check(logic_icon_for(4180) == LogicIcon::Button,
          "Native interactive switch icon missing");
    check(logic_icon_for(4180, "Toggle button") == LogicIcon::Toggle,
          "Toggle icon missing");
    check(logic_icon_for(4080, "Counter relay") == LogicIcon::Counter,
          "Counter icon missing");
    check(logic_icon_for(4080, "Delay timer") == LogicIcon::Timer,
          "Timer icon missing");
    check(logic_icon_for(4080, "Condition branch") == LogicIcon::Branch,
          "Condition icon missing");
    check(logic_icon_for(4080, "Sequence") == LogicIcon::Sequence,
          "Sequence icon missing");
    check(logic_icon_for(5000, "Light source") == LogicIcon::Enemy,
          "Role overrode proven enemy class");
    check(logic_icon_for(9999) == LogicIcon::Object,
          "Unknown actor silently assigned gameplay role");
    check(logic_icon_for(9999, "Light source") == LogicIcon::Light,
          "Decoded light role unavailable");
    check(logic_icon_for(9999, "Explosion") == LogicIcon::Explosion,
          "Decoded explosion role unavailable");
    check(logic_icon_for(9999, "Music") == LogicIcon::Music,
          "Decoded music role unavailable");
    check(logic_icon_for(4040) == LogicIcon::WaveSpawner &&
              logic_icon_for(4030) == LogicIcon::AreaTrigger,
          "Area/wave ambiguity");
    for (auto type :
         {4010u, 4020u, 4030u, 4040u, 4050u, 4060u, 4070u, 4080u, 4090u, 4100u,
          4110u, 4120u, 4130u, 4140u, 4150u, 4160u, 4170u, 4180u, 4190u, 4210u,
          4220u, 4230u, 4240u, 4260u, 4270u, 4280u, 4310u, 4320u, 4330u, 4340u})
      check(logic_icon_for(type) != LogicIcon::Object,
            "Known native logic/helper left as generic object");
    check(logic_icon_for(4080, "Gate") == LogicIcon::Gate, "Gate icon missing");
    const std::vector<LogicIconCandidate> cloud{
        {8, 40, 40, 20, false},
        {5, 40, 40, 10, false},
        {9, 10, 10, 4, false},
        {10, 10, 10, 30, true},
        {11, 10, 10, 40, true},
        {12, -1, 10, 1, true},
        {13, 40, 40, std::numeric_limits<float>::quiet_NaN(), false}};
    const auto layout = layout_logic_icons(cloud, 100, 100);
    check(layout == std::vector<size_t>({5, 10, 11}),
          "Dense helpers did not prefer nearest and retain priority selection");
    check(layout_logic_icons(cloud, -1, 100).empty(),
          "Invalid viewport produced helper layout");
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200, 1100);
    io.DeltaTime = 1.f / 60;
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    auto *draw = ImGui::GetForegroundDrawList();
    std::set<std::string_view> labels;
    for (unsigned i = 0; i < unsigned(LogicIcon::Count); ++i) {
      const auto kind = LogicIcon(i);
      check(labels.insert(logic_icon_name(kind)).second,
            "Duplicate semantic icon name");
      check((logic_icon_color(kind) >> 24) == 255, "Icon tint not visible");
      const int old = draw->VtxBuffer.Size;
      const auto center = ImVec2(60.f + (i % 7) * 150, 70.f + (i / 7) * 150);
      draw_logic_icon(draw, center, 18, kind, false, false);
      check(draw->VtxBuffer.Size > old + 4,
            "Semantic icon drew only a placeholder frame");
      const int normal = draw->VtxBuffer.Size - old;
      draw_logic_icon(draw, center, 18, kind, true, true);
      check(draw->VtxBuffer.Size - old - normal > normal,
            "Selected icon did not add outline");
      for (int k = old; k < draw->VtxBuffer.Size; ++k) {
        const auto v = draw->VtxBuffer[k].pos;
        check(std::isfinite(v.x) && std::isfinite(v.y),
              "Nonfinite icon geometry");
        check(std::abs(v.x - center.x) < 25 && std::abs(v.y - center.y) < 25,
              "Unbounded icon geometry");
      }
    }
    ImGui::EndFrame();
    ImGui::DestroyContext();
    std::cout << "PASS semantic_icons=" << unsigned(LogicIcon::Count)
              << " checks=" << checks << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
