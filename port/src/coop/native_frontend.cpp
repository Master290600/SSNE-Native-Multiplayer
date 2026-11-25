#include "ssne/performance.h"
#include "native_frontend.h"
#include "frontend_startup.h"
#include "checkpoint_codec.h"
#include "native_lobby.h"
#include "native_main_exit.h"
#include "native_main_online.h"
#include "native_online_setup.h"
#include "../crosshair_image.h"
#include "ssne/custom_crosshair.h"
#include "custom_level_menu.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "ssne/native_party.h"
#include "ssne/native_results.h"
#include "ssne/replica_simulation.h"
#include <map>
#include <mutex>
#include <atomic>
#include <deque>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <optional>
#include <iomanip>
#include <filesystem>
#include <cmath>
#include "invite_code.h"
#include "ps2_content_identity.h"
#include "native_profile_skin_select.h"
#include "native_survival_setup.h"
namespace port::coop { unsigned native_root_id(uint32_t owner); namespace {
constexpr unsigned group=0x148,title=group+1,first=group+2,description=group+6;
constexpr unsigned result_connection=0x151,lobby_start=0x152,lobby_connection=0x153,lobby_bots=0x154,options_button=0x155,survival_button=0x156,custom_levels_button=0x157,lobby_map=0x158,lobby_ready_button=0x159,profile_skin_selector=0x15b,lobby_invite=0x169;
// The native GUI looks up children by ID across retained menu panels.
constexpr unsigned no_custom_levels_label=0x16c;
enum class Page {Closed,Home,Host,Join,Connecting,Error,Bots,Connection,Rooms,Options,BrowserTools,InviteHost,InviteJoin,InviteStatus,OnlineSetup,SessionOptions,Direct};
enum class Edit {None,Address,Pin,Port,Name,Invitation,HostName};
bool name_field(Edit field){return field==Edit::Name || field==Edit::HostName;}
unsigned edit_limit(Edit field){return field==Edit::Pin?6u:field==Edit::Port?5u:field==Edit::Name?24u:field==Edit::HostName?32u:64u;}
void edit(Edit field);
void accept_edit(PPCContext& c);
FrontText lobby_invitation_label();
bool setup_restore_skin=false;
std::string setup_profile_name;
NativeFrontendServices services;
FrontendStartupReadiness startup_readiness;
std::atomic<int> startup_language_override{-1};
MultiplayerOptions options;Page options_return=Page::Home;bool options_save_failed=false;
Page rooms_return=Page::Join;
Page bots_return=Page::Closed;
Page host_details_return=Page::OnlineSetup;
Page direct_return=Page::Join;
FrontendEndpoint host_endpoint;
void save_options(){
    if(services.options_path.empty())return;
    std::ofstream out(services.options_path,std::ios::binary|std::ios::trunc);
    out<<"SSNE-MP-2\n"<<std::quoted(options.name)<<' '<<options.enemies<<' '<<options.capacity<<' '<<options.pickups<<'\n'
       <<std::quoted(options.host_name)<<' '<<std::quoted(options.pin)<<' '<<options.use_password<<' '<<options.skin<<'\n'<<host_endpoint.port<<'\n';out.close();
    options_save_failed=!out;if(options_save_failed)ssne::log_message("[native-network-options] save failed");
}

PPCFunc open_original=nullptr,select_original=nullptr,destroy_original=nullptr,language_original=nullptr,scene_original=nullptr,pause_original=nullptr,gui_update_original=nullptr;
struct Storage {uint32_t heap=0,memory=0,extra=0;};
struct Registry {uint32_t heap=0,memory=0,definitions=0,count=0;} registry;
std::map<uint32_t,Storage> storage;
uint32_t gui=0;Page page=Page::Closed;std::atomic<bool> visible{false};
bool footer_root(unsigned id,unsigned root){
    // Original confirmation dialogs allocate IDs dynamically. The ID
    // alone cannot identify one of our buttons in another menu.
    if(id==group+7 || id==group+8)return root==0x129;
    if(id==result_connection)return false;
    if(id==lobby_connection || id==lobby_bots || id==lobby_start || id==lobby_map || id==lobby_ready_button || id==lobby_invite)return root==0x9A;
    return id==options_button && root==group;
}
bool has_options_button(){return page==Page::OnlineSetup || page==Page::Direct || page==Page::Connection || page==Page::Bots || page==Page::Options || page==Page::Rooms || page==Page::BrowserTools || page==Page::SessionOptions;}
FrontText edit_error=FrontText::Count;
std::atomic<Edit> editing{Edit::None};std::atomic<uint64_t> input{0};
std::atomic<unsigned> hotkey{0};unsigned action=99,skill=2,cursor=0;bool changing=false,local_mode=false,setup_seen=false,wait_release=false,notify_error=false,last_host=false,host_pin_initialized=false;
bool waiting_for_host_world=false;
uint64_t mouse_epoch=1;
unsigned focus=0,room_offset=0,main_idle_frames=0;bool replace_all=false;
LanRoomSort room_sort=LanRoomSort::Ping;LanRoomFilter room_filter;
std::vector<FrontendRoom> room_view;
std::array<std::optional<FrontendRoom>,3> displayed_room_rows;
std::optional<FrontendRoom> room_details_selection;
std::optional<FrontendRoom> pending_room;
void choose_frontend_action(unsigned chosen){
    action=chosen;pending_room=page==Page::Rooms && chosen<displayed_room_rows.size()?displayed_room_rows[chosen]:std::nullopt;
}
std::vector<FrontendRoom> frontend_rooms(){
    auto fresh=sorted_lan_rooms(services.rooms?services.rooms():std::vector<FrontendRoom>{},room_sort,room_filter);
    room_view=stable_lan_room_view(room_view,std::move(fresh));return room_view;
}
void reset_room_view(){room_view.clear();room_offset=0;displayed_room_rows={};room_details_selection.reset();pending_room.reset();++mouse_epoch;}
FrontendEndpoint endpoint;std::optional<FrontendRoom> retry_room;std::string edit_buffer,error,invitation_code;bool last_invitation=false;
uint64_t lobby_copy_sequence=0;
NativeInvitationCopyState lobby_copy_state=NativeInvitationCopyState::Idle;
std::chrono::steady_clock::time_point lobby_copy_feedback_until{};
std::mutex text_mutex;std::deque<std::pair<unsigned,std::string>> text_events;
std::string pending_session_error;std::atomic<bool> session_error_pending{false};
std::mutex custom_map_mutex;NativeCustomMapProgress custom_map_progress;
std::atomic<NativeCustomMapPhase> custom_map_phase{NativeCustomMapPhase::None};
std::atomic<bool> custom_map_cancel{false};
uint32_t transfer_pause_gui=0;int transfer_pause_slot=-1;bool transfer_pause_was_open=false;
std::atomic<uint32_t> transfer_lobby_gui{0};
struct TransferPartyScope {int previous=ssne::native_party_owner();explicit TransferPartyScope(int slot){ssne::native_party_owner_set(slot);}~TransferPartyScope(){ssne::native_party_owner_set(previous);}};
bool custom_map_busy(){const auto phase=custom_map_phase.load();return phase!=NativeCustomMapPhase::None && phase!=NativeCustomMapPhase::Ready;}
std::string quoted_json(const std::string& value){std::string out="\"";for(unsigned char ch:value){if(ch=='\"'||ch=='\\')out+='\\';if(ch>=32)out+=char(ch);}return out+'\"';}
std::array<std::string,8> labels{};
void invalidate_labels(){labels.fill("\1");}
bool in_pause=false,engine_scope=false,result_overlay=false;
bool survival_selection_dispatch=false;
struct SurvivalSelectionScope {
    bool previous=survival_selection_dispatch;
    SurvivalSelectionScope(){survival_selection_dispatch=true;}
    ~SurvivalSelectionScope(){survival_selection_dispatch=previous;}
};
uint32_t result_definition=0,result_flags=0,result_focus=0,result_group_definition=0,result_group_flags=0;
std::atomic<bool> return_main_requested{false},handoff_release{false};
std::chrono::steady_clock::time_point next_back;
std::atomic<bool> paused{false};
uint32_t call(PPCContext& base,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t d=0,uint32_t e=0,uint32_t f=0){auto c=base;c.r[1]-=0x200;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);c.r[3]=a;c.r[4]=b;c.r[5]=d;c.r[6]=e;c.r[7]=f;ssne_call(c,fn);return c.r[3];}
uint32_t item(PPCContext& c,unsigned id){return gui?call(c,0x800F6030,gui,id):0;}
void retire_result_overlay(PPCContext& c,bool restore_focus){
    if(!result_overlay)return;
    // Restore BC without its open/close callbacks. Move native focus before
    // closing our root so ClosePanel cannot reopen BC and rerun result setup.
    if(result_definition)mem_write32(result_definition+0xc,result_flags);
    const auto previous_focus=result_focus,overlay_definition=result_group_definition,overlay_flags=result_group_flags;
    result_overlay=false;result_definition=result_group_definition=result_focus=0;
    visible=false;page=Page::Closed;editing=Edit::None;hotkey=0;action=99;handoff_release=true;
    const auto was_changing=changing;changing=true;
    if(gui){
        if(restore_focus && previous_focus){call(c,0x800F6B6C,gui);call(c,0x800F6A94,gui,previous_focus);}
        else call(c,0x800F6B6C,gui);
        // A hidden root cannot finish its native close animation and remains
        // linked in GUI+6C, blocking the scene loader. Close only our visible
        // root normally; preserve its native closing bit until removal.
        if(overlay_definition){mem_write32(overlay_definition+0xc,mem_read32(overlay_definition+0xc)&~0x30u);call(c,0x800F6178,gui,group);mem_write32(overlay_definition+0xc,(mem_read32(overlay_definition+0xc)&~0x40u)|(overlay_flags&0x40u));}
    }
    changing=was_changing;
}
const char* tr(FrontText id){return frontend_text(id,native_menu_language());}
FrontText room_mode(unsigned mode){switch(mode){case 1:return FrontText::ModeCoop;case 2:return FrontText::ModeDeathmatch;case 3:return FrontText::ModeFlag;case 4:return FrontText::ModeBomb;case 5:return FrontText::Survival;default:return FrontText::AllModes;}}
FrontText room_sort_label(){switch(room_sort){case LanRoomSort::Name:return FrontText::SortName;case LanRoomSort::Players:return FrontText::SortPlayers;case LanRoomSort::Mode:return FrontText::SortMode;case LanRoomSort::Map:return FrontText::SortMap;default:return FrontText::SortPing;}}
std::string room_details(const FrontendRoom& r){
    const auto now=lan_clock_ms();
    std::string out=native_name_input(r.name)+"\n"+tr(r.details_known?room_mode(r.mode):FrontText::StatusUnknown)+" | "+(r.details_known?(r.map_name.empty()?r.map:r.map_name):"?");
    out+="\n"+frontend_room_ping(r,now)+" | "+tr(lan_room_has_human_slot(r)?frontend_room_state(r,now):FrontText::Full);
    if(!lan_room_fresh(r,now))return out+"\n"+tr(FrontText::RefreshHelp);
    if(r.details_known){
        out+="\n"+std::string(tr(FrontText::Players))+": "+std::to_string(r.humans)+" | "+tr(FrontText::Bots)+": "+std::to_string(r.bots);
        out+="\n"+std::string(tr(FrontText::Spectators))+": "+std::to_string(r.spectators);
    }
    return out;
}
std::string room_rules(const FrontendRoom& r){
    if(!r.details_known)return native_name_input(r.name)+"\n"+tr(FrontText::StatusUnknown);
    return native_name_input(r.name)+"\n"+tr(FrontText::Enemies)+": "+frontend_factor_value(r.enemies,native_menu_language())+
        "\n"+tr(FrontText::AmmoCapacity)+": "+frontend_ammo_capacity_value(r.ammo_capacity,native_menu_language())+
        "\n"+tr(FrontText::AmmoPickups)+": "+frontend_factor_value(r.pickups,native_menu_language());
}
bool active(){return services.active && services.active();}
bool host(){return services.host && services.host();}
bool name_only_options(){return options_return==Page::Join || options_return==Page::InviteJoin || (active() && !host());}
void text(PPCContext& c,unsigned id,const std::string& value,unsigned index){
    if(index>=labels.size() || labels[index]==value)return;
    const auto node=item(c,id);if(!node)return;auto found=storage.find(gui);if(found==storage.end())return;
    const auto scratch=found->second.memory+1024+index*320;const auto shortened=value.substr(0,300);
    for(unsigned i=0;i<=shortened.size();++i)mem_write8(scratch+i,i<shortened.size()?uint8_t(shortened[i]):0);
    const auto table=mem_read32(node+0x5c);call(c,mem_read32(table+0x74),node+int16_t(mem_read16(table+0x70)),scratch);labels[index]=value;
}
void bind(PPCContext& c,unsigned id,unsigned event){call(c,0x800F6C14,gui,gui,id,event,0x800FF15C);}
void prepare_pause(PPCContext& c){
    const auto found=storage.find(gui);if(found==storage.end() || item(c,group+7))return;
    const auto memory=found->second.memory;
    if(const auto parent=item(c,0x129)){
        const auto d=mem_read32(parent+0x1c),children=mem_read32(d+0x44);const auto number=mem_read16(d+6);
        if(number==10){for(unsigned i=0;i<number;++i)mem_write32(memory+960+i*4,mem_read32(children+i*4));mem_write32(memory+960+number*4,group+7);mem_write32(memory+960+(number+1)*4,group+8);mem_write32(d+0x44,memory+960);mem_write16(d+6,number+2);}
        for(unsigned i=7;i<9;++i){call(c,0x800F5940,registry.memory+i*96,gui,parent);bind(c,group+i,22);}
    }
 }
bool prepare(PPCContext& c,bool with_root=true){
    if(storage.contains(gui)){
        // Native close removes the completed root from the active list, while
        // this GUI's text allocation and event registrations remain valid.
        if(with_root && !item(c,group)){
            online_setup_native::forget(gui);
            call(c,0x800F5940,registry.memory,gui,0);
            if(in_pause)prepare_pause(c);
            for(unsigned i=0;i<4;++i){bind(c,first+i,22);bind(c,first+i,12);}bind(c,group,23);invalidate_labels();
        }
        return true;
    }
    const auto heap=mem_read32(c.r[13]-0x6de0);
    if(!registry.memory){
        const auto definitions=mem_read32(c.r[13]-0x542c),count=mem_read32(c.r[13]-0x5428);
        if(count!=group)return false;
        const auto memory=call(c,0x80116084,heap,8192);if(!memory)return false;
        registry={heap,memory,definitions,count};
        const std::array<unsigned,9> sources{0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAE,0x12C,0x12C};
        for(unsigned i=0;i<sources.size();++i){
            const auto original=mem_read32(definitions+sources[i]*4),copy=memory+i*96;
            for(unsigned k=0;k<88;++k)mem_write8(copy+k,mem_read8(original+k));
            mem_write16(copy+2,group+i);mem_write16(copy+4,i>=7?0x129:i?group:0xffff);mem_write16(copy+8,0);mem_write32(copy+0x48,0);
            if(!i){mem_write16(copy+6,6);mem_write32(copy+0x44,memory+928);for(unsigned k=0;k<6;++k)mem_write32(memory+928+k*4,group+k+1);}
            if(i>=7)mem_write32(copy+0x30,f2u(90.f+(i-7)*30.f));
        }
        for(unsigned i=0;i<count;++i)mem_write32(memory+4096+i*4,mem_read32(definitions+i*4));
        for(unsigned i=0;i<sources.size();++i)mem_write32(memory+4096+(group+i)*4,memory+i*96);
        mem_write32(c.r[13]-0x542c,memory+4096);mem_write32(c.r[13]-0x5428,group+sources.size());
    }
    const auto memory=call(c,0x80116084,heap,4096);if(!memory)return false;
    storage[gui]={heap,memory};invalidate_labels();
    if(with_root){
        call(c,0x800F5940,registry.memory,gui,0);
        if(in_pause)prepare_pause(c);
        for(unsigned i=0;i<4;++i){bind(c,first+i,22);bind(c,first+i,12);}bind(c,group,23);
    }
    ssne::log_message("[native-network-menu] original widgets cloned gui="+std::to_string(gui));return true;
}
// These are ordinary native children with the existing pause-row font/style.
// Their descriptors live in this GUI's existing saved text allocation; native
// parent destruction owns the linked children, with no extra root or camera.
void footer_geometry(PPCContext& c,uint32_t node,float x,float y,float scale){
    using main_exit_native::readable;
    const auto desc=mem_read32(node+0x1c),params=mem_read32(node+0x14);
    if(!readable(desc,0x58) || !readable(params,4))return;
    if(u2f(mem_read32(desc+0x2c))!=x || u2f(mem_read32(desc+0x30))!=y){
        mem_write32(desc+0x2c,f2u(x));mem_write32(desc+0x30,f2u(y));
        if(const auto position=mem_read32(params);readable(position,16))for(unsigned i=0;i<4;++i)mem_write32(position+i*4,f2u(i%2?y:x));
    }
    if(!(scale>0))return;
    mem_write32(desc+0x54,f2u(scale));
    const auto helper=mem_read32(node+0xb8);if(!readable(helper,0x48))return;
    const auto fields=mem_read32(helper);if(!readable(fields,8) || mem_read16(fields)>1)return;
    const auto actual=u2f(mem_read32(fields+4));
    if(actual==scale || !std::isfinite(actual) || actual<=0)return;
    const auto ascent=u2f(mem_read32(helper+0x18)),height=u2f(mem_read32(helper+0x1c));
    if(!std::isfinite(ascent) || !std::isfinite(height))return;
    // Same native linear font metrics as the retained lobby captions. Reapply
    // glyph widths when a retained GUI changes between host/client footers.
    mem_write32(fields+4,f2u(scale));mem_write32(helper+0x18,f2u(ascent*scale/actual));mem_write32(helper+0x1c,f2u(height*scale/actual));
    const auto text=mem_read32(helper+8),vt=mem_read32(node+0x5c);
    if(readable(text,1))call(c,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),text);
}
void mouse_footer(PPCContext& c,uint32_t owner,unsigned root,unsigned id,unsigned offset,float x,float y,FrontText label,bool shown,bool enabled=true,float scale=.6f,float maximum=0.f){
    const auto saved_gui=gui;const bool saved_pause=in_pause;gui=owner;in_pause=false;
    // Children need descriptors/text storage, never an unopened custom root.
    // A factory-created 148 without Open/Close remains linked and blocks loads.
    if(!prepare(c,false)){gui=saved_gui;in_pause=saved_pause;return;}
    const auto found=storage.find(gui);const auto parent=item(c,root);
    if(found!=storage.end() && parent){
        if(offset>=480 && !found->second.extra)found->second.extra=call(c,0x80116084,found->second.heap,8192);
        if(offset>=480 && !found->second.extra){gui=saved_gui;in_pause=saved_pause;return;}
        const auto desc=offset>=480?found->second.extra+offset-480:found->second.memory+offset;auto node=item(c,id);const bool created=!node;
        if(!node){
            for(unsigned k=0;k<88;++k)mem_write8(desc+k,mem_read8(registry.memory+(id==survival_button?2:7)*96+k));
            mem_write16(desc+2,id);mem_write16(desc+4,root);mem_write16(desc+6,0);mem_write16(desc+8,0);mem_write32(desc+0x44,0);mem_write32(desc+0x48,0);
            // This child receives raw localized text below, never the cloned
            // pause row's original TDB caption during lazy initialization.
            mem_write32(desc+0x4c,0);
            mem_write32(desc+0x2c,f2u(x));mem_write32(desc+0x30,f2u(y));if(id!=survival_button)mem_write32(desc+0x54,f2u(scale));
            node=call(c,0x800F5940,desc,gui,parent);
            if(node){bind(c,id,22);if(id==options_button || id==survival_button || id==custom_levels_button)bind(c,id,12);}
        }
        if(node){
            // The original factory copies its descriptor into the widget.
            // Update that live copy, including host-only controls on clients.
            const auto live_desc=mem_read32(node+0x1c);
            const bool was_hidden=(mem_read32(live_desc+0xc)&0x30u)!=0;
            mem_write32(live_desc+0xc,(mem_read32(live_desc+0xc)&~0x30u)|(shown?(enabled?0:0x10u):0x30u));
            if(shown){
                // Open can create the Type1 text helper and apply its original
                // caption. Set our dynamic caption only after that step.
                if(created || was_hidden)call(c,0x800FA3CC,node);
                const std::string value=tr(label);const auto scratch=offset>=480?found->second.extra+1024+offset-480:found->second.memory+3584+offset;
                bool changed=created || was_hidden;for(unsigned i=0;i<=value.size() && i<90;++i)changed|=mem_read8(scratch+i)!=(i<value.size()?uint8_t(value[i]):0);
                const auto retained=u2f(mem_read32(live_desc+0x54));
                const float fitted=maximum>0 && !changed && retained>0 && retained<=scale?retained:scale;
                footer_geometry(c,node,x,y,id==survival_button?0.f:fitted);
                if(changed){for(unsigned i=0;i<=value.size() && i<90;++i)mem_write8(scratch+i,i<value.size()?uint8_t(value[i]):0);const auto vt=mem_read32(node+0x5c);call(c,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),scratch);}
                if(maximum>0)if(const auto helper=mem_read32(node+0xb8);main_exit_native::readable(helper,0x18)){
                    const auto width=u2f(mem_read32(helper+0x14));
                    if(std::isfinite(width) && width>maximum)footer_geometry(c,node,x,y,std::max(.25f,fitted*maximum/width));
                }
                const auto current=mem_read32(node+0x1c);mem_write32(current+0xc,(mem_read32(current+0xc)&~0x30u)|(enabled?0:0x10u));
            }
        }
    }
    gui=saved_gui;in_pause=saved_pause;
}

