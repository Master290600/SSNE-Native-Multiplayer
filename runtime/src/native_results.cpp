#include "ssne/native_results.h"
#include "ssne/native_results_countdown.h"
#include <chrono>
#include "ssne/native_entity.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/native_party.h"
#include "ssne/native_cutscene.h"
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <array>
#include <stdexcept>
#include <atomic>
namespace ssne {
namespace {
std::optional<NativeResults> result;
uint32_t gui=0;uint64_t sequence=0,subscription=0,saved_sequence=0;
bool save_host=false;PPCFunc continue_original=nullptr,results_original=nullptr,results_cleanup_original=nullptr,results_setup_original=nullptr;
uint64_t results_setup_serial=0;
std::atomic<bool> host_results_open{false};
NativeResultsCountdown results_countdown;
std::atomic<bool> replica_results_open{false};uint32_t replica_results_gui=0;uint64_t replica_results_sequence=0;
thread_local bool creating_replica_result_title=false;
PPCFunc original_gui_camera_allocator=nullptr;
void result_title_camera(PPCContext& context){
    if(!creating_replica_result_title){ssne_call_original(context,0x800F57DC,original_gui_camera_allocator);return;}
    // Raw GUI camera IDs 6..13 overlap the extended party's HUD/weapon views.
    // Original IDs 14/15 relocate to shared physical cameras 26/27, and the
    // original allocator's mark/free functions already support their bits.
    const auto mask=mem_read32(0x802ABAD0u);
    for(unsigned slot:{14u,15u})if(!(mask&(1u<<slot))){mem_write32(context.r[3],slot);ssne_call(context,0x800F582C);return;}
    fatal("No shared native results camera available");
}
std::array<uint64_t,2> card_error_generation{};
std::array<int32_t,2> card_last_error{};
// Preserve the original error/UI path, but stop failed asynchronous operations
// before they mark the temporary file complete or replace the previous save.
template<uint32_t Address> void trace_card_callback(PPCContext& context){
    const auto channel=context.r[3];const auto code=int32_t(context.r[4]);
    const auto state=0x802DA130u+channel*224u;
    const bool trace=channel<2 && std::getenv("SSNE_TEST_HIDDEN") && std::getenv("SSNE_COOP_TEST_CARD_OBSERVE") && (Address!=0x80105B6Cu || code<0);
    const auto prefix="[native-card-callback] address="+std::to_string(Address)+" channel="+std::to_string(channel)+" code="+std::to_string(code);
    if(trace)log_message(prefix+" before_state="+std::to_string(int32_t(mem_read32(state+0xd4)))+" before_operation="+std::to_string(mem_read32(state+0xdc)));
    constexpr bool guarded=Address==0x80106408u || Address==0x8010649Cu || Address==0x80106554u || Address==0x801066ACu || Address==0x80106678u;
    if(guarded && channel<2 && code<0){
        // The three write callbacks originally ignored negative completion
        // results. In particular 80106554 overwrote error state -6 with 4,
        // allowing cleanup/rename to promote a partially written file.
        ssne_call(context,0x80105B6Cu);
        log_message(prefix+" aborted=1");
    }else ssne_call_original(context,Address,ssne_find_func(Address)->func);
    if constexpr(Address==0x80105B6Cu){
        if(channel<2 && code<0 && int32_t(mem_read32(state+0xd4))<0){
            ++card_error_generation[channel];card_last_error[channel]=code;
        }
    }
    if(trace)log_message(prefix+" after_state="+std::to_string(int32_t(mem_read32(state+0xd4)))+" after_operation="+std::to_string(mem_read32(state+0xdc)));
}
void card_cleanup(PPCContext& context){
    const auto channel=context.r[3];
    const auto generation=channel<2?card_error_generation[channel]:0;
    ssne_call_original(context,0x80105010u,ssne_find_func(0x80105010u)->func);
    // Directory enumeration waits for asynchronous delete/rename, but its
    // final status update can overwrite their error with "ready". The caller
    // treats ready + operation 2 as save completion. Preserve any error from
    // this invocation before returning control to that caller.
    if(channel<2 && generation!=card_error_generation[channel] &&
       int32_t(mem_read32(0x802DA130u+channel*224u+0xd4))>=0){
        context.r[3]=channel;context.r[4]=uint32_t(card_last_error[channel]);
        ssne_call(context,0x80105B6Cu);
        log_message("[native-card-cleanup-error] channel="+std::to_string(channel)+" code="+std::to_string(card_last_error[channel])+" restored=1");
    }
}
void install_card_guards(){
    constexpr std::array<std::pair<uint32_t,uint64_t>,8> fingerprints{{
        {0x80105010u,0xd57d1e8aee5999ccull},
        {0x8010634Cu,0x23be16db54772843ull},{0x80106408u,0xd764b180304120a6ull},
        {0x8010649Cu,0xcd349c3e01d5967eull},{0x80106554u,0xcd349c3e01d5967eull},
        {0x801066ACu,0xbdf9e9fbf2057e22ull},{0x80106678u,0x912b2b560dbe1547ull},
        {0x80105B6Cu,0x25a0e716f17b59e7ull}}};
    for(auto [address,expected]:fingerprints){
        uint64_t hash=14695981039346656037ull;
        for(unsigned i=0;i<32;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}
        if(hash!=expected || !ssne_find_func(address))throw std::runtime_error("Native card callback revision differs");
    }
    ssne_install_hook(0x80105010u,card_cleanup);
    ssne_install_hook(0x8010634Cu,trace_card_callback<0x8010634Cu>);
    ssne_install_hook(0x80106408u,trace_card_callback<0x80106408u>);
    ssne_install_hook(0x8010649Cu,trace_card_callback<0x8010649Cu>);
    ssne_install_hook(0x80106554u,trace_card_callback<0x80106554u>);
    ssne_install_hook(0x801066ACu,trace_card_callback<0x801066ACu>);
    ssne_install_hook(0x80106678u,trace_card_callback<0x80106678u>);
    ssne_install_hook(0x80105B6Cu,trace_card_callback<0x80105B6Cu>);
}
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;}
struct ResultStack {uint32_t base;std::vector<uint8_t> saved;ResultStack(const PPCContext& c,unsigned bytes):base(c.r[1]-bytes),saved(bytes){for(unsigned i=0;i<saved.size();++i)saved[i]=mem_read8(base+i);}~ResultStack(){for(unsigned i=0;i<saved.size();++i)mem_write8(base+i,saved[i]);}};
void setup_results(PPCContext& context){
    ssne_call_original(context,0x80109020u,results_setup_original);++results_setup_serial;
    if(getenv("SSNE_TEST_HIDDEN"))log_message("[coop-results-setup] sequence="+std::to_string(results_setup_serial));
}
void cleanup_results(PPCContext& context){
    const bool closing=context.r[13]==0x802E22A0u && readable(context.r[4],0x18) && mem_read32(context.r[4]+0x14)==4;
    ssne_call_original(context,0x80109418u,results_cleanup_original);
    // The original close callback frees the sparkle object but leaves its
    // global pointer behind. Original host opens always overwrite it; replica
    // opens are lazy and must not reuse that allocation in the next level.
    if(closing){mem_write32(context.r[13]-0x75E8u,0);if(getenv("SSNE_TEST_HIDDEN"))log_message("[coop-results-cleanup] sparkle_retired=1");}
}
void begin_host_results(PPCContext& context){
    const auto menu=context.r[3];
    if(save_host && !replica_simulation() && native_party_extended() && context.r[13]==0x802E22A0u && readable(menu,0x90) && !mem_read32(menu+0x20)){
        // Some campaign levels never open a shared menu before their exit.
        // Initialize its original renderer before the result title dereferences it.
        ResultStack stack(context,0x400);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=menu;call.r[4]=3;call.r[5]=5;call.r[6]=8;call.ps0[1]=call.ps1[1]=u2f(mem_read32(0x802183C4));
        const auto owner=native_party_owner();native_party_owner_set(-1);
        try{ssne_call(call,0x800F6E94);}catch(...){native_party_owner_set(owner);throw;}native_party_owner_set(owner);
        if(!readable(mem_read32(menu+0x20),4))fatal("Native host results renderer failed to initialize");
        log_message("[coop-host-results-renderer] initialized=1");
    }
    const auto saved=creating_replica_result_title;const auto owner=native_party_owner();
    if(save_host && native_party_extended()){creating_replica_result_title=true;native_party_owner_set(-1);}
    try{ssne_call_original(context,0x801089E0u,results_original);}catch(...){creating_replica_result_title=saved;native_party_owner_set(owner);throw;}
    creating_replica_result_title=saved;native_party_owner_set(owner);
    if(save_host && result && readable(gui,0x90))host_results_open=!mem_read32(gui+0x14) && mem_read32(gui+0x6C)!=0;
}
void continue_and_save(PPCContext& context){
    const auto menu=context.r[3];const auto save_sequence=result?result->sequence:0;
    if(replica_simulation() && replica_results_open && menu==replica_results_gui && mem_read32(context.r[13]-0x75F0u)==2)return;
    const auto manager=mem_read32(0x802ABACCu);
    const bool ready=save_host && !replica_simulation() && menu==gui && save_sequence && save_sequence!=saved_sequence && readable(menu,0x90) && mem_read32(0x80000000u)==0x47334250u && context.r[13]==0x802E22A0u && mem_read32(context.r[13]-0x75F0u)==2 && mem_read32(context.r[13]-0x69D0u)>1 && readable(manager,16) && manager==mem_read32(context.r[13]-0x7610u) && !mem_read32(manager+0xC);
    // Native CloseMenu animates before active becomes false. Consume the
    // final Continue once even while its close animation is still visible.
    const bool final_host_continue=save_host && !replica_simulation() && menu==gui && save_sequence && context.r[13]==0x802E22A0u && mem_read32(context.r[13]-0x75F0u)==2;
    if(final_host_continue && results_countdown.was_issued(save_sequence))return;
    if(final_host_continue)results_countdown.issued(save_sequence);
    ssne_call_original(context,0x801091A4u,continue_original);
    if(!ready)return;
    // The original cooperative results updater closes/continues the level,
    // then serializes all four configuration sections only for a single player.
    // Extend that native save branch to the authority, without changing count.
    auto save=context;save.msr&=~0x8000u;save.r[3]=manager;save.r[4]=menu;
    ssne_call(save,0x8010700Cu);saved_sequence=save_sequence;
    log_message("[coop-native-progress-save-request] sequence="+std::to_string(save_sequence)+" players="+std::to_string(mem_read32(context.r[13]-0x69D0u))+" manager="+std::to_string(manager)+" state="+std::to_string(mem_read32(manager+0xC)));
}
}
void set_native_results_save_host(bool enabled){save_host=enabled;results_countdown.reset();host_results_open=false;}
void capture_native_results(uint32_t menu,uint32_t total) {
    if(!subscription || replica_simulation() || !readable(menu,0x90) || !readable(total,4))return;
    NativeResults next;next.sequence=++sequence;next.score=mem_read32(total);
    // ORIGINAL801089E0 populates13 native result variables, stride28. These
    // are host-computed values, never independently recalculated by a client.
    for(unsigned i=0;i<13;++i){next.values[i]=u2f(mem_read32(0x8023A8C0+i*28+8));next.coefficients[i*2]=u2f(mem_read32(0x8023A8C0+i*28+12));next.coefficients[i*2+1]=u2f(mem_read32(0x8023A8C0+i*28+16));if(!std::isfinite(next.values[i]) || std::abs(next.values[i])>1000000000.f)return;}
    result=next;gui=menu;
    std::string values;for(auto value:next.values)values+=(values.empty()?"":",")+std::to_string(value);log_message("[coop-native-results-values] "+values);
    log_message("[coop-native-results] sequence="+std::to_string(next.sequence)+" score="+std::to_string(next.score)+" elapsed_seconds="+std::to_string(next.values[1]*60.f)+" kills="+std::to_string(next.values[3])+" secrets="+std::to_string(next.values[5])+" total_secrets="+std::to_string(next.values[6]));
}
void install_native_results_observer() {
    if(subscription || mem_read32(0x80000000u)!=0x47334250u)return;
    const auto* f=ssne_find_func(0x801089E0);if(!f || mem_read32(f->address)!=0x9421FF88u)return;
    const auto* next=ssne_find_func(0x801091A4u);const auto* save=ssne_find_func(0x8010700Cu);
    if(!next || !save || mem_read32(next->address)!=0x9421FFD8u || mem_read32(save->address)!=0x9421FFE0u)return;
    install_card_guards();
    continue_original=next->func;ssne_install_hook(next->address,continue_and_save);
    const auto* allocator=ssne_find_func(0x800F57DC);if(!allocator || mem_read32(allocator->address)!=0x9421FFF8u)return;
    original_gui_camera_allocator=allocator->func;ssne_install_hook(allocator->address,result_title_camera);
    const auto* cleanup=ssne_find_func(0x80109418u);if(!cleanup || mem_read32(cleanup->address)!=0x9421FFF0u)return;
    const auto* setup=ssne_find_func(0x80109020u);if(!setup || mem_read32(setup->address)!=0x9421FFD0u)return;
    results_setup_original=setup->func;ssne_install_hook(setup->address,setup_results);
    results_cleanup_original=cleanup->func;ssne_install_hook(cleanup->address,cleanup_results);
    results_original=f->func;ssne_install_hook(f->address,begin_host_results);
    subscription=observe_native_entities([](const NativeEntityNotice& n){if(n.event==NativeEntityNotice::Event::LevelExit){result.reset();gui=0;host_results_open=false;results_countdown.reset();replica_results_open=false;replica_results_gui=0;}});
}
std::optional<NativeResults> native_results(){if(!result)return {};auto next=*result;for(unsigned i=0;i<13;++i){next.coefficients[i*2]=u2f(mem_read32(0x8023A8C0+i*28+12));next.coefficients[i*2+1]=u2f(mem_read32(0x8023A8C0+i*28+16));}next.active=readable(gui,0x90) && !mem_read32(gui+0x14) && mem_read32(gui+0x6C)!=0;if(save_host)host_results_open=next.active;const auto high=mem_read32(0x8023A8C0+8*28+0x18);next.high_score=readable(high,8)?mem_read32(high+4):next.score;return next;}
bool native_replica_results_presenting(){return replica_results_open.load();}
bool native_results_presenting(){return host_results_open.load() || replica_results_open.load();}
uint32_t native_results_menu(){return replica_simulation()?replica_results_open?replica_results_gui:0u:host_results_open?gui:0u;}
bool tick_native_results_auto_continue(PPCContext& context,void (*before_continue)(PPCContext&),std::string& error){
    const auto current=native_results();
    const bool authority=save_host && !replica_simulation() && native_party_extended();
    const auto now=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(!results_countdown.due(current?current->sequence:0,current && current->active,authority,now))return true;
    if(context.r[13]!=0x802E22A0u || !readable(context.r[1]-0x3000,0x3000) || !readable(gui,0x90) || !continue_original){error="Original authoritative result continuation context unavailable";return false;}
    // Dispatch the exact native Continue callback, including its fast-forward,
    // level exit and guarded progress save. It closes the menu on its final
    // state; the next observation then clears this countdown. No client exit.
    ResultStack stack(context,0x3000);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=gui;
    const auto owner=native_party_owner();native_party_owner_set(-1);
    const auto phase=mem_read32(context.r[13]-0x75F0u);
    // An open connection panel must not postpone the fifteen-second exit.
    // Restore the original result root before its own Continue closes it.
    try{if(before_continue)before_continue(call);ssne_call(call,0x801091A4u);}catch(...){native_party_owner_set(owner);throw;}native_party_owner_set(owner);
    log_message("[coop-results-auto-continue] sequence="+std::to_string(current->sequence)+" delay_ms=15000 phase="+std::to_string(phase));return true;
}

