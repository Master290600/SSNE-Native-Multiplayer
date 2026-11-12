// Test driver only. Compiled into a separate executable, never the public game.
void sync_scenario_tick(PPCContext& context,uint64_t tick){
    static std::chrono::steady_clock::time_point began{};
    static unsigned killed_wave=0;static bool pickup=false;
    const bool gameplay=mem_read32(ssne::game_address(0x802DB0E0))==4;
    bool ready=false;
    if(gameplay && g_coop_host){const auto players=g_coop_server->participants();ready=players[0].ready && (players[1].ready || getenv("SSNE_SYNC_LATE_JOIN"));}
    if(gameplay && g_coop_client)ready=g_coop_peer->latest_world()->players[g_coop_local_player.load()].ready;
    if(ready && !began.time_since_epoch().count())began=std::chrono::steady_clock::now();
    const auto seconds=began.time_since_epoch().count()?std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count():0.;
    ssne::PadState pad;
    if(ready && seconds>1 && seconds<12){pad.buttons=ssne::PAD_R;pad.trigger_r=255;}
    if(ready && seconds>22 && seconds<27)pad.stick_y=255;
    feed_local_pad(pad);
    if(!g_coop_host || !ready)return;
    if(seconds>5 && !pickup){const unsigned slot=getenv("SSNE_SYNC_LATE_JOIN")?0:1;ssne::mod_test_pickup_at_player(35,slot);pickup=true;log_line("[sync-scenario-pickup] chainsaw at slot="+std::to_string(slot));}
    if(seconds>8+4*killed_wave && killed_wave<5){
        auto sample=g_coop_engine->world().capture(tick);unsigned killed=0;
        std::vector<uint8_t> stack(0x2000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x2000+i);
        for(const auto& [id,e]:sample.entities)if(e.kind==port::coop::EntityKind::Enemy && e.properties.contains("health") && std::get<float>(e.properties.at("health"))>0){
            const auto actor=g_coop_engine->world().local_address(id);if(!actor)continue;
            auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=call.r[5]=0;ssne_call(call,0x8009B370u);++killed;
        }
        for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);
        ++killed_wave;log_line("[sync-scenario-native-kills] wave="+std::to_string(killed_wave)+" count="+std::to_string(killed));
    }
    if(tick%60==0){auto sample=g_coop_engine->world().capture(tick);std::string markers;
        for(const auto& [id,e]:sample.entities)if(e.kind==port::coop::EntityKind::Physics)markers+=std::to_string(std::get<int64_t>(e.properties.at("marker_index")))+":"+std::to_string(std::get<int64_t>(e.properties.at("marker_active")))+",";
        log_line("[sync-scenario-markers] seconds="+std::to_string(seconds)+" states="+markers);
    }
}
