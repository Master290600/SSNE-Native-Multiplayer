#include "native_lobby.h"
#include "native_configuration.h"
#include "native_bootstrap.h"
#include "native_frontend.h"
#include "ssne/input.h"
#include "ssne/recomp_hooks.h"
#include "ssne/native_party.h"
#include "ssne/runtime.h"
#include <array>
#include <atomic>
#include <stdexcept>
#include <map>
#include <set>
#include <string>
#include <fstream>
namespace port::coop { namespace {
AuthoritativeServer* server=nullptr;AuthoritativeClient* client=nullptr;
struct Panels {std::array<uint32_t,8> gui{};unsigned batch=0,count=8;bool opened=false;uint32_t main=0,text_storage=0,heap=0;std::array<std::string,8> labels{};};
std::map<uint32_t,Panels> panels;
std::map<uint32_t,uint32_t> known_rosters;
PPCFunc make_original=nullptr,open_original=nullptr,destroy_original=nullptr,close_original=nullptr,sprite_original=nullptr,sprite_update_original=nullptr,sprite_destroy_original=nullptr;
PPCFunc profiles_original=nullptr,layout_original=nullptr,update_original=nullptr;
bool installed=false;
std::set<uint32_t> controller_sprites;
std::atomic<bool> visible{false};
bool mode_chooser_opened=false;
LobbyLevelSelection level_selection;
LobbyConfirmRelease confirm_release;
std::array<std::string,8> model_keys{};
std::array<PlayerId,8> applied_skin_players{};
std::vector<int64_t> character_models;
std::map<int64_t,std::string> model_catalog;
bool prepare_characters(PPCContext& ctx){
    if(character_models.size()==8)return true;
    ssne::native_party_prepare_lobby(ctx);
    const auto profiles=read_native_configuration(native_global_configuration(),"PlayerProfiles");
    const auto catalog=read_native_configuration(native_global_configuration(),"MultiPlayerInfo");
    if(!profiles.error.empty() || !catalog.error.empty())return false;
    for(const auto& [key,value]:catalog.values){
        const std::string prefix="MultiPlayerInfo.Model";
        if(!key.starts_with(prefix) || !key.ends_with(".Name"))continue;
        const auto number=key.substr(prefix.size(),key.size()-prefix.size()-5);
        if(number.empty() || number.find_first_not_of("0123456789")!=std::string::npos)continue;
        if(const auto name=std::get_if<std::string>(&value);name && !name->empty())model_catalog.emplace(std::stoll(number),*name);
    }
    std::vector<int64_t> models;
    for(unsigned slot=0;slot<8;++slot){
        const auto profile=ssne::native_party_profile(slot);
        for(const auto& [key,value]:profiles.values){
            if(!key.ends_with(".ID") || value!=Property(int64_t(profile)))continue;
            const auto model_key=key.substr(0,key.size()-3)+".Model";
            const auto found=profiles.values.find(model_key);
            if(found!=profiles.values.end())if(const auto model=std::get_if<int64_t>(&found->second)){
                model_keys[slot]=model_key;models.push_back(*model);break;
            }
        }
        if(models.size()!=slot+1)return false;
    }
    if(model_catalog.empty())return false;
    character_models=std::move(models);return true;
}
PPCFunc level_confirm_original=nullptr;
PPCFunc level_close_original=nullptr,level_start_original=nullptr,difficulty_start_original=nullptr;
uint32_t invoke(PPCContext& base,uint32_t address,uint32_t a=0,uint32_t b=0,uint32_t c=0){auto call=base;call.r[1]-=0x100;call.msr&=~0x8000u;mem_write32(call.r[1],base.r[1]);call.r[3]=a;call.r[4]=b;call.r[5]=c;ssne_call(call,address);return call.r[3];}
uint32_t item(PPCContext& ctx,uint32_t gui,uint32_t id){return invoke(ctx,0x800F6030,gui,id);}
void hide(PPCContext& ctx,uint32_t gui,uint32_t id){
    auto node=item(ctx,gui,id);if(!node)return;
    auto d=mem_read32(node+0x1c);
    // Hide the logical group and its controller-number label. The artwork
    // itself is handled separately below because it has a separate primitive.
    if(id==0xa4 && (mem_read32(d+0xc)&4))invoke(ctx,0x800F6250,gui,id);
    mem_write32(d+0xc,mem_read32(d+0xc)|0x20);

}
// Retain native ownership/destruction, but give the PadGfx primitive no area.
// Repeat after animation updates so they cannot restore its drawing rectangle.
void suppress_controller(PPCContext& ctx,uint32_t sprite){
    const auto draw=mem_read32(sprite+8),vt=mem_read32(draw),scratch=ctx.r[1]-0x80;
    mem_write32(scratch,0);mem_write32(scratch+4,0);
    invoke(ctx,mem_read32(vt+0x34),draw+int16_t(mem_read16(vt+0x30)),scratch,scratch);
}
void sprite(PPCContext& ctx){
    std::string name;auto address=mem_read32(ctx.r[7]);
    if(address>=0x80000000u && address<=0x83ffff80u)for(unsigned i=0;i<128;++i){auto c=mem_read8(address+i);if(!c)break;name+=char(c);}
    const bool controller=name=="FrontendA:PadGfx";
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_CLIENT_UI")){
        auto readable=[](uint32_t p,unsigned n){return p>=0x80000000u && p<=ssne::guest_ram_end()-n;};
        const auto renderer=ctx.r[4],table=readable(renderer,4)?mem_read32(renderer):0,method=readable(table,0x60)?mem_read32(table+0x5C):0;
        ssne::log_message("[dev-ui-sprite] name="+name+" renderer="+std::to_string(renderer)+" table="+std::to_string(table)+" method="+std::to_string(method)+" caller="+std::to_string(ctx.lr));
        if(!ssne_find_func(method)){std::ofstream ram("invalid-ui-renderer.ram",std::ios::binary);ram.write(reinterpret_cast<char*>(ssne::g_ram),ssne::guest_ram_size());std::ofstream cpu("invalid-ui-renderer-context.bin",std::ios::binary);cpu.write(reinterpret_cast<char*>(&ctx),sizeof(ctx));}
    }
    ssne_call_original(ctx,0x800F2E50,sprite_original);
    if(controller){controller_sprites.insert(ctx.r[3]);if(server || client)suppress_controller(ctx,ctx.r[3]);}
}
void sprite_update(PPCContext& ctx){
    const auto sprite=ctx.r[3];ssne_call_original(ctx,0x800F31C8,sprite_update_original);
    if((server || client) && controller_sprites.contains(sprite))suppress_controller(ctx,sprite);
}
void sprite_destroy(PPCContext& ctx){controller_sprites.erase(ctx.r[3]);ssne_call_original(ctx,0x800F2F54,sprite_destroy_original);}
void extend_roster(PPCContext& ctx,uint32_t parent,uint32_t main){
    if(panels.contains(parent))return;
    auto& p=panels[parent];p.main=main;p.heap=mem_read32(ctx.r[13]-0x6de0);p.text_storage=invoke(ctx,0x80116084,p.heap,1024);if(!p.text_storage)throw std::runtime_error("Native lobby labels allocation failed");for(unsigned i=0;i<4;++i)p.gui[i]=mem_read32(parent+4+i*4);
    // Clone the original character-panel resource through its native factory.
    // Extra panels live in a side table; never extend the original four-pointer object.
    for(unsigned i=4;i<8;++i){
        auto factory=mem_read32(0x802ABA9C+4);auto vt=mem_read32(factory);
        const auto scratch=ctx.r[1]-0x80;const auto saved=mem_read32(scratch);
        invoke(ctx,mem_read32(vt+0x24),factory+int16_t(mem_read16(vt+0x20)),scratch);
        auto asset=mem_read32(scratch);mem_write32(scratch,saved);
        p.gui[i]=invoke(ctx,0x800F5448,asset,~0u);mem_write32(p.gui[i]+0x18,i%4);
    }
}
void make(PPCContext& ctx){
    const auto parent=ctx.r[3],main=ctx.r[4];ssne_call_original(ctx,0x800FEBD0,make_original);
    known_rosters[parent]=main;
    if(server || client)extend_roster(ctx,parent,main);
}
Panels* current(uint32_t parent){if(!server && !client)return nullptr;auto found=panels.find(parent);return found==panels.end()?nullptr:&found->second;}
void profiles(PPCContext& ctx){
    auto p=current(ctx.r[3]);if(!p){ssne_call_original(ctx,0x800FF3A0,profiles_original);return;}
    const auto parent=ctx.r[3];
    for(unsigned n=0;n<4;++n){const auto slot=p->batch*4+n,gui=p->gui[slot];
        invoke(ctx,0x800F56E0,gui,n);invoke(ctx,0x800FF25C,parent,n);
        const auto profile=ssne::native_party_profile(slot);
        invoke(ctx,0x800FF2D4,parent,gui,profile==~0u?slot%2:profile);
        // Network input is routed to the owner's slot. Independent native
        // panels must not all consume controller zero's shared-menu input.
        invoke(ctx,0x800F56E0,gui,~0u);
    }
}
void layout(PPCContext& ctx){
    auto p=current(ctx.r[3]);if(!p){ssne_call_original(ctx,0x800FF454,layout_original);return;}
    for(unsigned n=0;n<4;++n){
        const unsigned slot=p->batch*4+n;const auto gui=p->gui[slot],resource=mem_read32(gui+0x80),vt=mem_read32(resource);
        const auto scratch=ctx.r[1]-0x80;
        const float left=-300.f+152.f*float(slot%4),top=(p->count==8?-136.f:-60.f)+152.f*float(slot/4);
        const std::array<float,4> rect{left,top,left+140.f,top+136.f};
        for(unsigned j=0;j<4;++j)mem_write32(scratch+j*4,f2u(rect[j]));
        for(unsigned corner=0;corner<2;++corner){
            mem_write32(scratch+16,f2u(rect[corner*2]+u2f(mem_read32(ctx.r[13]-0x5dac))));
            mem_write32(scratch+20,f2u(rect[corner*2+1]+u2f(mem_read32(ctx.r[13]-0x5da8))));
            auto call=ctx;call.r[1]-=0x100;call.r[3]=resource+int16_t(mem_read16(vt+0x30));call.r[4]=corner?3:0;call.r[5]=4;call.r[6]=scratch+16;ssne_call(call,mem_read32(vt+0x34));
        }
        auto animate=ctx;animate.r[1]-=0x100;animate.r[3]=resource+int16_t(mem_read16(vt+0x38));animate.ps0[1]=animate.ps1[1]=.3;ssne_call(animate,mem_read32(vt+0x3c));
        if(auto model=item(ctx,gui,0xa3)){auto call=ctx;call.r[1]-=0x100;call.r[3]=mem_read32(model+0xbc);call.r[4]=scratch;call.r[5]=4;call.r[6]=scratch+8;call.r[7]=4;ssne_call(call,0x800F4438);}
        hide(ctx,gui,0x9d);hide(ctx,gui,0x9e);hide(ctx,gui,0xa4);hide(ctx,gui,0xa5);
    }
}
void set_text(PPCContext& ctx,uint32_t node,const std::string& text,uint32_t scratch){
    if(!node)return;
    const auto label=text.substr(0,80);for(unsigned i=0;i<=label.size();++i)mem_write8(scratch+i,i<label.size()?uint8_t(label[i]):0);
    auto vt=mem_read32(node+0x5c);invoke(ctx,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),scratch);
}
void open(PPCContext& ctx){
    const auto parent=mem_read32(ctx.r[4]+0x10);auto p=current(parent);if(!p){ssne_call_original(ctx,0x800FF5C0,open_original);return;}
    prepare_characters(ctx);std::string skin_error;if(!apply_native_lobby_skin_choices(ctx,skin_error))ssne::log_message("[coop-native-skin] "+skin_error);const auto mode_address=ctx.r[13]-0x69d4,mode=mem_read32(mode_address);

    if(server && mode==1){server->set_match_player_limit(8);ssne::native_party_configure(server->status().capacity);}
    p->count=mode>=2 && mode<=4?4:8;
    for(unsigned batch=0;batch<p->count/4;++batch){p->batch=batch;for(unsigned i=0;i<4;++i)mem_write32(parent+4+i*4,p->gui[batch*4+i]);mem_write32(mode_address,0);auto call=ctx;ssne_call_original(call,0x800FF5C0,open_original);}
    mem_write32(mode_address,mode);for(unsigned i=0;i<4;++i)mem_write32(parent+4+i*4,p->gui[i]);p->batch=0;p->opened=true;p->labels={};visible.store(true);
    // Reuse the main menu's own font resource for the PC-key footer.
    if(const auto title=item(ctx,p->main,0x9b)){
        const auto source=mem_read32(title+0x1c),desc=p->text_storage+896;
        for(unsigned i=0;i<88;++i)mem_write8(desc+i,mem_read8(source+i));
        mem_write16(desc+2,0x7ffe);mem_write32(desc+0xc,0x810);
        mem_write16(desc+6,0);mem_write16(desc+8,0);mem_write32(desc+0x28,4);mem_write32(desc+0x2c,f2u(0));mem_write32(desc+0x30,f2u(195));mem_write32(desc+0x4c,0);mem_write32(desc+0x50,1);mem_write32(desc+0x54,f2u(.6f));
        const auto existing_footer=item(ctx,p->main,0x7ffe);
        const auto footer=existing_footer?existing_footer:invoke(ctx,0x800F5940,desc,p->main,mem_read32(title+0x24));
        // Show this child directly. The normal GUI Open() walks to the root
        // and would recursively reopen the entire players screen.
        invoke(ctx,0x800FA3CC,footer);
        set_text(ctx,footer,frontend_text(server?FrontText::HostKeys:FrontText::ClientKeys,native_menu_language()),p->text_storage+768);
    }
    ssne::log_message("[coop-native-lobby] original panels="+std::to_string(p->count)+" controllers=hidden");
}
void update(PPCContext& ctx){
    const auto parent=mem_read32(ctx.r[4]+0x10);auto p=current(parent);if(!p){ssne_call_original(ctx,0x800FF8E4,update_original);return;}if(!p->opened)return;
    const auto world=server?server->latest_world():client->latest_world();
    const auto local=server?server->status().player:client->status().player;
    for(unsigned slot=0;slot<p->count;++slot){
        const bool connected=world && world->players[slot].connected;
        const auto label=std::to_string(slot+1)+". "+(connected?native_name_input(world->players[slot].name).substr(0,12):frontend_text(FrontText::Free,native_menu_language()))+(connected && local.slot==slot?" < >":"");
        if(label!=p->labels[slot]){auto node=item(ctx,p->gui[slot],0xa2);set_text(ctx,node,label,p->text_storage+slot*96);if(node){auto d=mem_read32(node+0x1c);mem_write32(d+0xc,mem_read32(d+0xc)&~0x20u);}p->labels[slot]=label;}
        for(unsigned id:{0xa3u}){auto node=item(ctx,p->gui[slot],id);if(node){auto d=mem_read32(node+0x1c);auto flags=mem_read32(d+0xc);mem_write32(d+0xc,connected?flags&~0x20u:flags|0x20u);}}
        hide(ctx,p->gui[slot],0x9d);hide(ctx,p->gui[slot],0x9e);hide(ctx,p->gui[slot],0xa4);hide(ctx,p->gui[slot],0xa5);
    }
    // The original parent still renders its original title and background.
    invoke(ctx,0x800F86C8,ctx.r[3]);
    // A model preview owns an independent native 3D viewport. The GUI
    // descriptor's hidden flag only hides the widget, not that camera.
    // Apply occupancy after widget animation, which may re-enable its view.
    const auto engine=mem_read32(0x802ABAA0u);
    for(unsigned slot=0;slot<8;++slot){
        const auto node=item(ctx,p->gui[slot],0xA3),draw=node?mem_read32(node+0xBC):0;
        if(!draw)continue;
        const auto view=mem_read32(draw+8);
        if(view>=16 || !engine)continue;
        auto call=ctx;call.r[1]-=0x100;call.msr&=~0x8000u;mem_write32(call.r[1],ctx.r[1]);
        call.r[3]=engine;call.r[4]=view;call.r[5]=slot<p->count && world && world->players[slot].connected;
        ssne_call(call,0x8011A240u);
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_CLIENT_UI")){
            static std::array<uint32_t,8> previous{};
            const auto signature=view*2+call.r[5]+1;
            if(previous[slot]!=signature){previous[slot]=signature;ssne::log_message("[lobby-preview] slot="+std::to_string(slot)+" view="+std::to_string(view)+" enabled="+std::to_string(call.r[5]));}
        }
    }
}
void close(PPCContext& ctx){auto p=current(mem_read32(ctx.r[4]+0x10));if(p){for(unsigned i=4;i<8;++i)invoke(ctx,0x800F6498,p->gui[i]);p->opened=false;visible.store(false);}ssne_call_original(ctx,0x800FF808,close_original);}
void level_close(PPCContext& ctx){
    const auto parent=mem_read32(ctx.r[4]+0x10);
    if(server && level_selection.owns(parent) && mem_read16(ctx.r[4]+0x18)==0x44){
        // The difficulty group shares the frontend root, but does not own the
        // level selector's preview resources. Its Back returns to level choice.
        if(level_selection.back_from_difficulty(parent)){
            invoke(ctx,0x800F6178,ctx.r[3],0x44);
            confirm_release.arm();invoke(ctx,0x800F60A0,ctx.r[3],0x4b,0);
            ssne::log_message("[coop-native-lobby] difficulty cancelled; level selection reopened");
        }
        return;
    }
    ssne_call_original(ctx,0x800FDF00,level_close_original);
    level_selection.closed(parent);
}
void difficulty_start(PPCContext& ctx){
    const auto parent=mem_read32(ctx.r[4]+0x10);
    if(!server || !level_selection.owns(parent)){
        ssne_call_original(ctx,0x80101200,difficulty_start_original);return;
    }
    if(level_selection.phase()!=LobbyLevelSelection::Phase::Difficulty)return;
    const auto config=read_native_configuration(native_global_configuration(),"GameSetup.RulesCoop.Difficulty");
    const auto field=config.values.find("GameSetup.RulesCoop.Difficulty");
    const auto* difficulty=field==config.values.end()?nullptr:std::get_if<int64_t>(&field->second);
    if(!config.error.empty() || !difficulty || !level_selection.confirm_difficulty(parent,*difficulty)){
        ssne::log_message("[coop-native-lobby] invalid difficulty confirmation rejected");return;
    }
    ssne::log_message("[coop-native-lobby] difficulty confirmed: "+std::to_string(*difficulty));
}
void level_start(PPCContext& ctx){
    const auto parent=ctx.r[3];
    if(!server || !level_selection.owns(parent)){
        ssne_call_original(ctx,0x800FFFF8,level_start_original);return;
    }
    if(level_selection.phase()==LobbyLevelSelection::Phase::Difficulty ||
       level_selection.phase()==LobbyLevelSelection::Phase::Selected ||
       level_selection.phase()==LobbyLevelSelection::Phase::Queued)return;
    // The original 800FE9AC has already checked unlocks and saved the selected
    // Level/LevelSet. Keep the original difficulty screen before handing off
    // to the eight-player loader, instead of treating level choice as Start.
    const auto mode=mem_read32(ctx.r[13]-0x69d4);
    const auto prefix=native_rules_prefix(mode);if(!prefix)return;
    if(!server->set_match_player_limit(mode==1?8:4)){
        level_selection.reset();confirm_release.arm();
        if(auto p=current(parent)){invoke(ctx,0x800F60A0,p->main,0x9a,0);set_text(ctx,item(ctx,p->main,0x7ffe),frontend_text(FrontText::ModeFull,native_menu_language()),p->text_storage+768);}
        ssne::log_message("[native-mode-menu] versus start rejected: occupied slots above four");return;
    }
    const auto config=read_native_configuration(native_global_configuration(),prefix);
    const auto level=config.values.find(std::string(prefix)+".Level");
    const auto region=config.values.find(std::string(prefix)+".LevelSet");
    const auto* name=level==config.values.end()?nullptr:std::get_if<std::string>(&level->second);
    const auto* set=region==config.values.end()?nullptr:std::get_if<int64_t>(&region->second);
    if(!config.error.empty() || !name || !set || !level_selection.confirm(parent,{*name,*set,1,mode})){
        ssne::log_message("[coop-native-lobby] level selection rejected: missing or invalid native selection");
        level_selection.reset();confirm_release.reset();return;
    }
    ssne::log_message("[coop-native-lobby] original level confirmed: "+*name+" set="+std::to_string(*set));
    if(mode!=1){level_selection.confirm_difficulty(parent,1);ssne::log_message("[native-mode-menu] versus map confirmed: mode="+std::to_string(mode)+" level="+*name);return;}
    // The original frontend already owns group 44, its four choices (47..4A),
    // translated labels, sounds and Difficulty config binding. Reuse those.
    auto bind=ctx;bind.r[1]-=0x100;bind.msr&=~0x8000u;mem_write32(bind.r[1],ctx.r[1]);
    bind.r[3]=ctx.r[4];bind.r[4]=parent;bind.r[5]=0x44;bind.r[6]=3;bind.r[7]=0x800FDF00;ssne_call(bind,0x800F6C14);
    confirm_release.arm();invoke(ctx,0x800FCC5C,parent,ctx.r[4]);
    ssne::log_message("[coop-native-lobby] original difficulty selection opened");
}
void level_confirm(PPCContext& ctx){
    const auto parent=mem_read32(ctx.r[4]+0x10),event=ctx.r[4],gui=ctx.r[3],mode=mem_read32(ctx.r[13]-0x69d4);
    ssne_call_original(ctx,0x800FE9AC,level_confirm_original);
    if(server && mode>=2 && mode<=4 && level_selection.owns(parent)){
        auto call=ctx;call.r[3]=parent;call.r[4]=gui;call.r[5]=event;level_start(call);
    }
}
void destroy(PPCContext& ctx){
    known_rosters.erase(ctx.r[3]);
    if(level_selection.owns(ctx.r[3])){level_selection.reset();confirm_release.reset();}
    uint32_t storage=0,heap=0;
    auto found=panels.find(ctx.r[3]);
    if(found!=panels.end()){
        storage=found->second.text_storage;heap=found->second.heap;
        for(unsigned i=4;i<8;++i){auto gui=found->second.gui[i],asset=mem_read32(gui+0x80);invoke(ctx,0x800F55B4,gui);auto vt=mem_read32(asset);invoke(ctx,mem_read32(vt+0xc),asset+int16_t(mem_read16(vt+8)),3);}
        panels.erase(found);
    }
    ssne_call_original(ctx,0x800FEE3C,destroy_original);
    if(storage)invoke(ctx,0x801161EC,heap,storage);
}
}
void install_native_lobby(AuthoritativeServer* host,AuthoritativeClient* peer){
    server=host;client=peer;visible.store(false);mode_chooser_opened=false;level_selection.reset();confirm_release.reset();model_keys={};applied_skin_players={};character_models.clear();model_catalog.clear();
    if(host)confirm_release.arm(); // Require release of the Host Start click before choosing a mode.
    if(installed)return;installed=true;
    ssne::log_message("[coop-native-lobby] hooks installed");
    if(mem_read32(0x80000000)!=0x47334250u)throw std::runtime_error("Native lobby requires verified PAL revision");
    constexpr std::array<std::pair<uint32_t,uint64_t>,14> fingerprints{{
        {0x800FFFF8,0xcd3cd596b4c17856ull},{0x800FDF00,0x973a72db28f0d5beull},
        {0x80101200,0xbf817146b1678916ull},{0x800FCC5C,0x3d97f9088129e808ull},
        {0x800F31C8,0xe06797b4ec35e4b9ull},{0x800F2F54,0x990a68e73266a73dull},{0x800F2E50,0x1c26651fd0f6801bull},{0x800FEBD0,0xbaf1d15ee621e1f7ull},{0x800FF5C0,0x02fa05e9f78f3484ull},
        {0x800FEE3C,0xa718ae53517484ffull},{0x800FF808,0xb4072a236ce51babull},
        {0x800FF3A0,0x4b3ea9d31aaf531full},{0x800FF454,0x914aadda308ab578ull},
        {0x800FF8E4,0xfa490cbd98a9fb73ull}}};
    for(auto [address,expected]:fingerprints){uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<32;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}if(hash!=expected || !ssne_find_func(address))throw std::runtime_error("Native lobby revision differs");}
    difficulty_start_original=ssne_find_func(0x80101200)->func;ssne_install_hook(0x80101200,difficulty_start);
    struct Hook{uint32_t address;PPCFunc function;PPCFunc* original;};
    for(auto h:{Hook{0x800FE9AC,level_confirm,&level_confirm_original},Hook{0x800FFFF8,level_start,&level_start_original},Hook{0x800FDF00,level_close,&level_close_original},Hook{0x800F31C8,sprite_update,&sprite_update_original},Hook{0x800F2F54,sprite_destroy,&sprite_destroy_original},Hook{0x800F2E50,sprite,&sprite_original},Hook{0x800FEBD0,make,&make_original},Hook{0x800FF5C0,open,&open_original},Hook{0x800FEE3C,destroy,&destroy_original},Hook{0x800FF808,close,&close_original},Hook{0x800FF3A0,profiles,&profiles_original},Hook{0x800FF454,layout,&layout_original},Hook{0x800FF8E4,update,&update_original}}){
        const auto f=ssne_find_func(h.address);if(!f)throw std::runtime_error("Original lobby handler unavailable");if(h.original)*h.original=f->func;ssne_install_hook(h.address,h.function);
    }
}
}

