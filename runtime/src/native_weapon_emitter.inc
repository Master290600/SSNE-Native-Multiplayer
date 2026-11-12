// Original flame factory state and the two visual subsections of 800D810C.
// Included inside the native effect anonymous namespace.
std::deque<NativeWeaponEmitter> emitters;
uint64_t emitter_sequence=0;
std::array<PPCFunc,3> original_emitters{};
constexpr std::array<uint32_t,3> emitter_factories{0x800D902Cu,0x800D9490u,0x800D94E8u};
constexpr std::array<unsigned,17> emitter_offsets{4,8,12,16,20,24,28,32,36,40,44,48,64,68,72,76,80};
bool emitter_event_valid(const NativeWeaponEmitter& e,std::string& error){
    if(e.tags[0]>2 || e.tags[1]>=12 || e.tags[3]>1 || e.tags[4]>1){error="Invalid native flame particle tags";return false;}
    for(auto value:e.values)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid native flame particle value";return false;}
    if(e.values[12]<0 || e.values[12]>10000 || e.values[13]<0 || e.values[14]<=0 || e.values[14]>60 || e.values[13]>e.values[14] || std::abs(e.values[16])>1){error="Invalid native flame particle lifetime/scale";return false;}
    return true;
}
bool emitter_list_valid(uint32_t head,uint32_t count,unsigned size,unsigned next,std::string& error){
    std::set<uint32_t> seen;uint32_t previous=0,node=mem_read32(head);
    while(node){if(!readable(node,size) || !seen.insert(node).second || seen.size()>32768 || mem_read32(node+next+4)!=previous){error="Invalid native flame visual list";return false;}previous=node;node=mem_read32(node+next);}
    if(seen.size()!=mem_read32(count)){error="Native flame visual count differs";return false;}return true;
}
bool emitter_lists_valid(const PPCContext& c,std::string& error){
    return emitter_list_valid(c.r[13]-0x5F9C,c.r[13]-0x5F98,0x68,0x60,error) &&
           emitter_list_valid(c.r[13]-0x5FA4,c.r[13]-0x5FA0,0x68,0x60,error) &&
           emitter_list_valid(0x8028BD5C,0x8028BD60,0x14,0xC,error) &&
           emitter_list_valid(0x8028BD54,0x8028BD58,0x14,0xC,error);
}
template<unsigned Mode>void emitter_factory_hook(PPCContext& c){
    const bool caller=Mode==0?(c.lr==0x80054F24u || c.lr==0x80054F48u):
                      Mode==1?(c.lr==0x80054F94u || c.lr==0x80054FB4u):
                              (c.lr==0x80055000u || c.lr==0x80055020u);
    bool capture=authoritative && !presenting && caller && c.r[3]<12 && readable(c.r[31],0x248);
    NativeWeaponEmitter e;e.tags={Mode,c.r[3],c.r[7],0,0};
    if(capture){const auto actor=mem_read32(c.r[31]+0x1D4);capture=readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u &&
        (mem_read32(actor+0x4BC+7*24)==c.r[31] || mem_read32(actor+0x4C0+7*24)==c.r[31]);}
    const auto before=capture?mem_read32(c.r[13]-0x5F98):0;
    ssne_call_original(c,emitter_factories[Mode],original_emitters[Mode]);
    if(!capture || c.r[3]!=1 || mem_read32(c.r[13]-0x5F98)!=before+1)return;
    const auto node=mem_read32(c.r[13]-0x5F9C);
    if(!readable(node,0x68) || mem_read32(node)!=Mode || mem_read32(node+0x54)!=e.tags[1])return;
    e.tags[4]=mem_read32(node+0x5C);e.tags[3]=e.tags[4]?mem_read32(node+0x34):0;
    for(unsigned i=0;i<e.values.size();++i)e.values[i]=(!e.tags[4] && i>=6 && i<12)?0.f:u2f(mem_read32(node+emitter_offsets[i]));
    std::string error;if(!emitter_event_valid(e,error)){log_message("[coop-flame-rejected] "+error);return;}
    std::lock_guard lock(effect_mutex);e.sequence=++emitter_sequence;retain_native_event(emitters,e);
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-flame-host] sequence="+std::to_string(e.sequence)+" mode="+std::to_string(Mode)+" collision="+std::to_string(e.tags[4])+" active="+std::to_string(before+1));
}
bool install_native_emitter_capture(std::string& error){
    for(const auto [address,signature]:{std::pair{0x800D902Cu,0x9421FEE8u},std::pair{0x800D9490u,0x9421FFF0u},std::pair{0x800D94E8u,0x9421FFF0u},std::pair{0x800D98B8u,0x9421FFF0u},std::pair{0x800D9A8Cu,0x9421FFF8u},std::pair{0x800DB040u,0x9421FFD0u}})
        if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native flame visual lifecycle unavailable";return false;}
    if(!original_emitters[0]){for(unsigned i=0;i<3;++i)original_emitters[i]=ssne_find_func(emitter_factories[i])->func;
        ssne_install_hook(emitter_factories[0],emitter_factory_hook<0>);ssne_install_hook(emitter_factories[1],emitter_factory_hook<1>);ssne_install_hook(emitter_factories[2],emitter_factory_hook<2>);}
    return true;
}
bool replay_native_emitter(PPCContext& context,const NativeWeaponEmitter& e,std::string& error){
    if(!context_ok(context) || !emitter_event_valid(e,error) || !emitter_lists_valid(context,error)){if(error.empty())error="Native flame context unavailable";return false;}
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    for(unsigned i=0;i<6;++i)mem_write32(call.r[1]+0x80+i*4,f2u(e.values[i]));
    call.r[3]=e.tags[1];call.r[4]=call.r[1]+0x80;call.r[5]=call.r[1]+0x8C;
    // r6=0 bypasses the original collision query and 8007E9B8 gameplay hit.
    // Apply the finalized host collision fields after native visual allocation.
    call.r[6]=0;call.r[7]=e.tags[2];call.r[8]=0x80;
    call.ps0[1]=call.ps1[1]=e.values[14];call.ps0[2]=call.ps1[2]=e.values[12];
    const auto before=mem_read32(context.r[13]-0x5F98);
    PresentationScope presentation;ssne_call(call,emitter_factories[e.tags[0]]);
    const auto node=mem_read32(context.r[13]-0x5F9C);
    if(call.r[3]!=1 || mem_read32(context.r[13]-0x5F98)!=before+1 || !readable(node,0x68)){
        error="Original flame factory did not allocate particle";return false;
    }
    for(unsigned i=0;i<e.values.size();++i)mem_write32(node+emitter_offsets[i],f2u(e.values[i]));
    mem_write32(node+0x34,e.tags[3]);mem_write32(node+0x5C,e.tags[4]);
    if(!emitter_lists_valid(context,error))return false;
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))log_message("[coop-flame-client] sequence="+std::to_string(e.sequence)+" mode="+std::to_string(e.tags[0])+" active="+std::to_string(before+1)+" damage_calls="+std::to_string(presentation_damage_calls));
    return true;
}
// The exact list recycling used by 800D810C; native nodes/resources stay owned
// by the original pools. This contains no actor query or gameplay dispatch.
void recycle_emitter_node(uint32_t node,uint32_t active,uint32_t free,unsigned next){
    const auto following=mem_read32(node+next),previous=mem_read32(node+next+4);
    if(previous)mem_write32(previous+next,following);else mem_write32(active,following);
    if(following)mem_write32(following+next+4,previous);
    mem_write32(active+4,mem_read32(active+4)-1);
    const auto first=mem_read32(free);mem_write32(node+next,first);mem_write32(node+next+4,0);
    if(first)mem_write32(first+next+4,node);mem_write32(free,node);mem_write32(free+4,mem_read32(free+4)+1);
}
bool update_native_emitters(PPCContext& context,float seconds,std::string& error){
    if(!emitter_lists_valid(context,error))return false;
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    PresentationScope presentation;
    for(auto node=mem_read32(context.r[13]-0x5F9C);node;){
        const auto next=mem_read32(node+0x60);const auto age=u2f(mem_read32(node+0x44)),limit=u2f(mem_read32(node+0x48));
        if(!std::isfinite(age) || !std::isfinite(limit) || mem_read32(node)>2){error="Invalid live native flame particle";return false;}
        call.r[3]=node;call.ps0[1]=call.ps1[1]=seconds;
        if(float(age+seconds)>limit){ssne_call(call,0x800D98B8);recycle_emitter_node(node,context.r[13]-0x5F9C,context.r[13]-0x5FA4,0x60);}
        else ssne_call(call,0x800D9A8C);
        node=next;
    }
    // 800D9AE4 may create a native ground flame through 800D857C. Update and
    // retire those original sprites, preserving the pool availability counter.
    call.gqr[6]=0x02050205;call.gqr[7]=0x03050305;
    for(auto node=mem_read32(0x8028BD5C);node;){const auto next=mem_read32(node+0xC);call.r[3]=node;call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x800DB040);
        if(!call.r[3]){recycle_emitter_node(node,0x8028BD5C,0x8028BD54,0xC);mem_write32(0x8028BD68,mem_read32(0x8028BD68)+1);}node=next;
    }
    return emitter_lists_valid(context,error);
}
