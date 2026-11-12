// Original additional chainsaw / power-gun surface marks. Bullet-impact marks
// call the same factory at800EDC54 and are already replayed by their own event.
std::deque<NativeWeaponSurface> surfaces,replica_surfaces;
uint64_t surface_sequence=0;
PPCFunc original_weapon_surface=nullptr;
bool surface_event_valid(const NativeWeaponSurface& e,std::string& error){
    if(!e.sequence || e.sequence>uint64_t(INT64_MAX) || !((e.weapon==0 && e.kind==7) || (e.weapon==9 && e.kind==3))){error="Invalid native weapon surface identity";return false;}
    for(auto value:e.values)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native weapon surface coefficient";return false;}
    double normal=0;for(unsigned i=3;i<6;++i)normal+=double(e.values[i])*e.values[i];
    if(normal<1.e-12 || normal>4 || e.values[6]<=0 || e.values[6]>100){error="Invalid native weapon surface normal/scale";return false;}return true;
}
void weapon_surface_hook(PPCContext& c){
    NativeWeaponSurface event;event.sequence=1;const bool chainsaw=c.lr==0x800539D8u,power=c.lr==0x80059F0Cu;
    bool accept=authoritative && !presenting && (chainsaw || power) && context_ok(c) && readable(c.r[31],0x1D8) &&
        mem_read32(c.r[31]+0xC)==(chainsaw?0x801FAB40u:0x801FB908u) && readable(c.r[3],12) && readable(c.r[4],12);
    const auto direct=context_ok(c)?mem_read32(c.r[13]-0x5554):0,queued=context_ok(c)?mem_read32(c.r[13]-0x5558):0;
    if(accept){event.weapon=chainsaw?0:9;event.kind=c.r[5];for(unsigned i=0;i<3;++i){event.values[i]=u2f(mem_read32(c.r[3]+i*4));event.values[3+i]=u2f(mem_read32(c.r[4]+i*4));}event.values[6]=float(c.ps0[1]);std::string why;accept=surface_event_valid(event,why);}
    ssne_call_original(c,0x800CCFD8u,original_weapon_surface);
    if(accept && (mem_read32(c.r[13]-0x5554)==direct+1 || mem_read32(c.r[13]-0x5558)==queued+1)){
        {std::lock_guard lock(effect_mutex);event.sequence=++surface_sequence;retain_native_event(surfaces,event);}
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-surface-host] sequence="+std::to_string(event.sequence)+" weapon="+std::to_string(event.weapon)+" kind="+std::to_string(event.kind));
    }
}
bool install_native_surface_capture(std::string& error){
    if(!ssne_find_func(0x800CCFD8u) || mem_read32(0x800CCFD8u)!=0x9421FFE0u || !ssne_find_func(0x800CCF38u)){error="Native weapon surface lifecycle unavailable";return false;}
    if(!original_weapon_surface){original_weapon_surface=ssne_find_func(0x800CCFD8u)->func;ssne_install_hook(0x800CCFD8u,weapon_surface_hook);}return true;
}
bool queue_native_surface(PPCContext& c,const NativeWeaponSurface& e,std::string& error){
    if(!context_ok(c) || !surface_event_valid(e,error)){if(error.empty())error="Native surface presentation context unavailable";return false;}
    std::lock_guard lock(effect_mutex);if(replica_surfaces.size()>=32768){error="Native surface presentation queue exhausted";return false;}replica_surfaces.push_back(e);return true;
}
bool update_native_surfaces(PPCContext& c,std::string& error){
    if(!context_ok(c)){error="Native surface update context unavailable";return false;}
    Scratch scratch(c);auto call=c;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],c.r[1]);PresentationScope presentation;
    // The original accepts four direct marks plus sixteen pending marks. Keep
    // bursts in our bounded queue until its original800CCF38 updater has room.
    for(unsigned i=0;i<20;++i){const auto direct=mem_read32(c.r[13]-0x5554),queued=mem_read32(c.r[13]-0x5558);
        if(direct>4 || queued>16){error="Invalid native surface factory queue";return false;}if(direct>=4 && queued>=16)break;
        NativeWeaponSurface event;{std::lock_guard lock(effect_mutex);if(replica_surfaces.empty())break;event=replica_surfaces.front();}
        for(unsigned j=0;j<6;++j)mem_write32(call.r[1]+0x100+j*4,f2u(event.values[j]));
        call.r[3]=call.r[1]+0x100;call.r[4]=call.r[1]+0x10C;call.r[5]=event.kind;call.ps0[1]=call.ps1[1]=event.values[6];ssne_call(call,0x800CCFD8u);
        if(mem_read32(c.r[13]-0x5554)!=direct+1 && mem_read32(c.r[13]-0x5558)!=queued+1){error="Original surface factory rejected available queue capacity";return false;}
        {std::lock_guard lock(effect_mutex);replica_surfaces.pop_front();}
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-surface-client] sequence="+std::to_string(event.sequence)+" weapon="+std::to_string(event.weapon)+" kind="+std::to_string(event.kind)+" damage_calls="+std::to_string(presentation_damage_calls));
    }return true;
}
