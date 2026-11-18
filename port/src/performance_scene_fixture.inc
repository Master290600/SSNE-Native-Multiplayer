// Hidden, explicitly armed native combat fixture. It observes the lightweight
// enemy registry once per second; no NativeWorld snapshot is used for SP.
bool perf_scene_fixture_enabled(){
    static const bool enabled=[] {const auto* h=getenv("SSNE_TEST_HIDDEN");const auto* f=getenv("SSNE_PERF_SCENE_FIXTURE");return h && !strcmp(h,"1") && f && !strcmp(f,"1");}();
    return enabled;
}
void perf_scene_fixture_frame(PPCContext& context,uint64_t frame){
    if(!perf_scene_fixture_enabled())return;
    using Clock=std::chrono::steady_clock;
    static bool started=false;static unsigned waves=0;
    static auto poll=Clock::time_point{},report_at=Clock::time_point{},next_wave=Clock::time_point{},last_wave=Clock::time_point{};
    static std::string error;
    auto integer=[](const char* name,int fallback,int low,int high){const auto* p=getenv(name);return p?std::clamp(atoi(p),low,high):fallback;};
    auto number=[](const char* name,float fallback,float low,float high){const auto* p=getenv(name);const float v=p?float(atof(p)):fallback;return std::isfinite(v)?std::clamp(v,low,high):fallback;};
    const auto now=Clock::now();const auto scene=mem_read32(ssne::game_address(0x802DB0E0));
    const auto mode=mem_read32(ssne::game_address(0x802DB1BC));
    const bool busy=ssne::local_information_open() || ssne::local_pause_menu_open() || ssne::native_movie_presenting() || ssne::native_cutscene_presenting() || ssne::native_results_presenting() || port::menu_open() || port::mod_menu_open();
    auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;};
    auto actor_at=[&](unsigned slot){const auto registry=ssne::native_party_registry();if(slot>=ssne::native_party_capacity() || !readable(registry+slot*4,4))return uint32_t(0);const auto actor=mem_read32(registry+slot*4);return readable(actor,0x790) && ssne::native_party_actor_occupied(actor)?actor:0u;};
    auto place=[&](uint32_t actor,const std::array<float,3>& xyz){
        const auto fn=ssne::game_address(0x8009C300u);if(!actor || !ssne_find_func(fn) || context.r[1]<0x80000300u || !readable(context.r[1]-0x300,0x300))return false;
        for(float f:xyz)if(!std::isfinite(f) || std::abs(f)>100000)return false;
        std::array<uint8_t,0x300> saved;std::memcpy(saved.data(),ssne::host_ptr(context.r[1]-0x300),saved.size());
        auto call=context;call.r[1]-=0x200;call.msr&=~0x8000u;mem_write32(call.r[1],context.r[1]);const auto p=call.r[1]+0x80;
        for(unsigned i=0;i<3;++i)mem_write32(p+i*4,f2u(xyz[i]));call.r[3]=actor;call.r[4]=p;
        try{ssne_call(call,fn);}catch(...){std::memcpy(ssne::host_ptr(context.r[1]-0x300),saved.data(),saved.size());throw;}
        std::memcpy(ssne::host_ptr(context.r[1]-0x300),saved.data(),saved.size());return true;
    };
    if(!started && error.empty() && now>=poll){
        poll=now+std::chrono::milliseconds(200);
        const auto* path=getenv("SSNE_PERF_SCENE_ARM_FILE");std::ifstream input(path?path:"scene-arm.txt");std::string command;input>>command;
        if(command=="start" && scene==4 && !busy && ssne::player_control_active()){
            std::array<float,3> anchor{};const bool supplied=bool(input>>anchor[0]>>anchor[1]>>anchor[2]);
            if(!g_coop_client && supplied && !place(actor_at(0),anchor))error="Native anchor setter unavailable";
            if(error.empty() && g_coop_host && actor_at(1)){
                const auto actor=actor_at(0);const float yaw=u2f(mem_read32(actor+0x290));
                std::array<float,3> behind{};for(unsigned i=0;i<3;++i)behind[i]=u2f(mem_read32(actor+0x20+i*4));
                behind[0]-=std::sin(yaw)*6.f;behind[2]-=std::cos(yaw)*6.f;
                if(!place(actor_at(1),behind))error="Native partner anchor setter unavailable";
            }
            if(error.empty()){started=true;ssne::perf::fixture_quiet.store(true,std::memory_order_release);next_wave=now+std::chrono::milliseconds(500);last_wave=now;}
        }
    }
    const unsigned target=unsigned(integer("SSNE_PERF_SCENE_WAVES",0,0,100));
    if(started && error.empty() && !g_coop_client && scene==4 && !busy && waves<target && now>=next_wave){
        const auto type=unsigned(integer("SSNE_PERF_SCENE_TYPE",12,0,255));const auto count=integer("SSNE_PERF_SCENE_COUNT",10,1,10);
        const float distance=number("SSNE_PERF_SCENE_DISTANCE",7.f,3.f,30.f)+float(waves)*number("SSNE_PERF_SCENE_SPACING",1.5f,0.f,5.f);
        ssne::mod_test_enemy_at_player(type,0,count,distance);++waves;last_wave=now;next_wave=now+std::chrono::milliseconds(integer("SSNE_PERF_SCENE_INTERVAL_MS",1000,1000,300000));
    }
    // Hidden read-only observer cadence; ordinary fixture reports stay1Hz.
    const auto* boss_observer=getenv("SSNE_TEST_CAMPAIGN_BOSS_OBSERVE");
    const unsigned observer_ms=boss_observer && !strcmp(boss_observer,"1")?
        unsigned(integer("SSNE_TEST_BOSS_OBSERVE_MS",1000,100,1000)):1000u;
    if(now<report_at)return;report_at=now+std::chrono::milliseconds(observer_ms);
    const auto enemies=ssne::netplay_bot_enemies();const auto level=port::coop::native_active_level();
    const auto local=actor_at(g_coop_client?unsigned(g_coop_local_player.load()):0);
    if(!g_coop_host && !g_coop_client && ssne::perf::enabled.load(std::memory_order_relaxed)){
        ssne::perf::Context diagnostic;diagnostic.frame=frame;diagnostic.epoch=1;diagnostic.scene=scene;diagnostic.level=level.value_or("");diagnostic.entities=enemies.size();
        if(local)for(unsigned i=0;i<3;++i)diagnostic.position[i]=u2f(mem_read32(local+0x20+i*4));ssne::perf::set_context(std::move(diagnostic));
    }
    std::ofstream out("fixture-state.json.pending",std::ios::binary|std::ios::trunc);
    out<<"{\"information_open\":"<<(ssne::local_information_open()?"true":"false")<<",\"pause_open\":"<<(ssne::local_pause_menu_open()?"true":"false")<<",\"movie_presenting\":"<<(ssne::native_movie_presenting()?"true":"false")<<",\"cutscene_presenting\":"<<(ssne::native_cutscene_presenting()?"true":"false")<<",\"results_presenting\":"<<(ssne::native_results_presenting()?"true":"false")<<",\"scene\":"<<scene<<",\"native_mode\":"<<mode<<",\"level\":"<<ssne::perf::quote(level.value_or(""))<<",\"frame\":"<<frame<<",\"started\":"<<(started?"true":"false")<<",\"quiet\":"<<(ssne::perf::fixture_quiet.load()?"true":"false")<<",\"presentation_busy\":"<<(busy?"true":"false")<<",\"local_actor\":"<<local<<",\"waves\":"<<waves<<",\"configured_waves\":"<<target<<",\"settled\":"<<(started && (g_coop_client || waves>=target) && now-last_wave>=std::chrono::seconds(2)?"true":"false")<<",\"error\":"<<ssne::perf::quote(error)<<",\"players\":[";
    bool comma=false;for(unsigned slot=0;slot<std::min(8u,ssne::native_party_capacity());++slot)if(const auto actor=actor_at(slot)){
        if(comma)out<<',';comma=true;out<<"{\"slot\":"<<slot<<",\"actor\":"<<actor<<",\"position\":[";
        for(unsigned i=0;i<3;++i){if(i)out<<',';out<<u2f(mem_read32(actor+0x20+i*4));}
        const auto aim=mem_read32(actor+0x37C),option=mem_read32(actor+0x378);
        if(readable(aim,0x60) && readable(option,0x28))out<<"],\"auto_aim_option\":"<<mem_read32(option+0x24)<<",\"auto_aim_mode\":"<<mem_read32(aim)<<",\"auto_aim_available\":"<<mem_read32(aim+0x44)<<",\"auto_aim_enabled\":"<<mem_read32(aim+0x48)<<",\"human_auto_aim_preference\":"<<unsigned(g_coop_engine?g_coop_engine->local_preferences()[0]:255)<<",\"yaw\":"<<u2f(mem_read32(actor+0x290));
        else out<<"],\"yaw\":"<<u2f(mem_read32(actor+0x290));
        out<<",\"pitch\":"<<u2f(mem_read32(actor+0x294))<<",\"selected_weapon\":"<<mem_read32(actor+0x5DC)<<",\"health\":"<<u2f(mem_read32(actor+0xCC));
        // Read-only lifecycle diagnostics for hidden private regression tests.
        // Camera/model pointers are independently guarded; no fixture is armed.
        const auto camera=mem_read32(actor+0x368),proxy=mem_read32(actor+0x148),model=readable(proxy,0x20)?mem_read32(proxy+0x10):0u;
        out<<",\"dead\":"<<mem_read32(actor+0x328)<<",\"death_state\":"<<mem_read32(actor+0x370)<<",\"flags\":"<<mem_read32(actor+0x1C)
           <<",\"occupied\":"<<(ssne::native_party_actor_occupied(actor)?"true":"false")<<",\"spawn_waiting\":"<<(ssne::native_party_spawn_waiting(actor)?"true":"false")
           <<",\"spawn_revision\":"<<ssne::native_party_spawn_revision(actor)<<",\"camera\":"<<camera<<",\"camera_mode\":";
        if(readable(camera,0xB0) && mem_read32(camera)==0x80210880u && mem_read32(camera+4)==actor)out<<mem_read32(camera+0xAC);else out<<"null";
        if(boss_observer && !strcmp(boss_observer,"1")){
            bool valid=readable(camera,0x100) && mem_read32(camera)==0x80210880u && mem_read32(camera+4)==actor;
            std::array<float,3> eye{};
            if(valid)for(unsigned axis=0;axis<3;++axis){eye[axis]=u2f(mem_read32(camera+0x30+axis*16));valid&=std::isfinite(eye[axis]) && std::abs(eye[axis])<=1000000.f;}
            out<<",\"camera_position\":";
            if(valid)out<<'['<<eye[0]<<','<<eye[1]<<','<<eye[2]<<']';else out<<"null";
        }
        out<<",\"body_proxy\":"<<proxy<<",\"body_mask\":";
        if(readable(proxy,0x20) && readable(model,0xB0) && mem_read32(model)==0x8021D668u)out<<mem_read32(proxy+0x18);else out<<"null";
        out<<",\"expected_body_mask\":"<<mem_read32(actor+0x170)<<'}';
    }
    out<<"],\"boss_hud\":";
    const auto* boss_observe=getenv("SSNE_TEST_CAMPAIGN_BOSS_OBSERVE");
    if(boss_observe && !strcmp(boss_observe,"1")){
        ssne::NativeBossHudObservation observed;std::string boss_error;
        const bool valid=ssne::observe_native_boss_hud(observed,boss_error);
        out<<"{\"valid\":"<<(valid?"true":"false")<<",\"present\":"<<(observed.state.present?"true":"false")
           <<",\"active\":"<<(observed.state.active?"true":"false")<<",\"suppressed\":"<<(observed.state.suppressed?"true":"false")
           <<",\"ratio\":";if(std::isfinite(observed.state.ratio))out<<observed.state.ratio;else out<<"null";
        out<<",\"hidden\":"<<(observed.hidden?"true":"false")<<",\"flash_seconds\":";
        if(std::isfinite(observed.flash_seconds))out<<observed.flash_seconds;else out<<"null";
        out<<",\"widget\":"<<observed.widget<<",\"drawable\":"<<observed.drawable<<",\"error\":"<<ssne::perf::quote(boss_error)<<'}';
    }else out<<"null";
    out<<",\"enemies\":[";comma=false;for(const auto& enemy:enemies){if(comma)out<<',';comma=true;out<<"{\"id\":"<<enemy.id<<",\"health\":"<<enemy.health<<",\"position\":["<<enemy.position[0]<<','<<enemy.position[1]<<','<<enemy.position[2]<<"]}";}
    out<<"],\"profile\":{";
    const auto profile=port::coop::read_native_configuration(port::coop::native_global_configuration(),"PlayerProfiles.Profile0");
    bool profile_comma=false;for(const char* key:{"Level","LevelSet","Difficulty"}){
        const auto found=profile.values.find(std::string("PlayerProfiles.Profile0.")+key);if(found==profile.values.end())continue;
        if(profile_comma)out<<',';profile_comma=true;out<<ssne::perf::quote(key)<<':';
        if(const auto value=std::get_if<std::string>(&found->second))out<<ssne::perf::quote(*value);
        else if(const auto value=std::get_if<int64_t>(&found->second))out<<*value;else out<<"null";
    }
    out<<"}}";out.close();if(out){std::remove("fixture-state.json");std::rename("fixture-state.json.pending","fixture-state.json");}
}
