// Host participant, called under the runtime checkpoint barrier. DSP decoder
// memory is part of core RAM; these are the accompanying native control values.
namespace {
uint64_t music_hash(const std::vector<uint8_t>& b){uint64_t h=14695981039346656037ull;for(size_t i=0;i<b.size()-8;++i){h^=b[i];h*=1099511628211ull;}return h;}
uint32_t music_word(const std::vector<uint8_t>& b,size_t at){uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|b.at(at+i);return v;}
uint64_t music_wide(const std::vector<uint8_t>& b,size_t at){return (uint64_t(music_word(b,at))<<32)|music_word(b,at+4);}
void music_sign(std::vector<uint8_t>& b){const auto h=music_hash(b);for(unsigned i=0;i<8;++i)b[b.size()-8+i]=uint8_t(h>>(56-i*8));}
bool music_safe(std::string& error){if(replica_simulation() || approved){error="Music checkpoint requires idle host context";return false;}return true;}
}
bool native_music_save_state(std::vector<uint8_t>& out,std::string& error){
    error.clear();if(!music_safe(error))return false;
    std::vector<uint8_t> b;
    auto word=[&](uint32_t v){for(int s=24;s>=0;s-=8)b.push_back(uint8_t(v>>s));};
    auto wide=[&](uint64_t v){word(uint32_t(v>>32));word(uint32_t(v));};
    word(0x4E4D4350);word(2);word(mem_read32(0x80000000));word(guest_ram_size());
    wide(sequence);wide(speech_sequence);wide(confirmed_music_sequence);wide(confirmed_speech_sequence);
    const auto m=request.value_or(NativeMusicRequest{});word(bool(request));wide(m.sequence);word(m.track);word(m.force);word(std::bit_cast<uint32_t>(m.fade));word(m.started);
    const auto s=speech.value_or(NativeSpeech{});word(bool(speech));wide(s.sequence);word(s.track);word(s.reference);wide(s.output_frames);word(s.started);
    const auto a=audio_speech_checkpoint();word(a.parameter_block);wide(a.sequence);wide(a.output);wide(a.target);wide(a.skipped);
    word(combo_music_active);word(combo_restore_track);wide(0);music_sign(b);out=std::move(b);return true;
}
bool native_music_load_state(const std::vector<uint8_t>& b,std::string& error){
    error.clear();if(!music_safe(error))return false;
    auto fail=[&](const char* why){error=why;return false;};
    if(b.size()!=160 || music_word(b,0)!=0x4E4D4350 || music_word(b,4)!=2 || music_word(b,8)!=mem_read32(0x80000000) || music_word(b,12)!=guest_ram_size() || music_wide(b,152)!=music_hash(b))return fail("Invalid native music checkpoint");
    const auto combo=music_word(b,144),restore=music_word(b,148);
    if(combo>1 || (restore!=UINT32_MAX && !native_music_track_valid(restore)))return fail("Invalid saved local combo music");
    const auto seq=music_wide(b,16),ss=music_wide(b,24),cm=music_wide(b,32),cs=music_wide(b,40);
    if(seq>INT64_MAX || ss>INT64_MAX || cm>seq || cs>ss)return fail("Invalid native music sequence");
    const auto present_m=music_word(b,48),force=music_word(b,64),started_m=music_word(b,72);
    NativeMusicRequest m{music_wide(b,52),music_word(b,60),force!=0,std::bit_cast<float>(music_word(b,68)),started_m!=0};
    const auto present_s=music_word(b,76),started_s=music_word(b,104);
    NativeSpeech s{music_wide(b,80),music_word(b,88),music_word(b,92),music_wide(b,96),started_s!=0};
    AudioSpeechCheckpoint a{music_word(b,108),music_wide(b,112),music_wide(b,120),music_wide(b,128),music_wide(b,136)};
    if(present_m>1 || force>1 || started_m>1 || present_s>1 || started_s>1)return fail("Invalid native music flags");
    if(present_m ? (!m.sequence || m.sequence>seq || !native_music_track_valid(m.track) || !std::isfinite(m.fade) || m.fade<0 || m.fade>600) : (m.sequence || m.track || force || music_word(b,68) || started_m))return fail("Invalid saved music request");
    if(present_s ? (!s.sequence || s.sequence>ss || !native_music_track_valid(s.track) || !s.reference || s.reference>32767 || s.output_frames>INT64_MAX) : (s.sequence || s.track || s.reference || s.output_frames || started_s))return fail("Invalid saved speech request");
    const auto pool=0x002C18A0u;
    if(a.sequence>ss || a.output>INT64_MAX || a.target>INT64_MAX || a.skipped>a.output || a.target>a.output+3200 || (a.sequence ? (a.parameter_block<pool || a.parameter_block>=pool+64*236 || (a.parameter_block-pool)%236) : (a.parameter_block || a.output || a.target || a.skipped)))return fail("Invalid speech decoder accounting");
    sequence=seq;speech_sequence=ss;confirmed_music_sequence=cm;confirmed_speech_sequence=cs;combo_music_active=combo;combo_restore_track=restore;
    request=present_m?std::optional(m):std::nullopt;speech=present_s?std::optional(s):std::nullopt;
    audio_restore_speech_checkpoint(a);return true;
}
bool native_music_checkpoint_probe(std::string& error){
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_TEST_MUSIC_CHECKPOINT")){error="Music checkpoint probe disabled";return false;}
    std::vector<uint8_t> saved,after;if(!native_music_save_state(saved,error))return false;
    unsigned rejected=0;
    auto reject=[&](const auto& bad){std::string why;if(native_music_load_state(bad,why) || why.empty() || !native_music_save_state(after,why) || after!=saved)return false;++rejected;return true;};
    auto bad=saved;bad.pop_back();if(!reject(bad)){error="Truncated music state accepted";return false;}
    for(unsigned at:{0u,8u,12u,32u,48u,60u,64u,68u,76u,88u,92u,108u,112u,128u}){bad=saved;for(unsigned i=0;i<4;++i)bad[at+i]=0xFF;music_sign(bad);if(!reject(bad)){error="Invalid music state accepted or changed live state at "+std::to_string(at);return false;}}
    auto changed=saved;const auto output=music_wide(saved,120)+1;for(unsigned i=0;i<8;++i)changed[120+i]=uint8_t(output>>(56-i*8));music_sign(changed);
    if(!native_music_load_state(changed,error) || !native_music_save_state(after,error) || after!=changed || !native_music_load_state(saved,error) || !native_music_save_state(after,error) || after!=saved){error="Speech accounting positive restore failed";return false;}
    std::ofstream file("native-music-checkpoint.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(saved.data()),saved.size());
    std::ofstream report("native-music-checkpoint.json");report<<"{\"rejected\":"<<rejected<<",\"music_sequence\":"<<music_wide(saved,16)<<",\"speech_sequence\":"<<music_wide(saved,24)<<",\"speech_frames\":"<<music_wide(saved,120)<<",\"positive_decoder_restore\":true}\n";
    if(!file || !report){error="Cannot write music checkpoint evidence";return false;}return true;
}
