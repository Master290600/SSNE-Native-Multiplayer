#pragma once
#include <cstdint>
#include <string>
struct PPCContext;
namespace ssne {
struct StateWriter;
struct StateReader;
// Guest thread, after native entity/mod hook installation. PAL only; original
// Trigger delay/counter/fanout remains responsible for when an effect fires.
bool install_editor_logic_effects(std::string &error);
void set_editor_logic_effects_authoritative(bool);
void editor_logic_effects_frame(PPCContext &);
void reset_editor_logic_effects(PPCContext &);
struct EditorLogicEffectStats {
  uint32_t helpers = 0, lights = 0;
  uint64_t explosions = 0, rejected = 0;
};
EditorLogicEffectStats editor_logic_effect_stats();
// The RAM image contains native Light handles. Keep their ownership in the
// existing transactional runtime checkpoint, never release pre-restore handles
// against restored RAM. apply=false is a side-effect-free layout validation.
void editor_effect_state_save(StateWriter &);
bool editor_effect_state_load(StateReader &, bool apply = true);
} // namespace ssne
