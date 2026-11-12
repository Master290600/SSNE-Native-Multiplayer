// The tap-to-switch list is separate from the held weapon wheel at HUD +22C.
bool capture_native_weapon_sidebar(uint32_t actor,EntityState& e,std::string& error){
    const auto hud=mem_read32(actor+0x374);
    if(!readable(hud,0x4B8)){error="Native weapon sidebar unavailable";return false;}
    const auto timer=u2f(mem_read32(hud+0x4B4));
    // The original constructor leaves timer/selection undefined until the
    // first tap. Its draw branch only runs when the timer is positive.
    const auto selected=int32_t(mem_read32(hud+0x4B0));
    const bool active=std::isfinite(timer) && timer>0 && timer<=60.f && selected>=0 && selected<12;
    std::vector<float> state{active?timer:0.f,active?float(selected):-1.f};
    for(unsigned i=0;i<12;++i)state.push_back(float(mem_read32(hud+0x3FC+i*16)));
    e.properties["weapon_sidebar"]=std::move(state);return validate_native_weapon_sidebar(actor,e,error);
}
bool validate_native_weapon_sidebar(uint32_t actor,const EntityState& e,std::string& error){
    const auto it=e.properties.find("weapon_sidebar");const auto* v=it==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&it->second);
    const auto hud=readable(actor,0x790)?mem_read32(actor+0x374):0;
    if(e.kind!=EntityKind::Player || !v || v->size()!=14 || !readable(hud,0x4B8)){error="Invalid weapon sidebar layout";return false;}
    for(unsigned i=0;i<v->size();++i)if(!std::isfinite((*v)[i]) || (i==0?std::abs((*v)[i])>60.f:(i==1?((*v)[i]< -1 || (*v)[i]>11):((*v)[i]<0 || (*v)[i]>2))) || (i>0 && (*v)[i]!=std::floor((*v)[i]))){error="Invalid native weapon sidebar state";return false;}
    for(unsigned i=0;i<12;++i)for(unsigned child=0;child<2;++child){
        const auto widget=mem_read32(hud+0x3F0+i*16+child*4),table=readable(widget,0x38)?mem_read32(widget+0x34):0;
        if(!readable(table,0x18) || !ssne_find_func(mem_read32(table+0x14))){error="Original sidebar widgets unavailable";return false;}
    }
    if(!ssne_find_func(0x800BE4F0u) || mem_read32(0x800BE4F0u)!=0x9421FFB0u){error="Original sidebar update unavailable";return false;}
    return true;
}
bool apply_native_weapon_sidebar(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_weapon_sidebar(actor,e,error))return false;
    const auto& v=std::get<std::vector<float>>(e.properties.at("weapon_sidebar"));const auto hud=mem_read32(actor+0x374);
    mem_write32(hud+0x4B4,f2u(v[0]));mem_write32(hud+0x4B0,uint32_t(int32_t(v[1])));
    for(unsigned i=0;i<12;++i)mem_write32(hud+0x3FC+i*16,uint32_t(v[2+i]));
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=hud;call.ps0[1]=call.ps1[1]=0.;ssne_call(call,0x800BE4F0u);return true;
}
