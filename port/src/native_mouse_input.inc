// Hidden crosshair settings tests use real ImGui input events, never widget activation.
struct CrosshairUiFixtureEvent {port::NativeMenuPointerEvent::Kind kind;float x,y,dx,dy;unsigned button;};
std::vector<CrosshairUiFixtureEvent> g_crosshair_test_imgui;
int g_crosshair_test_settings_request=-1;
void flush_crosshair_test_imgui() {
    if(!getenv("SSNE_TEST_HIDDEN")||!getenv("SSNE_TEST_CROSSHAIR"))return;
    auto& io=ImGui::GetIO();io.AddFocusEvent(true);
    for(const auto& event:g_crosshair_test_imgui) {
        using K=port::NativeMenuPointerEvent::Kind;
        if(event.kind==K::Cancel) {io.AddMouseButtonEvent(0,false);io.AddMouseButtonEvent(1,false);continue;}
        io.AddMousePosEvent(event.x,event.y);
        if(event.kind==K::Down||event.kind==K::Up) {
            const int button=event.button==SDL_BUTTON_LEFT?0:event.button==SDL_BUTTON_RIGHT?1:2;
            io.AddMouseButtonEvent(button,event.kind==K::Down);
        } else if(event.kind==K::Wheel)io.AddMouseWheelEvent(event.dx,event.dy);
    }
    g_crosshair_test_imgui.clear();
}
// Included by main.cpp: SDL/UI-thread adapter only. Native GUI calls are queued.
bool g_native_pointer_active=false,g_presentation_pointer_active=false;
Uint32 g_presentation_mouse_buttons=0;
WheelPulse g_presentation_click[2];
uint64_t g_presentation_input_sequence=0;
uint64_t g_pointer_event_sequence=0,g_pointer_accepted_sequence=0;
uint64_t g_pointer_seen_surface=0,g_pointer_seen_geometry=0,g_pointer_seen_view=0;
bool g_pointer_fixture_focused=true;
std::atomic<bool> g_local_roster_pointer_available{false};
std::atomic<bool> g_native_guest_presentation{false};
// Guest-frame publisher: both public queries inspect live guest scene/camera
// state, so SDL/UI code consumes only this immutable atomic snapshot.
void publish_native_mouse_presentation(){
    g_native_guest_presentation.store(ssne::native_cutscene_presenting() || ssne::native_movie_presenting(),std::memory_order_release);
}


void post_native_mouse(port::NativeMenuPointerEvent::Kind kind,float x=0,float y=0,uint8_t button=0,float wheel_x=0,float wheel_y=0){
    const auto snapshot=port::native_menu_mouse_snapshot();
    const auto point=port::point_in_game_view(g_pointer_image,x+g_pointer_origin_x,y+g_pointer_origin_y);
    port::NativeMenuPointerEvent event;event.kind=kind;event.u=point.u;event.v=point.v;event.inside=point.valid && point.inside;
    event.display_wide=g_pointer_image.wide;event.button=button;event.wheel_x=wheel_x;event.wheel_y=wheel_y;event.sequence=++g_pointer_event_sequence;event.image_sequence=g_pointer_image_sequence;
    event.surface_generation=snapshot.surface_generation;event.geometry_generation=snapshot.geometry_generation;
    port::queue_native_menu_pointer(event);
}
void clear_native_menu_pad_pulses();
void cancel_native_mouse(){g_presentation_mouse_buttons=0;g_presentation_click[0]=g_presentation_click[1]=WheelPulse{};clear_native_menu_pad_pulses();post_native_mouse(port::NativeMenuPointerEvent::Kind::Cancel);g_mouse_dx=g_mouse_dy=0;g_wheel[0]=g_wheel[1]=WheelPulse{};}

