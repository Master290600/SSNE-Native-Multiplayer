namespace {
bool vehicle_voice(uint32_t,unsigned);
constexpr unsigned vehicle_tag_count=20,vehicle_base_values=104;
constexpr std::array<uint32_t,6> vehicle_proxy_offsets{0xCC,0x214,0x218,0x21C,0x220,0x33C};
struct VehicleBinding {uint32_t wrapper=0,car=0,bones=0,animated=0;unsigned count=0,type=154;std::array<uint32_t,6> proxies{},roots{};};
bool vehicle_binding(int64_t reference,VehicleBinding& b,std::string& error){
    const auto wrapper=native_script_reference_address(reference);
    if(!wrapper || !readable(*wrapper,0x8C) || mem_read32(*wrapper+0x48)!=0x80209BB8u){error="Vehicle level reference unavailable";return false;}
    b.wrapper=*wrapper;b.car=mem_read32(b.wrapper+0x60);
    if(!b.car)return true;
    const auto vt=readable(b.car,0x1D8)?mem_read32(b.car+0x48):0;
    b.type=vt==0x80224D30u?155:vt==0x80224320u?158:154;
    if(!readable(b.car,b.type==155?0x384:b.type==158?0x448:0x458) || mem_read32(b.car+0x48)!=(b.type==155?0x80224D30u:b.type==158?0x80224320u:0x80225568u) || mem_read32(b.car+0x1D4)!=b.wrapper){error="Vehicle native class/owner mismatch";return false;}
    const auto driver=mem_read32(b.car+0x160);
    if(driver && (!readable(driver,0x790) || mem_read32(driver+0x48)!=0x802121E8u || mem_read32(driver+0x6C4)!=b.car)){error="Local vehicle driver is incomplete";return false;}
    // Native Hide releases the body/wheel proxies but keeps the car and its
    // physics allocation. Clevel5_1 starts with precisely this valid state.
    if(!driver && !mem_read32(b.wrapper+0x7C) && !mem_read32(b.car+0x130)){
        bool empty=true;for(unsigned i=0;i<(b.type==155?2u:b.type==158?1u:5u);++i)empty&=mem_read32(b.car+(b.type==155?0xCC+i*4:vehicle_proxy_offsets[i]))==0;
        if(empty){b.car=0;return true;}
    }
    for(unsigned i=0;i<(b.type==155?2u:b.type==158?1u:6u);++i){
        const auto proxy=mem_read32(b.car+(b.type==155?0xCC+i*4:vehicle_proxy_offsets[i]));
        if(!readable(proxy,0x38) || mem_read32(proxy)!=0x8021E7F8u || !readable(mem_read32(proxy+4),48) || mem_read8(proxy+0x2D)>1){error="Vehicle render binding unavailable";return false;}
        b.proxies[i]=proxy;b.roots[i]=mem_read32(proxy+4);
    }
    const auto model=mem_read32(b.proxies[b.type==155?1:0]+0x20);b.animated=model;
    if(!readable(model,0xB0) || mem_read32(model)!=0x8021D668u){error="Vehicle animated body unavailable";return false;}
    b.count=mem_read32(model+0x7C);b.bones=mem_read32(model+0x78);
    if(!b.count || b.count>128 || !readable(b.bones,b.count*48)){error="Vehicle body pose unavailable";return false;}
    return true;
}
const std::vector<int64_t>* vehicle_tags(const Properties& p){auto i=p.find("Session.Vehicles.Tags");return i==p.end()?nullptr:std::get_if<std::vector<int64_t>>(&i->second);}
const std::vector<float>* vehicle_values(const Properties& p){auto i=p.find("Session.Vehicles.Values");return i==p.end()?nullptr:std::get_if<std::vector<float>>(&i->second);}
bool vehicle_payload(const Properties& p,std::string& error){
    const auto t=vehicle_tags(p);const auto v=vehicle_values(p);
    if(!t || !v || t->size()%vehicle_tag_count || t->size()>16*vehicle_tag_count){error="Incomplete vehicle snapshot";return false;}
    size_t floats=0;std::set<int64_t> references,drivers;
    for(size_t n=0;n<t->size();n+=vehicle_tag_count){
        const auto* a=t->data()+n;VehicleBinding b;
        if(a[0]<=0 || a[0]>32767 || !references.insert(a[0]).second || !vehicle_binding(a[0],b,error))return false;
        for(unsigned i:{1u,2u,3u,5u})if(a[i]<0 || a[i]>1){error="Invalid vehicle state flag";return false;}
        if(a[4]<-1 || a[4]>=ssne::native_party_capacity() || (a[4]>=0 && !drivers.insert(a[4]).second) || bool(a[5])!=(a[4]>=0) || (a[7]!=154 && a[7]!=155 && a[7]!=158)){error="Invalid vehicle driver/type";return false;}
        if(!a[1]){if(a[2] || a[3] || a[4]!=-1 || a[5] || a[6] || std::any_of(a+8,a+20,[](int64_t x){return x!=0;})){error="Invalid absent vehicle state";return false;}continue;}
        if(a[7]!=b.type || a[6]<1 || a[6]>128 || (b.car?a[6]!=b.count:!mem_read32(b.wrapper+0x60))){error="Authoritative vehicle pose layout differs";return false;}
        if(a[4]>=0){const auto actor=mem_read32(ssne::native_party_registry()+unsigned(a[4])*4);if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || mem_read32(actor+0x17C)!=a[4] || !validate_native_movement_mode(actor,3,error)){error="Vehicle driver binding unavailable";return false;}}
        for(unsigned i=0;i<6;++i)if(a[8+i]<0 || a[8+i]>1 || a[14+i]<0 || uint64_t(a[14+i])>UINT32_MAX){error="Invalid vehicle visibility";return false;}
        floats+=vehicle_base_values+size_t(a[6])*12;
    }
    if(floats!=v->size()){error="Vehicle pose length differs";return false;}
    for(float x:*v)if(!std::isfinite(x) || std::abs(x)>1000000){error="Invalid vehicle pose value";return false;}
    size_t fuel_offset=0;
    for(size_t n=0;n<t->size();n+=vehicle_tag_count)if((*t)[n+1]){
        // +134/+138 are the original current/capacity pair. A zero capacity
        // would feed NaN/Inf into the native status bar setter on the client.
        if((*v)[fuel_offset+7]<=0){error="Invalid vehicle fuel capacity";return false;}
        fuel_offset+=vehicle_base_values+size_t((*t)[n+6])*12;
    }
    if(auto it=p.find("Session.Vehicles.Audio");it!=p.end()){
        const auto* audio=std::get_if<std::vector<float>>(&it->second);
        if(!audio || audio->size()!=t->size()/vehicle_tag_count*2){error="Invalid vehicle audio length";return false;}
        for(unsigned i=0;i<audio->size()/2;++i){const auto phase=(*audio)[i*2],pitch=(*audio)[i*2+1];
            if((phase!=0 && phase!=1 && phase!=2) || !std::isfinite(pitch) || pitch<.05f || pitch>8.f || (phase && (!(*t)[i*vehicle_tag_count+1] || (*t)[i*vehicle_tag_count+4]<0))){error="Invalid vehicle audio state";return false;}}
    }
    if(auto item=p.find("Session.Vehicles.ExtraAudio");item!=p.end()){
        const auto* extra=std::get_if<std::vector<int64_t>>(&item->second);
        if(!extra || extra->size()!=t->size()/vehicle_tag_count){error="Invalid combine audio count";return false;}
        for(unsigned i=0;i<extra->size();++i)if((*extra)[i]<0 || (*extra)[i]>3 || ((*extra)[i] && ((*t)[i*vehicle_tag_count+7]!=155 || !(*t)[i*vehicle_tag_count+1]))){error="Invalid combine audio state";return false;}
    }
    return true;
}
}
bool capture_native_vehicles(Properties& p,std::string& error){
    std::vector<int64_t> tags;std::vector<float> values,audio;std::vector<int64_t> extra_audio;
    const auto table=mem_read32(0x802DB7C4),count=mem_read32(0x802DB7C8);
    if(count>32768 || !readable(table,count*8)){error="Vehicle level map unavailable";return false;}
    for(unsigned n=0;n<count;++n){const auto wrapper=mem_read32(table+n*8+4);
        if(!readable(wrapper,0x8C) || mem_read32(wrapper+0x48)!=0x80209BB8u)continue;
        VehicleBinding b;const auto reference=mem_read16(table+n*8);if(!vehicle_binding(reference,b,error))return false;
        const auto at=tags.size();tags.resize(at+vehicle_tag_count);auto* a=tags.data()+at;a[0]=reference;a[4]=-1;a[7]=b.type;
        audio.insert(audio.end(),{0,1});extra_audio.push_back(b.car && b.type==155?unsigned(vehicle_voice(mem_read32(b.car+0x37C),468))|(unsigned(vehicle_voice(mem_read32(b.car+0x380),469))<<1):0);if(!b.car)continue;
        a[1]=1;a[2]=mem_read32(b.car+0x130);a[3]=mem_read32(wrapper+0x7C);a[5]=mem_read32(b.car+0x12C);a[6]=b.count;
        const auto driver=mem_read32(b.car+0x160);
        if(driver){if(!readable(driver,0x790) || mem_read32(driver+0x48)!=0x802121E8u || mem_read32(driver+0x6C4)!=b.car){error="Host vehicle driver is incomplete";return false;}a[4]=mem_read32(driver+0x17C);}
        const auto phase=mem_read32(b.car+0x1D0);audio[audio.size()-2]=float(phase);
        if(phase==2 && b.type!=155){const auto physics=mem_read32(b.car+0x1EC);
            if(!readable(physics,0x230)){error="Vehicle engine parameters unavailable";return false;}
            const float low=u2f(mem_read32(b.car+0x31C)),high=u2f(mem_read32(b.car+0x320)),range=u2f(mem_read32(physics+0x21C));
            if(!(range>0)){error="Invalid vehicle engine pitch range";return false;}
            audio.back()=low+(u2f(mem_read32(physics+0x22C))-u2f(mem_read32(physics+0x228)))*(high-low)/range;
        }
        auto append=[&](uint32_t address,unsigned floats){for(unsigned i=0;i<floats;++i)values.push_back(u2f(mem_read32(address+i*4)));};
        append(b.car+0x20,6);append(b.car+0x134,2);append(b.car+0x164,24);
        for(unsigned i=0;i<6;++i){if(b.proxies[i]){a[8+i]=mem_read8(b.proxies[i]+0x2D);a[14+i]=mem_read32(b.proxies[i]+0x30);append(b.roots[i],12);}else values.insert(values.end(),12,0.f);}
        append(b.bones,b.count*12);
    }
    p["Session.Vehicles.ExtraAudio"]=std::move(extra_audio);p["Session.Vehicles.Tags"]=std::move(tags);p["Session.Vehicles.Values"]=std::move(values);p["Session.Vehicles.Audio"]=std::move(audio);return vehicle_payload(p,error);
}
bool validate_native_vehicles(const Properties& p,const std::map<EntityId,EntityState>& entities,std::string& error){
    if(!vehicle_payload(p,error))return false;
    const auto& t=*vehicle_tags(p);std::set<int64_t> drivers;
    for(size_t n=0;n<t.size();n+=vehicle_tag_count)if(t[n+4]>=0)drivers.insert(t[n+4]);
    std::set<int64_t> seen;
    for(const auto& [id,e]:entities)if(e.kind==EntityKind::Player){
        const auto slot=e.properties.find("player_slot"),mode=e.properties.find("movement_mode");
        const auto s=slot==e.properties.end()?nullptr:std::get_if<int64_t>(&slot->second);const auto m=mode==e.properties.end()?nullptr:std::get_if<int64_t>(&mode->second);
        if(!s || !m || (drivers.contains(*s)!=(*m==3))){error="Player and vehicle ownership disagree";return false;}seen.insert(*s);
        if(drivers.contains(*s))for(const auto& [key,value]:e.properties){
            if((key.starts_with("body") && key.find("_bone_")!=std::string::npos) || key=="body_weapon_resource" || key.starts_with("render_row") || (key.starts_with("body_render") && key.find("_row")!=std::string::npos)){error="Driving player has a walking body pose";return false;}
        }
    }
    for(auto s:drivers)if(!seen.contains(s)){error="Vehicle driver is missing from world state";return false;}
    return true;
}
bool prepare_native_vehicle_models(PPCContext& context,const Properties& p,std::string& error){
    if(!context_ok(context) || !vehicle_payload(p,error))return false;
    StackScratch scratch(context);const auto& tags=*vehicle_tags(p);
    for(size_t n=0;n<tags.size();n+=vehicle_tag_count){
        VehicleBinding b;if(!vehicle_binding(tags[n],b,error))return false;
        const bool wanted=tags[n+1]!=0;if(wanted==bool(b.car))continue;
        const auto car=mem_read32(b.wrapper+0x60);if(!car){error="Vehicle model lifecycle has no native car";return false;}
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
        // The original wrapper's Show/Hide owns all mesh allocations. Release
        // an old driver through its native action before hiding the car.
        if(!wanted && mem_read32(car+0x160)){call.r[3]=mem_read32(car+0x160);call.r[4]=1;ssne_call(call,0x800B5558);}
        call.r[3]=b.wrapper;ssne_call(call,wanted?0x80086064u:0x800860B8u);
        VehicleBinding after;if(!vehicle_binding(tags[n],after,error))return false;
        if(wanted!=bool(after.car) || (wanted && after.count!=tags[n+6])){error="Original vehicle model lifecycle differs";return false;}
    }
    return true;
}
bool apply_native_vehicle_drivers(PPCContext& context,const Properties& p,const std::map<EntityId,EntityState>& entities,std::string& error){
    if(!context_ok(context) || !validate_native_vehicles(p,entities,error))return false;
    const auto& t=*vehicle_tags(p);StackScratch scratch(context);
    // Release old drivers first, including transfers between vehicles. The
    // original player action owns controller, HUD, view and weapon lifetimes.
    for(size_t n=0;n<t.size();n+=vehicle_tag_count){VehicleBinding b;if(!vehicle_binding(t[n],b,error))return false;if(!b.car)continue;
        const auto old=mem_read32(b.car+0x160),wanted=t[n+4]>=0?mem_read32(ssne::native_party_registry()+unsigned(t[n+4])*4):0;
        if(old && old!=wanted){if(!readable(old,0x790) || mem_read32(old+0x48)!=0x802121E8u || mem_read32(old+0x6C4)!=b.car){error="Local vehicle driver is incomplete";return false;}
            auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=old;call.r[4]=1;ssne_call(call,0x800B5558);}
    }
    for(size_t n=0;n<t.size();n+=vehicle_tag_count)if(t[n+4]>=0){VehicleBinding b;if(!vehicle_binding(t[n],b,error))return false;
        const auto actor=mem_read32(ssne::native_party_registry()+unsigned(t[n+4])*4);
        if(mem_read32(b.car+0x160)!=actor){auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=b.car;ssne_call(call,0x800B54D8);}
        if(mem_read32(b.car+0x160)!=actor || mem_read32(actor+0x6C4)!=b.car || mem_read32(actor+0x340)!=3){error="Original vehicle entry did not complete";return false;}
    }
    return true;
}
bool apply_native_vehicle_poses(const Properties& p,std::string& error){
    if(!vehicle_payload(p,error))return false;
    const auto& t=*vehicle_tags(p);const auto& v=*vehicle_values(p);size_t offset=0;
    for(size_t n=0;n<t.size();n+=vehicle_tag_count){VehicleBinding b;if(!vehicle_binding(t[n],b,error))return false;
        if(!t[n+1]){if(b.car)for(auto proxy:b.proxies)if(proxy){mem_write8(proxy+0x2D,0);mem_write32(proxy+0x30,0);}continue;}
        mem_write32(b.car+0x130,unsigned(t[n+2]));mem_write32(b.wrapper+0x7C,unsigned(t[n+3]));
        auto write=[&](uint32_t a,unsigned count){for(unsigned k=0;k<count;++k)mem_write32(a+k*4,f2u(v[offset++]));};
        write(b.car+0x20,6);write(b.car+0x134,2);write(b.car+0x164,24);
        for(unsigned i=0;i<6;++i){if(b.proxies[i]){write(b.roots[i],12);mem_write8(b.proxies[i]+0x2D,unsigned(t[n+8+i]));mem_write32(b.proxies[i]+0x30,uint32_t(t[n+14+i]));}else offset+=12;}
        const auto first_bone=offset;write(b.bones,b.count*12);
        if(ssne::replica_simulation()){
            const std::vector<float> pose(v.begin()+first_bone,v.begin()+offset);
            if(!ssne::replica_enemy_pose(b.car,b.animated,pose)){error="Vehicle received body pose registration failed";return false;}
        }
        for(unsigned axis=0;axis<3;++axis)mem_write32(b.wrapper+0x20+axis*4,mem_read32(b.car+0x20+axis*4));
    }
    return true;
}
bool prepare_native_vehicle_presentation(const Properties& p,NativeVehiclePresentation& out,std::string& error){
    if(!p.contains("Session.Vehicles.Tags") && !p.contains("Session.Vehicles.Values"))return true;
    if(!vehicle_payload(p,error))return false;
    const auto& t=*vehicle_tags(p);const auto& v=*vehicle_values(p);size_t offset=0;
    for(size_t n=0;n<t.size();n+=vehicle_tag_count){if(!t[n+1])continue;VehicleBinding b;if(!vehicle_binding(t[n],b,error))return false;
        auto stage=[&](uint32_t a,unsigned count){for(unsigned k=0;k<count;++k)out.words.emplace_back(a+k*4,v[offset++]);};
        stage(b.car+0x20,6);offset+=2;stage(b.car+0x164,24);
        for(auto root:b.roots)if(root)stage(root,12);else offset+=12;
        const auto first=offset;stage(b.bones,b.count*12);
        out.poses.push_back({b.car,b.animated,{v.begin()+first,v.begin()+offset}});
    }
    return true;
}
namespace {
bool vehicle_voice(uint32_t handle,unsigned sound){
    const auto record=0x802D69C8u+(handle&63u)*192u,resource=0x8023AD70u+sound*24u;
    return (handle&63u)<48 && mem_read32(record) && mem_read32(record+0xAC)==handle &&
        mem_read32(resource+8)==1 && readable(mem_read32(resource+12),4) && mem_read32(record+0xB0)==mem_read32(mem_read32(resource+12));
}
bool vehicle_engine_voice(uint32_t handle){return vehicle_voice(handle,458);}
void stop_vehicle_engine(PPCContext& context,uint32_t car){
    const auto handle=mem_read32(car+0x328);
    if(vehicle_engine_voice(handle)){auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=handle;ssne_call(call,0x8010EDFC);}
    mem_write32(car+0x328,UINT32_MAX);
}
}
bool update_native_vehicle_audio(PPCContext& context,const Properties& p,std::string& error){
    if(!context_ok(context) || !vehicle_payload(p,error))return false;
    const auto it=p.find("Session.Vehicles.Audio");if(it==p.end())return true; // Saved older states remain readable.
    const auto& audio=std::get<std::vector<float>>(it->second);const auto& tags=*vehicle_tags(p);StackScratch scratch(context);
    if(tags.empty())return true;
    const auto resource=0x8023AD70u+458u*24u;
    if(mem_read32(resource+8)!=1 || !readable(mem_read32(resource+12),4)){error="Native vehicle engine loop resource differs";return false;}
    for(size_t n=0;n<tags.size();n+=vehicle_tag_count){VehicleBinding b;if(!vehicle_binding(tags[n],b,error))return false;if(!b.car)continue;
        if(b.type==155){
            const auto extra=p.find("Session.Vehicles.ExtraAudio");const auto bits=extra==p.end()?0u:unsigned(std::get<std::vector<int64_t>>(extra->second)[n/vehicle_tag_count]);
            for(unsigned lane=0;lane<2;++lane){const auto offset=0x37Cu+lane*4,sound=468u+lane;auto handle=mem_read32(b.car+offset);const bool active=vehicle_voice(handle,sound);
                if(!(bits&(1u<<lane))){if(active){auto call=context;call.r[3]=handle;ssne_call(call,0x8010EDFC);}mem_write32(b.car+offset,UINT32_MAX);}
                else if(!active){auto call=context;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=sound;call.r[4]=b.car+0x20;ssne_call(call,0x8010ED14);mem_write32(b.car+offset,call.r[3]);}
            }
            continue;
        }
        const auto phase=unsigned(audio[n/vehicle_tag_count*2]);mem_write32(b.car+0x1D0,phase);
        if(phase!=2){stop_vehicle_engine(context,b.car);continue;}
        auto handle=mem_read32(b.car+0x328);
        if(!vehicle_engine_voice(handle)){
            auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=458;call.r[4]=b.car+0x20;
            ssne_call(call,0x8010ED14);handle=call.r[3];mem_write32(b.car+0x328,handle);
            // A full native voice pool is temporary; retry without blocking world presentation.
            if(!vehicle_engine_voice(handle))continue;
        }
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=mem_read32(0x802ABAB0u);call.r[4]=handle;
        call.ps0[1]=call.ps1[1]=audio[n/vehicle_tag_count*2+1];ssne_call(call,0x8015859C);
    }
    return true;
}
bool stop_native_vehicle_audio(PPCContext& context,std::string& error){
    if(!context_ok(context)){error="Vehicle audio stop context unavailable";return false;}StackScratch scratch(context);
    const auto table=mem_read32(0x802DB7C4),count=mem_read32(0x802DB7C8);
    if(count>32768 || !readable(table,count*8))return true; // The native scene has already unloaded.
    for(unsigned i=0;i<count;++i){const auto wrapper=mem_read32(table+i*8+4);if(!readable(wrapper,0x8C) || mem_read32(wrapper+0x48)!=0x80209BB8u)continue;
        const auto car=mem_read32(wrapper+0x60);if((readable(car,0x458) && mem_read32(car+0x48)==0x80225568u) || (readable(car,0x448) && mem_read32(car+0x48)==0x80224320u))stop_vehicle_engine(context,car);else if(readable(car,0x384) && mem_read32(car+0x48)==0x80224D30u)for(unsigned lane=0;lane<2;++lane){const auto offset=0x37Cu+lane*4,handle=mem_read32(car+offset);if(vehicle_voice(handle,468+lane)){auto call=context;call.r[3]=handle;ssne_call(call,0x8010EDFC);}mem_write32(car+offset,UINT32_MAX);}
    }
    return true;
}

