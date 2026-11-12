bool capture_native_weapon_bar(uint32_t actor,EntityState& e,std::string& error){
    const auto hud=mem_read32(actor+0x374);if(!readable(hud,0x3F0)){error="Native weapon bar unavailable";return false;}
    const bool active=mem_read32(hud+0x3E0)!=0;
    const auto target=active?int64_t(int32_t(mem_read32(hud+0x3E8))):-1;
    std::vector<int64_t> state{int64_t(active),active && target>=0?int64_t(int32_t(mem_read32(hud+0x3E4))):-1,target,active && target>=0?int64_t(mem_read32(hud+0x3EC)):0};
    for(unsigned i=0;i<12;++i){const auto row=hud+0x22C+i*36;const auto available=mem_read32(row+0x14);state.push_back(available);state.push_back(available?mem_read32(row+0x18):0);state.push_back(available?int64_t(int32_t(mem_read32(row+0x1C))):0);}
    e.properties["weapon_bar"]=std::move(state);
    std::vector<float> pose;for(unsigned i=0;i<12;++i)for(unsigned k=0;k<3;++k)pose.push_back(active?u2f(mem_read32(hud+0x22C+i*36+8+k*4)):0.f);
    e.properties["weapon_bar_pose"]=std::move(pose);return validate_native_weapon_bar(actor,e,error);
}
bool validate_native_weapon_bar(uint32_t actor,const EntityState& e,std::string& error){
    const auto it=e.properties.find("weapon_bar");const auto* v=it==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&it->second);const auto hud=readable(actor,0x790)?mem_read32(actor+0x374):0;
    if(e.kind!=EntityKind::Player || !v || v->size()!=40 || !readable(hud,0x3F0) || ((*v)[0]!=0 && (*v)[0]!=1) || (*v)[1]< -1 || (*v)[1]>=12 || (*v)[2]< -1 || (*v)[2]>=12 || (*v)[3]<0 || (*v)[3]>1){error="Invalid native weapon bar state";return false;}
    if(!(*v)[0] && ((*v)[1]!=-1 || (*v)[2]!=-1 || (*v)[3])){error="Inactive weapon bar has selection input";return false;}
    if(auto p=e.properties.find("weapon_bar_pose");p!=e.properties.end()){
        auto* pose=std::get_if<std::vector<float>>(&p->second);if(!pose || pose->size()!=36){error="Invalid weapon panel animation layout";return false;}
        for(unsigned i=0;i<pose->size();++i)if(!std::isfinite((*pose)[i]) || std::abs((*pose)[i])>10000.f || (i%3==2 && ((*pose)[i]<0 || (*pose)[i]>1))){error="Invalid weapon panel animation";return false;}
    }
    for(unsigned i=0;i<12;++i){const auto k=4+i*3;if(((*v)[k]!=0 && (*v)[k]!=1) || ((*v)[k+1]!=0 && (*v)[k+1]!=1) || (*v)[k+2]<0 || (*v)[k+2]>1000000000){error="Invalid native weapon bar item";return false;}if((*v)[k] && (!readable(mem_read32(hud+0x22C+i*36),0x38) || !readable(mem_read32(hud+0x230+i*36),0x38))){error="Native weapon bar widgets unavailable";return false;}}
    if(e.properties.contains("weapon_bar_pose")){
        for(unsigned i=0;i<12;++i)for(unsigned child=0;child<2;++child){
            const auto widget=mem_read32(hud+0x22C+i*36+child*4),table=readable(widget,0x38)?mem_read32(widget+0x34):0;
            if(!readable(table,0x10) || !ssne_find_func(mem_read32(table+0xC))){error="Native weapon panel update unavailable";return false;}
        }
        if(!ssne_find_func(0x800AB728u) || mem_read32(0x800AB728u)!=0x9421FFE0u){error="Original weapon panel position setter unavailable";return false;}
    }
    for(const auto [address,prologue]:{std::pair{0x800BDF80u,0x9421FFD8u},std::pair{0x800BE0BCu,0x9421FFF0u}})if(!ssne_find_func(address) || mem_read32(address)!=prologue){error="Original weapon bar lifecycle unavailable";return false;}
    return true;
}
bool apply_native_weapon_bar(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_weapon_bar(actor,e,error))return false;
    const auto& v=std::get<std::vector<int64_t>>(e.properties.at("weapon_bar"));const auto hud=mem_read32(actor+0x374);bool changed=false;
    for(unsigned i=0;i<12;++i){const auto row=hud+0x22C+i*36,k=4+i*3;for(unsigned j=0;j<3;++j){const auto p=row+0x14+j*4;changed|=mem_read32(p)!=uint32_t(v[k+j]);mem_write32(p,uint32_t(v[k+j]));}}
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    if(bool(mem_read32(hud+0x3E0))!=bool(v[0]) || (v[0] && changed)){call.r[3]=hud;call.r[4]=uint32_t(v[0]);ssne_call(call,0x800BDF80);}
    if(v[0] && v[2]>=0 && mem_read32(hud+0x3E8)!=uint32_t(v[2])){call.r[3]=hud;call.r[4]=uint32_t(v[2]);call.r[5]=uint32_t(v[3]);ssne_call(call,0x800BE0BC);}
    if(v[0]){mem_write32(hud+0x3E4,uint32_t(v[1]));mem_write32(hud+0x3E8,uint32_t(v[2]));mem_write32(hud+0x3EC,uint32_t(v[3]));}
    if(v[0] && e.properties.contains("weapon_bar_pose")){
        const auto& pose=std::get<std::vector<float>>(e.properties.at("weapon_bar_pose"));
        for(unsigned i=0;i<12;++i){const auto row=hud+0x22C+i*36;for(unsigned k=0;k<3;++k)mem_write32(row+8+k*4,f2u(pose[i*3+k]));
            // Commit the received panel animation without advancing the host
            // selection input or replaying any weapon action.
            for(unsigned child=0;child<2;++child){const auto widget=mem_read32(row+child*4),table=mem_read32(widget+0x34);call.r[3]=widget+uint32_t(int32_t(int16_t(mem_read16(table+8))));call.ps0[1]=call.ps1[1]=0.;ssne_call(call,mem_read32(table+0xC));}
            mem_write32(call.r[1]+0x60,f2u(pose[i*3]*(1.f-pose[i*3+2])));mem_write32(call.r[1]+0x64,f2u(pose[i*3+1]*(1.f-pose[i*3+2])));
            call.r[3]=mem_read32(row);call.r[4]=call.r[1]+0x60;ssne_call(call,0x800AB728u);
        }
    }
    return true;
}
