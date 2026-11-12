#pragma once
#include "ssne/ppc_context.h"
#include <optional>
#include <string>
#include <vector>
namespace ssne {
struct NativeMusicRequest {uint64_t sequence=0;uint32_t track=0;bool force=false;float fade=0;bool started=false;};
struct NativeSpeech {uint64_t sequence=0;uint32_t track=0,reference=0;uint64_t output_frames=0;bool started=false;};
void install_native_music_observer();
bool native_music_save_state(std::vector<uint8_t>&,std::string&);
bool native_music_load_state(const std::vector<uint8_t>&,std::string&);
bool native_music_checkpoint_probe(std::string&);
std::optional<NativeMusicRequest> native_music_request();
std::optional<NativeMusicRequest> native_music_pending_request();
bool native_music_track_valid(uint32_t);
bool apply_native_music_request(PPCContext&,const NativeMusicRequest&,std::string& error);
bool update_native_combo_music(PPCContext&,uint32_t local_actor,std::string& error);
std::optional<NativeSpeech> native_speech();
uint32_t native_speech_playing_track();
bool native_background_music_running();
bool apply_native_speech(PPCContext&,const NativeSpeech&,std::string& error);
bool prepare_native_speech(PPCContext&,const NativeSpeech&,std::string& error);
bool start_prepared_native_speech(PPCContext&,uint64_t sequence,std::string& error);
bool stop_native_replica_speech(PPCContext&,std::string& error);
}
