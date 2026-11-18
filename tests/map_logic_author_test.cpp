#include "../launcher/src/map_appearance.h"
#include "../launcher/src/map_document.h"
#include "../launcher/src/map_logic.h"
#include "gc/editor_button.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void check(bool v, const char *message) {
  ++checks;
  if (!v)
    throw std::runtime_error(message);
}
uint32_t u(const mapedit::Document &d, uint32_t p) {
  const auto &b = d.payload();
  return uint32_t(b.at(p)) << 24 | uint32_t(b.at(p + 1)) << 16 |
         uint32_t(b.at(p + 2)) << 8 | b.at(p + 3);
}
uint32_t id(const mapedit::Document &d, size_t i) {
  return u(d, d.objects().at(i).offset + 20);
}
mapedit::Document with_native_words(
    const mapedit::Document &d,
    const std::vector<std::pair<uint32_t,uint32_t>> &words) {
  auto payload=d.payload();
  for(auto [at,value]:words)
    for(unsigned k=0;k<4;++k)payload.at(at+k)=uint8_t(value>>(24-8*k));
  auto file=d.export_bytes();
  const auto compressed=uint32_t(file[16])<<24|uint32_t(file[17])<<16|
                        uint32_t(file[18])<<8|file[19];
  const auto zip=mapedit::stored_zlib(payload);
  std::vector<uint8_t> output(file.begin(),file.begin()+64);
  for(unsigned k=0;k<4;++k)output[16+k]=uint8_t(zip.size()>>(24-8*k));
  output.insert(output.end(),zip.begin(),zip.end());
  output.insert(output.end(),file.begin()+64+compressed,file.end());
  mapedit::Document result;result.parse(std::move(output));return result;
}
mapedit::Document renamed_native(const mapedit::Document &d, size_t object) {
  auto payload = d.payload();
  auto p = u(d, d.objects()[object].offset + 8);
  for (unsigned k = 0; k < 5; ++k)
    payload[p + k] = "Lamp"[k];
  auto file = d.export_bytes();
  const auto compressed = uint32_t(file[16]) << 24 | uint32_t(file[17]) << 16 |
                          uint32_t(file[18]) << 8 | file[19];
  auto zip = mapedit::stored_zlib(payload);
  std::vector<uint8_t> output(file.begin(), file.begin() + 64);
  for (unsigned k = 0; k < 4; ++k)
    output[16 + k] = uint8_t(zip.size() >> (24 - 8 * k));
  output.insert(output.end(), zip.begin(), zip.end());
  output.insert(output.end(), file.begin() + 64 + compressed, file.end());
  mapedit::Document result;
  result.parse(std::move(output));
  return result;
}
size_t index(const mapedit::Document &d, uint32_t ident) {
  auto it =
      std::find_if(d.objects().begin(), d.objects().end(),
                   [&](const auto &o) { return u(d, o.offset + 20) == ident; });
  check(it != d.objects().end(), "Authored ID missing");
  return size_t(it - d.objects().begin());
}
template <class F> void rejects(F f, const char *m) {
  bool yes = false;
  try {
    f();
  } catch (const std::exception &) {
    yes = true;
  }
  check(yes, m);
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc > 1, "Pass extracted original game_data");
    const auto data = std::filesystem::u8path(argv[1]);
    mapedit::Document original;
    original.load(data / "files/Levels/Rlevel1_1.ssw");
    auto doc = original;
    const auto untouched = original.export_bytes();
    {
      const auto found=std::find_if(original.objects().begin(),original.objects().end(),
                                   [](const auto&o){return o.type==4050;});
      check(found!=original.objects().end(),"Original DoorController template missing");
      const auto slot=size_t(found-original.objects().begin());
      const auto ident=id(original,slot);
      const auto raw=u(original,found->offset+16);
      auto legacy=with_native_words(original,{
        {raw+4,std::bit_cast<uint32_t>(3.f)},{raw+8,std::bit_cast<uint32_t>(4.f)},
        {raw+12,0},{raw+24,0},{raw+36,0xffffffffu}});
      auto config=[&]{return u(legacy,legacy.objects()[slot].offset+16);};
      auto dimensions_and_modes=[&](float width,float height,uint32_t key){
        const auto c=config();
        return u(legacy,c+4)==std::bit_cast<uint32_t>(height) &&
               u(legacy,c+8)==std::bit_cast<uint32_t>(width) &&
               u(legacy,c+12)==0 && u(legacy,c+24)==0 && u(legacy,c+36)==key;
      };
      auto settings=legacy.logic_settings(ident);
      check(settings && settings->radius==4 && settings->key==0,
            "Original width/sentinel key fixture did not decode");
      settings->name+=" renamed";
      legacy.set_logic_settings(ident,*settings);
      check(dimensions_and_modes(4,3,0xffffffffu),
            "Renaming original4050 changed height, native modes or sentinel key");
      settings->enabled=!settings->enabled;
      legacy.set_logic_settings(ident,*settings);
      check(dimensions_and_modes(4,3,0xffffffffu),
            "Enabling original4050 changed unrelated native fields");
      settings->key=7;
      legacy.set_logic_settings(ident,*settings);
      check(dimensions_and_modes(4,3,7),
            "Editing an original key changed native dimensions or modes");
      const auto before_resize=legacy.export_bytes();
      settings->radius=6;
      legacy.set_logic_settings(ident,*settings);
      check(dimensions_and_modes(6,6,7),
            "Explicit box-side edit did not update dimensions while preserving modes");
      const auto resized=legacy.export_bytes();
      check(legacy.undo() && legacy.export_bytes()==before_resize &&
                dimensions_and_modes(4,3,7) && legacy.redo() &&
                legacy.export_bytes()==resized && dimensions_and_modes(6,6,7),
            "Original4050 dimension/mode preservation was not atomic on undo/redo");
      mapedit::Document reopened;reopened.parse(legacy.export_bytes());
      const auto c=u(reopened,reopened.objects()[slot].offset+16);
      check(u(reopened,c+4)==std::bit_cast<uint32_t>(6.f) &&
                u(reopened,c+8)==std::bit_cast<uint32_t>(6.f) &&
                u(reopened,c+12)==0 && u(reopened,c+24)==0 && u(reopened,c+36)==7,
            "Original4050 modes did not survive export/reload");
      mapedit::LogicSpec own;own.kind=mapedit::LogicKind::KeyBranch;
      own.radius=2;own.key=3;
      const auto added=legacy.add_logic({1,2,3},own);
      const auto ac=u(legacy,legacy.objects()[added].offset+16);
      check(u(legacy,ac+4)==std::bit_cast<uint32_t>(2.f) &&
                u(legacy,ac+8)==std::bit_cast<uint32_t>(2.f) &&
                u(legacy,ac+12)==1 && u(legacy,ac+24)==1 && u(legacy,ac+36)==3,
            "New editor KeyBranch lost cube/PlayersOnly/KeyMode defaults");
    }
    std::vector<uint32_t> nodes;
    for (unsigned kind = 0; kind <= unsigned(mapedit::LogicKind::Toggle);
         ++kind) {
      mapedit::LogicSpec s;
      s.kind = mapedit::LogicKind(kind);
      s.name = "Authored " + std::to_string(kind);
      s.delay = .2f;
      s.radius = 2.5f;
      s.repeat = s.kind == mapedit::LogicKind::Counter ||
                 s.kind == mapedit::LogicKind::Timer;
      s.start_on_load = s.kind == mapedit::LogicKind::Timer;
      s.activation_limit = 4;
      auto before = doc.export_bytes();
      auto i = doc.add_logic({3, 2, 90}, s);
      auto ident = id(doc, i);
      nodes.push_back(ident);
      auto saved = doc.export_bytes();
      auto value = doc.logic_settings(ident);
      check(bool(value), "Settings not readable");
      check(value->kind == s.kind && value->name == s.name,
            "Wrong logic type/name");
      const auto revision = doc.structure_revision();
      doc.set_logic_settings(ident, *value);
      check(doc.structure_revision() == revision && doc.export_bytes() == saved,
            "Applying unchanged logic created an edit");
      check(doc.can_undo(), "Creation not undoable");
      check(doc.undo() && doc.export_bytes() == before,
            "Creation undo not atomic");
      check(doc.redo() && doc.export_bytes() == saved, "Creation redo differs");
      mapedit::Document reopen;
      reopen.parse(saved);
      auto restored = reopen.logic_settings(ident);
      check(restored && restored->name == s.name && restored->kind == s.kind,
            "Logic settings lost on native export/reload");
      check(reopen.logic_slots(ident).size() ==
                (s.kind == mapedit::LogicKind::Timer       ? 9
                 : s.kind == mapedit::LogicKind::Toggle    ? 18
                 : s.kind == mapedit::LogicKind::KeyBranch ? 3
                                                           : 10),
            "Wrong named output count");
    }
    for (auto kind :
         {mapedit::LogicKind::Light, mapedit::LogicKind::Explosion}) {
      mapedit::LogicSpec s;
      s.kind = kind;
      s.name = mapedit::logic_kind_name(kind);
      s.effect.radius = 12.5f;
      s.effect.damage = 60;
      s.effect.scale = 1.5f;
      s.effect.style = 2;
      s.effect.colour = {200, 110, 40, 255};
      auto n = doc.add_logic({2, 2, 82}, s);
      auto ident = id(doc, n);
      mapedit::Document saved;
      saved.parse(doc.export_bytes());
      auto actual = saved.logic_settings(ident);
      check(actual && actual->kind == kind &&
                actual->effect.radius == s.effect.radius,
            "Effect schema did not persist in SSW");
      if (kind == mapedit::LogicKind::Light) {
        check(
            saved.logic_slots(ident).empty() &&
                !saved.can_logic_target(ident, mapedit::LogicAction::Trigger) &&
                saved.can_logic_target(ident, mapedit::LogicAction::Enable),
            "Light exposes ignored native events");
        auto renamed = renamed_native(saved, index(saved, ident));
        auto changed = renamed.logic_settings(ident);
        check(changed && changed->kind == mapedit::LogicKind::Light &&
                  renamed.is_editor_logic(index(renamed, ident)),
              "Light type depends on its visible name");
        auto graph = mapedit::build_logic_graph(renamed);
        check(graph.find(ident) && graph.find(ident)->role == "Point light" &&
                  renamed.logic_slots(ident).empty(),
              "Renamed light loses icon/input semantics");
      } else
        check(actual->effect.damage == 60 && actual->effect.scale == 1.5f &&
                  actual->effect.style == 2,
              "Explosion settings lost on reload");
    }
    for (unsigned n = 1; n < gc::editor::effect_max_lights; ++n) {
      mapedit::LogicSpec light;
      light.kind = mapedit::LogicKind::Light;
      light.name = "Budget light " + std::to_string(n);
      doc.add_logic({0, 2, 80}, light);
    }
    rejects(
        [&] {
          mapedit::LogicSpec light;
          light.kind = mapedit::LogicKind::Light;
          doc.add_logic({0, 2, 80}, light);
        },
        "Ninth native light accepted but cannot run");
    auto area = nodes[0], relay = nodes[1], counter = nodes[2],
         timer = nodes[3], gate = nodes[4], button = nodes[6],
         toggle = nodes[7];
    doc.add_logic_link(button, relay, mapedit::LogicAction::Trigger);
    doc.add_logic_link(button, gate, mapedit::LogicAction::Enable);
    auto slots = doc.logic_slots(button);
    check(slots[0].target == relay && slots[1].target == gate,
          "Button fanout did not persist");
    check(slots[0].label.find("pressed") != std::string::npos,
          "Button output name missing");
    doc.set_logic_link(toggle, doc.logic_slots(toggle)[9].slot, gate,
                       mapedit::LogicAction::Disable);
    check(doc.logic_slots(toggle)[9].action == mapedit::LogicAction::Disable,
          "Toggle Off action not saved");
    auto graph = mapedit::build_logic_graph(doc);
    check(graph.find(toggle) && graph.find(toggle)->role == "Toggle switch",
          "Toggle graph role missing");
    check(std::any_of(graph.edges.begin(), graph.edges.end(),
                      [&](const auto &e) {
                        return e.from == button && e.to == relay && e.editable;
                      }),
          "Button graph lacks logical editable output");
    auto r1 = doc.add_logic(
        {0, 0, 0}, mapedit::LogicSpec{mapedit::LogicKind::Relay, "Cycle A"});
    auto a = id(doc, r1);
    auto r2 = doc.add_logic(
        {0, 0, 0}, mapedit::LogicSpec{mapedit::LogicKind::Relay, "Cycle B"});
    auto b = id(doc, r2);
    auto spec = doc.logic_settings(a);
    spec->delay = 0;
    doc.set_logic_settings(a, *spec);
    spec = doc.logic_settings(b);
    spec->delay = 0;
    doc.set_logic_settings(b, *spec);
    doc.add_logic_link(a, b, mapedit::LogicAction::Trigger);
    rejects([&] { doc.add_logic_link(b, a, mapedit::LogicAction::Trigger); },
            "Immediate native cycle accepted");
    spec = doc.logic_settings(b);
    spec->delay = .1f;
    doc.set_logic_settings(b, *spec);
    doc.add_logic_link(b, a, mapedit::LogicAction::Trigger);
    check(true, "Delayed loop rejected");
    const auto safe_loop = doc.export_bytes();
    spec = doc.logic_settings(b);
    spec->delay = 0;
    rejects([&] { doc.set_logic_settings(b, *spec); },
            "Removing the last loop delay accepted immediate recursion");
    check(doc.export_bytes() == safe_loop,
          "Rejected loop settings did not roll back atomically");
    check(!mapedit::logic_target_supports(4110, mapedit::LogicAction::Trigger),
          "Unsafe null-activator damage offered");
    check(mapedit::logic_target_supports(4310, mapedit::LogicAction::Enable) &&
              !mapedit::logic_target_supports(4310,
                                              mapedit::LogicAction::Trigger),
          "Particle receiver events guessed");
    check(
        mapedit::logic_target_supports(4120, mapedit::LogicAction::Trigger) &&
            mapedit::logic_target_supports(4100, mapedit::LogicAction::Disable),
        "Verified native actions unavailable");
    {
      auto waves = original;
      const auto enemy = std::find_if(
          waves.objects().begin(), waves.objects().end(),
          [](const auto &object) { return object.type == 5000; });
      check(enemy != waves.objects().end(), "No enemy template for repeat QA");
      const auto placed = waves.duplicate(
          size_t(enemy - waves.objects().begin()), {2, 0, 90});
      mapedit::LogicSpec repeated;
      repeated.name = "Reusable wave source";
      repeated.activation_limit = 3;
      const auto source = waves.add_logic({2, 1, 85}, repeated);
      const auto source_id = id(waves, source);
      const auto spawn = waves.link_wave_enemy(
          source_id, waves.objects()[placed].offset, 2);
      // The native wave spawner is deliberately hidden from the outliner.
      // Resolve its authored ID through the serialized actor chain instead.
      uint32_t spawn_actor = u(waves, 0x40);
      while (spawn_actor && u(waves, spawn_actor + 20) != spawn)
        spawn_actor = u(waves, spawn_actor);
      check(spawn_actor != 0, "Native wave helper ID missing");
      const auto spawn_config = u(waves, spawn_actor + 16);
      check(u(waves, spawn_config + 12) == 6,
            "Reusable logic wave total ignored its activation limit");
      auto settings = waves.logic_settings(source_id);
      settings->activation_limit = 0;
      waves.set_logic_settings(source_id, *settings);
      check(u(waves, spawn_config + 12) > 1000000 &&
                u(waves, spawn_config + 12) <= 0x7fffffff,
            "Unlimited source made a one-shot or signed-overflowing wave");
      settings->activation_limit = 2;
      waves.set_logic_settings(source_id, *settings);
      mapedit::Document saved;
      saved.parse(waves.export_bytes());
      check(u(saved, spawn_config + 12) == 4,
            "Edited wave source limit did not persist in its native spawner");
    }
    size_t brush = original.objects().size();
    for (size_t k = 0; k < original.objects().size(); ++k) {
      const auto &o = original.objects()[k];
      if (o.type != 4070)
        continue;
      auto target = u(original, u(original, o.offset + 16) + 36);
      if (std::any_of(original.objects().begin(), original.objects().end(),
                      [&](const auto &other) {
                        return other.type == 4180 &&
                               u(original, other.offset + 20) == target;
                      })) {
        brush = k;
        break;
      }
    }
    check(brush < original.objects().size(), "No original button template");
    doc = original;
    mapedit::LogicSpec s;
    s.kind = mapedit::LogicKind::Button;
    s.name = "Working native button";
    s.delay = 0;
    auto created = doc.add_button(original, brush, {2.5f, 1.5f, 85}, s);
    const auto button_id = id(doc, created),
               matrix = doc.objects()[created].transform;
    check(doc.objects()[created].type == 4180 &&
              doc.transform_type(created) == 4200,
          "Button native actor or transform capability missing");
    auto visual = std::find_if(
        doc.objects().begin(), doc.objects().end(),
        [&](const auto &o) { return o.type == 4200 && o.transform == matrix; });
    check(visual != doc.objects().end(), "Button has no shared visible model");
    const auto button_config=u(doc,doc.objects()[created].offset+16);
    check(u(doc,button_config+gc::editor::button_magic_offset)==gc::editor::button_magic &&
              u(doc,button_config+gc::editor::button_version_offset)==gc::editor::button_version,
          "Button interaction schema is missing");
    auto scene_model=u(doc,0x58);
    for(unsigned n=0;n<u(doc,u(doc,visual->offset+16));++n)
      scene_model=u(doc,scene_model);
    check((u(doc,scene_model+36)&2)==0,
          "Button native Use collider was disabled by SceneModel flags");
    check(doc.undo() && doc.export_bytes() == untouched,
          "Button model and logic were not one transaction");
    check(doc.redo(), "Button redo failed");
    created = index(doc, button_id);
    auto m = doc.matrix(created);
    m[0] *= 1.5f;
    m[4] *= 1.5f;
    m[8] *= 1.5f;
    m[3] += 2;
    doc.set_matrix(created, m);
    check(doc.matrix(created) == m, "Button transform ignored");
    mapedit::Document reloaded;
    reloaded.parse(doc.export_bytes());
    created = index(reloaded, button_id);
    check(reloaded.matrix(created) == m &&
              reloaded.transform_type(created) == 4200,
          "Button model transform lost on reload");
    auto appearance = mapedit::Appearance::load(reloaded, data);
    check(std::any_of(appearance.batches.begin(), appearance.batches.end(),
                      [&](const auto &batch) {
                        return batch.object == int(created) && batch.count > 0;
                      }),
          "Button model has no rendered geometry");
    auto clone = reloaded.duplicate(created, {5, 1.5f, 85});
    check(reloaded.objects()[clone].type == 4180 &&
              reloaded.transform_type(clone) == 4200,
          "Button duplication lost visual pairing");
    auto clone_id = id(reloaded, clone);
    reloaded.erase(clone);
    check(!std::any_of(reloaded.objects().begin(), reloaded.objects().end(),
                       [&](const auto &o) {
                         return u(reloaded, o.offset + 20) == clone_id;
                       }),
          "Button deletion failed");
    auto control = reloaded.add_logic(
        {5, 1, 85},
        mapedit::LogicSpec{mapedit::LogicKind::Relay, "Foreign receiver"});
    auto receiver = id(reloaded, control);
    reloaded.add_logic_link(button_id, receiver, mapedit::LogicAction::Trigger);
    mapedit::Document donor;
    donor.parse(reloaded.export_bytes());
    auto target = original;
    auto imported =
        target.import_object(donor, index(donor, button_id), {7, 1.5f, 85});
    auto imported_id = id(target, imported);
    check(target.objects()[imported].type == 4180 &&
              target.transform_type(imported) == 4200,
          "Cross-map button import lost its native visual pair");
    const auto imported_slots = target.logic_slots(imported_id);
    check(
        std::none_of(imported_slots.begin(), imported_slots.end(),
                     [](const auto &slot) { return slot.target.has_value(); }),
        "Cross-map logic kept foreign receiver IDs");
    auto visual_index = std::find_if(
        target.objects().begin(), target.objects().end(), [&](const auto &o) {
          return o.type == 4200 &&
                 o.transform == target.objects()[imported].transform;
        });
    check(visual_index != target.objects().end(),
          "Imported button visual missing");
    const auto vi = size_t(visual_index - target.objects().begin());
    check(target.is_logic_visual(vi) &&
              target.logic_owner_index(vi) == int(imported) &&
              !target.can_erase(vi),
          "Button ownership/visual deletion guard missing");
    auto copied_from_visual = target.duplicate(vi, {10, 1.5f, 85});
    check(target.objects()[copied_from_visual].type == 4180,
          "Duplicating a button mesh does not duplicate its controller");
    check(original.export_bytes() == untouched, "Original donor changed");
    std::cout << "PASS checks=" << checks << " native_button=" << button_id
              << " native_payload=" << reloaded.payload().size()
              << " visual_batches=" << appearance.batches.size() << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
