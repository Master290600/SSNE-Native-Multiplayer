// PAL 1.2.7 objective presentation. The host runs all pickups, scoring, bomb
// transfers, explosions and match expiry. Replicas receive logical carrier
// slots and scalar poses; guest pointers never travel over the connection.
bool versus_readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;}
uint32_t versus_flag(const NativeWorldSample& sample){
    static uint32_t object=0;static uint64_t epoch=0;
    if(epoch!=sample.epoch){epoch=sample.epoch;object=0;}
    for(const auto& entry:sample.unmapped)if(entry.vtable==0x80206C80u)object=entry.address;
    return versus_readable(object,0xD0) && mem_read32(object+0x48)==0x80206C80u?object:0;
}
constexpr std::array<unsigned,21> versus_flag_fields{0x20,0x24,0x28,0x78,0x7c,0x80,0x84,0x88,0x8c,0x90,0x94,0x98,0x9c,0xa0,0xa4,0xa8,0xac,0xb0,0xb4,0xb8,0xbc};
uint32_t versus_held_matrix(PPCContext& ctx,uint32_t actor){
    const auto model=mem_read32(actor+0x164),table=versus_readable(model,4)?mem_read32(model):0;
    if(!versus_readable(table,0x30) || !ssne_find_func(mem_read32(table+0x2c)))return 0;
    auto call=ctx;call.r[1]-=0x400;call.msr&=~0x8000u;mem_write32(call.r[1],ctx.r[1]);call.r[3]=model+int16_t(mem_read16(table+0x28));ssne_call(call,mem_read32(table+0x2c));
    return versus_readable(call.r[3],48)?call.r[3]:0;
}
void position_versus_timer(PPCContext& ctx){
    const auto scene=ssne::native_game_scene();
    if(!versus_readable(scene,0xA8) || !mem_read32(scene+0x94) || !ssne_find_func(0x800AB728))return;
    for(auto offset:{0x98u,0x9cu,0xa0u}){
        const auto widget=mem_read32(scene+offset);if(!versus_readable(widget,0x38))continue;
        const auto draw=mem_read32(widget);if(!versus_readable(draw,4) || !versus_readable(mem_read32(draw),0x38))continue;
        const float y=offset==0xa0?-202.f:-200.f;if(mem_read32(widget+8)==f2u(y))continue;
        std::array<uint8_t,0x1000> saved;const auto low=ctx.r[1]-0x1000;if(!versus_readable(low,saved.size()))return;
        std::memcpy(saved.data(),ssne::host_ptr(low),saved.size());
        auto call=ctx;call.r[1]-=0x200;call.msr&=~0x8000u;mem_write32(call.r[1],ctx.r[1]);mem_write32(call.r[1]+0x80,mem_read32(widget+4));mem_write32(call.r[1]+0x84,f2u(y));call.r[3]=widget;call.r[4]=call.r[1]+0x80;ssne_call(call,0x800AB728);
        std::memcpy(ssne::host_ptr(low),saved.data(),saved.size());
    }
}
void capture_versus(PPCContext& ctx,const NativeWorldSample& sample,Properties& mission){
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_TEST_VERSUS_OBJECTIVES") && sample.mode>=3){
        static uint64_t epoch=0;static unsigned frames=0;if(epoch!=sample.epoch){epoch=sample.epoch;frames=0;}
        const auto a=mem_read32(ssne::native_party_registry()),b=mem_read32(ssne::native_party_registry()+4);
        if(versus_readable(a,0x790) && versus_readable(b,0x790) && ssne::native_party_actor_occupied(a) && ssne::native_party_actor_occupied(b)){
            ++frames;auto invoke=[&](uint32_t fn,uint32_t actor,uint32_t arg){auto call=ctx;call.r[1]-=0x400;call.msr&=~0x8000u;mem_write32(call.r[1],ctx.r[1]);call.r[3]=actor;call.r[4]=arg;ssne_call(call,fn);};
            if(sample.mode==3)if(const auto flag=versus_flag(sample)){
                if(frames==120 || frames==420){const auto actor=frames==120?a:b;invoke(0x800B4A18,actor,flag);invoke(0x80080FE0,flag,0);ssne::log_message("[native-versus-test] flag collected slot="+std::to_string(frames==120?0:1));}
                if(frames==300 || frames==600){invoke(0x800B4A18,frames==300?a:b,0);
                    const auto scratch=ctx.r[1]-0x80;mem_write32(scratch,f2u(148.f));mem_write32(scratch+4,f2u(-6.99f));mem_write32(scratch+8,f2u(30.25f));
                    const auto table=mem_read32(flag+0x48);invoke(mem_read32(table+0xc4),flag+int16_t(mem_read16(table+0xc0)),scratch);invoke(0x80080DB4,flag,0);
                    ssne::log_message("[native-versus-test] flag dropped");}
            }
            if(sample.mode==4 && frames==180){for(auto actor:{a,b})if(mem_read32(actor+0x6a4))invoke(0x800B4B28,actor,0);invoke(0x800B4B28,b,1);mem_write32(ctx.r[13]-0x54a8,f2u(4.f));ssne::log_message("[native-versus-test] bomb transferred slot=1 fuse=4");}
        }
    }
    position_versus_timer(ctx);
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_TEST_IDLE_POSITION") && sample.mode==2){
        static bool placed=false;const auto actor=mem_read32(ssne::native_party_registry());
        if(!placed && versus_readable(actor,0x790) && ssne::native_party_actor_occupied(actor)){
            float x,y,z;if(std::sscanf(getenv("SSNE_TEST_IDLE_POSITION"),"%f,%f,%f",&x,&y,&z)==3){auto call=ctx;call.r[1]-=0x200;call.msr&=~0x8000u;mem_write32(call.r[1],ctx.r[1]);const auto p=call.r[1]+0x80;for(unsigned i=0;i<3;++i)mem_write32(p+i*4,f2u(i==0?x:i==1?y:z));call.r[3]=actor;call.r[4]=p;ssne_call(call,0x8009C300);placed=true;ssne::log_message("[idle-test] original position setter applied");}
        }
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_TEST_IDLE_POSITION") && sample.mode==2){
        const auto actor=mem_read32(ssne::native_party_registry());if(versus_readable(actor,0x790)){
            static std::ofstream trace("idle-test.csv");const auto pad=ssne::get_pad(0);
            trace<<sample.tick<<','<<std::setprecision(9)<<u2f(mem_read32(actor+0x20))<<','<<u2f(mem_read32(actor+0x24))<<','<<u2f(mem_read32(actor+0x28))<<','<<unsigned(pad.stick_x)<<','<<unsigned(pad.stick_y)<<','<<unsigned(pad.trigger_l)<<','<<unsigned(pad.trigger_r)<<','<<mem_read32(actor+0x340)<<'\n';
            if(sample.tick%60==0)trace.flush();
        }
    }
    std::vector<int64_t> carriers(4,0);std::vector<float> held(48,0.f);
    for(unsigned slot=0;slot<4;++slot){const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(versus_readable(actor,0x790) && sample.mode>=3){carriers[slot]=bool(mem_read32(actor+(sample.mode==3?0x694:0x6a4)));
            if(carriers[slot])if(const auto matrix=versus_held_matrix(ctx,actor))for(unsigned i=0;i<12;++i)held[slot*12+i]=u2f(mem_read32(matrix+i*4));
        }
    }
    mission["Session.Versus.HeldMatrices"]=std::move(held);
    mission["Session.Versus.Carriers"]=std::move(carriers);
    const auto clock=u2f(mem_read32(ctx.r[13]-0x54c4)),fuse=u2f(mem_read32(ctx.r[13]-0x54a8));
    mission["Session.Versus.Clocks"]=std::vector<float>{std::isfinite(clock)?clock:0.f,sample.mode==4 && std::isfinite(fuse)?fuse:0.f};
    if(sample.mode==3)if(const auto flag=versus_flag(sample)){
        std::vector<float> pose;for(auto offset:versus_flag_fields)pose.push_back(u2f(mem_read32(flag+offset)));
        if(std::all_of(pose.begin(),pose.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1000000.f;}))mission["Session.Versus.FlagPose"]=std::move(pose);
    }
}
bool apply_versus(PPCContext& ctx,const NativeWorldSample& local,const Properties& mission,std::string& error){
    if(local.mode<2 || local.mode>4)return true;
    auto fail=[&](const char* why){error=why;return false;};
    const auto c=mission.find("Session.Versus.Carriers"),t=mission.find("Session.Versus.Clocks");
    if(c==mission.end() || t==mission.end())return true; // Initial loading snapshots.
    const auto carriers=std::get_if<std::vector<int64_t>>(&c->second);const auto clocks=std::get_if<std::vector<float>>(&t->second);
    if(!carriers || carriers->size()!=4 || !clocks || clocks->size()!=2)return fail("Invalid versus objective state");
    for(auto v:*carriers)if(v!=0 && v!=1)return fail("Invalid versus carrier");
    for(auto v:*clocks)if(!std::isfinite(v) || std::abs(v)>1000000.f)return fail("Invalid versus clock");
    auto invoke=[&](uint32_t fn,uint32_t actor,uint32_t arg,float seconds=0.f){
        auto call=ctx;call.r[1]-=0x400;call.msr&=~0x8000u;mem_write32(call.r[1],ctx.r[1]);call.r[3]=actor;call.r[4]=arg;call.ps0[1]=call.ps1[1]=seconds;ssne_call(call,fn);
    };
    const auto h=mission.find("Session.Versus.HeldMatrices");const auto held=h==mission.end()?nullptr:std::get_if<std::vector<float>>(&h->second);
    if(!held || held->size()!=48)return fail("Invalid held objective pose");
    for(auto value:*held)if(!std::isfinite(value) || std::abs(value)>1000000.f)return fail("Invalid held objective coefficient");
    const auto flag=local.mode==3?versus_flag(local):0;
    for(unsigned slot=0;slot<4;++slot){const auto actor=mem_read32(ssne::native_party_registry()+slot*4);if(!versus_readable(actor,0x790))continue;
        if(local.mode==3 && flag && bool(mem_read32(actor+0x694))!=bool((*carriers)[slot])){
            invoke(0x800B4A18,actor,(*carriers)[slot]?flag:0);
            if((*carriers)[slot] && mem_read32(flag+0x60))invoke(0x80080FE0,flag,0);
        }
        if(local.mode==4){
            if(bool(mem_read32(actor+0x6a4))!=bool((*carriers)[slot]))invoke(0x800B4B28,actor,uint32_t((*carriers)[slot]));
            if((*carriers)[slot])invoke(0x800B4BC0,actor,0,std::max(0.f,(*clocks)[1]));
        }
        if((*carriers)[slot])if(const auto matrix=versus_held_matrix(ctx,actor))for(unsigned i=0;i<12;++i)mem_write32(matrix+i*4,f2u((*held)[slot*12+i]));
    }
    if(flag)if(const auto p=mission.find("Session.Versus.FlagPose");p!=mission.end()){
        const auto pose=std::get_if<std::vector<float>>(&p->second);if(!pose || pose->size()!=versus_flag_fields.size())return fail("Invalid flag pose");
        for(auto value:*pose)if(!std::isfinite(value) || std::abs(value)>1000000.f)return fail("Invalid flag pose coefficient");
        for(unsigned i=0;i<pose->size();++i)mem_write32(flag+versus_flag_fields[i],f2u((*pose)[i]));
        if(mem_read32(flag+0x60))invoke(0x80080DB4,flag,0);
    }
    mem_write32(ctx.r[13]-0x54c4,f2u((*clocks)[0]));mem_write32(ctx.r[13]-0x54a8,f2u((*clocks)[1]));
    // Original countdown routine with dt=0 updates only its native widgets.
    // Expiry and scene changes are solely the host's decision.
    const auto scene=ssne::native_game_scene();
    if((*clocks)[0]>0 && versus_readable(scene,0xA8) && !mem_read32(scene+0x80))invoke(0x80167EBC,scene,0);
    position_versus_timer(ctx);
    if(getenv("SSNE_TEST_HIDDEN")){static unsigned samples=0;if(++samples%60==0){unsigned mask=0,differences=0;for(unsigned slot=0;slot<4;++slot){mask|=unsigned((*carriers)[slot])<<slot;const auto actor=mem_read32(ssne::native_party_registry()+slot*4);if(local.mode>=3 && versus_readable(actor,0x790))differences+=bool(mem_read32(actor+(local.mode==3?0x694:0x6a4)))!=bool((*carriers)[slot]);}ssne::log_message("[native-versus-replica] mode="+std::to_string(local.mode)+" carriers="+std::to_string(mask)+" differences="+std::to_string(differences)+" clock="+std::to_string((*clocks)[0])+" fuse="+std::to_string((*clocks)[1]));}}
    return true;
}
