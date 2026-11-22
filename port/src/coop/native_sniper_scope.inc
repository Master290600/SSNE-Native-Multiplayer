// The PAL sniper creates an independent set of widgets on the shooter's HUD.
// The authoritative weapon tick that normally opens them does not run on replicas.
namespace {
std::string sniper_scope_key(unsigned slot){return "Session.SniperScope."+std::to_string(slot);}
}
void capture_native_sniper_scope(uint32_t actor,unsigned slot,Properties& mission){
    if(!readable(actor,0x790) || mem_read32(actor+0x5dc)!=8)return;
    const auto hud=mem_read32(actor+0x374);
    if(!readable(hud,0x52c) || !mem_read32(hud+0x4f0))return;
    const auto rifle=mem_read32(actor+0x4bc+8*24),indicator=mem_read32(hud+0x524);
    if(!readable(rifle,0x234) || !readable(indicator,8))return;
    const auto mode=mem_read32(rifle+0x228);
    const auto zoom=u2f(mem_read32(rifle+0x230));
    if(mode>1 || !std::isfinite(zoom) || zoom<1 || zoom>12)return;
    // 800BB188 formats the original auto-aim/raycast distance in this
    // eight-byte buffer. Only its numeric result belongs on the wire.
    unsigned integral=0,n=0;float range=0;
    const auto digit=[&](unsigned i){return int(mem_read8(hud+0x51c+i))-'0';};
    while(n<4 && digit(n)>=0 && digit(n)<=9){integral=10*integral+unsigned(digit(n));++n;}
    if(n && mem_read8(hud+0x51c+n)=='.' && digit(n+1)>=0 && digit(n+1)<=9 &&
       digit(n+2)>=0 && digit(n+2)<=9 && mem_read8(hud+0x51c+n+3)==0)
        range=float(integral)+float(10*digit(n+1)+digit(n+2))*.01f;
    // Optional mission extensions are ignored by older protocol-71 clients.
    mission[sniper_scope_key(slot)]=std::array<float,4>{float(mode),zoom,range,float(mem_read8(indicator+4)==0x14)};
}

bool apply_native_sniper_scope(PPCContext& context,uint32_t actor,unsigned slot,const Properties& mission,std::string& error){
    if(!readable(actor,0x790))return true;
    const auto hud=mem_read32(actor+0x374);
    if(!readable(hud,0x52c))return true; // A spawning player can have no presentation yet.
    const bool was_active=mem_read32(hud+0x4f0)!=0;
    const auto it=mission.find(sniper_scope_key(slot));
    const bool active=it!=mission.end();
    if(!active && !was_active)return true;
    std::array<float,4> state{2.f,1.f,0.f,0.f};
    if(active){
        const auto* sample=std::get_if<std::array<float,4>>(&it->second);
        if(!sample){error="Invalid scope HUD state";return false;}
        const auto [mode,zoom,range,ready]=*sample;
        if((mode!=0 && mode!=1) || !std::isfinite(zoom) || zoom<1 || zoom>12 ||
           !std::isfinite(range) || range<0 || range>=10000 || (ready!=0 && ready!=1)){
            error="Invalid scope HUD values";return false;
        }
        if(mem_read32(actor+0x5dc)!=8 || !readable(mem_read32(actor+0x4bc+8*24),0x234))return true;
        state=*sample;
    }
    if(!context_ok(context) || !ssne_find_func(0x800ba788u) || !ssne_find_func(0x800bb080u) ||
       !ssne_find_func(0x800bb188u) || !ssne_find_func(0x800bafccu)){
        error="Original sniper HUD setters unavailable";return false;
    }
    StackScratch scratch(context);
    auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    if(active!=was_active){
        call.r[3]=hud;call.r[4]=uint32_t(active);ssne_call(call,0x800ba788u);
    }
    const auto rifle=mem_read32(actor+0x4bc+8*24);
    if(readable(rifle,0x234)){
        mem_write32(rifle+0x228,uint32_t(state[0]));
        mem_write32(rifle+0x230,f2u(state[1]));
    }
    if(active){
        call.r[3]=hud;call.ps0[1]=call.ps1[1]=state[1];ssne_call(call,0x800bb080u);
        call.r[3]=hud;call.ps0[1]=call.ps1[1]=state[2];ssne_call(call,0x800bb188u);
        call.r[3]=hud;call.r[4]=uint32_t(state[3]);ssne_call(call,0x800bafccu);
    }
    return true;
}
