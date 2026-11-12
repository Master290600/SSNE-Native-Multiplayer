// Original RAPTOR beam: eight world-space sprites, separate from its hit ray.
// Included in native_effect.cpp's anonymous namespace after PresentationScope.
std::deque<NativeWeaponBeam> beams,replica_beams;
uint64_t beam_sequence=0;
PPCFunc original_weapon_beam=nullptr;
PPCFunc original_sirian_beam=nullptr,original_sirian_beam_stop=nullptr;
PPCFunc original_sirian_charge_update=nullptr,original_sirian_charge_stop=nullptr;
std::map<uint32_t,uint32_t> replica_beam_frames;
bool beam_event_valid(const NativeWeaponBeam& e,std::string& error){
    if(!e.sequence || e.sequence>uint64_t(INT64_MAX) || e.slot>=8 || (e.weapon!=8 && e.weapon!=9) || e.hand>1 ||
        (e.weapon==8?e.flags!=1:(e.flags!=0 && e.flags!=1 && e.flags!=3 && e.flags!=4 && e.flags!=5))){error="Invalid native beam identity";return false;}
    for(auto value:e.values)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native beam endpoint";return false;}
    if(!(e.flags&1)){for(auto value:e.values)if(value!=0){error="Invalid stopped beam geometry";return false;}return true;}
    if(e.flags==5){for(unsigned i=4;i<e.values.size();++i)if(e.values[i]!=0){error="Unexpected Sirian charge parameter";return false;}return true;}
    if(e.weapon==8){for(unsigned i=6;i<e.values.size();++i)if(e.values[i]!=0){error="Unexpected RAPTOR beam parameter";return false;}}
    else if(e.values[12]<=0){error="Invalid Sirian beam length";return false;}
    double length2=0;for(unsigned axis=0;axis<3;++axis){const auto delta=double(e.values[axis+3])-e.values[axis];length2+=delta*delta;}
    if(length2<1.e-12){error="Native beam has no direction";return false;}return true;
}
uint32_t beam_instance(unsigned slot,unsigned weapon=8,unsigned hand=0){
    if(slot>=native_party_capacity())return 0;const auto actor=mem_read32(native_party_registry()+slot*4);
    if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(actor+0x17C)!=slot)return 0;
    if((weapon!=8 && weapon!=9) || hand>1)return 0;
    const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
    return readable(instance,weapon==8?0x294:0x340) && mem_read32(instance+0xC)==(weapon==8?0x801FC590u:0x801FB908u) && mem_read32(instance+0x1D4)==actor?instance:0;
}
bool bind_beam_instance(uint32_t instance,NativeWeaponBeam& event){
    if(!readable(instance,0x1D8))return false;const auto actor=mem_read32(instance+0x1D4);if(!readable(actor,0x790))return false;
    event.slot=mem_read32(actor+0x17C);for(unsigned hand=0;hand<2;++hand)if(beam_instance(event.slot,event.weapon,hand)==instance){event.hand=hand;return true;}return false;
}
void publish_beam(NativeWeaponBeam event){
    {std::lock_guard lock(effect_mutex);event.sequence=++beam_sequence;retain_native_event(beams,event);}
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-beam-host] sequence="+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" weapon="+std::to_string(event.weapon)+" hand="+std::to_string(event.hand)+" flags="+std::to_string(event.flags));
}
bool beam_proxies_valid(uint32_t instance,bool require_all,std::string& error){
    unsigned count=0;
    for(unsigned i=0;i<8;++i){const auto proxy=mem_read32(instance+0x24C+i*4);if(!proxy)continue;++count;
        const auto table=readable(proxy,0x38)?mem_read32(proxy):0;
        if(!readable(table,0x30) || mem_read32(table+0x2C)!=0x8013E954u || !readable(mem_read32(proxy+4),48)){error="Invalid original beam render proxy";return false;}}
    if((require_all && count!=8) || (count && count!=8)){error="Incomplete original eight-sprite beam";return false;}return true;
}
void weapon_beam_hook(PPCContext& c){
    const auto instance=c.r[3];NativeWeaponBeam event;event.sequence=1;
    bool accept=authoritative && !presenting && (c.lr==0x80060484u || c.lr==0x80060580u) && context_ok(c) && readable(instance,0x294) &&
        mem_read32(instance+0xC)==0x801FC590u && readable(c.r[4],12) && readable(c.r[5],12);
    if(accept)accept=bind_beam_instance(instance,event);
    if(accept){for(unsigned point=0;point<2;++point)for(unsigned axis=0;axis<3;++axis)event.values[point*3+axis]=u2f(mem_read32(c.r[4+point]+axis*4));std::string why;accept=beam_event_valid(event,why);}
    ssne_call_original(c,0x800609F0u,original_weapon_beam);
    if(!accept)return;std::string error;if(!beam_proxies_valid(instance,true,error))fatal(error.c_str());
    // 609F0 sets category2, leaves the native all-view mask untouched, and
    // initializes age/start/direction. The original next visual tick poses it.
    publish_beam(event);
}
void sirian_beam_hook(PPCContext& c){
    const auto instance=c.r[3];NativeWeaponBeam event;event.sequence=1;event.weapon=9;
    bool accept=authoritative && !presenting && context_ok(c) && readable(instance,0x340) && readable(c.r[4],12) && bind_beam_instance(instance,event);
    if(accept){event.flags=mem_read32(instance+0x33C)?3:1;
        for(unsigned axis=0;axis<3;++axis){event.values[axis]=u2f(mem_read32(instance+0x180+axis*4));event.values[3+axis]=u2f(mem_read32(c.r[4]+axis*4));if(event.flags&2){event.values[6+axis]=u2f(mem_read32(instance+0x330+axis*4));event.values[9+axis]=u2f(mem_read32(instance+0x2A0+axis*4));}}
        event.values[12]=float(c.ps0[1]);std::string why;accept=beam_event_valid(event,why);}
    ssne_call_original(c,0x8005A8E8u,original_sirian_beam);if(accept)publish_beam(event);
}
void sirian_beam_stop_hook(PPCContext& c){
    NativeWeaponBeam event;event.sequence=1;event.weapon=9;event.flags=0;
    const bool accept=authoritative && !presenting && c.r[4]==0 && readable(c.r[3],0x340) && mem_read32(c.r[3]+0x23C) && bind_beam_instance(c.r[3],event);
    ssne_call_original(c,0x8005B114u,original_sirian_beam_stop);if(accept)publish_beam(event);
}
void sirian_charge_update_hook(PPCContext& c){
    NativeWeaponBeam event;event.sequence=1;event.weapon=9;event.flags=5;const auto instance=c.r[3];
    bool accept=authoritative && !presenting && readable(instance,0x340) && mem_read32(instance+0x320) && bind_beam_instance(instance,event);
    if(accept){for(unsigned axis=0;axis<3;++axis)event.values[axis]=u2f(mem_read32(instance+0x180+axis*4));event.values[3]=u2f(mem_read32(instance+0x1E4));std::string why;accept=beam_event_valid(event,why);}
    ssne_call_original(c,0x8005BE44u,original_sirian_charge_update);if(accept)publish_beam(event);
}
void sirian_charge_stop_hook(PPCContext& c){
    NativeWeaponBeam event;event.sequence=1;event.weapon=9;event.flags=4;
    const bool accept=authoritative && !presenting && c.r[4]==0 && readable(c.r[3],0x340) && mem_read32(c.r[3]+0x320) && bind_beam_instance(c.r[3],event);
    ssne_call_original(c,0x8005BCE8u,original_sirian_charge_stop);if(accept)publish_beam(event);
}
bool sirian_beam_proxies_valid(uint32_t instance,std::string& error){
    for(unsigned offset:{0x23Cu,0x240u,0x244u,0x2CCu,0x2D0u,0x2D4u,0x2D8u,0x2ECu,0x2F0u,0x2F4u,0x2F8u,0x30Cu,0x310u,0x314u,0x318u,0x320u,0x324u,0x328u,0x32Cu})if(const auto proxy=mem_read32(instance+offset)){
        const auto table=readable(proxy,0x38)?mem_read32(proxy):0;
        if(!readable(table,0x30) || mem_read32(table+0x2C)!=0x8013E954u || !readable(mem_read32(proxy+4),48)){error="Invalid original Sirian render proxy";return false;}}
    return true;
}
void stop_sirian_beam(PPCContext& call,uint32_t instance){
    // Only the visual subset of59F8C/ACFC. Those whole functions also mutate
    // weapon state, stop sounds and destroy physical projectiles.
    const auto secondary=mem_read32(instance+0x33C);mem_write32(instance+0x33C,1);
    call.r[3]=instance;call.r[4]=0;ssne_call(call,0x8005B114u);mem_write32(instance+0x33C,secondary);
    for(unsigned offset:{0x23Cu,0x240u})if(const auto proxy=mem_read32(instance+offset)){
        const auto table=mem_read32(proxy);call.r[3]=proxy+int16_t(mem_read16(table+0x10));call.r[4]=3;ssne_call(call,mem_read32(table+0x14));mem_write32(instance+offset,0);}
    // These three radial owners have no native countdown. The original
    //59F8C/ACFC stop path destroys them explicitly together with the beams.
    for(unsigned offset:{0x24Cu,0x260u,0x274u})if(mem_read32(instance+offset+4) || mem_read32(instance+offset+12)){
        call.r[3]=instance+offset;ssne_call(call,0x8009AC88u);}
}
bool replay_sirian_beam(PPCContext& call,uint32_t instance,const NativeWeaponBeam& event,std::string& error){
    if(!sirian_beam_proxies_valid(instance,error))return false;
    struct TemporaryModel {
        PPCContext& call;uint32_t instance;bool created=false;
        ~TemporaryModel(){if(created){call.r[3]=instance;call.r[4]=0;ssne_call(call,0x800A0FA4u);}}
    } model_scope{call,instance};
    if((event.flags&1) && !mem_read32(instance)){
        // A reliable visual event can arrive after selection has removed its
        // gun model. The original attachment helpers require that model even
        // though the resulting arc/sprite proxies have independent lifetimes.
        // Recreate/pose it only for this call, then release it through the
        // same native visual lifecycle without changing the selected weapon.
        if(!readable(mem_read32(instance+4),4) || mem_read32(mem_read32(instance+0xC)+0xBC)!=0x800A0FA4u){error="Sirian visual model resource unavailable";return false;}
        call.r[3]=instance;call.r[4]=1;ssne_call(call,0x800A0FA4u);model_scope.created=true;
        if(!readable(mem_read32(instance),0x38)){error="Original Sirian visual model was not recreated";return false;}
        call.r[3]=instance;call.ps0[1]=call.ps1[1]=0;ssne_call(call,0x800A032Cu);
    }
    if(event.flags&4){
        if(!(event.flags&1)){for(unsigned helper:{0x8005B7CCu,0x8005BCE8u}){call.r[3]=instance;call.r[4]=0;ssne_call(call,helper);}return true;}
        // Charge arcs use the original countdown as a visual scale, and the
        // four sprites use the gun's already-replicated attachment bones.
        // Temporarily provide the host visual inputs; do not run its timer or
        // physical projectile state machine on the replica.
        std::array<uint32_t,4> saved{};for(unsigned axis=0;axis<3;++axis){saved[axis]=mem_read32(instance+0x180+axis*4);mem_write32(instance+0x180+axis*4,f2u(event.values[axis]));}
        saved[3]=mem_read32(instance+0x1E4);mem_write32(instance+0x1E4,f2u(event.values[3]));
        if(!mem_read32(instance+0x30C)){call.r[3]=instance;call.r[4]=1;ssne_call(call,0x8005B7CCu);}
        call.r[3]=instance;ssne_call(call,0x8005B94Cu);
        call.r[3]=instance;if(!mem_read32(instance+0x320)){call.r[4]=1;ssne_call(call,0x8005BCE8u);}else ssne_call(call,0x8005BE44u);
        for(unsigned axis=0;axis<3;++axis)mem_write32(instance+0x180+axis*4,saved[axis]);mem_write32(instance+0x1E4,saved[3]);
        return sirian_beam_proxies_valid(instance,error);
    }
    if(!event.flags){stop_sirian_beam(call,instance);return true;}
    // A change between the one-view and secondary-view native branches needs
    // the original creation path again, otherwise its conditional allocation
    // is skipped by an already-live main proxy.
    const bool had_secondary=mem_read32(instance+0x244)!=0;
    if(mem_read32(instance+0x23C) && had_secondary!=bool(event.flags&2))stop_sirian_beam(call,instance);
    const std::array<unsigned,3> offsets{0x180,0x330,0x2A0};std::array<uint32_t,10> saved{};
    for(unsigned point=0;point<3;++point)for(unsigned axis=0;axis<3;++axis){saved[point*3+axis]=mem_read32(instance+offsets[point]+axis*4);mem_write32(instance+offsets[point]+axis*4,f2u(event.values[(point==0?0:point==1?6:9)+axis]));}
    saved[9]=mem_read32(instance+0x33C);mem_write32(instance+0x33C,(event.flags&2)?1:0);
    for(unsigned axis=0;axis<3;++axis)mem_write32(call.r[1]+0x100+axis*4,f2u(event.values[3+axis]));
    call.r[3]=instance;call.r[4]=call.r[1]+0x100;call.ps0[1]=call.ps1[1]=event.values[12];ssne_call(call,0x8005A8E8u);
    for(unsigned point=0;point<3;++point)for(unsigned axis=0;axis<3;++axis)mem_write32(instance+offsets[point]+axis*4,saved[point*3+axis]);mem_write32(instance+0x33C,saved[9]);
    return sirian_beam_proxies_valid(instance,error);
}
bool install_native_beam_capture(std::string& error){
    for(const auto [address,signature]:{std::pair{0x800609F0u,0x9421FFB8u},std::pair{0x80060B3Cu,0x9421FEB8u},std::pair{0x8006089Cu,0x9421FFE8u}})
        if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native beam visual lifecycle unavailable";return false;}
    for(const auto [address,signature]:{std::pair{0x8005A8E8u,0x9421FF58u},std::pair{0x8005B114u,0x9421FFD0u},std::pair{0x8005BE44u,0x9421FF28u},std::pair{0x8005BCE8u,0x9421FFD8u}})
        if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native Sirian beam visual lifecycle unavailable";return false;}
    if(!original_weapon_beam){original_weapon_beam=ssne_find_func(0x800609F0u)->func;ssne_install_hook(0x800609F0u,weapon_beam_hook);}
    if(!original_sirian_beam){original_sirian_beam=ssne_find_func(0x8005A8E8u)->func;ssne_install_hook(0x8005A8E8u,sirian_beam_hook);original_sirian_beam_stop=ssne_find_func(0x8005B114u)->func;ssne_install_hook(0x8005B114u,sirian_beam_stop_hook);}
    if(!original_sirian_charge_update){original_sirian_charge_update=ssne_find_func(0x8005BE44u)->func;ssne_install_hook(0x8005BE44u,sirian_charge_update_hook);original_sirian_charge_stop=ssne_find_func(0x8005BCE8u)->func;ssne_install_hook(0x8005BCE8u,sirian_charge_stop_hook);}return true;
}
bool queue_native_beam(PPCContext& c,const NativeWeaponBeam& e,std::string& error){
    if(!context_ok(c) || !beam_event_valid(e,error)){if(error.empty())error="Native beam presentation context unavailable";return false;}
    std::lock_guard lock(effect_mutex);if(replica_beams.size()>=32768){error="Native beam presentation queue exhausted";return false;}replica_beams.push_back(e);return true;
}
bool update_native_beams(PPCContext& c,float seconds,std::string& error){
    if(!context_ok(c) || !std::isfinite(seconds) || seconds<0 || seconds>.1f){error="Invalid native beam timestep";return false;}
    const auto frame=mem_read32(c.r[13]-0x6D80);const auto limit=u2f(mem_read32(c.r[13]-0x7BE8));
    if(!std::isfinite(limit) || limit<=0 || limit>30){error="Invalid original beam lifetime";return false;}
    Scratch scratch(c);auto call=c;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],c.r[1]);call.gqr[6]=0x02050205;call.gqr[7]=0x03050305;PresentationScope presentation;
    for(unsigned slot=0;slot<native_party_capacity();++slot)for(unsigned hand=0;hand<2;++hand)if(const auto instance=beam_instance(slot,8,hand)){
        if(!beam_proxies_valid(instance,false,error))return false;
        if(mem_read32(instance+0x24C) && (!replica_beam_frames.contains(instance) || replica_beam_frames.at(instance)!=frame)){call.r[3]=instance;call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x80060B3Cu);
            if(!beam_proxies_valid(instance,false,error))return false;
            replica_beam_frames[instance]=frame;
            if(!mem_read32(instance+0x24C)){replica_beam_frames.erase(instance);if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-beam-retired] slot="+std::to_string(slot));}}
    }
    // A network burst may contain several shots from one weapon. Preserve each
    // original short beam lifetime before609F0 replaces its eight pointers.
    // This also lets its first zero-scale frame reach the subsequent visible
    // fade frames; merely waiting one frame would still discard its geometry.
    for(unsigned consumed=0;consumed<512;++consumed){NativeWeaponBeam event;uint32_t instance=0;
        {std::lock_guard lock(effect_mutex);auto found=replica_beams.end();std::set<uint32_t> blocked;
            for(auto it=replica_beams.begin();it!=replica_beams.end();++it){const auto candidate=beam_instance(it->slot,it->weapon,it->hand);if(!candidate || blocked.contains(candidate))continue;
                const bool ready=it->weapon==8?!mem_read32(candidate+0x24C):(it->flags&1) || !replica_beam_frames.contains(candidate) || replica_beam_frames.at(candidate)!=frame;
                if(ready){found=it;instance=candidate;break;}blocked.insert(candidate);}
            if(found==replica_beams.end())break;event=*found;replica_beams.erase(found);}
        if(event.weapon==9){if(!replay_sirian_beam(call,instance,event,error))return false;if(event.flags&1)replica_beam_frames[instance]=frame;else replica_beam_frames.erase(instance);}
        else{
        if(!beam_proxies_valid(instance,false,error))return false;
        if(mem_read32(instance+0x24C)){error="Original beam did not retire before retrigger";return false;}
        if(!mem_read32(instance+0x248)){call.r[3]=instance;ssne_call(call,0x8006089Cu);}
        for(unsigned i=0;i<6;++i)mem_write32(call.r[1]+0x100+i*4,f2u(event.values[i]));
        call.r[3]=instance;call.r[4]=call.r[1]+0x100;call.r[5]=call.r[1]+0x10C;ssne_call(call,0x800609F0u);
        if(!beam_proxies_valid(instance,true,error))return false;
        call.r[3]=instance;call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x80060B3Cu);replica_beam_frames[instance]=frame;
        }
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-beam-client] sequence="+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" weapon="+std::to_string(event.weapon)+" hand="+std::to_string(event.hand)+" flags="+std::to_string(event.flags)+" damage_calls="+std::to_string(presentation_damage_calls));
    }
    return true;
}
