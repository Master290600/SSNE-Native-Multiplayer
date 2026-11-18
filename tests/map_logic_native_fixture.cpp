#include "../launcher/src/launcher_json.h"
#include "../launcher/src/map_document.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

uint32_t word(const mapedit::Document &d, uint32_t p) {
  const auto &b = d.payload();
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
uint32_t id(const mapedit::Document &d, size_t i) {
  return word(d, d.objects().at(i).offset + 20);
}
int main(int argc, char **argv) {
  try {
    if (argc < 3)
      throw std::runtime_error(
          "Usage: fixture original_data private_project_root [game_exe card]");
    const auto data = std::filesystem::u8path(argv[1]),
               out = std::filesystem::u8path(argv[2]);
    mapedit::Document original;
    original.load(data / "files/Levels/Rlevel1_1.ssw");
    auto doc = original;
    size_t brush = original.objects().size(), enemy = original.objects().size();
    for (size_t i = 0; i < original.objects().size(); ++i) {
      const auto &o = original.objects()[i];
      if (o.type == 4140 && o.name.find("Quick Start") != std::string::npos)
        doc.move(i, {2.5f, -.25f, 81});
      if (o.type == 5000 && enemy == original.objects().size())
        enemy = i;
      if (o.type == 4070) {
        const auto switch_id =
            word(original, word(original, o.offset + 16) + 36);
        for (const auto &target : original.objects())
          if (target.type == 4180 &&
              word(original, target.offset + 20) == switch_id)
            brush = i;
      }
    }
    if (brush == original.objects().size() ||
        enemy == original.objects().size())
      throw std::runtime_error("Original native templates unavailable");
    mapedit::LogicSpec button;
    button.kind = mapedit::LogicKind::Button;
    button.name = "QA press with Use";
    button.delay = 0;
    button.activation_limit = 1;
    const auto button_index = doc.add_button(original, brush,
                                             {3.625f, .75f, 82.25f}, button);
    const auto button_id = id(doc, button_index);
    mapedit::LogicSpec timer;
    timer.kind = mapedit::LogicKind::Timer;
    timer.name = "QA delayed wave";
    timer.delay = 1;
    timer.activation_limit = 1;
    const auto timer_index = doc.add_logic({5, 1, 82}, timer);
    const auto timer_id = id(doc, timer_index);
    mapedit::LogicSpec gate;
    gate.kind = mapedit::LogicKind::Gate;
    gate.name = "QA pressed gate";
    gate.delay = 0;
    const auto gate_index = doc.add_logic({6, 1, 82}, gate);
    const auto gate_id = id(doc, gate_index);
    const auto enemy_index = doc.duplicate(enemy, {2.5f, -.25f, 88});
    const auto enemy_offset = doc.objects()[enemy_index].offset;
    doc.add_logic_link(button_id, timer_id, mapedit::LogicAction::Trigger);
    doc.add_logic_link(button_id, gate_id, mapedit::LogicAction::Enable);
    const auto spawner_id = doc.link_wave_enemy(timer_id, enemy_offset, 2);
    mapedit::LogicSpec toggle;
    toggle.kind = mapedit::LogicKind::Toggle;
    toggle.name = "QA on off button";
    toggle.delay = 0;
    const auto toggle_index = doc.add_button(original, brush,
                                             {-2.5f, .75f, 82.25f}, toggle);
    const auto toggle_id = id(doc, toggle_index);
    doc.set_logic_link(toggle_id, doc.logic_slots(toggle_id)[0].slot, gate_id,
                       mapedit::LogicAction::Enable);
    doc.set_logic_link(toggle_id, doc.logic_slots(toggle_id)[9].slot, gate_id,
                       mapedit::LogicAction::Disable);
    // Real-native effects fixture: pressing the main button enables a blue
    // point light immediately; its native timer turns it off four seconds
    // later. The reusable toggle can independently switch it back on/off.
    mapedit::LogicSpec light;
    light.kind = mapedit::LogicKind::Light;
    light.name = "QA blue point light";
    light.enabled = false;
    light.effect.kind = gc::editor::EffectKind::Light;
    light.effect.radius = 8;
    light.effect.colour = {5, 45, 255, 255};
    const auto light_index = doc.add_logic({4, 2, 85}, light);
    const auto light_id = id(doc, light_index);
    doc.add_logic_link(button_id, light_id, mapedit::LogicAction::Enable);
    auto off = timer;
    off.name = "QA blue light off after four seconds";
    off.delay = 4;
    const auto off_index = doc.add_logic({6, 2, 85}, off);
    const auto off_id = id(doc, off_index);
    doc.add_logic_link(button_id, off_id, mapedit::LogicAction::Trigger);
    doc.add_logic_link(off_id, light_id, mapedit::LogicAction::Disable);
    doc.set_logic_link(toggle_id, doc.logic_slots(toggle_id)[1].slot, light_id,
                       mapedit::LogicAction::Enable);
    doc.set_logic_link(toggle_id, doc.logic_slots(toggle_id)[10].slot, light_id,
                       mapedit::LogicAction::Disable);
    // QA-only delay gives the observer time to record living enemies before
    // one radial damage pulse. Radius twelve covers enemies which approach
    // during the delay; the driver enables ordinary God mode for the players.
    mapedit::LogicSpec explosion;
    explosion.kind = mapedit::LogicKind::Explosion;
    explosion.name = "QA native explosion after wave";
    explosion.delay = 2;
    explosion.activation_limit = 1;
    explosion.effect.kind = gc::editor::EffectKind::Explosion;
    explosion.effect.radius = 12;
    explosion.effect.damage = 77;
    explosion.effect.scale = 1;
    explosion.effect.style = 0;
    const auto explosion_index = doc.add_logic({2.5f, .1f, 88}, explosion);
    const auto explosion_id = id(doc, explosion_index);
    doc.add_logic_link(timer_id, explosion_id, mapedit::LogicAction::Trigger);
    const auto project =
        mapedit::Project::create(out, "NativeLogic", "Rlevel1_1");
    project.save(doc);
    std::filesystem::path test;
    if (argc >= 5)
      test = project.prepare_test(data, std::filesystem::u8path(argv[3]),
                                  std::filesystem::u8path(argv[4]));
    std::cout << launcher::write_json(launcher::JSONValue::Object{
                     {"passed", true},
                     {"project", project.folder.string()},
                     {"test", test.string()},
                     {"button", int64_t(button_id)},
                     {"timer", int64_t(timer_id)},
                     {"gate", int64_t(gate_id)},
                     {"spawner", int64_t(spawner_id)},
                     {"toggle", int64_t(toggle_id)},
                     {"light", int64_t(light_id)},
                     {"light_off_timer", int64_t(off_id)},
                     {"explosion", int64_t(explosion_id)},
                     {"explosion_damage", int64_t(77)},
                     {"explosion_radius", int64_t(12)},
                     {"explosion_delay", int64_t(2)},
                     {"enemy_template", int64_t(id(doc, enemy_index))},
                     {"payload_bytes", int64_t(doc.payload().size())}})
              << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
