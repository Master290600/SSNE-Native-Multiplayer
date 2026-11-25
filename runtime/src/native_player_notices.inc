struct PickupNoticeState {uint32_t actor=0;NativePickupNotice notice;};
std::array<PickupNoticeState,8> pickup_notices{};uint64_t pickup_notice_serial=0;
PPCFunc original_popup=nullptr,original_step_once=nullptr,original_step_priority=nullptr;
thread_local uint32_t step_controller=0;
thread_local bool player_death_voice=false;
template<uint32_t Address>void step_owner_hook(PPCContext& c){
    struct Restore{
        uint32_t controller=step_controller;
        bool death=player_death_voice;
        ~Restore(){step_controller=controller;player_death_voice=death;}
    } restore;
    step_controller=c.r[3];
    // PAL's normal death and drowning paths have priority ten and use the
    // current skin's regular, submerged, or drowning death cue.
    player_death_voice=Address==0x800B7CDCu && c.r[5]==10 &&
        ((c.lr==0x800B7B0Cu && (c.r[4]==0 || c.r[4]==4)) ||
         (c.lr==0x800B7BB0u && c.r[4]==31));
    ssne_call_original(c,Address,Address==0x800B7CB4u?original_step_once:original_step_priority);
}
bool is_player_death_cue(const NativeFeedbackEvent& event){
    if(event.kind!=10 || event.variant>=native_party_capacity())return false;
    const auto actor=mem_read32(native_party_registry()+event.variant*4);
    if(!combo_feedback_slot(actor))return false;
    const auto controller=mem_read32(actor+0x174);
    const auto cues=readable(controller,8)?mem_read32(controller+4):0;
    return readable(cues,32*4) &&
        (event.code==mem_read32(cues) || event.code==mem_read32(cues+4*4) ||
         event.code==mem_read32(cues+31*4));
}
void trace_player_death_voice(const char* peer,const NativeFeedbackEvent& e,uint32_t handle,bool positional){
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_COOP_TRACE_LIFETIMES"))return;
    track_shot_voice(handle,0x200000000ull+e.sequence);
    log_message(std::string("[coop-player-death-voice-")+peer+"] sequence="+std::to_string(e.sequence)+
                " slot="+std::to_string(e.variant)+" code="+std::to_string(e.code)+
                " handle="+std::to_string(int32_t(handle))+" positional="+std::to_string(positional));
}
std::optional<unsigned> native_notice_owner(uint32_t object,unsigned actor_offset){
    for(unsigned slot=0;slot<native_party_capacity();++slot){const auto actor=mem_read32(native_party_registry()+slot*4);
        if(!readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u)continue;
        const auto target=mem_read32(actor+actor_offset);
        if(actor_offset==0x374){if(readable(target,0x38)){const auto popup=mem_read32(target+0x34);if(readable(popup,0x10) && mem_read32(popup+0xC)==object)return slot;}}
        else if(target==object)return slot;
    }return {};
}
void pickup_popup_hook(PPCContext& c){
    const auto owner=authoritative && !presenting?native_notice_owner(c.r[3],0x374):std::nullopt;
    NativePickupNotice notice;bool valid=owner && c.r[5]<=5 && c.r[6]<=1 && readable(c.r[4],128);
    if(valid){notice.flags=c.r[5] | (c.r[6]<<3);unsigned n=0;for(;n<128 && mem_read8(c.r[4]+n);++n)notice.text+=char(mem_read8(c.r[4]+n));valid=n<128 && !notice.text.empty();}
    ssne_call_original(c,0x800AF094u,original_popup);
    if(valid){notice.sequence=++pickup_notice_serial;log_message("[coop-pickup-text-host] sequence="+std::to_string(notice.sequence)+" slot="+std::to_string(*owner)+" bytes="+std::to_string(notice.text.size()));pickup_notices[*owner]={mem_read32(native_party_registry()+*owner*4),std::move(notice)};}
}
bool install_native_player_notices(std::string& error){
    if(!ssne_find_func(0x800AF094) || mem_read32(0x800AF094)!=0x9421FFE0u){error="Original pickup popup unavailable";return false;}
    if(!ssne_find_func(0x800B7CB4) || !ssne_find_func(0x800B7CDC)){error="Original footstep sound selection unavailable";return false;}
    if(!original_popup){original_popup=ssne_find_func(0x800AF094)->func;ssne_install_hook(0x800AF094,pickup_popup_hook);original_step_once=ssne_find_func(0x800B7CB4)->func;original_step_priority=ssne_find_func(0x800B7CDC)->func;ssne_install_hook(0x800B7CB4,step_owner_hook<0x800B7CB4>);ssne_install_hook(0x800B7CDC,step_owner_hook<0x800B7CDC>);}return true;
}
