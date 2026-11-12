// Isolated regression driver: native pickups, two firing bursts, then idle.
void sync_scenario_tick(PPCContext& context,uint64_t tick){
    static std::chrono::steady_clock::time_point began{};static bool pickup=false,checkpoint=false;
    const bool gameplay=mem_read32(ssne::game_address(0x802DB0E0))==4;
    bool ready=false;
    if(gameplay && g_coop_host){auto p=g_coop_server->participants();ready=p[0].ready && p[1].ready;}
    if(gameplay && g_coop_client)ready=g_coop_peer->latest_world()->players[g_coop_local_player.load()].ready;
    if(ready && !began.time_since_epoch().count())began=std::chrono::steady_clock::now();
    const auto seconds=began.time_since_epoch().count()?std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count():0.;
    ssne::PadState pad;
    if(ready && ((seconds>10 && seconds<14) || (seconds>18 && seconds<22))){pad.buttons=ssne::PAD_R;pad.trigger_r=255;}
    if(ready && seconds>2 && seconds<4){pad.buttons=ssne::PAD_R;pad.trigger_r=255;}
    test_held_buttons.store(pad.buttons);
    feed_local_pad(pad);
    if(g_coop_host && ready && seconds>7 && !pickup){ssne::mod_test_pickup_at_player(39,1);pickup=true;}
    if(g_coop_host && ready && seconds>10 && seconds<22 && !checkpoint && ssne::native_weapon_cue_state().size()>1){
        std::string why;if(!ssne::native_effect_checkpoint_probe(why))ssne::fatal(("Cue checkpoint probe failed: "+why).c_str());
        checkpoint=true;log_line("[idle-checkpoint] active_cues_restored=1");
    }
    if(ready && tick%60==0){
        std::string voices;
        for(unsigned i=0;i<48;++i){auto record=0x802D69C8u+i*192;auto handle=mem_read32(record+0xAC),sound=mem_read32(record+0xB0);if(handle)voices+=std::to_string(i)+":"+std::to_string(sound)+":"+std::to_string(handle)+",";}
        auto actor=mem_read32(ssne::native_party_registry()+4);std::string models;
        if(actor)for(unsigned w=0;w<12;++w)for(unsigned h=0;h<2;++h){auto inst=mem_read32(actor+0x4BC+w*24+h*4);if(inst>=0x80000000u && inst<ssne::guest_ram_end()-0x228){auto proxy=mem_read32(inst);if(proxy>=0x80000000u && proxy<ssne::guest_ram_end()-0x34)models+=std::to_string(w)+":"+std::to_string(h)+":"+std::to_string(mem_read32(proxy+0x24))+":"+std::to_string(mem_read32(proxy+0x30))+",";}}
        if(actor){auto w=mem_read32(actor+0x5DC),inst=mem_read32(actor+0x4BC+w*24),proxy=mem_read32(inst),anim=mem_read32(inst+0x28),draw=mem_read32(proxy+0x20);log_line("[idle-model-binding] weapon="+std::to_string(w)+" anim="+std::to_string(anim)+" draw="+std::to_string(draw)+" bones="+std::to_string(mem_read32(anim+0x7C))+" geometry="+std::to_string(mem_read32(anim+0x14))+" parent="+std::to_string(mem_read32(anim+0x94))+" draw_vt="+std::to_string(mem_read32(draw)));}
        log_line("[idle-probe] seconds="+std::to_string(seconds)+" voices="+voices+" models="+models);
    }
}
