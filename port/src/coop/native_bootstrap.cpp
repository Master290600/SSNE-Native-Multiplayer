#include "native_bootstrap.h"
#include "ssne/native_party.h"
#include "ssne/game_addresses.h"
#include "ssne/recomp_hooks.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include "ssne/runtime.h"
namespace port::coop {
#include "native_credits.inc"
std::optional<std::string> native_active_level() {
    if(mem_read32(0x80000000u)!=0x47334250u)return std::nullopt;
    const auto level=mem_read32(0x802DACC0);
    if(level<0x80000000u || level>ssne::guest_ram_end()-0x2E0)return std::nullopt;
    // 80167D10 passes the CCString pointer stored at level +2DC to 80159F70.
    auto name=read_native_text(mem_read32(level+0x2DC),64);
    if(!name || name->empty() || std::any_of(name->begin(),name->end(),[](unsigned char c){return !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_');}))return std::nullopt;
    return name;
}
bool activate_native_world_link(PPCContext& context,uint32_t object,std::string& target,std::string& error,bool terminal) {
    auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;};
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(object,0x58) || mem_read32(object+0x48)!=0x8020A750u){error="Native WorldLink class unavailable";return false;}
    const auto config=mem_read32(object+0x54);
    if(!readable(config,10) || mem_read16(config+8)!=(terminal?1:0)){error=terminal?"Native WorldLink is not a terminal exit":"Native WorldLink is not a next-level exit";return false;}
    const auto name=mem_read32(config+4);target.clear();
    for(unsigned i=0;i<65;++i){if(!readable(name+i,1)){error="Invalid WorldLink target storage";return false;}auto c=mem_read8(name+i);if(!c)break;target+=char(c);}
    if(target.empty() || target.size()>64 || std::any_of(target.begin(),target.end(),[](unsigned char c){return !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_');})){error="Invalid WorldLink target";return false;}
    const auto* event=ssne_find_func(0x80086C20);
    if(!event || mem_read32(event->address)!=0x9421FFE0u || context.r[13]!=0x802E22A0 || !readable(context.r[1]-0x300,0x300)){error="Native WorldLink context unavailable";return false;}
    // Event 13 is the original script activation, including profile progress and
    // the native results flow. Never bypass it by setting scene/loading flags.
    std::vector<uint8_t> stack(0x300);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x300+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    const auto message=context.r[1]-0x60;
    for(unsigned i=0;i<0x40;i+=4)mem_write32(message+i,0);
    mem_write32(message,13);call.r[3]=object;call.r[4]=message;
    try{ssne_call(call,event->address);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x300+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x300+i,stack[i]);return true;
}
bool activate_native_cutscene(PPCContext& context,uint32_t object,std::string& error) {
    auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;};
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(object,0x16C) || mem_read32(object+0x48)!=0x80203078u ||
       !readable(mem_read32(object+0x54),0x24) || mem_read32(object+0x60) || mem_read32(object+0x5C)) {
        error="Native cutscene entity unavailable or already active";return false;
    }
    const auto* handler=ssne_find_func(0x80077F74);
    if(!handler || mem_read32(handler->address)!=0x9421FFE0u || context.r[13]!=0x802E22A0 || !readable(context.r[1]-0x300,0x300)) {
        error="Native cutscene event context unavailable";return false;
    }
    std::vector<uint8_t> stack(0x300);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x300+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    const auto message=context.r[1]-0x60;
    for(unsigned i=0;i<0x40;i+=4)mem_write32(message+i,0);
    mem_write32(message,13);call.r[3]=object;call.r[4]=message;
    try{ssne_call(call,handler->address);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x300+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x300+i,stack[i]);
    if(!mem_read32(object+0x5C)){error="Original cutscene handler did not queue activation";return false;}
    return true;
}
bool activate_native_music_changer(PPCContext& context,uint32_t object,std::string& error){
    auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;};
    const auto* action=ssne_find_func(0x8008123C);
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(object,0x58) || mem_read32(object+0x48)!=0x80206FF0u || !readable(mem_read32(object+0x54),8) || !action || mem_read32(action->address)!=0x9421FFE0u || context.r[13]!=0x802E22A0 || (context.r[1]<0x80001000u || context.r[1]>=ssne::guest_ram_end())){error="Native music changer context unavailable";return false;}
    std::vector<uint8_t> stack(0x400);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x400+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=object;
    try{ssne_call(call,action->address);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x400+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x400+i,stack[i]);return true;
}
bool activate_native_sound_holder(PPCContext& context,uint32_t object,std::string& error){
    auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;};
    const auto* action=ssne_find_func(0x80083610);
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(object,0x6C) || mem_read32(object+0x48)!=0x802087A8u || !readable(mem_read32(object+0x54),0x24) || !action || mem_read32(action->address)!=0x9421FFD0u || context.r[13]!=0x802E22A0 || !readable(context.r[1]-0x600,0x600)){error="Native sound holder context unavailable";return false;}
    std::vector<uint8_t> stack(0x600);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x600+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=object;
    try{ssne_call(call,action->address);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x600+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x600+i,stack[i]);return true;
}
bool queue_native_offline_menu(PPCContext& context,std::string& error){
    const auto* quit=ssne_find_func(0x80167CD4u);
    if(mem_read32(0x80000000u)!=0x47334250u || !quit || mem_read32(quit->address)!=0x9421FFF8u || context.r[13]!=0x802E22A0u || (context.r[1]<0x80001000u || context.r[1]>=ssne::guest_ram_end())){error="Native menu return unavailable";return false;}
    // Exact Quit action from the original pause menu. It also resets the
    // frontend's scene state and audio; a presentation-only transition does not.
    std::vector<uint8_t> stack(0x400);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x400+i);
    auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    try{ssne_call(call,quit->address);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x400+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x400+i,stack[i]);return true;
}
bool queue_native_coop_presentation(PPCContext& context,uint32_t scene,std::string& error) {
    const auto* change=ssne_find_func(0x80155568);
    if((scene!=3 && scene!=5) || mem_read32(0x80000000u)!=0x47334250u || !change ||
       mem_read32(change->address)!=0x9421FFF8u || context.r[13]!=0x802E22A0u ||
       context.r[1]<0x80001000u || context.r[1]>=ssne::guest_ram_end()) {
        error="Native presentation transition context unavailable";return false;
    }
    std::vector<uint32_t> menus;std::set<uint32_t> visited;
    for(auto gui=mem_read32(0x8028BFA4);gui;gui=mem_read32(gui+8)) {
        if(gui<0x80000000u || gui>ssne::guest_ram_end()-0x90 || !visited.insert(gui).second || menus.size()>=256){error="Invalid native menu chain";return false;}
        if(mem_read32(gui+0x6C))menus.push_back(gui);
    }
    if(!ssne_find_func(0x800F6498) || !ssne_find_func(0x80155BC8)){error="Native menu handlers unavailable";return false;}
    std::vector<uint8_t> stack(0x200);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x200+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
    try {
        for(auto gui:menus){auto close=call;close.r[3]=gui;ssne_call(close,0x800F6498);}
        if(mem_read32(context.r[13]-0x6AAC)){auto resume=call;resume.r[3]=0;resume.r[4]=0;ssne_call(resume,0x80155BC8);}
        call.r[3]=scene;call.r[4]=1;ssne_call(call,change->address);
    }catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x200+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x200+i,stack[i]);return true;
}
bool queue_native_coop_level(PPCContext& context,const NativeBootstrapRequest& request,std::string& error) {
    if(mem_read32(0x80000000u)!=0x47334250u){error="Native bootstrap has not been verified for this game revision";return false;}
    if(request.level.empty() || request.level.size()>64 || std::any_of(request.level.begin(),request.level.end(),[](unsigned char c){return !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_');})){error="Invalid native level identifier";return false;}
    const auto* change=ssne_find_func(0x80155568);
    if(!change || mem_read32(change->address)!=0x9421FFF8u || context.r[13]!=0x802E22A0 || context.r[1]<0x80001000u || context.r[1]>=ssne::guest_ram_end()){error="Native scene transition context unavailable";return false;}
    std::vector<uint32_t> menus;std::set<uint32_t> visited;
    if(mem_read32(ssne::game_address(0x802DB0E0))==3) {
        auto gui=mem_read32(0x8028BFA4);
        while(gui) {
            if(gui<0x80000000 || gui>ssne::guest_ram_end()-0x90 || !visited.insert(gui).second || menus.size()>=256){error="Invalid native menu chain";return false;}
            if(mem_read32(gui+0x6C))menus.push_back(gui);gui=mem_read32(gui+8);
        }
        if(!ssne_find_func(0x800F6498)){error="Native menu close function unavailable";return false;}
        if(!ssne_find_func(0x80155BC8) || mem_read32(0x80155BC8)!=0x9421FFF8u){error="Native menu resume function unavailable";return false;}
    }
    const auto rules=native_rules_prefix(request.mode);if(!rules){error="Invalid native multiplayer mode";return false;}
    if(request.mode!=1)ssne::native_party_configure(4);
    auto setup=request.game_setup;setup[std::string(rules)+".Level"]=request.level;
    if(!write_native_configuration(context,native_global_configuration(),setup,"GameSetup",error))return false;
    // Native game initialization reads the selected mode and its rules, then
    // opens the Level string through 801674F8. Scene 4 is the actual game scene.
    mem_write32(ssne::game_address(0x802DB1BC),request.mode);
    std::vector<uint8_t> stack(0x200);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x200+i);
    PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=4;call.r[4]=1;
    try{
        // Scene transition waits until every native GUI's active-element list
        // is empty. Use the game's actual close routine; do not bypass the gate.
        for(auto gui:menus){PPCContext close=call;close.r[3]=gui;ssne_call(close,0x800F6498);}
        // Additional profile menus pause the scene manager. Their element-list
        // close alone leaves it paused, preventing every game/world update.
        // Finish the native menu exit through the game's own resume handler.
        if(mem_read32(context.r[13]-0x6AAC)) {
            PPCContext resume=call;resume.r[3]=0;resume.r[4]=0;ssne_call(resume,0x80155BC8);
        }
        ssne_call(call,0x80155568);
    }catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x200+i,stack[i]);throw;}
    for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x200+i,stack[i]);return true;
}
}
