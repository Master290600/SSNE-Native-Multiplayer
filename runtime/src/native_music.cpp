#include "ssne/native_music.h"
#include "ssne/audio.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/native_entity.h"
#include <cmath>
#include <vector>
#include <bit>
#include <fstream>
#include <cstring>
namespace ssne {
namespace {
PPCFunc request_original=nullptr,play_original=nullptr;
PPCFunc start_original=nullptr;
std::optional<NativeMusicRequest> request;
std::optional<NativeSpeech> speech;
uint64_t sequence=0;
uint64_t speech_sequence=0;
uint64_t confirmed_music_sequence=0;
uint64_t confirmed_speech_sequence=0,confirmed_replica_speech_sequence=0;
std::optional<NativeSpeech> replica_voice;
bool replica_speech_active=false;
bool replica_speech_held=false,replica_speech_ready=false,replica_start_released=false;
thread_local bool approved=false;
bool combo_music_active=false;
uint32_t combo_restore_track=UINT32_MAX;
thread_local bool combo_presentation=false;
bool readable(uint32_t object,unsigned bytes){return object>=0x80000000u && bytes<=guest_ram_size() && object<=guest_ram_end()-bytes;}
bool level_speech_source(uint32_t object,uint32_t track){
    if(!readable(object,0x6C) || mem_read32(object+0x48)!=0x802087A8u)return false;
    const auto config=mem_read32(object+0x54),reference=mem_read32(object+0x50);
    return reference>0 && reference<=32767 && readable(config,0x24) && mem_read32(config+0x1C)==track;
}
bool channel_running(unsigned channel){
    if(channel>6)return false;const auto record=0x802D59D0+channel*188,voice=mem_read32(record+0x4C);
    // ORIGINAL801113D0 starts a prepared stream. AXSetVoiceState801C75E0
    // writes AXVPB+146, the actual running field consumed by the AX decoder.
    return mem_read32(record)!=0 && readable(voice,0x148) && mem_read16(voice+0x146)==1;
}
void track_speech_output(uint64_t event){
    const auto channel=mem_read32(0x802DCCC0);if(channel>6)return;
    const auto voice=mem_read32(0x802D59D0+channel*188+0x4C);
    // ORIGINAL801C6974 copies AXVPB+138 to the separate DSP pool using the
    // voice index at+18. AXVPB+30 is another CPU copy, not that DSP pool.
    if(readable(voice,0x148)){const auto slot=mem_read32(voice+0x18);if(slot<64)audio_track_speech_voice(0x802C18A0+slot*236,event);}
}
void stream_start_hook(PPCContext& context){
    if(replica_simulation() && replica_voice && replica_speech_held && context.r[3]==mem_read32(0x802DCCC0)){
        if(!replica_speech_ready){replica_speech_ready=true;log_message("[coop-speech-client-prepared] sequence="+std::to_string(replica_voice->sequence));}
        if(!replica_start_released)return;
    }
    ssne_call_original(context,0x801113D0,start_original);
    const auto count=mem_read32(0x802DCCC0);
    if(count<=6)for(unsigned i=0;i<count;++i){const auto record=0x802D59D0+i*188,index=mem_read32(record+0x20);
        if(index<441 && channel_running(i))log_message("[coop-music-stream-running] replica="+std::to_string(replica_simulation())+" channel="+std::to_string(i)+" track="+std::to_string(mem_read32(0x80241178+index*28+12)));}
}
void request_hook(PPCContext& context){
    const auto track=context.r[3],force=context.r[4];const auto fade=float(context.ps0[1]);
    if(combo_presentation){ssne_call_original(context,0x801114B4,request_original);return;}
    if(replica_simulation() && !approved && mem_read32(0x802DB7E8)==4){log_message("[coop-music-local-request-blocked] track="+std::to_string(track));return;}
    if(combo_music_active)combo_restore_track=track;
    else ssne_call_original(context,0x801114B4,request_original);
    if(!replica_simulation() && native_music_track_valid(track) && force<=1 && std::isfinite(fade) && fade>=0 && fade<=600){
        request=NativeMusicRequest{++sequence,track,bool(force),fade};
        log_message("[coop-music-host-request] sequence="+std::to_string(sequence)+" track="+std::to_string(track)+" force="+std::to_string(force)+" fade="+std::to_string(fade));
    }
}
void play_hook(PPCContext& context){
    const auto track=context.r[3],loop=context.r[4];
    const auto source=context.r[31];const bool shared_speech=loop==0 && context.lr==0x80083750u && level_speech_source(source,track) && mem_read32(0x802DB7E8)==4;
    if(shared_speech && replica_simulation() && !approved){context.r[3]=0;log_message("[coop-speech-local-play-blocked] reference="+std::to_string(mem_read32(source+0x50)));return;}
    // Original80111540 enables adaptive background gains even for a one-shot
    // voice. If it overlaps a queued level crossfade, that resurrects gains
    // on inactive old music channels:8011063C then waits forever for their
    // sum to reach zero. Keep the pending switch in control until it starts;
    // speech still uses the original independent channel and loader.
    const auto pending_track=mem_read32(0x802DCD0Cu);
    const bool pending_switch=loop==0 && !mem_read32(0x802DCCF8u) &&
        pending_track!=UINT32_MAX && native_music_track_valid(pending_track);
    ssne_call_original(context,0x80111540,play_original);
    if(pending_switch && context.r[3] && mem_read32(0x802DCD0Cu)==pending_track){
        mem_write32(0x802DCCF8u,0);
        log_message("[coop-music-pending-switch-preserved] track="+std::to_string(pending_track)+" speech="+std::to_string(track));
    }
    log_message("[coop-music-native-play] track="+std::to_string(track)+" loop="+std::to_string(loop)+" result="+std::to_string(context.r[3])+" replica="+std::to_string(replica_simulation()));
    if(!replica_simulation() && !combo_music_active && context.r[3] && loop==1 && native_music_track_valid(track)){
        if(!request || request->track!=track)request=NativeMusicRequest{++sequence,track,false,0.f};
        request->started=true;
        log_message("[coop-music-host-queued] sequence="+std::to_string(request->sequence)+" track="+std::to_string(track));
    }
    if(shared_speech && !replica_simulation() && context.r[3] && native_music_track_valid(track)){
        speech=NativeSpeech{++speech_sequence,track,mem_read32(source+0x50)};
        track_speech_output(speech_sequence);
        log_message("[coop-speech-host-queued] sequence="+std::to_string(speech_sequence)+" track="+std::to_string(track)+" reference="+std::to_string(speech->reference));
    }
}
}
bool native_music_track_valid(uint32_t track){
    if(mem_read32(0x80000000u)!=0x47334250u || track==UINT32_MAX)return false;
    // ORIGINAL80111540 searches441 fixed StreamData catalog records,28bytes.
    for(unsigned i=0;i<441;++i)if(mem_read32(0x80241178+i*28+12)==track)return true;
    return false;
}
void install_native_music_observer(){
    if(request_original || mem_read32(0x80000000u)!=0x47334250u)return;
    const auto* change=ssne_find_func(0x801114B4);const auto* play=ssne_find_func(0x80111540);
    if(!change || !play || mem_read32(change->address)!=0x9421FFE0u || mem_read32(play->address)!=0x9421FFE0u)fatal("Unsupported native music controller.");
    const auto* start=ssne_find_func(0x801113D0);if(!start || mem_read32(start->address)!=0x9421FFE8u)fatal("Unsupported prepared native speech start.");
    request_original=change->func;play_original=play->func;start_original=start->func;ssne_install_hook(change->address,request_hook);ssne_install_hook(play->address,play_hook);ssne_install_hook(start->address,stream_start_hook);
    observe_native_entities([](const NativeEntityNotice& notice){if(notice.event==NativeEntityNotice::Event::LevelExit){if(replica_voice)audio_reconcile_speech(replica_voice->sequence,0);combo_music_active=false;combo_restore_track=UINT32_MAX;request.reset();speech.reset();replica_voice.reset();replica_speech_active=false;replica_speech_held=replica_speech_ready=replica_start_released=false;}});
}
uint32_t native_speech_playing_track(){
    if(mem_read32(0x80000000u)!=0x47334250u)return UINT32_MAX;
    // ORIGINAL80111620 selects the mono/speech channel after the music channels;
    // ORIGINAL80111208 stores its StreamData index at+20, start/play flags0/4.
    const auto channel=mem_read32(0x802DCCC0);if(channel>6)return UINT32_MAX;
    const auto record=0x802D59D0+channel*188;
    const auto index=mem_read32(record+0x20);
    if(index>440 || !channel_running(channel))return UINT32_MAX;
    // ORIGINAL801115B8 adds125*language for non-loop speech. The network keeps
    // the canonical source ID; each peer resolves its own localized catalog.
    const auto language=mem_read32(0x802DB838);const auto offset=uint64_t(language)*125;
    if(offset>index)return UINT32_MAX;
    const auto track=mem_read32(0x80241178+uint32_t(index-offset)*28+12);
    if(replica_voice && track==replica_voice->track && confirmed_replica_speech_sequence!=replica_voice->sequence){
        confirmed_replica_speech_sequence=replica_voice->sequence;
        log_message("[coop-speech-client-playing] sequence="+std::to_string(replica_voice->sequence)+" track="+std::to_string(track)+" reference="+std::to_string(replica_voice->reference));
    }
    return track;
}
bool native_background_music_running(){
    const auto count=mem_read32(0x802DCCC0);if(count>6)return true;
    for(unsigned channel=0;channel<count;++channel)if(channel_running(channel))return true;
    return false;
}
std::optional<NativeSpeech> native_speech(){
    if(!speech)return {};
    const bool playing=native_speech_playing_track()==speech->track;
    if(!playing){
        if(speech->started)return {};
        const auto channel=mem_read32(0x802DCCC0);if(channel>6)return {};
        const auto record=0x802D59D0+channel*188;
        if(!mem_read32(record) && !mem_read32(record+4) && !mem_read32(record+8))return {};
        return speech;
    }
    speech->started=true;
    if(confirmed_speech_sequence!=speech->sequence){confirmed_speech_sequence=speech->sequence;log_message("[coop-speech-host-started] sequence="+std::to_string(speech->sequence)+" track="+std::to_string(speech->track)+" reference="+std::to_string(speech->reference));}
    auto current=*speech;current.output_frames=audio_speech_output_frames(current.sequence);return current;
}
bool prepare_native_speech(PPCContext& context,const NativeSpeech& voice,std::string& error){
    if(!play_original || !replica_simulation() || !native_music_track_valid(voice.track) || context.r[13]!=0x802E22A0 || !readable(context.r[1]-0x600,0x600)){error="Replica native speech context unavailable";return false;}
    const auto base=context.r[1]-0x600;std::vector<uint8_t> stack(0x600);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(base+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=voice.track;call.r[4]=0;
    struct Approval{bool previous=approved;Approval(){approved=true;}~Approval(){approved=previous;}} approval;
    replica_voice=voice;replica_speech_held=true;replica_speech_ready=false;replica_start_released=false;
    try{ssne_call(call,0x80111540);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);
    if(!call.r[3]){replica_voice.reset();replica_speech_held=false;error="Replica native speech channel busy";return false;}
    replica_speech_active=true;replica_voice=voice;track_speech_output(voice.sequence);
    log_message("[coop-speech-client-queued] sequence="+std::to_string(voice.sequence)+" track="+std::to_string(voice.track)+" reference="+std::to_string(voice.reference));return true;
}
bool start_prepared_native_speech(PPCContext& context,uint64_t event,std::string& error){
    if(!replica_voice || replica_voice->sequence!=event || !replica_speech_active){error="Prepared native speech identity unavailable";return false;}
    replica_start_released=true;
    if(!replica_speech_ready)return true; // real native loader callback starts when ready
    if(!start_original || context.r[13]!=0x802E22A0 || !readable(context.r[1]-0x400,0x400)){error="Prepared native speech start context unavailable";return false;}
    const auto base=context.r[1]-0x400;std::vector<uint8_t> stack(0x400);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(base+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=mem_read32(0x802DCCC0);
    try{ssne_call(call,0x801113D0);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);
    log_message("[coop-speech-client-start-released] sequence="+std::to_string(event));return true;
}
bool apply_native_speech(PPCContext& context,const NativeSpeech& voice,std::string& error){return prepare_native_speech(context,voice,error) && start_prepared_native_speech(context,voice.sequence,error);}
bool stop_native_replica_speech(PPCContext& context,std::string& error){
    if(replica_voice)audio_reconcile_speech(replica_voice->sequence,0);
    if(!replica_speech_active)return true;
    const auto* stop=ssne_find_func(0x8011174C);const auto channel=mem_read32(0x802DCCC0);
    if(!replica_simulation() || !stop || mem_read32(stop->address)!=0x9421FFD8u || channel>6 || context.r[13]!=0x802E22A0 || !readable(context.r[1]-0x600,0x600)){error="Replica native speech stop unavailable";return false;}
    const auto base=context.r[1]-0x600;std::vector<uint8_t> stack(0x600);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(base+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=channel;call.r[4]=1; // native nonblocking stop
    try{ssne_call(call,stop->address);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);replica_speech_active=false;replica_voice.reset();replica_speech_held=replica_speech_ready=replica_start_released=false;log_message("[coop-speech-client-stopped]");return true;
}
std::optional<NativeMusicRequest> native_music_pending_request(){return request;}
std::optional<NativeMusicRequest> native_music_request(){
    if(!request || !request->started)return {};
    for(unsigned index=0;index<441;++index)if(mem_read32(0x80241178+index*28+12)==request->track){
        // Background crossfades alternate native channels. The combo track
        // need not occupy channel zero while the previous level track fades.
        const auto count=mem_read32(0x802DCCC0);if(count>6)return {};
        bool running=false;for(unsigned channel=0;channel<=count;++channel)if(channel_running(channel) && mem_read32(0x802D59D0+channel*188+0x20)==index)running=true;
        if(!running)return {};
        if(confirmed_music_sequence!=request->sequence){confirmed_music_sequence=request->sequence;log_message("[coop-music-host-started] sequence="+std::to_string(request->sequence)+" track="+std::to_string(request->track));}
        return request;
    }
    return {};
}
bool apply_native_music_request(PPCContext& context,const NativeMusicRequest& music,std::string& error){
    if(!request_original || !replica_simulation() || !native_music_track_valid(music.track) || !std::isfinite(music.fade) || music.fade<0 || music.fade>600 || context.r[13]!=0x802E22A0 || context.r[1]<0x80002000u || context.r[1]>guest_ram_end()-4){error="Replica native music request unavailable";return false;}
    if(combo_music_active){combo_restore_track=music.track;return true;}
    const auto base=context.r[1]-0x400;std::vector<uint8_t> stack(0x400);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(base+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=music.track;call.r[4]=music.force;call.ps0[1]=call.ps1[1]=music.fade;
    struct Approval{bool previous=approved;Approval(){approved=true;}~Approval(){approved=previous;}} approval;
    try{ssne_call(call,0x801114B4);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(base+i,stack[i]);
    log_message("[coop-music-client-request] sequence="+std::to_string(music.sequence)+" track="+std::to_string(music.track)+" fade="+std::to_string(music.fade));return true;
}
bool update_native_combo_music(PPCContext& context,uint32_t actor,std::string& error){
    if(!request_original || !ssne_find_func(0x80162844u) || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u ||
       context.r[13]!=0x802E22A0u || !readable(context.r[1]-0x600,0x600)){
        error="Local combo music context unavailable";return false;
    }
    const auto phase=mem_read32(actor+0x3AC);
    // Original800B5C00 enters the killing-spree track at phase4 and restores
    // the level track on5->6. Its multiplayer guard skips both music calls.
    const bool active=phase==4 || phase==5;
    if(active==combo_music_active)return true;
    const auto base=context.r[1]-0x600;std::vector<uint8_t> stack(0x600);
    std::memcpy(stack.data(),g_ram+(base-0x80000000u),stack.size());
    auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    uint32_t track=combo_restore_track;
    if(active){
        if(!ssne_find_func(0x80162844u)){error="Original combo music name resolver unavailable";return false;}
        call.r[3]=0x80212038u;ssne_call(call,0x80162844u);track=call.r[3];
    }
    if(!native_music_track_valid(track)){
        std::memcpy(g_ram+(base-0x80000000u),stack.data(),stack.size());
        error="Original combo/level music track unavailable";return false;
    }
    if(active){
        combo_restore_track=mem_read32(0x802DCD0Cu);
        if(!native_music_track_valid(combo_restore_track))combo_restore_track=mem_read32(0x802DCD10u);
    }
    struct Scope{bool old=combo_presentation;Scope(){combo_presentation=true;}~Scope(){combo_presentation=old;}} scope;
    call.r[3]=track;call.r[4]=1;call.ps0[1]=call.ps1[1]=active?.1:.05;
    ssne_call(call,0x801114B4u);combo_music_active=active;
    std::memcpy(g_ram+(base-0x80000000u),stack.data(),stack.size());
    log_message("[coop-local-combo-music] active="+std::to_string(active)+" track="+std::to_string(track));return true;
}
#include "native_music_state.inc"
}
