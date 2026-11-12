// Original PAL credits are a linked list of native glyph objects. Their scroll
// advances once per update, independently of dt. Replay only this presentation
// routine to the authority's step; never replay gameplay or copy guest pointers.
namespace {
bool credits_installed=false,credits_authority=false,credits_active=false;
uint64_t credits_step=0,credits_target=0,credits_expected_hash=0,credits_logged=~uint64_t(0);
uint64_t credits_exit_step=0;
bool credits_skip_requested=false;
PPCFunc credits_init_original=nullptr,credits_update_original=nullptr;
bool credits_readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;}
uint64_t credits_digest(uint32_t object){
    uint64_t h=14695981039346656037ull;
    auto add=[&](uint32_t v){for(unsigned i=0;i<4;++i){h^=uint8_t(v>>(24-i*8));h*=1099511628211ull;}};
    for(unsigned offset:{8u,12u,16u})add(mem_read32(object+offset));
    add(mem_read32(0x802DB890)); // logo visibility toggled by the original marker line
    auto fade=mem_read32(object+0x18);if(!credits_readable(fade,8))throw std::runtime_error("Invalid credits fade");
    add(mem_read32(fade));add(mem_read32(fade+4));
    std::set<uint32_t> visited;
    for(auto line=mem_read32(object+0x1c);line;line=mem_read32(line+8)){
        if(!credits_readable(line,0x1c) || visited.size()>=512 || !visited.insert(line).second)throw std::runtime_error("Invalid credits line chain");
        add(mem_read32(line+0x10));
        const auto transform=mem_read32(line+0x14),glyph=mem_read32(line+0x18);
        if(!credits_readable(transform,0x1c) || !credits_readable(glyph,0x24))throw std::runtime_error("Invalid credits glyph");
        const auto position=mem_read32(transform);
        if(!credits_readable(position,16))throw std::runtime_error("Invalid credits position");
        add(mem_read32(position+0xc)); // actual scrolling Y coordinate
        add(mem_read32(glyph+0x1c)); // original text height
    }
    add(uint32_t(visited.size()));return h;
}
void credits_init(PPCContext& ctx){
    const auto object=ctx.r[3];ssne_call_original(ctx,0x80164400,credits_init_original);
    credits_step=0;credits_logged=~uint64_t(0);credits_active=true;
    if(credits_authority){credits_exit_step=0;credits_skip_requested=false;}
    // Native init sets only the target of this freshly allocated fade pair.
    // Start its value explicitly, independent of previously freed heap bytes.
    const auto fade=mem_read32(object+0x18);
    if(!credits_readable(fade,8))throw std::runtime_error("Invalid credits initialization");
    mem_write32(fade,0);
}
void credits_update(PPCContext& ctx){
    const auto object=ctx.r[3];
    if(!credits_active || object!=0x80292560u || mem_read32(object+4)!=0x80221a18u)throw std::runtime_error("Unexpected native credits scene");
    const auto target=credits_authority?credits_step+1:credits_target;
    const auto budget=credits_authority?1u:120u;
    const auto start=std::chrono::steady_clock::now();
    unsigned advanced=0;
    while(credits_step<target && advanced<budget){
        auto call=ctx;call.ps0[1]=call.ps1[1]=double(1.0f/60.0f);
        const auto input=mem_read32(0x802ABA9C)+0x10;
        if(!credits_readable(input,0x678))throw std::runtime_error("Invalid credits input bank");
        const auto a=mem_read32(input+0x444),b=mem_read32(input+0x674);
        // Local confirms are consumed by the authenticated shared vote.
        mem_write32(input+0x444,(credits_authority?credits_skip_requested:credits_exit_step==credits_step+1)?1:0);mem_write32(input+0x674,0);
        try{ssne_call_original(call,0x80164B60,credits_update_original);}
        catch(...){mem_write32(input+0x444,a);mem_write32(input+0x674,b);throw;}
        mem_write32(input+0x444,a);mem_write32(input+0x674,b);
        ++credits_step;++advanced;
        if(credits_authority && !credits_exit_step && mem_read32(0x802DB7EC)==3)credits_exit_step=credits_step;
        // Bound catch-up work, including joins late in a long credits sequence.
        if(std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(3))break;
    }
    if(std::getenv("SSNE_TEST_HIDDEN") && credits_step!=credits_logged &&
       (credits_authority?credits_step%60==0:credits_step==target)){
        credits_logged=credits_step;const auto hash=credits_digest(object);
        ssne::log_message("[coop-credits-playhead] role="+std::string(credits_authority?"host":"client")+" step="+std::to_string(credits_step)+" target="+std::to_string(target)+" advanced="+std::to_string(advanced)+" digest="+std::to_string(hash)+" expected="+std::to_string(credits_expected_hash)+" match="+std::to_string(credits_authority || hash==credits_expected_hash));
    }
}
}
bool install_native_credits(bool authority,std::string& error){
    if(credits_installed){if(credits_authority!=authority){error="Native credits role changed";return false;}return true;}
    for(auto [address,expected]:{std::pair{0x80164400u,0x2b643315afb51678ull},std::pair{0x80164B60u,0xccb5c3ee58ca9da6ull}}){
        uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<32;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}
        if(mem_read32(0x80000000)!=0x47334250u || hash!=expected || !ssne_find_func(address)){error="Native credits revision differs";return false;}
    }
    credits_authority=authority;credits_init_original=ssne_find_func(0x80164400)->func;credits_update_original=ssne_find_func(0x80164B60)->func;
    ssne_install_hook(0x80164400,credits_init);ssne_install_hook(0x80164B60,credits_update);credits_installed=true;return true;
}
void publish_native_credits(Properties& mission){
    if(!credits_installed || !credits_authority || !credits_active)return;
    mission["Session.Credits.Step"]=int64_t(credits_step);
    mission["Session.Credits.ExitStep"]=int64_t(credits_exit_step);
    mission["Session.Credits.Digest"]=std::to_string(credits_digest(0x80292560));
}
bool receive_native_credits(const Properties& mission,std::string& error){
    const auto step=mission.find("Session.Credits.Step"),hash=mission.find("Session.Credits.Digest"),ending=mission.find("Session.Credits.ExitStep");
    if(step==mission.end() || hash==mission.end() || ending==mission.end() || !std::holds_alternative<int64_t>(ending->second) || !std::holds_alternative<int64_t>(step->second) || !std::holds_alternative<std::string>(hash->second)){
        error="Host credits timeline unavailable; use the same multiplayer build";return false;
    }
    const auto value=std::get<int64_t>(step->second);const auto& text=std::get<std::string>(hash->second);
    uint64_t digest=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),digest);
    const auto end_step=std::get<int64_t>(ending->second);
    if(value<0 || value>3600000 || end_step<0 || end_step>value || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()){error="Invalid host credits timeline";return false;}
    credits_target=uint64_t(value);credits_exit_step=uint64_t(end_step);credits_expected_hash=digest;return true;
}
void request_native_credits_skip(){if(credits_authority && credits_active)credits_skip_requested=true;}
bool native_credits_finishing(){return credits_active && credits_exit_step && credits_step>=credits_exit_step;}