namespace port::coop {
bool receive_native_lobby_characters(PPCContext& ctx,const Properties& mission,std::string& error){
    const auto found=mission.find("Session.CharacterModels");
    if(found==mission.end())return true;
    const auto models=std::get_if<std::vector<int64_t>>(&found->second);
    if(!models || models->size()!=8){error="Invalid lobby character list";return false;}
    if(!prepare_characters(ctx)){error="Native character catalog unavailable";return false;}
    Properties values;
    for(unsigned slot=0;slot<8;++slot){
        if(!model_catalog.contains((*models)[slot])){error="Unknown native character model";return false;}
        if(character_models[slot]!=(*models)[slot])values[model_keys[slot]]=(*models)[slot];
    }
    if(values.empty())return true;
    if(!write_native_configuration(ctx,native_global_configuration(),values,"PlayerProfiles",error))return false;
    for(auto& [parent,p]:panels)if(p.opened)for(unsigned slot=0;slot<8;++slot)if(character_models[slot]!=(*models)[slot]){
        // Refresh the original model widget directly. Rebuilding its profile
        // list would apply the old local-controller profile ownership filter.
        const auto node=item(ctx,p.gui[slot],0xa3);
        const auto profile=invoke(ctx,0x800C23A8,ssne::native_party_profile(slot));
        if(node && profile && mem_read32(node+0xc0))invoke(ctx,0x8010A32C,mem_read32(node+0xc0),invoke(ctx,0x800C2314,profile));
    }
    character_models=*models;return true;
}
bool apply_native_lobby_skin_choices(PPCContext& ctx,std::string& error){
    if(!server)return true;
    const auto choices=server->requested_skins();bool pending=false;
    for(unsigned slot=0;slot<8;++slot)pending|=choices[slot].player.generation && choices[slot].model!=255 && choices[slot].player!=applied_skin_players[slot];
    if(!pending)return true;
    if(!prepare_characters(ctx)){error="Native skin catalog is not ready";return false;}
    auto models=character_models;bool changed=false;
    for(unsigned slot=0;slot<8;++slot)if(choices[slot].player.generation && choices[slot].model!=255 && choices[slot].player!=applied_skin_players[slot]){
        if(!model_catalog.contains(choices[slot].model)){error="Unknown requested skin";return false;}
        models[slot]=choices[slot].model;changed=true;
    }
    if(changed && !receive_native_lobby_characters(ctx,{{"Session.CharacterModels",models}},error))return false;
    for(unsigned slot=0;slot<8;++slot)if(choices[slot].player.generation && choices[slot].model!=255)applied_skin_players[slot]=choices[slot].player;
    return true;
}
void native_lobby_character_input(PPCContext& ctx,unsigned slot,uint16_t pressed){
    if(!server || !native_lobby_visible() || slot>=8 || !prepare_characters(ctx))return;
    const auto direction=pressed&(ssne::PAD_LEFT|ssne::PAD_RIGHT);
    if(direction!=ssne::PAD_LEFT && direction!=ssne::PAD_RIGHT)return;
    auto models=character_models;auto choice=model_catalog.find(models[slot]);
    if(choice==model_catalog.end())return;
    if(direction==ssne::PAD_RIGHT){if(++choice==model_catalog.end())choice=model_catalog.begin();}
    else {if(choice==model_catalog.begin())choice=model_catalog.end();--choice;}
    models[slot]=choice->first;std::string error;
    if(!receive_native_lobby_characters(ctx,{{"Session.CharacterModels",models}},error))ssne::log_message("[coop-native-lobby] "+error);
}
void publish_native_lobby_characters(Properties& mission){if(character_models.size()==8)mission["Session.CharacterModels"]=character_models;}
void deactivate_native_lobby(){server=nullptr;client=nullptr;visible=false;mode_chooser_opened=false;level_selection.reset();confirm_release.reset();for(auto& [id,p]:panels)p.opened=false;}
void suspend_native_lobby(PPCContext& ctx){for(auto& [id,p]:panels)if(p.opened){for(auto child:p.gui)invoke(ctx,0x800F6498,child);invoke(ctx,0x800F6178,p.main,0x9a);p.opened=false;}visible=false;mode_chooser_opened=true;}
void resume_native_lobby(PPCContext& ctx){confirm_release.arm();for(auto& [id,p]:panels){invoke(ctx,0x800F60A0,p.main,0x9a);break;}}
bool attach_existing_native_lobby(PPCContext& ctx){
    if(!server && !client)return false;
    for(const auto& [parent,main]:known_rosters){
        if(parent<0x80000000u || parent>ssne::guest_ram_end()-24 || main<0x80000000u || main>ssne::guest_ram_end()-0x90 || mem_read32(main+0x14))continue;
        extend_roster(ctx,parent,main);
    }
    if(!panels.empty())ssne::log_message("[native-lobby-adopted] existing frontend retained");
    return !panels.empty();
}
void back_native_lobby(PPCContext& ctx){
    if(!server)return;
    for(auto& [parent,p]:panels)if(p.opened){
        const auto node=mem_read32(p.main+0x7c);
        if(node){confirm_release.arm();invoke(ctx,0x800FB130,node,23);ssne::log_message("[native-lobby-back] host returned to rules");}
        break;
    }
}
bool native_lobby_visible(){return visible.load();}
int native_lobby_mouse_owner(uint32_t gui){for(const auto& [parent,p]:panels)if(p.opened){if(p.main==gui)return -2;for(unsigned slot=0;slot<p.count;++slot)if(p.gui[slot]==gui)return int(slot);}return -1;}
uint32_t native_lobby_mouse_main(){for(const auto& [parent,p]:panels)if(p.opened)return p.main;return 0;}
std::array<uint32_t,8> native_lobby_mouse_guis(){for(const auto& [parent,p]:panels)if(p.opened)return p.gui;return {};}
uint16_t filter_native_lobby_confirmation(uint16_t buttons){return confirm_release.filter(buttons,ssne::PAD_A|ssne::PAD_START|ssne::PAD_B);}
std::optional<NativeLobbyLevel> take_native_lobby_level(){return server?level_selection.take():std::nullopt;}
void cancel_native_lobby_level_selection(){level_selection.reset();confirm_release.reset();}
bool open_native_lobby_level_selection(PPCContext& ctx,std::string& error){
    if(!server){error="Only the host can choose the campaign level";return false;}
    if(mem_read32(0x802DB7E8)!=3 || mem_read32(0x802DB7EC)==4){error="Level selection requires the lobby scene";return false;}
    if(level_selection.phase()!=LobbyLevelSelection::Phase::Idle)return true;
    for(auto& [parent,p]:panels)if(p.opened){
        if(!level_selection.begin(parent)){error="Native level selection is already active";return false;}
        confirm_release.arm();
        // These are eight independent GUIs, not children of the main GUI.
        // Close immediately so neither their artwork nor their input survives
        // into the original level selector's opening frame.
        for(auto gui:p.gui)invoke(ctx,0x800F6498,gui);
        invoke(ctx,0x800F6498,p.main);
        p.opened=false;visible.store(false);
        if(!invoke(ctx,0x800F60A0,p.main,0x4b,0)){
            cancel_native_lobby_level_selection();error="Original level selector did not open";return false;
        }
        ssne::log_message("[coop-native-lobby] original level selection opened");
        return true;
    }
    error="Original lobby is not ready";return false;
}
void show_native_lobby(PPCContext& ctx){
    if(mem_read32(0x802DB7E8)!=3){mode_chooser_opened=false;visible.store(false);for(auto& [parent,p]:panels)p.opened=false;cancel_native_lobby_level_selection();return;}
    if(level_selection.phase()==LobbyLevelSelection::Phase::Closed){
        cancel_native_lobby_level_selection();
        const auto mode=mem_read32(ctx.r[13]-0x69d4);
        const unsigned options=mode==2?0x64:mode==3?0x76:mode==4?0x86:0x59;
        for(auto& [parent,p]:panels){invoke(ctx,0x800F6498,p.main);for(auto id:{0x3fu,options,0x9au})invoke(ctx,0x800F60A0,p.main,id,0);}
        return;
    }
    if(level_selection.phase()!=LobbyLevelSelection::Phase::Idle)return;
    static bool traced=false;if(!traced){traced=true;ssne::log_message("[coop-native-lobby] parents="+std::to_string(panels.size()));}
    if(panels.empty() || mem_read32(0x802DB7EC)==4)return;
    for(auto& [parent,p]:panels){if(p.opened){auto call=ctx;call.r[3]=p.main;call.r[4]=ctx.r[1]-0xe0;mem_write32(call.r[4]+0x10,parent);update(call);return;}

        // Clear the outgoing groups without fading out the shared GUI root.
        // 800F643C schedules that root's destruction in a later frontend
        // frame. Reopening the roster immediately would then leave its new
        // widgets attached to a root which is freed underneath them, and the
        // next selector open dereferences it at 800FD34C.
        if(mode_chooser_opened)return;
        invoke(ctx,0x800F6498,p.main);
        if(client){
            const auto mode=mem_read32(ctx.r[13]-0x69d4);
            const unsigned options=mode==2?0x64:mode==3?0x76:mode==4?0x86:0x59;
            for(unsigned id:{0x3fu,options,0x9au})invoke(ctx,0x800F60A0,p.main,id,0);
            mode_chooser_opened=true;ssne::log_message("[native-mode-menu] client joined original player lobby");return;
        }
        mem_write32(ctx.r[13]-0x69d4,0);
        for(unsigned id:{0x3fu,0xa8u})invoke(ctx,0x800F60A0,p.main,id,0);
        mode_chooser_opened=true;
        ssne::log_message("[native-mode-menu] original mode selection opened");
        return;
    }
}
}
