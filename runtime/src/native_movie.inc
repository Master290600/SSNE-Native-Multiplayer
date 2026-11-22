// Included inside replica_simulation's private namespace. Original local Bink
// playback supplies its own audio/video clock. Host progress is used only
// during initial late-join catch-up; identity and lifetime still follow the host.
uint32_t movie_scene=0;
NativeMovieState movie_state;
std::atomic<bool> movie_vote_gate{false};
uint64_t movie_skip_pending=0;
uint64_t applied_movie_sequence=0;
struct MovieCatchup {
    uint32_t target=0;
    std::chrono::steady_clock::time_point until{};
};
MovieCatchup movie_catchup;
PPCFunc movie_start_original=nullptr,movie_stop_original=nullptr,movie_update_original=nullptr,movie_playing_original=nullptr;
bool movie_asset_valid(const std::string& name){return !name.empty() && name.size()<=96 && name.find("..") == std::string::npos && std::all_of(name.begin(),name.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';});}
struct MovieScratch {uint32_t address;std::array<uint8_t,2048> bytes;MovieScratch(PPCContext& c):address(c.r[1]-2048){for(unsigned i=0;i<bytes.size();++i)bytes[i]=mem_read8(address+i);}~MovieScratch(){for(unsigned i=0;i<bytes.size();++i)mem_write8(address+i,bytes[i]);}};
void movie_start_hook(PPCContext& ctx){
    movie_scene=ctx.r[3];std::string name;
    {MovieScratch scratch(ctx);auto call=ctx;call.r[1]-=0x200;call.r[3]=mem_read32(0x802D9058u);ssne_call(call,0x80159F70u);
    for(unsigned i=0;i<97 && readable(call.r[3]+i,1);++i){auto c=mem_read8(call.r[3]+i);if(!c)break;name+=char(c);}}
    ssne_call_original(ctx,0x801677B4u,movie_start_original);
    if(!replica){++movie_state.sequence;movie_state.asset=name;movie_state.active=true;log_message("[coop-movie-host-start] sequence="+std::to_string(movie_state.sequence)+" asset="+name);}
}
void movie_stop_hook(PPCContext& ctx){
    // The original scene update uses this same stop routine for a controller
    // press and natural EOF. Gate only that caller while Bink is still playing;
    // scene destruction and normal video completion must remain possible.
    const bool scene_stop=ctx.lr==0x8016692Cu && ctx.r[3]==movie_scene && native_movie_presenting();
    // A replica can finish its local (possibly shorter localized) clip first.
    // Keep the last image until the host ends it. Actual scene cleanup is a
    // different caller and must still release the resource.
    if(scene_stop && replica && movie_state.active)return;
    if(scene_stop && movie_vote_gate.load() && !replica && movie_skip_pending!=movie_state.sequence){
        const auto resource=mem_read32(movie_scene+0x80),bink=mem_read32(resource+0x10);
        if(!readable(bink,0x10) || mem_read32(bink+0xC)!=mem_read32(bink+8)){
            static uint64_t logged=0;if(logged!=movie_state.sequence){logged=movie_state.sequence;log_message("[coop-movie-skip-gated] sequence="+std::to_string(movie_state.sequence));}
            return;
        }
    }
    const bool approved=movie_skip_pending==movie_state.sequence && movie_skip_pending;
    ssne_call_original(ctx,0x80167A2Cu,movie_stop_original);movie_state.active=false;movie_skip_pending=0;movie_catchup={};
    if(approved)log_message("[coop-movie-skip-approved] sequence="+std::to_string(movie_state.sequence));
    log_message("[coop-movie-stop] sequence="+std::to_string(movie_state.sequence));
}
void movie_playing_hook(PPCContext& ctx){
    // Let the original scene update perform its normal close with its normal
    // interrupt context. Bink destruction can wait for guest I/O; calling it
    // inside the network frame observer (interrupts disabled) can deadlock.
    if(!replica && movie_vote_gate.load() && movie_skip_pending && movie_skip_pending==movie_state.sequence && ctx.lr==0x80166914u && native_movie_presenting() && ctx.r[3]==mem_read32(movie_scene+0x80)){ctx.r[3]=0;return;}
    ssne_call_original(ctx,0x801335D0u,movie_playing_original);
}
void movie_update_hook(PPCContext& ctx){
    if(replica && native_movie_presenting() && ctx.r[3]==mem_read32(movie_scene+0x80)){
        const auto resource=ctx.r[3],bink=mem_read32(resource+0x10);
        if(!readable(bink,0x20)){ctx.r[3]=0;return;}
        const uint32_t total=mem_read32(bink+8);
        const uint32_t frame=mem_read32(bink+0xC);
        if(!total || frame>=total){ctx.r[3]=0;return;}
        // BinkWait paces the local video and services its audio, even if the
        // most recent host snapshot is still at the start of the clip.
        ssne_call_original(ctx,0x80133160u,movie_update_original);
        if(!movie_catchup.target || mem_read32(resource+0x10)!=bink)return;
        const uint32_t after=mem_read32(bink+0xC);
        const uint32_t target=std::min(total,movie_catchup.target);
        const uint32_t rate=mem_read32(bink+0x14), divisor=mem_read32(bink+0x18);
        const double fps=rate && divisor ? std::clamp(double(rate)/divisor,1.,240.) : 30.;
        const auto tolerance=uint32_t(std::ceil(.5*fps));
        const auto now=std::chrono::steady_clock::now();
        if(now>=movie_catchup.until || target<=after || target-after<=tolerance){
            movie_catchup={};
            return;
        }
        if(after<=frame)return;

        // Extra decoding is limited to the first host position and the startup
        // window. A delayed or bursty host stream cannot make playback race.
        const unsigned extra=target-after>uint32_t(std::ceil(fps)) ? 3u : 1u;
        const uint32_t limit=target-tolerance;
        for(unsigned i=0;i<extra && mem_read32(resource+0x10)==bink && mem_read32(bink+0xC)<limit;++i){
            auto call=ctx;call.r[3]=resource;ssne_call(call,0x80132EF8u);
            if(std::chrono::steady_clock::now()-now>=std::chrono::milliseconds(4))break;
        }
        return;
    }
    ssne_call_original(ctx,0x80133160u,movie_update_original);
}
void install_movie_hooks(){
    const auto* start=ssne_find_func(0x801677B4u);const auto* stop=ssne_find_func(0x80167A2Cu);const auto* update=ssne_find_func(0x80133160u);const auto* playing=ssne_find_func(0x801335D0u);
    if(!start||!stop||!update||!playing||mem_read32(start->address)!=0x9421FEE0u||mem_read32(update->address)!=0x9421FFE8u||mem_read32(playing->address)!=0x81630010u)fatal("Native movie lifecycle unavailable");
    observe_native_entities([](const NativeEntityNotice& notice){if(notice.event==NativeEntityNotice::Event::LevelExit){movie_scene=0;movie_catchup={};applied_movie_sequence=movie_skip_pending=0;movie_state.active=false;movie_state.asset.clear();}});
    movie_start_original=start->func;movie_stop_original=stop->func;movie_update_original=update->func;movie_playing_original=playing->func;
    ssne_install_hook(start->address,movie_start_hook);ssne_install_hook(stop->address,movie_stop_hook);ssne_install_hook(update->address,movie_update_hook);ssne_install_hook(playing->address,movie_playing_hook);
}
bool apply_movie(PPCContext& ctx,const NativeMovieState& desired,std::string& error){
    if(!desired.active && !native_movie_presenting()){movie_state=desired;movie_catchup={};return true;}
    if(!replica || ctx.r[13]!=0x802E22A0u || !readable(ctx.r[1]-2048,2048) || !readable(movie_scene,0x94) || (desired.active && (!desired.sequence || !movie_asset_valid(desired.asset) || desired.frame>1000000))){error="Invalid native movie presentation";return false;}
    if(native_movie_presenting() && (!desired.active || applied_movie_sequence!=desired.sequence)){
        movie_state.active=false;
        MovieScratch scratch(ctx);auto call=ctx;call.r[1]-=0x200;mem_write32(call.r[1],ctx.r[1]);
        call.r[3]=movie_scene;ssne_call(call,0x80167A2Cu);
    }
    if(desired.active && (!native_movie_presenting() || applied_movie_sequence!=desired.sequence)){
        MovieScratch scratch(ctx);auto call=ctx;call.r[1]-=0x200;mem_write32(call.r[1],ctx.r[1]);
        // A client never runs level scripts; its movie queue must be empty.
        if(mem_read32(ctx.r[13]-0x54CC)){error="Unexpected pending replica movie";return false;}
        for(unsigned i=0;i<=desired.asset.size();++i)mem_write8(call.r[1]+0x80+i,i<desired.asset.size()?desired.asset[i]:0);
        call.r[3]=call.r[1]+0x80;ssne_call(call,0x80167714u);
        if(mem_read32(ctx.r[13]-0x54CC)!=1){error="Native movie enqueue failed";return false;}
        call.r[3]=movie_scene;ssne_call(call,0x801677B4u);
        if(!native_movie_presenting()){error="Native movie did not start";return false;}
        applied_movie_sequence=desired.sequence;
        movie_catchup={desired.frame,std::chrono::steady_clock::now()+std::chrono::seconds(2)};
        log_message("[coop-movie-client-start] sequence="+std::to_string(desired.sequence)+" asset="+desired.asset);
    }
    movie_state=desired;
    if(getenv("SSNE_TEST_HIDDEN") && desired.active){static unsigned samples=0;if(++samples%30==0){const auto now=native_movie_state();log_message("[coop-movie-position] target="+std::to_string(desired.frame)+" local="+std::to_string(now.frame)+" sequence="+std::to_string(desired.sequence));}}
    return true;
}
