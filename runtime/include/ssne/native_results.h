#pragma once
#include <array>
#include <optional>
#include <string>
#include <cstdint>
#include <vector>
#include "ssne/ppc_context.h"
namespace ssne {
struct NativeResults {uint64_t sequence=0;std::string level;uint32_t score=0,high_score=0;std::array<float,13> values{};std::array<float,26> coefficients{};bool active=false;};
void install_native_results_observer();
bool native_story_save_state(std::vector<uint8_t>&,std::string&);
bool native_story_load_state(const std::vector<uint8_t>&,std::string&);
bool native_story_checkpoint_probe(std::string&);
void set_native_results_save_host(bool enabled);
void capture_native_results(uint32_t gui,uint32_t total);
std::optional<NativeResults> native_results();
bool apply_native_replica_results(PPCContext&,const NativeResults&,std::string&);
bool clear_native_replica_results(PPCContext&,std::string&);
bool native_replica_results_presenting();
// Main-thread visibility is atomic; menu access and ticking remain guest-thread only.
bool native_results_presenting();
uint32_t native_results_menu();
bool tick_native_results_auto_continue(PPCContext&,void (*before_continue)(PPCContext&),std::string&);
}
