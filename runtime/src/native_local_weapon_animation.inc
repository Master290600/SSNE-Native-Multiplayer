namespace local_weapon_animation {
// PAL animator 80127718 advances at 25 keyframes per second. Its one-shot
// queue (80127E20) ends just before an extra, wrapping frame would be read.
constexpr float frames_per_second=25.f,timing_epsilon=.000001f;
constexpr unsigned max_bones=64,max_animations=256,max_frames=512;
constexpr uint32_t packed_scratch=0x8028EFF0u;
constexpr unsigned packed_scratch_size=0x1A00,stack_scratch_size=0x1200;

struct Rig {
    uint32_t instance,model,resource,data,packed,count;
};
std::optional<Rig> rig(uint32_t actor,unsigned weapon,unsigned hand){
    if(!replica || !original_bones || mem_read32(0x80000000u)!=0x47334250u ||
       weapon>=12 || hand>=2 || !readable(actor,0x790) || mem_read32(actor+0x48)!=0x802121E8u ||
       mem_read32(actor+0x17C)!=uint32_t(netplay_local_player()) || mem_read32(actor+0x5DC)!=weapon ||
       !native_party_actor_occupied(actor))return {};
    const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
    if(!readable(instance,0x228) || mem_read32(instance+0x1D4)!=actor)return {};
    const auto model=mem_read32(instance+0x28);
    if(!readable(model,0xB0) || mem_read32(model)!=0x8021D668u)return {};
    // Standalone first-person rigs never need the animator's shared-master
    // recursion or auxiliary overlay path, both of which can write other models.
    if(mem_read32(model+0x90) || mem_read32(model+0x94))return {};
    const auto resource=mem_read32(model+0xC),count=mem_read32(model+0x7C);
    if(!count || count>max_bones || !readable(resource,16))return {};
    const auto data=mem_read32(resource+0xC);
    if(!readable(data,0x48))return {};
    const auto packed=mem_read32(data+0x44);
    if(packed && !readable(packed,20))return {};
    return Rig{instance,model,resource,data,packed,count};
}

struct Keyframes {
    uint32_t records,count;
};
std::optional<Keyframes> keyframes(const Rig& rig,uint32_t animation){
    if(animation>=max_animations)return {};
    auto node=mem_read32(rig.data+0x34);
    for(unsigned i=0;i<animation;++i){
        if(!readable(node,4))return {};
        node=mem_read32(node);
    }
    if(!readable(node,20))return {};
    const auto count=mem_read32(node+0xC),records=mem_read32(node+0x10);
    if(!count || count>max_frames || !readable(records,count*20))return {};
    return Keyframes{records,count};
}

std::optional<uint32_t> animation_index(const Rig& rig,unsigned logical){
    if(logical>=max_animations)return {};
    // cAnimControl::PlayClip and the weapon speed override both map through
    // this optional table of 12-byte logical clip descriptors.
    const auto mapping=mem_read32(rig.instance+0x18);
    if(!mapping)return logical;
    if(!readable(mapping,(logical+1)*12))return {};
    return mem_read32(mapping+logical*12);
}

bool frame_readable(const Rig& rig,const Keyframes& frames,unsigned index){
    if(index>=frames.count)return false;
    const auto stream=mem_read32(frames.records+index*20);
    if(rig.packed){
        if(!readable(stream,rig.count*2))return false;
        const auto keys=mem_read32(rig.packed);
        if(!readable(keys,20))return false;
        for(unsigned bone=0;bone<rig.count;++bone)
            if(!readable(keys,unsigned(mem_read16(stream+bone*2))*20+20))return false;
    }else{
        if(!readable(stream,rig.count*4))return false;
        for(unsigned bone=0;bone<rig.count;++bone)
            if(!readable(mem_read32(stream+bone*4),48))return false;
    }
    return true;
}

std::optional<NativeLocalWeaponClip> resolve(uint32_t actor,unsigned weapon,unsigned hand,unsigned logical){
    const auto binding=rig(actor,weapon,hand);
    if(!binding)return {};
    const auto index=animation_index(*binding,logical);
    const auto frames=index?keyframes(*binding,*index):std::nullopt;
    if(!frames || !frame_readable(*binding,*frames,0))return {};
    NativeLocalWeaponClip clip{float(frames->count)/frames_per_second-timing_epsilon,binding->model,
                               binding->resource,binding->data,logical,*index,frames->count};
    // On the first fractional frame the stock animator blends from the
    // previously displayed key. An uninitialised replica may not have one.
    const auto old_animation=mem_read32(binding->model+0x40),old_frame=mem_read32(binding->model+0x44);
    if(const auto old=keyframes(*binding,old_animation);old && frame_readable(*binding,*old,old_frame)){
        clip.initial_animation=old_animation;
        clip.initial_frame=old_frame;
    }
    return clip;
}

template<unsigned Size> struct RestoreBytes {
    uint8_t* target;
    std::array<uint8_t,Size> old;
    explicit RestoreBytes(uint32_t p):target(host_ptr(p)){std::memcpy(old.data(),target,Size);}
    ~RestoreBytes(){std::memcpy(target,old.data(),Size);}
};

bool sample(PPCContext& context,uint32_t actor,unsigned weapon,unsigned hand,const NativeLocalWeaponClip& clip,
            float seconds,std::vector<float>& pose,uint32_t* sampled_frame=nullptr){
    const auto binding=rig(actor,weapon,hand);
    if(!binding || binding->model!=clip.model || binding->resource!=clip.resource || binding->data!=clip.data ||
       context.r[13]!=0x802E22A0u || (context.r[1]&7u) || !std::isfinite(seconds) || seconds<0.f)
        return false;
    const auto index=animation_index(*binding,clip.logical);
    const auto frames=index && *index==clip.animation?keyframes(*binding,*index):std::nullopt;
    if(!frames || frames->count!=clip.frames)return false;
    const auto position=std::min(seconds*frames_per_second,float(frames->count)-timing_epsilon*frames_per_second);
    const auto frame=std::min(unsigned(position),frames->count-1);
    const auto fraction=std::clamp(position-float(frame),0.f,1.f);
    auto previous_animation=clip.animation,previous_frame=frame?frame-1:0;
    if(!frame){
        const auto initial=keyframes(*binding,clip.initial_animation);
        if(initial && frame_readable(*binding,*initial,clip.initial_frame)){
            previous_animation=clip.initial_animation;
            previous_frame=clip.initial_frame;
        }
    }
    if(!frame_readable(*binding,*frames,frame) ||
       (previous_animation==clip.animation && !frame_readable(*binding,*frames,previous_frame)))return false;
    const auto stack=context.r[1]-stack_scratch_size;
    if(!readable(stack,stack_scratch_size) ||
       (binding->packed && !readable(packed_scratch,packed_scratch_size)))return false;

    // The original bone evaluator needs a valid guest ABI stack and writes its
    // output to model+0x78. Use a detached model and palette in private scratch.
    // Preserve the evaluator's two packed-key work arrays around the call.
    RestoreBytes<stack_scratch_size> stack_bytes(stack);
    std::optional<RestoreBytes<packed_scratch_size>> packed_bytes;
    if(binding->packed)packed_bytes.emplace(packed_scratch);
    auto call=context;
    call.msr&=~0x8000u;
    call.r[1]=stack+0x400;
    mem_write32(call.r[1],context.r[1]);
    const auto model=stack+0x440,palette=stack+0x500;
    std::memcpy(host_ptr(model),host_ptr(binding->model),0xB0);
    mem_write32(model+0x38,previous_animation);
    mem_write32(model+0x3C,previous_frame);
    mem_write32(model+0x40,clip.animation);
    mem_write32(model+0x44,frame);
    mem_write32(model+0x48,f2u(fraction));
    mem_write32(model+0x4C,f2u(-1.f)); // force a palette evaluation
    mem_write32(model+0x78,palette);
    call.r[3]=model;
    native_animation_pose_dispatch(call,original_bones);
    if(call.r[3]!=0)return false;
    std::vector<float> sampled;
    sampled.reserve(binding->count*12);
    for(unsigned word=0;word<binding->count*12;++word){
        const auto value=u2f(mem_read32(palette+word*4));
        if(!std::isfinite(value))return false;
        sampled.push_back(value);
    }
    pose=std::move(sampled);
    if(sampled_frame)*sampled_frame=frame;
    return true;
}
} // namespace local_weapon_animation
