// W11 has a sustained original three-second charge. Its two visual objects
// are separate from the weapon's primary model and from its gameplay tick.
std::optional<uint32_t> replica_bomb_frame;
PPCFunc original_bomb_feedback=nullptr;
// 8005E190 mixes damage with a per-player HUD flash. Capture only its verified
// 800BE700 call; replay never enters the detonation or camera-shake routines.
bool bomb_feedback_binding(unsigned slot,uint32_t& hud,uint32_t& flash){
    if(slot>=native_party_capacity())return false;
    const auto actor=mem_read32(native_party_registry()+slot*4);
    if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(actor+0x17C)!=slot)return false;
    hud=mem_read32(actor+0x374);if(!readable(hud,0x4BC))return false;
    flash=mem_read32(hud+0x4B8);if(!readable(flash,0x20) || mem_read32(flash+0x10)>1)return false;
    for(const auto [offset,expected]:{std::pair{0u,0x8011D458u},std::pair{0x18u,0x80122AFCu}}){const auto resource=mem_read32(flash+offset),table=readable(resource,4)?mem_read32(resource):0;
        if(!readable(table,0x48) || mem_read32(table+0x44)!=expected || !ssne_find_func(expected))return false;}
    return true;
}
void bomb_feedback_hook(PPCContext& c){
    const auto target=c.r[30],instance=c.r[31];uint32_t hud=0,flash=0;
    NativeFeedbackEvent event;event.kind=2;event.values[0]=float(c.ps0[1]);event.values[1]=float(c.ps0[2]);
    bool capture=authoritative && !presenting && c.lr==0x8005E284u && readable(target,0x790) && readable(instance,0x250) && mem_read32(instance+0xC)==0x801FBDA0u;
    if(capture){event.code=mem_read32(target+0x17C);const auto owner=mem_read32(instance+0x1D4);
        capture=bomb_feedback_binding(event.code,hud,flash) && mem_read32(native_party_registry()+event.code*4)==target && hud==c.r[3] && readable(owner,0x790) &&
            mem_read32(owner+0x48)==0x802121E8u && (mem_read32(owner+0x4BC+11*24)==instance || mem_read32(owner+0x4C0+11*24)==instance);}
    std::string why;capture=capture && validate_native_feedback(event,why);
    ssne_call_original(c,0x800BE700u,original_bomb_feedback);
    if(capture && mem_read32(flash+0x10)==1){std::lock_guard lock(effect_mutex);event.sequence=++feedback_sequence;retain_native_event(feedback_events,event);
        log_message("[coop-feedback-host] sequence="+std::to_string(event.sequence)+" kind=2 code="+std::to_string(event.code)+" duration="+std::to_string(event.values[0])+" strength="+std::to_string(event.values[1]));}
}
bool install_native_bomb_feedback_capture(std::string& error){
    for(const auto [address,signature]:{std::pair{0x800BE700u,0x9421FFF8u},std::pair{0x800ADEC0u,0x9421FFE8u},std::pair{0x800ADF68u,0x9421FFB8u}})
        if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native bomb HUD feedback unavailable";return false;}
    if(!original_bomb_feedback){original_bomb_feedback=ssne_find_func(0x800BE700u)->func;ssne_install_hook(0x800BE700u,bomb_feedback_hook);}return true;
}
uint32_t bomb_instance(uint32_t actor,unsigned hand){
    if(hand>1 || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)return 0;
    const auto instance=mem_read32(actor+0x4BC+11*24+hand*4);
    return readable(instance,0x250) && mem_read32(instance+0xC)==0x801FBDA0u && mem_read32(instance+0x1D4)==actor?instance:0;
}
bool bomb_proxy(uint32_t proxy,uint32_t& matrix){
    const auto table=readable(proxy,0x38)?mem_read32(proxy):0;matrix=readable(proxy,8)?mem_read32(proxy+4):0;
    return readable(table,0x30) && mem_read32(table+0x2C)==0x8013E954u && readable(matrix,48) && mem_read8(proxy+0x2D)<=1;
}
bool bomb_controller(uint32_t object){
    uint32_t matrix=0;return readable(object,0x30) && mem_read32(object+0x1C)==0x802171B0u && mem_read32(object+0x28)==64 &&
        readable(mem_read32(object+0x24),4) && bomb_proxy(mem_read32(object+0x20),matrix);
}
bool bomb_visual_valid(const std::vector<float>& v,const std::array<uint32_t,2>&,std::string& error){
    if(v.size()!=32){error="Invalid native bomb visual field count";return false;}
    for(auto f:v)if(!std::isfinite(f) || std::abs(f)>1000000.f){error="Invalid native bomb visual coefficient";return false;}
    for(unsigned i=0;i<4;++i)if(v[i]!=0 && v[i]!=1){error="Invalid native bomb visibility";return false;}
    if((!v[0] && v[2]) || (!v[1] && v[3]) || v[19]<0 || v[19]>1000){error="Invalid native bomb controller radius/state";return false;}return true;
}
bool capture_bomb_visual(uint32_t actor,unsigned hand,std::vector<float>& v,std::array<uint32_t,2>& masks,std::string& error){
    v.assign(32,0);masks={};const auto instance=bomb_instance(actor,hand);if(!instance)return true;
    const auto model=mem_read32(instance+0x240),controller=mem_read32(instance+0x238),uv=mem_read32(instance+0x244);uint32_t matrix=0;
    if(model){if(!bomb_proxy(model,matrix)){error="Invalid native bomb model";return false;}v[0]=1;v[2]=float(mem_read8(model+0x2D));masks[0]=mem_read32(model+0x30);for(unsigned i=0;i<12;++i)v[4+i]=u2f(mem_read32(matrix+i*4));}
    if(controller){if(!bomb_controller(controller)){error="Invalid native bomb particle controller";return false;}v[1]=1;const auto proxy=mem_read32(controller+0x20);v[3]=float(mem_read8(proxy+0x2D));masks[1]=mem_read32(proxy+0x30);for(unsigned i=0;i<3;++i)v[16+i]=u2f(mem_read32(controller+i*4));v[19]=u2f(mem_read32(controller+0x2C));}
    if(!readable(uv,48)){error="Native bomb UV matrix missing";return false;}for(unsigned i=0;i<12;++i)v[20+i]=u2f(mem_read32(uv+i*4));
    return bomb_visual_valid(v,masks,error);
}
bool apply_bomb_visual(PPCContext& context,uint32_t actor,unsigned hand,const std::vector<float>& v,const std::array<uint32_t,2>& masks,std::string& error){
    if(!context_ok(context) || hand>1 || !bomb_visual_valid(v,masks,error)){if(error.empty())error="Native bomb context unavailable";return false;}
    const auto instance=bomb_instance(actor,hand);if(!instance){if(v[0] || v[1]){error="Native bomb instance unavailable";return false;}return true;}
    uint32_t matrix=0;const auto existing_model=mem_read32(instance+0x240),existing_controller=mem_read32(instance+0x238),uv=mem_read32(instance+0x244);
    if((existing_model && !bomb_proxy(existing_model,matrix)) || (existing_controller && !bomb_controller(existing_controller)) || !readable(uv,48)){error="Invalid local bomb visual binding";return false;}
    for(auto address:{0x8005DA54u,0x800CE060u,0x800CE614u,0x800E9B64u,0x800E9E90u,0x800E9E98u,0x800E9F0Cu,0x8013E7E8u})if(!ssne_find_func(address)){error="Native bomb visual lifecycle unavailable";return false;}
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;
    call.r[3]=instance;call.r[4]=unsigned(v[0]);ssne_call(call,0x8005DA54u);
    const auto model=mem_read32(instance+0x240);if(model){if(!bomb_proxy(model,matrix)){error="Original bomb model creation failed";return false;}for(unsigned i=0;i<12;++i)mem_write32(matrix+i*4,f2u(v[4+i]));call.r[3]=model;call.r[4]=unsigned(v[2]);ssne_call(call,0x8013E7E8u);mem_write32(model+0x30,masks[0]);}
    auto controller=mem_read32(instance+0x238);
    if(!v[1] && controller){call.r[3]=mem_read32(0x802ABAB8u);call.r[4]=controller;ssne_call(call,0x800CE614u);mem_write32(instance+0x238,0);controller=0;}
    for(unsigned i=0;i<3;++i)mem_write32(call.r[1]+0x100+i*4,f2u(v[16+i]));
    if(v[1] && !controller){call.r[3]=mem_read32(0x802ABAB8u);call.r[4]=0x800;call.r[5]=call.r[1]+0x100;call.r[6]=64;ssne_call(call,0x800CE060u);controller=call.r[3];
        if(!bomb_controller(controller)){error="Original bomb particle creation failed";return false;}mem_write32(instance+0x238,controller);call.r[3]=controller;call.r[4]=5;ssne_call(call,0x800E9F0Cu);}
    if(controller){call.r[3]=controller;call.r[4]=call.r[1]+0x100;ssne_call(call,0x800E9E98u);call.r[3]=controller;call.ps0[1]=call.ps1[1]=v[19];ssne_call(call,0x800E9E90u);const auto proxy=mem_read32(controller+0x20);call.r[3]=proxy;call.r[4]=unsigned(v[3]);ssne_call(call,0x8013E7E8u);mem_write32(proxy+0x30,masks[1]);}
    for(unsigned i=0;i<12;++i)mem_write32(uv+i*4,f2u(v[20+i]));return true;
}
bool update_native_bombs(PPCContext& context,std::string& error){
    const auto frame=mem_read32(context.r[13]-0x6D80);if(replica_bomb_frame && *replica_bomb_frame==frame)return true;replica_bomb_frame=frame;
    Scratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);PresentationScope presentation;
    for(unsigned slot=0;slot<native_party_capacity();++slot){const auto actor=mem_read32(native_party_registry()+slot*4);for(unsigned hand=0;hand<2;++hand)if(const auto instance=bomb_instance(actor,hand))if(const auto controller=mem_read32(instance+0x238)){
        if(!bomb_controller(controller)){error="Invalid replica bomb particle controller";return false;}call.r[3]=controller;ssne_call(call,0x800E9B64u);}}
    return true;
}
