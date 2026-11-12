// Original death controller publishes its countdown through HUD +40/+3C.
// Only presentation crosses the network; the host retains the respawn clock.
bool capture_native_reentry_hud(uint32_t actor,EntityState& e,std::string& error){
    const auto hud=mem_read32(actor+0x374);
    if(!readable(hud,0x44)){error="Native reentry HUD unavailable";return false;}
    const bool active=mem_read32(hud+0x40)!=0;
    int64_t seconds=0;
    if(active){
        const auto widget=mem_read32(hud+0x3C);
        if(!readable(widget,0xC8)){error="Native reentry counter unavailable";return false;}
        std::string number;
        for(unsigned i=0;i<8 && mem_read8(widget+0x1C+i);++i)number+=char(mem_read8(widget+0x1C+i));
        if(number.empty() || number.size()>4 || number.find_first_not_of("0123456789")!=std::string::npos){error="Invalid original reentry counter";return false;}
        seconds=std::stoll(number);
    }
    e.properties["reentry_hud"]=std::vector<int64_t>{int64_t(active),seconds};
    return validate_native_reentry_hud(actor,e,error);
}
bool validate_native_reentry_hud(uint32_t actor,const EntityState& e,std::string& error){
    const auto it=e.properties.find("reentry_hud");
    const auto* v=it==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&it->second);
    const auto hud=readable(actor,0x790)?mem_read32(actor+0x374):0;
    if(e.kind!=EntityKind::Player || !v || v->size()!=2 || !readable(hud,0x44) || ((*v)[0]!=0 && (*v)[0]!=1) || (*v)[1]<0 || (*v)[1]>9999 || (!(*v)[0] && (*v)[1])){error="Invalid native reentry HUD state";return false;}
    for(unsigned off:{0x38u,0x3Cu}){
        const auto widget=mem_read32(hud+off),table=readable(widget,0xC8)?mem_read32(widget+0xC4):0;
        if(!readable(table,0x28) || !ssne_find_func(mem_read32(table+0x1C))){error="Native reentry text setter unavailable";return false;}
    }
    for(auto address:{0x800BBA34u,0x800BBAE8u,0x800BBA9Cu,0x800BBBDCu})
        if(!ssne_find_func(address) || mem_read32(address)!=(address==0x800BBAE8u?0x9421FFC0u:address==0x800BBBDCu?0x9421FFE8u:0x9421FFF0u)){error="Original reentry HUD lifecycle unavailable";return false;}
    return true;
}
bool apply_native_reentry_hud(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_reentry_hud(actor,e,error))return false;
    const auto& v=std::get<std::vector<int64_t>>(e.properties.at("reentry_hud"));
    const auto hud=mem_read32(actor+0x374);
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=hud;
    if(v[0]){
        if(!mem_read32(hud+0x40))ssne_call(call,0x800BBA34u);
        call.r[3]=hud;call.r[4]=uint32_t(v[1]);ssne_call(call,0x800BBAE8u);
        call.r[3]=hud;call.ps0[1]=call.ps1[1]=0.;ssne_call(call,0x800BBBDCu);
    }else if(mem_read32(hud+0x40))ssne_call(call,0x800BBA9Cu);
    return true;
}
