bool capture_native_combo_hud(uint32_t actor,EntityState& e,std::string& error){
    const auto hud=mem_read32(actor+0x374);if(!readable(hud,0x70)){error="Native combo HUD unavailable";return false;}
    std::vector<float> state(4,0.f);
    if(mem_read32(hud+0x20)==0 && mem_read32(hud+0x60)){
        state[0]=1;state[1]=u2f(mem_read32(hud+0x64));state[2]=u2f(mem_read32(hud+0x68));
        const auto widget=mem_read32(hud+0x5C);if(!readable(widget,0xC8)){error="Native combo counter unavailable";return false;}
        std::string number;for(unsigned i=0;i<32 && mem_read8(widget+0x1C+i);++i)number+=char(mem_read8(widget+0x1C+i));
        if(number.empty() || number.find_first_not_of("0123456789")!=std::string::npos || number.size()>9){error="Native combo counter is not a number";return false;}
        state[3]=float(std::stoul(number));
    }
    // The number is a child of the native sliding banner. Opening only that
    // child leaves the entire banner off screen, with an empty caption.
    const auto caption=mem_read32(hud+0x58);
    if(!readable(caption,0xD8)){error="Native combo caption unavailable";return false;}
    std::string text;
    for(unsigned i=0;i<128 && mem_read8(caption+0x1C+i);++i)text+=char(mem_read8(caption+0x1C+i));
    std::vector<float> banner;
    for(auto offset:{0x7Cu,0x80u}){
        const auto slide=mem_read32(hud+offset);
        if(!readable(slide,0x34)){error="Native combo slide unavailable";return false;}
        banner.push_back(float(mem_read32(slide)));
        for(auto field:{4u,8u,0x10u})banner.push_back(u2f(mem_read32(slide+field)));
    }
    e.properties["combo_hud"]=std::move(state);
    e.properties["combo_banner"]=std::move(banner);
    e.properties["combo_caption"]=std::move(text);
    // Text fading is independent of the two sliding edge effects. In
    // particular phase6 replaces the title with the kill count and phase7
    // shows the final award after the edge effects have already closed.
    e.properties["combo_caption_style"]=std::vector<float>{float(bool(mem_read32(caption+0xB8))),
        mem_read32(caption+0xB8) && mem_read32(caption+0xAC)?u2f(mem_read32(caption+0xB4)):0.f,
        float(bool(mem_read32(caption+0xBC))),u2f(mem_read32(caption+0xD4))};
    return validate_native_combo_hud(actor,e,error);
}
bool validate_native_combo_hud(uint32_t actor,const EntityState& e,std::string& error){
    const auto it=e.properties.find("combo_hud");const auto* v=it==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&it->second);
    const auto hud=readable(actor,0x790)?mem_read32(actor+0x374):0;
    if(e.kind!=EntityKind::Player || !v || v->size()!=4 || !readable(hud,0x84) || ((*v)[0]!=0 && (*v)[0]!=1)){error="Invalid combo HUD state";return false;}
    for(auto f:*v)if(!std::isfinite(f) || f<0 || f>1000000000.f){error="Invalid combo HUD coefficient";return false;}
    if((*v)[0] && ((*v)[2]<=0 || (*v)[2]>60 || (*v)[1]>(*v)[2]+.1f || (*v)[3]!=std::floor((*v)[3]))){error="Invalid combo HUD time/counter";return false;}
    if(!(*v)[0] && ((*v)[1]!=0 || (*v)[2]!=0 || (*v)[3]!=0)){error="Inactive combo HUD has payload";return false;}
    const auto widget=mem_read32(hud+0x5C);if(!readable(widget,0xC8) || !ssne_find_func(0x800BC0A8) || mem_read32(0x800BC0A8)!=0x9421FFD8u || !ssne_find_func(0x800BC174) || mem_read32(0x800BC174)!=0x9421FFB8u){error="Original combo HUD setters unavailable";return false;}
    const auto caption=e.properties.find("combo_caption"),banner=e.properties.find("combo_banner");
    const auto* text=caption==e.properties.end()?nullptr:std::get_if<std::string>(&caption->second);
    const auto* slide=banner==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&banner->second);
    if(!text || text->size()>127 || text->find('\0')!=std::string::npos || !slide || slide->size()!=8){error="Invalid native combo banner";return false;}
    if(const auto style=e.properties.find("combo_caption_style");style!=e.properties.end()){
        const auto* s=std::get_if<std::vector<float>>(&style->second);
        if(!s || s->size()!=4 || ((*s)[0]!=0 && (*s)[0]!=1) || ((*s)[2]!=0 && (*s)[2]!=1) ||
            !std::isfinite((*s)[1]) || (*s)[1]<0 || (*s)[1]>60 || !std::isfinite((*s)[3]) || (*s)[3]<0 || (*s)[3]>100){error="Invalid combo caption animation";return false;}
    }
    for(unsigned i=0;i<8;++i)if(!std::isfinite((*slide)[i]) || (*slide)[i]<0 || (*slide)[i]>1000000000.f ||
        (i%4==0 && (*slide)[i]!=0 && (*slide)[i]!=1) || (i%4==1 && (*slide)[i]>1) || (i%4==2 && (*slide)[i]>1)){error="Invalid native combo slide";return false;}
    const auto title=mem_read32(hud+0x58),table=readable(title,0xD8)?mem_read32(title+0xC4):0;
    if(!readable(table,0x20) || mem_read32(table+0x1C)!=0x800ABBE4u || !ssne_find_func(0x800ABBE4u) ||
        !ssne_find_func(0x800AF7B8u) || mem_read32(0x800AF7B8u)!=0x9421FFA0u){error="Original combo banner setters unavailable";return false;}
    for(auto offset:{0x7Cu,0x80u})if(!readable(mem_read32(hud+offset),0x34)){error="Native combo banner binding unavailable";return false;}
    for(auto [fn,signature]:{std::pair{0x800ABC7Cu,0x3D208021u},std::pair{0x800ABC9Cu,0x9421FFF0u},std::pair{0x800ABF54u,0x800300C8u}})
        if(!ssne_find_func(fn) || mem_read32(fn)!=signature){error="Original combo text animation unavailable";return false;}
    return true;
}
bool apply_native_combo_hud(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_combo_hud(actor,e,error))return false;
    const auto& v=std::get<std::vector<float>>(e.properties.at("combo_hud"));const auto hud=mem_read32(actor+0x374);
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    const auto title=mem_read32(hud+0x58);const auto& caption=std::get<std::string>(e.properties.at("combo_caption"));
    std::string displayed_caption;for(unsigned i=0;i<128 && mem_read8(title+0x1C+i);++i)displayed_caption+=char(mem_read8(title+0x1C+i));
    const auto style=e.properties.find("combo_caption_style");
    const auto* s=style==e.properties.end()?nullptr:&std::get<std::vector<float>>(style->second);
    if(displayed_caption!=caption || (s && !(*s)[0] && mem_read32(title+0xB8))){
        const auto buffer=call.r[1]+0x80;
        for(unsigned i=0;i<=caption.size();++i)mem_write8(buffer+i,i<caption.size()?uint8_t(caption[i]):0);
        call.r[3]=title;call.r[4]=buffer;ssne_call(call,0x800ABBE4u);
    }
    if(s){
        if((*s)[0] && !mem_read32(title+0xB8)){
            call.r[3]=title;
            if((*s)[1]>0){call.ps0[1]=call.ps1[1]=(*s)[1];ssne_call(call,0x800ABC7Cu);}
            else {ssne_call(call,0x800AB6ECu);mem_write32(title+0xB8,1);mem_write32(title+0xAC,0);}
        }
        call.r[3]=title;call.r[4]=uint32_t((*s)[2]);ssne_call(call,0x800ABC9Cu);
        if(u2f(mem_read32(title+0xD4))!=(*s)[3]){call.r[3]=title;call.ps0[1]=call.ps1[1]=(*s)[3];ssne_call(call,0x800ABF54u);}
    }
    if(v[0]){
        const auto widget=mem_read32(hud+0x5C);std::string displayed;for(unsigned i=0;i<32 && mem_read8(widget+0x1C+i);++i)displayed+=char(mem_read8(widget+0x1C+i));
        if(!mem_read32(hud+0x60) || displayed!=std::to_string(uint32_t(v[3]))){call.r[3]=hud;call.r[4]=uint32_t(v[3]);call.ps0[1]=call.ps1[1]=v[2];ssne_call(call,0x800BC0A8);}
        mem_write32(hud+0x64,f2u(v[1]));mem_write32(hud+0x68,f2u(v[2]));
    }else if(mem_read32(hud+0x60)){mem_write32(hud+0x64,mem_read32(hud+0x68));call.r[3]=hud;call.ps0[1]=call.ps1[1]=0;ssne_call(call,0x800BC174);}
    const auto& banner=std::get<std::vector<float>>(e.properties.at("combo_banner"));
    unsigned index=0;
    for(auto offset:{0x7Cu,0x80u}){
        const auto slide=mem_read32(hud+offset);const auto active=uint32_t(banner[index++]);mem_write32(slide,1);
        for(auto field:{4u,8u,0x10u})mem_write32(slide+field,f2u(banner[index++]));
        const auto target=mem_read32(slide+8);const auto fraction=u2f(mem_read32(slide+4));
        // An inactive/equal-target native slide returns before refreshing its
        // drawable position. Refresh once, including the fully hidden state.
        if(mem_read32(slide+4)==target)mem_write32(slide+8,f2u(fraction<1.f?1.f:0.f));
        call.r[3]=slide;call.ps0[1]=call.ps1[1]=0.;ssne_call(call,0x800AF7B8u);
        mem_write32(slide+8,target);mem_write32(slide,active);
    }
    return true;
}
