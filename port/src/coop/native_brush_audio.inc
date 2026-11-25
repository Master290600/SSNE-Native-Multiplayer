// MovingBrush::Update owns mission triggers as well as sound. Replicas consume
// its authored audio calls separately so opening a door never runs its script.
namespace {
constexpr size_t brush_audio_history=32;
struct BrushAudioHost {uint64_t serial=0;std::deque<std::pair<uint64_t,unsigned>> events;bool loop=false;};
struct BrushAudioReplica {
    uint64_t epoch=0,serial=0;
    uint32_t handle=UINT32_MAX,sound=350,start_handle=UINT32_MAX,start_sound=350;
    bool initialized=false;
    std::map<uint32_t,uint32_t> cues;
};
std::map<uint32_t,BrushAudioHost> brush_host_audio;
std::map<uint32_t,BrushAudioReplica> brush_replica_audio;
PPCFunc original_brush_cue=nullptr,original_brush_retire=nullptr;
uint64_t brush_audio_subscription=0;
bool brush_audio_trace(){return getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES");}
bool brush_audio_object(uint32_t object){return readable(object,0x13C) && mem_read32(object+0x48)==0x80206868u;}
bool brush_audio_sound(uint32_t sound){return sound<479;}
bool brush_audio_voice(uint32_t handle,uint32_t sound){
    if(!brush_audio_sound(sound) || int32_t(handle)<0 || (handle&63u)>=48)return false;
    const auto record=0x802D69C8u+(handle&63u)*192u,resource=0x8023AD70u+sound*24u;
    const auto count=mem_read32(resource+8),samples=mem_read32(resource+12);
    if(!readable(mem_read32(record),0x148) || mem_read32(record+0xAC)!=handle || count>64 || !readable(samples,count*4))return false;
    for(unsigned i=0;i<count;++i)if(mem_read32(record+0xB0)==mem_read32(samples+i*4))return true;
    return false;
}
void brush_audio_track(uint32_t object,uint32_t handle,uint64_t serial,unsigned stage){
    if(!brush_audio_trace() || (handle&63u)>=48)return;
    const auto record=0x802D69C8u+(handle&63u)*192u,voice=mem_read32(record);
    if(mem_read32(record+0xAC)!=handle || !readable(voice,0x148))return;
    const auto slot=mem_read32(voice+0x18);
    const auto tag=0x5000000000000000ull | (uint64_t(mem_read32(object+0x50)&0xFFFFu)<<32) | ((serial&0xFFFFFFFull)<<2) | stage;
    if(slot<64)ssne::audio_track_weapon_voice(0x802C18A0u+slot*236,record,handle,tag);
}
void brush_audio_stop_voice(PPCContext& context,BrushAudioReplica& audio){
    if(brush_audio_voice(audio.handle,audio.sound)){
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.lr=0;
        call.r[3]=audio.handle;ssne_call(call,0x8010EDFC);
    }
    audio.handle=UINT32_MAX;
}
void brush_audio_stop_cues(PPCContext& context,BrushAudioReplica& audio){
    for(const auto& [handle,sound]:audio.cues)if(brush_audio_voice(handle,sound)){
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.lr=0;
        call.r[3]=handle;ssne_call(call,0x8010EDFC);
    }
    audio.cues.clear();audio.start_handle=UINT32_MAX;
}
uint64_t brush_audio_event(uint32_t object,unsigned stage){
    auto& audio=brush_host_audio[object];
    if(audio.serial==uint64_t(INT64_MAX))ssne::fatal("Native moving-brush audio serial exhausted");
    audio.events.emplace_back(++audio.serial,stage);
    while(audio.events.size()>brush_audio_history)audio.events.pop_front();
    return audio.serial;
}
void brush_retire_hook(PPCContext& context){
    const auto object=context.r[31],handle=context.r[4];
    // The common stop wrapper keeps its caller in its own eight-byte frame.
    // Observe only the first stop in8007F53C, including doors with no end cue.
    // Hooking the low mixer entry leaves the shared weapon-stop hook intact.
    const bool capture=!ssne::replica_simulation() && context.lr==0x8010EE18u &&
        readable(context.r[1],16) && mem_read32(context.r[1]+12)==0x8007F544u &&
        brush_audio_object(object) && handle==mem_read32(object+0x68);
    ssne_call_original(context,0x8015841C,original_brush_retire);
    if(capture){const auto serial=brush_audio_event(object,2);
        if(brush_audio_trace())ssne::log_message("[coop-brush-audio-host] brush="+std::to_string(mem_read32(object+0x50))+" serial="+std::to_string(serial)+" stage=retire handle="+std::to_string(handle));}
}
void brush_cue_hook(PPCContext& context){
    const auto object=context.r[31],sound=context.r[3],caller=context.lr;
    const bool start=caller==0x8007F178u || caller==0x8007F270u,stop=caller==0x8007F5A0u;
    const bool capture=!ssne::replica_simulation() && (start || stop) && brush_audio_object(object) &&
        context.r[4]==object+0x70 && sound==mem_read32(object+(start?0x94:0x9C));
    ssne_call_original(context,0x8010EAE0,original_brush_cue);
    if(capture){
        const auto serial=brush_audio_event(object,stop?1:0);
        brush_audio_track(object,context.r[3],serial,stop?1:0);
        if(brush_audio_trace())ssne::log_message("[coop-brush-audio-host] brush="+std::to_string(mem_read32(object+0x50))+" serial="+std::to_string(serial)+" stage="+(stop?"stop":"start")+" sound="+std::to_string(sound)+" handle="+std::to_string(context.r[3]));
    }
}
bool install_brush_audio(std::string& error){
    if(!original_brush_cue){
        for(const auto [address,signature]:{std::pair{0x8010EAE0u,0x9421FFE0u},std::pair{0x8010ED14u,0x9421FFE0u},std::pair{0x8010EDFCu,0x9421FFF8u},std::pair{0x8015841Cu,0x9421FFE8u}})
            if(!ssne_find_func(address) || mem_read32(address)!=signature){error="Native moving-brush audio lifecycle unavailable";return false;}
        original_brush_cue=ssne_find_func(0x8010EAE0)->func;ssne_install_hook(0x8010EAE0,brush_cue_hook);
        original_brush_retire=ssne_find_func(0x8015841C)->func;ssne_install_hook(0x8015841C,brush_retire_hook);
    }
    if(!brush_audio_subscription)brush_audio_subscription=ssne::observe_native_entities([](const ssne::NativeEntityNotice& notice){
        if(notice.event!=ssne::NativeEntityNotice::Event::Removing && notice.event!=ssne::NativeEntityNotice::Event::LevelExit)return;
        const bool all=notice.event==ssne::NativeEntityNotice::Event::LevelExit;
        if(notice.context && context_ok(*notice.context)){
            StackScratch scratch(*notice.context);
            for(auto& [object,audio]:brush_replica_audio)if(all || object==notice.object){brush_audio_stop_voice(*notice.context,audio);brush_audio_stop_cues(*notice.context,audio);}
        }
        if(all){brush_host_audio.clear();brush_replica_audio.clear();}
        else {brush_host_audio.erase(notice.object);brush_replica_audio.erase(notice.object);}
    });
    return true;
}
}
bool capture_native_brush_audio(uint32_t object,EntityState& entity,std::string& error){
    if(!brush_audio_object(object) || !install_brush_audio(error))return false;
    for(unsigned offset:{0x94u,0x98u,0x9Cu})if(!brush_audio_sound(mem_read32(object+offset))){error="Invalid native moving-brush sound reference";return false;}
    auto& audio=brush_host_audio[object];
    const auto sound=mem_read32(object+0x98),handle=mem_read32(object+0x6C);
    const bool loop=sound!=350 && brush_audio_voice(handle,sound);
    if(loop!=audio.loop){
        audio.loop=loop;if(loop)brush_audio_track(object,handle,audio.serial,2);
        if(brush_audio_trace())ssne::log_message("[coop-brush-audio-host] brush="+std::to_string(mem_read32(object+0x50))+" serial="+std::to_string(audio.serial)+" stage="+(loop?"loop-start":"loop-stop")+" sound="+std::to_string(sound)+" handle="+std::to_string(handle));
    }
    std::vector<int64_t> events;events.reserve(audio.events.size()*2);
    for(const auto& [serial,stage]:audio.events){events.push_back(int64_t(serial));events.push_back(stage);}
    std::array<float,3> position;for(unsigned axis=0;axis<3;++axis)position[axis]=u2f(mem_read32(object+0x70+axis*4));
    entity.properties["brush_audio_serial"]=int64_t(audio.serial);
    entity.properties["brush_audio_events"]=std::move(events);
    entity.properties["brush_audio_loop"]=int64_t(loop);
    entity.properties["brush_audio_position"]=position;
    return validate_native_brush_audio(object,entity,error);
}
bool validate_native_brush_audio(uint32_t object,const EntityState& entity,std::string& error){
    const auto& p=entity.properties;
    if(!p.contains("brush_audio_serial") && !p.contains("brush_audio_events") && !p.contains("brush_audio_loop") && !p.contains("brush_audio_position"))return true;
    auto fail=[&](const char* message){error=message;return false;};
    if(!brush_audio_object(object))return fail("Moving-brush audio targets another native class");
    const auto serial=p.find("brush_audio_serial"),events=p.find("brush_audio_events"),loop=p.find("brush_audio_loop"),position=p.find("brush_audio_position");
    if(serial==p.end() || events==p.end() || loop==p.end() || position==p.end())return fail("Incomplete native moving-brush audio state");
    const auto* s=std::get_if<int64_t>(&serial->second);const auto* e=std::get_if<std::vector<int64_t>>(&events->second);
    const auto* l=std::get_if<int64_t>(&loop->second);const auto* pos=std::get_if<std::array<float,3>>(&position->second);
    if(!s || *s<0 || !e || e->size()%2 || e->size()>brush_audio_history*2 || !l || (*l!=0 && *l!=1) || !pos)return fail("Invalid native moving-brush audio state");
    int64_t last=0;for(size_t i=0;i<e->size();i+=2){if((*e)[i]<=last || (*e)[i]>*s || (*e)[i+1]<0 || (*e)[i+1]>2)return fail("Invalid native moving-brush cue history");last=(*e)[i];}
    if((*s==0)!=e->empty() || (!e->empty() && (*e)[e->size()-2]!=*s))return fail("Incomplete native moving-brush cue history");
    for(float value:*pos)if(!std::isfinite(value) || std::abs(value)>1000000.f)return fail("Invalid native moving-brush audio position");
    for(unsigned offset:{0x94u,0x98u,0x9Cu})if(!brush_audio_sound(mem_read32(object+offset)))return fail("Invalid native moving-brush sound reference");
    return true;
}
bool apply_native_brush_audio(PPCContext& context,uint64_t epoch,uint32_t object,const EntityState& entity,std::string& error,bool fresh_world){
    if(!entity.properties.contains("brush_audio_serial"))return true;
    if(!epoch || !context_ok(context) || !validate_native_brush_audio(object,entity,error) || !install_brush_audio(error))return false;
    const auto& p=entity.properties;auto& audio=brush_replica_audio[object];
    const auto serial=uint64_t(std::get<int64_t>(p.at("brush_audio_serial")));
    const auto& events=std::get<std::vector<int64_t>>(p.at("brush_audio_events"));
    // Separate hosts can both publish epoch1. The replica binding's fresh-world
    // signal owns that stream boundary even when its guest address is reused.
    const bool baseline=fresh_world || !audio.initialized || audio.epoch!=epoch;
    if(!baseline && serial<audio.serial){error="Native moving-brush audio history moved backwards";return false;}
    StackScratch scratch(context);
    if(baseline){brush_audio_stop_voice(context,audio);brush_audio_stop_cues(context,audio);audio={};audio.epoch=epoch;audio.serial=serial;audio.initialized=true;}
    std::erase_if(audio.cues,[](const auto& cue){return !brush_audio_voice(cue.first,cue.second);});
    const auto& position=std::get<std::array<float,3>>(p.at("brush_audio_position"));
    for(unsigned axis=0;axis<3;++axis)mem_write32(object+0x70+axis*4,f2u(position[axis]));
    // Results can open before a last in-level snapshot is applied. Consume its
    // history without resurrecting a door loop or replaying cues behind the UI.
    if(ssne::native_replica_results_presenting() || mem_read32(ssne::game_address(0x802DB0E0))!=4){
        brush_audio_stop_voice(context,audio);brush_audio_stop_cues(context,audio);audio.serial=serial;return true;
    }
    // A first/late-join baseline establishes a cursor without replaying old
    // opening/closing thumps. An already-moving authored loop is still resumed.
    const bool gap=!baseline && !events.empty() && uint64_t(events[0])>audio.serial+1;
    if(gap && brush_audio_trace())ssne::log_message("[coop-brush-audio-gap] brush="+std::to_string(mem_read32(object+0x50))+" previous="+std::to_string(audio.serial)+" serial="+std::to_string(serial));
    if(!baseline && !gap)for(size_t i=0;i<events.size();i+=2)if(uint64_t(events[i])>audio.serial){
        const bool stop=events[i+1]!=0;const auto sound=mem_read32(object+(stop?0x9C:0x94));
        if(stop){
            // PAL8007F53C retires the current opening thump and moving loop
            // before the authored stop cue, even if their samples are longer.
            if(brush_audio_voice(audio.start_handle,audio.start_sound)){
                auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.lr=0;
                call.r[3]=audio.start_handle;ssne_call(call,0x8010EDFC);audio.cues.erase(audio.start_handle);
            }
            audio.start_handle=UINT32_MAX;brush_audio_stop_voice(context,audio);
        }
        if(events[i+1]==2){
            if(brush_audio_trace())ssne::log_message("[coop-brush-audio-client] brush="+std::to_string(mem_read32(object+0x50))+" serial="+std::to_string(events[i])+" stage=retire");
            continue;
        }
        if(sound!=350){
            auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.lr=0;
            call.r[3]=sound;call.r[4]=object+0x70;ssne_call(call,0x8010EAE0);
            if(brush_audio_voice(call.r[3],sound))audio.cues[call.r[3]]=sound;
            if(!stop){audio.start_handle=call.r[3];audio.start_sound=sound;}
            brush_audio_track(object,call.r[3],uint64_t(events[i]),stop?1:0);
            if(brush_audio_trace())ssne::log_message("[coop-brush-audio-client] brush="+std::to_string(mem_read32(object+0x50))+" serial="+std::to_string(events[i])+" stage="+(stop?"stop":"start")+" sound="+std::to_string(sound)+" handle="+std::to_string(call.r[3]));
        }
    }
    audio.serial=serial;
    const bool loop=std::get<int64_t>(p.at("brush_audio_loop"))!=0;const auto sound=mem_read32(object+0x98);
    if(!loop || audio.sound!=sound){
        const bool playing=brush_audio_voice(audio.handle,audio.sound);brush_audio_stop_voice(context,audio);
        if(playing && brush_audio_trace())ssne::log_message("[coop-brush-audio-client] brush="+std::to_string(mem_read32(object+0x50))+" serial="+std::to_string(serial)+" stage=loop-stop sound="+std::to_string(audio.sound));
    }
    audio.sound=sound;
    if(loop && sound!=350 && !brush_audio_voice(audio.handle,sound)){
        auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.lr=0;
        call.r[3]=sound;call.r[4]=object+0x70;ssne_call(call,0x8010ED14);audio.handle=call.r[3];
        // If all native voices are occupied, the next snapshot retries. A
        // successful handle is checked against its generation and sample list.
        if(brush_audio_voice(audio.handle,sound)){
            brush_audio_track(object,audio.handle,serial,2);
            if(brush_audio_trace())ssne::log_message("[coop-brush-audio-client] brush="+std::to_string(mem_read32(object+0x50))+" serial="+std::to_string(serial)+" stage=loop-start sound="+std::to_string(sound)+" handle="+std::to_string(audio.handle)+" baseline="+std::to_string(baseline));
        }
    }
    return true;
}
bool stop_native_brush_audio(PPCContext& context,std::string& error){
    if(!context_ok(context)){error="Native moving-brush audio stop context unavailable";return false;}
    StackScratch scratch(context);for(auto& [object,audio]:brush_replica_audio){brush_audio_stop_voice(context,audio);brush_audio_stop_cues(context,audio);}
    return true;
}
