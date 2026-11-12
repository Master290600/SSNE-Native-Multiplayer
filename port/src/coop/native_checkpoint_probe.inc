// Included inside the native session's anonymous namespace; hidden fixtures only.
bool test_composed_checkpoint(NativeWorld& live,PPCContext& context,uint64_t frame,std::string& error,ssne::RuntimeCheckpointHooks session={}) {
    using ssne::g_ram;using ssne::g_locked_cache;
    const auto sample=live.capture(frame);
    std::vector<uint8_t> core,external,live_before,party_before;
    if(!live.save_lifetimes(live_before,error) || !ssne::native_party_save_state(party_before,error))return false;
    { NativeCheckpointParticipant participant(live,session);
      if(!ssne::runtime_checkpoint_capture(context,core,external,participant.hooks(),error))return false; }
    auto write=[&](const char* path,const auto& bytes){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return bool(f);};
    if(!write("native-composed-core.state",core) || !write("native-composed-external.bin",external)){error="Cannot write composed checkpoint evidence";return false;}
    const auto original=context;
    const std::vector<uint8_t> ram(g_ram,g_ram+ssne::guest_ram_size()),cache(g_locked_cache,g_locked_cache+ssne::kLockedCacheSize);
    auto word=[](const auto& b,size_t p){uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|b.at(p+i);return v;};
    auto resign=[](auto& b,size_t begin,size_t end){uint64_t h=14695981039346656037ull;for(size_t i=begin;i<end-8;++i){h^=b[i];h*=1099511628211ull;}for(unsigned i=0;i<8;++i)b[end-8+i]=uint8_t(h>>(56-8*i));};
    // Change a valid effect counter in the target. A later world failure must
    // restore the previous effect state as well as RAM and the party.
    const auto header=session.capture?28:24;
    const auto world_at=header+word(external,4),world_end=world_at+word(external,8),effect_end=world_end+word(external,12),music_end=effect_end+word(external,16),story_end=music_end+word(external,20);
    auto canonical_session=[](auto b){if(!b.empty()){std::fill_n(b.begin()+76,4,0);b.resize(b.size()-8);}return b;};
    const std::vector<uint8_t> session_before(external.begin()+story_end,external.end()-8);
    auto canonical_effect=[&](std::vector<uint8_t> bytes){size_t at=124;for(unsigned stride:{64u,52u,40u,60u,64u}){const auto n=word(bytes,at);at+=4+n*stride;}const auto n=word(bytes,at);at+=4;for(unsigned i=0;i<n;++i,at+=72)std::fill_n(bytes.begin()+at+64,8,0);bytes.resize(bytes.size()-8);return bytes;};
    const std::vector<uint8_t> effects_before(external.begin()+world_end,external.begin()+effect_end);
    auto target=external;uint64_t order=(uint64_t(word(target,world_end+108))<<32)|word(target,world_end+112);++order;
    for(unsigned i=0;i<8;++i)target[world_end+108+i]=uint8_t(order>>(56-8*i));
    const std::vector<uint8_t> music_before(external.begin()+effect_end,external.begin()+music_end);
    const std::vector<uint8_t> story_before(external.begin()+music_end,external.begin()+story_end);
    const uint64_t music_sequence=((uint64_t(word(target,effect_end+16))<<32)|word(target,effect_end+20))+1;
    for(unsigned i=0;i<8;++i)target[effect_end+16+i]=uint8_t(music_sequence>>(56-8*i));
    const uint64_t story_sequence=((uint64_t(word(target,music_end+32))<<32)|word(target,music_end+36))+1;
    for(unsigned i=0;i<8;++i)target[music_end+32+i]=uint8_t(story_sequence>>(56-8*i));
    if(session.capture){const uint64_t afk_epoch=((uint64_t(word(target,story_end+32))<<32)|word(target,story_end+36))+1;for(unsigned i=0;i<8;++i)target[story_end+32+i]=uint8_t(afk_epoch>>(56-8*i));resign(target,story_end,target.size()-8);}
    resign(target,music_end,story_end);resign(target,effect_end,music_end);resign(target,world_end,effect_end);resign(target,0,target.size());
    const std::vector<uint8_t> effects_target(target.begin()+world_end,target.begin()+effect_end);
    const std::vector<uint8_t> music_target(target.begin()+effect_end,target.begin()+music_end);
    const std::vector<uint8_t> story_target(target.begin()+music_end,target.begin()+story_end);
    const std::vector<uint8_t> session_target(target.begin()+story_end,target.end()-8);
    auto bad=target;
    bool corrupted=false;
    for(size_t at=world_at+60;at+36<=world_end-8;at+=36)if(word(bad,at+20)&4){bad[at+12]=0x80;bad[at+13]=bad[at+14]=bad[at+15]=0;corrupted=true;break;}
    // The original result screen has already unloaded gameplay actors. Exercise
    // world validation there with an invalid version instead of inventing one.
    if(!corrupted)bad[world_at+7]^=1;
    resign(bad,world_at,world_end);resign(bad,0,bad.size());
    unsigned rejected=0;
    for(unsigned mode=0;mode<3;++mode) {
        context.r[3]^=0x78512349u;g_ram[0x4000]^=0xAB;g_locked_cache[0]^=0x71;
        const auto changed=context;const auto changed_byte=g_ram[0x4000],changed_cache=g_locked_cache[0];
        bool accepted=false,effect_changed=false;unsigned calls=0,commits=0,aborts=0;
        {
            NativeCheckpointParticipant participant(live,session);auto hooks=participant.hooks();
            const auto prepare=hooks.restore;const auto commit=hooks.commit;
            const auto abort=hooks.abort;
            hooks.restore=[&](const auto& bytes,std::string& why){
                ++calls;
                if(calls==1 && (memcmp(&context,&original,sizeof(context)) || g_ram[0x4000]!=ram[0x4000])){why="World validation ran before restored RAM";return false;}
                const bool prepared=prepare(bytes,why);
                if(calls==1){std::vector<uint8_t> effects,music,story;std::string effect_error;effect_changed=ssne::native_effect_save_state(effects,effect_error) && canonical_effect(effects)==canonical_effect(effects_target) && ssne::native_music_save_state(music,effect_error) && music==music_target && ssne::native_story_save_state(story,effect_error) && story==story_target;}
                if(calls==1 && session.capture){std::vector<uint8_t> current;effect_changed=effect_changed && session.capture(current,why) && canonical_session(current)==canonical_session(session_target);}
                if(!prepared)return false;
                if(calls==1 && mode){if(mode==2)throw std::runtime_error("Injected failure with prepared world");why="Injected rejection with prepared world";return false;}
                return true;
            };
            hooks.commit=[&]{++commits;commit();};
            hooks.abort=[&]{++aborts;abort();};
            accepted=ssne::runtime_checkpoint_restore(context,core,mode?target:bad,hooks,error);
            if(!accepted && aborts==1){std::vector<uint8_t> unlocked;if(!live.save_lifetimes(unlocked,error) || unlocked!=live_before){error="Aborted checkpoint retained a changed world";return false;}}
        } // releases an uncommitted world candidate after rollback
        if(accepted || !effect_changed || calls!=2 || commits || aborts!=1 || memcmp(&context,&changed,sizeof(context)) || g_ram[0x4000]!=changed_byte || g_locked_cache[0]!=changed_cache){error="Composed checkpoint did not roll back changed runtime/effects";return false;}
        context.r[3]^=0x78512349u;g_ram[0x4000]^=0xAB;g_locked_cache[0]^=0x71;
        std::vector<uint8_t> after;
        if(!live.save_lifetimes(after,error) || after!=live_before || memcmp(g_ram,ram.data(),ram.size()) || memcmp(g_locked_cache,cache.data(),cache.size())){error="Aborted world preparation changed identities/epoch/RAM";return false;}
        if(!ssne::native_party_save_state(after,error) || after!=party_before){error="Aborted world preparation changed native party";return false;}
        if(!ssne::native_effect_save_state(after,error) || canonical_effect(after)!=canonical_effect(effects_before)){error="Aborted world preparation changed native effects";return false;}
        if(!ssne::native_music_save_state(after,error) || after!=music_before){error="Aborted world preparation changed native music";return false;}
        if(!ssne::native_story_save_state(after,error) || after!=story_before){error="Aborted world preparation changed native story state";return false;}
        if(session.capture && (!session.capture(after,error) || canonical_session(after)!=canonical_session(session_before))){error="Aborted world preparation changed native session";return false;}
        ++rejected;
    }
    // Commit into an initially empty index against real restored RAM. The live
    // session's epoch remains untouched until all other participants are ready.
    NativeWorld destination;
    for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player)destination.set_player_owner(unsigned(std::get<int64_t>(e.properties.at("player_slot"))),e.owner);
    context.r[3]^=0x34125678u;g_ram[0x4000]^=0xF1;
    unsigned commits=0;
    {
        NativeCheckpointParticipant participant(destination,session);auto hooks=participant.hooks();const auto commit=hooks.commit;
        hooks.commit=[&]{++commits;commit();};
        if(!ssne::runtime_checkpoint_restore(context,core,external,hooks,error))return false;
    }
    const auto restored=destination.capture(frame);
    if(commits!=1 || restored.epoch!=sample.epoch+1 || restored.entities!=sample.entities || restored.unmapped.size()!=sample.unmapped.size() || restored.created!=sample.created || restored.removed!=sample.removed || memcmp(&context,&original,sizeof(context)) || memcmp(g_ram,ram.data(),ram.size())){error="Composed checkpoint commit differs from original world";return false;}
    for(const auto& [id,e]:sample.entities)if(destination.local_address(id)!=live.local_address(id)){error="Composed checkpoint identity binding differs";return false;}
    for(size_t i=0;i<sample.unmapped.size();++i)if(restored.unmapped[i].address!=sample.unmapped[i].address || restored.unmapped[i].vtable!=sample.unmapped[i].vtable){error="Composed checkpoint lost unknown native object";return false;}
    std::vector<uint8_t> after;
    if(!live.save_lifetimes(after,error) || after!=live_before){error="Isolated commit changed live session epoch";return false;}
    std::ofstream audit("native-composed-checkpoint.json");
    audit<<"{\"post_ram_rejections\":"<<rejected<<",\"commit_calls\":"<<commits<<",\"entities\":"<<restored.entities.size()<<",\"unmapped\":"<<restored.unmapped.size()<<",\"live_epoch\":"<<sample.epoch<<",\"restored_epoch\":"<<restored.epoch<<",\"live_index_unchanged\":true,\"core_world_restored\":true,\"changed_effects_rolled_back\":true,\"scope\":\"Runtime/party/world/effects composition; successful world commit uses separate index. Historical live session rewind NOT VERIFIED\"}\n";
    if(!audit){error="Cannot write composed checkpoint audit";return false;}
    return true;
}
