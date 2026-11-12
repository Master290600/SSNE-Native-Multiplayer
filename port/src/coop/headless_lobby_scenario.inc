// Separate test executable only; exercises the public input adapter and native
// frontend loop without creating an SDL window or using the GPU.
void sync_scenario_tick(PPCContext& context,uint64_t tick){
    using namespace port::coop;
    static uint64_t opened=0,selector=0;static bool requested=false,passed=false,back=false;static unsigned returns=0;
    const auto state=g_coop_host?g_coop_server->latest_world():g_coop_peer->latest_world();
    const auto status=g_coop_host?g_coop_server->status():g_coop_peer->status();
    ssne::PadState pad;
    if(g_coop_host && back && native_lobby_visible()){
        ++returns;opened=tick;selector=0;requested=false;back=false;
        log_line("[lobby-scenario-return] count="+std::to_string(returns));
    }
    if(native_lobby_visible() && !requested){
        if(!opened){opened=tick;log_line("[lobby-scenario] original roster visible");}
        const auto elapsed=tick-opened;
        if(elapsed>=20 && elapsed<24)pad.stick_x=255;
        if(returns && elapsed>=40 && elapsed<44)pad.stick_x=0;
        const auto models=state->mission.find("Session.CharacterModels");
        if(g_coop_host && elapsed>60 && models!=state->mission.end()){
            const auto& values=std::get<std::vector<int64_t>>(models->second);
            if(values.size()==8 && values[0]==1 && values[1]==2 && status.joined==2){
                pad.buttons=ssne::PAD_A;requested=true;
                log_line("[lobby-scenario] both owner choices received; Enter opens selector");
            }
        }
    }
    if(g_coop_host && requested && !native_lobby_visible() && mem_read32(ssne::game_address(0x802DB0E0))==3){
        if(!selector)selector=tick;
        const auto elapsed=tick-selector;
        // Native keyboard arrows use movement axes. Navigate forward/back
        // through the real GUI input loop, then confirm after releasing Enter.
        if(elapsed>=20 && elapsed<24)pad.stick_x=255;
        if(elapsed>=50 && elapsed<54)pad.stick_x=0;
        const unsigned action_at=returns>=3?90:returns==1?8:40;
        if(elapsed>=action_at && elapsed<action_at+4){pad.buttons=returns<3?ssne::PAD_B:ssne::PAD_A;if(returns<3)back=true;}
        if(back && elapsed>200)throw std::runtime_error("Back did not restore original character lobby");
    }
    feed_local_pad(pad);
    if(!passed && mem_read32(ssne::game_address(0x802DB0E0))==4 && status.player.slot<8 && state->players[status.player.slot].ready){
        Properties values;publish_native_lobby_characters(values);
        const auto models=values.find("Session.CharacterModels");
        if(models==values.end() || std::get<std::vector<int64_t>>(models->second)[0]!=1 || std::get<std::vector<int64_t>>(models->second)[1]!=2)throw std::runtime_error("Lobby models lost at native level start");
        if(g_coop_host && returns!=3)throw std::runtime_error("Missing repeated Back cycles");
        passed=true;log_line("[lobby-scenario-PASS] original roster, owner choice, selector keyboard, native level and readiness; models=1,2; host return cycles=3");
    }
}