// The original driving-controller tick updates power-up slot6 from the car's
// current/capacity pair. Replica clients do not run that controller/physics
// tick. Refresh only its original HUD setter after a validated snapshot commit.
bool update_native_vehicle_huds(PPCContext& context,const Properties& p,std::string& error){
    if(!context_ok(context) || !vehicle_payload(p,error))return false;
    if(!ssne_find_func(0x800C1E00u) || mem_read32(0x800C1E00u)!=0x9421FFE8u){error="Native vehicle fuel HUD setter unavailable";return false;}
    const auto& tags=*vehicle_tags(p);StackScratch scratch(context);
    for(size_t n=0;n<tags.size();n+=vehicle_tag_count)if(tags[n+1] && tags[n+4]>=0){
        VehicleBinding b;if(!vehicle_binding(tags[n],b,error))return false;
        const auto actor=mem_read32(ssne::native_party_registry()+unsigned(tags[n+4])*4);
        if(mem_read32(b.car+0x160)!=actor || mem_read32(actor+0x340)!=3 || !mem_read32(actor+0x67C)){error="Native vehicle fuel HUD owner unavailable";return false;}
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
        call.r[3]=actor;call.r[4]=6;call.ps0[1]=call.ps1[1]=double(float(u2f(mem_read32(b.car+0x134))/u2f(mem_read32(b.car+0x138))));
        ssne_call(call,0x800C1E00u);
    }return true;
}
