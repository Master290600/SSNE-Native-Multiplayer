// Original attenuation/panning against this PC's listener. The split-screen
// mixer averages every player and uses the nearest distance; that makes a
// remote shooter its own zero-distance listener in an authoritative session.
PPCFunc original_spatial_audio=nullptr,original_single_listener=nullptr;
void spatial_audio_hook(PPCContext& c){
    const auto slot=netplay_local_player();const auto actor=slot>=0 && slot<8?mem_read32(native_party_registry()+unsigned(slot)*4):0;
    if((!authoritative && !replica_simulation()) || !netplay_world_ready() || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u || !readable(c.r[3],0x18)){
        ssne_call_original(c,0x8015877C,original_spatial_audio);return;
    }
    const auto manager=c.r[3],record=c.r[4];std::array<uint32_t,6> saved{};for(unsigned i=0;i<6;++i)saved[i]=mem_read32(manager+i*4);
    const auto camera=mem_read32(actor+0x368);const auto yaw=u2f(mem_read32(actor+0x290));
    if(!readable(camera,0x54) || mem_read32(camera)!=0x80210880u || mem_read32(camera+4)!=actor){ssne_call_original(c,0x8015877C,original_spatial_audio);return;}
    // Exactly the camera matrix components passed by original801555F0 to
    // SoundManager::SetListener80158660: translation and its Z axis. The
    // camera Z axis points backwards; movement's forward vector is reversed.
    for(unsigned i=0;i<3;++i){mem_write32(manager+i*4,mem_read32(camera+0x24+i*16+12));mem_write32(manager+0xC+i*4,mem_read32(camera+0x24+i*16+8));}
    try{ssne_call_original(c,0x801587D8,original_single_listener);}catch(...){for(unsigned i=0;i<6;++i)mem_write32(manager+i*4,saved[i]);throw;}
    if(readable(record,0xC0) && mem_read32(record+0xA4)==1){
        std::array<float,3> source{};std::array<float,12> basis{};
        for(unsigned i=0;i<3;++i)source[i]=u2f(mem_read32(record+0x8C+i*4));
        for(unsigned i=0;i<12;++i)basis[i]=u2f(mem_read32(camera+0x24+i*4));
        const auto pan=native_camera_sound_pan(source,basis);
        if(std::isfinite(pan)){mem_write32(record+0x24,f2u(pan));mem_write32(record+0x34,1);}
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_CLIENT_UI") && readable(record,0xC0) && mem_read32(record+0xA4)==1){static unsigned samples=0;if(samples++<120){float distance=0;for(unsigned i=0;i<3;++i){const auto delta=u2f(mem_read32(record+0x8C+i*4))-u2f(mem_read32(actor+0x20+i*4));distance+=delta*delta;}log_message("[dev-native-listener] slot="+std::to_string(slot)+" record="+std::to_string(record)+" distance="+std::to_string(std::sqrt(distance))+" attenuation="+std::to_string(u2f(mem_read32(record+0x2C)))+" pan="+std::to_string(u2f(mem_read32(record+0x24)))+" yaw="+std::to_string(yaw));}}
    for(unsigned i=0;i<6;++i)mem_write32(manager+i*4,saved[i]);
}
bool install_spatial_audio(std::string& error){
    if(original_spatial_audio)return true;
    const auto* spatial=ssne_find_func(0x8015877C);const auto* single=ssne_find_func(0x801587D8);
    if(!spatial || !single || mem_read32(spatial->address)!=0x9421FFF8u || mem_read32(single->address)!=0x9421FF60u){error="Original listener mixer unavailable";return false;}
    original_spatial_audio=spatial->func;original_single_listener=single->func;ssne_install_hook(spatial->address,spatial_audio_hook);return true;
}
void update_replica_loop_positions(){
    for(const auto& [key,loop]:replica_loops){
        const auto slot=key/2;if(slot>=native_party_capacity())continue;
        const auto actor=mem_read32(native_party_registry()+slot*4),index=loop.handle&63u;if(index>=48 || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)continue;
        const auto record=0x802D69C8u+index*192u;
        if(mem_read32(record+0xAC)==loop.handle && mem_read32(record) && mem_read32(record+0xA4)==1)for(unsigned i=0;i<3;++i)mem_write32(record+0x8C+i*4,mem_read32(actor+0x20+i*4));
    }
}
