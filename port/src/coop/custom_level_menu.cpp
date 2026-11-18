#include "custom_level_menu.h"
#include "custom_levels_runtime.h"
#include "ps2_multiplayer_maps.h"
#include "native_configuration.h"
#include "native_frontend.h"
#include "native_lobby.h"
#include "../crosshair_image.h"
#include "../native_crosshair.h"
#include "ssne/custom_crosshair.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace port::coop { namespace {
constexpr uint32_t counts=0x8021a770,records=0x8023ac78,borrowed_set=1;
constexpr uint32_t prepare_address=0x800fd1c4,close_address=0x800fdcfc;
constexpr uint32_t refresh_address=0x800fe2cc,category_address=0x800feaac;
constexpr uint32_t open_address=0x800fd07c,accept_address=0x800fe9ac;
constexpr uint32_t sprite_image_address=0x800f306c,sprite_destroy_address=0x800f2f54;
constexpr uint32_t world_record_address=0x8010c858;
constexpr uint32_t material_return=0x8011e90c;
constexpr unsigned max_entries=256,memory_bytes=80*1024;
PPCFunc original_open=nullptr,original_prepare=nullptr,original_close=nullptr;
PPCFunc original_refresh=nullptr,original_category=nullptr,original_accept=nullptr;
PPCFunc original_sprite_image=nullptr,original_sprite_destroy=nullptr;
PPCFunc original_world_record=nullptr;
uint32_t guest_memory=0,guest_heap=0,owner_gui=0,owner=0,old_count=0,old_records=0;
bool installed=false,active=false,merged=false,building=false;
bool image_hooks_installed=false;
bool table_active(){return active || merged;}
std::vector<const gc::custom::Level*> entries;
std::map<std::string,uint32_t> base_names;
std::map<std::string,std::shared_ptr<const ssne::CustomCrosshairImage>> images;
std::map<uint32_t,std::shared_ptr<const ssne::CustomCrosshairImage>> sprite_images;
std::map<uint32_t,bool> score_visibility;
uint64_t image_matches=0;
uint64_t catalog_generation=0;
unsigned last_language=~0u;
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;}
uint32_t word(uint32_t p){return readable(p,4)?mem_read32(p):0;}
uint32_t call(PPCContext& base,uint32_t address,uint32_t a=0,uint32_t b=0,uint32_t d=0){
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);c.r[3]=a;c.r[4]=b;c.r[5]=d;ssne_call(c,address);return c.r[3];
}
std::string text(uint32_t p,unsigned limit=96){std::string s;for(unsigned n=0;n<limit && readable(p+n,1);++n){const auto c=mem_read8(p+n);if(!c)return s;s+=char(c);}return {};}
void guest_text(uint32_t p,const std::string& s,unsigned limit=127){const auto n=std::min<size_t>(limit,s.size());for(unsigned i=0;i<n;++i)mem_write8(p+i,uint8_t(s[i]));mem_write8(p+unsigned(n),0);}
std::string pal_text(const std::string& utf8){
    std::string out;for(size_t i=0;i<utf8.size() && out.size()<100;){const auto c=uint8_t(utf8[i++]);
        if(c<128)out+=char(c);else if(c>=0xc2 && c<=0xc3 && i<utf8.size()){const auto next=uint8_t(utf8[i++]);out+=next>=0x80&&next<=0xbf?char((unsigned(c&31)<<6)|(next&63)):'?';}
        else{while(i<utf8.size() && (uint8_t(utf8[i])&0xc0)==0x80)++i;out+='?';}}
    return out;
}
unsigned mode(PPCContext& c){return native_lobby_survival()?5:word(c.r[13]-0x69d4);}
unsigned original_category_for_mode(PPCContext& c){return mode(c)>=2?1u:0u;}
uint32_t base_record(const std::string& name){
    for(unsigned set=0;set<3;++set){const auto count=table_active() && set==borrowed_set?old_count:word(counts+set*4),table=table_active() && set==borrowed_set?old_records:word(records+set*4);
        if(count>256 || !readable(table,count*12))continue;
        for(unsigned n=0;n<count;++n)if(text(word(table+n*12+8))==name)return table+n*12;
    }
    return 0;
}
std::vector<const gc::custom::Level*> eligible(PPCContext& c){std::vector<const gc::custom::Level*> out;for(const auto* level:custom_levels_available())if(level && custom_level_in_user_category(*level,mode(c)) && out.size()<max_entries)out.push_back(level);return out;}
bool bundled_level(const gc::custom::Level& level){return custom_level_is_bundled_ps2(level);}
std::vector<const gc::custom::Level*> bundled_eligible(PPCContext& c){
    std::vector<const gc::custom::Level*> out;
    if(mode(c)<2 || mode(c)>5)return out;
    for(const auto* level:custom_levels_available())
        if(level && custom_level_supports_mode(*level,mode(c)) && bundled_level(*level))out.push_back(level);
    return out;
}
void restore_score_visibility(){
    for(const auto& [descriptor,hidden]:score_visibility)
        if(readable(descriptor,16))mem_write32(descriptor+0xc,(word(descriptor+0xc)&~0x20u)|(hidden?0x20u:0u));
    score_visibility.clear();
}

