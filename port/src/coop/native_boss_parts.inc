// PAL Dark Lord has sixteen bone-attached hit regions, not factory enemies.
// Their native receiver8001BE1C stores health at +FC and phase at +1BC.
// Only indexed scalar/position data travels; owner pointers remain local.
bool validate_native_boss_parts(const EntityState& e,std::string& error){
    const auto item=e.properties.find("boss_weakpoints");
    if(item==e.properties.end())return true;
    const auto* values=std::get_if<std::vector<float>>(&item->second);
    if(e.kind!=EntityKind::Enemy || e.definition!=42 || !values || values->size()!=16*7){error="Invalid Dark Lord weakpoint schema";return false;}
    for(unsigned i=0;i<16;++i){const auto at=i*7;const auto& v=*values;
        for(unsigned k=0;k<7;++k)if(!std::isfinite(v[at+k]) || std::abs(v[at+k])>1000000.f){error="Invalid Dark Lord weakpoint coefficient";return false;}
        if((v[at]!=0 && v[at]!=1) || v[at+1]<0 || v[at+1]>2 || std::floor(v[at+1])!=v[at+1] || (v[at+2]!=0 && v[at+2]!=1) || v[at+3]<0){error="Invalid Dark Lord weakpoint state";return false;}
        if(!v[at])for(unsigned k=1;k<7;++k)if(v[at+k]){error="Retired Dark Lord weakpoint contains live state";return false;}
    }
    return true;
}
bool capture_native_boss_parts(uint32_t boss,EntityState& e,std::string& error){
    if(e.kind!=EntityKind::Enemy || e.definition!=42)return true;
    if(!readable(boss,0x23C) || mem_read32(boss+0x48)!=0x801EA4A8u){error="Dark Lord native owner differs";return false;}
    std::vector<float> values(16*7,0.f);
    for(unsigned i=0;i<16;++i){const auto part=mem_read32(boss+0x1FC+i*4);if(!part)continue;
        if(!readable(part,0x1C8) || mem_read32(part+0x48)!=0x801EAAF8u || mem_read32(part+0xCC)!=boss){error="Dark Lord native weakpoint binding differs";return false;}
        // The original update retires phase2 on its next pass. Replicas keep
        // that owned allocation until parent cleanup; it is logically absent.
        const auto phase=mem_read32(part+0x1BC);if(phase==2)continue;
        const auto at=i*7;values[at]=1;values[at+1]=float(phase);values[at+2]=float(mem_read32(part+0x100));values[at+3]=u2f(mem_read32(part+0xFC));
        for(unsigned k=0;k<3;++k)values[at+4+k]=u2f(mem_read32(part+0x20+k*4));
    }
    e.properties["boss_weakpoints"]=std::move(values);return validate_native_boss_parts(e,error);
}
bool apply_native_boss_parts(uint32_t boss,const EntityState& e,std::string& error){
    if(!validate_native_boss_parts(e,error))return false;
    const auto item=e.properties.find("boss_weakpoints");if(item==e.properties.end())return true;
    if(!readable(boss,0x23C) || mem_read32(boss+0x48)!=0x801EA4A8u){error="Dark Lord replica owner differs";return false;}
    const auto& values=std::get<std::vector<float>>(item->second);
    // Validate all local ownership before writing any region.
    for(unsigned i=0;i<16;++i){const auto part=mem_read32(boss+0x1FC+i*4);
        if((values[i*7] && !part) || (part && (!readable(part,0x1C8) || mem_read32(part+0x48)!=0x801EAAF8u || mem_read32(part+0xCC)!=boss))){error="Dark Lord replica weakpoint binding differs";return false;}
    }
    for(unsigned i=0;i<16;++i){const auto part=mem_read32(boss+0x1FC+i*4);if(!part)continue;const auto at=i*7;
        if(!values[at]){mem_write32(part+0x1BC,2);mem_write32(part+0x100,1);mem_write32(part+0xFC,f2u(0));continue;}
        mem_write32(part+0x1BC,uint32_t(values[at+1]));mem_write32(part+0x100,uint32_t(values[at+2]));mem_write32(part+0xFC,f2u(values[at+3]));
        for(unsigned k=0;k<3;++k)mem_write32(part+0x20+k*4,f2u(values[at+4+k]));
    }
    return true;
}
