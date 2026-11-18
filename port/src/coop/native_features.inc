// Guest-thread integration: native gameplay owns actors; the authority owns
// Survival policy and spectator permissions. No presentation state travels as
// an address and no watcher can supply gameplay input.
namespace {
int64_t feature_integer(const Properties& fields,const char* key,int64_t fallback=0){
    const auto it=fields.find(key);if(it==fields.end())return fallback;const auto* value=std::get_if<int64_t>(&it->second);return value?*value:fallback;
}
std::string feature_time(uint64_t milliseconds){
    const auto seconds=milliseconds/1000;return std::to_string(seconds/60)+":"+(seconds%60<10?"0":"")+std::to_string(seconds%60);
}
bool feature_menu(){return ssne::local_pause_menu_open() || ssne::local_information_open() || native_frontend_visible() || ssne::native_results_presenting() || ssne::native_cutscene_presenting() || ssne::native_movie_presenting();}
}
void NativeSession::configure_survival_start(){setup_["Session.Survival.Active"]=int64_t(1);setup_["Session.CustomMode"]=int64_t(5);}
bool NativeSession::configure_survival_session(const Properties& fields){
    const bool enabled=feature_integer(fields,"Session.Survival.Active")==1;
    const auto weapon_set=feature_integer(fields,"Session.Survival.WeaponSet"),powerups=feature_integer(fields,"Session.Survival.AllowPowerUps",1);
    if(enabled && (weapon_set<0 || weapon_set>10 || (powerups!=0 && powerups!=1))){report("Invalid Survival native rule selection");return false;}
    std::string error;if(!configure_native_survival_rules(enabled,unsigned(weapon_set),powerups==1,error)){report(error);return false;}
    if(survival_enabled_.exchange(enabled)!=enabled){
        survival_clock_ms_=0;survival_clock_tick_=survival_native_epoch_=0;survival_monsters_.clear();survival_kills_=0;survival_spawn_counter_=0;survival_.reset();
    }
    ssne::configure_native_survival(enabled);
    if(enabled){setup_["Session.Survival.WeaponSet"]=weapon_set;setup_["Session.Survival.AllowPowerUps"]=powerups;}
    if(server_ && enabled){survival_.set_enemy_multiplier(native_frontend_options().enemies);ssne::set_enemy_multiplier(1);} // Quotas already apply the shared enemy factor.
    return true;
}
void NativeSession::advance_survival_clock(PPCContext& ctx,uint64_t tick){
    if(!server_ || !survival_enabled_ || survival_clock_tick_==tick)return;survival_clock_tick_=tick;
    if(!queued_ || !host_ready_ || level_loading_ || !ssne::native_survival_resources_ready() || mem_read32(ssne::game_address(0x802DB0E0))!=4 || ssne::native_movie_presenting() || ssne::native_cutscene_presenting() || ssne::native_results_presenting())return;
    const float seconds=u2f(mem_read32(ctx.r[13]-0x7A08u));
    if(std::isfinite(seconds) && seconds>0.f && seconds<=10.f)survival_clock_ms_+=double(seconds)*1000.;
}
bool NativeSession::update_survival_round(PPCContext& ctx,const NativeWorldSample& sample,uint64_t frame){
    if(!server_ || !survival_enabled_ || !host_ready_ || level_loading_ || sample.scene!=4 || sample.mode!=1 || !sample.error.empty())return false;
    if(!ssne::native_survival_resources_ready()){const auto why=ssne::native_survival_resource_error();if(!why.empty())report("Survival resource loading: "+why);return false;}
    if(survival_native_epoch_!=sample.epoch){
        survival_native_epoch_=sample.epoch;survival_clock_ms_=0;survival_kills_=0;survival_spawn_counter_=0;survival_monsters_.clear();
        SurvivalConfig config;config.enemy_multiplier=native_frontend_options().enemies;
        for(unsigned char c:level_)config.seed=(config.seed^c)*16777619u;
        // Shortened timings exist only in an explicitly hidden owned test.
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_TEST_SURVIVAL_SHORT")){config.countdown_ms=1200;config.wave_ms=6000;config.restart_ms=3000;config.spawn_ms=500;config.alive_cap=12;}
        survival_.reset(config);server_->set_spectator_lock(0);
        if(log_)log_("[survival-round-ready] level="+level_+" epoch="+std::to_string(sample.epoch)+" resources="+std::to_string(ssne::native_survival_resources_ready()));
    }
    const auto people=server_->participants();SurvivalObservation observation;observation.now_ms=uint64_t(survival_clock_ms_);observation.voluntary_spectators=server_->voluntary_spectator_mask();
    std::vector<ssne::NativeSurvivalPoint> living,occupied;
    for(unsigned slot=0;slot<capacity_;++slot){const auto& person=people[slot];if(!person.connected)continue;
        observation.connected|=uint8_t(1u<<slot);observation.generations[slot]=person.id.generation;if(person.ready)observation.ready|=uint8_t(1u<<slot);
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(ssne::native_player_collision_radius(actor) && u2f(mem_read32(actor+0xCC))>0.f && !mem_read32(actor+0x328) && !ssne::native_party_spawn_waiting(actor)){
            observation.alive|=uint8_t(1u<<slot);if(!person.spectator)living.push_back({u2f(mem_read32(actor+0x20)),u2f(mem_read32(actor+0x24)),u2f(mem_read32(actor+0x28))});
        }
    }
    unsigned new_kills=0;
    for(auto it=survival_monsters_.begin();it!=survival_monsters_.end();){
        auto found=sample.entities.find(it->first);
        if(found==sample.entities.end()){if(survival_kills_<UINT32_MAX)++survival_kills_;++new_kills;it=survival_monsters_.erase(it);continue;}
        const auto hp=found->second.properties.find("health");const auto* health=hp==found->second.properties.end()?nullptr:std::get_if<float>(&hp->second);
        if(found->second.kind!=EntityKind::Enemy || (health && *health<=0.f)){if(survival_kills_<UINT32_MAX)++survival_kills_;++new_kills;it=survival_monsters_.erase(it);}else ++it;
    }
    for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Enemy){
        const auto hp=e.properties.find("health");const auto* health=hp==e.properties.end()?nullptr:std::get_if<float>(&hp->second);
        if(health && *health>0.f){++observation.live_enemies;occupied.push_back(std::get<std::array<float,3>>(e.properties.at("position")));}
    }
    observation.confirmed_kills=new_kills;survival_.set_enemy_multiplier(native_frontend_options().enemies);
    const auto before=survival_.state();const auto step=survival_.advance(observation);
    uint8_t locked=0;if(step.state.phase==SurvivalPhase::Running || step.state.phase==SurvivalPhase::Defeat)locked=uint8_t(observation.connected&~step.state.playing_mask);
    server_->set_spectator_lock(locked);
    bool changed=false;
    if(step.restart_due){
        // A defeated player can own a local pause/quit-confirmation GUI.
        // Keep its spectator lock and actor intact until that GUI is
        // closed; respawning here would invalidate its pending actions.
        if(feature_menu() || native_frontend_pending_actions())return changed;
        ssne::gx_sync();
        for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Enemy || e.kind==EntityKind::Projectile){std::string why;if(!remove_native_entity(ctx,native_.local_address(id),why)){report("Survival round cleanup: "+why);return changed;}changed=true;}
        survival_monsters_.clear();survival_kills_=0;server_->set_spectator_lock(0);survival_.restart(observation.now_ms);
        if(log_)log_("[survival-round-restart] round="+std::to_string(survival_.state().round));
    }else for(const auto& request:step.spawns){
        bool success=false;std::string why;ssne::NativeSurvivalPoint position{};
        if(!living.empty() && ssne::choose_native_survival_spawn(ctx,request.definition,living,occupied,++survival_spawn_counter_,position,why)){
            EntityState seed;seed.id=(uint64_t(1)<<62)|survival_spawn_counter_;seed.kind=EntityKind::Enemy;seed.definition=request.definition;seed.properties={{"position",position},{"spawn_position",position}};
            if(const auto actor=create_native_enemy(ctx,seed,why)){
                success=true;changed=true;occupied.push_back(position);const auto id=native_.local_entity_id(actor);if(id)survival_monsters_[id]=true;
                if(log_)log_("[survival-spawn] round="+std::to_string(step.state.round)+" wave="+std::to_string(step.state.wave)+" type="+std::to_string(request.definition)+" entity="+std::to_string(id)+" actor="+std::to_string(actor));
            }
        }
        survival_.finish_spawn(request.ticket,success);
        if(!success && log_ && survival_spawn_counter_%8==1)log_("[survival-spawn-wait] "+why);
    }
    if(log_ && (before.phase!=survival_.state().phase || before.wave!=survival_.state().wave || before.eliminated_mask!=survival_.state().eliminated_mask))log_("[survival-state] round="+std::to_string(survival_.state().round)+" phase="+std::to_string(unsigned(survival_.state().phase))+" wave="+std::to_string(survival_.state().wave)+" playing="+std::to_string(survival_.state().playing_mask)+" locked="+std::to_string(locked)+" ms="+std::to_string(survival_.state().survival_ms));
    return changed;
}
void NativeSession::publish_survival(Properties& mission)const{
    if(!survival_enabled_)return;const auto& s=survival_.state();
    mission["Session.Survival.Active"]=int64_t(1);mission["Session.CustomMode"]=int64_t(5);
    for(auto [key,value]:std::initializer_list<std::pair<const char*,uint64_t>>{{"Phase",unsigned(s.phase)},{"Round",s.round},{"Wave",s.wave},{"TimeMS",s.survival_ms},{"BestMS",s.best_ms},{"RemainingMS",s.phase_remaining_ms},{"WaveRemainingMS",s.wave_remaining_ms},{"Spawned",s.total_spawned},{"Kills",s.kills},{"Enemies",s.active_enemies},{"PlayingMask",s.playing_mask},{"WaitingMask",s.waiting_mask},{"EliminatedMask",s.eliminated_mask}})mission[std::string("Session.Survival.")+key]=int64_t(value);
    mission["Session.EnemyMultiplier"]=int64_t(survival_.config().enemy_multiplier);
}
ssne::PadState NativeSession::spectator_input(PPCContext& ctx,uint64_t epoch,PlayerId own,const std::array<Participant,max_players>& people,const ssne::PadState& input){
    std::array<SpectatorTarget,max_players> candidates{};
    for(unsigned slot=0;slot<max_players;++slot){const auto& p=people[slot];const auto actor=slot<ssne::native_party_capacity()?mem_read32(ssne::native_party_registry()+slot*4):0;
        const float health=ssne::native_player_collision_radius(actor) && !mem_read32(actor+0x328)?u2f(mem_read32(actor+0xCC)):0.f;
        candidates[slot]={p.id,p.connected,p.ready,p.spectator,health};
    }
    local_watching_=own.slot<max_players && people[own.slot].connected && people[own.slot].id==own && people[own.slot].spectator;
    bool consume=false;std::string menu_error;
    if(!spectator_menu_.update(ctx,epoch,own.slot,input,local_watching_,consume,menu_error) && log_ && !menu_error.empty())log_("[spectator-menu-wait] "+menu_error);
    auto selected_input=input;if(consume)selected_input.buttons&=~ssne::PAD_START;
    const auto decision=spectator_.update(epoch,own,candidates,local_watching_,selected_input,feature_menu());spectator_target_=decision.target;
    if(log_ && spectator_reported_!=decision.target){spectator_reported_=decision.target;log_("[spectator-target] local="+std::to_string(own.slot)+" watching="+std::to_string(local_watching_)+" target="+std::to_string(decision.target?decision.target->slot:max_players));}
    return decision.menu;
}
void NativeSession::begin_spectator_view(PPCContext& ctx){
    if(!local_watching_ || feature_menu() || level_loading_ || mem_read32(ssne::game_address(0x802DB0E0))!=4)return;
    const auto own=server_?server_->status().player:client_->status().player;
    std::string why;if(spectator_view_.begin(ctx,own.slot,spectator_target_?spectator_target_->slot:ssne::no_spectator_target,why)){++spectator_render_frames_;}
    else if(log_ && !why.empty()){
        static std::string previous;if(previous!=why){previous=why;log_("[spectator-view-wait] "+why);}
    }
}
void NativeSession::update_feature_hud(PPCContext& ctx){
    if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_DEV_NATIVE_MENU") && std::chrono::steady_clock::now()>=feature_evidence_next_){
        feature_evidence_next_=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        const auto state=server_?server_->latest_world():client_->latest_world();const auto own=server_?server_->status().player:client_->status().player;
        if(state){
            std::ofstream out("multiplayer-feature-state.json.pending",std::ios::binary|std::ios::trunc);
            out<<"{\"frame\":"<<state->tick<<",\"scene\":"<<mem_read32(ssne::game_address(0x802DB0E0))<<",\"level\":\""<<state->level<<"\",\"native_mode\":"<<mem_read32(ctx.r[13]-0x69d4)<<",\"runtime_ready\":"<<(server_?host_ready_:client_ready_)<<",\"feature_menu\":"<<feature_menu()<<",\"spectator_menu\":"<<ssne::native_spectator_menu_open()<<",\"survival_enabled\":"<<survival_enabled_<<",\"phase\":"<<feature_integer(state->mission,"Session.Survival.Phase")<<",\"wave\":"<<feature_integer(state->mission,"Session.Survival.Wave")<<",\"elapsed_ms\":"<<feature_integer(state->mission,"Session.Survival.TimeMS")<<",\"remaining_ms\":"<<feature_integer(state->mission,"Session.Survival.RemainingMS")<<",\"wave_remaining_ms\":"<<feature_integer(state->mission,"Session.Survival.WaveRemainingMS")<<",\"alive_enemies\":"<<feature_integer(state->mission,"Session.Survival.Enemies")<<",\"spawned\":"<<feature_integer(state->mission,"Session.Survival.Spawned")<<",\"kills\":"<<feature_integer(state->mission,"Session.Survival.Kills")<<",\"round_id\":"<<feature_integer(state->mission,"Session.Survival.Round")<<",\"playing_mask\":"<<feature_integer(state->mission,"Session.Survival.PlayingMask")<<",\"lock_mask\":"<<feature_integer(state->mission,"Session.Spectators.LockMask")<<",\"local_spectator\":"<<(own.slot<8 && state->players[own.slot].spectator)<<",\"target_slot\":"<<(spectator_target_?spectator_target_->slot:8)<<",\"spectator_render_frames\":"<<spectator_render_frames_<<",\"resources_ready\":"<<ssne::native_survival_resources_ready()<<",\"enemies\":[";
            bool comma=false;for(const auto& [id,entity]:state->entities)if(entity.kind==EntityKind::Enemy){
                if(comma)out<<',';comma=true;const auto actor=server_?native_.local_address(id):replica_.local_address(id);
                const bool bound=actor>=0x80000000u && actor<=ssne::guest_ram_end()-0xD0;const float health=bound?u2f(mem_read32(actor+0xCC)):0.f;
                out<<"{\"id\":"<<id<<",\"definition\":"<<entity.definition<<",\"bound\":"<<bound<<",\"actual_health\":";
                if(bound && std::isfinite(health))out<<health;else out<<"null";
                const auto found=entity.properties.find("health");const auto* expected=found==entity.properties.end()?nullptr:std::get_if<float>(&found->second);out<<",\"snapshot_health\":";
                if(expected && std::isfinite(*expected))out<<*expected;else out<<"null";out<<"}";
            }out<<"]}";
            out.close();if(out){std::remove("multiplayer-feature-state.json");std::rename("multiplayer-feature-state.json.pending","multiplayer-feature-state.json");}
        }
    }
    if(!queued_ || level_loading_ || mem_read32(ssne::game_address(0x802DB0E0))!=4 || feature_menu()){ssne::clear_native_session_hud(ctx);return;}
    const auto state=server_?server_->latest_world():client_->latest_world();if(!state)return;
    const auto own=server_?server_->status().player:client_->status().player;const unsigned language=native_menu_language();auto tr=[&](FrontText id){return std::string(frontend_text(id,language));};
    std::string title,detail;
    if(feature_integer(state->mission,"Session.Survival.Active")==1){
        const auto phase=feature_integer(state->mission,"Session.Survival.Phase");
        title=tr(FrontText::Survival)+" | "+tr(FrontText::Wave)+" "+std::to_string(feature_integer(state->mission,"Session.Survival.Wave"))+" | "+tr(FrontText::Time)+" "+feature_time(uint64_t(std::max<int64_t>(0,feature_integer(state->mission,"Session.Survival.TimeMS"))));
        if(!ssne::native_survival_resources_ready())detail=tr(ssne::native_survival_resource_error().empty()?FrontText::SurvivalLoading:FrontText::SurvivalLoadFailed);
        else if(phase==unsigned(SurvivalPhase::Waiting))detail=tr(FrontText::Waiting);
        else if(phase==unsigned(SurvivalPhase::Countdown))detail=tr(FrontText::Countdown)+" "+std::to_string((feature_integer(state->mission,"Session.Survival.RemainingMS")+999)/1000);
        else if(phase==unsigned(SurvivalPhase::Defeat))detail=tr(FrontText::RoundOver)+" | "+tr(FrontText::NextRound)+": "+std::to_string((feature_integer(state->mission,"Session.Survival.RemainingMS")+999)/1000);
        else detail=tr(FrontText::Enemies)+": "+std::to_string(feature_integer(state->mission,"Session.Survival.Enemies"))+" | Kills: "+std::to_string(feature_integer(state->mission,"Session.Survival.Kills"));
    }
    if(own.slot<max_players && state->players[own.slot].id==own && state->players[own.slot].spectator){
        const auto target=spectator_target_?spectator_target_->slot:max_players;const auto name=target<max_players?native_name_input(state->players[target].name):tr(FrontText::Waiting);
        if(title.empty()){title=tr(FrontText::Watching)+": "+name;detail=tr(FrontText::WatchHelp);}
        else if(feature_integer(state->mission,"Session.Survival.Phase")!=unsigned(SurvivalPhase::Defeat))detail=tr(FrontText::Watching)+": "+name+" | "+tr(FrontText::WatchHelp);
    }
    ssne::update_native_session_hud(ctx,title.substr(0,100),detail.substr(0,120),!title.empty());
}
