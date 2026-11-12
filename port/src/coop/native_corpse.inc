// Included in native_lifecycle.cpp, inside port::coop.
bool validate_native_corpse_creation(const EntityState& e,std::string& error){
    auto fail=[&](const char* why){error=why;return false;};
    if(e.kind!=EntityKind::Physics || e.definition!=3 || !e.properties.contains("corpse_definition"))return fail("Invalid corpse class");
    const auto def=std::get_if<int64_t>(&e.properties.at("corpse_definition"));
    if(!def || *def<0 || *def>60 || prototype(unsigned(*def))<0)return fail("Corpse has no original enemy prototype");
    for(const auto& [key,value]:e.properties){
        if(key=="corpse_definition")continue;
        if(key=="position" || key=="spawn_position" || key=="enemy_rotation" || key=="enemy_scale"){
            const auto p=std::get_if<std::array<float,3>>(&value);if(!p)return fail("Invalid corpse transform type");
            for(auto v:*p)if(!std::isfinite(v) || std::abs(v)>1000000 || (key=="enemy_scale" && (v<=0 || v>100)))return fail("Invalid corpse transform");
        }else if(key=="corpse_flags" || key=="corpse_mode"){
            const auto n=std::get_if<int64_t>(&value);if(!n || *n<0 || uint64_t(*n)>(key=="corpse_mode"?2u:UINT32_MAX))return fail("Invalid corpse flags/mode");
        }else if(key=="corpse_lifecycle"){
            const auto v=std::get_if<std::vector<float>>(&value);if(!v || v->size()!=13)return fail("Invalid corpse lifecycle");
            for(unsigned i=0;i<v->size();++i)if(!std::isfinite((*v)[i]) || std::abs((*v)[i])>1000000){error="Invalid corpse lifecycle coefficient index="+std::to_string(i)+" value="+std::to_string((*v)[i]);return false;}
            for(unsigned i=10;i<13;++i)if((*v)[i]!=std::floor((*v)[i]) || (*v)[i]<0 || (*v)[i]>(i==10?1.f:i==11?4.f:3.f))return fail("Invalid corpse presentation phase");
        }else if(key=="corpse_dissolve_matrices" || key.starts_with("corpse_particles") || key=="corpse_ghost_matrix"){
            const auto v=std::get_if<std::vector<float>>(&value);if(!v)return fail("Invalid corpse visual type");
            for(auto f:*v)if(!std::isfinite(f) || std::abs(f)>1000000)return fail("Invalid corpse visual coefficient");
            if(key=="corpse_ghost_matrix"){if(!v->empty() && v->size()!=12)return fail("Invalid corpse ghost matrix");}
            else if(v->empty() || (*v)[0]<0 || (*v)[0]>(key.starts_with("corpse_particles")?511.f:256.f) || (*v)[0]!=std::floor((*v)[0]) || v->size()!=1+size_t((*v)[0])*(key.starts_with("corpse_particles")?8u:13u))return fail("Invalid corpse visual count");
            if(key=="corpse_dissolve_matrices")for(unsigned i=0;i<unsigned((*v)[0]);++i)if((*v)[1+i*13]!=0 && (*v)[1+i*13]!=1)return fail("Invalid dissolve matrix presence");
            if(key.starts_with("corpse_particles"))for(unsigned i=0;i<unsigned((*v)[0]);++i)for(unsigned k=3;k<8;++k){auto n=(*v)[1+i*8+k];if(n!=std::floor(n) || n<0 || n>(k==7?65535.f:255.f))return fail("Invalid corpse particle colour/size");}
        }else if(key=="corpse_particle_count"){const auto n=std::get_if<int64_t>(&value);if(!n || *n<0 || *n>16384)return fail("Invalid corpse particle total");
        }else if(key=="corpse_matrix"){
            const auto m=std::get_if<std::vector<float>>(&value);if(!m || m->size()!=12)return fail("Invalid corpse world matrix");
            for(auto f:*m)if(!std::isfinite(f) || std::abs(f)>1000000)return fail("Invalid corpse matrix coefficient");
        }else if(key=="enemy_bone_count" || key=="enemy_bone_layout" || key=="enemy_bone_matrices"){
            const auto n=e.properties.find("enemy_bone_count"),h=e.properties.find("enemy_bone_layout"),m=e.properties.find("enemy_bone_matrices");
            if(n==e.properties.end() || h==e.properties.end() || m==e.properties.end() || !std::holds_alternative<int64_t>(n->second) || !std::holds_alternative<int64_t>(h->second) || !std::holds_alternative<std::vector<float>>(m->second))return fail("Incomplete corpse skeleton");
            const auto count=std::get<int64_t>(n->second);const auto& values=std::get<std::vector<float>>(m->second);if(count<1 || count>256 || values.size()!=size_t(count)*12)return fail("Invalid corpse bone count");
            for(auto v:values)if(!std::isfinite(v) || std::abs(v)>10000)return fail("Invalid corpse bone pose");
        }else if(render_field(key)){if(!validate_render_metadata(e,error))return false;}
        else return fail("Unsupported corpse property");
    }
    for(const auto* key:{"position","spawn_position","corpse_flags","corpse_mode","corpse_matrix"})if(!e.properties.contains(key))return fail("Incomplete corpse presentation");
    if(e.properties.contains("corpse_dissolve_matrices")){for(auto key:{"corpse_lifecycle","corpse_ghost_matrix","corpse_particle_count","corpse_particles0"})if(!e.properties.contains(key))return fail("Incomplete corpse dissolve snapshot");}
    if(e.properties.contains("corpse_particle_count")){const auto total=unsigned(std::get<int64_t>(e.properties.at("corpse_particle_count")));for(unsigned start=0;start<std::max(total,1u);start+=511){auto it=e.properties.find("corpse_particles"+std::to_string(start/511));if(it==e.properties.end() || !std::holds_alternative<std::vector<float>>(it->second) || std::get<std::vector<float>>(it->second)[0]!=float(std::min(511u,total-start)))return fail("Incomplete corpse particle chunks");}}
    return true;
}
uint32_t create_native_corpse(PPCContext& context,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_corpse_creation(e,error))return 0;
    for(auto address:{0x8008CDD4u,0x8008EE9Cu,0x8008E0E4u,0x80116084u,0x8009B288u})if(!ssne_find_func(address)){error="Original corpse lifecycle unavailable";return 0;}
    EntityState seed;seed.id=e.id;seed.kind=EntityKind::Enemy;seed.definition=uint32_t(std::get<int64_t>(e.properties.at("corpse_definition")));seed.properties["position"]=e.properties.at("position");seed.properties["spawn_position"]=e.properties.at("position");
    const auto source=create_native_enemy(context,seed,error);if(!source)return 0;
    const auto table=mem_read32(source+0x48);
    // Use the original actor's local geometry descriptor. No host addresses,
    // death callback, damage, score award or scripted death is replayed.
    if(mem_read32(source+0x38) || !readable(table,0x368) || !ssne_find_func(mem_read32(table+0x25C)) || !ssne_find_func(mem_read32(table+0x364))){error="Corpse geometry source has unverified lifecycle";return 0;}
    if(!retire_native_enemy_auxiliary(context,source,error))return 0;
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    auto method=[&](uint32_t offset,uint32_t arg=0){call.r[3]=source+uint32_t(int32_t(int16_t(mem_read16(table+offset-4))));call.r[4]=arg;ssne_call(call,mem_read32(table+offset));return call.r[3];};
    method(0x364,1);
    const auto descriptor=method(0x25C);
    if(!readable(descriptor,0x40)){call.r[3]=source;ssne_call(call,0x8009B288);error="Original corpse geometry descriptor unavailable";return 0;}
    call.r[3]=mem_read32(context.r[13]-0x6DE0);call.r[4]=0x188;ssne_call(call,0x80116084);const auto body=call.r[3];
    if(!readable(body,0x188)){call.r[3]=source;ssne_call(call,0x8009B288);error="Original corpse allocation failed";return 0;}
    mem_write32(context.r[13]-0x6DF0,mem_read32(context.r[13]-0x6DF0)-mem_read32(body-0x18));
    call.r[3]=body;call.r[4]=seed.definition;ssne_call(call,0x8008CDD4);
    call.r[3]=body;call.r[4]=uint32_t(std::get<int64_t>(e.properties.at("corpse_mode")));ssne_call(call,0x8008EE9C);
    call.r[3]=body;call.r[4]=descriptor;call.r[5]=0;ssne_call(call,0x8008E0E4);
    // Same ownership transfer as 80092C68: the corpse keeps the geometry and
    // the temporary actor no longer owns its old renderer handles.
    mem_write32(source+0x124,0);mem_write32(source+0xFC,0);call.r[3]=source;ssne_call(call,0x8009B288);
    if(mem_read32(body+0x48)!=0x8020C6B8u || !readable(mem_read32(body+0xF4),0xB0)){std::string cleanup;remove_native_entity(context,body,cleanup);error="Original corpse did not acquire its geometry";return 0;}
    return body;
}
bool apply_native_corpse_lifecycle(PPCContext& context,uint32_t body,const EntityState& e,std::string& error){
    if(!e.properties.contains("corpse_lifecycle"))return true;
    if(!context_ok(context) || !readable(body,0x188) || mem_read32(body+0x48)!=0x8020C6B8u || !validate_native_corpse_creation(e,error))return false;
    const auto& v=std::get<std::vector<float>>(e.properties.at("corpse_lifecycle"));
    const auto mode=mem_read32(body+0x174);if(mode==2)return true; // Explosion/gibs already use the authoritative event journal.
    const auto method=mode==1?0x8008DAA0u:0x8008D40Cu;
    for(auto fn:{method,0x8008E1E4u})if(!ssne_find_func(fn)){error="Original corpse presentation unavailable";return false;}
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    const auto dt_address=context.r[13]-0x7A08,dt=mem_read32(dt_address);mem_write32(dt_address,f2u(0.f));
    // Recreate only the original dissolve renderer/effect ownership once.
    // Do not call the world, death callback, physics update or damage code.
    if(v[10]==1 && !mem_read32(body+0x134) && v[11]<4){call.r[3]=body;ssne_call(call,0x8008E1E4);mem_write32(body+0x134,1);}
    constexpr unsigned offsets[]{0x128,0x12C,0x130,0x140,0x144,0x148,0x14C,0x150,0x154,0x118};
    for(unsigned i=0;i<10;++i)mem_write32(body+offsets[i],f2u(v[i]));
    mem_write32(body+0x180,uint32_t(v[12]));mem_write32(body+0x17C,uint32_t(v[11]));
    if(v[10]==1 && v[11]==4 && mem_read32(body+0x134)){call.r[3]=body;ssne_call(call,method);}
    if(v[10]==1 && v[11]<4){
        if(!e.properties.contains("corpse_dissolve_matrices") || !e.properties.contains("corpse_ghost_matrix") || !e.properties.contains("corpse_particles0")){mem_write32(dt_address,dt);error="Incomplete corpse dissolve snapshot";return false;}
        const auto& matrices=std::get<std::vector<float>>(e.properties.at("corpse_dissolve_matrices"));
        for(unsigned part=0;part<unsigned(matrices[0]);++part)if(matrices[1+part*13]){
            call.r[3]=mem_read32(body+0xCC);call.r[4]=part;ssne_call(call,0x8013E52C);const auto src=call.r[3];
            if(!readable(src,0x38) || mem_read32(src)!=0x8021D2A8u){mem_write32(dt_address,dt);error="Corpse dissolve source unavailable";return false;}
            call.r[3]=src;call.r[4]=2;ssne_call(call,0x801230DC);const auto matrix=call.r[3];
            if(!readable(matrix,48)){mem_write32(dt_address,dt);error="Corpse dissolve matrix unavailable";return false;}
            for(unsigned i=0;i<12;++i)mem_write32(matrix+i*4,f2u(matrices[2+part*13+i]));
        }
        const auto& ghost=std::get<std::vector<float>>(e.properties.at("corpse_ghost_matrix"));
        if(!ghost.empty()){const auto p=mem_read32(body+0x13C);call.r[3]=p;ssne_call(call,mem_read32(mem_read32(p)+0x2C));for(unsigned i=0;i<12;++i)mem_write32(call.r[3]+i*4,f2u(ghost[i]));}
        const auto total=unsigned(std::get<int64_t>(e.properties.at("corpse_particle_count")));
        if(total){const auto p=mem_read32(body+0x138);call.r[3]=p;ssne_call(call,mem_read32(mem_read32(p)+0x34));const auto data=call.r[3];
            const auto descriptor=mem_read32(mem_read32(context.r[13]-0x75E0)+4+mem_read32(body+0x124)*4);call.r[3]=descriptor;ssne_call(call,mem_read32(mem_read32(descriptor)+0x54));if(call.r[3]!=total){mem_write32(dt_address,dt);error="Corpse particle count differs from original geometry";return false;}
            if(!readable(data,total*20)){mem_write32(dt_address,dt);error="Corpse particle buffer unavailable";return false;}
            for(unsigned start=0;start<total;start+=511){const auto& particles=std::get<std::vector<float>>(e.properties.at("corpse_particles"+std::to_string(start/511)));
                for(unsigned i=0;i<unsigned(particles[0]);++i){const auto base=1+i*8,offset=(start+i)*20;for(unsigned k=0;k<3;++k)mem_write32(data+offset+k*4,f2u(particles[base+k]));for(unsigned k=0;k<4;++k)mem_write8(data+offset+12+k,uint8_t(particles[base+3+k]));mem_write16(data+offset+16,uint16_t(particles[base+7]));}}
            call.r[3]=p;ssne_call(call,mem_read32(mem_read32(p)+0x4C));
        }
    }
    mem_write32(dt_address,dt);return true;
}
bool capture_native_corpse_visual(PPCContext& context,uint32_t body,EntityState& e,std::string& error){
    if(!context_ok(context) || !readable(body,0x188) || mem_read32(body+0x48)!=0x8020C6B8u){error="Corpse visual owner unavailable";return false;}
    std::vector<float> matrices{0},ghost;unsigned total=0;e.properties["corpse_particles0"]=std::vector<float>{0};
    if(mem_read32(body+0x134) && mem_read32(body+0x17C)<4 && mem_read32(body+0x174)<2){
        StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
        for(unsigned part=0;part<=256;++part){call.r[3]=mem_read32(body+0xCC);call.r[4]=part;ssne_call(call,0x8013E52C);const auto source=call.r[3];if(!source)break;
            if(part==256 || !readable(source,0x38) || mem_read32(source)!=0x8021D2A8u){error="Unsupported corpse dissolve renderer";return false;}
            const auto node=mem_read32(source+0x14),matrix=readable(node,12)?mem_read32(node+8):0;matrices[0]++;matrices.push_back(matrix?1.f:0.f);
            if(matrix && !readable(matrix,48)){error="Invalid corpse dissolve matrix";return false;}for(unsigned i=0;i<12;++i)matrices.push_back(matrix?u2f(mem_read32(matrix+i*4)):0.f);
        }
        if(const auto p=mem_read32(body+0x13C)){call.r[3]=p;ssne_call(call,mem_read32(mem_read32(p)+0x2C));if(!readable(call.r[3],48)){error="Invalid corpse ghost matrix";return false;}for(unsigned i=0;i<12;++i)ghost.push_back(u2f(mem_read32(call.r[3]+i*4)));}
        if(const auto p=mem_read32(body+0x138)){
            const auto level=mem_read32(context.r[13]-0x75E0),def=mem_read32(body+0x124),descriptor=mem_read32(level+4+def*4);
            call.r[3]=descriptor;ssne_call(call,mem_read32(mem_read32(descriptor)+0x54));const auto count=call.r[3];if(count>16384){error="Corpse particle count exceeds snapshot capacity";return false;}
            call.r[3]=p;ssne_call(call,mem_read32(mem_read32(p)+0x34));const auto data=call.r[3];if(!readable(data,count*20)){error="Invalid corpse particle buffer";return false;}
            total=count;for(unsigned start=0;start<count;start+=511){std::vector<float> particles{float(std::min(511u,count-start))};for(unsigned i=start;i<std::min(start+511,count);++i){for(unsigned k=0;k<3;++k)particles.push_back(u2f(mem_read32(data+i*20+k*4)));for(unsigned k=0;k<4;++k)particles.push_back(float(mem_read8(data+i*20+12+k)));particles.push_back(float(mem_read16(data+i*20+16)));}e.properties["corpse_particles"+std::to_string(start/511)]=std::move(particles);}
        }
    }
    e.properties["corpse_dissolve_matrices"]=std::move(matrices);e.properties["corpse_ghost_matrix"]=std::move(ghost);e.properties["corpse_particle_count"]=int64_t(total);return validate_native_corpse_creation(e,error);
}
