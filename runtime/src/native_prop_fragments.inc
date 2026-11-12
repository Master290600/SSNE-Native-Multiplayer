// The original scenery-fragment pool also serves bullet impacts. Capture only
// the verified DestructionEntity caller to avoid replaying bullet debris twice.
PPCFunc original_prop_fragments=nullptr;
bool validate_prop_fragment_payload(const NativeFeedbackEvent& e,std::string& error){
    if(e.code>6 || !e.variant || e.variant>30 || e.values[9]<=0 || e.values[9]>100 || e.values[10]!=0 || e.values[11]!=0){error="Invalid scenery fragment material/count/scale";return false;}
    float speed=0;for(unsigned i=3;i<9;++i){if(std::abs(e.values[i])>10000){error="Invalid scenery fragment direction";return false;}if(i<6)speed+=e.values[i]*e.values[i];}
    if(speed<.000001f){error="Scenery fragment velocity cannot be normalized";return false;}return true;
}
void prop_fragment_hook(PPCContext& c){
    NativeFeedbackEvent e;e.kind=4;e.code=c.r[3];e.variant=c.r[4];bool capture=authoritative && !presenting && (c.lr==0x8007AF9Cu || c.lr==0x8007E3E0u);
    if(capture){for(unsigned vector=0;vector<3;++vector){capture&=readable(c.r[5+vector],12);if(capture)for(unsigned axis=0;axis<3;++axis)e.values[vector*3+axis]=u2f(mem_read32(c.r[5+vector]+axis*4));}e.values[9]=float(c.ps0[1]);std::string why;capture=capture && validate_native_feedback(e,why);}
    const auto before=capture?mem_read32(0x8028B8C8u):0;ssne_call_original(c,0x800CBAC4u,original_prop_fragments);
    if(capture && mem_read32(0x8028B8C8u)==before+1){std::lock_guard lock(effect_mutex);e.sequence=++feedback_sequence;retain_native_event(feedback_events,e);log_message("[coop-prop-fragments-host] sequence="+std::to_string(e.sequence)+" material="+std::to_string(e.code)+" count="+std::to_string(e.variant));}
}
bool play_prop_fragments(PPCContext& c,const NativeFeedbackEvent& e,std::string& error){
    if(!original_prop_fragments || !validate_bullet_lists(c,error))return false;
    const auto resource=mem_read32(0x802D9B40u+e.code*4),table=readable(resource,0x10)?mem_read32(resource):0;
    if(!readable(table,0x28) || !ssne_find_func(mem_read32(table+0x24))){error="Original scenery fragment resource unavailable";return false;}
    Scratch scratch(c);auto call=c;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],c.r[1]);
    for(unsigned i=0;i<9;++i)mem_write32(call.r[1]+0x80+i*4,f2u(e.values[i]));
    call.r[3]=e.code;call.r[4]=e.variant;for(unsigned i=0;i<3;++i)call.r[5+i]=call.r[1]+0x80+i*12;call.ps0[1]=call.ps1[1]=e.values[9];
    PresentationScope presentation;ssne_call(call,0x800CBAC4u);if(!validate_bullet_lists(c,error))return false;
    log_message("[coop-prop-fragments-client] sequence="+std::to_string(e.sequence)+" material="+std::to_string(e.code)+" count="+std::to_string(e.variant)+" damage_calls="+std::to_string(presentation_damage_calls));return true;
}