void sync_native_mouse_mode(){
    const auto snapshot=port::native_menu_mouse_snapshot();
    const bool focused=(getenv("SSNE_TEST_HIDDEN") || g_focused) && g_pointer_fixture_focused;
    const bool available=g_pointer_image.valid() && focused && !port::menu_open() && !port::mod_menu_open();
    const bool active=snapshot.active && available;
    // Credits and movies have no native GUI root. Keep their ordinary pad
    // Continue/Back path, without treating a disabled menu as a blank screen.
    const bool presentation=available && !snapshot.active && !snapshot.has_surface && (!ssne::player_control_active() || g_native_guest_presentation.load(std::memory_order_acquire)) && !ssne::native_results_presenting();
    const bool mode=active || presentation,previous=g_native_pointer_active || g_presentation_pointer_active;
    if(snapshot.surface_generation!=g_pointer_seen_surface || snapshot.geometry_generation!=g_pointer_seen_geometry || g_pointer_view_generation!=g_pointer_seen_view || active!=g_native_pointer_active || presentation!=g_presentation_pointer_active){
        cancel_native_mouse();g_pointer_seen_surface=snapshot.surface_generation;g_pointer_seen_geometry=snapshot.geometry_generation;g_pointer_seen_view=g_pointer_view_generation;
    }
    if(mode && !previous){
        g_pointer_restore_capture|=g_mouse_captured;g_mouse_captured=false;
        if(!getenv("SSNE_TEST_HIDDEN"))SDL_SetRelativeMouseMode(SDL_FALSE);
    }
    g_native_pointer_active=active;g_presentation_pointer_active=presentation;
    g_local_roster_pointer_available.store(active && !ImGui::GetIO().WantCaptureMouse,std::memory_order_release);
    if(!mode && g_pointer_restore_capture && focused && ssne::player_control_active() && !snapshot.active && !ssne::native_results_presenting() && !port::menu_open() && !port::mod_menu_open() && g_set.mouse_look){
        g_pointer_restore_capture=false;g_mouse_captured=true;g_mouse_dx=g_mouse_dy=0;
        if(!getenv("SSNE_TEST_HIDDEN")){g_capture_click|=SDL_GetMouseState(nullptr,nullptr);SDL_SetRelativeMouseMode(SDL_TRUE);}
    }
}

// Both SDL and private test coordinates pass through this exact adapter.
bool route_native_mouse_pointer(port::NativeMenuPointerEvent::Kind kind,float x,float y,uint8_t button=0,float wheel_x=0,float wheel_y=0){
    using Kind=port::NativeMenuPointerEvent::Kind;
    if(kind==Kind::Cancel){cancel_native_mouse();return true;}
    if(port::menu_open() || port::mod_menu_open() || ImGui::GetIO().WantCaptureMouse){cancel_native_mouse();return false;}
    if(g_native_pointer_active){post_native_mouse(kind,x,y,button,wheel_x,wheel_y);return true;}
    if(!g_presentation_pointer_active)return false;
    const auto point=port::point_in_game_view(g_pointer_image,x+g_pointer_origin_x,y+g_pointer_origin_y);
    if(button==SDL_BUTTON_LEFT || button==SDL_BUTTON_RIGHT){
        const Uint32 mask=SDL_BUTTON(button);
        if(kind==Kind::Down && point.valid && point.inside && !(g_presentation_mouse_buttons&mask)){
            g_presentation_mouse_buttons|=mask;
            // The game samples pads less often than SDL polls events. Preserve
            // even a Down+Up received in the same window-event batch.
            auto& pulse=g_presentation_click[button==SDL_BUTTON_LEFT?0:1];pulse.pending=std::min(pulse.pending+1,4);
        }
        if(kind==Kind::Up)g_presentation_mouse_buttons&=~mask;
    }
    g_presentation_input_sequence=++g_pointer_event_sequence;
    return true;
}

// The caller may still feed keyboard/controller input in this same frame.
bool handle_native_mouse_event(const SDL_Event& event){
    using Kind=port::NativeMenuPointerEvent::Kind;
    const bool pointer=event.type==SDL_MOUSEMOTION || event.type==SDL_MOUSEBUTTONDOWN || event.type==SDL_MOUSEBUTTONUP || event.type==SDL_MOUSEWHEEL;
    if(pointer && getenv("SSNE_TEST_HIDDEN"))return true; // Hidden fixtures use only the shared file adapter.
    if(!pointer || (!g_native_pointer_active && !g_presentation_pointer_active))return false;
    if(port::menu_open() || port::mod_menu_open()){cancel_native_mouse();return false;}
    if(ImGui::GetIO().WantCaptureMouse){cancel_native_mouse();return false;}
    // Mouse events for other SDL windows must not hit this game's retained image.
    const auto* viewport=ImGui::GetMainViewport();
    const Uint32 id=Uint32(reinterpret_cast<uintptr_t>(viewport->PlatformHandle));
    const Uint32 event_window=event.type==SDL_MOUSEMOTION?event.motion.windowID:event.type==SDL_MOUSEWHEEL?event.wheel.windowID:event.button.windowID;
    if(id && event_window && id!=event_window)return false;
    if(event.type==SDL_MOUSEMOTION)return route_native_mouse_pointer(Kind::Move,float(event.motion.x),float(event.motion.y));
    else if(event.type==SDL_MOUSEBUTTONDOWN || event.type==SDL_MOUSEBUTTONUP)return route_native_mouse_pointer(event.type==SDL_MOUSEBUTTONDOWN?Kind::Down:Kind::Up,float(event.button.x),float(event.button.y),event.button.button);
    else{
        int x=0,y=0;SDL_GetMouseState(&x,&y);
#if SDL_VERSION_ATLEAST(2,26,0)
        x=event.wheel.mouseX;y=event.wheel.mouseY;
#endif
        float dx=float(event.wheel.x),dy=float(event.wheel.y);
#if SDL_VERSION_ATLEAST(2,0,18)
        dx=event.wheel.preciseX;dy=event.wheel.preciseY;
#endif
        if(event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED){dx=-dx;dy=-dy;}
        return route_native_mouse_pointer(Kind::Wheel,float(x),float(y),0,dx,dy);
    }
    return true;
}

