// Original PAL gib/blood-fragment allocator and visual lifetime manager.
// Included in native_effect.cpp's private namespace.
PPCFunc original_gib=nullptr;
bool validate_bullet_lists(const PPCContext&,std::string&);
constexpr float reptiloid_impact_scale_marker=0x1p-16f;
bool is_reptiloid_impact(const NativeFeedbackEvent& event){
    return event.code==8 && event.variant==2 && event.values[9]==reptiloid_impact_scale_marker;
}
bool validate_gib_list(const PPCContext& c,std::string& error){
    const auto count=mem_read32(c.r[13]-0x5F78);auto current=mem_read32(c.r[13]-0x5F7C);uint32_t previous=0;std::set<uint32_t> seen;
    while(current){
        if(!readable(current,0x220) || !seen.insert(current).second || seen.size()>1024 || mem_read32(current+0x21C)!=previous){error="Invalid native gib presentation list";return false;}
        previous=current;current=mem_read32(current+0x218);
    }
    if(seen.size()!=count){error="Native gib presentation count differs";return false;}return true;
}
bool validate_gib_payload(const NativeFeedbackEvent& e,std::string& error){
    if(e.code>10 || e.variant>64 || e.values[9]<=0 || e.values[9]>100 || e.values[10]!=0 || e.values[11]!=0){error="Invalid original gib type/count/scale";return false;}
    for(unsigned i=3;i<9;++i)if(std::abs(e.values[i])>10000){error="Invalid original gib velocity/rotation";return false;}
    return true;
}
void gib_hook(PPCContext& c){
    NativeFeedbackEvent event;event.kind=3;event.code=c.r[6];event.variant=c.r[7];
    bool capture=authoritative && !presenting;
    if(capture){
        for(unsigned vector=0;vector<3;++vector){capture&=readable(c.r[3+vector],12);if(capture)for(unsigned axis=0;axis<3;++axis)event.values[vector*3+axis]=u2f(mem_read32(c.r[3+vector]+axis*4));}
        event.values[9]=float(c.ps0[1]);
        if(c.lr==0x8006BE94u && event.code==8 && event.variant==2 && event.values[9]==0.f &&
           readable(c.r[27],0x18C) && mem_read32(c.r[27]+0x48)==0x801FE6F8u &&
           mem_read16(c.r[27]+0xD4)==0x8Bu){
            // The original green orb passes zero here. Keep a positive value
            // on the existing wire field so older clients still accept it.
            event.values[9]=reptiloid_impact_scale_marker;
        }
        std::string why;capture=capture && validate_native_feedback(event,why);
    }
    const auto before=capture?mem_read32(c.r[13]-0x5F78):0;
    ssne_call_original(c,0x800DC310,original_gib);
    if(capture && mem_read32(c.r[13]-0x5F78)==before+1){
        std::lock_guard lock(effect_mutex);event.sequence=++feedback_sequence;retain_native_event(feedback_events,event);
        log_message("[coop-feedback-host] sequence="+std::to_string(event.sequence)+" kind=3 code="+std::to_string(event.code)+" variant="+std::to_string(event.variant));
        if(is_reptiloid_impact(event) && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))
            log_message("[coop-reptiloid-impact-host] sequence="+std::to_string(event.sequence)+
                        " shot="+std::to_string(attack_trace.shot)+" slot="+std::to_string(attack_trace.slot)+
                        " x="+std::to_string(event.values[0])+" y="+std::to_string(event.values[1])+
                        " z="+std::to_string(event.values[2]));
    }
}
bool play_reptiloid_impact_sparks(PPCContext& call,const NativeFeedbackEvent& event,std::string& error){
    constexpr uint32_t factory=0x800E7430u;
    if(!ssne_find_func(factory) || mem_read32(factory)!=0x9421FFF0u){
        error="Original Reptiloid impact particle factory unavailable";return false;
    }
    if(!validate_bullet_lists(call,error))return false;
    const auto count=call.r[13]-0x5F58u,before=mem_read32(count);
    const auto data=call.r[1]+0x100;
    for(unsigned axis=0;axis<3;++axis)mem_write32(data+axis*4,f2u(event.values[axis]));
    // Four particles from PAL 8006BE1C's green projectile branch. These
    // are separate from the two green fragments produced by the gib event.
    mem_write32(data+0x18,0);
    mem_write32(data+0x1C,mem_read32(0x801FE688u)); // downward acceleration
    mem_write32(data+0x20,0);
    mem_write32(data+0x24,0x64FF64FFu);
    for(unsigned field=0;field<3;++field)
        mem_write32(data+0x28+field*4,mem_read32(0x801FE68Cu+field*4));
    // Seed local visual variation by impact identity. The first particle
    // starts at rest, and the other three use the native velocity range.
    uint32_t random=uint32_t(event.sequence)^uint32_t(event.sequence>>32);
    for(unsigned particle=0;particle<4;++particle){
        for(unsigned axis=0;axis<3;++axis){
            random=random*1664525u+1013904223u;
            const float velocity=particle?std::fma(float(random),0x1p-31f,-1.f)*6.f:0.f;
            mem_write32(data+0xC+axis*4,f2u(velocity));
        }
        call.r[3]=data;
        ssne_call(call,factory);
    }
    if(mem_read32(count)!=before+4){error="Reptiloid impact did not create its four native sparks";return false;}
    if(!validate_bullet_lists(call,error))return false;
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))
        log_message("[coop-reptiloid-impact-client] sequence="+std::to_string(event.sequence)+
                    " sparks=4 damage_calls="+std::to_string(presentation_damage_calls));
    return true;
}
bool play_gib(PPCContext& c,const NativeFeedbackEvent& e,std::string& error){
    if(!original_gib || !ssne_find_func(0x800DC694) || mem_read32(0x800DC310)!=0x9421FFC0u || mem_read32(0x800DC694)!=0x9421FFE8u || !validate_gib_list(c,error)){if(error.empty())error="Original gib presentation unavailable";return false;}
    const auto row=0x80215E20u+e.code*16;
    if(mem_read32(row)!=e.code || mem_read32(row+4)>10 || mem_read32(row+8)>16){error="Native gib resource palette differs";return false;}
    const auto resource=mem_read32(0x802D59A4u+mem_read32(row+4)*4);
    if(!readable(resource,0x10) || !readable(mem_read32(resource),0x28) || !ssne_find_func(mem_read32(mem_read32(resource)+0x24))){error="Native gib model resource is not loaded";return false;}
    Scratch scratch(c);auto call=c;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],c.r[1]);
    for(unsigned i=0;i<9;++i)mem_write32(call.r[1]+0x80+i*4,f2u(e.values[i]));
    for(unsigned i=0;i<3;++i)call.r[3+i]=call.r[1]+0x80+i*12;
    const bool reptiloid=is_reptiloid_impact(e);
    call.r[6]=e.code;call.r[7]=e.variant;call.ps0[1]=call.ps1[1]=reptiloid?0.f:e.values[9];
    PresentationScope presentation;ssne_call(call,0x800DC310);
    if(!validate_gib_list(c,error))return false;
    // The palette/count combination is emitted only when a green Reptiloid
    // projectile dies (the call at 8006BE90).
    if(reptiloid && !play_reptiloid_impact_sparks(call,e,error))return false;
    log_message("[coop-gib-client] sequence="+std::to_string(e.sequence)+" type="+std::to_string(e.code)+" count="+std::to_string(e.variant)+" active="+std::to_string(mem_read32(c.r[13]-0x5F78))+" damage_calls="+std::to_string(presentation_damage_calls));return true;
}
