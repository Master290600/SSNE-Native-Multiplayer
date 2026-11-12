void sync_scenario_tick(PPCContext& context,uint64_t tick){
    static std::chrono::steady_clock::time_point began{};static bool requested=false;
    bool ready=false;
    if(mem_read32(ssne::game_address(0x802DB0E0))==4){if(g_coop_host){auto p=g_coop_server->participants();ready=p[0].ready&&p[1].ready;}else if(g_coop_client)ready=g_coop_peer->latest_world()->players[g_coop_local_player.load()].ready;}
    if(ready&&!began.time_since_epoch().count())began=std::chrono::steady_clock::now();
    const auto seconds=began.time_since_epoch().count()?std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count():0.;
    ssne::PadState pad;const bool skip=getenv("SSNE_TEST_MOVIE_SKIP");
    if(g_coop_host&&skip&&seconds>12&&seconds<13)pad.buttons=ssne::PAD_A;
    feed_local_pad(pad);
    if(g_coop_host&&ready&&seconds>3&&!requested){auto call=context;call.r[1]-=0x400;const std::string asset="0_1_FMV";for(unsigned i=0;i<=asset.size();++i)mem_write8(call.r[1]+0x80+i,i<asset.size()?asset[i]:0);call.r[3]=call.r[1]+0x80;ssne_call(call,0x80167714u);requested=true;log_line("[movie-test] queued=1");}
    if(ready&&tick%30==0){const auto m=ssne::native_movie_state();log_line("[movie-test] seconds="+std::to_string(seconds)+" active="+std::to_string(m.active)+" sequence="+std::to_string(m.sequence)+" frame="+std::to_string(m.frame)+" mask="+std::to_string(mem_read32(0x802ABAD0)));}
}
