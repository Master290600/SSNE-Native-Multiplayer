// UI and guest-thread adapters. No game RAM is read from the UI thread here.
const char* qol_text(unsigned language,const char* en,const char* de,const char* fr){return language==1?de:language==2?fr:en;}
void sync_multiplayer_qol(){
    static uint64_t generation=0;
    static std::array<std::array<port::Bind,port::Settings::kMpSlots>,port::MP_COUNT> bindings{};
    bool rebound=false;
    for(unsigned a=0;a<port::MP_COUNT;++a)for(unsigned b=0;b<port::Settings::kMpSlots;++b)if(!(bindings[a][b]==g_set.multiplayer[a][b])){bindings[a][b]=g_set.multiplayer[a][b];rebound=true;}
    const auto current=g_chat_session_generation.load(std::memory_order_acquire);
    if(generation!=current || rebound){generation=current;g_tactical_ping_requested_at=0;g_roster_input.clear();g_roster_held=false;}
    const bool clear_view=g_chat_available && (g_focused || getenv("SSNE_TEST_HIDDEN")) &&
        !g_chat.active() && !port::menu_open() && !port::mod_menu_open() &&
        !port::coop::native_frontend_visible() && !port::coop::native_lobby_visible() &&
        !ssne::local_pause_menu_open() && !ssne::local_information_open() && !ssne::native_results_presenting() &&
        !ssne::native_cutscene_presenting() && !ssne::native_movie_presenting() && !ssne::paused();
    g_roster_allowed=clear_view && ssne::netplay_world_ready();
    const bool allowed=clear_view && (ssne::player_control_active() || ssne::player_in_vehicle()) && !g_coop_spectating;
    g_qol_input_allowed.store(allowed,std::memory_order_release);
    if(!g_roster_allowed){g_roster_input.clear();g_roster_held=false;}
    if(!allowed)g_tactical_ping_requested_at=0;
}
bool handle_multiplayer_qol_event(const SDL_Event& event){
    if(event.type==SDL_WINDOWEVENT && (event.window.event==SDL_WINDOWEVENT_FOCUS_LOST || event.window.event==SDL_WINDOWEVENT_MINIMIZED)){
        g_roster_input.clear();g_roster_held=false;g_roster_allowed=false;g_tactical_ping_requested_at=0;g_qol_input_allowed=false;return false;
    }
    if(event.type==SDL_KEYUP || event.type==SDL_MOUSEBUTTONUP){g_roster_input.update(g_set,port::MP_ROSTER,event);g_roster_held=g_roster_allowed && g_roster_input.active();}
    if(!g_chat_available || !(g_focused || getenv("SSNE_TEST_HIDDEN")) || g_chat.active() || ImGui::GetIO().WantTextInput || port::menu_open() || port::mod_menu_open() || port::coop::native_frontend_text_active())return false;
    auto consume=[&]{
        if(event.type==SDL_KEYDOWN && unsigned(event.key.keysym.scancode)<g_chat_key_suppressed.size())g_chat_key_suppressed[event.key.keysym.scancode]=true;
        if(event.type==SDL_MOUSEBUTTONDOWN && event.button.button>=1 && event.button.button<=5)g_chat_mouse_suppressed|=SDL_BUTTON(event.button.button);
        return true;
    };
    if(port::multiplayer_event_slots(g_set,port::MP_AFK,event,true)){
        std::lock_guard lock(g_session_mutex);
        const auto status=g_coop_host && g_coop_server?g_coop_server->status():g_coop_client && g_coop_peer?g_coop_peer->status():port::coop::NetworkStatus{};
        const auto world=g_coop_client && g_coop_peer?g_coop_peer->latest_world():nullptr;
        const auto people=g_coop_host && g_coop_server?g_coop_server->participants():world?world->players:std::array<port::coop::Participant,port::coop::max_players>{};
        if(status.connected && status.player.slot<people.size()){
            const auto& person=people[status.player.slot];
            // Returning from AFK also works during results/pause/loading; only
            // entry requires an active local gameplay view.
            if(person.connected && person.id==status.player && (person.afk || (person.ready && g_qol_input_allowed.load(std::memory_order_acquire)))){
                if(g_coop_host)g_coop_server->host_afk(++g_coop_command_sequence,!person.afk);else g_coop_peer->set_afk(!person.afk);
                return consume();
            }
        }
    }
    if(g_roster_allowed && g_roster_input.update(g_set,port::MP_ROSTER,event)){
        g_roster_held=g_roster_input.active();return consume();
    }
    if(g_qol_input_allowed.load(std::memory_order_acquire) && port::multiplayer_event_slots(g_set,port::MP_PING,event,true)){
        g_tactical_ping_requested_at=SDL_GetTicks64()+1;return consume();
    }
    return false;
}
bool refresh_tactical_entity(port::coop::EntityState& entity){
    const auto address=g_coop_engine->world().local_address(entity.id);
    if(address<0x80000000u || address>ssne::guest_ram_end()-0xD0u)return false;
    std::array<float,3> position{};
    for(unsigned axis=0;axis<3;++axis)position[axis]=u2f(mem_read32(address+0x20+axis*4));
    if(!port::coop::tactical_ping_position_valid(position))return false;
    entity.properties["position"]=position;
    if(entity.kind==port::coop::EntityKind::Player || entity.kind==port::coop::EntityKind::Enemy){
        const auto health=u2f(mem_read32(address+0xCC));if(!std::isfinite(health))return false;entity.properties["health"]=health;
    }
    if(entity.kind==port::coop::EntityKind::Pickup){
        const auto binding=port::coop::native_render_binding(address,port::coop::EntityKind::Pickup);
        entity.properties["pickup_view_mask"]=int64_t(binding && binding->model?mem_read32(binding->model+0x30):0);
        std::vector<int64_t> ownership;for(unsigned slot=0;slot<ssne::native_party_capacity();++slot)ownership.push_back(ssne::native_party_pickup_value(address,slot));
        entity.properties["pickup_ownership"]=std::move(ownership);
    }
    return true;
}
void refresh_local_tactical_view(){
    if(!g_tactical_host_view)return;
    if(!g_coop_host || !g_coop_server || !g_coop_engine_ready || !g_coop_engine){g_tactical_host_view.reset();return;}
    const auto world=g_coop_server->latest_world();
    if(!world || world->epoch!=g_tactical_host_view->epoch || world->level!=g_tactical_host_view->level ||
       !port::coop::tactical_ping_gameplay(*world) || g_coop_server->publication_demand().tcp_peers ||
       g_coop_engine->world().current_epoch()!=g_tactical_host_native_epoch || g_coop_server->pings().empty()){
        g_tactical_host_view.reset();return;
    }
    const auto now=SDL_GetTicks64();if(now<g_tactical_host_refresh)return;g_tactical_host_refresh=now+100;
    auto current=std::make_shared<port::coop::WorldState>(*g_tactical_host_view);
    current->players=g_coop_server->participants();
    for(auto it=current->entities.begin();it!=current->entities.end();){if(!refresh_tactical_entity(it->second))it=current->entities.erase(it);else ++it;}
    if(g_coop_server->update_local_ping_view(*current))g_tactical_host_view=std::move(current);
    else g_tactical_host_view.reset();
}
void process_tactical_ping(PPCContext& context){
    std::lock_guard lock(g_session_mutex);
    refresh_local_tactical_view();
    const auto requested=g_tactical_ping_requested_at.exchange(0,std::memory_order_acq_rel);
    if(!requested || !g_qol_input_allowed.load(std::memory_order_acquire))return;
    const auto now=SDL_GetTicks64()+1;
    if(now<requested || now-requested>250 || !g_coop_engine_ready || !g_coop_engine ||
       (!ssne::player_control_active() && !ssne::player_in_vehicle()) || ssne::local_pause_menu_open() || ssne::local_information_open() ||
       port::coop::native_frontend_visible() || port::coop::native_lobby_visible() || ssne::native_results_presenting() ||
       ssne::native_cutscene_presenting() || ssne::native_movie_presenting() || ssne::paused())return;
    const auto status=g_coop_host?g_coop_server->status():g_coop_peer->status();
    auto world=g_coop_host?g_coop_server->latest_world():g_coop_peer->latest_world();
    const auto camera=ssne::mod_view_camera();
    if(!status.connected || !world || !camera)return;
    const bool local_only=g_coop_host && !g_coop_server->publication_demand().tcp_peers;
    std::shared_ptr<port::coop::WorldState> local;
    if(local_only){
        // Explicit button press only: read the lifetime index and tiny position/
        // health fields, never bones or a full network world publication.
        const auto bindings=g_coop_engine->world().capture_bindings();
        if(!bindings.error.empty() || bindings.scene!=4)return;
        local=std::make_shared<port::coop::WorldState>();local->epoch=world->epoch;local->level=world->level;local->players=g_coop_server->participants();
        local->mission["Session.NativePhase"]=std::string("gameplay");
        for(auto entry:bindings.entities){auto& entity=entry.second;
            if(entity.kind!=port::coop::EntityKind::Player && entity.kind!=port::coop::EntityKind::Enemy && entity.kind!=port::coop::EntityKind::Pickup)continue;
            if(refresh_tactical_entity(entity))local->entities.emplace(entry.first,std::move(entity));
        }
        world=local;
    }
    const auto ping=port::resolve_tactical_ping(context,*world,status.player,*camera);
    const bool sent=ping && (g_coop_host?(local_only?g_coop_server->send_ping(*ping,*local):g_coop_server->send_ping(*ping)):g_coop_peer->send_ping(*ping));
    if(sent && local){
        const auto own=local->players[status.player.slot].character;
        std::erase_if(local->entities,[&](const auto& entry){return entry.first!=own && entry.first!=ping->target;});
        g_tactical_host_native_epoch=g_coop_engine->world().current_epoch();g_tactical_host_refresh=0;g_tactical_host_view=std::move(local);
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_NATIVE_MENU"))
        log_line("[tactical-ping-input] slot="+std::to_string(status.player.slot)+" resolved="+std::to_string(bool(ping))+" sent="+std::to_string(sent));
}
void draw_multiplayer_qol(){
    if(!g_chat_available)return;
    std::vector<port::coop::TacticalPing> pings;
    std::shared_ptr<const port::coop::WorldState> world;
    std::shared_ptr<const port::coop::WorldState> ping_world;
    std::vector<port::RosterRow> rows;
    std::vector<port::TeamHudRow> team_rows;
    port::coop::PlayerId own{};
    {
        std::lock_guard lock(g_session_mutex);
        if(g_coop_host && g_coop_server)pings=g_coop_server->pings();
        else if(g_coop_client && g_coop_peer)pings=g_coop_peer->pings();
        else return;
        own=g_coop_host?g_coop_server->status().player:g_coop_peer->status().player;
        if(!pings.empty() || g_roster_held || g_set.team_hud)world=g_coop_host?g_coop_server->latest_world():g_coop_peer->latest_world();
        // Ping views contain only traced entities. Keep session mode/round
        // metadata and player ownership on the published world for the HUD.
        ping_world=world;
        if(g_coop_host && g_tactical_host_view && world && g_tactical_host_view->epoch==world->epoch && !g_coop_server->publication_demand().tcp_peers)ping_world=g_tactical_host_view;
        if(g_roster_held || g_set.team_hud){
            const auto people=g_coop_host?g_coop_server->participants():world?world->players:std::array<port::coop::Participant,port::coop::max_players>{};
            const auto latency=g_coop_host?g_coop_server->player_latencies():g_coop_peer->player_latencies();
            std::array<port::RosterHealth,port::coop::max_players> health{};
            std::array<port::TeamHudSample,port::coop::max_players> team_samples{};
            if(g_coop_host && g_coop_engine){const auto values=g_coop_engine->participant_health();for(unsigned i=0;i<health.size();++i){
                const auto& value=values[i];health[i]={value.player,value.health,value.valid};
                team_samples[i]={value.player,value.character,value.epoch,value.health,value.armor,value.position,value.valid,value.armor_known,value.position_known};
            }}
            else if(world)for(unsigned i=0;i<health.size();++i){
                const auto& person=people[i];const auto entity=world->entities.find(person.character);
                if(person.connected && entity!=world->entities.end() && entity->second.kind==port::coop::EntityKind::Player && entity->second.owner==person.id){
                    const auto value=entity->second.properties.find("health");
                    if(value!=entity->second.properties.end())if(const auto* hp=std::get_if<float>(&value->second))health[i]={person.id,*hp,true};
                }
            }
            uint8_t bots=0;
            if(g_coop_host)bots=g_coop_server->bot_mask();
            else if(world){const auto it=world->mission.find("Session.Bots.Mask");if(it!=world->mission.end())if(const auto* mask=std::get_if<int64_t>(&it->second))bots=uint8_t(*mask);}
            if(g_roster_held)rows=port::multiplayer_roster(people,latency,health,bots);
            if(g_set.team_hud && world){
                if(!g_coop_host)team_samples=port::team_hud_samples(*world);
                const auto mask=[&](const char* key){const auto found=world->mission.find(key);if(found!=world->mission.end())if(const auto* value=std::get_if<int64_t>(&found->second))return uint8_t(*value);return uint8_t(0);};
                team_rows=port::team_hud_rows(world->epoch,port::team_hud_cooperative(*world),own,people,team_samples,bots,mask("Session.Survival.WaitingMask"),mask("Session.Survival.EliminatedMask"));
            }
        }
    }
    const auto language=g_set.game_language>=0?unsigned(g_set.game_language):port::native_crosshair_language();
    const bool visible=g_qol_input_allowed.load(std::memory_order_acquire) && !g_chat.active() && !port::menu_open() && !port::mod_menu_open();
    const bool team_visible=g_set.team_hud && g_roster_allowed && !g_chat.active() && !port::menu_open() && !port::mod_menu_open();
    if(team_visible && g_pointer_image.valid())port::draw_team_hud(team_rows,g_pointer_image,language);
    if(visible && !pings.empty() && ping_world && g_pointer_image.valid())if(const auto camera=ssne::mod_view_camera())
        port::draw_tactical_pings(pings,ping_world.get(),*camera,g_pointer_image,language);
    if(g_roster_allowed && g_roster_held && !g_chat.active() && !port::menu_open() && !port::mod_menu_open()){
        const auto* viewport=ImGui::GetMainViewport();
        const auto image=g_pointer_image.valid()?g_pointer_image:port::GameViewRect{viewport->Pos.x,viewport->Pos.y,viewport->Size.x,viewport->Size.y,1};
        ImGui::SetNextWindowPos(ImVec2(image.x+image.width*.5f,image.y+image.height*.22f),ImGuiCond_Always,ImVec2(.5f,0));
        ImGui::SetNextWindowSize(ImVec2(std::min(650.f,std::max(300.f,image.width-24.f)),0),ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.78f);
        const auto flags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::Begin("##multiplayer-roster",nullptr,flags);
        ImGui::TextColored(ImVec4(1.f,.82f,.15f,1.f),"%s  (%u)",qol_text(language,"PLAYERS","SPIELER","JOUEURS"),unsigned(rows.size()));
        if(ImGui::BeginTable("##players",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)){
            ImGui::TableSetupColumn(qol_text(language,"Name","Name","Nom"),ImGuiTableColumnFlags_WidthStretch,2.f);
            ImGui::TableSetupColumn(qol_text(language,"Status","Status","Etat"),ImGuiTableColumnFlags_WidthStretch,1.4f);
            ImGui::TableSetupColumn(qol_text(language,"Health","Leben","Vie"));ImGui::TableSetupColumn("Ping");ImGui::TableHeadersRow();
            for(const auto& row:rows){
                ImGui::TableNextRow();ImGui::TableNextColumn();
                std::string name=row.name+(row.host?" [Host]":row.bot?" [Bot]":"");ImGui::TextUnformatted(name.c_str());
                ImGui::TableNextColumn();const char* state="";
                switch(row.status){
                case port::RosterStatus::Loading:state=qol_text(language,"Loading","Lädt","Chargement");break;
                case port::RosterStatus::Spectator:state=qol_text(language,"Spectating","Zuschauer","Spectateur");break;
                case port::RosterStatus::Dead:state=qol_text(language,"Dead","Tot","Mort");break;
                case port::RosterStatus::Afk:state="AFK (Bot)";break;
                case port::RosterStatus::Playing:state=qol_text(language,"Playing","Spielt","En jeu");break;
                }
                ImGui::TextUnformatted(state);ImGui::TableNextColumn();if(row.health_known)ImGui::Text("%.0f",std::ceil(row.health));else ImGui::TextUnformatted("--");
                ImGui::TableNextColumn();if(row.ping_known){const auto color=row.ping_ms<80?ImVec4(.45f,1,.45f,1):row.ping_ms<160?ImVec4(1,.83f,.25f,1):ImVec4(1,.35f,.3f,1);ImGui::TextColored(color,"%u ms",row.ping_ms);}else ImGui::TextUnformatted("--");
            }
            ImGui::EndTable();
        }
        const auto ping_key=port::multiplayer_binding_label(g_set,port::MP_PING),roster_key=port::multiplayer_binding_label(g_set,port::MP_ROSTER);
        ImGui::Separator();ImGui::TextWrapped(qol_text(language,"%s: mark | Hold %s: players","%s: markieren | %s halten: Spielerliste","%s : marquer | Maintenir %s : joueurs"),ping_key.c_str(),roster_key.c_str());
        ImGui::End();
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_NATIVE_MENU")){
        static uint64_t next=0;const auto now=SDL_GetTicks64();if(now<next)return;next=now+200;
        std::ofstream out("multiplayer-qol-state.json.pending");out<<"{\"input_allowed\":"<<visible<<",\"roster_held\":"<<g_roster_held<<",\"team_hud_enabled\":"<<g_set.team_hud<<",\"team_hud_visible\":"<<(team_visible && !team_rows.empty())<<",\"local_slot\":"<<own.slot<<",\"bindings\":[";
        for(unsigned a=0;a<port::MP_COUNT;++a){if(a)out<<',';out<<'[';for(unsigned b=0;b<port::Settings::kMpSlots;++b){if(b)out<<',';out<<std::quoted(port::bind_to_string(g_set.multiplayer[a][b]));}out<<']';}
        out<<"],\"team_rows\":[";bool comma=false;
        for(const auto& row:team_rows){if(comma)out<<',';comma=true;out<<"{\"slot\":"<<row.player.slot<<",\"generation\":"<<row.player.generation<<",\"name\":"<<std::quoted(row.name)<<",\"state\":"<<unsigned(row.status)<<",\"bot\":"<<row.bot<<",\"health_known\":"<<row.health_known<<",\"health\":"<<row.health<<",\"armor_known\":"<<row.armor_known<<",\"armor\":"<<row.armor<<",\"distance_known\":"<<row.distance_known<<",\"distance\":"<<row.distance<<"}";}
        out<<"],\"ping_count\":"<<pings.size()<<",\"pings\":[";comma=false;
        for(const auto& ping:pings){if(comma)out<<',';comma=true;out<<"{\"slot\":"<<ping.sender.slot<<",\"kind\":"<<unsigned(ping.kind)<<",\"target\":"<<ping.target<<",\"sequence\":"<<ping.sequence<<",\"remaining_ms\":"<<ping.remaining_ms<<"}";}
        out<<"],\"rows\":[";comma=false;
        for(const auto& row:rows){if(comma)out<<',';comma=true;out<<"{\"slot\":"<<row.player.slot<<",\"generation\":"<<row.player.generation<<",\"name\":"<<std::quoted(row.name)<<",\"state\":"<<unsigned(row.status)<<",\"bot\":"<<row.bot<<",\"health_known\":"<<row.health_known<<",\"health\":"<<row.health<<",\"ping_known\":"<<row.ping_known<<",\"ping_ms\":"<<row.ping_ms<<"}";}out<<"]}";out.close();
        if(out){std::remove("multiplayer-qol-state.json");std::rename("multiplayer-qol-state.json.pending","multiplayer-qol-state.json");}
    }
}