// Trusted lobby model intents still travel through normal client ControlInput.
std::mutex g_native_menu_pulse_mutex;
std::deque<uint16_t> g_native_menu_pulses;
uint16_t g_native_menu_pulse=0;
std::chrono::steady_clock::time_point g_native_menu_pulse_until{};
bool g_native_menu_pulse_gap=false;
// Original offline Players9A polls a real controller A edge instead of a GUI
// event22 callback. The guest validates that exact root before queuing this
// single intent; UI delivery still uses feed_local_pad's owned KBM port only.
struct LocalRosterConfirm {
    bool pending=false,active=false;
    uint64_t surface=0,geometry=0;
    std::chrono::steady_clock::time_point queued{},until{},gap_until{};
};
LocalRosterConfirm g_local_roster_confirm;
void queue_native_local_roster_confirm(){
    const auto snapshot=port::native_menu_mouse_snapshot();
    if(network_game() || !snapshot.active || !g_local_roster_pointer_available.load(std::memory_order_acquire))return;
    const auto now=std::chrono::steady_clock::now();
    std::lock_guard lock(g_native_menu_pulse_mutex);
    if(g_local_roster_confirm.pending || g_local_roster_confirm.active || now<g_local_roster_confirm.gap_until)return;
    g_local_roster_confirm={};g_local_roster_confirm.pending=true;
    g_local_roster_confirm.surface=snapshot.surface_generation;g_local_roster_confirm.geometry=snapshot.geometry_generation;g_local_roster_confirm.queued=now;
}
uint16_t take_native_local_roster_confirm(const port::NativeMenuMouseSnapshot& snapshot,std::chrono::steady_clock::time_point now){
    // Caller holds the pulse mutex. No guest RAM, SDL or ImGui reads occur here.
    auto& pulse=g_local_roster_confirm;
    if(network_game() || !g_local_roster_pointer_available.load(std::memory_order_acquire) || !snapshot.active || snapshot.surface_generation!=pulse.surface || snapshot.geometry_generation!=pulse.geometry){pulse={};return 0;}
    if(pulse.pending){
        if(now-pulse.queued>std::chrono::milliseconds(250)){pulse={};return 0;}
        pulse.pending=false;pulse.active=true;pulse.until=now+std::chrono::milliseconds(100);
    }
    if(pulse.active && now>=pulse.until){pulse.active=false;pulse.gap_until=now+std::chrono::milliseconds(100);}
    return pulse.active?ssne::PAD_A:0;
}
void clear_native_menu_pad_pulses(){
    std::lock_guard lock(g_native_menu_pulse_mutex);g_native_menu_pulses.clear();g_native_menu_pulse=0;g_native_menu_pulse_gap=false;g_local_roster_confirm={};g_local_roster_pointer_available.store(false,std::memory_order_release);
}
void queue_native_menu_pad_pulse(uint16_t buttons){
    buttons&=ssne::PAD_LEFT|ssne::PAD_RIGHT;if(!buttons)return;
    std::lock_guard lock(g_native_menu_pulse_mutex);if(g_native_menu_pulses.size()<16)g_native_menu_pulses.push_back(buttons);
}
uint16_t take_native_menu_pad_pulse(){
    const auto snapshot=port::native_menu_mouse_snapshot();
    std::lock_guard lock(g_native_menu_pulse_mutex);
    const auto now=std::chrono::steady_clock::now();
    const auto roster=take_native_local_roster_confirm(snapshot,now);
    if(!port::coop::native_lobby_visible() || port::coop::native_frontend_visible()){g_native_menu_pulses.clear();g_native_menu_pulse=0;g_native_menu_pulse_gap=false;return roster;}
    if(g_native_menu_pulse && now>=g_native_menu_pulse_until){g_native_menu_pulse=0;g_native_menu_pulse_gap=true;g_native_menu_pulse_until=now+std::chrono::milliseconds(100);}
    if(g_native_menu_pulse_gap && now>=g_native_menu_pulse_until)g_native_menu_pulse_gap=false;
    if(!g_native_menu_pulse && !g_native_menu_pulse_gap && !g_native_menu_pulses.empty()){g_native_menu_pulse=g_native_menu_pulses.front();g_native_menu_pulses.pop_front();g_native_menu_pulse_until=now+std::chrono::milliseconds(100);}
    return uint16_t(g_native_menu_pulse|roster);
}