uint32_t item(PPCContext& c,uint32_t gui,unsigned id){return call(c,0x800f6030,gui,id);}
void set_text(PPCContext& c,unsigned id,const std::string& value,uint32_t scratch){
    const auto node=item(c,owner_gui,id);if(!node)return;const auto table=word(node+0x5c);if(!readable(table,0x78))return;
    guest_text(scratch,value);call(c,word(table+0x74),node+int16_t(mem_read16(table+0x70)),scratch);
}
void normalize_rules(PPCContext& c){
    const auto rules=call(c,0x8016742c);if(!rules)return;
    const auto config=read_native_configuration(rules);const auto level=config.values.find("Level");
    const auto* alias=level==config.values.end()?nullptr:std::get_if<std::string>(&level->second);
    if(alias && find_custom_level(*alias)){std::string error;write_native_configuration(c,rules,{{"LevelSet",int64_t(0)}},"LevelSet",error);}
}
void restore_table(){
    restore_score_visibility();
    if(!table_active())return;
    // Both the default merged list and the optional custom list borrow only
    // the original set1 slot. Campaign and Lost Levels tables never change.
    mem_write32(counts+borrowed_set*4,old_count);mem_write32(records+borrowed_set*4,old_records);
    active=merged=false;ssne::gx_custom_menu_image_scope({});
}
std::shared_ptr<const ssne::CustomCrosshairImage> preview(const gc::custom::Level& level){
    if(const auto old=images.find(level.alias);old!=images.end())return old->second;
    std::shared_ptr<const ssne::CustomCrosshairImage> result;
    if(!level.preview_path.empty()){
        port::CrosshairImage png;std::string error;
        if(port::load_crosshair_png(level.preview_path,png,error)){
            // Native thumbnails are square. Fit rather than distort arbitrary
            // user PNG proportions, with transparent letterboxing.
            auto image=std::make_shared<ssne::CustomCrosshairImage>();image->width=image->height=256;image->right=image->bottom=256;image->rgba.resize(256*256*4);
            const float scale=256.f/float(std::max(png.width,png.height));
            const unsigned w=std::max(1u,unsigned(png.width*scale)),h=std::max(1u,unsigned(png.height*scale));
            const unsigned left=(256-w)/2,top=(256-h)/2;
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto src=(size_t(std::min(png.height-1,unsigned(y/scale)))*png.width+std::min(png.width-1,unsigned(x/scale)))*4;
                const auto dest=(size_t(top+y)*256+left+x)*4;std::copy_n(png.rgba.begin()+src,4,image->rgba.begin()+dest);}
            result=std::move(image);
        }else ssne::log_message("[custom-level-menu] preview fallback: "+level.id+": "+error);
    }
    images.emplace(level.alias,result);return result;
}
void refresh_labels(PPCContext& c){
    if(!table_active() || !guest_memory || !readable(owner,0x54) || entries.empty())return;
    const auto selected=word(owner+0x30);if(selected>=entries.size())return;
    restore_score_visibility();
    const auto* level=entries[selected];
    // Null rows are unchanged native maps in the merged multiplayer list.
    // Their authored labels and scores were just refreshed by the game.
    if(!level){last_language=native_menu_language();return;}
    set_text(c,0x4d,custom_level_menu_category(*level,native_menu_language()),guest_memory+73728);
    set_text(c,0x4e,pal_text(level->title),guest_memory+73984);
    // Original campaign scores/medals are not records for an added package.
    for(unsigned id=0x50;id<=0x58;++id)if(const auto node=item(c,owner_gui,id)){const auto d=word(node+0x1c);if(readable(d,16)){score_visibility.try_emplace(d,(word(d+0xc)&0x20u)!=0);mem_write32(d+0xc,word(d+0xc)|0x20u);}}
    last_language=native_menu_language();
}
void prepare_records(PPCContext& c,uint32_t gui,uint32_t parent,bool include_original=false){
    auto added=include_original?bundled_eligible(c):eligible(c);if(added.empty())return;
    if(!guest_memory){guest_heap=word(c.r[13]-0x6de0);guest_memory=call(c,0x80116084,guest_heap,memory_bytes);if(!guest_memory)throw std::runtime_error("Custom level menu allocation failed");}
    old_count=word(counts+borrowed_set*4);old_records=word(records+borrowed_set*4);
    if(!old_count || old_count>256 || !readable(old_records,old_count*12))throw std::runtime_error("Original map category is unavailable");
    const unsigned first=include_original?old_count:0;
    if(first+added.size()>max_entries)throw std::runtime_error("Combined multiplayer map category is too large");
    entries.assign(first,nullptr);entries.insert(entries.end(),added.begin(),added.end());
    base_names.clear();
    // Preserve every original record and its ordering, including the first
    // two native string IDs. New maps follow the existing multiplayer maps.
    for(unsigned i=0;i<first*12;i+=4)mem_write32(guest_memory+i,word(old_records+i));
    for(unsigned i=first;i<entries.size();++i){const auto& level=*entries[i];const auto row=guest_memory+i*12,strings=guest_memory+4096+i*256;
        auto source=base_record(level.base_level);
        if(!source)source=old_records;
        mem_write32(row,word(source));mem_write32(row+4,word(source+4));guest_text(strings,level.alias,63);guest_text(strings+64,level.base_level,63);mem_write32(row+8,strings);base_names[level.alias]=strings+64;
    }
    owner_gui=gui;owner=parent;active=!include_original;merged=include_original;
    mem_write32(records+borrowed_set*4,guest_memory);mem_write32(counts+borrowed_set*4,unsigned(entries.size()));
    ssne::log_message(std::string("[custom-level-menu] prepared ")+(merged?"multiplayer+ps2":"custom")+" originals="+std::to_string(first)+" added="+std::to_string(added.size()));
}
void rebuild(PPCContext& c,bool custom,unsigned category=0){
    const auto gui=owner_gui,parent=owner;if(!gui || !parent)return;
    call(c,close_address,parent);restore_table();
    if(custom){prepare_records(c,gui,parent);if(!active)return;}
    else if(category==borrowed_set && !bundled_eligible(c).empty())prepare_records(c,gui,parent,true);
    const auto actual_mode=word(c.r[13]-0x69d4);if(custom)mem_write32(c.r[13]-0x69d4,2);
    building=true;call(c,prepare_address,parent,gui,custom?borrowed_set:category);building=false;
    mem_write32(c.r[13]-0x69d4,actual_mode);
    const auto event=c.r[1]-0x100;for(unsigned i=0;i<0x20;i+=4)mem_write32(event+i,0);mem_write32(event+0x10,parent);
    call(c,refresh_address,gui,event);call(c,0x800f86c8,gui);
    ssne::log_message(std::string("[custom-level-menu] category=")+(custom?"custom":"original")+" count="+std::to_string(word(parent+0x2c)));
}
void open_hook(PPCContext& c){
    if(table_active())restore_table();owner_gui=c.r[3];owner=word(c.r[4]+0x10);
    ssne_call_original(c,open_address,original_open);
    if(!eligible(c).empty()){
        const auto gui=owner_gui,parent=owner;auto bind=c;bind.r[1]-=0x200;bind.msr&=~0x8000u;mem_write32(bind.r[1],c.r[1]);bind.r[3]=gui;bind.r[4]=parent;bind.r[5]=0x4b;bind.r[6]=25;bind.r[7]=category_address;ssne_call(bind,0x800f6c14);
        const auto rules=call(c,0x8016742c);const auto config=read_native_configuration(rules);
        if(const auto found=config.values.find("Level");found!=config.values.end())if(const auto alias=std::get_if<std::string>(&found->second);alias && find_custom_level(*alias) && !(merged && bundled_level(*find_custom_level(*alias))))rebuild(c,true);
    }
}
void prepare_hook(PPCContext& c){
    // The normal multiplayer category includes the bundled maps immediately.
    // Normalize the carousel category only: custom aliases deliberately keep
    // LevelSet=0 in game rules for the existing native loader/network path.
    if(!building && !table_active() && !bundled_eligible(c).empty()){
        prepare_records(c,c.r[4],c.r[3],true);c.r[5]=borrowed_set;
    }
    ssne_call_original(c,prepare_address,original_prepare);
}
void close_hook(PPCContext& c){const auto parent=c.r[3];ssne_call_original(c,close_address,original_close);if(!building && parent==owner)restore_table();}
void refresh_hook(PPCContext& c){
    const bool custom=active && word(c.r[4]+0x10)==owner;const auto actual=word(c.r[13]-0x69d4);
    if(custom)mem_write32(c.r[13]-0x69d4,2);
    if(table_active())restore_score_visibility();
    ssne_call_original(c,refresh_address,original_refresh);
    if(custom)mem_write32(c.r[13]-0x69d4,actual);
    if(table_active())refresh_labels(c);
}
void category_hook(PPCContext& c){
    if(eligible(c).empty()){ssne_call_original(c,category_address,original_category);return;}
    const auto event=word(c.r[4]+0x14),parent=word(c.r[4]+0x10);
    owner_gui=c.r[3];owner=parent;
    if(event!=25){ssne_call_original(c,category_address,original_category);if(table_active())refresh_labels(c);return;}
    call(c,0x800f9b74,0x1c7);
    if(active)rebuild(c,false,original_category_for_mode(c));
    else if(word(parent+0x48)==2 || mode(c)>=2)rebuild(c,true);
    else{
        // Keep the native Lost Levels gate; a new catalogue never unlocks it.
        const auto scratch=c.r[1]-0x80;guest_text(scratch,"Rlevel1_3");
        if(call(c,0x800c32f8,scratch))rebuild(c,false,2);else rebuild(c,true);
    }
}
void accept_hook(PPCContext& c){ssne_call_original(c,accept_address,original_accept);normalize_rules(c);}
void world_record_hook(PPCContext& c){
    // Continue/lobby metadata lookups must recognize an alias even after its
    // carousel resources have closed. The immutable original base record is
    // sufficient there; it does not replace the selected alias in game rules.
    if(const auto* level=find_custom_level(text(c.r[3])))if(const auto record=base_record(level->base_level)){c.r[3]=record;return;}
    ssne_call_original(c,world_record_address,original_world_record);
}
void image_hook(PPCContext& c){
    const auto sprite=c.r[3];sprite_images.erase(sprite);
    // Mode names select authored subimages in the native FrontEndA atlas.
    // Keep its texture binding and UV lookup, even if a map shares a name.
    if(c.lr==0x800ff148u){
        ssne_call_original(c,sprite_image_address,original_sprite_image);
        return;
    }
    const auto alias=text(c.r[5]);
    const auto* level=find_custom_level(alias);
    if(level){const auto base=base_names.find(alias);if(base!=base_names.end())c.r[5]=base->second;else if(const auto record=base_record(level->base_level))c.r[5]=word(record+8);}
    ssne_call_original(c,sprite_image_address,original_sprite_image);
    if(level)if(auto image=preview(*level))sprite_images.emplace(sprite,std::move(image));
}
void sprite_destroy_hook(PPCContext& c){sprite_images.erase(c.r[3]);if(sprite_images.empty())ssne::gx_custom_menu_image_scope({});ssne_call_original(c,sprite_destroy_address,original_sprite_destroy);}
void material_observer(uint32_t caller,uint32_t quad){
    if(sprite_images.empty())return;
    std::shared_ptr<const ssne::CustomCrosshairImage> image;
    if(caller==material_return)for(const auto& [sprite,candidate]:sprite_images)if(word(sprite+8)==quad && word(quad)==0x8021cb58u){image=candidate;++image_matches;break;}
    ssne::gx_custom_menu_image_scope(std::move(image));
}
void ensure_menu_image_hooks(){
    if(image_hooks_installed)return;
    if(word(0x80000000)!=0x47334250u)throw std::runtime_error("Native menu images require the PAL game");
    struct Hook{uint32_t address;PPCFunc hook;PPCFunc* original;};
    const std::array hooks{Hook{sprite_image_address,image_hook,&original_sprite_image},Hook{sprite_destroy_address,sprite_destroy_hook,&original_sprite_destroy}};
    for(const auto& h:hooks){const auto f=ssne_find_func(h.address);if(!f)throw std::runtime_error("Original native menu image handler is unavailable");const auto previous=*ssne_hook_slot(h.address);*h.original=previous?previous:f->func;}
    for(const auto& h:hooks)ssne_install_hook(h.address,h.hook);
    port::set_native_material_observer(material_observer);image_hooks_installed=true;
}
std::string json_quote(const std::string& s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';}
} // namespace

