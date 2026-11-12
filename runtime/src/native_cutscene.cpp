#include "ssne/native_cutscene.h"
#include "ssne/runtime.h"
#include "ssne/game_addresses.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/native_entity.h"
#include "ssne/native_party.h"
#include <cmath>
#include <vector>
namespace ssne {
namespace {
PPCFunc original=nullptr;
PPCFunc initialize_original=nullptr,update_original=nullptr;
uint32_t presentation_object=0,presentation_engine=0,presentation_view=0,saved_mask=0;
uint32_t host_camera_object=0;
std::array<uint32_t,19> saved_view{};
thread_local bool approved=false;
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;}
bool valid(uint32_t object){return readable(object,0x178) && mem_read32(object+0x48)==0x80203078u;}
void initialize_gate(PPCContext& ctx){
    if(replica_simulation())return;const auto object=ctx.r[3];ssne_call_original(ctx,0x80078184,initialize_original);
    if(valid(object) && mem_read32(object+0x60)){
        host_camera_object=object;
        // Original cinematic mask bit4 conflicts with extended player5 models.
        // The cinematic camera shows world models without first-person weapons.
        const auto engine=mem_read32(0x802ABAA0),slot=native_party_extended()?24u:4u;
        const auto view=native_party_view_engine(engine)+0xDC+slot*76;
        if(readable(view,76))mem_write32(view+0x1C,0x80000000u);
    }
}
void update_gate(PPCContext& ctx){if(replica_simulation())return;ssne_call_original(ctx,0x80077D28,update_original);}
void event_gate(PPCContext& ctx) {
    const auto object=ctx.r[3],event=ctx.r[4];
    if(valid(object) && readable(event,4) && mem_read32(event)==5 && !approved) {
        log_message("[coop-native-cutscene-skip-gated] level_ref="+std::to_string(mem_read32(object+0x50)));return;
    }
    ssne_call_original(ctx,0x80077F74,original);
}
}
void install_native_cutscene_gate(){
    if(original || mem_read32(0x80000000u)!=0x47334250u)return;
    const auto* f=ssne_find_func(0x80077F74);if(!f || mem_read32(f->address)!=0x9421FFE0u)return;
    original=f->func;ssne_install_hook(f->address,event_gate);
    const auto* init=ssne_find_func(0x80078184);const auto* update=ssne_find_func(0x80077D28);
    if(!init || !update || mem_read32(init->address)!=0x9421FF78u || mem_read32(update->address)!=0x9421FFB8u)fatal("Unsupported native scripted cutscene camera.");
    initialize_original=init->func;update_original=update->func;
    ssne_install_hook(init->address,initialize_gate);ssne_install_hook(update->address,update_gate);
    observe_native_entities([](const NativeEntityNotice& notice){
        if(notice.event==NativeEntityNotice::Event::LevelExit){presentation_object=presentation_engine=presentation_view=host_camera_object=0;}
        else if(notice.event==NativeEntityNotice::Event::Removing){if(notice.object==presentation_object)clear_native_cutscene_presentation();if(notice.object==host_camera_object)host_camera_object=0;}
    });
}
bool native_cutscene_is_presenting(uint32_t object){
    if(mem_read32(game_address(0x802DB0E0u))!=4 || !valid(object) || !mem_read32(object+0x60) || !(mem_read32(0x802ABAD0u)&0x10u))return false;
    const auto engine=mem_read32(0x802ABAA0u);
    if(!readable(engine,0x600))return false;
    const auto view=native_party_view_engine(engine)+0xDC+(native_party_extended()?24u:4u)*76;
    // A camera entity can keep updating while gameplay still renders player
    // views. Its local +60 flag alone does not make it the visible cinematic.
    // Mirror only the camera selected by the original host renderer.
    return readable(view,76) && mem_read32(view)==object+0x13Cu && mem_read32(view+0x30)!=0;
}
bool native_cutscene_presenting(){return presentation_object!=0 || native_cutscene_is_presenting(host_camera_object);}
bool validate_native_cutscene_checkpoint(const NativeCutsceneCheckpoint& s,std::string& error){
    if(replica_simulation() || approved || presentation_object){error="Cutscene checkpoint requires idle host context";return false;}
    if(s.active>1 || (s.camera ? (!valid(s.camera) || mem_read32(s.camera+0x50)!=s.reference || bool(mem_read32(s.camera+0x60))!=bool(s.active)) : (s.reference || s.active))){error="Saved cinematic camera does not match restored RAM";return false;}
    return true;
}
bool capture_native_cutscene_checkpoint(NativeCutsceneCheckpoint& s,std::string& error){
    s={host_camera_object,host_camera_object?mem_read32(host_camera_object+0x50):0,host_camera_object?uint32_t(mem_read32(host_camera_object+0x60)!=0):0};
    return validate_native_cutscene_checkpoint(s,error);
}
void restore_native_cutscene_checkpoint(const NativeCutsceneCheckpoint& s){host_camera_object=s.camera;}
unsigned native_cutscene_camera_differences(const std::array<float,14>& camera){
    if(!valid(presentation_object) || !readable(presentation_view,76))return 16;
    unsigned differences=0;for(unsigned i=0;i<14;++i)differences+=mem_read32(presentation_object+0x140+i*4)!=f2u(camera[i]);
    differences+=mem_read32(presentation_view)!=presentation_object+0x13C;
    differences+=mem_read32(presentation_view+0x30)!=1;return differences;
}
void clear_native_cutscene_presentation(){
    if(presentation_object && readable(presentation_view,76) && mem_read32(0x802ABAA0)==presentation_engine){
        for(unsigned i=0;i<saved_view.size();++i)mem_write32(presentation_view+i*4,saved_view[i]);
        mem_write32(0x802ABAD0,saved_mask);
        log_message("[coop-client-cutscene-camera-cleared]");
    }
    presentation_object=presentation_engine=presentation_view=0;
}
bool present_native_cutscene(PPCContext& context,uint32_t object,const std::array<float,14>& camera,std::string& error){
    if(!replica_simulation() || !valid(object) || mem_read32(object+0x13C)!=0x80203320u){error="Replica native camera entity unavailable";return false;}
    for(auto value:camera)if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Invalid replica camera values";return false;}
    if(camera[12]<=0 || camera[12]>=180 || camera[13]<0 || camera[13]>1024){error="Invalid replica camera projection";return false;}
    const auto engine=mem_read32(0x802ABAA0),slot=native_party_extended()?24u:4u;
    if(!readable(engine,0x600)){error="Replica native camera engine unavailable";return false;}
    const auto view=native_party_view_engine(engine)+0xDC+slot*76;
    if(!readable(view,76)){error="Replica native shared view unavailable";return false;}
    if(presentation_object!=object || presentation_engine!=engine){
        clear_native_cutscene_presentation();presentation_object=object;presentation_engine=engine;presentation_view=view;
        saved_mask=mem_read32(0x802ABAD0);for(unsigned i=0;i<saved_view.size();++i)saved_view[i]=mem_read32(view+i*4);
        log_message("[coop-client-cutscene-camera-bound] level_ref="+std::to_string(mem_read32(object+0x50))+" view="+std::to_string(slot));
    }
    for(unsigned i=0;i<camera.size();++i)mem_write32(object+0x140+i*4,f2u(camera[i]));
    // Verified native view fields: camera, full-screen rectangle, world model
    // mask, active. The replica does not execute STARTCUTSCENE mission events.
    mem_write32(view,object+0x13C);for(unsigned i=4;i<=0x10;i+=4)mem_write32(view+i,0);
    mem_write32(view+0x14,0);mem_write32(view+0x18,3);mem_write32(view+0x1C,0x80000000u);mem_write32(view+0x30,1);
    mem_write32(0x802ABAD0,0x10);return true;
}
bool request_native_cutscene_skip(PPCContext& context,uint32_t object,std::string& error){
    if(!original || replica_simulation() || !valid(object) || !mem_read32(object+0x60) || context.r[13]!=0x802E22A0 || !readable(context.r[1]-0x300,0x300)){
        error="Authoritative native cutscene skip unavailable";return false;
    }
    std::vector<uint8_t> stack(0x300);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x300+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    const auto message=context.r[1]-0x60;for(unsigned i=0;i<0x40;i+=4)mem_write32(message+i,0);
    mem_write32(message,5);call.r[3]=object;call.r[4]=message;
    struct Approval{bool previous=approved;Approval(){approved=true;}~Approval(){approved=previous;}} approval;
    try{ssne_call(call,0x80077F74);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x300+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x300+i,stack[i]);
    log_message("[coop-native-cutscene-skip-approved] level_ref="+std::to_string(mem_read32(object+0x50)));return true;
}
}