// Private acceptance tests feed logical SDL window coordinates through this
// same adapter, without posting desktop events or focusing/capturing a window.
void native_mouse_test_command(){
    if(!getenv("SSNE_TEST_HIDDEN"))return;
    const auto path=getenv("SSNE_NATIVE_MOUSE_COMMAND");if(!path || !*path)return;
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary);uint64_t sequence=0;std::string action;
    if(!(file>>sequence>>action) || sequence<=g_pointer_accepted_sequence)return;
    using Kind=port::NativeMenuPointerEvent::Kind;
    float x=0,y=0,dx=0,dy=0;unsigned button=0;bool valid=true;Kind kind=Kind::Cancel;
    if(action=="move"){kind=Kind::Move;valid=bool(file>>x>>y);}
    else if(action=="down" || action=="up"){
        kind=action=="down"?Kind::Down:Kind::Up;std::string name;valid=bool(file>>name>>x>>y);
        button=name=="left"?SDL_BUTTON_LEFT:name=="middle"?SDL_BUTTON_MIDDLE:name=="right"?SDL_BUTTON_RIGHT:0;valid&=button!=0;
    }else if(action=="wheel"){kind=Kind::Wheel;valid=bool(file>>dx>>dy>>x>>y);}
    else if(action=="cancel"){}
    else if(action=="settings"&&getenv("SSNE_TEST_CROSSHAIR")) {
        std::string value;if(!(file>>value)||(value!="on"&&value!="off"))return;
        g_crosshair_test_settings_request=value=="on"?1:0;g_pointer_accepted_sequence=sequence;return;
    }
    else if(action=="focus_lost" || action=="focus_gained"){g_pointer_fixture_focused=action=="focus_gained";sync_native_mouse_mode();}
    else valid=false;
    if(!valid || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(dx) || !std::isfinite(dy))return;
    g_pointer_accepted_sequence=sequence;
    if(getenv("SSNE_TEST_CROSSHAIR")&&(port::menu_open()||port::mod_menu_open()))
        g_crosshair_test_imgui.push_back({kind,x,y,dx,dy,button});
    else route_native_mouse_pointer(kind,x,y,uint8_t(button),dx,dy);
}
void write_native_mouse_input_state(){
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_NATIVE_MOUSE_COMMAND"))return;
    static uint32_t last=0;const auto now=SDL_GetTicks();if(now-last<25)return;last=now;
    const auto snapshot=port::native_menu_mouse_snapshot();
    std::ofstream file("native-mouse-input-state.json",std::ios::binary|std::ios::trunc);
    file<<"{\"accepted_sequence\":"<<g_pointer_accepted_sequence<<",\"queued_sequence\":"<<g_pointer_event_sequence
        <<",\"image_rect\":["<<g_pointer_image.x-g_pointer_origin_x<<','<<g_pointer_image.y-g_pointer_origin_y<<','<<g_pointer_image.width<<','<<g_pointer_image.height
        <<"],\"image_sequence\":"<<g_pointer_image_sequence<<",\"view_generation\":"<<g_pointer_view_generation
        <<",\"surface_generation\":"<<snapshot.surface_generation<<",\"geometry_generation\":"<<snapshot.geometry_generation
        <<",\"active\":"<<(g_native_pointer_active?"true":"false")<<",\"presentation\":"<<(g_presentation_pointer_active?"true":"false")
        <<",\"player_control\":"<<(ssne::player_control_active()?"true":"false")<<",\"vehicle_control\":"<<(ssne::player_in_vehicle()?"true":"false")
        <<",\"fallback_buttons\":"<<g_presentation_mouse_buttons<<",\"fallback_sequence\":"<<g_presentation_input_sequence<<",\"captured\":"<<(g_mouse_captured?"true":"false")<<"}";
}
