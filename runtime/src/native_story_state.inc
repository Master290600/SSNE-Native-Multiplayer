// Host control state accompanying the native cinematic/result objects in RAM.
namespace {
uint32_t story_word(const std::vector<uint8_t>& b,size_t at){uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|b.at(at+i);return v;}
uint64_t story_wide(const std::vector<uint8_t>& b,size_t at){return (uint64_t(story_word(b,at))<<32)|story_word(b,at+4);}
uint64_t story_hash(const std::vector<uint8_t>& b){uint64_t h=14695981039346656037ull;for(size_t i=0;i<b.size()-8;++i){h^=b[i];h*=1099511628211ull;}return h;}
void story_sign(std::vector<uint8_t>& b){const auto h=story_hash(b);for(unsigned i=0;i<8;++i)b[b.size()-8+i]=uint8_t(h>>(56-i*8));}
}
bool native_story_save_state(std::vector<uint8_t>& out,std::string& error){
    error.clear();NativeCutsceneCheckpoint camera;
    if(!capture_native_cutscene_checkpoint(camera,error))return false;
    if(result && !result->level.empty()){error="Unexpected native result level metadata";return false;}
    std::vector<uint8_t> b;auto word=[&](uint32_t v){for(int s=24;s>=0;s-=8)b.push_back(uint8_t(v>>s));};
    auto wide=[&](uint64_t v){word(uint32_t(v>>32));word(uint32_t(v));};
    word(0x4E535431);word(1);word(mem_read32(0x80000000));word(guest_ram_size());
    word(camera.camera);word(camera.reference);word(camera.active);word(save_host);
    wide(sequence);wide(saved_sequence);word(gui);word(gui?mem_read32(gui):0);word(bool(result));
    const auto r=native_results().value_or(NativeResults{});wide(r.sequence);word(r.score);word(r.active);for(auto v:r.values)word(f2u(v));
    wide(0);story_sign(b);out=std::move(b);return true;
}
bool native_story_load_state(const std::vector<uint8_t>& b,std::string& error){
    error.clear();auto fail=[&](const char* why){error=why;return false;};
    if(b.size()!=136 || story_word(b,0)!=0x4E535431 || story_word(b,4)!=1 || story_word(b,8)!=mem_read32(0x80000000) || story_word(b,12)!=guest_ram_size() || story_wide(b,128)!=story_hash(b))return fail("Invalid story checkpoint");
    NativeCutsceneCheckpoint camera{story_word(b,16),story_word(b,20),story_word(b,24)};
    if(!validate_native_cutscene_checkpoint(camera,error))return false;
    const auto seq=story_wide(b,32),saved=story_wide(b,40);const auto menu=story_word(b,48),table=story_word(b,52),present=story_word(b,56),active=story_word(b,72);
    if(story_word(b,28)!=unsigned(save_host) || seq>INT64_MAX || saved>seq || present>1 || active>1)return fail("Invalid result checkpoint counters/flags");
    NativeResults r;r.sequence=story_wide(b,60);r.score=story_word(b,68);r.active=active;
    for(unsigned i=0;i<13;++i){r.values[i]=u2f(story_word(b,76+i*4));if(!std::isfinite(r.values[i]) || std::abs(r.values[i])>1000000000.f)return fail("Invalid saved result value");}
    if(present){
        if(!r.sequence || r.sequence>seq || !readable(menu,0x90) || mem_read32(menu)!=table || bool(!mem_read32(menu+0x14) && mem_read32(menu+0x6C))!=bool(active))return fail("Saved results menu does not match restored RAM");
    }else{
        if(menu || table || r.sequence || r.score || active)return fail("Unexpected absent result metadata");
        for(auto v:r.values)if(v!=0)return fail("Unexpected absent result values");
    }
    sequence=seq;saved_sequence=saved;gui=menu;result=present?std::optional(r):std::nullopt;
    results_countdown.reset();host_results_open=present && active && save_host;
    restore_native_cutscene_checkpoint(camera);return true;
}
bool native_story_checkpoint_probe(std::string& error){
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_TEST_STORY_CHECKPOINT")){error="Story checkpoint probe disabled";return false;}
    std::vector<uint8_t> saved,after;if(!native_story_save_state(saved,error))return false;unsigned rejected=0;
    auto reject=[&](const auto& bad){std::string why;if(native_story_load_state(bad,why) || why.empty() || !native_story_save_state(after,why) || after!=saved)return false;++rejected;return true;};
    auto bad=saved;bad.pop_back();if(!reject(bad)){error="Truncated story checkpoint accepted";return false;}
    for(unsigned at:{0u,8u,12u,16u,24u,28u,40u,48u,56u,72u,76u}){bad=saved;for(unsigned i=0;i<4;++i)bad[at+i]=0xFF;story_sign(bad);if(!reject(bad)){error="Invalid story checkpoint accepted at "+std::to_string(at);return false;}}
    auto changed=saved;for(unsigned i=16;i<28;++i)changed[i]=0;const auto seq=story_wide(saved,32)+1;
    for(unsigned i=0;i<8;++i)changed[32+i]=uint8_t(seq>>(56-i*8));story_sign(changed);
    if(!native_story_load_state(changed,error) || !native_story_save_state(after,error) || after!=changed || !native_story_load_state(saved,error) || !native_story_save_state(after,error) || after!=saved){error="Story checkpoint positive restore failed";return false;}
    std::ofstream file("native-story-checkpoint.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(saved.data()),saved.size());
    std::ofstream report("native-story-checkpoint.json");report<<"{\"rejected\":"<<rejected<<",\"camera\":"<<story_word(saved,16)<<",\"camera_active\":"<<story_word(saved,24)<<",\"result_sequence\":"<<story_wide(saved,32)<<",\"saved_sequence\":"<<story_wide(saved,40)<<",\"result_present\":"<<story_word(saved,56)<<",\"result_active\":"<<story_word(saved,72)<<",\"positive_restore\":true}\n";
    if(!file || !report){error="Cannot write story checkpoint evidence";return false;}return true;
}
