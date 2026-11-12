#pragma once
#include "native_configuration.h"
namespace port::coop {
struct NativeBootstrapRequest {
    std::string level;
    Properties game_setup;
    uint32_t mode = 1;
};
inline const char* native_rules_prefix(uint32_t mode) {
    switch(mode){case 1:return "GameSetup.RulesCoop";case 2:return "GameSetup.RulesFragmatch";case 3:return "GameSetup.RulesHoldTheFlag";case 4:return "GameSetup.RulesPassTheBomb";default:return nullptr;}
}
// Queue the native game scene after validating/applying the host's level/rules.
// Only the guest thread at a frame safe point may call this.
std::optional<std::string> native_active_level();
bool activate_native_world_link(PPCContext&,uint32_t object,std::string& target,std::string& error,bool terminal=false);
// Test/debug activation of an existing original scripted camera entity.
// Uses the original event handler; does not synthesize camera flags or scripts.
bool activate_native_cutscene(PPCContext&,uint32_t object,std::string& error);
bool activate_native_music_changer(PPCContext&,uint32_t object,std::string& error);
bool activate_native_sound_holder(PPCContext&,uint32_t object,std::string& error);
// Authority-selected original credits/menu scene; never a client level decision.
bool queue_native_offline_menu(PPCContext&,std::string& error);
bool queue_native_coop_presentation(PPCContext&,uint32_t scene,std::string& error);
bool install_native_credits(bool authority,std::string& error);
void publish_native_credits(Properties&);
bool receive_native_credits(const Properties&,std::string& error);
bool native_credits_finishing();
void request_native_credits_skip();
bool queue_native_coop_level(PPCContext&,const NativeBootstrapRequest&,std::string& error);
}
