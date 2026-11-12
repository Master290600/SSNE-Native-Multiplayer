#pragma once
#include "ssne/ppc_context.h"
#include <string>
#include <array>
namespace ssne {
void install_native_cutscene_gate();
struct NativeCutsceneCheckpoint {uint32_t camera=0,reference=0,active=0;};
bool capture_native_cutscene_checkpoint(NativeCutsceneCheckpoint&,std::string&);
bool validate_native_cutscene_checkpoint(const NativeCutsceneCheckpoint&,std::string&);
void restore_native_cutscene_checkpoint(const NativeCutsceneCheckpoint&);
// Guest-thread only: invoke the ORIGINAL skip request after server unanimity.
bool request_native_cutscene_skip(PPCContext&,uint32_t object,std::string& error);
bool present_native_cutscene(PPCContext&,uint32_t local_object,const std::array<float,14>& camera,std::string& error);
void clear_native_cutscene_presentation();
bool native_cutscene_presenting();
// True only when the original cinematic viewport and render mask select it.
bool native_cutscene_is_presenting(uint32_t object);
unsigned native_cutscene_camera_differences(const std::array<float,14>&);
}