void bind_native_menu_image(uint32_t sprite,std::shared_ptr<const ssne::CustomCrosshairImage> image){
    if(!image){sprite_images.erase(sprite);if(sprite_images.empty())ssne::gx_custom_menu_image_scope({});return;}
    if(!readable(sprite,12))return;
    ensure_menu_image_hooks();sprite_images[sprite]=std::move(image);
}
void install_custom_level_menu(){
    ensure_menu_image_hooks();
    if(installed || custom_levels_available().empty())return;
    if(word(0x80000000)!=0x47334250u)throw std::runtime_error("Custom level menu requires the PAL game");
    struct Hook{uint32_t address;PPCFunc hook;PPCFunc* original;};
    const std::array hooks{Hook{open_address,open_hook,&original_open},Hook{prepare_address,prepare_hook,&original_prepare},Hook{close_address,close_hook,&original_close},Hook{refresh_address,refresh_hook,&original_refresh},Hook{category_address,category_hook,&original_category},Hook{accept_address,accept_hook,&original_accept},Hook{world_record_address,world_record_hook,&original_world_record}};
    for(const auto& h:hooks){const auto f=ssne_find_func(h.address);if(!f)throw std::runtime_error("Original custom level menu handler is unavailable");const auto previous=*ssne_hook_slot(h.address);*h.original=previous?previous:f->func;}
    for(const auto& h:hooks)ssne_install_hook(h.address,h.hook);
    installed=true;ssne::log_message("[custom-level-menu] native carousel hooks installed");
}
void custom_level_menu_catalog_changed(PPCContext& c){
    const auto generation=custom_levels_generation();if(generation==catalog_generation)return;
    catalog_generation=generation;install_custom_level_menu();
    // Existing native sprites retain shared image ownership until their own
    // destruction. Subsequent opens load the verified host thumbnail anew.
    images.clear();
    // Do not reopen a hidden carousel during the connection transition. Its
    // next original open obtains the new choices and constructs new sprites.
    if(table_active())refresh_labels(c);
}
std::string custom_level_menu_category(const gc::custom::Level& level,unsigned language){
    return custom_level_is_bundled_ps2(level)?bundled_ps2_map_category(level.id,language):frontend_text(FrontText::CustomLevels,language);
}
bool custom_level_menu_available(PPCContext&){return installed;}
bool custom_level_menu_has_user_levels(PPCContext& c){return installed && !eligible(c).empty();}
bool custom_level_menu_active(){return active;}
uint32_t custom_level_menu_gui(){return owner_gui;}
void custom_level_menu_toggle(PPCContext& c,uint32_t gui){if(!installed || eligible(c).empty())return;owner_gui=gui;if(!owner)return;call(c,0x800f9b74,0x1c7);rebuild(c,!active,original_category_for_mode(c));}
void custom_level_menu_forget_gui(PPCContext& c,uint32_t gui){
    if(gui!=owner_gui)return;restore_table();sprite_images.clear();ssne::gx_custom_menu_image_scope({});
    if(guest_memory)call(c,0x801161ec,guest_heap,guest_memory);guest_memory=guest_heap=owner_gui=owner=0;entries.clear();base_names.clear();
}
std::string custom_level_menu_diagnostics(){
    const auto selected=word(owner+0x30);const auto* level=table_active() && selected<entries.size()?entries[selected]:nullptr;
    std::ostringstream out;out<<"{\"installed\":"<<installed<<",\"active\":"<<active<<",\"merged\":"<<merged<<",\"standard_bundled\":"<<merged<<",\"original_count\":"<<(merged?old_count:0)<<",\"bundled_count\":"<<(merged?entries.size()-old_count:0)<<",\"gui\":"<<owner_gui<<",\"owner\":"<<owner<<",\"count\":"<<(table_active()?entries.size():0)<<",\"selected\":"<<selected<<",\"alias\":"<<json_quote(level?level->alias:"")<<",\"title\":"<<json_quote(level?level->title:"")<<",\"preview_matches\":"<<image_matches<<",\"rendered_custom_previews\":"<<ssne::custom_menu_image_replacements()<<",\"loaded_images\":"<<images.size()<<",\"original_table_restored\":"<<(!table_active() && (!old_records || (word(records+4)==old_records && word(counts+4)==old_count)))<<"}";return out.str();
}
void custom_level_menu_frame(PPCContext& c){
    if(custom_levels_generation()!=catalog_generation)custom_level_menu_catalog_changed(c);
    if(table_active() && last_language!=native_menu_language())refresh_labels(c);
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_DEV_NATIVE_MENU"))return;
    static auto next=std::chrono::steady_clock::time_point{};const auto now=std::chrono::steady_clock::now();if(now<next)return;next=now+std::chrono::milliseconds(250);
    std::ofstream out("native-custom-level-state.json.pending",std::ios::binary|std::ios::trunc);out<<custom_level_menu_diagnostics();out.close();if(out){std::remove("native-custom-level-state.json");std::rename("native-custom-level-state.json.pending","native-custom-level-state.json");}
}
} // namespace port::coop
