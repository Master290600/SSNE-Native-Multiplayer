// Included inside native_effect.cpp's anonymous namespace, after Scratch and
// PresentationScope. PAL shell helpers finish velocity/acceleration and the
// renderer matrix AFTER allocator8006938C; observe only their completed result.
std::deque<NativeWeaponCasing> casings,replica_casings;
uint64_t casing_sequence=0;
PPCFunc original_casing_allocator=nullptr;
std::array<PPCFunc,3> original_casing_helpers{};
inline constexpr std::array<uint32_t,3> casing_helpers{0x8005696Cu,0x80061CF8u,0x800587D4u};
inline constexpr std::array<uint32_t,3> casing_allocator_returns{0x80056990u,0x80061D1Cu,0x800587F8u};
inline constexpr std::array<unsigned,3> casing_weapons{1,2,4};
inline constexpr std::array<uint32_t,3> casing_weapon_tables{0x801FB280u,0x801FC858u,0x801FB598u};
struct CasingCapture {unsigned helper=0,slot=0;uint32_t instance=0,node=0;};
thread_local CasingCapture* active_casing_capture=nullptr;

bool casing_event_valid(const NativeWeaponCasing& e,std::string& error){
    if(!e.sequence || e.sequence>uint64_t(INT64_MAX) || e.slot>=8 ||
       (e.weapon!=1 && e.weapon!=2 && e.weapon!=4) || e.type!=0){error="Invalid native casing identity";return false;}
    for(auto f:e.values)if(!std::isfinite(f) || std::abs(f)>1000000.f){error="Invalid native casing transform/motion";return false;}
    if(e.values[18]<0 || e.values[18]>30){error="Invalid native casing age";return false;}
    return true;
}
bool casing_proxy(uint32_t node,uint32_t& proxy,uint32_t& matrix){
    if(!readable(node,0x2C))return false;proxy=mem_read32(node);
    if(!readable(proxy,0x38))return false;const auto table=mem_read32(proxy);matrix=mem_read32(proxy+4);
    return readable(table,0x30) && mem_read32(table+0x2C)==0x8013E954u && readable(matrix,48);
}
bool casing_lists_valid(const PPCContext& c,std::string& error){
    std::set<uint32_t> all;
    for(unsigned list=0;list<2;++list){const auto offset=list?0x6078u:0x6080u;
        auto node=mem_read32(c.r[13]-offset);uint32_t previous=0,count=0;
        while(node){if(!readable(node,0x2C) || !all.insert(node).second || ++count>32 || mem_read32(node+0x28)!=previous){error="Invalid native casing lifetime list";return false;}
            if(!list){uint32_t proxy,matrix;if(!casing_proxy(node,proxy,matrix)){error="Invalid native casing render proxy";return false;}}
            previous=node;node=mem_read32(node+0x24);
        }
        if(count!=mem_read32(c.r[13]-offset+4)){error="Native casing count differs from list";return false;}
    }
    if(all.size()>32){error="Native casing pool exceeds original capacity";return false;}return true;
}
void casing_allocator_hook(PPCContext& c){
    auto* capture=active_casing_capture;
    const bool accepted=capture && authoritative && !presenting && c.r[3]==0 && c.lr==casing_allocator_returns[capture->helper];
    ssne_call_original(c,0x8006938Cu,original_casing_allocator);
    // A exhausted original pool returns zero; never infer creation from the
    // unchanged active-list head, which would duplicate an earlier shell.
    if(accepted)capture->node=c.r[3];
}
template<unsigned Index>void casing_helper_hook(PPCContext& c){
    const auto instance=c.r[3];CasingCapture capture;capture.helper=Index;capture.instance=instance;
    bool accept=authoritative && !presenting && context_ok(c) && readable(instance,0x228) &&
        mem_read32(instance+0xC)==casing_weapon_tables[Index] && mem_read32(instance)!=0;
    if(accept){const auto actor=mem_read32(instance+0x1D4);accept=readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u;
        if(accept){capture.slot=mem_read32(actor+0x17C);accept=capture.slot<native_party_capacity() && mem_read32(native_party_registry()+capture.slot*4)==actor &&
            (mem_read32(actor+0x4BC+casing_weapons[Index]*24)==instance || mem_read32(actor+0x4C0+casing_weapons[Index]*24)==instance);}}
    struct Restore {CasingCapture* previous;~Restore(){active_casing_capture=previous;}} restore{active_casing_capture};
    active_casing_capture=accept?&capture:nullptr;
    ssne_call_original(c,casing_helpers[Index],original_casing_helpers[Index]);
    uint32_t proxy=0,matrix=0;
    if(!accept || !capture.node || !casing_proxy(capture.node,proxy,matrix))return;
    NativeWeaponCasing event;event.sequence=1;event.slot=capture.slot;event.weapon=casing_weapons[Index];event.mask=mem_read32(proxy+0x30);
    for(unsigned i=0;i<12;++i)event.values[i]=u2f(mem_read32(matrix+i*4));
    for(unsigned i=0;i<7;++i)event.values[12+i]=u2f(mem_read32(capture.node+4+i*4));
    std::string error;if(!casing_event_valid(event,error))fatal(error.c_str());
    {std::lock_guard lock(effect_mutex);event.sequence=++casing_sequence;retain_native_event(casings,event);}
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-casing-host] sequence="+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" weapon="+std::to_string(event.weapon)+" node="+std::to_string(capture.node));
}
bool install_native_casing_capture(std::string& error){
    for(const auto [address,signature]:{std::pair{0x8005696Cu,0x9421FF20u},std::pair{0x80061CF8u,0x9421FF18u},std::pair{0x800587D4u,0x9421FF20u},std::pair{0x8006938Cu,0x9421FFD8u},std::pair{0x80069284u,0x9421FFE0u}})
        if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native casing lifecycle unavailable";return false;}
    if(!original_casing_allocator){original_casing_allocator=ssne_find_func(0x8006938Cu)->func;ssne_install_hook(0x8006938Cu,casing_allocator_hook);}
    const std::array<PPCFunc,3> hooks{casing_helper_hook<0>,casing_helper_hook<1>,casing_helper_hook<2>};
    for(unsigned i=0;i<3;++i)if(!original_casing_helpers[i]){original_casing_helpers[i]=ssne_find_func(casing_helpers[i])->func;ssne_install_hook(casing_helpers[i],hooks[i]);}
    return true;
}
bool queue_native_casing(PPCContext& context,const NativeWeaponCasing& event,std::string& error){
    if(!context_ok(context) || !casing_event_valid(event,error)){if(error.empty())error="Native casing presentation context unavailable";return false;}
    std::lock_guard lock(effect_mutex);if(replica_casings.size()>=32768){error="Native casing presentation queue exhausted";return false;}
    replica_casings.push_back(event);return true;
}
bool update_native_casings(PPCContext& context,float seconds,std::string& error){
    if(!context_ok(context) || !std::isfinite(seconds) || seconds<0 || seconds>.1f){error="Invalid native casing timestep";return false;}
    if(!casing_lists_valid(context,error))return false;
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    PresentationScope presentation;const auto before=mem_read32(call.r[13]-0x607C);
    // Original visual manager performs no collision queries or damage dispatch:
    // 80068E44 integrates position/velocity/acceleration, then80069284 recycles
    // expired nodes through80068B70. Keep the same native32-node pool.
    call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x80069284u);
    if(!casing_lists_valid(call,error))return false;
    const auto after=mem_read32(call.r[13]-0x607C);
    if(before!=after && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-casing-retired] count="+std::to_string(before-after)+" active="+std::to_string(after));
    while(mem_read32(call.r[13]-0x6078)){
        NativeWeaponCasing event;{std::lock_guard lock(effect_mutex);if(replica_casings.empty())break;event=replica_casings.front();}
        call.r[3]=event.type;ssne_call(call,0x8006938Cu);const auto node=call.r[3];uint32_t proxy=0,matrix=0;
        if(!node || !casing_proxy(node,proxy,matrix)){error="Original casing allocator did not create a render proxy";return false;}
        for(unsigned i=0;i<12;++i)mem_write32(matrix+i*4,f2u(event.values[i]));
        for(unsigned i=0;i<7;++i)mem_write32(node+4+i*4,f2u(event.values[12+i]));
        mem_write32(proxy+0x30,event.mask);
        {std::lock_guard lock(effect_mutex);replica_casings.pop_front();}
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-weapon-casing-client] sequence="+std::to_string(event.sequence)+" slot="+std::to_string(event.slot)+" weapon="+std::to_string(event.weapon)+" node="+std::to_string(node)+" damage_calls="+std::to_string(presentation_damage_calls));
    }
    return casing_lists_valid(call,error);
}
