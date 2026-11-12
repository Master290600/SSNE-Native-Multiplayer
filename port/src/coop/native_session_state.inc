// Host gameplay state outside guest RAM. Called only at a runtime safe point
// while session_checkpoint_hooks() holds the server's control lease.
void NativeSession::configure_saves(savefile::Identity identity) {
    std::lock_guard lock(save_mutex_);save_identity_=std::move(identity);
}
void NativeSession::configure_startup_save(savefile::State state,int slot) {
    if(!server_ || queued_ || slot<1 || slot>5)throw std::runtime_error("Invalid startup save request");
    startup_save_=std::move(state);startup_slot_=slot;
}
bool NativeSession::request_save(bool save,int slot,std::string path,std::string& error) {
    std::lock_guard lock(save_mutex_);
    if(!server_){error="Only the host can save or load the cooperative session";return false;}
    if(!save_identity_){error="Cooperative saves are not initialized";return false;}
    if(save_busy_){error="A cooperative save operation is already pending";return false;}
    if(slot<1 || slot>5 || path.empty()){error="Invalid cooperative save slot";return false;}
    save_request_=SaveRequest{save,slot,std::move(path),std::chrono::steady_clock::now()+std::chrono::seconds(10)};save_busy_=true;return true;
}
bool NativeSession::take_save_result(SaveResult& result) {
    std::lock_guard lock(save_mutex_);if(!save_result_)return false;
    result=std::move(*save_result_);save_result_.reset();save_busy_=false;return true;
}
void NativeSession::process_save_request(PPCContext& context,bool ready,uint64_t frame) {
    {std::lock_guard lock(save_mutex_);if(!save_request_)return;}
    std::string why;
    if(ssne::runtime_checkpoint_visit([&]{perform_save_request(context,ready,frame);},why))return;
    std::lock_guard lock(save_mutex_);
    if(save_request_ && std::chrono::steady_clock::now()>=save_request_->deadline){
        save_result_=SaveResult{save_request_->save,false,save_request_->slot,"No safe save/load point: "+why};save_request_.reset();
    }
}
void NativeSession::perform_save_request(PPCContext& context,bool ready,uint64_t frame) {
    std::optional<SaveRequest> request;savefile::Identity identity;
    {std::lock_guard lock(save_mutex_);if(!save_request_)return;request=std::move(save_request_);save_request_.reset();identity=*save_identity_;}
    SaveResult result{request->save,false,request->slot,{}};
    const auto before_epoch=epoch_;
    bool restoring=false;
    try{
        if(!ready)throw std::runtime_error("Wait until the cooperative level has finished loading");
        identity.game=mem_read32(0x80000000);identity.ram=ssne::guest_ram_size();identity.capacity=capacity_;
        std::string error;
        if(request->save){
            savefile::State state;state.identity=identity;state.timestamp=uint64_t(std::time(nullptr));
            {NativeCheckpointParticipant participant(native_,session_checkpoint_hooks());
            if(!ssne::runtime_checkpoint_capture(context,state.core,state.extra,participant.hooks(),error))throw std::runtime_error(error);}
            savefile::write_atomic(std::filesystem::u8path(request->path),savefile::encode(state));
            result.message="Cooperative session saved";
        }else{
            const auto state=savefile::read(std::filesystem::u8path(request->path),identity);
            restoring=true;const bool restored=restore_historical_checkpoint(context,state.core,state.extra,error);restoring=false;
            if(!restored)throw std::runtime_error(error);
            result.message="Cooperative session loaded; reconnecting player views";
        }
        result.ok=true;
        write_save_test_evidence(request->save,frame);
    }catch(const std::exception& e){if(restoring)throw;result.ok=false;result.message=e.what();}
    if(log_)log_("[coop-save-result] save="+std::to_string(result.save)+" slot="+std::to_string(result.slot)+" ok="+std::to_string(result.ok)+" frame="+std::to_string(frame)+" old_epoch="+std::to_string(before_epoch)+" epoch="+std::to_string(epoch_)+" message="+result.message);
    std::lock_guard lock(save_mutex_);save_result_=std::move(result);
}
void NativeSession::write_save_test_evidence(bool save,uint64_t frame) {
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_TEST_PUBLIC_SAVE")){
            std::string error;
            WorldState state;state.epoch=epoch_;state.revision=1;state.tick=frame;state.level=level_;state.entities=native_.capture(frame).entities;
            const auto bytes=encode_delta(difference(nullptr,state));std::ofstream file(save?"public-saved-world.bin":"public-loaded-world.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            if(!file)throw std::runtime_error("Cannot write public save test evidence");
            std::vector<uint8_t> story;
            if(!ssne::native_story_save_state(story,error))throw std::runtime_error(error);
            std::ofstream story_file(save?"public-saved-story.bin":"public-loaded-story.bin",std::ios::binary);
            story_file.write(reinterpret_cast<const char*>(story.data()),story.size());
            if(!story_file)throw std::runtime_error("Cannot write public story test evidence");
            dump_native_profiles(save?"public-saved-profiles.tsv":"public-loaded-profiles.tsv");
        }
}
bool NativeSession::save_session_checkpoint(std::vector<uint8_t>& out,std::string& error) const {
    if(!server_){error="Only the authoritative host has session checkpoints";return false;}
    if(active_movie_sequence_ || ssne::native_movie_presenting()){error="Wait until the video has finished before saving";return false;}
    try {
        checkpoint::Writer w;w.u32(0x4E534331);w.u32(mem_read32(0x80000000));w.u32(ssne::guest_ram_size());w.u32(capacity_);
        w.u64(epoch_);w.u64(cutscene_epoch_);w.u64(afk_epoch_);
        w.flag(queued_);w.flag(host_ready_);w.flag(level_loading_);w.u64(loading_epoch_);
        w.u32(active_cutscene_object_);w.u64(active_cutscene_vote_);w.flag(cutscene_skip_applied_);
        const auto remaining=std::chrono::duration_cast<std::chrono::microseconds>(afk_next_decision_-std::chrono::steady_clock::now()).count();
        w.u32(uint32_t(std::clamp<int64_t>(remaining,0,125000)));
        WorldState configuration;configuration.epoch=epoch_;configuration.revision=1;configuration.level=level_;configuration.mission=setup_;
        w.blob(encode_delta(difference(nullptr,configuration)));w.blob(server_->checkpoint_authority());
        auto id=[&](PlayerId p){w.u32(p.slot);w.u64(p.generation);};
        for(unsigned i=0;i<max_players;++i){
            id(players_[i]);w.u64(characters_[i]);
            const auto& j=join_placements_[i];id(j.player);w.u32(j.actor);w.u64(j.spawn_revision);w.flag(j.requested);
            const auto& a=afk_aim_leases_[i];id(a.player);w.u32(a.actor);w.u32(a.aim);w.flag(a.saved);
            const auto& l=native_life_wait_[i];id(l.player);w.u32(l.actor);w.u64(l.spawn_revision);
            const auto& r=afk_respawn_wait_[i];w.u32(r.actor);w.u64(r.spawn_revision);
            w.u32(afk_follow_slots_[i]);w.flag(afk_native_active_[i]);w.flag(afk_states_[i]);w.u32(afk_decisions_[i]);
            const auto& input=afk_inputs_[i];const auto& p=input.pad;
            for(auto v:{uint32_t(p.buttons),uint32_t(p.stick_x),uint32_t(p.stick_y),uint32_t(p.cstick_x),uint32_t(p.cstick_y),uint32_t(p.trigger_l),uint32_t(p.trigger_r),uint32_t(input.pressed),uint32_t(input.yaw),uint32_t(input.pitch),uint32_t(input.weapon)})w.u32(v);
            const auto b=afk_bots_[i].checkpoint();w.u32(b.target);for(auto f:b.previous)w.real(f);w.u32(b.stalled);w.u32(b.escape);w.flag(b.moving);w.u32(uint32_t(b.trail.size()));for(const auto& point:b.trail)for(auto f:point)w.real(f);
        }
        const auto aim=native_afk_manual_aim_bindings();w.u32(uint32_t(aim.size()));for(auto [actor,pointer]:aim){w.u32(actor);w.u32(pointer);}
        w.blob(save_native_frontend_state());out=w.finish();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeSession::load_session_checkpoint(const std::vector<uint8_t>& bytes,std::string& error,bool reconcile_connections) {
    if(!server_){error="Only the authoritative host has session checkpoints";return false;}
    try {
        checkpoint::Reader r(bytes,2*1024*1024);
        auto require=[](bool ok){if(!ok)throw std::runtime_error("Invalid native session checkpoint state");};
        require(r.u32()==0x4E534331 && r.u32()==mem_read32(0x80000000) && r.u32()==ssne::guest_ram_size() && r.u32()==capacity_);
        const auto epoch=r.u64(),cutscene_epoch=r.u64(),afk_epoch=r.u64();require(epoch!=0);
        const bool queued=r.flag(),ready=r.flag(),loading=r.flag();const auto loading_epoch=r.u64();
        const auto camera=r.u32();const auto vote=r.u64();const auto skipped=r.flag();const auto delay=r.u32();require(delay<=125000);
        WorldDelta delta;WorldState configuration;
        require(decode_delta(r.blob(1024*1024),delta,error) && delta.full && apply_delta(configuration,delta,error));
        require(configuration.epoch==epoch && configuration.entities.empty() && !configuration.vote.cutscene);
        for(const auto& p:configuration.players)require(!p.connected);
        const auto authority=r.blob(4096);
        require(authority.size()>=32);
        uint64_t saved_vote=0;for(unsigned i=16;i<24;++i)saved_vote=(saved_vote<<8)|authority[i];require(vote==saved_vote);
        auto players=players_;auto characters=characters_;auto joins=join_placements_;auto leases=afk_aim_leases_;
        auto life=native_life_wait_;auto respawn=afk_respawn_wait_;auto follow=afk_follow_slots_;auto active=afk_native_active_;
        auto afk=afk_states_;auto decisions=afk_decisions_;auto inputs=afk_inputs_;auto bots=afk_bots_;
        auto id=[&](){PlayerId p;p.slot=r.u32();p.generation=r.u64();require(p==PlayerId{} || (p.slot<capacity_ && p.generation));return p;};
        auto actor_valid=[&](uint32_t actor,unsigned slot){return actor>=0x80000000u && actor<=ssne::guest_ram_end()-0x790u && mem_read32(actor+0x48)==0x802121E8u && mem_read32(actor+0x17C)==slot && mem_read32(ssne::native_party_registry()+slot*4)==actor;};
        for(unsigned i=0;i<max_players;++i){
            players[i]=id();characters[i]=r.u64();require(players[i]==PlayerId{} || players[i].slot==i);
            require(!characters[i] || players[i].generation);for(unsigned j=0;j<i;++j)require(!characters[i] || characters[i]!=characters[j]);
            auto& j=joins[i];j.player=id();j.actor=r.u32();j.spawn_revision=r.u64();j.requested=r.flag();
            require(j.player==PlayerId{} || j.player==players[i]);require(!j.actor || actor_valid(j.actor,i));require(!j.requested || j.actor);
            require(j.player.generation || (!j.actor && !j.spawn_revision && !j.requested));
            auto& a=leases[i];a.player=id();a.actor=r.u32();a.aim=r.u32();a.saved=r.flag();
            require(!a.actor || (a.player==players[i] && actor_valid(a.actor,i) && a.aim>=0x80000000u && a.aim<=ssne::guest_ram_end()-0x58u && mem_read32(a.actor+0x37C)==a.aim));
            require(a.actor || (!a.aim && a.player==PlayerId{} && !a.saved));
            auto& l=life[i];l.player=id();l.actor=r.u32();l.spawn_revision=r.u64();require(!l.actor || (l.player==players[i] && actor_valid(l.actor,i) && l.spawn_revision<=ssne::native_party_spawn_revision(l.actor)));
            require(l.actor || (l.player==PlayerId{} && !l.spawn_revision));
            auto& p=respawn[i];p.actor=r.u32();p.spawn_revision=r.u64();require(!p.actor || (actor_valid(p.actor,i) && p.spawn_revision<=ssne::native_party_spawn_revision(p.actor)));
            require(p.actor || !p.spawn_revision);
            follow[i]=r.u32();active[i]=r.flag();afk[i]=r.flag();decisions[i]=r.u32();require(follow[i]<=max_players && (!active[i] || (i<capacity_ && afk[i])));
            auto small=[&](uint32_t limit){auto v=r.u32();require(v<=limit);return v;};
            auto& input=inputs[i];input.pad.buttons=uint16_t(small(0xFFFF));input.pad.stick_x=uint8_t(small(255));input.pad.stick_y=uint8_t(small(255));input.pad.cstick_x=uint8_t(small(255));input.pad.cstick_y=uint8_t(small(255));input.pad.trigger_l=uint8_t(small(255));input.pad.trigger_r=uint8_t(small(255));input.pressed=uint16_t(small(0xFFFF));input.yaw=std::bit_cast<int32_t>(r.u32());input.pitch=std::bit_cast<int32_t>(r.u32());input.weapon=uint8_t(small(255));
            require(!(input.pad.buttons&~uint16_t(0x1F7F)) && !(input.pressed&~uint16_t(0x1F7F)) && input.yaw>=-2000000 && input.yaw<=2000000 && input.pitch>=-2000000 && input.pitch<=2000000 && (input.weapon==255 || input.weapon<32));
            port::netplay::CompanionBot::Checkpoint b;b.target=r.u32();for(auto& f:b.previous)f=r.real();b.stalled=r.u32();b.escape=r.u32();b.moving=r.flag();const auto count=small(128);for(unsigned n=0;n<count;++n){std::array<float,3> point;for(auto& f:point)f=r.real();b.trail.push_back(point);}require(bots[i].restore_checkpoint(std::move(b)));
        }
        std::map<uint32_t,uint32_t> bindings;const auto count=r.u32();require(count<=capacity_);
        for(unsigned n=0;n<count;++n){const auto actor=r.u32(),aim=r.u32();require(bindings.emplace(actor,aim).second);const auto lease=std::find_if(leases.begin(),leases.end(),[&](const auto& a){return a.actor==actor && a.aim==aim;});require(actor && lease!=leases.end());}
        const auto frontend=r.blob(4096);r.done();
        if(!restore_native_frontend_state(frontend,error,false))return false;
        require(!camera || (camera>=0x80000000u && camera<=ssne::guest_ram_end()-0x180u && mem_read32(camera+0x48)==0x80203078u));
        require(bool(camera)==bool(vote) && (!skipped || vote));
        // Validate all allocations and native bindings before changing live state.
        // An old map may refer to the pre-load RAM. Do not mutate it until
        // authority validation passes; the enclosing transaction restores RAM
        // before applying the backup on a later failure.
        if(!validate_native_afk_manual_aim_bindings(bindings,error))return false;
        if(reconcile_connections){
            if(!server_->restore_gameplay_authority(authority,error))return false;
            const auto current=server_->participants();
            std::array<uint64_t,max_players> saved_generations{},live_generations{};
            for(unsigned i=0;i<max_players;++i){
                saved_generations[i]=players[i].generation;live_generations[i]=current[i].id.generation;
                const auto previous=players[i];players[i]=current[i].id;
                if(!current[i].connected){
                    bindings.erase(leases[i].actor);characters[i]=0;joins[i]={};leases[i]={};life[i]={};respawn[i]={};
                    follow[i]=max_players;active[i]=afk[i]=false;decisions[i]=0;inputs[i]={};bots[i]={};
                }else{
                    if(joins[i].player.generation)joins[i].player=players[i];
                    if(leases[i].actor)leases[i].player=players[i];
                    if(life[i].actor)life[i].player=players[i];
                    // A newly occupied saved slot uses the normal safe join
                    // placement, rather than activating an arbitrary dormant actor.
                    if(!previous.generation)joins[i]={players[i]};
                }
            }
            ssne::rebind_native_effect_sources(saved_generations,live_generations);
        }else if(!server_->restore_checkpoint_authority(authority,error))return false;
        if(!restore_native_afk_manual_aim_bindings(std::move(bindings),error))return false;
        if(!restore_native_frontend_state(frontend,error,true))return false;
        players_=players;characters_=characters;join_placements_=joins;afk_aim_leases_=leases;native_life_wait_=life;afk_respawn_wait_=respawn;
        afk_follow_slots_=follow;afk_native_active_=active;afk_states_=afk;afk_decisions_=decisions;afk_inputs_=inputs;afk_bots_.swap(bots);
        epoch_=epoch;cutscene_epoch_=cutscene_epoch;afk_epoch_=afk_epoch;queued_=queued;host_ready_=ready;level_loading_=loading;loading_epoch_=loading_epoch;
        active_cutscene_object_=camera;active_cutscene_vote_=vote;active_movie_sequence_=0;cutscene_skip_applied_=skipped;
        level_.swap(configuration.level);setup_.swap(configuration.mission);
        afk_next_decision_=std::chrono::steady_clock::now()+std::chrono::microseconds(delay);return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
ssne::RuntimeCheckpointHooks NativeSession::session_checkpoint_hooks() {
    auto lease=server_->checkpoint_control_lease();
    return {[this,lease](auto& bytes,auto& error){return save_session_checkpoint(bytes,error);},
            [this,lease](const auto& bytes,auto& error){return load_session_checkpoint(bytes,error);},{},{}};
}
bool NativeSession::restore_historical_checkpoint(PPCContext& context,const std::vector<uint8_t>& core,const std::vector<uint8_t>& extra,std::string& error,bool reject_before_commit) {
    // Hooks are process-local. Saved native AFK leases may exist even when
    // nobody has enabled a bot in this newly started process yet.
    if(!prepare_native_afk_manual_aim(error))return false;
    auto session=session_checkpoint_hooks();const auto live=server_->checkpoint_authority();
    bool target_restore=true;
    // Runtime restores the rollback RAM before calling this hook a second time.
    // Only the requested historical state may reconcile membership; rollback
    // must recover the captured state exactly, including votes and AFK policy.
    session.restore=[&](const auto& bytes,auto& why){return load_session_checkpoint(bytes,why,target_restore);};
    std::unique_ptr<AuthoritativeServer::PreparedResume> resume;
    uint64_t new_epoch=0,producer_epoch=0,projectile_epoch=0;NativeEventCursors tails{};
    ssne::NativeProducerCursors runtime_tails{};
    session.commit=[&]{
        resume->commit();
        if(!ssne::acknowledge_native_event_publication(producer_epoch,runtime_tails) || !native_.acknowledge_projectile_visuals(projectile_epoch,tails[9]))ssne::fatal("Checkpoint producer baseline changed during commit");
        epoch_=cutscene_epoch_=afk_epoch_=new_epoch;characters_={};host_ready_=false;
        active_cutscene_object_=0;active_cutscene_vote_=0;active_movie_sequence_=0;cutscene_skip_applied_=false;cutscene_trace_states_.clear();
    };
    session.abort=[&]{resume.reset();};
    NativeCheckpointParticipant participant(native_,std::move(session),[&](uint64_t epoch,uint64_t projectile_tail,std::string& why){
        if(std::exchange(reject_before_commit,false)){why="Historical rollback probe";return false;}
        const auto producer=ssne::native_event_producer_snapshot();producer_epoch=producer.epoch;runtime_tails=producer.cursors;
        std::copy_n(runtime_tails.begin(),9,tails.begin());
        tails[10]=runtime_tails[9];tails[11]=runtime_tails[10];
        // The prepared lifetime candidate holds the world lock. Use its saved
        // tail supplied by the checkpoint participant, never the live producer.
        projectile_epoch=epoch;tails[9]=projectile_tail;
        resume=server_->prepare_checkpoint_resume(live,epoch,level_,setup_,tails,why);new_epoch=epoch;return bool(resume);
    });
    auto hooks=participant.hooks();auto restore=hooks.restore;bool first_call=true;
    // A malformed outer native bundle can fail before session.restore runs.
    // Count the outer restore attempt so rollback is always exact, even then.
    hooks.restore=[&](const auto& bytes,auto& why){target_restore=std::exchange(first_call,false);if(!target_restore)reject_before_commit=false;return restore(bytes,why);};
    return ssne::runtime_checkpoint_restore(context,core,extra,hooks,error);
}
bool NativeSession::test_historical_checkpoint(PPCContext& context,uint64_t frame,std::string& error) {
    static std::vector<uint8_t> core,extra;static uint64_t saved_frame=0;static bool done=false;
    static std::map<EntityId,EntityState> saved_entities;
    const bool imported=env_requested("SSNE_TEST_HISTORICAL_IMPORT");
    auto read=[](const char* name){std::ifstream f(name,std::ios::binary);return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),{});};
    auto write=[](const char* name,const auto& bytes){std::ofstream file(name,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return bool(file);};
    if(done || frame<900)return true;
    const auto people=server_->participants();
    const auto expected=env_present("SSNE_TEST_HISTORICAL_PEERS")?unsigned(std::atoi(getenv("SSNE_TEST_HISTORICAL_PEERS"))):capacity_;
    if(std::count_if(people.begin(),people.end(),[](const auto& p){return p.connected && p.ready;})!=expected)return true;
    const auto min_generation=env_present("SSNE_TEST_HISTORICAL_GENERATION")?unsigned(std::atoi(getenv("SSNE_TEST_HISTORICAL_GENERATION"))):1u;
    if(expected && people[expected-1].id.generation<min_generation)return true;
    if(core.empty()){
        if(imported){
            core=read("historical-core.state");extra=read("historical-external.bin");
            WorldDelta delta;WorldState saved;
            if(core.empty() || extra.empty() || !decode_delta(read("historical-world.bin"),delta,error) || !delta.full || !apply_delta(saved,delta,error)){error="Cannot read imported historical checkpoint evidence";return false;}
            saved_frame=saved.tick;saved_entities=std::move(saved.entities);
            if(log_)log_("[coop-history-imported] saved_frame="+std::to_string(saved_frame)+" entities="+std::to_string(saved_entities.size()));
        }else{
        if(env_requested("SSNE_TEST_HISTORICAL_RESPAWN") && std::none_of(afk_respawn_wait_.begin(),afk_respawn_wait_.end(),[](const auto& wait){return wait.actor!=0;}))return true;
        if(env_requested("SSNE_TEST_HISTORICAL_CUTSCENE")){
            const auto vote=server_->skip_vote();
            if(!active_cutscene_object_ || mem_read32(active_cutscene_object_+0x50)!=1953 || !ssne::native_cutscene_presenting() || !vote.received || vote.received>=vote.required)return true;
        }
        if(const auto mode=getenv("SSNE_TEST_HISTORICAL_EFFECT");mode && *mode){
            const auto effects=ssne::native_effect_checkpoint_stats();
            const bool present=std::string(mode)=="rocket"?effects.rockets>0:std::string(mode)=="queued"?effects.queued>0:effects.loops>0;
            if(!present)return true;
        }
        NativeCheckpointParticipant participant(native_,session_checkpoint_hooks());
        if(!ssne::runtime_checkpoint_capture(context,core,extra,participant.hooks(),error))return false;
        saved_entities=native_.capture(frame).entities;
        saved_frame=frame;
        WorldState expected;expected.epoch=epoch_;expected.revision=1;expected.tick=frame;expected.level=level_;expected.entities=saved_entities;
        if(!write("historical-core.state",core) || !write("historical-external.bin",extra) || !write("historical-world.bin",encode_delta(difference(nullptr,expected)))){error="Cannot write historical checkpoint";return false;}
        if(log_)log_("[coop-history-saved] frame="+std::to_string(frame)+" epoch="+std::to_string(epoch_));return true;
        }
    }
    if(frame<saved_frame+600)return true;
    if(env_requested("SSNE_TEST_HISTORICAL_RESPAWN") && std::any_of(afk_respawn_wait_.begin(),afk_respawn_wait_.end(),[](const auto& wait){return wait.actor!=0;}))return true; // First life must finish respawning before replay.
    if(env_requested("SSNE_TEST_HISTORICAL_CUTSCENE") && ssne::native_cutscene_presenting())return true; // Rewind only after the original cinematic has ended.
    // Reload bytes from disk; require a real interval of native simulation.
    const auto disk_core=read("historical-core.state"),disk_extra=read("historical-external.bin");
    if(disk_core!=core || disk_extra!=extra){error="Historical checkpoint file differs";return false;}
    const auto before=native_.capture(frame);const auto old_epoch=epoch_;
    if(env_requested("SSNE_TEST_HISTORICAL_ROLLBACK")){
        const auto authority=server_->checkpoint_authority();std::vector<uint8_t> state;
        if(!save_session_checkpoint(state,error))return false;
        auto canonical=[](auto bytes){std::fill_n(bytes.begin()+76,4,0);bytes.resize(bytes.size()-8);return bytes;};
        auto malformed=disk_extra;malformed[0]^=1;std::string malformed_error;
        if(restore_historical_checkpoint(context,disk_core,malformed,malformed_error) || malformed_error!="Invalid native checkpoint bundle"){error="Malformed native bundle was not rejected after RAM restoration";return false;}
        std::vector<uint8_t> after_malformed;
        if(!save_session_checkpoint(after_malformed,error) || server_->checkpoint_authority()!=authority || canonical(after_malformed)!=canonical(state) || native_.capture(frame).entities!=before.entities){error="Malformed native bundle did not recover exact live authority and gameplay";return false;}
        if(log_)log_("[coop-history-invalid-bundle] exact=1");
        std::vector<uint8_t> effects;
        if(!ssne::native_effect_save_state(effects,error) || !write("historical-rollback-before-effects.bin",effects))return false;
        std::string why;
        if(restore_historical_checkpoint(context,disk_core,disk_extra,why,true) || why.find("Historical rollback probe")==std::string::npos){error="Historical failure probe did not reject before commit: "+why;return false;}
        std::vector<uint8_t> restored;if(!save_session_checkpoint(restored,error))return false;
        if(!ssne::native_effect_save_state(effects,error) || !write("historical-rollback-after-effects.bin",effects))return false;
        if(server_->checkpoint_authority()!=authority || canonical(state)!=canonical(restored) || native_.capture(frame).entities!=before.entities){error="Failed historical restore did not exactly recover live state";return false;}
        if(log_)log_("[coop-history-rollback] exact=1");
    }
    if(!restore_historical_checkpoint(context,disk_core,disk_extra,error))return false;
    std::vector<uint8_t> restored_effects;
    if(!ssne::native_effect_save_state(restored_effects,error) || !write("historical-restored-effects.bin",restored_effects))return false;
    const auto after=native_.capture(frame);
    unsigned changed=0,players=0;for(const auto& [id,e]:after.entities)if(e.kind==EntityKind::Player){++players;auto old=before.entities.find(id);changed+=old==before.entities.end() || old->second.properties!=e.properties;}
    if(!changed || after.epoch<=old_epoch){error="Historical test did not rewind changed native players";return false;}
    // Character gameplay is saved by party slot; connection ownership is live.
    auto expected_entities=saved_entities;
    for(auto& [id,e]:expected_entities)if(e.kind==EntityKind::Player){
        const auto slot=unsigned(std::get<int64_t>(e.properties.at("player_slot")));
        e.owner=people[slot].id;
    }
    if(after.entities!=expected_entities){error="Historical native entities differ from saved state";return false;}
    std::ofstream audit("historical-checkpoint.json");audit<<"{\"saved_frame\":"<<saved_frame<<",\"restored_frame\":"<<frame<<",\"old_epoch\":"<<old_epoch<<",\"new_epoch\":"<<after.epoch<<",\"players\":"<<players<<",\"changed_players\":"<<changed<<",\"imported\":"<<(imported?"true":"false")<<",\"restored\":true}\n";
    if(log_)log_("[coop-history-restored] frame="+std::to_string(frame)+" saved_frame="+std::to_string(saved_frame)+" epoch="+std::to_string(after.epoch)+" changed_players="+std::to_string(changed));
    if(!audit){error="Cannot write historical checkpoint audit";return false;}done=true;return true;
}
bool NativeSession::test_session_checkpoint(std::string& error) {
    std::vector<uint8_t> before;if(!save_session_checkpoint(before,error))return false;
    auto canonical=[](auto b){std::fill_n(b.begin()+76,4,0);b.resize(b.size()-8);return b;};
    auto sign=[](auto& b){const auto h=checkpoint::hash(b,b.size()-8);for(unsigned i=0;i<8;++i)b[b.size()-8+i]=uint8_t(h>>(56-i*8));};
    unsigned rejected=0;
    auto reject=[&](const auto& bytes){std::string why;std::vector<uint8_t> after;if(load_session_checkpoint(bytes,why) || why.empty() || !save_session_checkpoint(after,error) || canonical(after)!=canonical(before))return false;++rejected;return true;};
    auto short_file=before;short_file.pop_back();if(!reject(short_file)){error="Truncated session checkpoint changed state";return false;}
    for(unsigned at:{0u,4u,8u,12u,40u,44u,48u,60u,72u,76u,80u}){auto bad=before;std::fill_n(bad.begin()+at,4,255);sign(bad);if(!reject(bad)){error="Malformed session checkpoint changed state at "+std::to_string(at);return false;}}
    unsigned active=0,trail=0;for(unsigned i=0;i<max_players;++i){active+=afk_native_active_[i];trail+=unsigned(afk_bots_[i].checkpoint().trail.size());}
    const auto aim=native_afk_manual_aim_bindings().size();const auto vote=server_->skip_vote();
    auto authority=server_->checkpoint_authority();checkpoint::Reader authority_reader(authority,4096);
    authority_reader.at=8;const auto counter=authority_reader.u64();authority_reader.u64();const bool requested=authority_reader.flag(),decided=authority_reader.flag();
    if(decided)authority_reader.at+=36;
    unsigned cleared_votes=0;
    for(unsigned i=0;i<max_players;++i){authority_reader.u32();authority_reader.u64();for(unsigned n=0;n<3;++n)authority_reader.flag();authority_reader.u64();authority_reader.text(64);authority_reader.u64();authority_reader.u64();const auto at=authority_reader.at;const bool voted=authority_reader.flag();if(voted && !requested){std::fill_n(authority.begin()+at,4,0);++cleared_votes;}}
    if(counter==UINT64_MAX){error="Checkpoint test cutscene counter exhausted";return false;}
    for(unsigned i=0;i<8;++i)authority[8+i]=uint8_t((counter+1)>>(56-i*8));sign(authority);
    if(!server_->restore_checkpoint_authority(authority,error))return false;
    afk_decisions_[0]^=0x12345678u;for(auto& bot:afk_bots_)bot={};clear_native_afk_manual_aim();
    std::vector<uint8_t> changed;const bool differs=save_session_checkpoint(changed,error) && canonical(changed)!=canonical(before);
    if(!load_session_checkpoint(before,error))return false;
    std::vector<uint8_t> after;if(!differs || !save_session_checkpoint(after,error) || canonical(after)!=canonical(before)){error="Changed native session state failed to restore";return false;}
    std::ofstream file("native-session-checkpoint.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(before.data()),before.size());
    std::ofstream audit("native-session-checkpoint.json");audit<<"{\"rejected\":"<<rejected<<",\"active_bots\":"<<active<<",\"trail_points\":"<<trail<<",\"aim_bindings\":"<<aim<<",\"cutscene\":"<<vote.cutscene<<",\"votes\":"<<vote.received<<",\"required\":"<<vote.required<<",\"cleared_votes_restored\":"<<cleared_votes<<",\"authority_changed_restored\":true,\"positive_restore\":true}\n";
    if(!file || !audit){error="Cannot write session checkpoint evidence";return false;}return true;
}
