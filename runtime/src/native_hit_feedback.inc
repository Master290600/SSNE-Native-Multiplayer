// Ordinary hits use the splatter controller, independently of wall impacts/gibs.
PPCFunc original_splatter_velocity=nullptr,original_splatter_direction=nullptr,original_hurt_feedback=nullptr;
thread_local std::map<uint32_t,NativeFeedbackEvent> pending_splatters;
PPCFunc original_combo_speech=nullptr;
std::optional<unsigned> combo_feedback_slot(uint32_t actor){
    if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)return {};
    const auto slot=mem_read32(actor+0x17C);if(slot>=native_party_capacity() || mem_read32(native_party_registry()+slot*4)!=actor)return {};return slot;
}
void combo_speech_hook(PPCContext& c){
    const auto slot=combo_feedback_slot(c.r[31]);const bool capture=authoritative && !presenting && slot && c.lr==0x800B5D6Cu && c.r[3]==0x89 && c.r[4]==0;
    ssne_call_original(c,0x801115B8,original_combo_speech);
    if(capture && c.r[3]){NativeFeedbackEvent e;e.kind=9;e.code=0x89;e.variant=*slot;std::lock_guard lock(effect_mutex);e.sequence=++feedback_sequence;retain_native_event(feedback_events,e);}
}
bool hurt_feedback_binding(unsigned slot,uint32_t& hud){
    if(slot>=native_party_capacity())return false;
    const auto actor=mem_read32(native_party_registry()+slot*4);
    if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(actor+0x17C)!=slot)return false;
    hud=mem_read32(actor+0x374);if(!readable(hud,0x21C))return false;
    const auto flash=mem_read32(hud+0x208);if(!readable(flash,0x20))return false;
    for(unsigned field:{0x20Cu,0x210u,0x214u,0x218u})if(!readable(mem_read32(hud+field),0x28))return false;
    for(const auto [offset,expected]:{std::pair{0u,0x8011D458u},std::pair{0x18u,0x80122AFCu}}){const auto resource=mem_read32(flash+offset),table=readable(resource,4)?mem_read32(resource):0;
        if(!readable(table,0x48) || mem_read32(table+0x44)!=expected)return false;}
    return true;
}
void hurt_feedback_hook(PPCContext& c){
    NativeFeedbackEvent e;e.kind=7;e.values[0]=float(c.ps0[1]);bool capture=false;
    if(authoritative && !presenting)for(unsigned slot=0;slot<native_party_capacity();++slot){uint32_t hud=0;if(hurt_feedback_binding(slot,hud) && hud==c.r[3]){e.code=slot;capture=true;break;}}
    std::string why;capture=capture && validate_native_feedback(e,why);
    ssne_call_original(c,0x800BD83C,original_hurt_feedback);
    if(capture){std::lock_guard lock(effect_mutex);e.sequence=++feedback_sequence;retain_native_event(feedback_events,e);}
}
void splatter_velocity_hook(PPCContext& c){
    const auto object=c.r[3];NativeFeedbackEvent e;e.kind=6;
    bool capture=authoritative && !presenting && readable(object,0xA0) && mem_read32(object+0x1C)==0x80214FE8u && readable(c.r[4],12);
    if(capture){e.code=mem_read32(object+0x98);for(unsigned i=0;i<3;++i){e.values[i]=u2f(mem_read32(object+i*4));e.values[3+i]=u2f(mem_read32(c.r[4]+i*4));}}
    ssne_call_original(c,0x800CF638,original_splatter_velocity);
    if(capture){if(pending_splatters.size()>256)pending_splatters.clear();pending_splatters[object]=e;}
}
void splatter_direction_hook(PPCContext& c){
    const auto found=pending_splatters.find(c.r[3]);std::optional<NativeFeedbackEvent> e;
    if(authoritative && !presenting && found!=pending_splatters.end() && readable(c.r[4],12)){
        e=found->second;pending_splatters.erase(found);for(unsigned i=0;i<3;++i)e->values[6+i]=u2f(mem_read32(c.r[4]+i*4));
        std::string why;if(!validate_native_feedback(*e,why))e.reset();
    }
    ssne_call_original(c,0x800CF79C,original_splatter_direction);
    if(e){std::lock_guard lock(effect_mutex);e->sequence=++feedback_sequence;retain_native_event(feedback_events,*e);}
}
bool install_native_hit_feedback(std::string& error){
    for(const auto [address,signature]:{std::pair{0x800CF638u,0x9421FF78u},std::pair{0x800CF79Cu,0x9421FFC8u},std::pair{0x800BD83Cu,0x9421FFF8u},std::pair{0x800CE060u,0x9421FFE0u},std::pair{0x800CF3F8u,0x9421FF58u}})
        if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native hit presentation unavailable";return false;}
    if(!original_hurt_feedback){original_hurt_feedback=ssne_find_func(0x800BD83C)->func;ssne_install_hook(0x800BD83C,hurt_feedback_hook);
        original_splatter_velocity=ssne_find_func(0x800CF638)->func;ssne_install_hook(0x800CF638,splatter_velocity_hook);
        original_splatter_direction=ssne_find_func(0x800CF79C)->func;ssne_install_hook(0x800CF79C,splatter_direction_hook);}
    if(!original_combo_speech){if(!ssne_find_func(0x801115B8) || mem_read32(0x801115B8)!=0x9421FFF0u){error="Original combo announcement unavailable";return false;}original_combo_speech=ssne_find_func(0x801115B8)->func;ssne_install_hook(0x801115B8,combo_speech_hook);}
    return true;
}
bool play_native_hit_feedback(PPCContext& context,const NativeFeedbackEvent& e,std::string& error){
    if(e.kind==8 || e.kind==9){
        // A combo belongs to its player, unlike spatial world sounds.
        if(int(e.variant)!=netplay_local_player())return true;
        const auto actor=mem_read32(native_party_registry()+e.variant*4);if(!combo_feedback_slot(actor)){error="Native combo audio owner unavailable";return false;}
        Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;
        if(e.kind==9){call.r[3]=e.code;call.r[4]=0;ssne_call(call,0x801115B8);return true;}
        if(!e.code){call.r[3]=mem_read32(actor+0x43C);ssne_call(call,0x8010EDFC);mem_write32(actor+0x43C,UINT32_MAX);return true;}
        call.r[3]=e.code;call.r[4]=uint32_t(e.values[0]);ssne_call(call,0x8010E7B0);
        if(e.code==0x180 || e.code==0x182)mem_write32(actor+0x43C,call.r[3]);return true;
    }
    uint32_t hud=0;if(e.kind==7 && !hurt_feedback_binding(e.code,hud)){error="Native hurt HUD unavailable";return false;}
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;
    if(e.kind==7){call.r[3]=hud;call.ps0[1]=call.ps1[1]=e.values[0];ssne_call(call,0x800BD83C);return true;}
    const auto manager=mem_read32(0x802ABAB8u);if(!readable(manager,12)){error="Native splatter manager unavailable";return false;}
    for(unsigned i=0;i<9;++i)mem_write32(call.r[1]+0x80+i*4,f2u(e.values[i]));
    call.r[3]=manager;call.r[4]=1;call.r[5]=call.r[1]+0x80;call.r[6]=e.code;ssne_call(call,0x800CE060);
    const auto object=call.r[3];if(!readable(object,0xA0) || mem_read32(object+0x1C)!=0x80214FE8u){error="Native splatter creation failed";return false;}
    call.r[3]=object;call.r[4]=call.r[1]+0x8C;ssne_call(call,0x800CF638);
    call.r[3]=object;call.r[4]=call.r[1]+0x98;ssne_call(call,0x800CF79C);return true;
}
bool update_native_splatters(PPCContext& context,float seconds,std::string& error){
    const auto manager=mem_read32(0x802ABAB8u);if(!readable(manager,12)){error="Invalid native splatter manager";return false;}
    std::vector<uint32_t> objects;std::set<uint32_t> seen;auto node=mem_read32(manager);
    while(node){if(!readable(node,12) || !seen.insert(node).second || seen.size()>32768){error="Invalid native splatter list";return false;}const auto object=mem_read32(node);
        if(!readable(object,0x20)){error="Invalid native particle object";return false;}if(mem_read32(object+0x1C)==0x80214FE8u){if(!readable(object,0xA0)){error="Invalid splatter storage";return false;}objects.push_back(object);}node=mem_read32(node+4);}
    if(objects.empty())return true;
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;
    for(auto object:objects){call.r[3]=object;call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,0x800CF3F8);if(mem_read32(object+0x18)){call.r[3]=manager;call.r[4]=object;ssne_call(call,0x800CE614);}}
    return true;
}
