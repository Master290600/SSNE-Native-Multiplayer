#include "ssne/native_session_hud.h"
#include "ssne/runtime.h"
#include "ssne/game_addresses.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/native_cutscene.h"
#include "ssne/native_results.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <exception>
#include <utility>
namespace ssne { namespace native_session_hud {
constexpr uint32_t drawable_vtable=0x8021CAB0u,text_vtable=0x80211428u,tag=0x53485544u;
constexpr unsigned wrapper_bytes=0xF0,stack_bytes=0x3000;
// Leave the original top-center score and versus timer unobstructed.
constexpr float row_x=-300.f,title_y=-160.f,detail_y=-140.f,title_scale=.55f,detail_scale=.45f;
constexpr uint32_t shutdown_address=0x80166480u;
struct Identity {uint32_t scene=0,root=0,heap=0,font=0,engine=0,heap_entry=0;bool operator==(const Identity&)const=default;};
struct Rows {Identity owner{};std::array<uint32_t,2> widget{};uint32_t head=0;};
Rows rows;PPCFunc previous_shutdown=nullptr;bool installed=false,busy=false;
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;}
bool context_ok(const PPCContext& c){return c.r[13]==0x802E22A0u && c.r[2]==0x802F22A0u && !(c.r[1]&7u) && readable(c.r[1]-stack_bytes,stack_bytes) && c.r[1]>=0x80005000u;}
constexpr std::array<std::pair<uint32_t,uint32_t>,12> functions{{
    {0x800AB5ACu,0x9421FFD0u},{0x800AB7C8u,0x9421FFD0u},{0x800AB994u,0x9421FFF0u},
    {0x800ABBE4u,0x9421FFE8u},{0x800AB728u,0x9421FFE0u},{0x800AB6B0u,0x9421FFF8u},
    {0x800AB6ECu,0x9421FFF8u},{0x80116084u,0x9421FFE8u},{0x801161ECu,0x9421FFE8u},
    {0x8011D15Cu,0x90830040u},{0x8011D164u,0x9421FFF0u},{shutdown_address,0x9421FFE8u}
}};
bool revision(){
    if(mem_read32(0x80000000u)!=0x47334250u || mem_read32(drawable_vtable+0xCu)!=0x8011D164u || mem_read32(drawable_vtable+0x44u)!=0x8011D458u || mem_read32(text_vtable+0x1Cu)!=0x800ABBE4u)return false;
    for(auto [address,opcode]:functions)if(!ssne_find_func(address) || mem_read32(address)!=opcode)return false;
    return true;
}
bool identity(PPCContext& c,Identity& out,uint32_t scene=0){
    if(!context_ok(c) || !revision())return false;
    out.scene=scene?scene:native_game_scene();out.heap=mem_read32(c.r[13]-0x6DE0u);out.engine=mem_read32(0x802ABAA0u);out.font=mem_read32(0x802ABAC4u);
    // The native allocator takes a small heap handle. Its descriptor table
    // contains 24-byte entries; heap zero is the normal PAL gameplay heap.
    const auto heaps=mem_read32(c.r[13]-0x5CBCu),count=mem_read32(c.r[13]-0x5CB8u);
    if(!count || count>256 || out.heap>=count || !readable(heaps,count*24))return false;
    out.heap_entry=heaps+out.heap*24;const auto capacity=mem_read32(out.heap_entry);
    if(capacity<wrapper_bytes || capacity>guest_ram_size())return false;
    if(!readable(out.scene,0xA8) || !readable(out.engine,0x40) || mem_read32(out.engine)!=0x8021C600u || !readable(out.font,4))return false;
    const auto font_table=mem_read32(out.font);if(!readable(font_table,0x38) || !ssne_find_func(mem_read32(font_table+0x2C)) || !ssne_find_func(mem_read32(font_table+0x34)))return false;
    out.root=mem_read32(out.scene+0x78);return readable(out.root,0x44) && mem_read32(out.root)==drawable_vtable && mem_read32(out.root+8)==out.engine && mem_read32(out.root+4)==out.engine+4;
}
bool owned(const Identity& id,uint32_t widget,unsigned line){
    if(!readable(widget-24,wrapper_bytes+24) || mem_read32(widget-20)!=id.heap_entry || mem_read32(widget-24)<wrapper_bytes || mem_read32(widget-24)>4096 || mem_read32(widget+0xC4)!=text_vtable || mem_read32(widget+0xC8)!=tag || mem_read32(widget+0xCC)!=line || mem_read32(widget+0xD0)!=id.scene || mem_read32(widget+0xD4)!=id.root || mem_read32(widget+0xD8)!=id.heap || mem_read32(widget+0xDC)!=id.font || mem_read32(widget+0xEC)!=~tag)return false;
    const auto draw=mem_read32(widget),font=mem_read32(widget+0x14);if(mem_read32(widget+0xE0)!=font || mem_read32(widget+0xE4)!=mem_read32(widget+0x18) || mem_read32(widget+0xE8)!=draw)return false;
    if(!readable(draw,0x44) || mem_read32(draw)!=drawable_vtable || mem_read32(draw+0x14)!=id.root || mem_read32(draw+8)!=id.engine || mem_read32(draw+0x40)!=widget || !readable(font,4))return false;
    const auto table=mem_read32(font);return readable(table,0x18) && ssne_find_func(mem_read32(table+0x14));
}
bool adopt(const Identity& id){
    const auto head=mem_read32(id.root+0x18);
    if(rows.owner==id && rows.head==head && (!rows.widget[0] || owned(id,rows.widget[0],0)) && (!rows.widget[1] || owned(id,rows.widget[1],1)))return true;
    Rows found;found.owner=id;found.head=head;std::vector<uint32_t> visited;
    // Native user_data points back only to our tagged wrapper. This can safely
    // adopt a restored local checkpoint without trusting an old C++ pointer.
    for(auto child=head;child;child=mem_read32(child+0x10)){
        if(visited.size()>=256 || !readable(child,0x44) || mem_read32(child)!=drawable_vtable || mem_read32(child+0x14)!=id.root || std::find(visited.begin(),visited.end(),child)!=visited.end()){rows={};return false;}
        visited.push_back(child);const auto wrapper=mem_read32(child+0x40);
        if(!readable(wrapper,wrapper_bytes) || mem_read32(wrapper)!=child || mem_read32(wrapper+0xC8)!=tag)continue;
        const auto line=mem_read32(wrapper+0xCC);if(line>1 || !owned(id,wrapper,line))continue;
        if(found.widget[line]){rows={};return false;}found.widget[line]=wrapper;
    }
    rows=found;return true;
}
struct Scratch {
    PPCContext base;std::array<uint8_t,stack_bytes> bytes{};uint32_t low;
    explicit Scratch(PPCContext& c):base(c),low(c.r[1]-stack_bytes){std::memcpy(bytes.data(),host_ptr(low),bytes.size());}
    ~Scratch(){std::memcpy(host_ptr(low),bytes.data(),bytes.size());}
    uint32_t invoke(uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t d=0,float f1=0,float f2=0){
        auto c=base;c.msr&=~0x8000u;c.r[1]-=0x400;mem_write32(c.r[1],base.r[1]);c.r[3]=a;c.r[4]=b;c.r[5]=d;c.ps0[1]=c.ps1[1]=f1;c.ps0[2]=c.ps1[2]=f2;ssne_call(c,fn);return c.r[3];
    }
};
struct Busy {Busy(){busy=true;}~Busy(){busy=false;}};
std::string clean(const std::string& text){std::string out;out.reserve(std::min<size_t>(96,text.size()));for(unsigned char c:text){if(out.size()==96)break;if(c<32 || (c>=127 && c<160)){if(c=='\n' || c=='\r' || c=='\t')out+=' ';continue;}out+=char(c);}return out;}
void release_wrapper(Scratch& call,const Identity& id,uint32_t widget){
    if(!readable(widget-24,24) || mem_read32(widget-20)!=id.heap_entry)return;const auto size=mem_read32(widget-24);if(size<wrapper_bytes || size>4096)return;
    mem_write32(call.base.r[13]-0x6DF0u,mem_read32(call.base.r[13]-0x6DF0u)+size);call.invoke(0x801161ECu,id.heap,widget);
}
uint32_t create(Scratch& call,const Identity& id,unsigned line){
    const auto widget=call.invoke(0x80116084u,id.heap,wrapper_bytes);if(!readable(widget-24,wrapper_bytes+24) || mem_read32(widget-20)!=id.heap_entry)return 0;
    const auto size=mem_read32(widget-24);if(size<wrapper_bytes || size>4096)return 0;
    mem_write32(call.base.r[13]-0x6DF0u,mem_read32(call.base.r[13]-0x6DF0u)-size);
    for(unsigned i=0;i<wrapper_bytes;i+=4)mem_write32(widget+i,0);mem_write32(widget+0xC4,text_vtable);
    try{
        call.invoke(0x800AB5ACu,widget,id.root,4,row_x,line?detail_y:title_y);
        call.invoke(0x800AB7C8u,widget,id.font,0,line?detail_scale:title_scale);
        const auto draw=mem_read32(widget);if(!readable(draw,0x44) || mem_read32(draw)!=drawable_vtable || mem_read32(draw+0x14)!=id.root || mem_read32(draw+0x40))throw std::runtime_error("Native session HUD parent mismatch");
        mem_write32(widget+0xC8,tag);mem_write32(widget+0xCC,line);mem_write32(widget+0xD0,id.scene);mem_write32(widget+0xD4,id.root);mem_write32(widget+0xD8,id.heap);mem_write32(widget+0xDC,id.font);mem_write32(widget+0xE0,mem_read32(widget+0x14));mem_write32(widget+0xE4,mem_read32(widget+0x18));mem_write32(widget+0xE8,draw);mem_write32(widget+0xEC,~tag);
        call.invoke(0x8011D15Cu,draw,widget);call.invoke(0x800AB6ECu,widget);return widget;
    }catch(...){
        const auto draw=mem_read32(widget);if(readable(draw,0x44) && mem_read32(draw)==drawable_vtable && mem_read32(draw+0x14)==id.root){
            const auto font=mem_read32(widget+0x14);if(readable(font,4) && readable(mem_read32(font),0x18) && ssne_find_func(mem_read32(mem_read32(font)+0x14)))call.invoke(0x800AB994u,widget);
            call.invoke(0x8011D164u,draw,3);
        }
        release_wrapper(call,id,widget);throw;
    }
}
bool row_visible(uint32_t widget){return !(mem_read16(mem_read32(widget)+0x1E)&1u);}
void hide(Scratch& call){for(unsigned i=0;i<2;++i)if(owned(rows.owner,rows.widget[i],i) && row_visible(rows.widget[i]))call.invoke(0x800AB6ECu,rows.widget[i]);}
void end(PPCContext& c,uint32_t explicit_scene=0){
    Identity id;if(busy || !identity(c,id,explicit_scene) || !adopt(id))return;Busy guard;Scratch call(c);
    for(unsigned i=0;i<2;++i){const auto widget=rows.widget[i];if(!owned(id,widget,i))continue;
        const auto draw=mem_read32(widget);call.invoke(0x800AB6ECu,widget);call.invoke(0x8011D15Cu,draw,0);call.invoke(0x800AB994u,widget);call.invoke(0x8011D164u,draw,3);mem_write32(widget+0xC8,0);release_wrapper(call,id,widget);
    }
    rows={};
}
void shutdown(PPCContext& c){
    std::exception_ptr failure;try{end(c,c.r[3]);}catch(...){failure=std::current_exception();}
    // Original scene teardown must still run if an optional HUD callback fails.
    ssne_call_original(c,shutdown_address,previous_shutdown);if(failure)std::rethrow_exception(failure);
}
} // namespace native_session_hud
void install_native_session_hud(){
    using namespace native_session_hud;if(installed || !revision())return;
    auto slot=ssne_hook_slot(shutdown_address);if(!slot)return;previous_shutdown=*slot?*slot:ssne_find_func(shutdown_address)->func;
    if(previous_shutdown==shutdown)return;ssne_install_hook(shutdown_address,shutdown);installed=true;log_message("[native-session-hud] original CcText shared HUD installed");
}
void clear_native_session_hud(PPCContext& c){
    using namespace native_session_hud;Identity id;if(busy || !identity(c,id) || !adopt(id))return;bool visible=false;for(unsigned i=0;i<2;++i)if(owned(id,rows.widget[i],i) && row_visible(rows.widget[i]))visible=true;if(!visible)return;Busy guard;Scratch call(c);hide(call);
}
void end_native_session_hud(PPCContext& c){native_session_hud::end(c);}
bool update_native_session_hud(PPCContext& c,const std::string& title,const std::string& detail,bool shown){
    using namespace native_session_hud;
    if(!context_ok(c) || !revision())return false;
    if(!shown || mem_read32(game_address(0x802DB0E0u))!=4 || local_pause_menu_open() || local_information_open() || native_movie_presenting() || native_cutscene_presenting() || native_results_presenting() || native_replica_results_presenting()){
        clear_native_session_hud(c);return true;
    }
    Identity id;if(busy || !identity(c,id) || !adopt(id))return false;Busy guard;Scratch call(c);
    const std::array<std::string,2> text{clean(title),clean(detail)};
    for(unsigned i=0;i<2;++i){if(!rows.widget[i])rows.widget[i]=create(call,id,i);if(!owned(id,rows.widget[i],i))return false;}
    for(unsigned i=0;i<2;++i){
        const auto widget=rows.widget[i];
        if(text[i].empty()){if(row_visible(widget))call.invoke(0x800AB6ECu,widget);continue;}
        std::string previous;for(unsigned k=0;k<128 && mem_read8(widget+0x1C+k);++k)previous+=char(mem_read8(widget+0x1C+k));
        if(previous!=text[i]){const auto buffer=c.r[1]-0x100;for(unsigned k=0;k<=text[i].size();++k)mem_write8(buffer+k,k<text[i].size()?uint8_t(text[i][k]):0);call.invoke(0x800ABBE4u,widget,buffer);}
        // The original HUD camera already maps its 640-unit GUI canvas to
        // the selected output aspect. Extra widescreen expansion clips left.
        const float x=row_x,y=i?detail_y:title_y;
        if(mem_read32(widget+4)!=f2u(x) || mem_read32(widget+8)!=f2u(y)){const auto buffer=c.r[1]-0x100;mem_write32(buffer,f2u(x));mem_write32(buffer+4,f2u(y));call.invoke(0x800AB728u,widget,buffer);}
        if(!row_visible(widget))call.invoke(0x800AB6B0u,widget);
    }
    rows.head=mem_read32(id.root+0x18);return true;
}
} // namespace ssne
