#include "ssne/native_spectator.h"
#include "ssne/game_addresses.h"
#include "ssne/native_party.h"
#include "ssne/native_results.h"
#include "ssne/native_cutscene.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include "ssne/recomp_hooks.h"
namespace ssne { namespace {
thread_local NativeSpectatorView* render_view=nullptr;
bool readable(uint32_t p,uint32_t n){return p>=0x80000000u && p<=guest_ram_end() && n<=guest_ram_end()-p && !(p&3u);}
bool overlap(uint32_t a,uint32_t an,uint32_t b,uint32_t bn){return uint64_t(a)<uint64_t(b)+bn && uint64_t(b)<uint64_t(a)+an;}
bool player(uint32_t actor,unsigned slot){return readable(actor,0x790) && mem_read32(actor+0x48)==0x802121E8u && mem_read32(actor+0x17C)==slot;}
bool camera(uint32_t object,uint32_t actor){return readable(object,0xE0) && mem_read32(object)==0x80210880u && mem_read32(object+4)==actor && !overlap(object,0xE0,actor,0x790);}
bool matrix(const std::array<uint32_t,12>& bits){
    std::array<float,12> m;for(unsigned i=0;i<12;++i){m[i]=u2f(bits[i]);if(!std::isfinite(m[i]) || std::abs(m[i])>(i%4==3?1000000.f:2.f))return false;}
    for(unsigned row=0;row<3;++row){float norm=0.f;for(unsigned col=0;col<3;++col)norm+=m[row*4+col]*m[row*4+col];if(std::abs(norm-1.f)>.05f)return false;}
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b){float dot=0.f;for(unsigned col=0;col<3;++col)dot+=m[a*4+col]*m[b*4+col];if(std::abs(dot)>.05f)return false;}
    const float det=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);return std::abs(det-1.f)<=.08f;
}
bool hud_root(uint32_t hud,uint32_t root,uint32_t engine){
    return readable(hud,0x534) && mem_read32(hud)==root && readable(root,0x44) && mem_read32(root)==0x8021CAB0u &&
        mem_read32(root+4)==engine+4 && mem_read32(root+8)==engine;
}
bool revision(){
    // Original PAL camera getter copies +24. The body-mask facade forwards
    // +18 to four handles whose checked setter writes only handle+30.
    return mem_read32(0x80000000u)==0x47334250u && mem_read32(0x800A6A8Cu)==0x9421FFF8u &&
        mem_read32(0x80210880u+0x14)==0x800A5EDCu && mem_read16(0x80210880u+0x10)==0 &&
        mem_read32(0x801527C0u)==0x9421FFF0u && mem_read32(0x801527DCu)==0x93DF0018u &&
        mem_read32(0x8013E814u)==0x9421FFF0u && mem_read32(0x8013E82Cu)==0x90830030u && mem_read32(0x8021CAB0u+0x44)==0x8011D458u &&
        mem_read32(0x8011D458u)==0x2C040002u && mem_read32(0x8011D4A0u)==0xA003001Eu &&
        mem_read32(0x8011D4A4u)==0x60000001u && mem_read32(0x8011D4A8u)==0xB003001Eu;
}
}
NativeSpectatorView::~NativeSpectatorView(){std::string ignored;end(ignored);}
bool NativeSpectatorView::begin(PPCContext& context,unsigned local_slot,unsigned target_slot,std::string& error){
    if(active_ || render_view){error="Spectator render lease already active";return false;}
    if(!revision() || context.r[13]!=0x802E22A0u || context.r[2]!=0x802F22A0u ||
       mem_read32(game_address(0x802DB0E0u))!=4 || !netplay_world_ready() || local_slot>=native_party_capacity() ||
       netplay_local_player()!=int(local_slot) || target_slot>no_spectator_target || target_slot==local_slot ||
       native_results_presenting() || native_replica_results_presenting() || native_cutscene_presenting() || native_movie_presenting() ||
       local_information_open() || local_pause_menu_open()){error="Spectator view unavailable during native transition";return false;}
    const auto registry=native_party_registry(),engine=mem_read32(0x802ABAA0u);
    const unsigned capacity=native_party_capacity();
    if(!readable(registry,capacity*4u) || !readable(engine,0x768) || mem_read32(engine)!=0x8021C600u ||
       (native_party_extended() && !native_party_render_layout_safe(engine))){error="Spectator party/view layout unavailable";return false;}
    const auto local_actor=mem_read32(registry+local_slot*4u);
    const auto local_camera=readable(local_actor,0x790)?mem_read32(local_actor+0x368):0u;
    const auto view=native_party_view_engine(engine)+0xDC+local_slot*76u;
    if(!player(local_actor,local_slot) || !camera(local_camera,local_actor) || native_party_actor_occupied(local_actor) ||
       !readable(view,76) || overlap(view,76,local_camera,0xE0) || (mem_read32(view) && !readable(mem_read32(view),4)) || mem_read32(view+0x30)>1){error="Spectator local actor is not a reserved observer";return false;}
    std::array<Word,40> backup{};std::array<uint32_t,40> next{};unsigned count=0;
    auto add=[&](uint32_t address,uint32_t value){
        if(!readable(address,4) || count>=backup.size())return false;
        for(unsigned i=0;i<count;++i)if(backup[i].address==address)return next[i]==value;
        backup[count]={address,mem_read32(address)};next[count++]=value;return true;
    };
    if(!add(view,local_camera) || !add(view+0x30,1)){error="Spectator world view unavailable";return false;}
    const unsigned view_words=count;
    uint32_t target_actor=0,proxy=0;std::array<uint32_t,4> handles{};unsigned camera_words=view_words;
    if(target_slot!=no_spectator_target){
        if(target_slot>=capacity){error="Spectator target slot outside party";return false;}
        target_actor=mem_read32(registry+target_slot*4u);
        const auto target_camera=readable(target_actor,0x790)?mem_read32(target_actor+0x368):0u;
        if(!player(target_actor,target_slot) || !camera(target_camera,target_actor) || !native_party_actor_occupied(target_actor) ||
           overlap(local_actor,0x790,target_actor,0x790) || overlap(local_camera,0xE0,target_camera,0xE0) ||
           mem_read32(target_actor+0x328)!=0 || !std::isfinite(u2f(mem_read32(target_actor+0xCC))) || !(u2f(mem_read32(target_actor+0xCC))>0.f)){
            error="Spectator target is no longer a live participant";return false;
        }
        std::array<uint32_t,12> target_matrix{};
        for(unsigned i=0;i<12;++i)target_matrix[i]=mem_read32(target_camera+0x24+i*4u);
        if(!matrix(target_matrix)){error="Spectator target camera cache incomplete";return false;}
        for(unsigned i=0;i<12;++i)if(!add(local_camera+0x24+i*4u,target_matrix[i]) || !add(local_camera+0x54+i*4u,target_matrix[i]))return false;
        for(unsigned offset:{0x84u,0x88u}){
            const auto value=mem_read32(target_camera+offset);if(!std::isfinite(u2f(value)) || std::abs(u2f(value))>10000.f){error="Spectator target camera effects invalid";return false;}
            if(!add(local_camera+offset,value))return false;
        }
        const auto mode=mem_read32(target_camera+0xAC);if(mode>4 || !add(local_camera+0xAC,mode)){error="Spectator target camera mode invalid";return false;}
        camera_words=count;
        proxy=mem_read32(target_actor+0x148);
        const auto model=readable(proxy,0x20)?mem_read32(proxy+0x10):0u;
        if(!readable(proxy,0x20) || overlap(proxy,0x20,local_camera,0xE0) || overlap(proxy,0x20,target_actor,0x790) ||
           !readable(model,0xB0) || mem_read32(model)!=0x8021D668u || !add(proxy+0x18,0u)){
            error="Spectator body-mask owner unavailable";return false;
        }
        for(unsigned i=0;i<4;++i){
            const auto handle=mem_read32(proxy+i*4u);handles[i]=handle;if(!handle)continue;
            const auto table=readable(handle,0x34)?mem_read32(handle):0u;
            if(!readable(handle,0x34) || !readable(table,0xD8) || mem_read16(table+0xD0)!=0 || mem_read32(table+0xD4)!=0x8013E814u ||
               overlap(handle,0x34,local_camera,0xE0) || overlap(handle,0x34,local_actor,0x790) || overlap(handle,0x34,target_actor,0x790) || !add(handle+0x30,0u)){
                error="Spectator body render-mask schema differs";return false;
            }
        }
    }
    const unsigned body_words=count;
    // The original HUD is an engine GUI graph, not a viewport-local camera.
    // Hide only its owned root for this render; shared Pause and session HUD
    // roots stay live. Original GUIBase8011D458 option1 sets bit0 at+1E.
    const auto hud=mem_read32(local_actor+0x374),root=readable(hud,0x534)?mem_read32(hud):0u;
    const auto scene=native_game_scene();
    if(!hud_root(hud,root,engine) || !readable(scene,0x7C) || root==mem_read32(scene+0x78) || root==mem_read32(local_actor+0x71C) ||
       overlap(root,0x44,local_actor,0x790) || overlap(root,0x44,local_camera,0xE0)){
        error="Spectator local HUD root ownership unavailable";return false;
    }
    for(unsigned slot=0;slot<capacity;++slot)if(slot!=local_slot){
        const auto other=mem_read32(registry+slot*4),other_hud=player(other,slot)?mem_read32(other+0x374):0u;
        if(readable(other_hud,4) && mem_read32(other_hud)==root){error="Spectator HUD root is shared with another player";return false;}
    }
    if(!add(root+0x1C,mem_read32(root+0x1C)|1u)){error="Spectator HUD visibility lease unavailable";return false;}
    // Complete validation and backup precede all writes. No original callback,
    // scheduler poll, allocator or world-position write occurs in this scope.
    words_=backup;count_=count;view_words_=view_words;camera_words_=camera_words;body_words_=body_words;hud_=hud;hud_root_=root;local_=local_slot;target_=target_slot;local_actor_=local_actor;target_actor_=target_actor;camera_=local_camera;proxy_=proxy;engine_=engine;view_=view;handles_=handles;extended_=native_party_extended();
    for(unsigned i=0;i<count_;++i)mem_write32(words_[i].address,next[i]);
    active_=true;render_view=this;return true;
}
bool NativeSpectatorView::end(std::string& error){
    if(!active_)return true;
    if(render_view==this)render_view=nullptr;
    active_=false;
    const auto registry=native_party_registry();
    const bool layout=revision() && readable(registry,native_party_capacity()*4u);
    const bool view_valid=layout && readable(engine_,0x768) && mem_read32(0x802ABAA0u)==engine_ && mem_read32(engine_)==0x8021C600u && local_<native_party_capacity() &&
        native_party_view_engine(engine_)+0xDC+local_*76u==view_ && readable(view_,76) && mem_read32(view_)==camera_;
    const bool local_valid=layout && local_<native_party_capacity() &&
        mem_read32(registry+local_*4u)==local_actor_ && player(local_actor_,local_) && mem_read32(local_actor_+0x368)==camera_ && camera(camera_,local_actor_);
    bool target_valid=true;
    if(target_!=no_spectator_target){
        target_valid=layout && target_<native_party_capacity() && mem_read32(registry+target_*4u)==target_actor_ && player(target_actor_,target_) &&
            mem_read32(target_actor_+0x148)==proxy_ && readable(proxy_,0x20);
        if(target_valid)for(unsigned i=0;i<4;++i){
            if(mem_read32(proxy_+i*4u)!=handles_[i]){target_valid=false;break;}
            if(handles_[i]){const auto table=readable(handles_[i],0x34)?mem_read32(handles_[i]):0u;if(!readable(table,0xD8) || mem_read16(table+0xD0)!=0 || mem_read32(table+0xD4)!=0x8013E814u){target_valid=false;break;}}
        }
    }
    const bool hud_valid=layout && local_<native_party_capacity() && mem_read32(registry+local_*4u)==local_actor_ && player(local_actor_,local_) &&
        mem_read32(local_actor_+0x374)==hud_ && hud_root(hud_,hud_root_,engine_);
    // Restore independent still-live owners even if the other owner was retired.
    // This prevents a valid teammate body retaining the temporary hidden mask.
    for(unsigned i=0;i<count_;++i)if(i<view_words_?view_valid:i<camera_words_?local_valid:i<body_words_?target_valid:hud_valid)mem_write32(words_[i].address,words_[i].original);
    const bool valid=view_valid && local_valid && target_valid && hud_valid;
    if(!valid)error="Spectator render resource lifetime changed; stale writes suppressed";
    count_=view_words_=camera_words_=body_words_=0;hud_=hud_root_=0;local_=target_=no_spectator_target;local_actor_=target_actor_=camera_=proxy_=engine_=view_=0;handles_={};return valid;
}
uint32_t NativeSpectatorView::filter_render_mask(uint32_t mask)const{
    if(!active_ || local_>=8)return mask;
    return (mask|(1u<<local_))&~((1u<<(local_+(extended_?8u:6u)))|(1u<<(local_+(extended_?16u:10u))));
}
uint32_t native_spectator_filter_render_mask(uint32_t mask){return render_view?render_view->filter_render_mask(mask):mask;}
namespace {
thread_local NativeSpectatorMenu* local_menu_owner=nullptr;
std::atomic<bool> spectator_menu_visible{false};
PPCFunc menu_close_original=nullptr,menu_close_previous=nullptr;
bool menu_close_installed=false;
bool menu_context(const PPCContext& c){return c.r[2]==0x802F22A0u && c.r[13]==0x802E22A0u && c.r[1]>=0x80005000u && c.r[1]<=guest_ram_end()-4u;}
bool menu_revision(){
    return mem_read32(0x80000000u)==0x47334250u && ssne_find_func(0x800F9978u) && ssne_find_func(0x800F9AACu) && ssne_find_func(0x800F643Cu) && ssne_find_func(0x800F56E0u) &&
        mem_read32(0x800F9978u)==0x9421FFE8u && mem_read32(0x800F9AACu)==0x9421FFF0u && mem_read32(0x800F643Cu)==0x9421FFF0u && mem_read32(0x800F56E0u)==0x38000000u;
}
void menu_call(PPCContext& context,unsigned slot,uint32_t function,uint32_t gui,uint32_t arg=0,uint32_t arg5=0,uint32_t arg6=0){
    struct Scope {
        uint32_t stack;int owner;std::array<uint8_t,0x2000> bytes;
        explicit Scope(PPCContext& c):stack(c.r[1]-0x2000u),owner(native_party_owner()){std::memcpy(bytes.data(),host_ptr(stack),bytes.size());}
        ~Scope(){std::memcpy(host_ptr(stack),bytes.data(),bytes.size());native_party_owner_set(owner);}
    } restore(context);
    auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200u;mem_write32(call.r[1],context.r[1]);call.r[3]=gui;call.r[4]=arg;call.r[5]=arg5;call.r[6]=arg6;
    native_party_owner_set(int(slot));ssne_call(call,function);
}
}
struct NativeSpectatorMenuAccess {
    static void close(PPCContext& context){
        if(local_menu_owner && local_menu_owner->owns_ && context.r[3]==local_menu_owner->gui_ && local_menu_owner->binding_valid(context)){
            std::string why;if(!local_menu_owner->close_owned(context,why))throw std::runtime_error(why);return;
        }
        ssne_call_original(context,0x800F9AACu,menu_close_previous?menu_close_previous:menu_close_original);
    }
};
bool NativeSpectatorMenu::binding_valid(const PPCContext& c)const{
    if(!menu_context(c) || !menu_revision() || slot_>=native_party_capacity() || netplay_local_player()!=int(slot_) || replica_simulation())return false;
    const auto registry=native_party_registry();
    return readable(registry,native_party_capacity()*4u) && mem_read32(registry+slot_*4u)==actor_ && player(actor_,slot_) &&
        mem_read32(actor_+0x71C)==gui_ && readable(gui_,0x104) && mem_read32(game_address(0x802DB0E0u))==4;
}
void NativeSpectatorMenu::reset(){
    if(local_menu_owner==this){local_menu_owner=nullptr;spectator_menu_visible.store(false);}
    epoch_=0;actor_=gui_=0;slot_=no_spectator_target;start_down_=owns_=closing_=false;
}
bool NativeSpectatorMenu::close_owned(PPCContext& c,std::string& error){
    if(!owns_ || closing_)return true;
    if(!binding_valid(c)){reset();return true;}
    closing_=true;const auto mask_address=c.r[13]-0x765Cu,mask=mem_read32(mask_address);
    try {
        // Exactly the GUI-specific cleanup used by original800F9A00, without
        // iterating any other GUI or calling global pause/input/audio resume.
        menu_call(c,slot_,0x800F643Cu,gui_);
        mem_write32(mask_address,mem_read32(mask_address)&~(1u<<slot_));
        owns_=closing_=false;if(local_menu_owner==this)local_menu_owner=nullptr;spectator_menu_visible.store(false);return true;
    }catch(const std::exception& e){mem_write32(mask_address,mask);closing_=false;error=e.what();return false;}
}
bool NativeSpectatorMenu::update(PPCContext& c,uint64_t epoch,unsigned slot,const PadState& raw,bool watching,bool& consume_start,std::string& error){
    consume_start=false;const bool down=(raw.buttons&PAD_START)!=0;
    if(epoch_ && (epoch_!=epoch || slot_!=slot)){reset();}
    if(replica_simulation()){reset();return true;} // the existing replica path handles both living and dead observers
    if(!watching && !owns_){start_down_=down;return true;}
    if(!menu_context(c) || !menu_revision()){reset();error="Original local spectator menu unavailable";return false;}
    if(!epoch || slot>=native_party_capacity() || netplay_local_player()!=int(slot) || !netplay_world_ready() || mem_read32(game_address(0x802DB0E0u))!=4){reset();return true;}
    const auto registry=native_party_registry(),actor=readable(registry,native_party_capacity()*4u)?mem_read32(registry+slot*4u):0u;
    const auto gui=player(actor,slot)?mem_read32(actor+0x71C):0u;
    if(!player(actor,slot) || !readable(gui,0x104)){reset();error="Spectator local pause GUI unavailable";return false;}
    if(actor_ && (actor_!=actor || gui_!=gui)){reset();}
    epoch_=epoch;slot_=slot;actor_=actor;gui_=gui;
    if(owns_ && !(mem_read32(c.r[13]-0x765Cu)&(1u<<slot_))){owns_=false;if(local_menu_owner==this)local_menu_owner=nullptr;spectator_menu_visible.store(false);}
    const bool blocked=native_results_presenting() || native_replica_results_presenting() || native_cutscene_presenting() || native_movie_presenting() || local_information_open();
    if(!watching || blocked){start_down_=down;return close_owned(c,error);}
    if(native_party_actor_occupied(actor_)){start_down_=down;error="Spectator menu requires inactive gameplay body";return false;}
    consume_start=down;const bool pressed=down && !start_down_;start_down_=down;
    if(!pressed){spectator_menu_visible.store(owns_);return true;}
    if(owns_)return close_owned(c,error);
    if(local_menu_owner && local_menu_owner!=this){error="Another spectator menu owns the local GUI";return false;}
    auto* close_slot=ssne_hook_slot(0x800F9AACu);
    if(!menu_close_installed){menu_close_original=ssne_find_func(0x800F9AACu)->func;menu_close_previous=*close_slot;ssne_install_hook(0x800F9AACu,NativeSpectatorMenuAccess::close);menu_close_installed=true;}
    else if(*close_slot!=NativeSpectatorMenuAccess::close){error="Local pause-close hook ownership changed";return false;}
    const auto mask_address=c.r[13]-0x765Cu,mask=mem_read32(mask_address);
    try {
        if(int16_t(mem_read16(gui_+0x84))!=int(slot_))menu_call(c,slot_,0x800F56E0u,gui_,slot_);
        // Set our OWN local pause bit before entering the original open action.
        // That deliberately skips its global800F986C head, while preserving the
        // existing frontend hook and the native6178/6290 local GUI tail.
        mem_write32(mask_address,mask|(1u<<slot_));
        owns_=true;local_menu_owner=this;menu_call(c,slot_,0x800F9978u,gui_,0x129u,0x134u,c.r[13]-0x53A0u);
        spectator_menu_visible.store(true);return true;
    }catch(const std::exception& e){mem_write32(mask_address,mask);owns_=false;if(local_menu_owner==this)local_menu_owner=nullptr;spectator_menu_visible.store(false);error=e.what();return false;}
}
bool native_spectator_menu_open(){return spectator_menu_visible.load();}
namespace {
bool objective_fingerprint(uint32_t address,unsigned length,uint64_t expected){
    if(!readable(address,length) || !ssne_find_func(address))return false;
    uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<length;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}return hash==expected;
}
bool held_model(uint32_t actor){
    const auto model=mem_read32(actor+0x164);if(!model)return true;
    if(!readable(model,4))return false;const auto table=mem_read32(model);
    return readable(table,0x18) && mem_read16(table+0x10)==0 && ssne_find_func(mem_read32(table+0x14));
}
}
bool release_native_spectator_objectives(PPCContext& c,uint32_t actor,std::string& error){
    error.clear();
    if(!menu_context(c) || mem_read32(0x80000000u)!=0x47334250u || replica_simulation() || !netplay_world_ready() || mem_read32(game_address(0x802DB0E0u))!=4){error="Spectator objective release requires an authoritative PAL gameplay context";return false;}
    const auto capacity=native_party_capacity(),registry=native_party_registry();
    if(!capacity || capacity>8 || !readable(actor,0x790) || !readable(registry,capacity*4)){error="Spectator objective player registry unavailable";return false;}
    const auto slot=mem_read32(actor+0x17C);
    if(slot>=capacity || !player(actor,slot) || mem_read32(registry+slot*4)!=actor){error="Spectator objective player binding differs";return false;}
    const auto flag=mem_read32(actor+0x694),bomb=mem_read32(actor+0x6A4),mode=mem_read32(0x802DB8CCu);
    if(flag>1 || bomb>1 || (flag && bomb) || (flag && mode!=3) || (bomb && mode!=4)){error="Spectator objective carrier mode differs";return false;}
    if(!flag && !bomb)return true;
    if(!readable(mem_read32(actor+0x374),0x534) || !held_model(actor) || !objective_fingerprint(0x800B72A0u,0x70,0x8CBB56F648A61EDFull)){error="Spectator held-objective visual lifecycle unavailable";return false;}
    uint32_t object=0;
    if(flag){
        object=mem_read32(actor+0x698);
        if(!readable(object,0xD0) || mem_read32(object+0x48)!=0x80206C80u || mem_read16(0x80206C80u+0xC0)!=0 || mem_read32(0x80206C80u+0xC4)!=0x8009C300u || !ssne_find_func(0x8009C300u) ||
           !objective_fingerprint(0x800B4A18u,0xBC,0xE706277C4C46570Cull) || !objective_fingerprint(0x800808E0u,0x78,0x057CD7DEBF2B6728ull) || !objective_fingerprint(0x80080F5Cu,0x84,0x2324CC81C1947EB1ull)){
            error="Spectator flag-drop binding or original revision unavailable";return false;
        }
        for(unsigned i=0;i<3;++i){const float value=u2f(mem_read32(actor+0x20+i*4));if(!std::isfinite(value) || std::abs(value)>1000000.f){error="Spectator flag-drop position is invalid";return false;}}
    }else if(mem_read32(c.r[13]-0x54B4u)!=actor || !objective_fingerprint(0x800B4B28u,0x98,0xB2465EEA1EE1EA4Aull) || !objective_fingerprint(0x80168778u,0x2C,0xA863649D7389B155ull)){
        error="Spectator bomb carrier or original revision unavailable";return false;
    }
    try {
        // Original drop detaches the held visual, updates HUD/mode ownership,
        // and (flag) recreates the dropped world pickup at the actor position.
        // Never clear carrier fields by hand or synthesize a death/weapon tick.
        menu_call(c,slot,flag?0x800B4A18u:0x800B4B28u,actor,0);
    }catch(const std::exception& e){error=std::string("Original spectator objective release failed: ")+e.what();return false;}
    if(mem_read32(actor+0x694) || mem_read32(actor+0x6A4) || mem_read32(actor+0x164) || (flag && (mem_read32(actor+0x698) || !readable(mem_read32(object+0x60),4))) || (bomb && mem_read32(c.r[13]-0x54B4u)==actor)){
        error="Original spectator objective release did not complete";return false;
    }
    return true;
}
}

