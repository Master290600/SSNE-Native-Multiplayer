#include "native_frontend.h"
#include "checkpoint_codec.h"
#include "native_lobby.h"
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
namespace port::coop { namespace {
constexpr unsigned group=0x148,title=group+1,first=group+2,description=group+6;
constexpr unsigned result_connection=0x151,lobby_start=0x152,lobby_connection=0x153,lobby_bots=0x154,options_button=0x155;
enum class Page {Closed,Home,Host,Join,Connecting,Error,Bots,Connection,Rooms,Options};
enum class Edit {None,Address,Pin,Port,Name};
NativeFrontendServices services;
MultiplayerOptions options;Page options_return=Page::Home;bool options_save_failed=false;
void save_options(){
    if(services.options_path.empty())return;
    std::ofstream out(services.options_path,std::ios::binary|std::ios::trunc);
    out<<"SSNE-MP-1\n"<<std::quoted(options.name)<<' '<<options.enemies<<' '<<options.capacity<<' '<<options.pickups<<'\n';out.close();
    options_save_failed=!out;if(options_save_failed)ssne::log_message("[native-network-options] save failed");
}

PPCFunc open_original=nullptr,select_original=nullptr,destroy_original=nullptr,language_original=nullptr,scene_original=nullptr,pause_original=nullptr;
struct Storage {uint32_t heap=0,memory=0;};
struct Registry {uint32_t heap=0,memory=0,definitions=0,count=0;} registry;
std::map<uint32_t,Storage> storage;
uint32_t gui=0;Page page=Page::Closed;std::atomic<bool> visible{false};
bool has_options_button(){return page==Page::Home || page==Page::Host || page==Page::Join || page==Page::Connection || page==Page::Bots || page==Page::Options;}
std::atomic<Edit> editing{Edit::None};std::atomic<uint64_t> input{0};
std::atomic<unsigned> hotkey{0};unsigned action=99,skill=2,cursor=0;bool changing=false,local_mode=false,setup_seen=false,wait_release=false,notify_error=false,last_host=false,host_pin_initialized=false;
uint64_t mouse_epoch=1;
unsigned focus=0,room_offset=0,main_idle_frames=0;bool replace_all=false;
FrontendEndpoint endpoint;std::optional<FrontendRoom> retry_room;std::string edit_buffer,error;
std::mutex text_mutex;std::deque<std::pair<unsigned,std::string>> text_events;
std::array<std::string,8> labels{};
void invalidate_labels(){labels.fill("\1");}
bool in_pause=false,engine_scope=false,result_overlay=false;
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
bool active(){return services.active && services.active();}
bool host(){return services.host && services.host();}
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
void mouse_footer(PPCContext& c,uint32_t owner,unsigned root,unsigned id,unsigned offset,float x,float y,FrontText label,bool shown){
    const auto saved_gui=gui;const bool saved_pause=in_pause;gui=owner;in_pause=false;
    // Children need descriptors/text storage, never an unopened custom root.
    // A factory-created 148 without Open/Close remains linked and blocks loads.
    if(!prepare(c,false)){gui=saved_gui;in_pause=saved_pause;return;}
    const auto found=storage.find(gui);const auto parent=item(c,root);
    if(found!=storage.end() && parent){
        const auto desc=found->second.memory+offset;auto node=item(c,id);const bool created=!node;
        if(!node){
            for(unsigned k=0;k<88;++k)mem_write8(desc+k,mem_read8(registry.memory+7*96+k));
            mem_write16(desc+2,id);mem_write16(desc+4,root);mem_write16(desc+6,0);mem_write16(desc+8,0);mem_write32(desc+0x44,0);mem_write32(desc+0x48,0);
            mem_write32(desc+0x2c,f2u(x));mem_write32(desc+0x30,f2u(y));mem_write32(desc+0x54,f2u(.6f));
            node=call(c,0x800F5940,desc,gui,parent);
            if(node){bind(c,id,22);if(id==options_button)bind(c,id,12);}
        }
        if(node){
            const bool was_hidden=(mem_read32(desc+0xc)&0x30u)!=0;
            mem_write32(desc+0xc,(mem_read32(desc+0xc)&~0x30u)|(shown?0:0x30u));
            if(shown){
                const std::string value=tr(label);const auto scratch=found->second.memory+3584+offset;
                bool changed=created;for(unsigned i=0;i<=value.size() && i<90;++i)changed|=mem_read8(scratch+i)!=(i<value.size()?uint8_t(value[i]):0);
                if(changed){for(unsigned i=0;i<=value.size() && i<90;++i)mem_write8(scratch+i,i<value.size()?uint8_t(value[i]):0);const auto vt=mem_read32(node+0x5c);call(c,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),scratch);}
                if(created || was_hidden)call(c,0x800FA3CC,node);
            }
        }
    }
    gui=saved_gui;in_pause=saved_pause;
}
void prepare_mouse_footers(PPCContext& c){
    if(!active())return;
    if(ssne::native_results_presenting()){
        const auto owner=ssne::native_results_menu();if(owner)mouse_footer(c,owner,0xBC,result_connection,0,210.f,205.f,FrontText::Connection,true);
    }
    else if(const auto owner=native_lobby_mouse_main()){
        mouse_footer(c,owner,0x9A,lobby_start,96,-200.f,210.f,FrontText::Start,host());
        mouse_footer(c,owner,0x9A,lobby_connection,192,0.f,210.f,FrontText::Connection,true);
        mouse_footer(c,owner,0x9A,lobby_bots,288,200.f,210.f,FrontText::Bots,host());
        const auto saved_gui=gui;gui=owner;if(const auto old=item(c,0x7ffe)){const auto d=mem_read32(old+0x1c);mem_write32(d+0xc,mem_read32(d+0xc)|0x20u);}gui=saved_gui;
    }
}
void show(PPCContext& c,Page next){
    if(!prepare(c))return;
    ++mouse_epoch;
    // A result GUI already owns its local player's native open-mask bit.
    // Rebinding it to port zero would make CloseMenu clear the wrong bit.
    if(!result_overlay)call(c,0x800F56E0,gui,in_pause?unsigned(std::max(0,ssne::netplay_local_player())):0u);
    changing=true;
    if(result_overlay){if(result_group_definition)mem_write32(result_group_definition+0xc,(mem_read32(result_group_definition+0xc)&~0x30u)|0x40u);}
    else call(c,0x800F6178,gui,group);
    page=next;invalidate_labels();visible=true;editing=Edit::None;
    focus=next==Page::Connecting?3:0;call(c,0x800F60A0,gui,group,0);call(c,0x800FA5BC,item(c,first+focus));changing=false;wait_release=true;
    ssne::log_message("[native-network-menu] page="+std::to_string(unsigned(page))+" language="+std::to_string(native_menu_language()));
}
void close(PPCContext& c,unsigned target){
    ++mouse_epoch;
    if(result_overlay && !target){retire_result_overlay(c,true);return;}
    if(!target && active() && (page==Page::Bots || page==Page::Connection))handoff_release=true;
    changing=true;call(c,0x800F6178,gui,group);editing=Edit::None;visible=false;page=Page::Closed;
    // Opening main can restore the previous Multiplayer child. Suppress our
    // entry hook until native Back has actually unwound to the main root.
    if(target==0x3d){return_main_requested=true;next_back={};}
    if(target)call(c,0x800F60A0,gui,target,0);changing=false;
}
void select(PPCContext& c){
    const unsigned id=mem_read16(c.r[4]+0x18),event=mem_read32(c.r[4]+0x14);
    if(id==group){if(event==23 && !changing && visible){visible=false;editing=Edit::None;action=98;}return;}
    if(id==group+7 || id==group+8 || id==result_connection || id==lobby_connection || id==lobby_bots || id==options_button){if(event==12 && id==options_button)focus=4;if(event==22)hotkey=id==options_button?3:(id==group+8 || id==lobby_bots)?2:1;return;}
    if(id>=first && id<first+4){if(event==12)focus=id-first;if(event==22 && editing==Edit::None && !wait_release)action=id-first;return;}
    ssne_call_original(c,0x800FF15C,select_original);
}
void open(PPCContext& c){
    const auto root=c.r[3],id=c.r[4];if(id==0x3d){in_pause=false;local_mode=false;gui=root;}
    ssne_call_original(c,0x800F60A0,open_original);
    if(id==0xA7 && !active() && !local_mode && !return_main_requested){gui=root;call(c,0x800F6178,gui,0xA7);show(c,Page::Home);}
    else if(id==0xA7 && active())setup_seen=true;

}
void pause_open(PPCContext& c){
    const auto root=c.r[3];
    if(c.r[4]==0x129 && active() && mem_read32(0x802DB7E8)==4){
        const auto slot=ssne::netplay_local_player();const auto actor=slot>=0 && slot<8?mem_read32(ssne::native_party_registry()+slot*4):0;
        if(actor>=0x80000000u && actor<ssne::guest_ram_end()-0x790u && mem_read32(actor+0x71c)==root){
            gui=root;in_pause=true;prepare(c,false);
        }
    }
    ssne_call_original(c,0x800F9978,pause_original);
    if(in_pause && gui==root && storage.contains(root)){
        // Pause resources are created lazily by the original open call.
        prepare_pause(c);labels[6]=labels[7]="\1";text(c,group+7,tr(FrontText::Connection),6);text(c,group+8,host()?tr(FrontText::Bots):"",7);
        if(const auto node=item(c,group+7))call(c,0x800FA3CC,node);
        if(const auto node=item(c,group+8)){if(host())call(c,0x800FA3CC,node);else{const auto d=mem_read32(node+0x1c);mem_write32(d+0xc,mem_read32(d+0xc)|0x30u);}}
    }
}
void destroy(PPCContext& c){
    const auto root=c.r[3];auto found=storage.find(root);const auto owned=found==storage.end()?Storage{}:found->second;
    if(gui==root){gui=0;in_pause=false;result_overlay=false;setup_seen=false;main_idle_frames=0;visible=false;editing=Edit::None;page=Page::Closed;invalidate_labels();}
    ssne_call_original(c,0x800F55B4,destroy_original);
    if(owned.memory){storage.erase(root);call(c,0x801161EC,owned.heap,owned.memory);}
    if(storage.empty() && registry.memory){mem_write32(c.r[13]-0x542c,registry.definitions);mem_write32(c.r[13]-0x5428,registry.count);const auto old=registry;registry={};call(c,0x801161EC,old.heap,old.memory);}
}
void language(PPCContext& c){
    if(getenv("SSNE_TEST_HIDDEN"))if(const auto value=getenv("SSNE_NATIVE_TEST_LANGUAGE")){const auto n=std::strtol(value,nullptr,10);if(n>=0 && n<3)c.r[3]=uint32_t(n);}
    ssne_call_original(c,0x801597BC,language_original);
}
void scene(PPCContext& c){const auto results=ssne::native_results();if(!engine_scope && active() && c.r[3]==3 && mem_read32(0x802DB7E8)==4 && paused && (!results || !results->active))action=97;ssne_call_original(c,0x80155568,scene_original);}
void resume(PPCContext& c){if(ssne::native_results_presenting() && gui==ssne::native_results_menu()){retire_result_overlay(c,true);return;}if(in_pause){call(c,0x800F9978,gui,0x129,0x134,c.r[13]-0x53a0);}else resume_native_lobby(c);}
void edit(Edit field){editing=field;edit_buffer=field==Edit::Name?options.name:field==Edit::Address?endpoint.address:field==Edit::Pin?endpoint.pin:std::to_string(endpoint.port);cursor=0;replace_all=true;}
void accept_edit(PPCContext& c){
    const auto field=editing.load();if(field==Edit::Name){auto changed=options;changed.name=edit_buffer;if(valid_multiplayer_options(changed)){options=std::move(changed);save_options();}else return;}if(field==Edit::Address && !edit_buffer.empty())endpoint.address=edit_buffer;if(field==Edit::Pin)endpoint.pin=edit_buffer;
    if(field==Edit::Port){try{endpoint.port=unsigned(std::stoul(edit_buffer));}catch(...){endpoint.port=0;}}editing=Edit::None;wait_release=true;
    if(page==Page::Join && field==Edit::Pin){focus=2;call(c,0x800FA5BC,item(c,first+focus));}
}
std::string field(Edit type,const std::string& value){if(editing!=type)return value;auto out=edit_buffer;out.insert(std::min<size_t>(cursor,out.size()),"_");return out;}
FrontText failure(){if(error=="lan-expired")return FrontText::LanExpired;if(error=="invalid-address")return FrontText::InvalidAddress;if(error=="invalid-pin")return FrontText::InvalidPin;if(error=="invalid-port")return FrontText::InvalidPort;if(error=="lost")return FrontText::Lost;if(error.find("differs")!=std::string::npos || error.find("PIN")!=std::string::npos)return FrontText::Version;if(error.find("full")!=std::string::npos)return FrontText::Full;if(error=="invalid")return FrontText::Invalid;return FrontText::Failed;}
void update(PPCContext& c){
    std::array<std::string,4> buttons;FrontText heading=FrontText::Multiplayer;std::string hint;
    switch(page){
    case Page::Home:buttons={tr(FrontText::Host),tr(FrontText::Join),tr(FrontText::Local),tr(FrontText::Back)};hint=tr(FrontText::HostHelp);break;
    case Page::Host:heading=FrontText::Host;buttons={tr(FrontText::Start),std::string(tr(FrontText::Pin))+": "+field(Edit::Pin,endpoint.pin),std::string(tr(FrontText::Port))+": "+field(Edit::Port,std::to_string(endpoint.port)),tr(FrontText::Back)};hint=tr(FrontText::HostHelp);break;
    case Page::Join:heading=FrontText::Join;buttons={std::string(tr(FrontText::Address))+": "+field(Edit::Address,endpoint.address),std::string(tr(FrontText::Pin))+": "+field(Edit::Pin,endpoint.pin),tr(FrontText::Join),tr(FrontText::Search)};hint=tr(FrontText::JoinHelp);break;
    case Page::Connecting:heading=FrontText::Connection;buttons={tr(FrontText::Connecting),"","",tr(FrontText::Cancel)};hint=endpoint.address+":"+std::to_string(endpoint.port);break;
    case Page::Error:heading=FrontText::Connection;buttons={tr(FrontText::Retry),tr(FrontText::Address),tr(FrontText::Pin),tr(FrontText::Back)};hint=tr(failure());break;
    case Page::Bots:{heading=FrontText::Bots;const auto count=services.bot?services.bot(0,skill):0;buttons={std::string(tr(FrontText::AddBot))+" ("+std::to_string(count)+")",std::string(tr(FrontText::Skill))+": "+tr(skill==1?FrontText::Easy:skill==2?FrontText::Normal:FrontText::Hard),tr(FrontText::RemoveBot),tr(FrontText::Back)};hint=tr(FrontText::BotHelp);break;}
    case Page::Connection:heading=FrontText::Connection;buttons={tr(FrontText::Back),host()?tr(FrontText::Bots):"",tr(FrontText::Leave),services.afk && services.set_afk?tr(services.afk()?FrontText::AfkLeave:FrontText::AfkEnter):""};hint=services.status?services.status():"";break;
    case Page::Options:{
        heading=FrontText::Options;const auto current=active() && !host() && services.shared_options?services.shared_options():options;
        const auto cap=current.capacity;const auto capacity=cap==2?tr(FrontText::Unlimited):cap==0?"x1":("x"+std::to_string(cap==1?2u:cap));
        buttons={std::string(tr(FrontText::PlayerName))+": "+field(Edit::Name,options.name.empty()?tr(FrontText::ProfileName):options.name),std::string(tr(FrontText::Enemies))+": x"+std::to_string(current.enemies),std::string(tr(FrontText::AmmoCapacity))+": "+capacity,std::string(tr(FrontText::AmmoPickups))+": x"+std::to_string(current.pickups)};
        hint=tr(options_save_failed?FrontText::SaveFailed:active() && focus==0?FrontText::NameLocked:active() && !host()?FrontText::ClientRules:FrontText::RulesHelp);break;
    }
    case Page::Rooms:{heading=FrontText::Search;auto rooms=services.rooms?services.rooms():std::vector<FrontendRoom>{};for(unsigned i=0;i<3;++i)buttons[i]=room_offset+i<rooms.size()?rooms[room_offset+i].name+" ("+std::to_string(rooms[room_offset+i].players)+"/"+std::to_string(rooms[room_offset+i].capacity)+")":"";buttons[3]=tr(rooms.size()>3?FrontText::More:FrontText::Back);hint=services.scanning && services.scanning()?tr(FrontText::Searching):rooms.empty()?tr(FrontText::None):tr(FrontText::RoomsHelp);break;}
    default:return;
    }
    mouse_footer(c,gui,group,options_button,384,0.f,205.f,page==Page::Options?FrontText::Back:FrontText::Options,has_options_button());
    const bool first_room_arrived=page==Page::Rooms && labels[1].empty() && !buttons[0].empty();
    const bool room_focus_missing=page==Page::Rooms && buttons[focus].empty();
    if(editing!=Edit::None)hint=tr(editing==Edit::Name?FrontText::NameTyping:FrontText::Typing);text(c,title,tr(heading),0);for(unsigned i=0;i<4;++i){text(c,first+i,buttons[i],i+1);if(const auto node=item(c,first+i)){const auto d=mem_read32(node+0x1c);const auto flags=mem_read32(d+0xc);mem_write32(d+0xc,(flags&~0x30u)|(buttons[i].empty()?0x30u:((page==Page::Connecting && i!=3) || (page==Page::Options && active() && (i==0 || !host())))?0x10u:0u));}}text(c,description,hint,5);
    // Search results arrive after OpenPanel chose Back from the empty list.
    // Move focus only when a row first becomes available or disappears;
    // metadata refreshes must preserve the user's current native selection.
    if(first_room_arrived || room_focus_missing){focus=first_room_arrived?0u:3u;call(c,0x800FA5BC,item(c,first+focus));}
}
void start(PPCContext& c,bool hosting){
    last_host=hosting;retry_room.reset();
    auto address=endpoint;if(!hosting){const auto colon=address.address.rfind(':');if(colon!=std::string::npos){try{address.port=unsigned(std::stoul(address.address.substr(colon+1)));}catch(...){address.port=0;}address.address.resize(colon);}}
    if(const auto invalid=frontend_endpoint_error(address);invalid!=FrontendEndpointError::None){error=invalid==FrontendEndpointError::Address?"invalid-address":invalid==FrontendEndpointError::Pin?"invalid-pin":"invalid-port";ssne::log_message("[native-network-menu] validation failed: "+error);show(c,Page::Error);return;}
    error=services.start?services.start(hosting,address):"unavailable";if(!error.empty()){ssne::log_message("[native-network-menu] start failed: "+error);show(c,Page::Error);return;}setup_seen=false;main_idle_frames=0;if(hosting)close(c,0);else show(c,Page::Connecting);
}
void join_lan(PPCContext& c,const FrontendRoom& room){
    retry_room=room;last_host=false;endpoint.address=room.address;endpoint.port=room.port;
    error=services.join_lan?services.join_lan(room):"unavailable";
    if(!error.empty()){ssne::log_message("[native-network-menu] LAN join failed: "+error);show(c,Page::Error);return;}
    setup_seen=false;main_idle_frames=0;show(c,Page::Connecting);
}
}
unsigned native_menu_language(){const auto language=mem_read32(0x802DB838);return language<3?language:0;}
MultiplayerOptions native_frontend_options(){return options;}
void configure_native_frontend(NativeFrontendServices value){services=std::move(value);
    options={};if(!services.options_path.empty()){
        std::ifstream in(services.options_path,std::ios::binary);std::string magic;MultiplayerOptions saved;
        std::error_code ec;const auto size=std::filesystem::file_size(services.options_path,ec);
        if(in && !ec && size<=4096 && in.peek()!=EOF){std::getline(in,magic);if(magic=="SSNE-MP-1" && (in>>std::quoted(saved.name)>>saved.enemies>>saved.capacity>>saved.pickups) && valid_multiplayer_options(saved))options=std::move(saved);}
    }
if(getenv("SSNE_TEST_HIDDEN"))if(const auto port=getenv("SSNE_NATIVE_MENU_PORT")){const auto n=std::strtol(port,nullptr,10);if(n>0 && n<=65535)endpoint.port=unsigned(n);}}
void install_native_frontend(){
    for(auto [address,expected]:{std::pair{0x800F60A0u,0xae6ce28a14da5c62ull},std::pair{0x800FF15Cu,0xda46ee353752077cull},std::pair{0x800F55B4u,0x53f0f80b625ba833ull},std::pair{0x801597BCu,0x5ca15657a7fd49e3ull},std::pair{0x80155568u,0xa927cf7d3a0c0ea0ull},std::pair{0x800F9978u,0x850e4e0004bd9a48ull}}){uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<32;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}if(!ssne_find_func(address) || hash!=expected)throw std::runtime_error("Original network menu revision differs");}
    open_original=ssne_find_func(0x800F60A0)->func;ssne_install_hook(0x800F60A0,open);select_original=ssne_find_func(0x800FF15C)->func;ssne_install_hook(0x800FF15C,select);destroy_original=ssne_find_func(0x800F55B4)->func;ssne_install_hook(0x800F55B4,destroy);
    language_original=ssne_find_func(0x801597BC)->func;ssne_install_hook(0x801597BC,language);
    pause_original=ssne_find_func(0x800F9978)->func;ssne_install_hook(0x800F9978,pause_open);
    scene_original=ssne_find_func(0x80155568)->func;ssne_install_hook(0x80155568,scene);
}
bool native_frontend_visible(){return visible.load();}
bool native_frontend_text_active(){return editing!=Edit::None;}
void native_frontend_return_main(){return_main_requested=true;next_back={};}
void native_frontend_disconnected(std::string){error="lost";notify_error=true;}
void native_frontend_engine_scope(bool enabled){engine_scope=enabled;}
bool native_frontend_text_input(const std::string& value){if(!native_frontend_text_active())return false;std::lock_guard lock(text_mutex);if(text_events.size()<128)text_events.emplace_back(0,value.substr(0,256));return true;}
bool native_frontend_edit_key(unsigned key){if(!native_frontend_text_active())return false;std::lock_guard lock(text_mutex);text_events.emplace_back(key,"");return true;}
bool native_frontend_input(const ssne::PadState& pad){
    input.store(uint64_t(pad.buttons)|(uint64_t(pad.stick_x)<<16)|(uint64_t(pad.stick_y)<<24));static uint16_t before=0;const auto pressed=pad.buttons&~before;before=pad.buttons;
    if(handoff_release){if(pad.buttons&(ssne::PAD_A|ssne::PAD_B|ssne::PAD_START))return true;handoff_release=false;}
    if(!visible && (native_lobby_visible() || paused || ssne::native_results_presenting()) && active()){if((pressed&ssne::PAD_X) || (!host() && native_lobby_visible() && (pressed&ssne::PAD_B))){hotkey=1;return true;}if(host() && (pressed&ssne::PAD_Y)){hotkey=2;return true;}}return visible.load() || return_main_requested.load();
}
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
        if(services.rooms)for(const auto& room:services.rooms()){
            for(unsigned char ch:room.session_id)value=(value^ch)*1099511628211ull;
            value=(value^room.port)*1099511628211ull;
        }
    }
    return value;
}
bool native_frontend_mouse_allowed(uint32_t owner,uint32_t,uint16_t root,uint16_t id,unsigned event){
    if(visible){
        if(owner!=gui || root!=group)return false;
        if(id>=first && id<first+4){
            if(labels[id-first+1].empty() || (page==Page::Connecting && id!=first+3))return false;
            if((page==Page::Options && active() && (id==first || !host())) || (page==Page::Bots && id!=first+3 && !host()) || (page==Page::Connection && id==first+1 && !host()))return false;
        }
        return true;
    }
    if(!active())return true;
    if(root==group)return false;
    if(ssne::native_results_presenting())return owner==ssne::native_results_menu() && (host() || id==result_connection);
    if(native_lobby_visible()){
        const auto who=native_lobby_mouse_owner(owner);
        if(who==-2)return id==lobby_connection || (host() && (id==lobby_start || id==lobby_bots));
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
    if(id==group+7 || id==group+8 || id==result_connection || id==lobby_connection || id==lobby_bots || id==options_button){
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
        if(editing!=Edit::None)accept_edit(c);
        focus=id-first;wait_release=false;action=focus;return true;
    }
    return false;
}
void native_frontend_prepare_results_close(PPCContext& c){retire_result_overlay(c,true);}
void native_frontend_frame(PPCContext& c){
    prepare_mouse_footers(c);
    // A replica follows the authority into the next level even if its local
    // connection panel was open. Retire that panel with the result screen.
    if(result_overlay && !ssne::native_results_presenting())retire_result_overlay(c,false);
    // Native Back restores the main root internally without calling open(3D).
    // Scope the local bypass to its setup flow so the next Multiplayer entry
    // can show Host/Join/Local again.
    if(local_mode && !active() && !visible){
        const auto root=native_root_id();
        if(root==0xa7)setup_seen=true;
        else if(root==0x3d && setup_seen){local_mode=false;setup_seen=false;}
    }
    if(return_main_requested && !active() && mem_read32(0x802DB7E8)==3){
        unsigned checked=0;
        for(auto root=mem_read32(0x8028BFA4);root && checked++<128;root=mem_read32(root+8)){
            if(root<0x80000000u || root>ssne::guest_ram_end()-0x90)break;
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
        if(getenv("SSNE_DEV_NATIVE_MENU")){const auto shared=services.shared_options?services.shared_options():options;std::ofstream out("native-menu-state.json.pending");out<<"{\"page\":"<<unsigned(page)<<",\"visible\":"<<visible<<",\"editing\":"<<unsigned(editing.load())<<",\"language\":"<<native_menu_language()<<",\"port\":"<<endpoint.port<<",\"pin\":\""<<endpoint.pin<<"\",\"address\":\""<<endpoint.address<<"\",\"active\":"<<active()<<",\"host\":"<<host()<<",\"scene\":"<<mem_read32(0x802DB7E8)<<",\"lobby\":"<<native_lobby_visible()<<",\"runtime_ready\":"<<ssne::netplay_world_ready()<<",\"local_slot\":"<<own<<",\"info\":"<<ssne::local_information_open()<<",\"movie\":"<<ssne::native_movie_presenting()<<",\"pause\":"<<paused<<",\"native_focus\":"<<native_focus_id()<<",\"native_root\":"<<native_root_id()<<",\"returning\":"<<return_main_requested<<",\"gui\":"<<gui<<",\"gui_port\":"<<(gui?int16_t(mem_read16(gui+0x84)):-2)<<",\"wait_release\":"<<wait_release<<",\"input\":"<<uint16_t(input.load())<<",\"focus\":"<<focus<<",\"skill\":"<<skill<<",\"enemy_factor\":"<<options.enemies<<",\"ammo_capacity\":"<<options.capacity<<",\"pickup_factor\":"<<options.pickups<<",\"shared_enemies\":"<<shared.enemies<<",\"shared_capacity\":"<<shared.capacity<<",\"shared_pickups\":"<<shared.pickups<<",\"name_bytes\":"<<options.name.size()<<",\"players\":"<<(services.players?services.players():0)<<",\"bots\":"<<(host() && services.bot?services.bot(0,skill):0)<<",\"rooms\":"<<(services.rooms?services.rooms().size():0)<<",\"scanning\":"<<(services.scanning && services.scanning())<<",\"room_focus_matches\":"<<(page!=Page::Rooms || native_focus_id()==first+focus)<<",\"results_root\":"<<(ssne::native_results_presenting()?native_root_id(ssne::native_results_menu()):65535u)<<"}";out.close();std::remove("native-menu-state.json");std::rename("native-menu-state.json.pending","native-menu-state.json");}
    }
    if(notify_error && !return_main_requested && !active() && gui && mem_read32(0x802DB7E8)==3){notify_error=false;call(c,0x800F60A0,gui,0x3f);show(c,Page::Error);}
    const auto requested=hotkey.exchange(0);
    if(requested==3 && visible && has_options_button()){
        if(editing!=Edit::None)accept_edit(c);
        if(page==Page::Options)show(c,options_return);else{options_return=page;show(c,Page::Options);}
    }
    else if(requested && active()){
        const auto results=ssne::native_results_presenting()?ssne::native_results_menu():0u;
        if(results){
            gui=results;in_pause=false;
            if(!prepare(c))return;
            const auto panel=item(c,0xBC),overlay=item(c,group);if(!panel || !overlay)return;
            result_definition=mem_read32(panel+0x1c);result_flags=mem_read32(result_definition+0xc);result_focus=mem_read32(gui+0x7c);
            result_group_definition=mem_read32(overlay+0x1c);result_group_flags=mem_read32(result_group_definition+0xc);result_overlay=true;
            mem_write32(result_definition+0xc,result_flags|0x30u);
            // Native focus switching preserves the previous root only when
            // the newly opened root carries its original overlay bit 0x40.
            mem_write32(result_group_definition+0xc,(result_group_flags&~0x30u)|0x40u);
        }
        else if(gui){if(in_pause)call(c,0x800F6178,gui,0x129);else suspend_native_lobby(c);}
        if(gui)show(c,requested==2?Page::Bots:Page::Connection);
    }
    if(page==Page::Connecting){if(active())close(c,0);else if(services.connecting && !services.connecting()){error=services.status?services.status():"failed";show(c,Page::Error);}}
    std::deque<std::pair<unsigned,std::string>> events;{std::lock_guard lock(text_mutex);events.swap(text_events);}
    for(auto& [key,value]:events)if(editing!=Edit::None){
        if(key==1)accept_edit(c);else if(key==2){editing=Edit::None;wait_release=true;}else if(key==5){replace_all=true;cursor=0;}else if((key==3 || key==4) && replace_all){edit_buffer.clear();cursor=0;replace_all=false;}else if(key==3 && cursor)edit_buffer.erase(--cursor,1);else if(key==4 && cursor<edit_buffer.size())edit_buffer.erase(cursor,1);
        else if(!key){if(replace_all && !value.empty()){edit_buffer.clear();cursor=0;replace_all=false;}for(unsigned char ch:(editing==Edit::Name?native_name_input(value):value)){const bool allowed=editing==Edit::Name?(ch>=32 && (ch<127 || ch>=160)):editing==Edit::Address?std::string("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:").find(char(ch))!=std::string::npos:ch>='0' && ch<='9';const auto limit=editing==Edit::Pin?6u:editing==Edit::Port?5u:editing==Edit::Name?24u:64u;if(allowed){if(cursor<edit_buffer.size())edit_buffer[cursor++]=char(ch);else if(edit_buffer.size()<limit){edit_buffer+=char(ch);++cursor;}}}}
    }
    static uint16_t before=0;const auto packed=input.load();auto pad=uint16_t(packed);const auto y=uint8_t(packed>>24);if(y>192)pad|=ssne::PAD_UP;else if(y<64)pad|=ssne::PAD_DOWN;const auto pressed=uint16_t(pad&~before);before=pad;
    // The transport owns controller routing. Feed its local menu edges into
    // the original widgets' focus/event functions, keeping game input neutral.
    if(visible && gui && editing==Edit::None && !wait_release){
        if(pressed&(ssne::PAD_UP|ssne::PAD_DOWN)){
            const int direction=pressed&ssne::PAD_UP?-1:1;
            const unsigned count=has_options_button()?5:4;
            for(unsigned i=0;i<count;++i){focus=unsigned(int(focus)+direction+int(count))%count;const auto node=item(c,focus==4?options_button:first+focus);if(node && (focus==4 || (!labels[focus+1].empty() && (page!=Page::Connecting || focus==3) && (page!=Page::Options || !active() || (focus!=0 && host()))))){if(focus==4){call(c,0x800F6B6C,gui);call(c,0x800F6A94,gui,node);}else call(c,0x800FA5BC,node);break;}}
        }else if(pressed&(ssne::PAD_A|ssne::PAD_START)){
            if(focus==4)hotkey=3;
            const auto node=item(c,focus==4?options_button:first+focus);if(node && (focus==4 || (!labels[focus+1].empty() && (page!=Page::Connecting || focus==3) && (page!=Page::Options || !active() || (focus!=0 && host())))))if(focus!=4)call(c,0x800FB130,node,22);
        }else if(pressed&ssne::PAD_B)action=98; // Route Back through the page transition, not just widget hiding.
    }
    if(editing!=Edit::None){
        if(pressed&ssne::PAD_B){editing=Edit::None;wait_release=true;}else if(pressed&(ssne::PAD_A|ssne::PAD_START))accept_edit(c);else if(pressed&ssne::PAD_LEFT){replace_all=false;if(cursor)--cursor;}else if(pressed&ssne::PAD_RIGHT){replace_all=false;cursor=std::min<unsigned>(cursor+1,edit_buffer.size());}
        else if(pressed&ssne::PAD_X){replace_all=false;if(cursor<edit_buffer.size())edit_buffer.erase(cursor,1);else if(cursor)edit_buffer.erase(--cursor,1);}
        else if(pressed&ssne::PAD_Y){replace_all=false;const auto limit=editing==Edit::Pin?6u:editing==Edit::Port?5u:editing==Edit::Name?24u:64u;if(edit_buffer.size()<limit){edit_buffer+='0';cursor=unsigned(edit_buffer.size()-1);}}
        else if(pressed&(ssne::PAD_UP|ssne::PAD_DOWN)){replace_all=false;if(cursor>=edit_buffer.size())edit_buffer+='0';auto& ch=edit_buffer[cursor];const std::string alphabet=editing==Edit::Address?"0123456789.-:abcdefghijklmnopqrstuvwxyz":editing==Edit::Name?" abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-":"0123456789";const auto at=alphabet.find(ch);ch=alphabet[(unsigned(at==std::string::npos?0:at)+(pressed&ssne::PAD_UP?1:alphabet.size()-1))%alphabet.size()];}
    }
    const auto chosen=std::exchange(action,99u);
    if(chosen==97){if(services.stop)services.stop();}else if(chosen==98){if(page==Page::Options)show(c,options_return);else if(page==Page::Connecting){if(services.stop)services.stop();show(c,Page::Join);}else if(page==Page::Rooms)show(c,Page::Join);else if(page!=Page::Home && !active())show(c,Page::Home);else if(active()){close(c,0);resume(c);}else if(gui)close(c,0x3d);}
    else if(chosen<4)switch(page){
    case Page::Options:
        if(chosen==0){if(!active())edit(Edit::Name);}
        else if(!active() || host()){
            if(chosen==1)options.enemies=options.enemies%5+1;
            else if(chosen==2)options.capacity=next_ammo_capacity(options.capacity);
            else options.pickups=options.pickups%5+1;
            save_options();if(active() && host() && services.set_rules)services.set_rules(options);
        }break;
    case Page::Home:if(chosen==0){if(!host_pin_initialized){endpoint.pin=std::to_string(100000+uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())%900000);host_pin_initialized=true;}show(c,Page::Host);}else if(chosen==1)show(c,Page::Join);else if(chosen==2){local_mode=true;setup_seen=false;close(c,0xA7);}else close(c,0x3d);break;
    case Page::Host:if(chosen==0)start(c,true);else if(chosen==1)edit(Edit::Pin);else if(chosen==2)edit(Edit::Port);else show(c,Page::Home);break;
    case Page::Join:if(chosen==0)edit(Edit::Address);else if(chosen==1)edit(Edit::Pin);else if(chosen==2)start(c,false);else{room_offset=0;if(services.scan)services.scan();show(c,Page::Rooms);}break;
    case Page::Connecting:if(chosen==3){if(services.stop)services.stop();show(c,Page::Join);}break;
    case Page::Error:if(chosen==0){if(retry_room){room_offset=0;if(services.scan)services.scan();show(c,Page::Rooms);}else start(c,last_host);}else if(chosen==1){show(c,Page::Join);edit(Edit::Address);}else if(chosen==2){show(c,Page::Join);focus=1;call(c,0x800FA5BC,item(c,first+focus));edit(Edit::Pin);}else show(c,Page::Home);break;
    case Page::Bots:if(chosen==0 && services.bot)services.bot(1,skill);else if(chosen==1)skill=skill%3+1;else if(chosen==2 && services.bot)services.bot(-1,skill);else if(chosen==3){close(c,0);resume(c);}break;
    case Page::Connection:if(chosen==0){close(c,0);resume(c);}else if(chosen==1 && host())show(c,Page::Bots);else if(chosen==2 && services.stop){close(c,0);if(in_pause)call(c,0x800F9AAC,gui,0);services.stop();}else if(chosen==3 && services.afk && services.set_afk){services.set_afk(!services.afk());wait_release=true;}break;
    case Page::Rooms:{const auto rooms=services.rooms?services.rooms():std::vector<FrontendRoom>{};if(chosen==3){if(rooms.size()>3){room_offset=room_offset+3<rooms.size()?room_offset+3:0;invalidate_labels();}else show(c,Page::Join);}else if(room_offset+chosen<rooms.size()){const auto& room=rooms[room_offset+chosen];if(valid_lan_secret(room.session_id) && services.join_lan)join_lan(c,room);else{endpoint.address=room.address+":"+std::to_string(room.port);show(c,Page::Join);focus=1;call(c,0x800FA5BC,item(c,first+focus));edit(Edit::Pin);}}break;}
    default:break;
    }
    if(visible && gui)update(c);
}
std::vector<uint8_t> save_native_frontend_state(){
    checkpoint::Writer w;w.u32(0x4e464531);w.u32(registry.heap);w.u32(registry.memory);w.u32(registry.definitions);w.u32(registry.count);
    w.u32(uint32_t(storage.size()));for(const auto& [root,v]:storage){w.u32(root);w.u32(v.heap);w.u32(v.memory);}
    w.u32(gui);w.u32(unsigned(page));w.flag(in_pause);w.flag(local_mode);w.flag(setup_seen);return w.finish();
}
bool restore_native_frontend_state(const std::vector<uint8_t>& bytes,std::string& why,bool apply){
    try{
        checkpoint::Reader r(bytes,4096);unsigned field=0;auto require=[&](bool v){++field;if(!v)throw std::runtime_error("Invalid native menu checkpoint field "+std::to_string(field));};
        auto pointer=[](uint32_t p,unsigned size){return p>=0x80000000u && p<=ssne::guest_ram_end()-size;};
        require(r.u32()==0x4e464531);Registry next{r.u32(),r.u32(),r.u32(),r.u32()};const auto count=r.u32();require(count<=16);
        require(next.memory?((next.heap<32||pointer(next.heap,4))&&pointer(next.memory,8192)&&pointer(next.definitions,group*4)&&next.count==group):(!next.heap&&!next.definitions&&!next.count));
        if(next.memory){require(mem_read16(next.memory+2)==group);for(unsigned i=0;i<9;++i)require(mem_read32(next.memory+4096+(group+i)*4)==next.memory+i*96);}
        std::map<uint32_t,Storage> roots;for(unsigned i=0;i<count;++i){const auto root=r.u32();Storage v{r.u32(),r.u32()};require(pointer(root,96)&&(v.heap<32||pointer(v.heap,4))&&pointer(v.memory,4096)&&roots.emplace(root,v).second);}
        require(bool(count)==bool(next.memory));const auto root=r.u32(),p=r.u32();const bool pause=r.flag(),local=r.flag(),seen=r.flag();r.done();
        require((!root||pointer(root,96))&&p<=unsigned(Page::Options));require(p==0||roots.contains(root));
        if(apply){++mouse_epoch;result_overlay=false;retry_room.reset();handoff_release=false;registry=next;storage.swap(roots);gui=root;page=Page(p);options_return=active()?Page::Connection:Page::Home;in_pause=pause;local_mode=local;setup_seen=seen;visible=page!=Page::Closed;editing=Edit::None;input=0;hotkey=0;action=99;wait_release=true;invalidate_labels();std::lock_guard lock(text_mutex);text_events.clear();}
        return true;
    }catch(const std::exception& e){why=e.what();return false;}
}
}
