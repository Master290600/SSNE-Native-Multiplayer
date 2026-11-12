// Integration-driver commands are accepted only by an explicitly hidden test
// host. They dispatch original native handlers, never remote gameplay commands.
bool campaign_probe(PPCContext& context,const NativeWorldSample& sample,NativeWorld& world,
                    const std::string& level,const std::function<void(const std::string&)>& log) {
    const auto path=getenv("SSNE_COOP_CAMPAIGN_COMMAND");
    if(!getenv("SSNE_TEST_HIDDEN") || !path || sample.scene!=4)return false;
    auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;};
    auto text_at=[&](uint32_t p)->std::optional<std::string>{std::string s;for(unsigned i=0;i<65;++i){if(!readable(p+i,1))return {};const auto c=mem_read8(p+i);if(!c)return s;if(c<32 || c>126)return {};s+=char(c);}return {};};
    if(sample.tick%60==0){
        std::ofstream out("campaign-state.json.pending");
        out<<"{\"level\":\""<<level<<"\",\"epoch\":"<<sample.epoch<<",\"tick\":"<<sample.tick<<",\"objects\":[";
        std::map<uint32_t,unsigned> spawn_definitions;
        const auto owner=mem_read32(0x802DACC0),manager=readable(owner,0x29C)?mem_read32(owner+0x298):0,resource=readable(manager,8)?mem_read32(manager+4):0;
        auto node=readable(resource,0x44)?mem_read32(resource+0x40):0;std::set<uint32_t> nodes;
        while(readable(node,0x1C) && nodes.insert(node).second && nodes.size()<32768){
            const auto proto=mem_read32(node+0x10);
            if(mem_read32(node+0xC)==5000 && readable(proto,4) && mem_read32(proto)<61)spawn_definitions[mem_read32(node+0x14)]=mem_read32(ssne::game_address(0x8021AB2Cu)+mem_read32(proto)*4);
            node=mem_read32(node);
        }
        bool first=true;
        auto emit=[&](uint64_t id,uint32_t object,unsigned kind,unsigned definition){
            if(!first)out<<',';first=false;
            const auto vt=mem_read32(object+0x48);
            out<<"{\"id\":"<<id<<",\"address\":"<<object<<",\"kind\":"<<kind<<",\"definition\":"<<definition<<",\"vtable\":"<<vt<<",\"index\":"<<mem_read32(object+0x50)<<",\"position\":[";
            for(unsigned i=0;i<3;++i){if(i)out<<',';out<<u2f(mem_read32(object+0x20+i*4));}out<<']';
            if(kind==unsigned(EntityKind::Enemy) && definition==42)if(auto found=sample.entities.find(id);found!=sample.entities.end())if(auto points=found->second.properties.find("boss_weakpoints");points!=found->second.properties.end()){
                out<<",\"weakpoints\":[";const auto& values=std::get<std::vector<float>>(points->second);for(unsigned i=0;i<values.size();++i){if(i)out<<',';out<<values[i];}out<<']';
            }
            if(kind==unsigned(EntityKind::Enemy))out<<",\"health\":"<<u2f(mem_read32(object+0xCC));
            if(kind==unsigned(EntityKind::Player)){
                out<<",\"slot\":"<<mem_read32(object+0x17C)<<",\"health\":"<<u2f(mem_read32(object+0xCC))<<",\"dead\":"<<mem_read32(object+0x328)<<",\"weapon\":"<<mem_read32(object+0x5DC)<<",\"flag\":"<<bool(mem_read32(object+0x694))<<",\"bomb\":"<<bool(mem_read32(object+0x6A4));
                if(auto found=sample.entities.find(id);found!=sample.entities.end())for(const auto* key:{"weapon_owned_counts","ammo","ammo_maximum"}){
                    out<<",\""<<key<<"\":[";const auto& values=std::get<std::vector<int64_t>>(found->second.properties.at(key));for(unsigned i=0;i<values.size();++i){if(i)out<<',';out<<values[i];}out<<']';
                }
            }
            if(vt==0x80204E30u){const auto c=mem_read32(object+0x54);if(readable(c,0x18))if(auto found=spawn_definitions.find(mem_read32(c+0x14));found!=spawn_definitions.end())out<<",\"spawn_type\":"<<found->second;}
            if(vt==0x8020A750u){const auto c=mem_read32(object+0x54);if(readable(c,10)){const auto text=text_at(mem_read32(c+4));if(text)out<<",\"target\":\""<<*text<<"\",\"terminal\":"<<mem_read16(c+8);}}
            out<<'}';
        };
        for(const auto& [id,e]:sample.entities)emit(id,world.local_address(id),unsigned(e.kind),e.definition);
        for(const auto& e:sample.unmapped)emit(0,e.address,255,0);
        out<<"]}";out.close();std::remove("campaign-state.json");std::rename("campaign-state.json.pending","campaign-state.json");
    }
    static uint64_t accepted=0;uint64_t sequence=0;std::string action;
    std::ifstream input(path);if(!(input>>sequence>>action) || sequence<=accepted)return false;
    if(action=="empty_primary"){
        unsigned slot,weapon;if(!(input>>slot>>weapon) || slot>=ssne::native_party_capacity() || weapon>=12)return false;
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);std::string why;
        const auto before=native_owned_primary_ammo(context,actor,weapon,why);
        if(!before || !ssne_find_func(0x8009F458u))return false;
        const auto instance=mem_read32(actor+0x4BC+weapon*24),config=mem_read32(instance+0x1D0),inventory=mem_read32(instance+0x1B0);
        if(!readable(config,60) || !readable(inventory,0xDC))return false;
        const auto index=int32_t(mem_read32(config+0x38));if(index<0 || index>=18)return false;
        std::array<uint8_t,0x300> saved;std::memcpy(saved.data(),ssne::host_ptr(context.r[1]-0x300),saved.size());
        auto call=context;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.msr&=~0x8000u;
        // Original inventory setter: empty both native stocks, without changing
        // ownership, weapon state, trigger, target selection or audio handles.
        for(unsigned bank=0;bank<2;++bank){call.r[3]=inventory;call.r[4]=uint32_t(index);call.r[5]=bank;call.r[6]=0;ssne_call(call,0x8009F458u);}
        std::memcpy(ssne::host_ptr(context.r[1]-0x300),saved.data(),saved.size());accepted=sequence;
        if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=empty_primary slot="+std::to_string(slot)+" weapon="+std::to_string(weapon)+" before="+std::to_string(*before));return true;
    }else if(action=="player_place"){
        unsigned slot;std::array<float,3> position;if(!(input>>slot>>position[0]>>position[1]>>position[2]) || slot>=ssne::native_party_capacity())return false;
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(!readable(actor,0x790) || !ssne::native_party_actor_occupied(actor) || !ssne_find_func(0x8009C300))return false;
        for(auto v:position)if(!std::isfinite(v) || std::abs(v)>100000.f)return false;
        std::array<uint8_t,0x300> saved;std::memcpy(saved.data(),ssne::host_ptr(context.r[1]-0x300),saved.size());
        auto call=context;call.r[1]-=0x200;call.msr&=~0x8000u;mem_write32(call.r[1],context.r[1]);const auto p=call.r[1]+0x80;
        for(unsigned i=0;i<3;++i)mem_write32(p+i*4,f2u(position[i]));call.r[3]=actor;call.r[4]=p;ssne_call(call,0x8009C300);
        std::memcpy(ssne::host_ptr(context.r[1]-0x300),saved.data(),saved.size());accepted=sequence;
        if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=player_place slot="+std::to_string(slot));return true;
    }else if(action=="link"){
        std::string desired;input>>desired;
        for(const auto& item:sample.unmapped)if(item.vtable==0x8020A750u){
            const auto c=mem_read32(item.address+0x54);if(!readable(c,10))continue;
            const auto text=text_at(mem_read32(c+4));if(!text || (!desired.empty() && *text!=desired))continue;
            std::string target,error;
            if(activate_native_world_link(context,item.address,target,error,mem_read16(c+8)==1)){
                accepted=sequence;if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=link from="+level+" target="+target+" terminal="+std::to_string(mem_read16(c+8)));return true;
            }
            if(log)log("[campaign-command-error] "+error);
        }
    }else if(action=="drive"){
        int64_t reference=0;int slot=-1;if(!(input>>reference>>slot) || slot< -1 || slot>=int(ssne::native_party_capacity()))return false;
        const auto wrapper=native_script_reference_address(reference);if(!wrapper || !readable(*wrapper,0x8C) || mem_read32(*wrapper+0x48)!=0x80209BB8u)return false;
        const auto car=mem_read32(*wrapper+0x60);if(!readable(car,0x1D8))return false;
        const auto actor=slot<0?mem_read32(car+0x160):mem_read32(ssne::native_party_registry()+unsigned(slot)*4);if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)return false;
        std::vector<uint8_t> saved(0x3000);for(unsigned i=0;i<saved.size();++i)saved[i]=mem_read8(context.r[1]-0x3000+i);
        auto restore=[&]{for(unsigned i=0;i<saved.size();++i)mem_write8(context.r[1]-0x3000+i,saved[i]);};
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=slot<0?1:car;
        try{ssne_call(call,slot<0?0x800B5558u:0x800B54D8u);}catch(...){restore();throw;}restore();accepted=sequence;
        if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=drive slot="+std::to_string(slot)+" driver="+std::to_string(mem_read32(car+0x160)));return true;
    }else if(action=="dump"){
        std::ofstream ram("campaign-capture.ram",std::ios::binary);ram.write(reinterpret_cast<const char*>(ssne::g_ram),ssne::guest_ram_size());
        std::ofstream cpu("campaign-context.bin",std::ios::binary);cpu.write(reinterpret_cast<const char*>(&context),sizeof(context));accepted=sequence;
        if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=dump");
    }else if(action=="damage" || action=="node_damage"){
        uint64_t id=0;unsigned index=0;float amount=0;if(!(input>>id) || (action=="node_damage" && !(input>>index)) || !(input>>amount) || !std::isfinite(amount) || amount<=0 || amount>1000000)return false;
        const auto found=sample.entities.find(id);if(found==sample.entities.end() || found->second.kind!=EntityKind::Enemy)return false;
        auto object=world.local_address(id);
        if(action=="node_damage"){
            if(found->second.definition!=42 || index>=16)return false;object=mem_read32(object+0x1FC+index*4);
            if(!object){accepted=sequence;if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=damage retired=1");return false;}
            if(!readable(object,0x1C8) || mem_read32(object+0x48)!=0x801EAAF8u)return false;
        }
        const auto vt=mem_read32(object+0x48),method=mem_read32(vt+0x9C);if(!ssne_find_func(method))return false;
        std::array<uint8_t,0x1000> saved;for(unsigned i=0;i<saved.size();++i)saved[i]=mem_read8(context.r[1]-0x1000+i);
        auto restore=[&]{for(unsigned i=0;i<saved.size();++i)mem_write8(context.r[1]-0x1000+i,saved[i]);};
        const auto packet=context.r[1]-0x100;for(unsigned i=0;i<0x40;i+=4)mem_write32(packet+i,0);
        mem_write32(packet,f2u(amount));mem_write32(packet+8,mem_read32(ssne::native_party_registry()));mem_write32(packet+12,0);
        unsigned flags=0;if(input>>flags)mem_write32(packet+4,flags);
        for(unsigned i=0;i<3;++i)mem_write32(packet+16+i*4,mem_read32(object+0x20+i*4));
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=object+int16_t(mem_read16(vt+0x98));call.r[4]=packet;
        const auto health_offset=action=="node_damage"?0xFCu:0xCCu;const auto before=u2f(mem_read32(object+health_offset));try{ssne_call(call,method);}catch(...){restore();throw;}restore();accepted=sequence;
        if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=damage definition="+std::to_string(found->second.definition)+" before="+std::to_string(before)+" after="+std::to_string(u2f(mem_read32(object+health_offset))));return true;
    }else if(action=="event"){
        int64_t index=0;unsigned event=0;if(!(input>>index>>event))return false;
        const auto object=native_script_reference_address(index);if(!object || !*object)return false;
        const auto vt=mem_read32(*object+0x48),method=mem_read32(vt+0x44);
        if(event!=13 && !(vt==0x80209BB8u && (event==11 || event==12)))return false;
        if(!ssne_find_func(method) || !readable(context.r[1]-0x300,0x300))return false;
        std::array<uint8_t,0x300> saved;for(unsigned i=0;i<saved.size();++i)saved[i]=mem_read8(context.r[1]-0x300+i);
        auto restore=[&]{for(unsigned i=0;i<saved.size();++i)mem_write8(context.r[1]-0x300+i,saved[i]);};
        PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);const auto message=context.r[1]-0x60;
        for(unsigned i=0;i<0x40;i+=4)mem_write32(message+i,0);mem_write32(message,event);call.r[3]=*object;call.r[4]=message;
        try{ssne_call(call,method);}catch(...){restore();throw;}restore();accepted=sequence;
        if(log)log("[campaign-command] sequence="+std::to_string(sequence)+" action=event index="+std::to_string(index)+" vtable="+std::to_string(vt));
    }
    return false;
}