void survival_details(PPCContext& c,uint32_t owner){
    const auto saved_gui=gui;gui=owner;
    if(active() && host() && native_root_id(gui)==0xA7){
        // Reuse the original Deathmatch hover callback and texture bank.
        // Event 12 changes the preview only; event 22 commits the mode.
        if(const auto deathmatch=item(c,0xAA))call(c,0x800FB130,deathmatch,12);
        labels[6]="\1";text(c,0xAE,tr(FrontText::SurvivalHelp),6);
    }
    gui=saved_gui;
}
void prepare_survival_setup(PPCContext& c,uint32_t owner,bool enabled){
    const auto saved_gui=gui;gui=owner;
    if(item(c,survival_setup_native::root_id) && prepare(c,false)){
        auto& saved=storage.at(gui);if(!saved.extra)saved.extra=call(c,0x80116084,saved.heap,8192);
        if(saved.extra){
            if(enabled)mem_write32(c.r[13]-0x69d4,2);
            survival_setup_native::prepare(c,gui,saved.extra+3328,enabled,tr(FrontText::Survival));
        }
    }
    gui=saved_gui;
}

void prepare_mouse_footers(PPCContext& c){
    if(gui)prepare_survival_setup(c,gui,active() && host() && native_lobby_survival() && native_root_id(gui)==0x64);
    if(gui && native_root_id(gui)==0x3d && !visible && prepare(c,false)){
        auto& saved=storage.at(gui);if(!saved.extra)saved.extra=call(c,0x80116084,saved.heap,8192);
        if(saved.extra){
            main_exit_native::ensure(c,gui,saved.extra+2048,saved.extra+2304,saved.extra+2176,tr(FrontText::ExitGame));
            main_online_native::ensure(c,gui,saved.extra+2944,tr(FrontText::Online));
        }
    }
    if(gui && !active() && !visible && native_root_id(gui)==0xaf){
        if(prepare(c,false)){
            auto& saved=storage.at(gui);if(!saved.extra)saved.extra=call(c,0x80116084,saved.heap,8192);
            if(saved.extra)profile_skin_native::prepare(c,gui,saved.extra+2560);
        }
    }
    if(const auto custom_gui=custom_level_menu_gui();custom_gui && native_root_id(custom_gui)==0x4b){
        const bool available=custom_level_menu_available(c),has_levels=custom_level_menu_has_user_levels(c);
        mouse_footer(c,custom_gui,0x4b,custom_levels_button,576,170.f,190.f,custom_level_menu_active()?FrontText::OriginalLevels:FrontText::CustomLevels,available,has_levels);
        // A separate disabled native caption never replaces the selected map
        // title/category. The PS2 carousel remains usable with no user maps.
        mouse_footer(c,custom_gui,0x4b,no_custom_levels_label,960,170.f,165.f,FrontText::NoCustomLevels,available&&!has_levels,false,.34f);
    }
    if(!active())return;
    if(host() && gui && native_root_id(gui)==0xA7){
        const auto base=item(c,0xA9),last=item(c,0xAC);
        if(base && last){const auto b=mem_read32(base+0x1c),l=mem_read32(last+0x1c);mouse_footer(c,gui,0xA7,survival_button,480,u2f(mem_read32(b+0x2c)),u2f(mem_read32(l+0x30))+30.f,FrontText::Survival,true);}
    }
    if(ssne::native_results_presenting()){
        // Results retain only their original Continue/Back controls. Retire a
        // retained old button without creating any custom root in this GUI.
        const auto owner=ssne::native_results_menu();if(owner)if(const auto node=call(c,0x800F6030,owner,result_connection)){
            const auto desc=mem_read32(node+0x1c);mem_write32(desc+0xc,mem_read32(desc+0xc)|0x30u);
        }
    }
    else if(const auto owner=native_lobby_mouse_main()){
        const bool hosting=host();const auto wide=ssne::widescreen();
        const float half=320.f*(std::isfinite(wide)?std::clamp(wide,1.f,4.f):1.f);
        const float outer=std::max(248.f,half-106.666667f),step=outer*.5f;
        // Use the actual widescreen space. The long native Sam-font actions
        // need a smaller authored size in4:3, including their focus animation;
        // their rendered ink extends beyond CcText's advance-width metric.
        const float scale=hosting?.46f:.6f,long_scale=.32f+.14f*std::clamp((half-320.f)/106.666667f,0.f,1.f);
        mouse_footer(c,owner,0x9A,lobby_start,96,hosting?-outer:-234.f,210.f,FrontText::Start,hosting,native_lobby_can_start(),scale);
        mouse_footer(c,owner,0x9A,lobby_map,672,hosting?-step:-78.f,210.f,FrontText::Map,hosting,true,scale);
        if(hosting || call(c,0x800F6030,owner,lobby_invite))mouse_footer(c,owner,0x9A,lobby_invite,864,0.f,210.f,lobby_invitation_label(),hosting,true,long_scale);
        mouse_footer(c,owner,0x9A,lobby_connection,192,hosting?step:78.f,210.f,FrontText::Connection,true,true,hosting?long_scale:.6f);
        mouse_footer(c,owner,0x9A,lobby_bots,288,hosting?outer:234.f,210.f,FrontText::Options,hosting,true,long_scale);
        mouse_footer(c,owner,0x9A,lobby_ready_button,768,140.f,-126.f,native_lobby_ready()?FrontText::LobbyReadyYes:FrontText::LobbyReadyNo,true,native_lobby_can_ready(),.48f);
        const auto saved_gui=gui;gui=owner;if(const auto old=item(c,0x7ffe)){const auto d=mem_read32(old+0x1c);mem_write32(d+0xc,mem_read32(d+0xc)|0x20u);}gui=saved_gui;
    }
}
void hide_online_widgets(PPCContext& c){
    const auto found=storage.find(gui);if(found==storage.end() || !found->second.extra)return;
    const auto root=item(c,group);if(!root)return;
    online_setup_native::prepare(c,gui,root,found->second.extra+4096,{}, {},false);
    if(const auto node=online_setup_native::home_icon(c,gui,root,found->second.extra+4096,false))
        if(main_exit_native::readable(node,0xb8))bind_native_menu_image(mem_read32(node+0xb4),{});
}
void show(PPCContext& c,Page next,unsigned previous_panel=group){
    if(!prepare(c) || !item(c,group))return;
    hide_online_widgets(c);
    ++mouse_epoch;
    // A result GUI already owns its local player's native open-mask bit.
    // Rebinding it to port zero would make CloseMenu clear the wrong bit.
    if(!result_overlay)call(c,0x800F56E0,gui,in_pause?unsigned(std::max(0,ssne::netplay_local_player())):0u);
    changing=true;
    if(result_overlay){if(result_group_definition)mem_write32(result_group_definition+0xc,(mem_read32(result_group_definition+0xc)&~0x30u)|0x40u);}
    else call(c,0x800F6178,gui,previous_panel);
    page=next;setup_restore_skin=next==Page::OnlineSetup;pending_room.reset();invalidate_labels();visible=true;editing=Edit::None;
    focus=next==Page::Connecting?3:0;call(c,0x800F60A0,gui,group,0);call(c,0x800FA5BC,item(c,first+focus));changing=false;wait_release=true;
    ssne::log_message("[native-network-menu] page="+std::to_string(unsigned(page))+" language="+std::to_string(native_menu_language()));
}
void close(PPCContext& c,unsigned target){
    hide_online_widgets(c);
    // ClosePanel may recycle this tree at exactly the same guest address
    // on the next visit, while the enclosing CcGUI and its scratch survive.
    online_setup_native::forget(gui);
    ++mouse_epoch;
    if(result_overlay && !target){retire_result_overlay(c,true);return;}
    if(!target && active() && (page==Page::Bots || page==Page::Connection || page==Page::SessionOptions))handoff_release=true;
    changing=true;call(c,0x800F6178,gui,group);editing=Edit::None;visible=false;page=Page::Closed;
    // Opening main can restore the previous Multiplayer child. Suppress our
    // entry hook until native Back has actually unwound to the main root.
    if(target==0x3d){return_main_requested=true;next_back={};}
    if(target)call(c,0x800F60A0,gui,target,0);changing=false;
}
void open_online(PPCContext& c,uint32_t owner,Page next=Page::Home){
    if(active() || changing || return_main_requested)return;
    gui=owner;in_pause=false;local_mode=false;setup_seen=false;action=99;main_idle_frames=0;
    // Prepare the destination while the main menu still owns focus. Closing
    // the source and reopening the destination is one native panel transition.
    show(c,next,0x3d);
}
bool setup_event(PPCContext& c,uint32_t owner,unsigned id,unsigned event){
    if(!visible || page!=Page::OnlineSetup || owner!=gui)return false;
    auto found=std::find(online_setup_native::editable_ids.begin(),online_setup_native::editable_ids.end(),id);
    if(found==online_setup_native::editable_ids.end())return false;
    if(event==12){focus=unsigned(found-online_setup_native::editable_ids.begin());return true;}
    if(event==14){
        if(id==online_setup_native::character_id){
            if(online_setup_native::changed_model(c,gui)){const auto node=item(c,id);options.skin=mem_read32(node+0xd0);save_options();}
        }
        return true;
    }
    if(event!=22)return false;
    if(editing!=Edit::None){accept_edit(c);if(editing!=Edit::None)return true;}
    focus=unsigned(found-online_setup_native::editable_ids.begin());
    if(id==online_setup_native::name_id)edit(Edit::Name);
    else if(id==online_setup_native::host_name_id)edit(Edit::HostName);
    else if(id==online_setup_native::details_id)hotkey=9;
    else if(const auto node=item(c,id)){const auto d=mem_read32(node+0x1c);call(c,0x800FB130,node,(mem_read32(d+0xc)&0x2000u?20u:18u)+1);}
    return true;
}
void select(PPCContext& c){
    const unsigned id=mem_read16(c.r[4]+0x18),event=mem_read32(c.r[4]+0x14);
    if(id==survival_setup_native::difficulty_enum && native_root_id(c.r[3])==0x64){if(event==14 && active() && host() && native_lobby_survival())survival_setup_native::changed(c,c.r[3]);return;}
    if(native_root_id(c.r[3])==group && setup_event(c,c.r[3],id,event))return;
    if(id==main_online_native::online_id && native_root_id(c.r[3])==0x3d){if(event==22)open_online(c,c.r[3]);return;}
    if(id==profile_skin_selector && native_root_id(c.r[3])==0xaf){
        if(!active() && (event==14 || event==22))profile_skin_native::changed(c,c.r[3]);
        if(!active() && (event==22 || event==24 || event==25 || event==26))call(c,0x800F6E3C,c.r[3],0xb1,event);
        return;
    }
    if(id==main_exit_native::exit_id && native_root_id(c.r[3])==0x3d){if(event==22 && services.request_quit)services.request_quit();return;}
    if(id==lobby_invite && native_root_id(c.r[3])==0x9a){if(event==22 && active() && host() && native_lobby_visible() && c.r[3]==native_lobby_mouse_main())hotkey=8;return;}
    if(id==lobby_start || id==lobby_map || id==lobby_ready_button){if(event==22 && native_root_id(c.r[3])==0x9a)hotkey=id==lobby_start?7:id==lobby_map?5:6;return;}
    if(id==custom_levels_button){if(event==22)custom_level_menu_toggle(c,c.r[3]);return;}
    if(id==survival_button){if(event==12)survival_details(c,c.r[3]);if(event==22)hotkey=4;return;}
    if(id==group){if(event==23 && !changing && visible){visible=false;editing=Edit::None;action=98;}return;}
    if(id==group+7 || id==group+8 || id==result_connection || id==lobby_connection || id==lobby_bots || id==options_button){if(!footer_root(id,native_root_id(c.r[3])))return;if(event==12 && id==options_button)focus=4;if(event==22)hotkey=id==options_button?3:(id==group+8 || id==lobby_bots)?2:1;return;}
    if(id>=first && id<first+4){if(event==12)focus=id-first;if(event==22 && editing==Edit::None && !wait_release)choose_frontend_action(id-first);return;}
    // Native Back restores this menu without calling open(A7). Reset
    // the custom mode on every real original-mode selection instead.
    if(event==22 && id>=0xA9 && id<=0xAC && !survival_selection_dispatch){
        set_native_lobby_survival(false);
        ssne::log_message("[native-mode-selection] mode="+std::to_string(id-0xA8)+" survival=0");
    }
    ssne_call_original(c,0x800FF15C,select_original);
}
void open(PPCContext& c){
    const auto root=c.r[3],id=c.r[4];
    FrontendStartupReadiness::Open opening(startup_readiness,root,id==0x3d);
    if(id==0x3d){in_pause=false;local_mode=false;gui=root;}
    ssne_call_original(c,0x800F60A0,open_original);
    if(id==0x64)prepare_survival_setup(c,root,active() && host() && native_lobby_survival());
    if(id==0xA7 && !active()){gui=root;local_mode=true;setup_seen=true;}
    else if(id==0xA7 && active()){setup_seen=true;set_native_lobby_survival(false);}
    opening.completed();
}
void pause_open(PPCContext& c){
    const auto root=c.r[3];
    ssne::prepare_network_pause_panel(c);
    if(c.r[4]==0x129 && active() && mem_read32(0x802DB7E8)==4){
        const auto slot=ssne::netplay_local_player();const auto actor=slot>=0 && slot<8?mem_read32(ssne::native_party_registry()+slot*4):0;
        if(actor>=0x80000000u && actor<ssne::guest_ram_end()-0x790u && mem_read32(actor+0x71c)==root){
            gui=root;in_pause=true;prepare(c,false);
        }
    }
    ssne_call_original(c,0x800F9978,pause_original);
    if(in_pause && gui==root && storage.contains(root)){
        // Pause resources are created lazily by the original open call.
        prepare_pause(c);labels[6]=labels[7]="\1";text(c,group+7,tr(FrontText::Connection),6);text(c,group+8,host()?tr(FrontText::Options):"",7);
        if(const auto node=item(c,group+7))call(c,0x800FA3CC,node);
        if(const auto node=item(c,group+8)){if(host())call(c,0x800FA3CC,node);else{const auto d=mem_read32(node+0x1c);mem_write32(d+0xc,mem_read32(d+0xc)|0x30u);}}
    }
}
void destroy(PPCContext& c){
    const auto root=c.r[3];auto found=storage.find(root);const auto owned=found==storage.end()?Storage{}:found->second;
    ssne::forget_network_pause_panel(c,root);
    profile_skin_native::forget(root);online_setup_native::forget(root);
    startup_readiness.destroyed(root);survival_setup_native::forget(root);
    custom_level_menu_forget_gui(c,root);
    if(transfer_pause_gui==root){transfer_pause_gui=0;transfer_pause_slot=-1;}
    if(transfer_lobby_gui.load()==root)transfer_lobby_gui=0;
    if(gui==root){gui=0;in_pause=false;result_overlay=false;setup_seen=false;main_idle_frames=0;visible=false;editing=Edit::None;page=Page::Closed;invalidate_labels();}
    ssne_call_original(c,0x800F55B4,destroy_original);
    if(owned.extra)call(c,0x801161EC,owned.heap,owned.extra);
    if(owned.memory){storage.erase(root);call(c,0x801161EC,owned.heap,owned.memory);}
    if(storage.empty() && registry.memory){mem_write32(c.r[13]-0x542c,registry.definitions);mem_write32(c.r[13]-0x5428,registry.count);const auto old=registry;registry={};call(c,0x801161EC,old.heap,old.memory);}
}
void language(PPCContext& c){
    const auto preferred=startup_language_override.load(std::memory_order_relaxed);
    // Preserve the original EN/DE/FR preload loop at return address 80161E64.
    // Only override the OS restore and the later saved-setting application;
    // their original callers perform the corresponding resource reload.
    if(preferred>=0 && (c.lr==0x80161E94u || c.lr==0x8016406Cu))c.r[3]=uint32_t(preferred);
    if(getenv("SSNE_TEST_HIDDEN"))if(const auto value=getenv("SSNE_NATIVE_TEST_LANGUAGE")){const auto n=std::strtol(value,nullptr,10);if(n>=0 && n<3)c.r[3]=uint32_t(n);}
    ssne_call_original(c,0x801597BC,language_original);
}
PPCFunc profile_body_tick_original=nullptr;
void profile_body_tick(PPCContext& c){
    // The PAL profile screen owns this callback exclusively.
    if(!active())profile_skin_native::sync_body(c,c.r[3]);
    ssne_call_original(c,0x80100a18,profile_body_tick_original);
}
void gui_update(PPCContext& c){const auto owner=c.r[3];ssne_call_original(c,0x800F86C8,gui_update_original);online_setup_native::after_gui_update(c,owner);}
void scene(PPCContext& c){const auto results=ssne::native_results();if(!engine_scope && active() && c.r[3]==3 && mem_read32(0x802DB7E8)==4 && paused && (!results || !results->active))action=97;ssne_call_original(c,0x80155568,scene_original);}
void resume(PPCContext& c){if(ssne::native_results_presenting() && gui==ssne::native_results_menu()){retire_result_overlay(c,true);return;}if(in_pause){call(c,0x800F9978,gui,0x129,0x134,c.r[13]-0x53a0);}else resume_native_lobby(c);}
void release_transfer_pause(PPCContext& c){
    if(!transfer_pause_gui)return;
    const auto owner=transfer_pause_gui;const auto slot=transfer_pause_slot;transfer_pause_gui=0;transfer_pause_slot=-1;
    const auto registry=ssne::native_party_registry();
    const auto actor=slot>=0 && slot<8 && registry?mem_read32(registry+unsigned(slot)*4):0u;
    if(owner!=gui || actor<0x80000000u || actor>ssne::guest_ram_end()-0x790u || mem_read32(actor+0x71c)!=owner)return;
    TransferPartyScope owner_scope(slot);
    // Local GUI cleanup preserves authority simulation and every other GUI's
    // pause bit. If the user had paused, return to that original pause panel.
    if(transfer_pause_was_open)call(c,0x800F9978,owner,0x129,0x134,c.r[13]-0x53a0);
    else{call(c,0x800F643C,owner);mem_write32(c.r[13]-0x765c,mem_read32(c.r[13]-0x765c)&~(1u<<slot));in_pause=false;}
}
void show_custom_map_progress(PPCContext& c){
    if(host() || !custom_map_busy() || page==Page::Connecting || session_error_pending)return;
    const auto scene=mem_read32(0x802DB7E8);
    if(scene==4){
        // The frontend GUI may already have been freed when the host changes
        // maps. Obtain the current actor's native pause GUI afresh instead.
        const auto slot=ssne::netplay_local_player();const auto registry=ssne::native_party_registry();
        const auto actor=slot>=0 && slot<8 && registry?mem_read32(registry+unsigned(slot)*4):0u;
        if(actor<0x80000000u || actor>ssne::guest_ram_end()-0x790u)return;
        const auto owner=mem_read32(actor+0x71c);if(owner<0x80000000u || owner>ssne::guest_ram_end()-0x104u)return;
        gui=owner;in_pause=true;transfer_pause_gui=owner;transfer_pause_slot=slot;
        TransferPartyScope owner_scope(slot);
        const auto mask=mem_read32(c.r[13]-0x765c);transfer_pause_was_open=(mask&(1u<<slot))!=0;
        if(int16_t(mem_read16(owner+0x84))!=slot)call(c,0x800F56E0,owner,unsigned(slot));
        // Skip OpenMenu's global pause/audio head, as the existing native
        // spectator menu does, while keeping its local renderer lifecycle.
        mem_write32(c.r[13]-0x765c,mask|(1u<<slot));
        call(c,0x800F9978,owner,0x129,0x134,c.r[13]-0x53a0);call(c,0x800F6178,owner,0x129);
    }else if(scene==3 && gui){
        // Initial joins check the selected package before their first lobby.
        // Suspend only an open lobby, so Ready has a matching owner to resume.
        if(native_lobby_visible()){transfer_lobby_gui=gui;suspend_native_lobby(c);}
    }
    else return;
    show(c,Page::Connecting);
}
Page connection_origin(){
    if(last_host)return Page::Home;
    if(retry_room)return Page::Rooms;
    return last_invitation?Page::InviteJoin:Page::Direct;
}
void prepare_host_endpoint(){
    if(options.use_password)host_endpoint.pin=options.pin;
    else if(!host_pin_initialized){
        host_endpoint.pin=std::to_string(100000+uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())%900000);
        host_pin_initialized=true;
    }
}
void cancel_connection(PPCContext& c){
    waiting_for_host_world=false;
    transfer_lobby_gui=0;
    custom_map_cancel=true;{std::lock_guard lock(custom_map_mutex);custom_map_progress={};custom_map_phase=NativeCustomMapPhase::None;}
    const auto origin=connection_origin();error="cancelled";notify_error=false;close(c,0);release_transfer_pause(c);
    if(services.stop)services.stop();
    if(gui && mem_read32(0x802DB7E8)==3){return_main_requested=false;show(c,origin);}
}
void edit(Edit field){edit_error=FrontText::Count;const auto& address=page==Page::Host?host_endpoint:endpoint;editing=field;edit_buffer=field==Edit::Invitation?invitation_code:field==Edit::Name?options.name:field==Edit::HostName?options.host_name:field==Edit::Address?address.address:field==Edit::Pin?address.pin:std::to_string(address.port);cursor=0;replace_all=true;}
void accept_edit(PPCContext& c){
    const auto field=editing.load();if(field==Edit::Invitation){invitation_code=edit_buffer;std::string why;if(const auto decoded=invite::decode_invitation(invitation_code,why))invitation_code=invite::encode_invitation(*decoded);}
    if(name_field(field)){auto changed=options;if(field==Edit::Name)changed.name=edit_buffer;else changed.host_name=edit_buffer;if(valid_multiplayer_options(changed)){options=std::move(changed);save_options();}else return;}
    auto& address=page==Page::Host?host_endpoint:endpoint;
    if(field==Edit::Address)address.address=edit_buffer;
    if(field==Edit::Pin){
        if(page==Page::Host){
            if(edit_buffer.size()!=6 || edit_buffer.find_first_not_of("0123456789")!=std::string::npos){edit_error=FrontText::InvalidPin;return;}
            options.pin=edit_buffer;host_pin_initialized=true;save_options();
        }
        address.pin=edit_buffer;
    }
    if(field==Edit::Port){
        unsigned port=0;for(char digit:edit_buffer)port=port*10+unsigned(digit-'0');
        if(!port || port>65535){edit_error=FrontText::InvalidPort;return;}
        address.port=port;if(page==Page::Host)save_options();
    }
    editing=Edit::None;wait_release=true;
    if((page==Page::Direct && field==Edit::Pin) || (page==Page::InviteJoin && field==Edit::Invitation)){
        focus=page==Page::Direct?3:1;call(c,0x800FA5BC,item(c,first+focus));
    }
}
std::string field(Edit type,const std::string& value){if(editing!=type)return value;auto out=edit_buffer;out.insert(std::min<size_t>(cursor,out.size()),"_");return out;}
std::string invitation_display(std::string value){for(size_t at=19;at<value.size();at+=20)value.insert(at,"\n");return value;}
NativeInvitationStatus invitation_status(){return services.invitation?services.invitation():NativeInvitationStatus{};}
FrontText lobby_invitation_label(){
    const auto value=invitation_status();const auto now=std::chrono::steady_clock::now();
    if(value.copy_sequence!=lobby_copy_sequence || value.copy_state!=lobby_copy_state){
        lobby_copy_sequence=value.copy_sequence;lobby_copy_state=value.copy_state;
        lobby_copy_feedback_until=now+std::chrono::seconds(3);
    }
    if(value.copy_state==NativeInvitationCopyState::Waiting)return FrontText::InviteCopyWaiting;
    if(now<lobby_copy_feedback_until){
        if(value.copy_state==NativeInvitationCopyState::Copied)return FrontText::InvitationCopied;
        if(value.copy_state==NativeInvitationCopyState::Failed)return FrontText::InviteCopyFailed;
    }
    return FrontText::CopyInvitation;
}
bool invitation_host_pending(){return services.invitation_host_pending && services.invitation_host_pending();}
void back_from_invitation_status(PPCContext& c){
    if(!invitation_host_pending()){show(c,Page::Connection);return;}
    // Stopping this not-yet-configured host requests Main as a normal session
    // stop would. Preserve any earlier request, but retire this stop's request
    // so Back returns to invitation creation without leaving a live server.
    const bool returning=return_main_requested.load();const auto back=next_back;
    if(services.stop)services.stop();
    return_main_requested=returning;next_back=back;setup_seen=false;main_idle_frames=0;
    show(c,Page::InviteHost);
}
void configure_invitation_host(PPCContext& c){
    if(!invitation_host_pending() || !services.continue_invitation_host)return;
    const bool returning=return_main_requested.load();const auto back=next_back;
    error=services.continue_invitation_host();
    if(!error.empty()){
        ssne::log_message("[native-network-menu] invitation setup failed: "+error);
        // A transport failure remains on its status page. If native setup
        // itself failed and stopped the session, keep the normal retry page.
        if(!active()){return_main_requested=returning;next_back=back;show(c,Page::Error);}
        return;
    }
    setup_seen=false;main_idle_frames=0;handoff_release=true;close(c,0);
}
FrontText failure(){if(error=="invalid-invite")return FrontText::InviteInvalid;if(error=="host failed")return FrontText::HostFailed;if(error.starts_with("invite:"))return FrontText::InviteFailed;if(error.starts_with("custom-level:"))return FrontText::CustomLevelUnavailable;if(error.starts_with("native-setup:"))return FrontText::NativeSetupFailed;if(error=="lan-expired")return FrontText::LanExpired;if(error=="invalid-address")return FrontText::InvalidAddress;if(error=="invalid-pin")return FrontText::InvalidPin;if(error=="invalid-port")return FrontText::InvalidPort;if(error=="lost:timeout")return FrontText::ConnectionTimeout;if(error=="lost")return FrontText::Lost;if(error.find("differs")!=std::string::npos || error.find("PIN")!=std::string::npos)return FrontText::Version;if(error.find("full")!=std::string::npos)return FrontText::Full;if(error=="invalid")return FrontText::Invalid;return FrontText::Failed;}
std::shared_ptr<const ssne::CustomCrosshairImage> online_icon_image(){
    // Optional artwork is extracted locally from the user's PS2 disc. The
    // executable/source package contains no original game pixels.
    static const auto image=[]()->std::shared_ptr<const ssne::CustomCrosshairImage>{
        port::CrosshairImage png;std::string why;if(!port::load_crosshair_png("online-icon.png",png,why) || png.width>512 || png.height>512)return {};
        auto out=std::make_shared<ssne::CustomCrosshairImage>();out->width=out->right=png.width;out->height=out->bottom=png.height;out->rgba=std::move(png.rgba);return out;
    }();return image;
}
void online_widgets(PPCContext& c){
    if(!visible || !gui)return;const auto root=item(c,group);const auto found=storage.find(gui);if(!root || found==storage.end())return;
    auto& saved=found->second;if(!saved.extra)saved.extra=call(c,0x80116084,saved.heap,8192);if(!saved.extra)return;
    const bool setup=page==Page::OnlineSetup,home=page==Page::Home;
    const std::array<std::string,3> setup_labels{tr(FrontText::OnlineCharacter),tr(FrontText::OnlineNickname),tr(FrontText::OnlineGameName)};
    const auto name=field(Edit::Name,options.name),host_name=field(Edit::HostName,options.host_name.empty()?(options.name.empty()?setup_profile_name:options.name):options.host_name);

    const auto nodes=online_setup_native::prepare(c,gui,root,saved.extra+4096,setup_labels,{name,host_name,tr(FrontText::ConnectionDetails)},setup);
    if(setup && nodes){
        setup_profile_name=nodes.profile_name;
        if(setup_restore_skin){
            if(options.skin<mem_read32(nodes.character+0xd4)){call(c,0x800F15D0,nodes.character,options.skin);online_setup_native::changed_model(c,gui);}
            setup_restore_skin=false;call(c,0x800FA5BC,nodes.character);focus=0;
        }
    }
    const auto icon=online_setup_native::home_icon(c,gui,root,saved.extra+4096,home);
    if(main_exit_native::readable(icon,0xb8))bind_native_menu_image(mem_read32(icon+0xb4),home?online_icon_image():std::shared_ptr<const ssne::CustomCrosshairImage>{});
    for(unsigned i=0;i<4;++i)if(const auto node=item(c,first+i)){
        const auto original=registry.memory+(2+i)*96;
        online_setup_native::position(node,home?80.f:u2f(mem_read32(original+0x2c)),home?-60.f+40.f*i:u2f(mem_read32(original+0x30)));
    }
    if(home)if(const auto node=item(c,description)){const auto d=mem_read32(node+0x1c);mem_write32(d+0xc,mem_read32(d+0xc)|0x30u);}
    if(!home)if(const auto node=item(c,description)){
        online_setup_native::visibility(c,node,true);const auto original=registry.memory+6*96;
        online_setup_native::position(node,setup?80.f:u2f(mem_read32(original+0x2c)),setup?188.f:u2f(mem_read32(original+0x30)));
        online_setup_native::text_scale(c,node,setup?.32f:u2f(mem_read32(original+0x54)));
    }
}
void update(PPCContext& c){
    std::array<std::string,4> buttons;FrontText heading=FrontText::Online;std::string hint;bool room_focus_relocated=false;
    switch(page){
    case Page::Home:buttons={tr(FrontText::OnlineJoin),tr(FrontText::OnlineHost),tr(FrontText::OnlineSetup),""};break;
    case Page::OnlineSetup:heading=FrontText::OnlineSetup;break;
    case Page::Host:
        heading=FrontText::ConnectionDetails;buttons={
            std::string(tr(FrontText::Pin))+": "+field(Edit::Pin,host_endpoint.pin),
            std::string(tr(FrontText::Port))+": "+field(Edit::Port,std::to_string(host_endpoint.port)),
            tr(options.use_password?FrontText::LanPinOn:FrontText::LanPinOff),tr(FrontText::Back)};
        hint=tr(FrontText::HostConnectionHelp);break;
    case Page::Join:heading=FrontText::Join;buttons={tr(FrontText::EnterCode),tr(FrontText::Search),tr(FrontText::JoinByIP),tr(FrontText::Back)};break;
    case Page::Direct:
        heading=FrontText::JoinByIP;buttons={
            std::string(tr(FrontText::Address))+": "+field(Edit::Address,endpoint.address),
            std::string(tr(FrontText::Pin))+": "+field(Edit::Pin,endpoint.pin),
            std::string(tr(FrontText::Port))+": "+field(Edit::Port,std::to_string(endpoint.port)),tr(FrontText::Join)};
        hint=tr(FrontText::DirectHelp);break;
    case Page::InviteHost:heading=FrontText::InternetGame;buttons={tr(FrontText::CreateInvitation),tr(FrontText::Options),"",tr(FrontText::Back)};hint=tr(FrontText::InvitationHelp);break;
    case Page::InviteJoin:heading=FrontText::InvitationCode;buttons={tr(FrontText::InvitationCode),tr(FrontText::Join),tr(FrontText::PlayerOptions),tr(FrontText::Back)};hint=invitation_display(field(Edit::Invitation,invitation_code))+"\n"+tr(FrontText::JoinInvitationHelp);break;
    case Page::InviteStatus:{
        const auto value=invitation_status();const bool ready=!value.code.empty() && value.phase!=1 && value.phase!=5;
        heading=FrontText::InvitationCode;buttons={ready?tr(FrontText::CopyInvitation):"",tr(FrontText::Options),invitation_host_pending()?tr(FrontText::ConfigureGame):"",tr(FrontText::Back)};
        hint=invitation_display(value.code)+"\n"+tr(value.phase==5?FrontText::InviteFailed:value.phase==1?FrontText::InvitePreparing:value.route==2?FrontText::InviteRelay:value.route==1?FrontText::InviteDirect:FrontText::InviteWaiting);
        if(lobby_invitation_label()==FrontText::InvitationCopied)hint+="\n"+std::string(tr(FrontText::InvitationCopied));break;
    }
    case Page::Connecting:{heading=FrontText::Connection;buttons={tr(FrontText::Connecting),"","",tr(FrontText::Cancel)};
        hint=waiting_for_host_world && active()?tr(FrontText::SynchronizingHost):
            last_invitation?std::string(tr(FrontText::JoinInvitationHelp)):endpoint.address+":"+std::to_string(endpoint.port);
        if(custom_map_busy()){
            const auto transfer=native_frontend_custom_map_progress();const auto phase=transfer.phase;
            buttons[0]=tr(phase==NativeCustomMapPhase::Requesting?FrontText::CustomMapRequesting:phase==NativeCustomMapPhase::Downloading?FrontText::CustomMapDownloading:phase==NativeCustomMapPhase::Verifying?FrontText::CustomMapVerifying:FrontText::CustomMapInstalling);
            auto name=native_name_input(transfer.title);if(name.size()>30)name=name.substr(0,27)+"...";buttons[1]=name;
            if(transfer.total){std::ostringstream bytes;bytes<<std::fixed<<std::setprecision(1)<<double(std::min(transfer.received,transfer.total))/(1024*1024)<<" / "<<double(transfer.total)/(1024*1024)<<" MiB ("<<std::min<uint64_t>(100,transfer.received*100/transfer.total)<<"%)";buttons[2]=bytes.str();}
            hint=tr(FrontText::CustomMapDownloadHelp);
        }break;}
    case Page::Error:
        heading=FrontText::Connection;buttons={tr(FrontText::Retry),
            retry_room?"":tr(last_host?FrontText::ConnectionDetails:last_invitation?FrontText::InvitationCode:FrontText::JoinByIP),
            "",tr(FrontText::Back)};
        hint=tr(failure());break;
    case Page::Bots:{heading=FrontText::Bots;const auto count=services.bot?services.bot(0,skill):0;buttons={std::string(tr(FrontText::AddBot))+" ("+std::to_string(count)+")",std::string(tr(FrontText::Skill))+": "+tr(skill==1?FrontText::Easy:skill==2?FrontText::Normal:FrontText::Hard),tr(FrontText::RemoveBot),tr(FrontText::Back)};hint=tr(FrontText::BotHelp);break;}
    case Page::Connection:heading=FrontText::Connection;buttons={tr(FrontText::Back),host()?tr(FrontText::Options):"",tr(FrontText::Leave),services.spectating && services.set_spectating && services.spectator_available && services.spectator_available() && !(services.spectator_locked && services.spectator_locked()) && mem_read32(0x802DB7E8)==4?tr(services.spectating()?FrontText::PlayAgain:FrontText::Watch):""};hint=services.status?services.status():"";if(services.spectator_locked && services.spectator_locked())hint+="\n"+std::string(tr(FrontText::RoundWait));break;
    case Page::SessionOptions:
        heading=FrontText::Options;
        buttons={std::string(tr(FrontText::Enemies))+": "+frontend_factor_value(options.enemies,native_menu_language()),std::string(tr(FrontText::AmmoCapacity))+": "+frontend_ammo_capacity_value(options.capacity,native_menu_language()),std::string(tr(FrontText::AmmoPickups))+": "+frontend_factor_value(options.pickups,native_menu_language()),tr(FrontText::Bots)};
        hint=tr(FrontText::RulesHelp);break;
    case Page::Options:{
        const bool name_only=name_only_options();heading=name_only?FrontText::PlayerOptions:FrontText::Options;
        buttons[0]=std::string(tr(FrontText::PlayerName))+": "+field(Edit::Name,options.name.empty()?tr(FrontText::ProfileName):options.name);
        if(!name_only){
            buttons[1]=std::string(tr(FrontText::Enemies))+": "+frontend_factor_value(options.enemies,native_menu_language());
            buttons[2]=std::string(tr(FrontText::AmmoCapacity))+": "+frontend_ammo_capacity_value(options.capacity,native_menu_language());
            buttons[3]=std::string(tr(FrontText::AmmoPickups))+": "+frontend_factor_value(options.pickups,native_menu_language());
        }
        hint=tr(options_save_failed?FrontText::SaveFailed:name_only?(active()?FrontText::NameLocked:FrontText::PlayerOptionsHelp):active() && focus==0?FrontText::NameLocked:FrontText::RulesHelp);break;
    }
    case Page::Rooms:{
        heading=FrontText::Search;auto rooms=frontend_rooms();if(room_offset>=rooms.size())room_offset=0;
        // Keep keyboard/controller focus on the same server when an earlier
        // row disappears. An expired selection moves to More/Back, not a peer.
        if(focus<3 && displayed_room_rows[focus]){
            const auto key=lan_room_key(*displayed_room_rows[focus]);
            const auto found=std::find_if(rooms.begin(),rooms.end(),[&](const auto& room){return lan_room_key(room)==key;});
            if(found==rooms.end()){focus=3;room_focus_relocated=true;}
            else{const auto index=unsigned(found-rooms.begin());const auto offset=index/3*3,selected=index%3;
                room_focus_relocated=room_offset!=offset || focus!=selected;room_offset=offset;focus=selected;}
        }
        std::array<std::optional<FrontendRoom>,3> next_rows;
        const auto now=lan_clock_ms();
        for(unsigned i=0;i<3;++i)if(room_offset+i<rooms.size()){
            const auto& room=rooms[room_offset+i];next_rows[i]=room;
            buttons[i]=frontend_room_row(room,native_name_input(room.name),native_menu_language(),now);
        }
        bool changed=false;for(unsigned i=0;i<3;++i)changed|=bool(next_rows[i])!=bool(displayed_room_rows[i]) ||
            (next_rows[i] && displayed_room_rows[i] && lan_room_key(*next_rows[i])!=lan_room_key(*displayed_room_rows[i]));
        if(changed)++mouse_epoch;displayed_room_rows=std::move(next_rows);
        buttons[3]=tr(rooms.size()>3?FrontText::More:FrontText::Back);
        if(!rooms.empty())room_details_selection=rooms[room_offset+(focus<3 && room_offset+focus<rooms.size()?focus:0)];
        hint=rooms.empty()?tr(services.scanning && services.scanning()?FrontText::Searching:FrontText::None):room_details(*room_details_selection);break;
    }
    case Page::BrowserTools:
        heading=FrontText::BrowserOptions;buttons={tr(FrontText::Refresh),std::string(tr(FrontText::Sort))+": "+tr(room_sort_label()),std::string(tr(FrontText::Filter))+": "+tr(room_filter.mode?room_mode(room_filter.mode):FrontText::AllModes),tr(FrontText::Back)};hint=room_details_selection?room_rules(*room_details_selection):tr(FrontText::RoomsHelp);break;
    default:return;
    }
    mouse_footer(c,gui,group,options_button,384,-200.f,160.f,page==Page::Direct?FrontText::Back:page==Page::Rooms?FrontText::BrowserOptions:page==Page::BrowserTools?(room_filter.hide_full?FrontText::AllRooms:FrontText::AvailableOnly):(page==Page::Options || page==Page::OnlineSetup || page==Page::SessionOptions || page==Page::Bots)?FrontText::Back:(active() && !host())?FrontText::PlayerOptions:FrontText::Options,has_options_button());
    const bool first_room_arrived=page==Page::Rooms && labels[1].empty() && !buttons[0].empty();
    const bool room_focus_missing=page==Page::Rooms && focus<4 && buttons[focus].empty();
    const bool connection_focus_missing=page==Page::Connection && focus<4 && buttons[focus].empty();
    if(editing!=Edit::None)hint=edit_error!=FrontText::Count?tr(edit_error):editing==Edit::Invitation?invitation_display(field(Edit::Invitation,invitation_code))+"\n"+tr(FrontText::Typing):tr(name_field(editing)?FrontText::NameTyping:FrontText::Typing);text(c,title,tr(heading),0);for(unsigned i=0;i<4;++i){text(c,first+i,buttons[i],i+1);if(const auto node=item(c,first+i)){const auto d=mem_read32(node+0x1c);const auto flags=mem_read32(d+0xc);mem_write32(d+0xc,(flags&~0x30u)|(buttons[i].empty()?0x30u:((page==Page::Connecting && i!=3) || (page==Page::Options && active() && (i==0 || !host())) || (page==Page::SessionOptions && (!active() || !host())))?0x10u:0u));}}text(c,description,hint,5);online_widgets(c);
    // Native OpenPanel can deliver its deferred default-row focus after our
    // initial show(). Once status rows are disabled, keep its only action on
    // Cancel so keyboard/controller confirmation cannot select a status line.
    if(page==Page::Connecting){focus=3;if(const auto cancel=item(c,first+3);cancel && mem_read32(gui+0x7c)!=cancel)call(c,0x800FA5BC,cancel);}
    // Search results arrive after OpenPanel chose Back from the empty list.
    // Move focus only when a row first becomes available or disappears;
    // metadata refreshes must preserve the user's current native selection.
    if(first_room_arrived || room_focus_missing || connection_focus_missing || room_focus_relocated){if(!room_focus_relocated)focus=first_room_arrived || connection_focus_missing?0u:3u;call(c,0x800FA5BC,item(c,first+focus));}
}
void start(PPCContext& c,bool hosting,bool invitation=false){
    transfer_lobby_gui=0;last_host=hosting;last_invitation=invitation;retry_room.reset();
    native_frontend_custom_map_progress({});notify_error=false;error.clear();
    auto address=hosting?host_endpoint:endpoint;address.internet=invitation;address.invitation_code.clear();
    if(invitation && !hosting){std::string why;const auto parsed=invite::decode_invitation(invitation_code,why);if(!parsed){error="invalid-invite";show(c,Page::Error);return;}address.invitation_code=invite::encode_invitation(*parsed);address.address="127.0.0.1";address.port=27341;address.pin=parsed->pin;}
    if(!hosting && !invitation){const auto colon=address.address.rfind(':');if(colon!=std::string::npos){try{address.port=unsigned(std::stoul(address.address.substr(colon+1)));}catch(...){address.port=0;}address.address.resize(colon);}}
    if(const auto invalid=frontend_endpoint_error(address);invalid!=FrontendEndpointError::None){error=invalid==FrontendEndpointError::Address?"invalid-address":invalid==FrontendEndpointError::Pin?"invalid-pin":"invalid-port";ssne::log_message("[native-network-menu] validation failed: "+error);show(c,Page::Error);return;}
    error=services.start?services.start(hosting,address):"unavailable";if(!error.empty()){ssne::log_message("[native-network-menu] start failed: "+error);show(c,Page::Error);return;}setup_seen=false;main_idle_frames=0;if(hosting && invitation)show(c,Page::InviteStatus);else if(hosting)close(c,0);else{waiting_for_host_world=true;show(c,Page::Connecting);}
}
void join_lan(PPCContext& c,const FrontendRoom& room){
    transfer_lobby_gui=0;retry_room=room;last_host=false;last_invitation=false;endpoint.internet=false;endpoint.invitation_code.clear();endpoint.address=room.address;endpoint.port=room.port;
    native_frontend_custom_map_progress({});notify_error=false;error.clear();
    error=services.join_lan?services.join_lan(room):"unavailable";
    if(!error.empty()){ssne::log_message("[native-network-menu] LAN join failed: "+error);show(c,Page::Error);return;}
    setup_seen=false;main_idle_frames=0;waiting_for_host_world=true;show(c,Page::Connecting);
}
}
bool native_frontend_start_profile(PPCContext& c){
    static bool opened=false;
    if(!native_frontend_startup_ready() || opened || !gui || native_root_id(gui)!=0x3d || visible || changing)return false;
    const auto entry=item(c,0x3e);if(!entry)return false;
    opened=true;call(c,0x800FB130,entry,22);return true;
}
bool native_frontend_startup_ready(){
    return startup_readiness.ready() && gui==startup_readiness.owner() && !changing &&
           native_root_id(gui)!=0xaf;
}
void configure_native_startup_language(int language){startup_language_override.store(language>=-1 && language<=2?language:-1,std::memory_order_relaxed);}
unsigned native_menu_language(){const auto language=mem_read32(0x802DB838);return language<3?language:0;}
MultiplayerOptions native_frontend_options(){return options;}
void configure_native_frontend(NativeFrontendServices value){waiting_for_host_world=false;transfer_lobby_gui=0;services=std::move(value);
    startup_readiness.reset();
    options={};if(!services.options_path.empty()){
        std::ifstream in(services.options_path,std::ios::binary);std::string magic;MultiplayerOptions saved;
        std::error_code ec;const auto size=std::filesystem::file_size(services.options_path,ec);
        if(in && !ec && size<=4096 && in.peek()!=EOF){std::getline(in,magic);if((magic=="SSNE-MP-1" || magic=="SSNE-MP-2") && (in>>std::quoted(saved.name)>>saved.enemies>>saved.capacity>>saved.pickups)){if(magic=="SSNE-MP-2")in>>std::quoted(saved.host_name)>>std::quoted(saved.pin)>>saved.use_password>>saved.skin;if(in && valid_multiplayer_options(saved)){options=std::move(saved);unsigned saved_port=0;if(in>>saved_port && saved_port>0 && saved_port<=65535)host_endpoint.port=saved_port;}}}
    }
if(getenv("SSNE_TEST_HIDDEN"))if(const auto port=getenv("SSNE_NATIVE_MENU_PORT")){const auto n=std::strtol(port,nullptr,10);if(n>0 && n<=65535)host_endpoint.port=endpoint.port=unsigned(n);}}
void install_native_frontend(){
    for(auto [address,expected]:{std::pair{0x800F60A0u,0xae6ce28a14da5c62ull},std::pair{0x800FF15Cu,0xda46ee353752077cull},std::pair{0x800F55B4u,0x53f0f80b625ba833ull},std::pair{0x801597BCu,0x5ca15657a7fd49e3ull},std::pair{0x80155568u,0xa927cf7d3a0c0ea0ull},std::pair{0x800F9978u,0x850e4e0004bd9a48ull},std::pair{0x800F86C8u,0x3849d0d1122bcc69ull},std::pair{0x80100a18u,0x7b7753aedb5e3f76ull}}){uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<32;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}if(!ssne_find_func(address) || hash!=expected)throw std::runtime_error("Original network menu revision differs");}
    open_original=ssne_find_func(0x800F60A0)->func;ssne_install_hook(0x800F60A0,open);select_original=ssne_find_func(0x800FF15C)->func;ssne_install_hook(0x800FF15C,select);destroy_original=ssne_find_func(0x800F55B4)->func;ssne_install_hook(0x800F55B4,destroy);
    language_original=ssne_find_func(0x801597BC)->func;ssne_install_hook(0x801597BC,language);
    const auto menu_hook=*ssne_hook_slot(0x800F9978);
    pause_original=menu_hook ? menu_hook : ssne_find_func(0x800F9978)->func;
    ssne_install_hook(0x800F9978,pause_open);
    gui_update_original=ssne_find_func(0x800F86C8)->func;ssne_install_hook(0x800F86C8,gui_update);
    scene_original=ssne_find_func(0x80155568)->func;ssne_install_hook(0x80155568,scene);
    profile_body_tick_original=ssne_find_func(0x80100a18)->func;ssne_install_hook(0x80100a18,profile_body_tick);
    install_custom_level_menu();
}
bool native_frontend_visible(){return visible.load();}
bool native_frontend_text_active(){return editing!=Edit::None;}
bool native_frontend_pending_actions(){
    if(visible.load() || paused.load() || page!=Page::Closed || editing.load()!=Edit::None ||
       hotkey.load() || action!=99 || return_main_requested.load() ||
       handoff_release.load() || changing || notify_error || result_overlay || session_error_pending.load() || custom_map_phase.load()!=NativeCustomMapPhase::None)return true;
    std::lock_guard lock(text_mutex);return !text_events.empty();
}
void native_frontend_return_main(){waiting_for_host_world=false;transfer_lobby_gui=0;return_main_requested=true;next_back={};}
void native_frontend_finish_join(PPCContext& c){
    if(!std::exchange(waiting_for_host_world,false))return;
    if(page==Page::Connecting && gui)close(c,0);
}
void native_frontend_disconnected(const std::string& reason){
    waiting_for_host_world=false;
    transfer_lobby_gui=0;
    if(session_error_pending.load() || error.starts_with("custom-level:") || error.starts_with("native-setup:") || error=="cancelled")return;
    native_frontend_custom_map_progress({});
    error=reason.starts_with("No cooperative data received") || reason.starts_with("No cooperative send progress")
        ? "lost:timeout" : "lost";
    notify_error=true;
}
static void queue_session_error(std::string reason){
    transfer_lobby_gui=0;
    std::lock_guard lock(text_mutex);
    if(session_error_pending)return;
    pending_session_error=std::move(reason);
    session_error_pending=true;
}
void native_frontend_custom_level_error(std::string message){queue_session_error("custom-level:"+message.substr(0,256));}
void native_frontend_bootstrap_error(const std::string& message){queue_session_error("native-setup:"+message.substr(0,256));}
void native_frontend_custom_map_progress(NativeCustomMapProgress progress){
    std::lock_guard lock(custom_map_mutex);
    if(progress.phase==NativeCustomMapPhase::None)custom_map_cancel=false;
    else if(custom_map_cancel.load())return;
    progress.title.resize(std::min<size_t>(progress.title.size(),192));progress.alias.resize(std::min<size_t>(progress.alias.size(),96));
    custom_map_progress=std::move(progress);custom_map_phase.store(custom_map_progress.phase);
}
NativeCustomMapProgress native_frontend_custom_map_progress(){std::lock_guard lock(custom_map_mutex);return custom_map_progress;}
bool native_frontend_take_custom_map_cancel(){return custom_map_cancel.exchange(false);}
void native_frontend_engine_scope(bool enabled){engine_scope=enabled;}
bool native_frontend_text_input(const std::string& value){if(!native_frontend_text_active())return false;std::lock_guard lock(text_mutex);if(text_events.size()<128)text_events.emplace_back(0,value.substr(0,256));return true;}
bool native_frontend_edit_key(unsigned key){if(!native_frontend_text_active())return false;std::lock_guard lock(text_mutex);text_events.emplace_back(key,"");return true;}
bool native_frontend_input(const ssne::PadState& pad){
    input.store(uint64_t(pad.buttons)|(uint64_t(pad.stick_x)<<16)|(uint64_t(pad.stick_y)<<24));static uint16_t before=0;const auto pressed=pad.buttons&~before;before=pad.buttons;
    if(handoff_release){if(pad.buttons&(ssne::PAD_A|ssne::PAD_B|ssne::PAD_START))return true;handoff_release=false;}
    if(!visible && !ssne::native_results_presenting() && (native_lobby_visible() || paused) && active()){if(native_lobby_visible() && ((pressed&ssne::PAD_Z) || (!host() && (pressed&(ssne::PAD_A|ssne::PAD_START))))){hotkey=6;return true;}if((pressed&ssne::PAD_X) || (!host() && native_lobby_visible() && (pressed&ssne::PAD_B))){hotkey=1;return true;}if(host() && (pressed&ssne::PAD_Y)){hotkey=2;return true;}}return visible.load() || return_main_requested.load();
}
void native_frontend_lobby_ready(){hotkey=6;}
void native_frontend_lobby_map(){hotkey=5;}
void native_frontend_tick(PPCContext&){
    if(!visible)return;
    if(!(uint16_t(input.load())&(ssne::PAD_A|ssne::PAD_B|ssne::PAD_START)))wait_release=false;
    ssne::set_pad(0,{});if(ssne::netplay_local_player()>0)ssne::set_pad(ssne::netplay_local_player(),{});ssne::si_poll();
}
unsigned native_focus_id(){const auto node=gui?mem_read32(gui+0x7c):0;return node>=0x80000000u && node<ssne::guest_ram_end()-0x60?mem_read16(mem_read32(node+0x1c)+2):65535;}
unsigned native_root_id(uint32_t owner=gui){auto node=owner?mem_read32(owner+0x7c):0;for(unsigned n=0;n<32 && node>=0x80000000u && node<ssne::guest_ram_end()-0x60;++n){const auto parent=mem_read32(node+0x24);if(!parent)return mem_read16(mem_read32(node+0x1c)+2);node=parent;}return 65535;}
uint64_t native_frontend_mouse_epoch(){
    uint64_t value=mouse_epoch*1099511628211ull;
    value^=uint64_t(active())|(uint64_t(host())<<1)|(uint64_t(unsigned(page))<<8);
    if(page==Page::Rooms){
        value=(value^room_offset)*1099511628211ull;
        for(const auto& room:displayed_room_rows){
            if(room){for(unsigned char ch:lan_room_key(*room))value=(value^ch)*1099511628211ull;}
            else value=(value^UINT64_MAX)*1099511628211ull;
        }
    }
    return value;
}
bool native_frontend_mouse_allowed(uint32_t owner,uint32_t node,uint16_t root,uint16_t id,unsigned event){
    // Native button captions still report selectable when their disabled flag
    // only changes the draw colour. Keep the empty custom category inert, and
    // never let its informational caption consume a carousel confirmation.
    if(root==0x4b && (id==no_custom_levels_label || (id==custom_levels_button && node &&
       (mem_read32(mem_read32(node+0x1c)+0xc)&0x10u))))return false;
    if(visible){
        if(owner!=gui || root!=group)return false;
        if(id>=first && id<first+4){
            if(labels[id-first+1].empty() || (page==Page::Connecting && id!=first+3))return false;
            if((page==Page::Options && active() && (id==first || !host())) || (page==Page::Bots && id!=first+3 && !host()) || (page==Page::SessionOptions && (!active() || !host())) || (page==Page::Connection && id==first+1 && !host()))return false;
        }
        return true;
    }
    if(!active())return true;
    if(root==group)return false;
    if(ssne::native_results_presenting())return owner==ssne::native_results_menu() && host() && id!=result_connection;
    if(native_lobby_visible()){
        const auto who=native_lobby_mouse_owner(owner);
        if(who==-2)return id==lobby_connection || (id==lobby_ready_button && native_lobby_can_ready()) || (host() && (id==lobby_invite || id==lobby_map || id==lobby_bots || (id==lobby_start && native_lobby_can_start())));
        return who==ssne::netplay_local_player() && (id==0xA2 || id==0xA3) && (event!=24 || host());
    }
    const auto scene=mem_read32(0x802DB7E8);
    if(scene==3)return host(); // The authority alone changes shared rules/map/difficulty.
    const auto own=ssne::netplay_local_player();
    const auto actor=own>=0 && own<8?mem_read32(ssne::native_party_registry()+unsigned(own)*4):0;
    if(actor>=0x80000000u && actor<=ssne::guest_ram_end()-0x790u && mem_read32(actor+0x71c)==owner)return true;
    return scene==4 && ssne::local_information_open() && owner==ssne::native_gameplay_gui();
}
bool native_frontend_mouse_event(PPCContext& c,uint32_t owner,uint32_t,uint16_t root,uint16_t id,unsigned event){
    if(root==group && setup_event(c,owner,id,event))return true;
    if(root==0x3d && id==main_online_native::online_id){if(event==22)open_online(c,owner);return true;}
    if(root==0x3d && id==main_exit_native::exit_id){if(event==22 && services.request_quit)services.request_quit();return true;}
    if(root==0x9a && id==lobby_invite){if(event==22 && active() && host() && native_lobby_visible() && owner==native_lobby_mouse_main() && native_frontend_mouse_allowed(owner,0,root,id,event))hotkey=8;return true;}
    if(root==0x9a && (id==lobby_start || id==lobby_map || id==lobby_ready_button)){if(event==22 && native_lobby_visible() && native_frontend_mouse_allowed(owner,0,root,id,event))hotkey=id==lobby_start?7:id==lobby_map?5:6;return true;}
    if(id==profile_skin_selector && root==0xaf && event==22){
        if(!active()){profile_skin_native::changed(c,owner);call(c,0x800F6E3C,owner,0xb1,22);}
        return true;
    }
    if(id==custom_levels_button && root==0x4b){if(event==22)custom_level_menu_toggle(c,owner);return true;}
    if(id==survival_button && root==0xA7){if(event==12)survival_details(c,owner);if(event==22 && active() && host()){gui=owner;hotkey=4;}return true;}
    if(footer_root(id,root)){
        if(event==23)return false; // Back belongs to the current native root/page.
        if(event==22 && (active() || (id==options_button && visible && owner==gui))){
            gui=owner;if(id!=options_button)in_pause=root==0x129;
            // Keep Continue as the score screen's exact native focus. The
            // preferred-child focus helper would descend back into this button.
            if(id==result_connection)if(const auto score=item(c,0xBC)){call(c,0x800F6B6C,gui);call(c,0x800F6A94,gui,score);}
            hotkey=id==options_button?3:(id==group+8 || id==lobby_bots)?2:1;
        }
        return true;
    }
    if(owner!=gui || !visible || root!=group)return false;
    if(event==23){if(editing!=Edit::None){editing=Edit::None;wait_release=true;}else action=98;return true;}
    if(event==22 && id>=first && id<first+4){
        if(editing!=Edit::None){accept_edit(c);if(editing!=Edit::None)return true;}
        focus=id-first;wait_release=false;choose_frontend_action(focus);return true;
    }
    return false;
}
void native_frontend_prepare_results_close(PPCContext& c){retire_result_overlay(c,true);}
void native_frontend_frame(PPCContext& c){
    if(session_error_pending.load()){
        std::string reason;{std::lock_guard lock(text_mutex);reason.swap(pending_session_error);session_error_pending=false;}
        native_frontend_custom_map_progress({});release_transfer_pause(c);
        if(active() && services.stop)services.stop();
        error=std::move(reason);notify_error=true;
        if(error.starts_with("custom-level:"))ssne::log_message("[custom-level-error] "+error.substr(13));
    }
    custom_level_menu_frame(c);
    show_custom_map_progress(c);
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_NATIVE_MENU")){
        static auto next=std::chrono::steady_clock::time_point{};const auto now=std::chrono::steady_clock::now();
        if(now>=next){next=now+std::chrono::milliseconds(100);const auto transfer=native_frontend_custom_map_progress();
            std::ofstream out("native-custom-map-transfer.json.pending",std::ios::binary|std::ios::trunc);
            out<<"{\"phase\":"<<unsigned(transfer.phase)<<",\"busy\":"<<custom_map_busy()<<",\"cancel\":"<<custom_map_cancel.load()<<",\"title\":"<<quoted_json(transfer.title)<<",\"alias\":"<<quoted_json(transfer.alias)<<",\"preview_path\":"<<quoted_json(transfer.preview_path)<<",\"received\":"<<transfer.received<<",\"total\":"<<transfer.total<<",\"visible\":"<<(visible && page==Page::Connecting)<<",\"cancel_focus\":"<<(focus==3)<<",\"pause_gui\":"<<transfer_pause_gui<<"}";
            out.close();if(out){std::remove("native-custom-map-transfer.json");std::rename("native-custom-map-transfer.json.pending","native-custom-map-transfer.json");}
        }
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_NATIVE_MENU") && page==Page::Rooms){
        static auto next=std::chrono::steady_clock::time_point{};if(std::chrono::steady_clock::now()>=next){next=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
            std::ofstream out("lan-browser-state.json.pending",std::ios::binary|std::ios::trunc);out<<"{\"rooms\":[";bool comma=false;
            for(const auto& r:frontend_rooms()){if(comma)out<<',';comma=true;out<<"{\"session_id\":\""<<r.session_id<<"\",\"port\":"<<r.port<<",\"mode\":"<<r.mode<<",\"map\":\""<<r.map<<"\",\"ping_ms\":"<<r.ping_ms<<",\"humans\":"<<r.humans<<",\"bots\":"<<r.bots<<",\"spectators\":"<<r.spectators<<",\"enemies\":"<<r.enemies<<",\"ammo_capacity\":"<<r.ammo_capacity<<",\"pickups\":"<<r.pickups<<",\"running\":"<<r.running<<",\"details_known\":"<<r.details_known<<",\"joinable\":"<<(lan_room_has_human_slot(r) && valid_lan_secret(r.join_ticket) && r.join_expires_ms>lan_clock_ms())<<"}";}
            out<<"]}";out.close();if(out){std::remove("lan-browser-state.json");std::rename("lan-browser-state.json.pending","lan-browser-state.json");}
        }
    }
    prepare_mouse_footers(c);
    // A replica follows the authority into the next level even if its local
    // connection panel was open. Retire that panel with the result screen.
    if(result_overlay && !ssne::native_results_presenting())retire_result_overlay(c,false);
    // Native Back restores the main root internally without calling open(3D).
    // Local split-screen setup and Online own separate main-menu entries.
    if(local_mode && !active() && !visible){
        const auto root=native_root_id();
        if(root==0xa7)setup_seen=true;
        else if(root==0x3d && setup_seen){local_mode=false;setup_seen=false;}
    }
    if(return_main_requested && !active() && mem_read32(0x802DB7E8)==3){
        unsigned checked=0;
        for(auto root=mem_read32(0x8028BFA4);root && checked++<128;root=mem_read32(root+8)){
            if(root<0x80000000u || root>ssne::guest_ram_end()-0x90)break;
            const bool interrupted_join=notify_error && root==gui && storage.contains(root) &&
                (page==Page::Connecting || !mem_read32(root+0x7c));
            if(!mem_read32(root+0x14) && interrupted_join){
                in_pause=false;return_main_requested=false;break;
            }
            if(!mem_read32(root+0x14) && (call(c,0x800F6030,root,0x9a) || call(c,0x800F6030,root,0x3d))){
                gui=root;in_pause=false;
                if(native_root_id()==0x3d){if(uint16_t(input.load())&(ssne::PAD_A|ssne::PAD_B|ssne::PAD_START))break;return_main_requested=false;ssne::log_message("[native-network-menu] original main menu restored");}
                else if(std::chrono::steady_clock::now()>=next_back){const auto node=mem_read32(gui+0x7c);if(node){call(c,0x800FB130,node,23);next_back=std::chrono::steady_clock::now()+std::chrono::milliseconds(300);}}
                break;
            }
        }
    }
    if(active() && page==Page::Closed && gui && mem_read32(0x802DB7E8)==3 && mem_read32(0x802DB7EC)!=4){
        const auto root=native_root_id();
        if(root==0xa7 || root==0x9a || root==0x59 || root==0x64 || root==0x76 || root==0x86 || root==0x4b || root==0x44){setup_seen=true;main_idle_frames=0;}
        else if(root==0x3d && setup_seen){if(++main_idle_frames>=4){action=97;main_idle_frames=0;}}
        else main_idle_frames=0;
    }else main_idle_frames=0;
    if(visible){native_frontend_tick(c);ssne::refresh_native_menu_input(c);}
    const auto own=ssne::netplay_local_player();paused=active() && mem_read32(0x802DB7E8)==4 && own>=0 && own<8 && (mem_read32(c.r[13]-0x765c)&(1u<<own));
    if(getenv("SSNE_TEST_HIDDEN")){
        static uint64_t accepted=0;
        if(const auto path=getenv("SSNE_NATIVE_MENU_COMMAND")){std::ifstream f(path);uint64_t sequence;std::string command;if(f>>sequence>>command && sequence>accepted){if(command=="text"){std::string value;std::getline(f,value);native_frontend_text_input(value.empty()?value:value.substr(1));}else if(command=="key"){unsigned key;if(f>>key)native_frontend_edit_key(key);}accepted=sequence;}}
        if(getenv("SSNE_DEV_NATIVE_MENU") && !ssne::perf::fixture_quiet.load(std::memory_order_relaxed)){const auto shared=services.shared_options?services.shared_options():options;const auto invite=invitation_status();const auto& displayed=(host() || page==Page::Host)?host_endpoint:endpoint;std::ofstream out("native-menu-state.json.pending");out<<"{\"page\":"<<unsigned(page)<<",\"visible\":"<<visible<<",\"editing\":"<<unsigned(editing.load())<<",\"language\":"<<native_menu_language()<<",\"port\":"<<displayed.port<<",\"pin\":\""<<displayed.pin<<"\",\"address\":\""<<displayed.address<<"\",\"active\":"<<active()<<",\"invitation_host_pending\":"<<invitation_host_pending()<<",\"invitation_copy_state\":"<<unsigned(invite.copy_state)<<",\"invitation_copy_sequence\":"<<invite.copy_sequence<<",\"invitation_code\":"<<quoted_json(invite.code)<<",\"host\":"<<host()<<",\"scene\":"<<mem_read32(0x802DB7E8)<<",\"lobby\":"<<native_lobby_visible()<<",\"runtime_ready\":"<<ssne::netplay_world_ready()<<",\"local_slot\":"<<own<<",\"info\":"<<ssne::local_information_open()<<",\"movie\":"<<ssne::native_movie_presenting()<<",\"pause\":"<<paused<<",\"survival_mode\":"<<native_lobby_survival()<<",\"game_mode\":"<<mem_read32(c.r[13]-0x69d4)<<",\"mode_preview\":"<<(native_root_id()==0xA7 && native_focus_id()==survival_button)<<",\"native_focus\":"<<native_focus_id()<<",\"native_root\":"<<native_root_id()<<",\"returning\":"<<return_main_requested<<",\"gui\":"<<gui<<",\"gui_port\":"<<(gui?int16_t(mem_read16(gui+0x84)):-2)<<",\"wait_release\":"<<wait_release<<",\"input\":"<<uint16_t(input.load())<<",\"focus\":"<<focus<<",\"skill\":"<<skill<<",\"enemy_factor\":"<<options.enemies<<",\"ammo_capacity\":"<<options.capacity<<",\"pickup_factor\":"<<options.pickups<<",\"shared_enemies\":"<<shared.enemies<<",\"shared_capacity\":"<<shared.capacity<<",\"shared_pickups\":"<<shared.pickups<<",\"name_bytes\":"<<options.name.size()<<",\"players\":"<<(services.players?services.players():0)<<",\"bots\":"<<(host() && services.bot?services.bot(0,skill):0)<<",\"rooms\":"<<(services.rooms?services.rooms().size():0)<<",\"scanning\":"<<(services.scanning && services.scanning())<<",\"room_focus_matches\":"<<(page!=Page::Rooms || native_focus_id()==first+focus)<<",\"results_root\":"<<(ssne::native_results_presenting()?native_root_id(ssne::native_results_menu()):65535u)<<"}";out.close();std::remove("native-menu-state.json");std::rename("native-menu-state.json.pending","native-menu-state.json");}
    }
    if(notify_error && !return_main_requested && !active() && gui && mem_read32(0x802DB7E8)==3){notify_error=false;open_online(c,gui,Page::Error);}
    const auto requested=hotkey.exchange(0);
    if(requested==9){
        if(visible && page==Page::OnlineSetup){host_details_return=page;prepare_host_endpoint();show(c,Page::Host);}
        return;
    }
    if(requested==8){
        if(active() && host() && native_lobby_visible() && services.copy_lobby_invitation)services.copy_lobby_invitation();
        return;
    }
    if(requested>=5 && requested<=7){
        if(active() && native_lobby_visible()){
            std::string why;
            if(requested==6)toggle_native_lobby_ready();
            else if(host() && requested==5){if(!open_native_lobby_level_selection(c,why))ssne::log_message("[lobby-map] "+why);}
            else if(host() && !start_native_lobby_level(why))ssne::log_message("[lobby-start] "+why);
        }
        return;
    }
    if(requested==4 && host() && gui && native_root_id(gui)==0xA7){
        if(const auto deathmatch=item(c,0xAA)){
            SurvivalSelectionScope dispatch;set_native_lobby_survival(true);
            call(c,0x800FB130,deathmatch,22);
            ssne::log_message("[native-survival-menu] original deathmatch setup selected for Survival");
        }
    }
    if(requested==3 && visible && has_options_button()){
        if(editing!=Edit::None)accept_edit(c);
        if(editing!=Edit::None)return;
        if(page==Page::Direct)show(c,direct_return);
        else if(page==Page::Rooms)show(c,Page::BrowserTools);
        else if(page==Page::BrowserTools){room_filter.hide_full=!room_filter.hide_full;reset_room_view();invalidate_labels();}
        else if(page==Page::OnlineSetup)show(c,Page::Home);else if(page==Page::SessionOptions){bots_return=Page::Closed;close(c,0);resume(c);}else if(page==Page::Bots && bots_return==Page::SessionOptions)show(c,Page::SessionOptions);else if(page==Page::Connection && host())show(c,Page::SessionOptions);else if(page==Page::Options)show(c,options_return);else{options_return=page;show(c,Page::Options);}
    }
    else if(requested && requested!=4 && active()){
        const auto results=ssne::native_results_presenting()?ssne::native_results_menu():0u;
        if(results)return;
        if(requested==2 && !host())return;
        if(gui){if(in_pause)call(c,0x800F6178,gui,0x129);else suspend_native_lobby(c);}
        if(gui)show(c,requested==2?Page::SessionOptions:Page::Connection);
    }
    if(page==Page::Connecting && !custom_map_busy()){
        if(active() && !waiting_for_host_world){
            const auto lobby=transfer_lobby_gui.exchange(0);
            close(c,0);release_transfer_pause(c);
            if(lobby && lobby==gui && mem_read32(0x802DB7E8)==3)resume_native_lobby(c);
        }else if(!active() && !notify_error && services.connecting && !services.connecting()){
            waiting_for_host_world=false;
            release_transfer_pause(c);error=services.status?services.status():"failed";show(c,Page::Error);
        }
    }
    if(custom_map_phase.load()==NativeCustomMapPhase::Ready && (page!=Page::Connecting || waiting_for_host_world)){
        release_transfer_pause(c);auto complete=native_frontend_custom_map_progress();complete.phase=NativeCustomMapPhase::None;native_frontend_custom_map_progress(std::move(complete));
    }
    std::deque<std::pair<unsigned,std::string>> events;{std::lock_guard lock(text_mutex);events.swap(text_events);}
    for(auto& [key,value]:events)if(editing!=Edit::None){
        if(key==1)accept_edit(c);else if(key==2){editing=Edit::None;wait_release=true;}else if(key==5){replace_all=true;cursor=0;}else if((key==3 || key==4) && replace_all){edit_buffer.clear();cursor=0;replace_all=false;}else if(key==3 && cursor)edit_buffer.erase(--cursor,1);else if(key==4 && cursor<edit_buffer.size())edit_buffer.erase(cursor,1);
        else if(!key){if(replace_all && !value.empty()){edit_buffer.clear();cursor=0;replace_all=false;}for(unsigned char ch:(name_field(editing)?native_name_input(value):value)){const bool allowed=name_field(editing)?(ch>=32 && (ch<127 || ch>=160)):editing==Edit::Invitation?std::string("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ- ").find(char(ch))!=std::string::npos:editing==Edit::Address?std::string("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:").find(char(ch))!=std::string::npos:ch>='0' && ch<='9';const auto limit=edit_limit(editing);if(allowed){if(cursor<edit_buffer.size())edit_buffer[cursor++]=char(ch);else if(edit_buffer.size()<limit){edit_buffer+=char(ch);++cursor;}}}}
    }
    static uint16_t before=0;const auto packed=input.load();auto pad=uint16_t(packed);const auto y=uint8_t(packed>>24);if(y>192)pad|=ssne::PAD_UP;else if(y<64)pad|=ssne::PAD_DOWN;const auto pressed=uint16_t(pad&~before);before=pad;
    // The transport owns controller routing. Feed its local menu edges into
    // the original widgets' focus/event functions, keeping game input neutral.
    if(visible && gui && editing==Edit::None && !wait_release && page==Page::OnlineSetup){
        auto setup_id=[&](){return focus<online_setup_native::editable_ids.size()?unsigned(online_setup_native::editable_ids[focus]):options_button;};
        if(pressed&(ssne::PAD_UP|ssne::PAD_DOWN)){
            focus=unsigned((int(focus)+(pressed&ssne::PAD_UP?-1:1)+5)%5);
            if(const auto node=item(c,setup_id()))call(c,0x800FA5BC,node);
        }else if(pressed&(ssne::PAD_LEFT|ssne::PAD_RIGHT)){
            const auto id=setup_id();if(id==online_setup_native::character_id)
                if(const auto node=item(c,id)){const auto d=mem_read32(node+0x1c);call(c,0x800FB130,node,(mem_read32(d+0xc)&0x2000u?20u:18u)+(pressed&ssne::PAD_RIGHT?1u:0u));}
        }else if(pressed&(ssne::PAD_A|ssne::PAD_START)){
            if(focus==4)hotkey=3;else setup_event(c,gui,setup_id(),22);
        }else if(pressed&ssne::PAD_B)action=98;
    }else if(visible && gui && editing==Edit::None && !wait_release){
        if(pressed&(ssne::PAD_UP|ssne::PAD_DOWN)){
            const int direction=pressed&ssne::PAD_UP?-1:1;
            const unsigned count=has_options_button()?5:4;
            for(unsigned i=0;i<count;++i){focus=unsigned(int(focus)+direction+int(count))%count;const auto node=item(c,focus==4?options_button:first+focus);if(node && (focus==4 || (!labels[focus+1].empty() && (page!=Page::Connecting || focus==3) && (page!=Page::Options || !active() || (focus!=0 && host())) && (page!=Page::SessionOptions || (active() && host()))))){if(focus==4){call(c,0x800F6B6C,gui);call(c,0x800F6A94,gui,node);}else call(c,0x800FA5BC,node);break;}}
        }else if(pressed&(ssne::PAD_A|ssne::PAD_START)){
            if(focus==4)hotkey=3;
            const auto node=item(c,focus==4?options_button:first+focus);if(node && (focus==4 || (!labels[focus+1].empty() && (page!=Page::Connecting || focus==3) && (page!=Page::Options || !active() || (focus!=0 && host())) && (page!=Page::SessionOptions || (active() && host())))))if(focus!=4)call(c,0x800FB130,node,22);
        }else if(pressed&ssne::PAD_B)action=98; // Route Back through the page transition, not just widget hiding.
    }
    if(editing!=Edit::None){
        if(pressed&ssne::PAD_B){editing=Edit::None;wait_release=true;}else if(pressed&(ssne::PAD_A|ssne::PAD_START))accept_edit(c);else if(pressed&ssne::PAD_LEFT){replace_all=false;if(cursor)--cursor;}else if(pressed&ssne::PAD_RIGHT){replace_all=false;cursor=std::min<unsigned>(cursor+1,edit_buffer.size());}
        else if(pressed&ssne::PAD_X){replace_all=false;if(cursor<edit_buffer.size())edit_buffer.erase(cursor,1);else if(cursor)edit_buffer.erase(--cursor,1);}
        else if(pressed&ssne::PAD_Y){replace_all=false;const auto limit=edit_limit(editing);if(edit_buffer.size()<limit){edit_buffer+='0';cursor=unsigned(edit_buffer.size()-1);}}
        else if(pressed&(ssne::PAD_UP|ssne::PAD_DOWN)){replace_all=false;if(cursor>=edit_buffer.size())edit_buffer+='0';auto& ch=edit_buffer[cursor];const std::string alphabet=editing==Edit::Invitation?"0123456789ABCDEFGHJKMNPQRSTVWXYZ-":editing==Edit::Address?"0123456789.-:abcdefghijklmnopqrstuvwxyz":name_field(editing)?" abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-":"0123456789";const auto at=alphabet.find(ch);ch=alphabet[(unsigned(at==std::string::npos?0:at)+(pressed&ssne::PAD_UP?1:alphabet.size()-1))%alphabet.size()];}
    }
    const auto chosen=std::exchange(action,99u);const auto selected_room=std::exchange(pending_room,std::nullopt);
    if(chosen==97){if(services.stop)services.stop();}else if(chosen==98){if(page==Page::InviteHost)show(c,Page::Host);else if(page==Page::InviteJoin)show(c,Page::Join);else if(page==Page::Host)show(c,host_details_return);else if(page==Page::Direct)show(c,direct_return);else if(page==Page::Error)show(c,connection_origin());else if(page==Page::InviteStatus)back_from_invitation_status(c);else if(page==Page::SessionOptions){bots_return=Page::Closed;close(c,0);resume(c);}else if(page==Page::Bots && bots_return==Page::SessionOptions)show(c,Page::SessionOptions);else if(page==Page::Options)show(c,options_return);else if(page==Page::Connecting)cancel_connection(c);else if(page==Page::Rooms)show(c,rooms_return);else if(page==Page::BrowserTools)show(c,Page::Rooms);else if(page!=Page::Home && !active())show(c,Page::Home);else if(active()){close(c,0);resume(c);}else if(gui)close(c,0x3d);}
    else if(chosen<4)switch(page){
    case Page::SessionOptions:
        if(active() && host()){
            if(chosen==3){bots_return=Page::SessionOptions;show(c,Page::Bots);}
            else{if(chosen==0)options.enemies=options.enemies%5+1;else if(chosen==1)options.capacity=next_ammo_capacity(options.capacity);else if(chosen==2)options.pickups=options.pickups%5+1;save_options();if(services.set_rules)services.set_rules(options);}
        }break;
    case Page::Options:
        if(chosen==0){if(!active())edit(Edit::Name);}
        else if(!name_only_options() && (!active() || host())){
            if(chosen==1)options.enemies=options.enemies%5+1;
            else if(chosen==2)options.capacity=next_ammo_capacity(options.capacity);
            else options.pickups=options.pickups%5+1;
            save_options();if(active() && host() && services.set_rules)services.set_rules(options);
        }break;
    case Page::BrowserTools:
        if(chosen==0){reset_room_view();if(services.scan)services.scan();show(c,Page::Rooms);}
        else if(chosen==1){room_sort=LanRoomSort((unsigned(room_sort)+1)%5);reset_room_view();}
        else if(chosen==2){room_filter.mode=(room_filter.mode+1)%6;reset_room_view();}
        else show(c,Page::Rooms);break;
    case Page::Home:
        if(chosen==0)show(c,Page::Join);
        else if(chosen==1){prepare_host_endpoint();start(c,true);}
        else if(chosen==2)show(c,Page::OnlineSetup);
        else close(c,0x3d);
        break;
    case Page::InviteHost:if(chosen==0)start(c,true,true);else if(chosen==1){options_return=page;show(c,Page::Options);}else show(c,Page::Host);break;
    case Page::InviteJoin:if(chosen==0)edit(Edit::Invitation);else if(chosen==1)start(c,false,true);else if(chosen==2){options_return=page;show(c,Page::Options);}else show(c,Page::Join);break;
    case Page::InviteStatus:if(chosen==0 && services.copy_invitation){const auto value=invitation_status();if(!value.code.empty() && value.phase!=1 && value.phase!=5)services.copy_invitation(value.code);}else if(chosen==1){options_return=page;show(c,Page::Options);}else if(chosen==2)configure_invitation_host(c);else if(chosen==3)back_from_invitation_status(c);break;
    case Page::Host:
        if(chosen==0)edit(Edit::Pin);
        else if(chosen==1)edit(Edit::Port);
        else if(chosen==2){options.use_password=!options.use_password;prepare_host_endpoint();save_options();}
        else show(c,host_details_return);
        break;
    case Page::Join:
        if(chosen==0){show(c,Page::InviteJoin);edit(Edit::Invitation);}
        else if(chosen==1){rooms_return=Page::Join;reset_room_view();if(services.scan)services.scan();show(c,Page::Rooms);}
        else if(chosen==2){direct_return=Page::Join;show(c,Page::Direct);}
        else show(c,Page::Home);
        break;
    case Page::Direct:
        if(chosen==0)edit(Edit::Address);
        else if(chosen==1)edit(Edit::Pin);
        else if(chosen==2)edit(Edit::Port);
        else start(c,false);
        break;
    case Page::Connecting:if(chosen==3)cancel_connection(c);break;
    case Page::Error:
        if(chosen==0){
            if(retry_room){reset_room_view();if(services.scan)services.scan();show(c,Page::Rooms);}
            else start(c,last_host,last_invitation);
        }else if(chosen==1 && !retry_room){
            if(last_host){host_details_return=Page::Error;prepare_host_endpoint();show(c,Page::Host);}
            else if(last_invitation){show(c,Page::InviteJoin);edit(Edit::Invitation);}
            else show(c,Page::Direct);
        }else if(chosen==3)show(c,connection_origin());
        break;
    case Page::Bots:if(chosen==0 && services.bot)services.bot(1,skill);else if(chosen==1)skill=skill%3+1;else if(chosen==2 && services.bot)services.bot(-1,skill);else if(chosen==3){if(bots_return==Page::SessionOptions)show(c,Page::SessionOptions);else{close(c,0);resume(c);}}break;
    case Page::Connection:if(chosen==0){close(c,0);resume(c);}else if(chosen==1 && host())show(c,Page::SessionOptions);else if(chosen==2 && services.stop){close(c,0);if(in_pause)call(c,0x800F9AAC,gui,0);services.stop();}else if(chosen==3 && services.spectating && services.set_spectating && !(services.spectator_locked && services.spectator_locked())){services.set_spectating(!services.spectating());close(c,0);resume(c);}break;
    case Page::Rooms:{
        if(chosen==3){const auto rooms=frontend_rooms();if(rooms.size()>3){room_offset=room_offset+3<rooms.size()?room_offset+3:0;invalidate_labels();}else show(c,rooms_return);}
        else if(selected_room){
            retry_room=*selected_room;last_host=false;last_invitation=false;
            const auto current=selected_lan_room(*selected_room,services.rooms?services.rooms():std::vector<FrontendRoom>{},lan_clock_ms());
            if(!current){error="lan-expired";show(c,Page::Error);}
            else if(!lan_room_has_human_slot(*current)){error="full";show(c,Page::Error);}
            else if(valid_lan_secret(current->session_id) && valid_lan_secret(current->join_ticket) && current->join_expires_ms>lan_clock_ms() && services.join_lan)join_lan(c,*current);
            else{endpoint.address=current->address;endpoint.port=current->port;direct_return=Page::Rooms;show(c,Page::Direct);focus=1;call(c,0x800FA5BC,item(c,first+focus));edit(Edit::Pin);}
        }break;
    }
    default:break;
    }
    if(visible && gui)update(c);
}
std::vector<uint8_t> save_native_frontend_state(){
    checkpoint::Writer w;w.u32(0x4e464533);w.u32(registry.heap);w.u32(registry.memory);w.u32(registry.definitions);w.u32(registry.count);
    w.u32(uint32_t(storage.size()));for(const auto& [root,v]:storage){w.u32(root);w.u32(v.heap);w.u32(v.memory);w.u32(v.extra);}
    w.u32(gui);w.u32(unsigned(page));w.flag(in_pause);w.flag(local_mode);w.flag(setup_seen);w.flag(native_lobby_survival());return w.finish();
}
bool restore_native_frontend_state(const std::vector<uint8_t>& bytes,std::string& why,bool apply){
    try{
        checkpoint::Reader r(bytes,4096);unsigned field=0;auto require=[&](bool v){++field;if(!v)throw std::runtime_error("Invalid native menu checkpoint field "+std::to_string(field));};
        auto pointer=[](uint32_t p,unsigned size){return p>=0x80000000u && p<=ssne::guest_ram_end()-size;};
        const auto format=r.u32();require(format==0x4e464533);Registry next{r.u32(),r.u32(),r.u32(),r.u32()};const auto count=r.u32();require(count<=16);
        require(next.memory?((next.heap<32||pointer(next.heap,4))&&pointer(next.memory,8192)&&pointer(next.definitions,group*4)&&next.count==group):(!next.heap&&!next.definitions&&!next.count));
        if(next.memory){require(mem_read16(next.memory+2)==group);for(unsigned i=0;i<9;++i)require(mem_read32(next.memory+4096+(group+i)*4)==next.memory+i*96);}
        std::map<uint32_t,Storage> roots;for(unsigned i=0;i<count;++i){const auto root=r.u32();Storage v{r.u32(),r.u32(),format==0x4e464533?r.u32():0};require(!v.extra || pointer(v.extra,8192));require(pointer(root,96)&&(v.heap<32||pointer(v.heap,4))&&pointer(v.memory,4096)&&roots.emplace(root,v).second);}
        require(bool(count)==bool(next.memory));const auto root=r.u32(),p=r.u32();const bool pause=r.flag(),local=r.flag(),seen=r.flag(),survival=format==0x4e464533?r.flag():false;r.done();
        require((!root||pointer(root,96))&&p<=unsigned(Page::Direct));require(p==0||roots.contains(root));
        if(apply){for(const auto& [owner,stored]:storage){(void)stored;online_setup_native::forget(owner);}setup_restore_skin=p==unsigned(Page::OnlineSetup);native_frontend_custom_map_progress({});waiting_for_host_world=false;transfer_lobby_gui=0;transfer_pause_gui=0;transfer_pause_slot=-1;set_native_lobby_survival(survival);++mouse_epoch;result_overlay=false;retry_room.reset();reset_room_view();handoff_release=false;registry=next;storage.swap(roots);gui=root;page=Page(p);bots_return=page==Page::Bots?Page::SessionOptions:Page::Closed;options_return=active()?Page::Connection:Page::Home;in_pause=pause;local_mode=local;setup_seen=seen;visible=page!=Page::Closed;editing=Edit::None;input=0;hotkey=0;action=99;wait_release=true;invalidate_labels();std::lock_guard lock(text_mutex);text_events.clear();}
        return true;
    }catch(const std::exception& e){why=e.what();return false;}
}
}


