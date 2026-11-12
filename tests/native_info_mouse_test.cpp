#include "../port/src/native_menu_mouse.h"
#include "ssne/native_party.h"
#include "ssne/game_addresses.h"
#include "ssne/recomp_hooks.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
namespace ssne {
std::array<uint8_t,kRamSize> ram{};uint8_t* g_ram=ram.data();uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return kRamSize;}uint32_t guest_ram_end(){return 0x80000000u+kRamSize;}
uint32_t mmio_read(uint32_t,int){std::abort();}void mmio_write(uint32_t,uint32_t,int){std::abort();}
uint32_t native_party_registry(){return 0x81110000;}unsigned native_party_capacity(){return 8;}
bool native_movie_presenting(){return false;}
}
namespace {
constexpr uint32_t gui=0x81000000,root=0x81000200,rootdef=0x81001000,owner=0x81002000,actor=0x81100000;
constexpr uint32_t body=0x81003000,left=0x81003200,right=0x81003400;
std::vector<std::pair<uint32_t,unsigned>> calls;
bool retire_after_step=false;
void check(bool x,const char* why){if(!x){std::cerr<<why<<'\n';std::abort();}}
void original(PPCContext&){}
void bytes(uint32_t address,const char* text){for(unsigned i=0;text[i];i+=2){char pair[]={text[i],text[i+1],0};mem_write8(address+i/2,uint8_t(std::strtoul(pair,nullptr,16)));}}
void make_node(uint32_t p,uint32_t definition,uint16_t id,unsigned type){mem_write32(p+0x1c,definition);mem_write32(p+0x20,gui);mem_write32(p+0x24,root);mem_write16(definition,uint16_t(type));mem_write16(definition+2,id);}
void sprite(uint32_t wrapper,uint32_t object,uint32_t parameters,uint32_t alpha,float visible=1){mem_write32(wrapper,parameters);mem_write32(wrapper+8,object);mem_write32(parameters+0x18,alpha);mem_write32(alpha,f2u(visible));}
}
const SSNEFuncEntry g_ssne_funcs[]={{0x801019d0,original,"Info ctor"},{0x80102240,original,"scroll"},{0x80102278,original,"page"},{0x801022ec,original,"close"},{0x801024b8,original,"refresh"},{0x800fb130,original,"native GUI event"}};
const size_t g_ssne_func_count=std::size(g_ssne_funcs);const uint32_t g_ssne_entry=0;const bool g_ssne_has_game_code=true;
const SSNEFuncEntry* ssne_find_func(uint32_t fn){for(const auto& f:g_ssne_funcs)if(f.address==fn)return &f;return nullptr;}
void ssne_call(PPCContext& c,uint32_t fn){
    check(fn==0x800fb130,"Bridge bypassed original native GUI dispatcher");calls.emplace_back(c.r[3],c.r[4]);
    if(c.r[4]==20||c.r[4]==21){const auto n=mem_read32(owner+0x5c),old=mem_read32(owner+0x60);mem_write32(owner+0x60,(old+n+(c.r[4]==20?-1:1))%n);if(retire_after_step)mem_write32(owner+0x6c,0);}
}
#include "../port/src/coop/native_info_mouse.inc"
int main(){
    mem_write32(0x80000000,0x47334250);
    bytes(0x801019d0,"9421ffd87c0802a6bf21000c9001002c7c7f1b787cb92b787c9d23784bf996a5");
    bytes(0x80102240,"9421fff07c0802a6bfc10008900100147c9e2378388000e64bff3dd9809e0014");
    bytes(0x80102278,"9421fff07c0802a6bfc100089001001483e400107c7e1b78801f005c2c000001");
    bytes(0x801022ec,"9421ffe07c0802a693e1001c900100247c7f1b783c60802238638fdc4cc63182");
    bytes(0x801024b8,"9421ffd07c0802a6dbe10028bf410010900100347c7d1b787c9e2378388000ed");
    mem_write32(0x8028bfa4,gui);mem_write32(gui+0x6c,root);mem_write32(gui+0x7c,root);make_node(root,rootdef,0xe3,0);mem_write32(root+0x24,0);mem_write32(rootdef+0xc,0x80);
    mem_write32(gui+0x94,0x81001200);
    unsigned k=0;for(auto [action,fn]:{std::pair{20u,0x80102278u},{21u,0x80102278u},{28u,0x80102240u},{29u,0x80102240u},{23u,0x801022ecu}}){const uint32_t p=0x81001200+32*k++;mem_write32(p+8,k<5?p+32:0);mem_write32(p+0x10,owner);mem_write32(p+0x14,action);mem_write16(p+0x18,0xe3);mem_write32(p+0x1c,fn);}
    mem_write32(owner+0x48,0x80219030);mem_write32(owner+0x94,gui);mem_write32(owner+0x90,actor);mem_write32(owner+0x6c,0x81002500);mem_write32(owner+0x5c,3);mem_write32(owner+0x60,0);mem_write32(owner+0x4c,0x81004000);
    mem_write32(actor+0x48,ssne::game_address(0x80212268u));mem_write32(actor+0x17c,5);mem_write32(ssne::native_party_registry()+20,actor);mem_write16(gui+0x84,1);
    make_node(body,0x81001100,0xe6,1);mem_write32(root+0x34,body);mem_write32(body+8,left);make_node(left,0x81001150,0xed,6);mem_write32(left+8,right);make_node(right,0x810011a0,0xee,6);mem_write32(0x81001150+0xc,0x30);mem_write32(0x810011a0+0xc,0x10);
    for(unsigned i=0;i<3;++i){const uint32_t e=0x81004000+i*0x40,w=0x81005000+i*0x100;mem_write32(e+8,i<2?e+0x40:0);mem_write32(e+0x30,w);sprite(w,w+0x40,w+0x90,w+0xc0);}
    mem_write32(body+0xd8,0x81006000);sprite(0x81006000,0x81006040,0x81006090,0x810060c0);mem_write32(0x81006090+0x10,0x810060e0);mem_write8(0x810060e0+3,0);mem_write32(body+0xdc,0x81006100);sprite(0x81006100,0x81006140,0x81006190,0x810061c0);
    mem_write32(right+0xb4,0x81006200);sprite(0x81006200,0x81006240,0x81006290,0x810062c0);
    PPCContext c;c.r[1]=0x83ff0000;c.r[3]=0x12345678;const auto stack=c.r[1];
    check(native_info_mouse::local_gui(gui,5),"Info ownership must follow owning player/registry, including slot5, rather than GUIport1");check(!native_info_mouse::local_gui(gui,1),"Foreign player cannot acquire this Info GUI");
    auto regions=native_info_mouse::regions(c);check(regions.size()==6,"Expected body, three real tabs, one visible body arrow and one visible page arrow");check(regions[0].event==0&&regions[0].viewport_node==body&&regions[0].previous_event==28&&regions[0].next_event==29,"Body uses the original text viewport and only original scroll events");
    check(std::none_of(regions.begin(),regions.end(),[](const auto& r){return r.control=="info.scroll.previous"||r.control=="info.page.previous";}),"Hidden original arrows must have no hit region");
    const auto callback_count=calls.size();
    mem_write32(rootdef+0xc,0xc0);mem_write32(0x810011a0+0xc,0x50);
    check(native_info_mouse::local_gui(gui,5)&&!native_info_mouse::local_gui(gui,1),"A live native overlay retains exact local ownership isolation");
    check(native_info_mouse::regions(c).size()==regions.size(),"Overlay flag40 must preserve visible body/tab/page-arrow regions");
    check(native_info_mouse::event(c,gui,root,0xe3,0x5200,28)&&calls.size()==callback_count+1&&calls.back()==std::pair{root,28u},"Valid overlay must dispatch only through original scroll callback");calls.pop_back();
    for(const auto flag:{0x20u,0x02u}){
        mem_write32(rootdef+0xc,0xc0|flag);const auto count=calls.size();
        check(!native_info_mouse::local_gui(gui,5)&&native_info_mouse::regions(c).empty()&&!native_info_mouse::event(c,gui,root,0xe3,0x5200,28)&&calls.size()==count,"Hidden or closing overlays must revoke ownership/regions/actions");
        const auto diagnostic=native_info_mouse::diagnostics();
        check(diagnostic.find(flag==0x20?"hidden_root":"closing_root")!=std::string::npos&&diagnostic.find("\"owner_state\":false")!=std::string::npos,"Copied linked-root diagnostic must explain rejected Info surface");
    }
    mem_write32(rootdef+0xc,0x80);mem_write32(0x810011a0+0xc,0x10);
    check(native_info_mouse::diagnostics().find("\"reason\":\"accepted\"")!=std::string::npos,"Live Info diagnostic must preserve accepted owner status");
    mem_write8(0x810060e0+3,255);mem_write8(0x810060e0+7,0);const auto fading=native_info_mouse::regions(c);
    check(std::any_of(fading.begin(),fading.end(),[](const auto& r){return r.control=="info.scroll.previous";}),"Arrow hit region must follow current rendered alpha, not its fading target");mem_write8(0x810060e0+3,0);
    check(native_info_mouse::event(c,gui,root,0xe3,0x5002,22),"Clicking tab2 must traverse its original selection callbacks");check(calls.size()==2&&calls[0]==std::pair{root,21u}&&calls[1]==std::pair{root,21u}&&mem_read32(owner+0x60)==2,"Tab callback traversal must select exactly the requested original entry");
    check(c.r[1]==stack&&c.r[3]==0x12345678,"Original callback changed caller PPC context");
    check(native_info_mouse::event(c,gui,root,0xe3,0x5200,28)&&calls.back()==std::pair{root,28u},"Wheel-up forwards through E3's original scroll callback");
    check(native_info_mouse::event(c,gui,root,0xe3,0x5002,20)&&mem_read32(owner+0x60)==1,"Wheel over a tab moves one previous entry rather than jumping to the hovered tab");
    check(native_info_mouse::event(c,gui,root,0xe3,0x5002,23)&&calls.back()==std::pair{root,23u},"Right Back over custom Info regions preserves original root close");
    check(!native_info_mouse::event(c,gui,root,0xe3,0x5200,0),"A body click must not fabricate a native action");
    const auto before=calls.size();check(!native_info_mouse::event(c,gui,root,0xbc,0x5000,20)&&!native_info_mouse::event(c,gui,root,0xe3,0x51ff,22)&&calls.size()==before,"Wrong root/out-of-range tab must not dispatch");
    mem_write32(owner+0x60,0);retire_after_step=true;check(!native_info_mouse::event(c,gui,root,0xe3,0x5002,22)&&calls.size()==before+1,"Retired Info owner must stop a bounded multi-step click before another callback");retire_after_step=false;check(native_info_mouse::regions(c).empty()&&!native_info_mouse::local_gui(gui,5),"Closed Info owner must retire all mouse regions and ownership");
    mem_write32(owner+0x6c,0x81002500);mem_write32(0x81004000+8,0x81004000);check(native_info_mouse::regions(c).empty(),"Cyclic/stale tab registry must fail closed");
    std::cout<<"Info mouse ownership, slot5 isolation, visible native primitives, scroll routing, overlay visibility, linked-root diagnostics, bounded tab callbacks and stale lifecycle passed (native dispatcher mocked).\n";
}