bool clear_native_replica_results(PPCContext& context,std::string& error){
    if(!replica_results_open)return true;
    if(context.r[13]!=0x802E22A0u || !readable(context.r[1]-0x3000,0x3000) || !readable(replica_results_gui,0x90) || !ssne_find_func(0x800F7C9C) || !ssne_find_func(0x800F6498)){error="Replica result menu close context unavailable";return false;}
    ResultStack stack(context,0x3000);
    struct SharedOwner{int saved=native_party_owner();SharedOwner(){native_party_owner_set(-1);}~SharedOwner(){native_party_owner_set(saved);}} owner;
    auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=replica_results_gui;call.r[4]=0;ssne_call(call,0x800F9AAC);
    // CloseMenu fades active roots. The native level loader cannot advance
    // while that list is nonempty, including an already closing modal root.
    // Use the same original immediate drain as the main-menu bootstrap.
    call.r[3]=replica_results_gui;ssne_call(call,0x800F6498u);
    // Native CloseMenu may retain the shared background for another open GUI.
    // This replica title belongs only to the completed level. Retire it with
    // the original model/sprite/camera destructor before loading the next one.
    if(mem_read32(replica_results_gui+0x40)){call.r[3]=replica_results_gui;ssne_call(call,0x800F7C9C);}
    replica_results_open=false;replica_results_gui=0;return true;
}
bool apply_native_replica_results(PPCContext& context,const NativeResults& desired,std::string& error){
    // Host setup normally installs these hooks for capture. A replica has no
    // host observer setup, but needs the same GUI lifecycle/button hooks.
    install_native_results_observer();
    if(!subscription || !original_gui_camera_allocator || !continue_original){error="Original replica result UI hooks unavailable";return false;}
    const auto slot=netplay_local_player();const auto actor=slot>=0 && slot<8?mem_read32(native_party_registry()+unsigned(slot)*4):0u;
    // Level results belong to the gameplay scene's shared GUI. A player's
    // pause GUI can load BC but is not drawn during the normal result phase.
    const auto scene_menu=netplay_world_ready()?prepare_native_gameplay_gui(context,error):native_gameplay_gui();
    if(netplay_world_ready() && !scene_menu && !error.empty())return false;
    const auto menu=scene_menu?scene_menu:!netplay_world_ready() && readable(actor,0x790)?mem_read32(actor+0x71C):0;
    if(!replica_simulation() || !desired.sequence || context.r[13]!=0x802E22A0u || !readable(context.r[1]-0x800,0x800) || !readable(menu,0x90)){error="Replica original result GUI unavailable";return false;}
    for(auto value:desired.coefficients)if(!std::isfinite(value) || std::abs(value)>1000000000.f){error="Invalid authoritative result coefficient";return false;}
    for(auto value:desired.values)if(!std::isfinite(value) || std::abs(value)>1000000000.f){error="Invalid authoritative result value";return false;}
    for(auto address:{0x800F9978u,0x800F6C14u,0x800F79BCu,0x800F7DACu,0x801091A4u})if(!ssne_find_func(address)){error="Original result UI lifecycle unavailable";return false;}
    if(replica_results_open && replica_results_sequence==desired.sequence)return true;
    ResultStack stack(context,0x800);
    auto call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    // The lazy gameplay GUI was created after the initial global menu setup.
    // If another local GUI has already set the menu-open mask, OpenMenu skips
    // that setup entirely. Prepare this GUI's own original 3D renderer too;
    // otherwise the native bomb/title model never receives an update/draw.
    if(!mem_read32(menu+0x20)){
        if(!ssne_find_func(0x800F6E94)){error="Original result renderer unavailable";return false;}
        call.r[3]=menu;call.r[4]=3;call.r[5]=5;call.r[6]=8;
        call.ps0[1]=call.ps1[1]=u2f(mem_read32(0x802183C4));ssne_call(call,0x800F6E94);
        if(!readable(mem_read32(menu+0x20),4)){error="Original result renderer did not initialize";return false;}
    }
    for(unsigned i=0;i<desired.values.size();++i){mem_write32(0x8023A8C0+i*28+8,f2u(desired.values[i]));mem_write32(0x8023A8C0+i*28+12,f2u(desired.coefficients[i*2]));mem_write32(0x8023A8C0+i*28+16,f2u(desired.coefficients[i*2+1]));}
    // Original801089E0 resets this three-element stat-order cursor. Event5
    // does not: carrying value3 into another result reads the following text
    // bytes ("Scor") as an index and corrupts the formatter/GUI state.
    mem_write32(context.r[13]-0x53F4u,0);
    replica_results_gui=menu;replica_results_sequence=desired.sequence;replica_results_open=true;
    // Original level-result menu BC and its existing variable binding. Neither
    // score calculation, medal award, campaign progress nor saves run here.
    const auto setup_before=results_setup_serial;
    for(const auto [event,callback]:{std::pair{5u,0x80109020u},std::pair{4u,0x80109418u},std::pair{0x16u,0x801091A4u},std::pair{9u,0x80109528u}}){call.r[3]=menu;call.r[4]=0;call.r[5]=0xBC;call.r[6]=event;call.r[7]=callback;ssne_call(call,0x800F6C14);}
    call.r[3]=menu;call.r[4]=0xBC;call.r[5]=0;call.r[6]=context.r[13]-0x52C0;ssne_call(call,0x800F9978);
    // A menu can inherit the global open-mask from another GUI, so OpenMenu
    // need not send event5 again. Its previous result variables were freed by
    // event4; explicitly run the original setup when this open skipped it.
    if(results_setup_serial==setup_before){call.r[3]=menu;ssne_call(call,0x80109020u);}
    struct TitleScope{bool saved=creating_replica_result_title;int owner=native_party_owner();TitleScope(){creating_replica_result_title=true;native_party_owner_set(-1);}~TitleScope(){creating_replica_result_title=saved;native_party_owner_set(owner);}};
    {TitleScope title;call.r[3]=menu;call.r[4]=0x8021A014;call.r[5]=0x8021A01C;call.r[6]=0x8021A02C;call.r[7]=0;ssne_call(call,0x800F79BC);
    call.r[3]=menu;call.r[4]=0x8021A014;ssne_call(call,0x800F7DAC);}
    // The final/high score records use integer animation targets through
    // +18, unlike the first seven float statistics. Do not recalculate them
    // from this client's campaign profile.
    for(unsigned index:{7u,8u}){const auto number=mem_read32(0x8023A8C0+index*28+0x18);if(!readable(number,8)){error="Original result total-score variable unavailable";replica_results_open=false;return false;}const auto total=index==7?desired.score:std::max(desired.score,desired.high_score);mem_write32(number,total);mem_write32(number+4,total);call.r[3]=menu;call.r[4]=index;ssne_call(call,0x80109D34);}
    call.r[3]=menu;call.r[4]=desired.score;ssne_call(call,0x80109970);
    // Original result sparkle effect, with normal menu cleanup.
    if(!mem_read32(context.r[13]-0x75E8u)){
        call.r[3]=mem_read32(context.r[13]-0x6DE0);call.r[4]=0x708;ssne_call(call,0x80116084);const auto bomb=call.r[3];
        if(!readable(bomb,0x708)){error="Original result bomb allocation failed";replica_results_open=false;return false;}
        mem_write32(context.r[13]-0x6DF0,mem_read32(context.r[13]-0x6DF0)-mem_read32(bomb-0x18));
        mem_write32(context.r[1]-0x80,mem_read32(0x8021A058));mem_write32(context.r[1]-0x7C,mem_read32(0x8021A05C));
        mem_write32(context.r[13]-0x75E8,bomb);call.r[3]=bomb;call.r[4]=mem_read32(menu+0x20);call.r[5]=context.r[1]-0x80;call.r[6]=4;ssne_call(call,0x800AE188);
    }
    if(mem_read32(menu+0x14) || !mem_read32(menu+0x6C)){replica_results_open=false;error="Original result menu did not open";return false;}
    const auto title=mem_read32(menu+0x40);const auto camera=readable(title,16)?mem_read32(title+8):~0u;
    if(camera!=14 && camera!=15){error="Original result title used a player camera";replica_results_open=false;return false;}
    log_message("[coop-client-native-results] sequence="+std::to_string(desired.sequence)+" score="+std::to_string(desired.score)+" gui="+std::to_string(menu)+" title_camera="+std::to_string(camera));return true;
}
#include "native_story_state.inc"
}
