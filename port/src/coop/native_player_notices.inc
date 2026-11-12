
#include "native_netricsa_notices.inc"

// Native player presentation omitted by the replica's actor/world tick.
// Popup flags preserve native style0..5 and the numeric continuation bit3.
// Only logical text IDs, the authored image name, and final popup text travel.
struct PlayerNoticeCursor {uint64_t epoch=0,popup=0;uint32_t actor=0;std::string intro;};
std::array<PlayerNoticeCursor,8> player_notice_cursors;
bool capture_native_player_notices(uint32_t actor,EntityState& e,std::string& error){
    const auto popup=ssne::native_pickup_notice(actor);
    e.properties["pickup_notice_sequence"]=int64_t(popup.sequence);e.properties["pickup_notice_flags"]=int64_t(popup.flags);e.properties["pickup_notice_text"]=popup.text;
    std::vector<int64_t> intro;std::string image;
    const auto active=native_active_level();std::string wanted;
    if(active && active->size()>6){wanted="NETRISCA_"+active->substr(0,6)+"_"+active->substr(6)+"_INTRO";for(auto& c:wanted)c=char(std::toupper(static_cast<unsigned char>(c)));}
    // Level scripts address one player when queuing their introduction. The
    // campaign page is shared; each client's own information list is rebuilt
    // below, even when its host-side actor never received that script event.
    for(unsigned candidate=0;candidate<=ssne::native_party_capacity() && image.empty();++candidate){
    const auto source=candidate==0?actor:mem_read32(ssne::native_party_registry()+(candidate-1)*4);
    const auto info=readable(source,0x790)?mem_read32(source+0x720):0;
    if(readable(info,0x98) && mem_read32(info+0x48)==0x80219030u){
        std::set<uint32_t> seen;auto page=mem_read32(info+0x4C);
        while(page){if(!readable(page,0x3C) || !seen.insert(page).second || seen.size()>1024){error="Invalid Netricsa page list";return false;}
            if(mem_read32(page+0x38)==0x80219300u){const auto name=mem_read32(page+0x20);std::string value;
                if(readable(name,96)){unsigned n=0;for(;n<96 && mem_read8(name+n);++n)value+=char(mem_read8(name+n));auto match=value;for(auto& c:match)c=char(std::toupper(static_cast<unsigned char>(c)));if(n<96 && match==wanted){intro={int64_t(mem_read32(page+0x10)),int64_t(mem_read32(page+0x14))};image=std::move(value);}}
            }page=mem_read32(page+8);
        }
    }}
    e.properties["level_intro_text_ids"]=std::move(intro);e.properties["level_intro_image"]=std::move(image);return capture_shared_netricsa(actor,e,error);
}
bool validate_native_player_notices(uint32_t actor,const EntityState& e,std::string& error){
    auto fail=[&](const char* text){error=text;return false;};
    if(e.kind!=EntityKind::Player || !readable(actor,0x790))return fail("Player notice targets another class");
    const auto seq=e.properties.find("pickup_notice_sequence"),flags=e.properties.find("pickup_notice_flags"),text=e.properties.find("pickup_notice_text"),ids=e.properties.find("level_intro_text_ids"),image=e.properties.find("level_intro_image");
    if(seq==e.properties.end() || flags==e.properties.end() || text==e.properties.end() || ids==e.properties.end() || image==e.properties.end())return fail("Incomplete player notices");
    const auto* n=std::get_if<int64_t>(&seq->second);const auto* f=std::get_if<int64_t>(&flags->second);const auto* t=std::get_if<std::string>(&text->second);const auto* v=std::get_if<std::vector<int64_t>>(&ids->second);const auto* name=std::get_if<std::string>(&image->second);
    if(!n || *n<0 || !f || *f<0 || (*f&~15) || (*f&7)>5 || !t || t->size()>127 || t->find('\0')!=std::string::npos || (!*n && !t->empty()) || !v || (v->size()!=0 && v->size()!=2) || !name || name->size()>95 || name->empty()!=v->empty())return fail("Invalid player notice schema");
    if(std::any_of(t->begin(),t->end(),[](unsigned char c){return (c<32 && c!='\n' && c!='\r' && c!='\t') || c==127;}))return fail("Invalid player notice display text");
    for(auto id:*v)if(id<=0 || id>65535)return fail("Invalid Netricsa text ID");
    if(!name->empty() && (!name->starts_with("NETRISCA_") || !name->ends_with("_INTRO") || std::any_of(name->begin(),name->end(),[](unsigned char c){return !(c=='_' || (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9'));})))return fail("Invalid Netricsa image resource name");
    std::vector<NetricsaPageDescription> catalog;if(!unpack_netricsa_pages(e,catalog,error))return false;
    const auto hud=mem_read32(actor+0x374),popup=readable(hud,0x38)?mem_read32(hud+0x34):0,info=mem_read32(actor+0x720);
    if(!readable(popup,0x10) || !readable(mem_read32(popup+0xC),0xC8) || !readable(info,0x98) || mem_read32(info+0x48)!=0x80219030u || mem_read32(info+0x90)!=actor)return fail("Native player notice bindings unavailable");
    std::set<uint32_t> pages;for(auto page=mem_read32(info+0x4C);page;page=mem_read32(page+8))if(!readable(page,0x3C) || !pages.insert(page).second || pages.size()>1024)return fail("Invalid local Netricsa page list");return true;
}
bool apply_native_player_notices(PPCContext& context,uint64_t epoch,uint32_t actor,const EntityState& e,std::string& error){
    if(!context_ok(context) || !validate_native_player_notices(actor,e,error))return false;
    const auto slot=mem_read32(actor+0x17C);if(slot>=8 || slot!=unsigned(ssne::netplay_local_player()))return true;
    auto& cursor=player_notice_cursors[slot];if(cursor.epoch!=epoch || cursor.actor!=actor)cursor={epoch,0,actor,{}};
    StackScratch scratch(context);auto invoke=[&](uint32_t fn,uint32_t r3,uint32_t r4,uint32_t r5=0,uint32_t r6=0){auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=r3;call.r[4]=r4;call.r[5]=r5;call.r[6]=r6;ssne_call(call,fn);};
    const auto sequence=uint64_t(std::get<int64_t>(e.properties.at("pickup_notice_sequence")));
    if(sequence>cursor.popup){cursor.popup=sequence;const auto& text=std::get<std::string>(e.properties.at("pickup_notice_text"));
        if(!text.empty()){const auto buffer=context.r[1]-0x100;for(unsigned i=0;i<=text.size();++i)mem_write8(buffer+i,i<text.size()?uint8_t(text[i]):0);const auto flags=uint32_t(std::get<int64_t>(e.properties.at("pickup_notice_flags")));invoke(0x800AF094,mem_read32(mem_read32(mem_read32(actor+0x374)+0x34)+0xC),buffer,flags&7,flags>>3);ssne::log_message("[coop-pickup-text-client] sequence="+std::to_string(sequence)+" slot="+std::to_string(slot)+" bytes="+std::to_string(text.size()));}
    }
    if(!apply_shared_netricsa(context,actor,e,error))return false;
    const auto& image=std::get<std::string>(e.properties.at("level_intro_image"));if(image.empty() || cursor.intro==image)return true;
    const auto info=mem_read32(actor+0x720);const auto& ids=std::get<std::vector<int64_t>>(e.properties.at("level_intro_text_ids"));
    bool exists=false;for(auto page=mem_read32(info+0x4C);page;page=mem_read32(page+8))if(mem_read32(page+0x38)==0x80219300u && mem_read32(page+0x14)==ids[1])exists=true;
    if(!exists){
        // Resolve the self-referencing authored script string in THIS loaded
        // level. Native entry19 retains this pointer until level destruction;
        // never give it a network pointer or a temporary stack string.
        uint32_t local_image=0;const auto* begin=ssne::g_ram+0x400000;const auto* end=ssne::g_ram+ssne::guest_ram_size();
        for(auto* p=begin;p<end;){p=std::search(p,end,image.begin(),image.end());if(p==end)break;const auto address=uint32_t(p-ssne::g_ram)+0x80000000u;
            if(readable(address,unsigned(image.size()+1)) && !mem_read8(address+unsigned(image.size())) && mem_read32(address-12)==address){local_image=address;break;}++p;
        }
        if(!local_image){error="Authored local Netricsa intro image not loaded";return false;}
        const auto event=context.r[1]-0x180;for(unsigned i=0;i<7;++i)mem_write32(event+i*4,0);
        mem_write32(event,19);mem_write32(event+8,local_image);mem_write32(event+12,uint32_t(ids[0]));mem_write32(event+16,uint32_t(ids[1]));
        // Queue the original entry without its immediate-show request; select
        // and open it once below, after it has been linked to the local list.
        invoke(0x80102CF0,info,event);
    }
    // An existing entry can precede weapon/enemy pages. Select this level's
    // exact page rather than whichever page the host last had selected.
    unsigned index=0;for(auto page=mem_read32(info+0x4C);page;page=mem_read32(page+8),++index)if(mem_read32(page+0x38)==0x80219300u && mem_read32(page+0x14)==ids[1]){mem_write32(info+0x60,index);break;}
    invoke(0x80101F4C,info,0);cursor.intro=image;
    ssne::log_message("[coop-client-level-netricsa] epoch="+std::to_string(epoch)+" slot="+std::to_string(slot)+" image="+image);return true;
}
