#include "native_lobby.h"
#include "native_configuration.h"
#include "native_bootstrap.h"
#include "native_frontend.h"
#include "native_online_setup.h"
#include "custom_levels_runtime.h"
#include "native_lobby_map_view.h"
#include "ssne/input.h"
#include "ssne/recomp_hooks.h"
#include "ssne/native_party.h"
#include "ssne/runtime.h"
#include "ssne/custom_crosshair.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <map>
#include <set>
#include <string>
#include <fstream>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <sstream>
namespace port::coop { namespace {
AuthoritativeServer* server=nullptr;AuthoritativeClient* client=nullptr;
struct Panels {std::array<uint32_t,8> gui{},ready_nodes{},ready_sprites{},ready_quads{};std::array<bool,8> ready_shown{};unsigned batch=0,count=8;bool opened=false;uint32_t main=0,text_storage=0,heap=0;std::array<std::string,8> labels{},roles{},readiness{};std::array<std::string,14> information{};std::array<float,13> information_scale{};std::array<std::string,3> title_lines{};float layout_width=0;std::string preview,map_key,title_key;lobby_map_native::Metadata metadata;};
struct LobbyLayout {float half_width,sidebar_width,sidebar_x,grid_scale,grid_x;};
LobbyLayout lobby_layout(){
    const auto wide=ssne::widescreen();const float half=320.f*(std::isfinite(wide)?std::clamp(wide,1.f,4.f):1.f);
    const float sidebar=std::clamp(112.f+(half-426.666667f)*.51f,112.f,180.f);
    const float grid=std::min(1.f,(2.f*half-24.f-sidebar-12.f)/596.f);
    return {half,sidebar,half-12.f-sidebar*.5f,grid,std::min(0.f,half-12.f-sidebar-12.f-298.f*grid)};
}
std::map<uint32_t,Panels> panels;
std::map<uint32_t,uint32_t> known_rosters;
PPCFunc make_original=nullptr,open_original=nullptr,destroy_original=nullptr,close_original=nullptr,sprite_original=nullptr,sprite_update_original=nullptr,sprite_destroy_original=nullptr;
PPCFunc profiles_original=nullptr,layout_original=nullptr,update_original=nullptr;
bool installed=false;
std::set<uint32_t> controller_sprites;
std::set<uint32_t> lobby_rule_map_sprites;
std::atomic<bool> visible{false};
PPCFunc ready_material_original=nullptr;
struct ReadyBadgeOwner {uint32_t sprite=0,gui=0;bool shown=false;};
// Texture identity lasts until the native sprite is destroyed, including its
// closing fade. Readiness controls the primitive's area, never this registry.
std::map<uint32_t,ReadyBadgeOwner> ready_badge_quads;
bool ready_image_scope=false;
void finish_ready_image(){if(ready_image_scope){ssne::gx_custom_menu_image_scope({});ready_image_scope=false;}}
std::shared_ptr<const ssne::CustomCrosshairImage> ready_badge_image(){
    static const auto image=[](){
        auto result=std::make_shared<ssne::CustomCrosshairImage>();
        result->width=result->height=result->right=result->bottom=64;result->rgba.resize(64*64*4);
        auto distance=[](float x,float y,float ax,float ay,float bx,float by){
            const float dx=bx-ax,dy=by-ay,t=std::clamp(((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy),0.f,1.f);
            return std::hypot(x-ax-dx*t,y-ay-dy*t);
        };
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
            const float d=std::min(distance(x+.5f,y+.5f,10,32,25,46),distance(x+.5f,y+.5f,25,46,54,13));
            const float alpha=std::clamp(7.f-d,0.f,1.f),green=std::clamp(4.5f-d,0.f,1.f);
            if(!alpha)continue;const float ink=green/alpha;const auto n=(y*64+x)*4;
            result->rgba[n]=uint8_t(5+(65-5)*ink);result->rgba[n+1]=uint8_t(12+(245-12)*ink);
            result->rgba[n+2]=uint8_t(3+(25-3)*ink);result->rgba[n+3]=uint8_t(std::lround(alpha*255));
        }
        return std::shared_ptr<const ssne::CustomCrosshairImage>(result);
    }();return image;
}
void ready_material(PPCContext& ctx){
    const auto caller=ctx.lr,quad=ctx.r[28];
    // End our previous image before the existing material hook/preview observer
    // selects its own scope. Never replace that observer or the user's reticle.
    finish_ready_image();ssne_call_original(ctx,0x8012680c,ready_material_original);
    if(caller!=0x8011e90c || ready_badge_quads.empty())return;
    const auto owner=ready_badge_quads.find(quad);
    if(owner!=ready_badge_quads.end() &&
       lobby_map_native::readable(owner->second.sprite,12) && mem_read32(owner->second.sprite+8)==quad &&
       lobby_map_native::readable(quad,4) && mem_read32(quad)==0x8021cb58){
        ssne::gx_custom_menu_image_scope(ready_badge_image());ready_image_scope=true;
    }
}
bool mode_chooser_opened=false;
std::atomic<bool> survival_mode{false};
unsigned lobby_mode=1;
LobbyLevelSelection level_selection;
LobbyConfirmRelease confirm_release;
bool selection_return_pending=false,start_requested=false;
std::optional<NativeLobbyLevel> starting_level;
std::atomic<bool> selected_level_available{false};
std::optional<bool> client_ready_pending;
int64_t client_ready_token=0;
std::array<unsigned,3> previous_lobby_rules{0,0,0};
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
void forget_sprite(uint32_t sprite){
    controller_sprites.erase(sprite);lobby_rule_map_sprites.erase(sprite);
    for(auto it=ready_badge_quads.begin();it!=ready_badge_quads.end();)if(it->second.sprite==sprite)it=ready_badge_quads.erase(it);else ++it;
    for(auto& [parent,p]:panels)for(unsigned slot=0;slot<8;++slot)if(p.ready_sprites[slot]==sprite){p.ready_sprites[slot]=0;p.ready_quads[slot]=0;}
}
void sprite(PPCContext& ctx){
    // The game also reclaims pooled sprites in bulk during a GUI transition.
    // Begin each new lifetime before the constructor's first native update.
    forget_sprite(ctx.r[3]);
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
    const auto sprite=ctx.r[3];
    if(online_setup_native::hidden_icon_sprite(sprite) || ((server || client) && controller_sprites.contains(sprite))){suppress_controller(ctx,sprite);return;}
    if(visible.load() && lobby_rule_map_sprites.contains(sprite)){
        // These old previews share their atlas with the retained sidebar.
        // Collapse only their own geometry; never publish an atlas alpha zero.
        suppress_controller(ctx,sprite);return;
    }
    const auto badge=ready_badge_quads.find(mem_read32(sprite+8));
    if(badge!=ready_badge_quads.end() && badge->second.sprite==sprite && !badge->second.shown){
        // The native image atlas shares its material with the map thumbnail.
        // Updating a hidden badge would publish alpha zero to every image.
        // Animation advances in the owning widget; this sprite update only
        // copies its geometry and tint. Collapse our own quad without tinting
        // that shared material. Visible badges retain the original fade path.
        suppress_controller(ctx,sprite);return;
    }
    ssne_call_original(ctx,0x800F31C8,sprite_update_original);
}
void sprite_destroy(PPCContext& ctx){
    forget_sprite(ctx.r[3]);
    finish_ready_image();ssne_call_original(ctx,0x800F2F54,sprite_destroy_original);
}
void extend_roster(PPCContext& ctx,uint32_t parent,uint32_t main){
    if(panels.contains(parent))return;
    // Native children retain their descriptors until the owning GUI is freed.
    // Names/footer0..983, name/role descriptors1024..2559, role text2560..3327;
    // alias scratch4096..4191; Ready descriptors5120..5887/text5888..6655;
    // header descriptors6656..7903, image7936..8019, header text8192..11519;
    // extra title descriptors11520..11711/text11712..11967/scratch12000..12080;
    // character arrow descriptors12288..13823/text13824..13951;
    // Ready image descriptors14336..15103/alias15104..15199.
    auto& p=panels[parent];p.main=main;p.heap=mem_read32(ctx.r[13]-0x6de0);p.text_storage=invoke(ctx,0x80116084,p.heap,15360);if(!p.text_storage)throw std::runtime_error("Native lobby labels allocation failed");for(unsigned i=0;i<4;++i)p.gui[i]=mem_read32(parent+4+i*4);
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
        const auto view=lobby_layout();
        const float center=view.grid_x+(-228.f+152.f*float(slot%4))*view.grid_scale;
        const float left=center-70.f*view.grid_scale,top=-100.f+152.f*float(slot/4);
        const std::array<float,4> rect{left,top,left+140.f*view.grid_scale,top+76.f};
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
void caption_scale(PPCContext& ctx,uint32_t node,float scale){
    if(!node)return;
    const auto helper=mem_read32(node+0xb8);
    if(!lobby_map_native::readable(helper,0x48))return;
    const auto fields=mem_read32(helper);
    if(!lobby_map_native::readable(fields,8) || mem_read16(fields)>1)return;
    const auto actual=u2f(mem_read32(fields+4));
    if(actual==scale || !std::isfinite(actual) || actual<=0 || !(scale>0))return;
    const auto ascent=u2f(mem_read32(helper+0x18)),height=u2f(mem_read32(helper+0x1c));
    if(!std::isfinite(ascent) || !std::isfinite(height))return;
    // Restore the native font's linear vertical metrics and live scale before
    // refreshing glyph widths. Font1 construction clamps sizes below .6;
    // localized sidebar headings can also need a smaller Font0 on narrow views.
    mem_write32(fields+4,f2u(scale));
    mem_write32(helper+0x18,f2u(ascent*scale/actual));
    mem_write32(helper+0x1c,f2u(height*scale/actual));
    const auto text=mem_read32(helper+8),vt=mem_read32(node+0x5c);
    if(lobby_map_native::readable(text,1))invoke(ctx,mem_read32(vt+0x74),node+int16_t(mem_read16(vt+0x70)),text);
}
void caption_position(uint32_t node,float x,float y){
    if(!node)return;
    const auto desc=mem_read32(node+0x1c),params=mem_read32(node+0x14);
    if(!lobby_map_native::readable(desc,0x34) || !lobby_map_native::readable(params,4))return;
    mem_write32(desc+0x2c,f2u(x));mem_write32(desc+0x30,f2u(y));
    const auto position=mem_read32(params);
    // Native800FB728 allocates this current/target position vector once.
    // Update both endpoints without reopening or rebuilding the owned widget.
    if(lobby_map_native::readable(position,16))for(unsigned i=0;i<4;++i)mem_write32(position+i*4,f2u(i%2?y:x));
}
uint32_t caption(PPCContext& ctx,uint32_t gui,unsigned id,uint32_t original,uint32_t desc,float x,float y,float scale,unsigned anchor=4,unsigned font=1){
    if(const auto existing=item(ctx,gui,id)){caption_position(existing,x,y);caption_scale(ctx,existing,scale);return existing;}
    if(!original)return 0;
    const auto source=mem_read32(original+0x1c);
    for(unsigned i=0;i<88;++i)mem_write8(desc+i,mem_read8(source+i));
    mem_write16(desc,1);mem_write16(desc+2,id);mem_write16(desc+6,0);mem_write16(desc+8,0);
    // The original small white frontend font, rather than another menu action.
    mem_write32(desc+0xc,0x810);mem_write32(desc+0x28,anchor);
    mem_write32(desc+0x2c,f2u(x));mem_write32(desc+0x30,f2u(y));
    mem_write32(desc+0x44,0);mem_write32(desc+0x48,0);mem_write32(desc+0x4c,0);
    // Two native halfwords: original regular font1, centered alignment1.
    mem_write32(desc+0x50,(font<<16)|1u);mem_write32(desc+0x54,f2u(scale));
    const auto node=invoke(ctx,0x800F5940,desc,gui,mem_read32(original+0x24));
    if(node){invoke(ctx,0x800FA3CC,node);caption_scale(ctx,node,scale);}
    return node;
}
void caption_visibility(PPCContext& ctx,uint32_t node,bool shown){
    if(!node)return;const auto desc=mem_read32(node+0x1c),flags=mem_read32(desc+0xc);
    if(shown && (flags&0x20u))invoke(ctx,0x800FA3CC,node);
    const auto live=mem_read32(node+0x1c),current=mem_read32(live+0xc);
    // Read-only labels retain the native disabled bit, so keyboard/gamepad
    // focus cannot enter names or status text. Visibility is a separate bit.
    mem_write32(live+0xc,shown?(current|0x10u)&~0x20u:current|0x30u);
}
void refresh_ready_badge(PPCContext& ctx,Panels& p,unsigned slot,bool reopened=false){
    const auto node=p.ready_nodes[slot];if(!lobby_map_native::readable(node,0xb8))return;
    const auto definition=mem_read32(node+0x1c);
    if(mem_read32(node+0x20)!=p.main || !lobby_map_native::readable(definition,4) || mem_read16(definition)!=6 || mem_read16(definition+2)!=0x7e40u+slot)return;
    auto sprite=mem_read32(node+0xb4);
    const bool rebound=p.ready_shown[slot] && (reopened || sprite!=p.ready_sprites[slot]);
    if(rebound){
        // Native opening may reconstruct the sprite or reset its image. Bind
        // after opening, then obtain the actual retained sprite/quad again.
        lobby_map_native::show_thumbnail(ctx,node,"FrontEndA:GTCooperative",p.text_storage+15104);
        sprite=mem_read32(node+0xb4);
    }
    const auto quad=lobby_map_native::readable(sprite,12)?mem_read32(sprite+8):0;
    const bool valid=lobby_map_native::readable(quad,4) && mem_read32(quad)==0x8021cb58;
    p.ready_sprites[slot]=valid?sprite:0;p.ready_quads[slot]=valid?quad:0;
    if(valid){
        ready_badge_quads[quad]={sprite,p.main,p.ready_shown[slot]};
        // Image lookup also sets the quad's authored image rectangle. Restore
        // the native position/size/fade parameters after each actual rebind.
        if(rebound)invoke(ctx,0x800F31C8,sprite);
        // Hidden GUI definitions can leave an independent sprite drawable.
        // Repeat after native animation, as with the original PadGfx artwork.
        if(!p.ready_shown[slot])suppress_controller(ctx,sprite);
    }
}
void ready_badge(PPCContext& ctx,Panels& p,unsigned slot,bool shown,const LobbyLayout& view){
    const auto id=0x7e40u+slot;auto node=item(ctx,p.main,id);
    if(shown && !node){
        node=lobby_map_native::create_thumbnail(ctx,p.main,p.text_storage+14336+slot*96,id,0,0,32.f*view.grid_scale,32.f*view.grid_scale);
    }
    p.ready_nodes[slot]=node;p.ready_shown[slot]=shown;
    if(!node)return;
    const float center=view.grid_x+(-228.f+152.f*float(slot%4))*view.grid_scale;
    caption_position(node,center+20.f*view.grid_scale,-96.f+152.f*float(slot/4));
    const auto descriptor=mem_read32(node+0x1c),params=mem_read32(node+0x14);
    const float size=32.f*view.grid_scale;
    if(lobby_map_native::readable(descriptor,0x3c)){mem_write32(descriptor+0x34,f2u(size));mem_write32(descriptor+0x38,f2u(size));}
    if(lobby_map_native::readable(params,12)){
        const auto dimensions=mem_read32(params+8);
        if(lobby_map_native::readable(dimensions,16))for(unsigned i=0;i<4;++i)mem_write32(dimensions+i*4,f2u(size));
    }
    const bool reopened=shown && lobby_map_native::readable(descriptor,16) && (mem_read32(descriptor+0xc)&0x20u);
    caption_visibility(ctx,node,shown);
    refresh_ready_badge(ctx,p,slot,reopened);
    if(!p.ready_quads[slot])caption_visibility(ctx,node,false);
}
void fitted_text(PPCContext& ctx,uint32_t node,const std::string& source,uint32_t scratch,float maximum){
    if(!node)return;
    // The native setter updates CcText's font metrics. Fit using those actual
    // glyph widths, including wide names, instead of a character-count guess.
    for(size_t keep=source.size();;--keep){
        set_text(ctx,node,keep==source.size()?source:source.substr(0,keep)+"...",scratch);
        const auto helper=mem_read32(node+0xb8);
        if(helper>=0x80000000u && helper<=ssne::guest_ram_end()-0x20){
            const auto width=u2f(mem_read32(helper+0x14));
            if(std::isfinite(width) && width<=maximum+.01f)break;
        }
        if(!keep)break;
    }
}
float caption_width(uint32_t node){
    const auto helper=node?mem_read32(node+0xb8):0;
    return lobby_map_native::readable(helper,0x18)?u2f(mem_read32(helper+0x14)):0.f;
}
float sidebar_text(PPCContext& ctx,uint32_t node,const std::string& source,uint32_t scratch,float maximum,float base){
    caption_scale(ctx,node,base);set_text(ctx,node,source,scratch);
    const auto width=caption_width(node);float scale=base;
    if(std::isfinite(width) && width>maximum){scale=std::max(.30f,base*maximum/width);caption_scale(ctx,node,scale);}
    fitted_text(ctx,node,source,scratch,maximum);return scale;
}
std::array<std::string,3> wrapped_title(PPCContext& ctx,uint32_t node,const std::string& source,uint32_t scratch,float maximum){
    std::array<std::string,3> lines;std::istringstream words(source);std::string word;unsigned line=0;
    while(words>>word){
        const auto candidate=lines[line].empty()?word:lines[line]+" "+word;
        set_text(ctx,node,candidate,scratch);
        if(caption_width(node)<=maximum || lines[line].empty() || line==2)lines[line]=candidate;
        else lines[++line]=word;
    }
    return lines;
}
FrontText lobby_mode_text(unsigned mode){
    switch(mode){case 2:return FrontText::ModeDeathmatch;case 3:return FrontText::ModeFlag;
        case 4:return FrontText::ModeBomb;case 5:return FrontText::Survival;default:return FrontText::ModeCoop;}
}
void preview_character(PPCContext& ctx,uint32_t node,uint32_t profile,bool reopening=false){
    if(!node || !profile)return;
    const auto state=mem_read32(node+0xc0);
    if(!lobby_map_native::readable(state,20))return;
    const auto name=invoke(ctx,0x800C2314,profile);
    const auto target=lobby_map_native::text(name,64);
    if(target.empty())return;
    // Original SetModel is a crossfade factory, not an idempotent refresh.
    // Reopening a lobby retires both old instances through their native owner;
    // an actual character change retains the original transition animation.
    if(reopening)invoke(ctx,0x8010A09C,state);
    const auto current=mem_read32(state);
    if(lobby_map_native::readable(current,0x40) &&
       lobby_map_native::text(mem_read32(current+0x3c),64)==target)return;
    invoke(ctx,0x8010A32C,state,name);
}
void refresh_previews(PPCContext& ctx,const Panels& p){
    for(unsigned slot=0;slot<p.count;++slot){
        const auto node=item(ctx,p.gui[slot],0xa3);
        const auto profile=invoke(ctx,0x800C23A8,ssne::native_party_profile(slot));
        preview_character(ctx,node,profile,true);
    }
}
void adopt_survival_setup(PPCContext& ctx,uint32_t parent){
    if(!server || (level_selection.phase()!=LobbyLevelSelection::Phase::Idle && level_selection.phase()!=LobbyLevelSelection::Phase::Selected))return;
    const auto config=read_native_configuration(native_global_configuration(),"GameSetup");
    auto integer=[&](const char* key,int64_t fallback){const auto field=config.values.find(key);if(field!=config.values.end())if(const auto value=std::get_if<int64_t>(&field->second))return *value;return fallback;};
    const auto field=config.values.find("GameSetup.RulesFragmatch.Level");const auto name=field==config.values.end()?nullptr:std::get_if<std::string>(&field->second);
    if(!config.error.empty() || !name || name->empty())return;
    const auto set=find_custom_level(*name)?int64_t(0):integer("GameSetup.RulesFragmatch.LevelSet",0),difficulty=integer("GameSetup.RulesCoop.Difficulty",1);
    if(difficulty<0 || difficulty>3)return;
    const auto& old=level_selection.selected();if(old && old->mode==5 && old->level==*name && old->level_set==set && old->difficulty==difficulty)return;
    if(level_selection.begin(parent) && level_selection.confirm(parent,{*name,set,difficulty,5}) && level_selection.confirm_difficulty(parent,difficulty)){
        selected_level_available=true;server->reset_lobby_readiness();confirm_release.arm();
    }
}
void open(PPCContext& ctx){
    const auto parent=mem_read32(ctx.r[4]+0x10);auto p=current(parent);if(!p){ssne_call_original(ctx,0x800FF5C0,open_original);return;}
    prepare_characters(ctx);std::string skin_error;if(!apply_native_lobby_skin_choices(ctx,skin_error))ssne::log_message("[coop-native-skin] "+skin_error);const auto mode_address=ctx.r[13]-0x69d4;const bool survival_setup=server && survival_mode && mem_read32(mode_address)==2;if(server && survival_mode)mem_write32(mode_address,1);const auto mode=mem_read32(mode_address);

    if(server && mode==1){server->set_match_player_limit(8);ssne::native_party_configure(server->status().capacity);}
    p->count=mode>=2 && mode<=4?4:8;if(server){const auto next=survival_mode?5u:mode>=1 && mode<=4?mode:1u;if(next!=lobby_mode){level_selection.sync_mode(next);starting_level.reset();selected_level_available=bool(level_selection.selected());selection_return_pending=false;start_requested=false;server->reset_lobby_readiness();}lobby_mode=next;}
    if(survival_setup)adopt_survival_setup(ctx,parent);
    for(unsigned batch=0;batch<p->count/4;++batch){p->batch=batch;for(unsigned i=0;i<4;++i)mem_write32(parent+4+i*4,p->gui[batch*4+i]);mem_write32(mode_address,0);auto call=ctx;ssne_call_original(call,0x800FF5C0,open_original);}
    mem_write32(mode_address,mode);for(unsigned i=0;i<4;++i)mem_write32(parent+4+i*4,p->gui[i]);p->batch=0;p->opened=true;p->labels={};p->roles={};p->readiness={};p->information={};p->preview.clear();p->title_key.clear();p->layout_width=0;
    finish_ready_image();for(unsigned slot=0;slot<8;++slot)ready_badge(ctx,*p,slot,false,lobby_layout());visible.store(true);
    // Native Open reapplies the four-controller profile filter. Restore each
    // network slot's preview even when its authoritative model ID did not change.
    refresh_previews(ctx,*p);
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
    const auto language=native_menu_language();auto tr=[&](FrontText text){return frontend_text(text,language);};
    auto integer=[&](const char* key,int64_t fallback){if(world){const auto found=world->mission.find(key);if(found!=world->mission.end())if(const auto value=std::get_if<int64_t>(&found->second))return *value;}return fallback;};
    const auto bot_mask=uint64_t(integer("Session.Bots.Mask",0));
    const unsigned mode=server?lobby_mode:unsigned(integer("Session.Lobby.Mode",integer("Session.CustomMode",integer("Session.GameMode",1))));
    const unsigned count=mode>=2 && mode<=4?4:8;
    if(p->count!=count){
        // The client can already be in its original roster when the host's
        // mode arrives. Register/open the extra batch by the same native path
        // used by open(), without reopening the main root or its mode chooser.
        if(count==4){for(unsigned slot=4;slot<8;++slot)invoke(ctx,0x800F6498,p->gui[slot]);p->count=4;}
        else{
            p->count=8;p->batch=1;const auto mode_address=ctx.r[13]-0x69d4,previous_mode=mem_read32(mode_address);
            for(unsigned slot=0;slot<4;++slot)mem_write32(parent+4+slot*4,p->gui[slot+4]);
            const auto root=item(ctx,p->main,0x9a);
            if(!root || mem_read32(root+0x20)!=p->main)throw std::runtime_error("Native lobby root unavailable while updating player slots");
            // Original callback r3 is the GUI; GUI+20 is its renderer.
            // The root widget+20 is the GUI, one indirection too many.
            mem_write32(mode_address,0);auto call=ctx;call.r[3]=p->main;ssne_call_original(call,0x800FF5C0,open_original);
            mem_write32(mode_address,previous_mode);for(unsigned slot=0;slot<4;++slot)mem_write32(parent+4+slot*4,p->gui[slot]);
        }
        p->batch=0;p->labels={};p->roles={};p->readiness={};p->preview.clear();p->title_key.clear();refresh_previews(ctx,*p);auto call=ctx;call.r[3]=parent;layout(call);
    }
    const auto view=lobby_layout();
    if(p->layout_width!=view.half_width){
        p->layout_width=view.half_width;p->labels={};p->roles={};p->readiness={};p->information={};p->title_key.clear();
        const auto batch=p->batch;
        for(unsigned i=0;i<p->count/4;++i){p->batch=i;auto call=ctx;call.r[3]=parent;layout(call);}
        p->batch=batch;
    }
    unsigned occupied=0;for(unsigned slot=0;slot<p->count;++slot)occupied+=world && world->players[slot].connected;
    const auto heading=std::string(tr(FrontText::Lobby));
    if(p->information[13]!=heading){set_text(ctx,item(ctx,p->main,0x9b),heading,p->text_storage+768);p->information[13]=heading;}
    const auto capacity=unsigned(integer("Session.AmmoCapacity",0));
    const auto ammo=frontend_ammo_capacity_value(capacity,language);
    std::string map;const auto level_field=world?world->mission.find("Session.Lobby.Level"):Properties::const_iterator{};
    if(world && level_field!=world->mission.end())if(const auto text=std::get_if<std::string>(&level_field->second))map=*text;
    const auto level_set=unsigned(integer("Session.Lobby.LevelSet",0)),difficulty=unsigned(integer("Session.Lobby.Difficulty",1));
    const auto map_key=map+"#"+std::to_string(level_set)+"#"+std::to_string(language);
    if(p->map_key!=map_key || (!map.empty() && !p->metadata.valid)){p->metadata=map.empty()?lobby_map_native::Metadata{}:lobby_map_native::metadata(ctx,map,level_set,p->text_storage+4096);p->map_key=map_key;}
    const auto& metadata=p->metadata;
    const auto ready_mask=uint64_t(integer("Session.Lobby.ReadyMask",0));unsigned ready_count=0;
    if(world)for(unsigned slot=0;slot<p->count;++slot)ready_count+=world->players[slot].connected && bool(ready_mask&(uint64_t(1)<<slot));
    for(unsigned slot=0;slot<8;++slot)ready_badge(ctx,*p,slot,slot<p->count && world && world->players[slot].connected && bool(ready_mask&(uint64_t(1)<<slot)),view);
    const auto difficulty_text=difficulty==0?FrontText::Easy:difficulty==1?FrontText::Normal:difficulty==2?FrontText::Hard:FrontText::Serious;
    auto client_hint=FrontText::LobbyWaitingReady;
    if(map.empty())client_hint=FrontText::LobbyWaitingForMap;
    else if(occupied && ready_count==occupied)client_hint=FrontText::LobbyWaitingForHost;
    const std::array<std::string,13> information{
        std::string(tr(lobby_mode_text(mode)))+"    "+std::to_string(occupied)+" / "+std::to_string(p->count)+" "+tr(FrontText::Players),
        tr(FrontText::LobbySettings),
        server?std::string{}:std::string(tr(client_hint)),
        map.empty()?std::string(tr(FrontText::LobbyNoMap)):metadata.valid?metadata.title:map,
        tr(FrontText::Skill),
        occupied>1?std::string(tr(FrontText::LobbyReady))+" "+std::to_string(ready_count)+" / "+std::to_string(occupied):std::string{},
        map.empty()?std::string("-"):std::string(tr(difficulty_text)),
        tr(FrontText::Enemies),frontend_factor_value(unsigned(integer("Session.EnemyMultiplier",1)),language),
        tr(FrontText::AmmoCapacity),ammo,
        tr(FrontText::AmmoPickups),frontend_factor_value(unsigned(integer("Session.AmmoPickupMultiplier",1)),language)
    };
    const auto title=item(ctx,p->main,0x9b);
    for(unsigned line=0;line<information.size();++line){
        const auto old=item(ctx,p->main,0x7ff0+line);
        const std::array<float,13> xs{140.f,view.sidebar_x,-170.f,view.sidebar_x,view.sidebar_x,-145.f,view.sidebar_x,view.sidebar_x,view.sidebar_x,view.sidebar_x,view.sidebar_x,view.sidebar_x,view.sidebar_x};
        const std::array<float,13> ys{-150.f,6.f,210.f,-64.f,35.f,-123.f,50.f,79.f,94.f,123.f,138.f,167.f,182.f};
        const bool sam=line==1 || line==3 || line==6 || line==8 || line==10 || line==12;
        const bool setting_label=line==4 || line==7 || line==9 || line==11;
        const float base=line==0?.64f:line==3?.36f:sam?.40f:setting_label?.40f:.6f;
        const bool changed=!old || p->information[line]!=information[line];
        const auto node=caption(ctx,p->main,0x7ff0+line,title,p->text_storage+6656+line*96,xs[line],ys[line],!changed && p->information_scale[line]>0?p->information_scale[line]:base,4,sam?0:1);
        if(node && changed){
            if(line!=3){
                const float maximum=line==0?340.f:line==2?270.f:line==5?230.f:view.sidebar_width-4.f;
                if(line==1 || setting_label)p->information_scale[line]=sidebar_text(ctx,node,information[line],p->text_storage+8192+line*256,maximum,base);
                else{fitted_text(ctx,node,information[line],p->text_storage+8192+line*256,maximum);p->information_scale[line]=base;}
            }
            p->information[line]=information[line];
        }
        caption_visibility(ctx,node,!information[line].empty());
    }
    const auto title_node=item(ctx,p->main,0x7ff3);const auto title_key=information[3]+"#"+std::to_string(view.sidebar_width);
    const bool title_changed=p->title_key!=title_key;
    if(title_changed){p->title_lines=wrapped_title(ctx,title_node,information[3],p->text_storage+12000,view.sidebar_width-4.f);p->title_key=title_key;}
    for(unsigned line=0;line<3;++line){
        const auto node=line?caption(ctx,p->main,0x7fe0+line,title,p->text_storage+11520+(line-1)*96,view.sidebar_x,-64.f+19.f*line,.36f,4,0):title_node;
        if(title_changed)fitted_text(ctx,node,p->title_lines[line],line?p->text_storage+11712+(line-1)*128:p->text_storage+8192+3*256,view.sidebar_width-4.f);
        caption_visibility(ctx,node,!p->title_lines[line].empty());
    }
    const auto thumbnail=lobby_map_native::create_thumbnail(ctx,p->main,p->text_storage+7936,0x7fe0,view.sidebar_x-32.f,-134.f);
    caption_position(thumbnail,view.sidebar_x-32.f,-134.f);
    if(thumbnail && p->preview!=map){if(map.empty() || lobby_map_native::show_thumbnail(ctx,thumbnail,map,p->text_storage+4096))p->preview=map;}
    caption_visibility(ctx,thumbnail,!map.empty());
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_DEV_NATIVE_MENU")){
        static auto next=std::chrono::steady_clock::time_point{};const auto now=std::chrono::steady_clock::now();
        if(now>=next){next=now+std::chrono::milliseconds(150);
            auto quoted=[](const std::string& text){std::string out="\"";constexpr char hex[]="0123456789abcdef";for(unsigned char c:text){if(c=='\"' || c=='\\'){out+='\\';out+=char(c);}else if(c<32 || c>=127){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}return out+'\"';};
            std::ofstream out("native-lobby-setup-state.json.pending",std::ios::binary|std::ios::trunc);
            out<<"{\"map\":"<<quoted(map)<<",\"title\":"<<quoted(metadata.title)<<",\"region\":"<<quoted(metadata.region)<<",\"level_set\":"<<level_set<<",\"difficulty\":"<<difficulty<<",\"mode\":"<<mode<<",\"language\":"<<language<<",\"slots\":"<<p->count<<",\"ready_mask\":"<<ready_mask<<",\"ready_token\":"<<integer("Session.Lobby.ReadyToken",0)<<",\"local_ready\":"<<native_lobby_ready()<<",\"can_ready\":"<<native_lobby_can_ready()<<",\"can_start\":"<<native_lobby_can_start()<<",\"thumbnail_node\":"<<thumbnail<<",\"thumbnail_applied\":"<<(!map.empty() && p->preview==map)<<",\"preview_models\":[";
            for(unsigned slot=0;slot<p->count;++slot){
                int64_t model=-1;const auto node=item(ctx,p->gui[slot],0xa3);
                const auto state=node?mem_read32(node+0xc0):0;
                const auto wrapper=lobby_map_native::readable(state,4)?mem_read32(state):0;
                if(lobby_map_native::readable(wrapper,0x40)){
                    const auto resource=lobby_map_native::text(mem_read32(wrapper+0x3c));
                    for(const auto& [id,name]:model_catalog)if(name==resource){model=id;break;}
                }
                if(slot)out<<',';out<<model;
            }
            out<<"]}";
            out.close();if(out){std::remove("native-lobby-setup-state.json");std::rename("native-lobby-setup-state.json.pending","native-lobby-setup-state.json");}
        }
    }
    for(unsigned slot=0;slot<p->count;++slot){
        const bool connected=world && world->players[slot].connected;
        const auto label=connected?native_name_input(world->players[slot].name):std::string{};
        const float center=view.grid_x+(-228.f+152.f*float(slot%4))*view.grid_scale,row=152.f*float(slot/4);
        const auto old_name=item(ctx,p->main,0x7e00+slot);
        const auto name=caption(ctx,p->main,0x7e00+slot,title,p->text_storage+1024+slot*96,center,-16.f+row,.46f,4,0);
        if(!old_name || label!=p->labels[slot]){fitted_text(ctx,name,label,p->text_storage+slot*96,132.f*view.grid_scale);p->labels[slot]=label;}
        caption_visibility(ctx,name,connected);hide(ctx,p->gui[slot],0xa2);
        // Keep the visible arrows inside the existing model's left/right
        // mouse regions. Read-only captions cannot steal those clicks or focus;
        // keyboard, controller and network authority keep their original path.
        for(unsigned side=0;side<2;++side){
            const auto id=0x7e30u+slot*2+side,old_arrow=item(ctx,p->main,id);
            const auto arrow=caption(ctx,p->main,id,title,p->text_storage+12288+(slot*2+side)*96,center+(side?58.f:-58.f)*view.grid_scale,-55.f+row,.46f*view.grid_scale,4,0);
            if(!old_arrow)set_text(ctx,arrow,side?">":"<",p->text_storage+13824+(slot*2+side)*8);
            caption_visibility(ctx,arrow,connected && local.slot==slot);
        }
        const bool bot=(bot_mask&(uint64_t(1)<<slot))!=0;
        auto role=std::to_string(slot+1)+"  "+tr(!connected?FrontText::LobbyOpenSlot:bot?FrontText::LobbyBot:slot==0?FrontText::LobbyHost:FrontText::LobbyPlayer);
        if(connected && local.slot==slot)role+=" - "+std::string(tr(FrontText::LobbyYou));
        if(connected && !bot && world->players[slot].afk)role+=" - AFK";
        if(connected && world->players[slot].spectator)role=std::to_string(slot+1)+"  "+tr(FrontText::Watching)+(local.slot==slot?" - "+std::string(tr(FrontText::LobbyYou)):std::string{});
        const auto old_role=item(ctx,p->main,0x7e10+slot);
        const auto status=caption(ctx,p->main,0x7e10+slot,title,p->text_storage+1792+slot*96,center,10.f+row,.6f);
        if(status && (!old_role || p->roles[slot]!=role)){fitted_text(ctx,status,role,p->text_storage+2560+slot*96,138.f*view.grid_scale);p->roles[slot]=role;}
        caption_visibility(ctx,status,true);
        const std::string ready=connected?tr(ready_mask&(uint64_t(1)<<slot)?FrontText::LobbyReady:FrontText::LobbyNotReady):"";
        const auto prior_ready=item(ctx,p->main,0x7e20+slot);
        const auto ready_label=caption(ctx,p->main,0x7e20+slot,title,p->text_storage+5120+slot*96,center,32.f+row,.6f);
        if(ready_label && (!prior_ready || p->readiness[slot]!=ready)){fitted_text(ctx,ready_label,ready,p->text_storage+5888+slot*96,138.f*view.grid_scale);p->readiness[slot]=ready;}
        caption_visibility(ctx,ready_label,connected);
        for(unsigned id:{0xa3u}){auto node=item(ctx,p->gui[slot],id);if(node){auto d=mem_read32(node+0x1c);auto flags=mem_read32(d+0xc);mem_write32(d+0xc,connected?flags&~0x20u:flags|0x20u);}}
        hide(ctx,p->gui[slot],0x9d);hide(ctx,p->gui[slot],0x9e);hide(ctx,p->gui[slot],0xa4);hide(ctx,p->gui[slot],0xa5);
    }
    for(unsigned slot=p->count;slot<8;++slot){caption_visibility(ctx,item(ctx,p->main,0x7e00+slot),false);caption_visibility(ctx,item(ctx,p->main,0x7e10+slot),false);caption_visibility(ctx,item(ctx,p->main,0x7e20+slot),false);for(unsigned side=0;side<2;++side)caption_visibility(ctx,item(ctx,p->main,0x7e30+slot*2+side),false);}
    // The original parent still renders its original title and background.
    auto clear_prompts=[&](uint32_t owner,unsigned id){if(const auto node=item(ctx,owner,id)){const auto definition=mem_read32(node+0x1c);for(unsigned button=0;button<6;++button)mem_write32(definition+0x10+button*4,0);}};
    clear_prompts(p->main,0x9a);for(auto id:{0x152u,0x153u,0x154u,0x158u,0x159u})clear_prompts(p->main,id);
    for(auto owner:p->gui)for(auto id:{0xa0u,0xa2u,0xa3u})clear_prompts(owner,id);
    invoke(ctx,0x800F86C8,ctx.r[3]);
    // The original versus-rule roots remain linked behind the roster. Their
    // animated map sprites outlive the rule text, so retire their own quads
    // while the lobby is visible. Their next ordinary update restores them
    // when Back opens the original rules again.
    for(unsigned id:{0x5bu,0x66u,0x78u,0x88u})if(const auto node=item(ctx,p->main,id)){
        const auto descriptor=mem_read32(node+0x1c);
        if(!lobby_map_native::readable(descriptor,4) || mem_read16(descriptor)!=6)continue;
        const auto sprite=mem_read32(node+0xb4);
        if(lobby_map_native::readable(sprite,12) && lobby_map_native::readable(mem_read32(sprite+8),4)){
            lobby_rule_map_sprites.insert(sprite);suppress_controller(ctx,sprite);
        }
    }
    for(unsigned slot=0;slot<8;++slot)refresh_ready_badge(ctx,*p,slot);
    for(unsigned slot=0;slot<8;++slot)hide(ctx,p->gui[slot],0xa2);
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
void close(PPCContext& ctx){auto p=current(mem_read32(ctx.r[4]+0x10));if(p){finish_ready_image();for(unsigned i=4;i<8;++i)invoke(ctx,0x800F6498,p->gui[i]);p->opened=false;visible.store(false);}ssne_call_original(ctx,0x800FF808,close_original);}
void level_close(PPCContext& ctx){
    const auto parent=mem_read32(ctx.r[4]+0x10);
    if(server && level_selection.owns(parent) && mem_read16(ctx.r[4]+0x18)==0x44){
        // The difficulty group shares the frontend root, but does not own the
        // level selector's preview resources. Its Back returns to level choice.
        if(level_selection.back_from_difficulty(parent)){
            invoke(ctx,0x800F6178,ctx.r[3],0x44);
            if(survival_mode)mem_write32(ctx.r[13]-0x69d4,2);
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
    selection_return_pending=true;
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
    if(!server->set_match_player_limit(mode==1 || survival_mode?8:4)){
        level_selection.reset();confirm_release.arm();
        if(auto p=current(parent)){invoke(ctx,0x800F60A0,p->main,0x9a,0);set_text(ctx,item(ctx,p->main,0x7ffe),frontend_text(FrontText::ModeFull,native_menu_language()),p->text_storage+768);}
        ssne::log_message("[native-mode-menu] versus start rejected: occupied slots above four");return;
    }
    const auto config=read_native_configuration(native_global_configuration(),prefix);
    const auto level=config.values.find(std::string(prefix)+".Level");
    const auto region=config.values.find(std::string(prefix)+".LevelSet");
    const auto* name=level==config.values.end()?nullptr:std::get_if<std::string>(&level->second);
    const auto* set=region==config.values.end()?nullptr:std::get_if<int64_t>(&region->second);
    // The Custom category borrows only the carousel's set1 table. Loading an
    // alias uses ordinary rules bounds and never persists that borrowed table.
    const auto selected_set=name && find_custom_level(*name)?int64_t(0):set?*set:int64_t(0);
    if(!config.error.empty() || !name || !set || !level_selection.confirm(parent,{*name,selected_set,1,survival_mode?5u:mode})){
        ssne::log_message("[coop-native-lobby] level selection rejected: missing or invalid native selection");
        level_selection.reset();confirm_release.reset();return;
    }
    ssne::log_message("[coop-native-lobby] original level confirmed: "+*name+" set="+std::to_string(*set));
    if(mode!=1 && !survival_mode){level_selection.confirm_difficulty(parent,1);selection_return_pending=true;ssne::log_message("[native-mode-menu] versus map confirmed: mode="+std::to_string(mode)+" level="+*name);return;}
    // Survival borrows mode2 only for the multiplayer-map catalogue. Its
    // native difficulty callbacks read RulesCoop, including the Difficulty%d
    // preview sprite; RulesFragmatch has no Difficulty field.
    if(survival_mode)mem_write32(ctx.r[13]-0x69d4,1);
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
    finish_ready_image();
    known_rosters.erase(ctx.r[3]);
    if(level_selection.owns(ctx.r[3])){level_selection.reset();confirm_release.reset();}
    uint32_t storage=0,heap=0,gui=0;
    auto found=panels.find(ctx.r[3]);
    if(found!=panels.end()){
        storage=found->second.text_storage;heap=found->second.heap;gui=found->second.main;
        for(unsigned i=4;i<8;++i){auto gui=found->second.gui[i],asset=mem_read32(gui+0x80);invoke(ctx,0x800F55B4,gui);auto vt=mem_read32(asset);invoke(ctx,mem_read32(vt+0xc),asset+int16_t(mem_read16(vt+8)),3);}
        panels.erase(found);
    }
    ssne_call_original(ctx,0x800FEE3C,destroy_original);
    for(auto it=ready_badge_quads.begin();it!=ready_badge_quads.end();)if(it->second.gui==gui)it=ready_badge_quads.erase(it);else ++it;
    if(storage)invoke(ctx,0x801161EC,heap,storage);
}
}
void install_native_lobby(AuthoritativeServer* host,AuthoritativeClient* peer){
    finish_ready_image();
    server=host;client=peer;visible.store(false);mode_chooser_opened=false;level_selection.reset();confirm_release.reset();model_keys={};applied_skin_players={};character_models.clear();model_catalog.clear();
    if(host)confirm_release.arm(); // Require release of the Host Start click before choosing a mode.
    survival_mode=false;lobby_mode=1;selection_return_pending=false;start_requested=false;starting_level.reset();selected_level_available=false;client_ready_pending.reset();client_ready_token=0;previous_lobby_rules={0,0,0};
    if(installed)return;installed=true;
    ssne::log_message("[coop-native-lobby] hooks installed");
    if(mem_read32(0x80000000)!=0x47334250u)throw std::runtime_error("Native lobby requires verified PAL revision");
    constexpr std::array<std::pair<uint32_t,uint64_t>,16> fingerprints{{
        {0x800FFFF8,0xcd3cd596b4c17856ull},{0x800FDF00,0x973a72db28f0d5beull},
        {0x80101200,0xbf817146b1678916ull},{0x800FCC5C,0x3d97f9088129e808ull},
        {0x800F31C8,0xe06797b4ec35e4b9ull},{0x800F2F54,0x990a68e73266a73dull},{0x800F2E50,0x1c26651fd0f6801bull},{0x800FEBD0,0xbaf1d15ee621e1f7ull},{0x800FF5C0,0x02fa05e9f78f3484ull},
        {0x800FEE3C,0xa718ae53517484ffull},{0x800FF808,0xb4072a236ce51babull},
        {0x800FF3A0,0x4b3ea9d31aaf531full},{0x800FF454,0x914aadda308ab578ull},
        {0x800FF8E4,0xfa490cbd98a9fb73ull},
        {0x8010A09C,0xcd64ba9d9eeaf04bull},{0x8010A32C,0x01db16f35f32107aull}}};
    for(auto [address,expected]:fingerprints){uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<32;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}if(hash!=expected || !ssne_find_func(address))throw std::runtime_error("Native lobby revision differs");}
    uint64_t material_hash=14695981039346656037ull;for(unsigned i=0;i<0x700;++i){material_hash^=mem_read8(0x8012680c+i);material_hash*=1099511628211ull;}
    const auto material=ssne_find_func(0x8012680c);if(!material || material_hash!=0xbc9108d7d5adddb0ull)throw std::runtime_error("Native Ready image material revision differs");
    const auto previous_material=*ssne_hook_slot(0x8012680c);ready_material_original=previous_material?previous_material:material->func;ssne_install_hook(0x8012680c,ready_material);
    difficulty_start_original=ssne_find_func(0x80101200)->func;ssne_install_hook(0x80101200,difficulty_start);
    struct Hook{uint32_t address;PPCFunc function;PPCFunc* original;};
    for(auto h:{Hook{0x800FE9AC,level_confirm,&level_confirm_original},Hook{0x800FFFF8,level_start,&level_start_original},Hook{0x800FDF00,level_close,&level_close_original},Hook{0x800F31C8,sprite_update,&sprite_update_original},Hook{0x800F2F54,sprite_destroy,&sprite_destroy_original},Hook{0x800F2E50,sprite,&sprite_original},Hook{0x800FEBD0,make,&make_original},Hook{0x800FF5C0,open,&open_original},Hook{0x800FEE3C,destroy,&destroy_original},Hook{0x800FF808,close,&close_original},Hook{0x800FF3A0,profiles,&profiles_original},Hook{0x800FF454,layout,&layout_original},Hook{0x800FF8E4,update,&update_original}}){
        const auto f=ssne_find_func(h.address);if(!f)throw std::runtime_error("Original lobby handler unavailable");if(h.original){const auto previous=*ssne_hook_slot(h.address);*h.original=previous?previous:f->func;}ssne_install_hook(h.address,h.function);
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
        preview_character(ctx,node,profile);
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
void publish_native_lobby_characters(Properties& mission){
    if(character_models.size()==8)mission["Session.CharacterModels"]=character_models;
    if(!server)return;
    mission["Session.Lobby.Mode"]=int64_t(lobby_mode);
    if(mem_read32(0x802DB7E8)!=3)return;
    const auto& selected=level_selection.selected()?level_selection.selected():starting_level;selected_level_available=bool(selected);
    mission["Session.Lobby.Level"]=selected?selected->level:std::string{};
    mission["Session.Lobby.LevelSet"]=selected?selected->level_set:int64_t(0);
    mission["Session.Lobby.Difficulty"]=selected?selected->difficulty:int64_t(1);
    // The proposal advertises content for prefetching, without changing the
    // authoritative active level/epoch or loading it before explicit Start.
    std::string identity_error;
    if(!bind_custom_level_identity(selected?selected->level:std::string{},selected && selected->mode!=5?selected->mode:1u,mission,identity_error,false)){
        mission["Session.Lobby.Level"]=std::string{};selected_level_available=false;
    }
    const auto options=native_frontend_options();const std::array<unsigned,3> rules{options.enemies,options.capacity,options.pickups};
    if(previous_lobby_rules!=rules){if(previous_lobby_rules[0])server->reset_lobby_readiness();previous_lobby_rules=rules;}
}
void deactivate_native_lobby(){finish_ready_image();server=nullptr;client=nullptr;visible=false;mode_chooser_opened=false;lobby_mode=1;level_selection.reset();confirm_release.reset();selection_return_pending=false;start_requested=false;starting_level.reset();selected_level_available=false;client_ready_pending.reset();previous_lobby_rules={0,0,0};for(auto& [id,p]:panels)p.opened=false;}
void suspend_native_lobby(PPCContext& ctx){finish_ready_image();for(auto& [id,p]:panels)if(p.opened){for(auto child:p.gui)invoke(ctx,0x800F6498,child);invoke(ctx,0x800F6178,p.main,0x9a);p.opened=false;}visible=false;mode_chooser_opened=true;}
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
bool native_lobby_survival(){return survival_mode.load();}
void set_native_lobby_survival(bool enabled){survival_mode.store(enabled);}
int native_lobby_mouse_owner(uint32_t gui){for(const auto& [parent,p]:panels)if(p.opened){if(p.main==gui)return -2;for(unsigned slot=0;slot<p.count;++slot)if(p.gui[slot]==gui)return int(slot);}return -1;}
uint32_t native_lobby_mouse_main(){for(const auto& [parent,p]:panels)if(p.opened)return p.main;return 0;}
std::array<uint32_t,8> native_lobby_mouse_guis(){for(const auto& [parent,p]:panels)if(p.opened)return p.gui;return {};}
uint16_t filter_native_lobby_confirmation(uint16_t buttons){return confirm_release.filter(buttons,ssne::PAD_A|ssne::PAD_START|ssne::PAD_B);}
std::optional<NativeLobbyLevel> take_native_lobby_level(){if(!server || !std::exchange(start_requested,false))return std::nullopt;return level_selection.take();}
bool native_lobby_has_level(){return selected_level_available.load();}
namespace {
int64_t lobby_integer(const std::shared_ptr<const WorldState>& world,const char* key){if(world){const auto field=world->mission.find(key);if(field!=world->mission.end())if(const auto value=std::get_if<int64_t>(&field->second))return *value;}return 0;}
bool lobby_map_available(const std::shared_ptr<const WorldState>& world){if(!world)return false;const auto field=world->mission.find("Session.Lobby.Level");if(field==world->mission.end())return false;const auto level=std::get_if<std::string>(&field->second);return level && !level->empty() && (!is_custom_level_alias(*level) || find_custom_level(*level));}
}
bool native_lobby_ready(){const auto world=server?server->latest_world():client?client->latest_world():nullptr;const auto slot=server?server->status().player.slot:client?client->status().player.slot:8u;return slot<8 && bool(uint64_t(lobby_integer(world,"Session.Lobby.ReadyMask"))&(uint64_t(1)<<slot));}
bool native_lobby_can_ready(){
    if(!native_lobby_visible() || (!server && !client))return false;
    const auto world=server?server->latest_world():client->latest_world();
    if(client_ready_pending){const auto token=lobby_integer(world,"Session.Lobby.ReadyToken");if(token!=client_ready_token || native_lobby_ready()==*client_ready_pending)client_ready_pending.reset();else return false;}
    return lobby_integer(world,"Session.Lobby.Open") && lobby_map_available(world);
}
bool native_lobby_can_start(){
    if(!server || !native_lobby_visible() || level_selection.phase()!=LobbyLevelSelection::Phase::Selected || !level_selection.selected())return false;
    const auto world=server->latest_world();if(!lobby_integer(world,"Session.Lobby.Open"))return false;
    const auto mask=uint64_t(lobby_integer(world,"Session.Lobby.ReadyMask"));
    for(const auto& player:world->players)if(player.connected && !(mask&(uint64_t(1)<<player.id.slot)))return false;
    return true;
}
void toggle_native_lobby_ready(){if(!native_lobby_can_ready())return;const bool ready=!native_lobby_ready();if(server)server->lobby_ready(ready);else{client_ready_token=lobby_integer(client->latest_world(),"Session.Lobby.ReadyToken");client_ready_pending=ready;client->lobby_ready(ready);}}
bool start_native_lobby_level(std::string& error){
    if(!native_lobby_can_start()){error=frontend_text(native_lobby_has_level()?FrontText::LobbyWaitingReady:FrontText::LobbySelectFirst,native_menu_language());return false;}
    if(!server->begin_lobby_game()){error=frontend_text(FrontText::LobbyWaitingReady,native_menu_language());return false;}
    starting_level=level_selection.selected();start_requested=true;confirm_release.arm();return true;
}
void cancel_native_lobby_level_selection(){level_selection.reset();confirm_release.reset();selection_return_pending=false;start_requested=false;selected_level_available=false;starting_level.reset();}
bool open_native_lobby_level_selection(PPCContext& ctx,std::string& error){
    if(!server){error="Only the host can choose the campaign level";return false;}
    if(mem_read32(0x802DB7E8)!=3 || mem_read32(0x802DB7EC)==4){error="Level selection requires the lobby scene";return false;}
    if(level_selection.phase()!=LobbyLevelSelection::Phase::Idle && level_selection.phase()!=LobbyLevelSelection::Phase::Selected)return true;
    for(auto& [parent,p]:panels)if(p.opened){
        if(!level_selection.begin(parent)){error="Native level selection is already active";return false;}
        selection_return_pending=false;confirm_release.arm();
        // These are eight independent GUIs, not children of the main GUI.
        // Close immediately so neither their artwork nor their input survives
        // into the original level selector's opening frame.
        for(auto gui:p.gui)invoke(ctx,0x800F6498,gui);
        invoke(ctx,0x800F6498,p.main);
        p.opened=false;visible.store(false);
        if(survival_mode)mem_write32(ctx.r[13]-0x69d4,2); // Original multiplayer-map carousel.
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
    if(level_selection.phase()==LobbyLevelSelection::Phase::Closed || (selection_return_pending && level_selection.phase()==LobbyLevelSelection::Phase::Selected)){
        const bool confirmed=selection_return_pending;selection_return_pending=false;
        if(confirmed){selected_level_available=true;if(server)server->reset_lobby_readiness();}else level_selection.cancel();
        confirm_release.arm();
        if(survival_mode)mem_write32(ctx.r[13]-0x69d4,1);
        const auto mode=mem_read32(ctx.r[13]-0x69d4);
        const unsigned options=survival_mode?0x64:mode==2?0x64:mode==3?0x76:mode==4?0x86:0x59;
        for(auto& [parent,p]:panels){invoke(ctx,0x800F6498,p.main);for(auto id:{0x3fu,options,0x9au})invoke(ctx,0x800F60A0,p.main,id,0);}
        return;
    }
    if(level_selection.phase()!=LobbyLevelSelection::Phase::Idle && level_selection.phase()!=LobbyLevelSelection::Phase::Selected)return;
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
