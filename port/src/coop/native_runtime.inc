#include "native_info_mouse.inc"
// Session ownership changes only at the guest frame boundary. UI requests take
// the same mutex; renderer/tick observers themselves are never replaced live.
bool native_connecting=false,native_card_lease=false,native_session_owned=false,native_lan_join=false;
std::string native_data_directory,native_connection_error,native_offline_states;
port::coop::Properties native_offline_setup,native_client_profiles;
ssne::ModOptions native_offline_mods;
ssne::AmmoRules native_offline_ammo;unsigned native_offline_enemies=1;
PPCContext* native_safe_context=nullptr;
uint64_t native_current_frame=0,native_frame_origin=0,native_tick_number=0;
std::chrono::steady_clock::time_point native_connect_started;
port::coop::LanAdvertisement native_advertisement;
port::coop::LanBrowser native_browser;
uint16_t native_discovery_port(){
    if(getenv("SSNE_TEST_HIDDEN"))if(const auto value=getenv("SSNE_NATIVE_MENU_DISCOVERY_PORT")){char* end=nullptr;const auto port=std::strtoul(value,&end,10);if(end && !*end && port>0 && port<=65535)return uint16_t(port);}
    return port::coop::lan_discovery_port;
}
void advertise_native_session(){
    if(!g_coop_host || !g_coop_server)return;auto* server=g_coop_server.get();
    native_advertisement.start(port::netplay::content_hash(native_data_directory+"/sys/main.dol"),[server](){
        const auto status=server->status();port::coop::FrontendRoom room;if(!status.connected)return room;
        room.port=status.port;room.players=status.joined;room.capacity=status.capacity;room.name=server->participants()[0].name;room.session_id=server->lan_session_id();
        const auto world=server->latest_world();if(auto i=world->mission.find("Session.GameMode");i!=world->mission.end())if(auto mode=std::get_if<int64_t>(&i->second);mode && *mode>=2 && *mode<=4)room.capacity=4;
        return room;
    },native_discovery_port(),getenv("SSNE_TEST_HIDDEN")!=nullptr,[server](const std::string& peer,uint32_t nonce){return server->issue_lan_join_ticket(peer,nonce);});
}
void snapshot_native_setup(){
    const auto root=port::coop::native_global_configuration();
    native_offline_setup=port::coop::read_native_configuration(root,"GameSetup").values;
    native_offline_mods=ssne::mod_current_options();native_offline_ammo=ssne::ammo_rules();native_offline_enemies=ssne::enemy_multiplier();
}
void activate_native_session(){
    const auto status=g_coop_host?g_coop_server->status():g_coop_peer->status();
    g_coop_local_player=status.player.slot;g_coop_capacity=status.capacity;
    ssne::set_cheat_host(g_coop_host);
    ssne::netplay_set_roster(status.capacity);ssne::netplay_set_local_player(int(status.player.slot));ssne::mod_netplay_player(int(status.player.slot));
    if(g_coop_host)g_coop_engine=std::make_unique<port::coop::NativeSession>(*g_coop_server,status.capacity,"lobby",log_line);
    else g_coop_engine=std::make_unique<port::coop::NativeSession>(*g_coop_peer,log_line);
    if(g_coop_host)g_coop_engine->configure_saves(g_coop_save_identity);
    port::coop::install_native_lobby(g_coop_server.get(),g_coop_peer.get());
    native_frame_origin=native_current_frame;native_tick_number=0;
    g_states_dir=native_offline_states+"coop/";
    ssne::set_authoritative_gameplay(true);ssne::gpu_set_nonblocking_frames(true);ssne::mouse_aim_set_multiplayer(true);ssne::gpu_renderer_set_player_views(true);
    for(unsigned slot=0;slot<4;++slot)ssne::set_pad_connected(slot,slot<status.capacity);
    g_coop_engine_ready=true;native_session_owned=true;native_connecting=false;
    if(g_coop_host)advertise_native_session();
    if(const auto path=getenv("SSNE_PERF_LOG"))g_performance.open(path,g_coop_host?"host":"client");
    log_line("[native-session-start] role="+std::string(g_coop_host?"host":"client")+" slot="+std::to_string(status.player.slot)+" port="+std::to_string(status.port));
}
void stop_native_session(PPCContext& c){
    std::lock_guard lock(g_session_mutex);
    const bool running=g_coop_host || g_coop_client;
    const bool in_menu=mem_read32(ssne::game_address(0x802DB0E0))==3 && mem_read32(ssne::game_address(0x802DB0E4))!=4;
    native_advertisement.stop();native_browser.stop();
    g_coop_engine_ready=false;g_coop_host=false;g_coop_client=false;native_connecting=false;native_lan_join=false;
    ssne::set_cheat_host(false);
    port::coop::deactivate_native_lobby();
    if(g_coop_peer)g_coop_peer->stop();if(g_coop_server)g_coop_server->stop();
    g_coop_engine.reset();g_coop_peer.reset();g_coop_server.reset();
    std::string why;
    ssne::set_replica_simulation(false);ssne::set_native_results_save_host(true);
    port::coop::native_frontend_prepare_results_close(c);
    ssne::clear_native_replica_results(c,why);ssne::clear_native_cutscene_presentation();
    ssne::stop_native_weapon_loops(c,why);ssne::stop_native_replica_speech(c,why);port::coop::stop_native_vehicle_audio(c,why);
    ssne::set_ammo_rules(native_offline_ammo.capacity,native_offline_ammo.pickup_multiplier);ssne::set_enemy_multiplier(native_offline_enemies);
    ssne::mod_options(native_offline_mods);ssne::mod_shared_options(0,3.f);ssne::mod_netplay_player(-1);
    const auto root=port::coop::native_global_configuration();
    if(!native_offline_setup.empty() && !port::coop::write_native_configuration(c,root,native_offline_setup,"GameSetup",why))log_line("[native-session-restore] "+why);
    if(!native_client_profiles.empty() && !port::coop::write_native_configuration(c,root,native_client_profiles,"PlayerProfiles",why))log_line("[native-session-profile-restore] "+why);
    native_client_profiles.clear();native_offline_setup.clear();
    if(native_card_lease){ssne::exi_end_client_session();native_card_lease=false;}
    ssne::native_party_leave_network();ssne::netplay_set_local_player(0);g_coop_local_player=0;
    ssne::set_authoritative_gameplay(false);ssne::gpu_set_nonblocking_frames(false);ssne::mouse_aim_set_multiplayer(false);ssne::gpu_renderer_set_player_views(false);
    for(unsigned slot=0;slot<8;++slot){ssne::set_pad(slot,{});ssne::native_party_set_input(slot,{});if(slot<4)ssne::set_pad_connected(slot,slot==0 || g_set.pad_player[slot]>0);}
    g_states_dir=native_offline_states;native_session_owned=false;
    if(running)port::coop::native_frontend_return_main();
    if(running && !in_menu && !port::coop::queue_native_offline_menu(c,why))log_line("[native-session-menu-error] "+why);
    log_line("[native-session-stop] returned_to_offline=1");
}
std::string start_native_session_impl(bool hosting,const port::coop::FrontendEndpoint& endpoint,const port::coop::FrontendRoom* room){
    std::lock_guard lock(g_session_mutex);
    if(g_coop_host || g_coop_client || native_connecting)return "already active";
    try{
        snapshot_native_setup();native_lan_join=room!=nullptr;g_lan_pin=room?std::string{}:endpoint.pin;g_coop_capacity=8;g_coop_level="lobby";
        const auto fingerprint=port::netplay::content_hash(native_data_directory+"/sys/main.dol");
        std::string name="Sam";
        const auto profile=port::coop::read_native_configuration(port::coop::native_global_configuration(),"PlayerProfiles.Profile0.Name");
        if(auto it=profile.values.find("PlayerProfiles.Profile0.Name");it!=profile.values.end())if(auto text=std::get_if<std::string>(&it->second);text && !text->empty())name=*text;
        const auto preferences=port::coop::native_frontend_options();if(!preferences.name.empty())name=port::coop::native_name_utf8(preferences.name);
        if(hosting){
            g_coop_save_identity=port::coop::savefile::identify(std::filesystem::u8path(native_data_directory));
            g_coop_save_identity.game=0x47334250;g_coop_save_identity.ram=0x04000000;g_coop_save_identity.capacity=8;
            g_coop_server=std::make_unique<port::coop::AuthoritativeServer>(8);
            if(!g_coop_server->start(uint16_t(endpoint.port),endpoint.pin,fingerprint,name,255)){g_coop_server.reset();return "host failed";}
            ssne::set_ammo_rules(preferences.capacity,preferences.pickups);ssne::set_enemy_multiplier(preferences.enemies);
            g_lan_addresses=port::netplay::local_ipv4_addresses();g_coop_host=true;
            activate_native_session();return {};
        }
        native_client_profiles=port::coop::read_native_configuration(port::coop::native_global_configuration(),"PlayerProfiles").values;
        if(!ssne::exi_begin_client_session())return "local profile isolation unavailable";native_card_lease=true;
        g_coop_peer=std::make_unique<port::coop::AuthoritativeClient>();
        const bool started=room?g_coop_peer->start_lan(endpoint.address,uint16_t(endpoint.port),room->join_ticket,fingerprint,name,255):g_coop_peer->start(endpoint.address,uint16_t(endpoint.port),endpoint.pin,fingerprint,name,255);
        if(!started){g_coop_peer.reset();ssne::exi_end_client_session();native_card_lease=false;return "connection failed";}
        native_connecting=true;native_connect_started=std::chrono::steady_clock::now();native_connection_error.clear();return {};
    }catch(const std::exception& e){const std::string why=e.what();if(native_safe_context)stop_native_session(*native_safe_context);return why;}
}
std::string start_native_session(bool hosting,const port::coop::FrontendEndpoint& endpoint){return start_native_session_impl(hosting,endpoint,nullptr);}
void setup_native_frontend(const std::string& data){
    port::coop::install_native_lobby(nullptr,nullptr);
    native_data_directory=data;native_offline_states=g_states_dir;
    if(native_offline_states.ends_with("coop/"))native_offline_states.resize(native_offline_states.size()-5);
    port::coop::NativeFrontendServices callbacks;
    callbacks.options_path=g_settings_path+".multiplayer";
    callbacks.shared_options=[](){
        auto rules=port::coop::native_frontend_options();if(!g_coop_client || !g_coop_peer)return rules;
        const auto world=g_coop_peer->latest_world();if(!world)return rules;
        for(auto [key,out]:{std::pair{"Session.EnemyMultiplier",&rules.enemies},std::pair{"Session.AmmoCapacity",&rules.capacity},std::pair{"Session.AmmoPickupMultiplier",&rules.pickups}})
            if(auto i=world->mission.find(key);i!=world->mission.end())if(const auto n=std::get_if<int64_t>(&i->second)) *out=unsigned(*n);
        return rules;
    };
    callbacks.set_rules=[](const port::coop::MultiplayerOptions& rules){if(g_coop_host){ssne::set_ammo_rules(rules.capacity,rules.pickups);ssne::set_enemy_multiplier(rules.enemies);}};
    callbacks.active=[](){return bool(g_coop_host || g_coop_client);};callbacks.host=[](){return g_coop_host.load();};callbacks.connecting=[](){return native_connecting;};
    callbacks.afk=[](){
        if(g_coop_host && g_coop_server){const auto status=g_coop_server->status();const auto people=g_coop_server->participants();return status.player.slot<people.size() && people[status.player.slot].afk;}
        if(g_coop_client && g_coop_peer){const auto status=g_coop_peer->status();const auto world=g_coop_peer->latest_world();return status.player.slot<world->players.size() && world->players[status.player.slot].afk;}return false;
    };
    callbacks.set_afk=[](bool enabled){std::lock_guard lock(g_session_mutex);if(g_coop_host && g_coop_server)g_coop_server->host_afk(++g_coop_command_sequence,enabled);else if(g_coop_client && g_coop_peer)g_coop_peer->set_afk(enabled);};
    callbacks.start=start_native_session;
    callbacks.join_lan=[](const port::coop::FrontendRoom& room){
        if(!native_browser.can_join(room))return std::string("lan-expired");
        port::coop::FrontendEndpoint endpoint;endpoint.address=room.address;endpoint.port=room.port;return start_native_session_impl(false,endpoint,&room);
    };
    callbacks.players=[](){return g_coop_host && g_coop_server?g_coop_server->status().joined:g_coop_client && g_coop_peer?g_coop_peer->status().joined:0u;};
    callbacks.stop=[](){if(native_safe_context)stop_native_session(*native_safe_context);};
    callbacks.status=[](){
        if(!g_coop_host && !g_coop_client)return native_connection_error;
        const auto status=g_coop_host?g_coop_server->status():g_coop_peer->status();
        return (g_coop_host?g_lan_addresses:std::string(""))+"\n"+port::coop::frontend_text(port::coop::FrontText::Port,port::coop::native_menu_language())+": "+std::to_string(status.port)+(native_lan_join?std::string{}:"\n"+std::string(port::coop::frontend_text(port::coop::FrontText::Pin,port::coop::native_menu_language()))+": "+g_lan_pin);
    };
    callbacks.bot=[](int change,unsigned skill){
        std::lock_guard lock(g_session_mutex);if(!g_coop_host || !g_coop_server)return 0u;
        if(change>0)g_coop_server->add_bot(skill);
        else if(change<0){const auto people=g_coop_server->participants();const auto mask=g_coop_server->bot_mask();for(unsigned i=8;i-->1;)if(mask&(1u<<i)){g_coop_server->remove_bot(people[i].id);break;}}
        unsigned count=0;const auto mask=g_coop_server->bot_mask();for(unsigned i=1;i<8;++i)count+=bool(mask&(1u<<i));return count;
    };
    callbacks.scan=[](){native_browser.search(port::netplay::content_hash(native_data_directory+"/sys/main.dol"),native_discovery_port(),getenv("SSNE_TEST_HIDDEN")!=nullptr);};
    callbacks.rooms=[](){return native_browser.rooms();};callbacks.scanning=[](){return native_browser.searching();};
    port::coop::configure_native_frontend(std::move(callbacks));
    port::NativeMenuMouseServices mouse;
    mouse.allowed=[](uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event){
        const auto own=ssne::netplay_local_player();
        if((g_coop_host || g_coop_client) && root==0xE3)return !port::coop::native_frontend_visible() && ssne::local_information_open() && own>=0 && native_info_mouse::local_gui(gui,unsigned(own));
        return port::coop::native_frontend_mouse_allowed(gui,node,root,id,event);
    };
    mouse.surface_epoch=port::coop::native_frontend_mouse_epoch;
    mouse.regions=[](PPCContext& c){auto regions=port::native_carousel_mouse_regions(c);auto info=native_info_mouse::regions(c);regions.insert(regions.end(),info.begin(),info.end());return regions;};
    mouse.model_region=[](uint32_t gui,uint32_t,uint16_t,uint16_t id){return (g_coop_host || g_coop_client) && port::coop::native_lobby_visible() && port::coop::native_lobby_mouse_owner(gui)>=0 && id==0xA3;};
    mouse.model_owner=[](uint32_t gui,uint32_t,uint16_t,uint16_t){return port::coop::native_lobby_mouse_owner(gui);};
    mouse.intercept=[](PPCContext& c,uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event){
        if(port::coop::native_frontend_mouse_event(c,gui,node,root,id,event))return true;
        if(port::native_carousel_mouse_event(c,gui,node,root,id,event))return true;
        if(native_info_mouse::event(c,gui,node,root,id,event))return true;
        // Original local Join is polled from the existing controller, not a
        // GUI event22 binding. Let its normal eligibility/update path join.
        if(!network_game() && root==0x9A && id==0x9A && event==22 && mem_read32(0x802DB7E8)==3 && mem_read32(0x802DB7EC)!=4){queue_native_local_roster_confirm();return true;}
        if(!(g_coop_host || g_coop_client))return false;
        const auto owner=port::coop::native_lobby_mouse_owner(gui);
        if(owner>=0 && (id==0xA2 || id==0xA3)){
            if(event==24){if(g_coop_host && owner==ssne::netplay_local_player() && g_coop_engine_ready.load())g_coop_engine->request_lobby_campaign(false);return true;}
            if(event==23){if(g_coop_host)port::coop::back_native_lobby(c);else port::coop::native_frontend_mouse_event(c,port::coop::native_lobby_mouse_main(),0,0x9A,0x153,22);return true;}
            if(owner==ssne::netplay_local_player()){
                if(event==18 || event==20 || event==28 || event==30)queue_native_menu_pad_pulse(ssne::PAD_LEFT);
                else if(event==19 || event==21 || event==29 || event==31 || event==22)queue_native_menu_pad_pulse(ssne::PAD_RIGHT);
            }
            return true; // Roster models change through the existing authoritative input path.
        }
        if(owner==-2){
            if(event==22 && id==0x152 && g_coop_host && g_coop_engine_ready.load())g_coop_engine->request_lobby_campaign(false);
            else if(event==23){if(g_coop_host)port::coop::back_native_lobby(c);else port::coop::native_frontend_mouse_event(c,gui,node,root,0x153,22);}
            return true;
        }
        return false;
    };
    port::configure_native_menu_mouse(std::move(mouse));
    port::install_native_menu_mouse();
    if(g_coop_host)advertise_native_session();
}
void session_frontend_frame(PPCContext& c,uint64_t frame){
    native_safe_context=&c;native_current_frame=frame;
    if(native_connecting){
        std::lock_guard lock(g_session_mutex);const auto status=g_coop_peer->status();
        if(status.connected){g_coop_client=true;activate_native_session();}
        else if(!status.connecting || std::chrono::steady_clock::now()-native_connect_started>std::chrono::seconds(15)){
            native_connection_error=status.message;log_line("[native-session-connect-error] "+status.message);stop_native_session(c);
        }
    }
    if(g_coop_client && g_coop_peer && !g_coop_peer->status().connected){const auto why=g_coop_peer->status().message;stop_native_session(c);port::coop::native_frontend_disconnected(why);}
    port::coop::native_frontend_frame(c);
    publish_native_mouse_presentation();
    port::consume_native_menu_pointer(c);
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_NATIVE_MENU")){
        std::ofstream out("native-carousel-mouse-state.json.pending");out<<port::native_carousel_mouse_diagnostics();out.close();
        std::remove("native-carousel-mouse-state.json");std::rename("native-carousel-mouse-state.json.pending","native-carousel-mouse-state.json");
        port::coop::Properties roster;port::coop::publish_native_lobby_characters(roster);
        const auto model_field=roster.find("Session.CharacterModels");
        const auto models=model_field==roster.end()?nullptr:std::get_if<std::vector<int64_t>>(&model_field->second);
        const auto guis=port::coop::native_lobby_mouse_guis();
        std::ofstream lobby("native-lobby-mouse-state.json.pending");lobby<<"{\"active\":"<<(port::coop::native_lobby_visible()?"true":"false")<<",\"own\":"<<ssne::netplay_local_player()<<",\"host\":"<<(g_coop_host?"true":"false")<<",\"guis\":[";
        for(unsigned i=0;i<guis.size();++i){if(i)lobby<<',';lobby<<guis[i];}lobby<<"],\"models\":[";
        if(models)for(unsigned i=0;i<models->size();++i){if(i)lobby<<',';lobby<<(*models)[i];}lobby<<"]}";lobby.close();
        std::remove("native-lobby-mouse-state.json");std::rename("native-lobby-mouse-state.json.pending","native-lobby-mouse-state.json");
        std::ofstream information("native-info-mouse-state.json.pending");information<<native_info_mouse::diagnostics();information.close();
        std::remove("native-info-mouse-state.json");std::rename("native-info-mouse-state.json.pending","native-info-mouse-state.json");
    }
    if(g_coop_engine_ready){port::coop::native_frontend_engine_scope(true);g_coop_engine->frame(c,frame-native_frame_origin);port::coop::native_frontend_engine_scope(false);}
    else if(g_native_world)capture_native_game(c,frame);
}
void session_frontend_tick(PPCContext& c,uint64_t tick){
    g_game_updates.fetch_add(1,std::memory_order_relaxed);
    if(g_coop_engine_ready){port::coop::native_frontend_engine_scope(true);g_coop_engine->tick(c,++native_tick_number);port::coop::native_frontend_engine_scope(false);}
    else if(g_native_world)bootstrap_native_game(c,tick);
    port::coop::native_frontend_tick(c);
}
