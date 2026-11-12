struct PickupNoticeState {uint32_t actor=0;NativePickupNotice notice;};
std::array<PickupNoticeState,8> pickup_notices{};uint64_t pickup_notice_serial=0;
PPCFunc original_popup=nullptr,original_step_once=nullptr,original_step_priority=nullptr;
thread_local uint32_t step_controller=0;
template<uint32_t Address>void step_owner_hook(PPCContext& c){const auto previous=step_controller;step_controller=c.r[3];struct Restore{uint32_t value;~Restore(){step_controller=value;}}restore{previous};ssne_call_original(c,Address,Address==0x800B7CB4u?original_step_once:original_step_priority);}
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
