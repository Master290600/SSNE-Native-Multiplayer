bool validate_native_model_states(const std::vector<int64_t>& states,std::string& error){
    auto fail=[&](const char* why){error=why;return false;};const auto level=mem_read32(0x802DACC0);
    if(!readable(level,0x29C))return fail("Native model-state level unavailable");
    const auto manager=mem_read32(level+0x298);if(!readable(manager,0x28) || mem_read32(manager)!=0x8021E660u)return fail("Native model-state manager unavailable");
    const auto count=mem_read32(manager+0x1C),entries=mem_read32(manager+0x18);
    if(count>4096 || states.size()!=count || (count && !readable(entries,count*20)) || !ssne_find_func(0x80133EF8) || mem_read32(0x80133EF8)!=0x81630018u)return fail("Native model-state catalog differs");
    for(unsigned i=0;i<count;++i){const auto variants=mem_read32(entries+i*20+0xC);if(variants>4096 || states[i]< -1 || states[i]>=variants)return fail("Native model-state variant outside local resource");}
    return true;
}
bool capture_native_model_states(std::vector<int64_t>& states,std::string& error){
    const auto level=mem_read32(0x802DACC0);if(!readable(level,0x29C)){error="Native model-state level unavailable";return false;}
    const auto manager=mem_read32(level+0x298);if(!readable(manager,0x28) || mem_read32(manager+0x1C)>4096 || (mem_read32(manager+0x1C) && !readable(mem_read32(manager+0x18),mem_read32(manager+0x1C)*20))){error="Native model-state table unavailable";return false;}
    states.clear();for(unsigned i=0;i<mem_read32(manager+0x1C);++i)states.push_back(int32_t(mem_read32(mem_read32(manager+0x18)+i*20+8)));
    return validate_native_model_states(states,error);
}
bool apply_native_model_states(PPCContext& context,const std::vector<int64_t>& states,std::string& error){
    if(!context_ok(context) || !validate_native_model_states(states,error))return false;
    const auto manager=mem_read32(mem_read32(0x802DACC0)+0x298),entries=mem_read32(manager+0x18);
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    for(unsigned i=0;i<states.size();++i)if(mem_read32(entries+i*20+8)!=uint32_t(states[i])){call.r[3]=manager;call.r[4]=i;call.r[5]=uint32_t(states[i]);ssne_call(call,0x80133EF8);}
    return true;
}
