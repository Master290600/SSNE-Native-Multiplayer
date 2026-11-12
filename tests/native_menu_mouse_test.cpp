#include "../port/src/native_menu_mouse.h"
#include "ssne/recomp_hooks.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>
namespace ssne {
std::array<uint8_t,kRamSize> ram{};uint8_t* g_ram=ram.data();uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return kRamSize;}uint32_t guest_ram_end(){return 0x80000000u+kRamSize;}
uint32_t mmio_read(uint32_t,int){std::abort();}void mmio_write(uint32_t,uint32_t,int){std::abort();}
int mouse_test_owner=-1;int native_party_owner(){return mouse_test_owner;}void native_party_owner_set(int x){mouse_test_owner=x;}
void audio_track_weapon_voice(uint32_t,uint32_t,uint32_t,uint64_t){}
uint32_t native_party_view_engine(uint32_t x){return x;}uint32_t native_party_view_slot(uint32_t x){return mouse_test_owner==5&&x==11?13:x;}
}
namespace {
std::vector<std::tuple<uint32_t,uint32_t,uint32_t,uint32_t>> calls;
void check(bool condition,const char* message){if(!condition){std::cerr<<message<<'\n';std::abort();}}
void stub(PPCContext&){}
constexpr uint32_t gui=0x81000000,root=0x81000100,row=0x81000300,rootdef=0x81001000,rowdef=0x81001100;
uint64_t sequence=0;bool refuse_focus=false;
void write_float(uint32_t a,float x){mem_write32(a,f2u(x));}
void label(uint32_t node,uint32_t helper,float x,float top,float width,float height,float alignment=0){
    const auto text=helper+0x100;mem_write32(node+0xb8,helper);mem_write32(helper+0x48,text);mem_write32(text,0x8021cb58);
    mem_write32(helper+0x10,4);write_float(text+0x14,x);write_float(text+0x18,top+height);write_float(helper+0x14,width);write_float(helper+0x18,height);write_float(helper+0x20,alignment);
}
void reset_row(unsigned type){
    mem_write16(rowdef,uint16_t(type));mem_write16(rowdef+2,0x3f);mem_write32(rowdef+0xc,0);
    mem_write32(row+0x1c,rowdef);mem_write32(row+0x20,gui);mem_write32(row+0x24,root);mem_write32(row+8,0);mem_write32(row+0x34,0);
    mem_write32(gui+0x7c,row);mem_write32(root+0x34,row);
}
void event(PPCContext& c,port::NativeMenuPointerEvent::Kind kind,float x,float y,unsigned button=0,bool inside=true,uint64_t surface=0){
    auto snapshot=port::native_menu_mouse_snapshot();port::NativeMenuPointerEvent e;e.kind=kind;e.u=.5f+x/640.f;e.v=.5f+y/480.f;e.button=uint8_t(button);e.inside=inside;e.sequence=++sequence;e.surface_generation=surface?surface:snapshot.surface_generation;e.geometry_generation=snapshot.geometry_generation;
    port::queue_native_menu_pointer(e);port::consume_native_menu_pointer(c);
}
unsigned navigation_sounds(){return unsigned(std::count_if(calls.begin(),calls.end(),[](const auto& v){return std::get<0>(v)==0x800f9b74&&std::get<1>(v)==0x1c7;}));}
unsigned count(uint32_t fn,unsigned arg=~0u){return unsigned(std::count_if(calls.begin(),calls.end(),[&](const auto& v){return std::get<0>(v)==fn&&(arg==~0u||std::get<2>(v)==arg);}));}
}
const SSNEFuncEntry g_ssne_funcs[]={{0x800f9b74,stub,"original_gui_sound"},{0x800fac6c,stub,"eligible"},{0x800fa5bc,stub,"focus"},{0x800fb130,stub,"event"},{0x800f2440,stub,"set"},{0x800f6030,stub,"find"},{0x800f6e3c,stub,"changed"},{0x80100004,stub,"store"},{0x800f0504,stub,"numeric_event"},{0x800f2284,stub,"slider_event"},{0x800f6b6c,stub,"clear_focus"},{0x800f6a94,stub,"exact_focus"}};
const size_t g_ssne_func_count=std::size(g_ssne_funcs);const uint32_t g_ssne_entry=0;const bool g_ssne_has_game_code=true;
const SSNEFuncEntry* ssne_find_func(uint32_t fn){for(const auto& e:g_ssne_funcs)if(e.address==fn)return &e;return nullptr;}
PPCFunc* ssne_hook_slot(uint32_t){static PPCFunc slot=nullptr;return &slot;}
void ssne_install_hook(uint32_t fn,PPCFunc hook){*ssne_hook_slot(fn)=hook;}
void ssne_call(PPCContext& c,uint32_t fn){
    calls.emplace_back(fn,c.r[3],c.r[4],c.r[5]);
    if(fn==0x800fac6c){const auto node=c.r[3],definition=mem_read32(node+0x1c),flags=mem_read32(definition+0xc);bool bound=false;auto binding=mem_read32(mem_read32(node+0x20)+0x94);for(unsigned i=0;binding&&i<32;++i,binding=mem_read32(binding+8))if(mem_read16(binding+0x18)==mem_read16(definition+2)){bound=true;break;}c.r[3]=!(flags&0x20)&&(bound||!(flags&0x10));}
    else if(fn==0x800fa5bc&&!refuse_focus)mem_write32(mem_read32(c.r[3]+0x20)+0x7c,c.r[3]);
    else if(fn==0x800f6b6c)mem_write32(c.r[3]+0x7c,0);
    else if(fn==0x800f6a94)mem_write32(c.r[3]+0x7c,c.r[4]);
    else if(fn==0x800f2440){auto range=mem_read32(c.r[3]+0xb0);write_float(range,std::clamp(float(c.ps0[1]),u2f(mem_read32(range+4)),u2f(mem_read32(range+8))));}
    else if(fn==0x800f6030)c.r[3]=row;
    else if(fn==0x800fb130){const auto node=c.r[3],event=c.r[4];const auto type=mem_read16(mem_read32(node+0x1c));
        if(type==3&&(event==18||event==19||event==20||event==21))mem_write32(node+0xd0,uint32_t(std::clamp(int32_t(mem_read32(node+0xd0))+((event==19||event==21)?1:-1),0,int32_t(mem_read32(node+0xd4))-1)));
        if(type==5&&(event==20||event==21)){const auto range=mem_read32(node+0xb0);write_float(range,std::clamp(u2f(mem_read32(range))+(event==21?1.f:-1.f),u2f(mem_read32(range+4)),u2f(mem_read32(range+8))));}
    }
}
int main(){
    PPCContext c;c.r[1]=0x83ff0000;
    mem_write32(0x80000000,0x47334250);mem_write32(0x8028bfa4,gui);mem_write32(gui+0x6c,root);mem_write32(root+0x1c,rootdef);mem_write32(root+0x20,gui);mem_write16(rootdef+2,0x3d);
    reset_row(1);label(row,0x81200000,80,-35,120,24);port::install_native_menu_mouse();port::consume_native_menu_pointer(c);
    check(port::menu_mouse_wants_pointer(),"original selectable native text is pointer-owned");
    const auto first=port::native_menu_mouse_snapshot();
    event(c,port::NativeMenuPointerEvent::Kind::Move,100,-20);check(port::native_menu_mouse_diagnostics().find("\"id\":63")!=std::string::npos,"hover reports real native widget identity");
    event(c,port::NativeMenuPointerEvent::Kind::Down,100,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,100,-20,1);check(count(0x800fb130,22)==1,"click routes native activation exactly once");
    event(c,port::NativeMenuPointerEvent::Kind::Down,100,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,280,-20,1);check(count(0x800fb130,22)==1,"release outside actual measured text does not activate");
    write_float(0x81200014,240);port::consume_native_menu_pointer(c);check(port::native_menu_mouse_snapshot().geometry_generation==first.geometry_generation,"animated/localized width changes preserve an in-flight click generation");
    event(c,port::NativeMenuPointerEvent::Kind::Down,100,-20,1);port::NativeMenuPointerEvent cancel;cancel.kind=port::NativeMenuPointerEvent::Kind::Cancel;cancel.surface_generation=99999;port::queue_native_menu_pointer(cancel);port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Up,100,-20,1);check(count(0x800fb130,22)==1,"stale-generation Cancel still releases captured press");
    mem_write16(rootdef+2,0xa7);port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Down,100,-20,1,true,first.surface_generation);event(c,port::NativeMenuPointerEvent::Kind::Up,100,-20,1);check(count(0x800fb130,22)==1,"old-surface click cannot activate a replacement page");
    port::NativeMenuMouseServices policy;policy.allowed=[](auto,auto,auto,auto,unsigned){return false;};port::configure_native_menu_mouse(policy);port::consume_native_menu_pointer(c);check(!port::menu_mouse_wants_pointer(),"authority-denied controls do not acquire pointer");
    event(c,port::NativeMenuPointerEvent::Kind::Down,100,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,100,-20,1);check(count(0x800fb130,22)==1,"authority is enforced before original callback");port::configure_native_menu_mouse({});
    reset_row(5);mem_write32(rowdef+0xc,0);mem_write32(row+0x88,4);mem_write32(row+0x14,0x81201000);mem_write32(0x81201000,0x81201100);mem_write32(0x81201008,0x81201200);write_float(0x81201100,0);write_float(0x81201104,0);write_float(0x81201200,200);write_float(0x81201204,20);mem_write32(row+0xb0,0x81201300);write_float(0x81201300,0);write_float(0x81201304,0);write_float(0x81201308,100);mem_write16(row+0xac,0x3f);mem_write32(row+0x5c,0x81201400);mem_write32(0x8120143c,0x80100004);port::consume_native_menu_pointer(c);
    const auto slider_navigation=navigation_sounds();
    event(c,port::NativeMenuPointerEvent::Kind::Down,50,10,1);check(u2f(mem_read32(0x81201300))==25,"slider picks live track position");event(c,port::NativeMenuPointerEvent::Kind::Move,250,10,0,false);check(u2f(mem_read32(0x81201300))==100,"drag clamps outside displayed game image");event(c,port::NativeMenuPointerEvent::Kind::Up,250,10,1,false);check(count(0x80100004)>0&&count(0x800f6e3c,0x3f)>0,"slider commits original configuration binding and changed notification");check(navigation_sounds()==slider_navigation,"dragging already-focused slider does not spam selection audio");
    // The native ancestor scissors off the left half of the 200px track.
    // Its visible midpoint x150 is still 75%, not 50% of the full range.
    mem_write32(rootdef+0xc,0x1000);mem_write32(root+0x14,0x81204000);mem_write32(0x81204000,0x81204100);mem_write32(0x81204008,0x81204200);write_float(0x81204100,100);write_float(0x81204104,0);write_float(0x81204200,100);write_float(0x81204204,20);port::consume_native_menu_pointer(c);
    event(c,port::NativeMenuPointerEvent::Kind::Down,150,10,1);event(c,port::NativeMenuPointerEvent::Kind::Up,150,10,1);check(u2f(mem_read32(0x81201300))==75,"partial native scissor does not rescale the slider's full track");mem_write32(rootdef+0xc,0);
    reset_row(3);mem_write32(row+0xc0,0x81202000);mem_write32(0x81202008,0x81202040);mem_write32(0x81202014,0x81202100);mem_write32(0x81202054,0x81202300);label(0x81203000,0x81202100,0,50,100,24);label(0x81203100,0x81202300,0,90,100,24);mem_write32(row+0xd0,0);mem_write32(row+0xd4,2);port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Down,20,100,1);event(c,port::NativeMenuPointerEvent::Kind::Up,20,100,1);check(mem_read32(row+0xd0)==1,"clicked native selector option advances through its original handler");
    // Simulate focus-dependent native footer helpers and a fast Move+Down in
    // one guest frame; the next collect changes only the footer focus proxy.
    reset_row(1);label(row,0x81200000,80,-35,120,24);const uint32_t other=0x81000500,otherdef=0x81001200;
    mem_write32(row+8,other);mem_write32(other+0x1c,otherdef);mem_write32(other+0x20,gui);mem_write32(other+0x24,root);mem_write16(otherdef,1);mem_write16(otherdef+2,0x40);label(other,0x81205000,80,20,120,24);
    // The original controller GUI requests navigation cue 0x1C7 only for a
    // real selection change. Mouse motion follows the same sound boundary.
    port::consume_native_menu_pointer(c);const auto nav_before=navigation_sounds();
    event(c,port::NativeMenuPointerEvent::Kind::Move,100,30);
    check(mem_read32(gui+0x7c)==other&&navigation_sounds()==nav_before+1,"hover new row requests exactly one original GUI navigation cue in r3");
    for(unsigned i=0;i<8;++i)event(c,port::NativeMenuPointerEvent::Kind::Move,100.f+i,30);
    event(c,port::NativeMenuPointerEvent::Kind::Move,-300,200);
    event(c,port::NativeMenuPointerEvent::Kind::Move,100,30);
    port::NativeMenuPointerEvent nav_cancel;nav_cancel.kind=port::NativeMenuPointerEvent::Kind::Cancel;port::queue_native_menu_pointer(nav_cancel);port::consume_native_menu_pointer(c);
    event(c,port::NativeMenuPointerEvent::Kind::Move,100,30);
    event(c,port::NativeMenuPointerEvent::Kind::Down,100,30,1);event(c,port::NativeMenuPointerEvent::Kind::Up,100,30,1);
    check(navigation_sounds()==nav_before+1,"same-row motion, blank reentry, cancellation and same-row click do not repeat navigation audio");
    event(c,port::NativeMenuPointerEvent::Kind::Down,100,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,100,-20,1);
    check(mem_read32(gui+0x7c)==row&&navigation_sounds()==nav_before+2,"direct Down without prior hover requests one navigation cue, never another on Up");
    mem_write32(otherdef+0xc,0x10);port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Move,100,30);
    check(mem_read32(gui+0x7c)==row&&navigation_sounds()==nav_before+2,"disabled decorative row has no hover selection sound");
    mem_write32(otherdef+0xc,0);port::NativeMenuMouseServices sound_policy;sound_policy.allowed=[](auto,auto,auto,uint16_t id,unsigned){return id!=0x40;};port::configure_native_menu_mouse(sound_policy);port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Move,100,30);
    check(mem_read32(gui+0x7c)==row&&navigation_sounds()==nav_before+2,"authority-denied row has no hover selection sound");
    port::configure_native_menu_mouse({});port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Move,100,30,0,true,999999);
    check(mem_read32(gui+0x7c)==row&&navigation_sounds()==nav_before+2,"stale pointer motion cannot produce navigation audio");
    refuse_focus=true;event(c,port::NativeMenuPointerEvent::Kind::Move,100,30);refuse_focus=false;
    check(mem_read32(gui+0x7c)==row&&navigation_sounds()==nav_before+2,"refused original focus callback does not produce a success sound");
    event(c,port::NativeMenuPointerEvent::Kind::Move,100,30);
    check(mem_read32(gui+0x7c)==other&&navigation_sounds()==nav_before+3,"valid later hover remains audible after a rejected focus attempt");
    mem_write32(gui+0x7c,row);port::consume_native_menu_pointer(c);
    mem_write32(rowdef+0x10,1);mem_write32(otherdef+0x10,1);mem_write32(otherdef+0x14,1);mem_write32(gui+0xc4,0x81206000);mem_write32(0x81206000,0x81206100);mem_write32(0x81206004,0x81206300);mem_write32(0x81206318,0x81206400);write_float(0x81206400,1);label(0x81206500,0x81206100,32,-44,80,24);mem_write32(0x81206110,2);port::consume_native_menu_pointer(c);check(port::native_menu_mouse_diagnostics().find("0.908333")!=std::string::npos,"original footer bottom-left anchor maps to actual screen bottom instead of overlapping Main rows");
    const auto fast=port::native_menu_mouse_snapshot();const auto before_fast=count(0x800fb130,22);
    for(auto kind:{port::NativeMenuPointerEvent::Kind::Move,port::NativeMenuPointerEvent::Kind::Down}){port::NativeMenuPointerEvent e;e.kind=kind;e.u=.5f+100.f/640;e.v=.5f+30.f/480;e.inside=true;e.button=1;e.sequence=++sequence;e.surface_generation=fast.surface_generation;e.geometry_generation=fast.geometry_generation;port::queue_native_menu_pointer(e);}port::consume_native_menu_pointer(c);
    event(c,port::NativeMenuPointerEvent::Kind::Up,100,30,1);check(count(0x800fb130,22)==before_fast+1,"hover focus changes in native footer proxy do not cancel a fast row click");
    // Native footer prompts borrow the focused row. During their entrance
    // animation they can overlap that row, then move before physical release.
    // The visible row owns this click; the optional prompt is a fallback only.
    label(0x81206500,0x81206100,80,20,120,24);mem_write32(0x81206110,4);port::consume_native_menu_pointer(c);const auto before_overlap=count(0x800fb130,22);event(c,port::NativeMenuPointerEvent::Kind::Down,100,30,1);write_float(0x81206214,-200);port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Up,100,30,1);check(count(0x800fb130,22)==before_overlap+1,"real native row keeps click ownership when an overlapping footer prompt animates away before release");
    event(c,port::NativeMenuPointerEvent::Kind::Down,-180,30,1);event(c,port::NativeMenuPointerEvent::Kind::Up,-180,30,1);check(count(0x800fb130,22)==before_overlap+2,"native footer prompt remains clickable where no real control overlaps");write_float(0x81206400,0);port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Down,-180,30,1);event(c,port::NativeMenuPointerEvent::Kind::Up,-180,30,1);check(count(0x800fb130,22)==before_overlap+2,"fully faded native footer prompt has no pointer region");write_float(0x81206400,1);
    // Original prompt table80239054 orders A/B/X/Y/Z/Start, while native
    // input table80218408 maps Start to24 and X/Y/Z to25/26/27. Assert actual
    // native dispatch for each retained prompt, rather than mirrored indices.
    constexpr unsigned original_prompt_events[]={22,23,25,26,27,24};
    for(unsigned i=0;i<6;++i){const auto helper=0x81600000+i*0x1000,params=helper+0x200,alpha=helper+0x300;mem_write32(otherdef+0x10+4*i,1);mem_write32(0x81206000+8*i,helper);mem_write32(0x81206004+8*i,params);mem_write32(params+0x18,alpha);write_float(alpha,1);label(helper+0x600,helper,-260,-180+30.f*i,80,20);}
    port::consume_native_menu_pointer(c);
    for(unsigned i=0;i<6;++i){const auto expected=original_prompt_events[i],before_prompt=count(0x800fb130,expected);event(c,port::NativeMenuPointerEvent::Kind::Down,-220,-170+30.f*i,1);event(c,port::NativeMenuPointerEvent::Kind::Up,-220,-170+30.f*i,1);check(count(0x800fb130,expected)==before_prompt+1,"all six native footer prompts preserve original A/B/X/Y/Z/Start event semantics");}
    event(c,port::NativeMenuPointerEvent::Kind::Down,0,0,3);const auto before_back=count(0x800fb130,23);event(c,port::NativeMenuPointerEvent::Kind::Up,0,0,3);check(count(0x800fb130,23)==before_back+1,"right release on blank native menu area routes one focused Back");
    mem_write32(gui+0xc4,0);
    uint64_t logical_page=1;port::NativeMenuMouseServices reused;reused.surface_epoch=[&]{return logical_page;};port::configure_native_menu_mouse(reused);port::consume_native_menu_pointer(c);const auto before_page=count(0x800fb130,22);
    event(c,port::NativeMenuPointerEvent::Kind::Down,100,30,1);++logical_page;port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Up,100,30,1);check(count(0x800fb130,22)==before_page,"logical page epoch cancels old release even when native row address and ID are reused");port::configure_native_menu_mouse({});

    reset_row(1);label(row,0x81200000,80,-35,120,24,-60);port::set_native_menu_mouse_display(4.f/3.f);port::consume_native_menu_pointer(c);const auto wide=port::native_menu_mouse_diagnostics();check(wide.find("0.5234375")!=std::string::npos,"widescreen uses original centered canvas rather than stretching hit boxes");
    port::set_native_menu_mouse_display(1);const auto image_snapshot=port::native_menu_mouse_snapshot();const auto before_image=count(0x800fb130,22);
    for(auto kind:{port::NativeMenuPointerEvent::Kind::Down,port::NativeMenuPointerEvent::Kind::Up}){port::NativeMenuPointerEvent e;e.kind=kind;e.u=.5f+25.f/(640.f*(4.f/3.f));e.v=.5f-20.f/480;e.display_wide=4.f/3.f;e.inside=true;e.button=1;e.sequence=++sequence;e.surface_generation=image_snapshot.surface_generation;e.geometry_generation=image_snapshot.geometry_generation;port::queue_native_menu_pointer(e);port::consume_native_menu_pointer(c);}
    check(count(0x800fb130,22)==before_image+1,"event-retained displayed aspect wins over a newer current image aspect");
    // Native FAC6C allows a registered GUI handler before consulting flag10.
    // Original Main/Profile/Difficulty controls use that callback override;
    // flag10 alone marks decorative labels only when no native handler exists.
    mem_write32(rowdef+0xc,0x10);mem_write32(gui+0x94,0x81207000);mem_write16(0x81207018,0x3f);port::consume_native_menu_pointer(c);check(port::menu_mouse_wants_pointer(),"registered original handler overrides static decorative flag10");const auto before_bound=count(0x800fb130,22);
    event(c,port::NativeMenuPointerEvent::Kind::Down,25,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,25,-20,1);check(count(0x800fb130,22)==before_bound+1,"bound flag10 original control remains mouse-activatable");
    mem_write32(gui+0x94,0);port::consume_native_menu_pointer(c);check(!port::menu_mouse_wants_pointer(),"unbound flag10 static text remains decorative");
    reset_row(3);mem_write32(rowdef+0xc,0x2000);mem_write32(row+0xc0,0x81202000);mem_write32(0x81202008,0);mem_write32(0x81202014,0x81202100);label(0x81203000,0x81202100,20,-35,100,24);mem_write32(row+0xd0,2);mem_write32(row+0xd4,4);
    for(unsigned side=0;side<2;++side){const auto icon=0x81208000+side*0x100;mem_write32(0x8120214c+side*28,icon);mem_write32(icon+0x44,4);write_float(icon+0x14,side?130.f:-10.f);write_float(icon+0x18,-35);write_float(icon+0x1c,side?150.f:10.f);write_float(icon+0x20,-11);}
    port::consume_native_menu_pointer(c);event(c,port::NativeMenuPointerEvent::Kind::Down,0,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,0,-20,1);check(mem_read32(row+0xd0)==1,"native visible previous-arrow glyph changes selector through original event");event(c,port::NativeMenuPointerEvent::Kind::Down,140,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,140,-20,1);check(mem_read32(row+0xd0)==2,"native visible next-arrow glyph changes selector through original event");event(c,port::NativeMenuPointerEvent::Kind::Down,50,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,50,-20,1);check(mem_read32(row+0xd0)==3,"single native current-value helper cycles rather than resetting to C0 entry zero");
    auto wheel=[&](float x,float y,float notches){const auto state=port::native_menu_mouse_snapshot();port::NativeMenuPointerEvent e;e.kind=port::NativeMenuPointerEvent::Kind::Wheel;e.u=.5f+x/640;e.v=.5f+y/480;e.inside=true;e.wheel_y=notches;e.sequence=++sequence;e.surface_generation=state.surface_generation;e.geometry_generation=state.geometry_generation;port::queue_native_menu_pointer(e);port::consume_native_menu_pointer(c);};
    for(unsigned i=0;i<3;++i)wheel(50,-20,.25f);check(mem_read32(row+0xd0)==3,"precise fractional wheel does not round each small event into a full step");wheel(50,-20,.25f);check(mem_read32(row+0xd0)==2,"selector wheel up chooses previous after one accumulated notch");wheel(50,-20,.75f);port::NativeMenuPointerEvent wheel_cancel;wheel_cancel.kind=port::NativeMenuPointerEvent::Kind::Cancel;port::queue_native_menu_pointer(wheel_cancel);port::consume_native_menu_pointer(c);wheel(50,-20,.25f);check(mem_read32(row+0xd0)==2,"Cancel clears precise wheel remainder");
    reset_row(5);mem_write32(row+0x88,4);mem_write32(row+0x14,0x81201000);mem_write32(row+0xb0,0x81201300);write_float(0x81201300,10);port::consume_native_menu_pointer(c);wheel(50,10,1);check(u2f(mem_read32(0x81201300))==11,"numeric slider wheel up increases value");wheel(50,10,20);check(u2f(mem_read32(0x81201300))==15,"one large wheel event is bounded to four native adjustments");
    port::NativeMenuMouseServices denied_surface;denied_surface.allowed=[](auto,auto,auto,auto,unsigned){return false;};port::configure_native_menu_mouse(denied_surface);port::consume_native_menu_pointer(c);check(!port::native_menu_mouse_snapshot().active&&port::native_menu_mouse_snapshot().has_surface,"authority-denied GUI still owns a real surface and blocks presentation fallback");
    port::configure_native_menu_mouse({});mem_write32(rowdef+0x14,0);mem_write32(rootdef+0x14,0);port::consume_native_menu_pointer(c);const auto protected_back=count(0x800fb130,23);event(c,port::NativeMenuPointerEvent::Kind::Down,0,0,3);event(c,port::NativeMenuPointerEvent::Kind::Up,0,0,3);check(count(0x800fb130,23)==protected_back,"original pages with no native B prompt or Back handler ignore right click");
    reset_row(3);mem_write32(rowdef+0xc,0x2000);mem_write32(row+0xc0,0x81202000);mem_write32(row+0xd0,2);mem_write32(row+0xd4,4);mem_write32(gui+0x94,0x81207000);mem_write16(0x81207018,0x3f);mem_write32(0x81207014,22);port::consume_native_menu_pointer(c);const auto before_enum_confirm=count(0x800fb130,22);event(c,port::NativeMenuPointerEvent::Kind::Down,50,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,50,-20,1);check(count(0x800fb130,22)==before_enum_confirm+1&&mem_read32(row+0xd0)==2,"collapsed native selector with authored Confirm callback confirms selected profile instead of cycling it");
    mem_write32(gui+0x94,0);mem_write16(rowdef+8,1);mem_write32(rowdef+0x48,0x81207100);mem_write32(0x81207100,22);port::consume_native_menu_pointer(c);const auto before_enum_action=count(0x800fb130,22);event(c,port::NativeMenuPointerEvent::Kind::Down,50,-20,1);event(c,port::NativeMenuPointerEvent::Kind::Up,50,-20,1);check(count(0x800fb130,22)==before_enum_action+1&&mem_read32(row+0xd0)==2,"declarative native Confirm action also takes precedence over selector cycling");mem_write16(rowdef+8,0);
    reset_row(7);mem_write16(rowdef+2,0xa3);mem_write16(rootdef+2,0xa0);mem_write32(rowdef+0xc,0x10);mem_write32(row+0xbc,0x81302000);mem_write32(0x81302008,11);mem_write32(0x802abaa0,0x81300000);const auto preview=0x81300000+76*13;write_float(preview+0xe0,100);write_float(preview+0xe4,50);write_float(preview+0xe8,240);write_float(preview+0xec,186);mem_write32(preview+0xf0,4);mem_write32(preview+0xf4,4);unsigned model_intent=0;port::NativeMenuMouseServices model;model.model_region=[](auto,auto,auto,auto){return true;};model.model_owner=[](auto,auto,auto,auto){return 5;};model.intercept=[&](PPCContext&,auto,auto,auto,auto,unsigned intent){model_intent=intent;return true;};port::configure_native_menu_mouse(model);port::consume_native_menu_pointer(c);check(ssne::native_party_owner()==-1,"extra-slot model viewport lookup restores previous native owner scope");event(c,port::NativeMenuPointerEvent::Kind::Down,120,80,1);event(c,port::NativeMenuPointerEvent::Kind::Up,120,80,1);check(model_intent==18,"extra-slot model uses owner's real retained viewport and authoritative previous intent");
    port::configure_native_menu_mouse({});mem_write32(rootdef+0xc,0x82);port::consume_native_menu_pointer(c);check(!port::native_menu_mouse_snapshot().active&&port::native_menu_mouse_snapshot().has_surface,"linked closing native root retains visible surface ownership without active hits");const auto before_close=count(0x800fb130,22);event(c,port::NativeMenuPointerEvent::Kind::Down,120,80,1);event(c,port::NativeMenuPointerEvent::Kind::Up,120,80,1);check(count(0x800fb130,22)==before_close,"closing root does not accept stale menu hits");mem_write32(rootdef+0xc,0xa0);port::consume_native_menu_pointer(c);check(!port::native_menu_mouse_snapshot().has_surface,"hidden native root does not claim a visible presentation surface");
    // The original 2D renderer owns its own viewport and projection. Its
    // native Wide Screen mode changes half-width to 426.6667 before the PC
    // display factor; this is independent from the selected output aspect.
    auto projection=[&](float half_width){write_float(0x802dc690,half_width);write_float(0x802dc694,240);write_float(0x80292394,1/half_width);write_float(0x802923a8,-1.f/240);};
    auto group=[&](uint32_t at,uint32_t parent,float l,float t,float r,float b,unsigned tl,unsigned br,float scale){mem_write32(at,0x8021cab0);mem_write32(at+0x14,parent);mem_write8(at+0x1c,uint8_t(tl));mem_write8(at+0x1d,uint8_t(br));write_float(at+0x20,l);write_float(at+0x24,t);write_float(at+0x28,r);write_float(at+0x2c,b);write_float(at+0x30,scale);};
    auto projected_event=[&](port::NativeMenuPointerEvent::Kind kind,float x,float y,float wide,unsigned button){const auto state=port::native_menu_mouse_snapshot();port::NativeMenuPointerEvent e;e.kind=kind;e.u=.5f+x/(640*wide);e.v=.5f+y/480;e.display_wide=wide;e.inside=true;e.button=uint8_t(button);e.sequence=++sequence;e.surface_generation=state.surface_generation;e.geometry_generation=state.geometry_generation;port::queue_native_menu_pointer(e);port::consume_native_menu_pointer(c);};
    reset_row(1);mem_write32(rootdef+0xc,0);mem_write16(rootdef+2,0x3d);label(row,0x81200000,80,-35,120,24);group(0x81400000,0,0,0,0,0,0,3,1);group(0x81400100,0x81400000,-320,-240,320,240,4,4,1);mem_write32(0x81200110,0x81400100);projection(320);port::set_native_menu_mouse_display(4.f/3.f);port::consume_native_menu_pointer(c);const auto before_native_wide=count(0x800fb130,22);
    projection(1280.f/3.f);port::consume_native_menu_pointer(c);check(port::native_menu_mouse_diagnostics().find("native_ortho_matrix")!=std::string::npos,"pointer uses the actual original native orthographic matrix");projected_event(port::NativeMenuPointerEvent::Kind::Down,65,-20,4.f/3.f,1);projected_event(port::NativeMenuPointerEvent::Kind::Up,65,-20,4.f/3.f,1);check(count(0x800fb130,22)==before_native_wide+1,"native wide projection shrinks hit geometry by its actual M00 before display widescreen");
    projection(320);port::consume_native_menu_pointer(c);projected_event(port::NativeMenuPointerEvent::Kind::Down,100,-20,4.f/3.f,1);const auto before_matrix_change=count(0x800fb130,22);projection(1280.f/3.f);port::consume_native_menu_pointer(c);projected_event(port::NativeMenuPointerEvent::Kind::Up,100,-20,4.f/3.f,1);check(count(0x800fb130,22)==before_matrix_change,"original native projection change cancels an in-flight gesture");
    // A1 uses bottom-center coordinates inside a player-card renderer, rather
    // than the screen's bottom-center. Preserve both native parent anchors.
    projection(320);group(0x81400100,0x81400000,-300,-136,-160,0,4,4,1);label(row,0x81200000,0,-24,100,24,-50);mem_write32(0x81200010,8);mem_write32(0x81200110,0x81400100);port::consume_native_menu_pointer(c);const auto before_panel=count(0x800fb130,22);projected_event(port::NativeMenuPointerEvent::Kind::Down,-250,-12,4.f/3.f,1);projected_event(port::NativeMenuPointerEvent::Kind::Up,-250,-12,4.f/3.f,1);check(count(0x800fb130,22)==before_panel+1,"native bottom-center text follows the real child player-card viewport");
    group(0x81400100,0x81400000,-300,-136,-160,0,4,4,.5f);port::consume_native_menu_pointer(c);const auto before_panel_scale=count(0x800fb130,22);projected_event(port::NativeMenuPointerEvent::Kind::Down,-250,-6,4.f/3.f,1);projected_event(port::NativeMenuPointerEvent::Kind::Up,-250,-6,4.f/3.f,1);check(count(0x800fb130,22)==before_panel_scale+1,"native renderer local scale is applied after its viewport anchors");
    // Safe-frame anchors use their native ROM aspect, independent from the
    // projection's half-width. This differs from an ordinary 4:3 inset.
    write_float(0x8021ca54,20.f/47.f);group(0x81400100,0x81400000,-320,-240,320,240,4,4,1);label(row,0x81200000,0,0,100,24);mem_write32(0x81200010,9);mem_write32(0x81200110,0x81400100);port::consume_native_menu_pointer(c);const auto before_safe_frame=count(0x800fb130,22);projected_event(port::NativeMenuPointerEvent::Kind::Down,-300,-124,4.f/3.f,1);projected_event(port::NativeMenuPointerEvent::Kind::Up,-300,-124,4.f/3.f,1);check(count(0x800fb130,22)==before_safe_frame+1,"native safe-frame top anchor reads the renderer's actual ROM aspect");
    label(row,0x81200000,-100,0,100,24);mem_write32(0x81200010,10);mem_write32(0x81200110,0x81400100);port::consume_native_menu_pointer(c);projected_event(port::NativeMenuPointerEvent::Kind::Down,240,148,4.f/3.f,1);projected_event(port::NativeMenuPointerEvent::Kind::Up,240,148,4.f/3.f,1);check(count(0x800fb130,22)==before_safe_frame+2,"native safe-frame bottom anchor uses the opposite edge of the same viewport");
    // A wheel-only body region follows the retained native text viewport,
    // including its parent scale, native wide projection and partial scissor.
    projection(1280.f/3.f);group(0x81400100,0x81400000,-300,-136,-160,0,4,4,.5f);mem_write16(0x8140011e,2);mem_write16(rootdef+2,0xe3);mem_write32(rowdef+0xc,0x10);mem_write32(gui+0x7c,row);
    constexpr uint32_t viewport=0x81500000;mem_write32(viewport+0x1c,0x81500100);mem_write32(viewport+0x20,gui);mem_write32(viewport+0x2c,0x81400100);mem_write32(viewport+0x14,0x81500200);mem_write32(viewport+0x88,8);mem_write32(0x81500200,0x81500300);mem_write32(0x81500208,0x81500308);write_float(0x81500300,-200);write_float(0x81500304,-50);write_float(0x81500308,240);write_float(0x8150030c,40);
    unsigned body_events=0,body_intent=0;port::NativeMenuMouseServices body;body.regions=[](PPCContext&){port::NativeMenuMouseRegion r;r.gui=gui;r.node=root;r.root=r.id=0xe3;r.viewport_node=viewport;r.event=0;r.previous_event=28;r.next_event=29;r.control="info.body";return std::vector{r};};body.intercept=[&](PPCContext&,auto,auto,auto,auto,unsigned intent){++body_events;body_intent=intent;return true;};port::configure_native_menu_mouse(body);port::consume_native_menu_pointer(c);const auto before_body_focus=count(0x800fa5bc),before_body_sound=navigation_sounds();projected_event(port::NativeMenuPointerEvent::Kind::Move,-180,-15,4.f/3.f,0);projected_event(port::NativeMenuPointerEvent::Kind::Down,-180,-15,4.f/3.f,1);projected_event(port::NativeMenuPointerEvent::Kind::Up,-180,-15,4.f/3.f,1);check(!body_events&&count(0x800fa5bc)==before_body_focus&&mem_read32(gui+0x7c)==row,"wheel-only viewport has no hover focus or left-click side effect");check(navigation_sounds()==before_body_sound,"wheel-only viewport has no pointer selection sound");
    auto body_wheel=[&](float x){const auto state=port::native_menu_mouse_snapshot();port::NativeMenuPointerEvent e;e.kind=port::NativeMenuPointerEvent::Kind::Wheel;e.u=.5f+x/(640*(4.f/3.f));e.v=.5f-15.f/480;e.display_wide=4.f/3.f;e.inside=true;e.wheel_y=-1;e.sequence=++sequence;e.surface_generation=state.surface_generation;e.geometry_generation=state.geometry_generation;port::queue_native_menu_pointer(e);port::consume_native_menu_pointer(c);};body_wheel(-230);check(!body_events,"native parent scissor excludes the clipped part of the full text viewport");body_wheel(-180);check(body_events==1&&body_intent==29&&mem_read32(gui+0x7c)==root,"actual projected viewport receives its original scroll event and root focus");check(navigation_sounds()==before_body_sound,"wheel-only scroll retains root focus semantics without navigation audio");port::configure_native_menu_mouse({});projection(320);mem_write16(0x8140011e,0);
    // Hovering the original Continue footer must focus BC itself even when
    // BC remembers its new Connection child as preferred keyboard focus.
    mem_write16(rootdef+2,0xbc);mem_write32(rootdef+0xc,0x80);mem_write32(rootdef+0x10,1);mem_write32(root+0x30,row);mem_write16(rowdef+2,0x151);label(row,0x81200000,210,185,100,24);mem_write32(0x81200110,0);mem_write32(gui+0xc4,0x81206000);mem_write32(0x81206000,0x81206100);mem_write32(0x81206004,0x81206300);mem_write32(0x81206318,0x81206400);write_float(0x81206400,1);label(0x81206500,0x81206100,32,-44,80,24);mem_write32(0x81206110,2);mem_write32(gui+0x7c,row);port::consume_native_menu_pointer(c);const auto old_clear=count(0x800f6b6c),old_exact=count(0x800f6a94),old_recursive=count(0x800fa5bc),old_continue_sound=navigation_sounds();projected_event(port::NativeMenuPointerEvent::Kind::Move,-260,205,4.f/3.f,0);check(mem_read32(gui+0x7c)==root&&count(0x800f6b6c)==old_clear+1&&count(0x800f6a94)==old_exact+1&&count(0x800fa5bc)==old_recursive,"BC Continue hover restores exact native BC focus without following preferred Connection child");check(navigation_sounds()==old_continue_sound+1,"BC Continue exact focus path requests the original navigation sound once");projected_event(port::NativeMenuPointerEvent::Kind::Move,-260,205,4.f/3.f,0);check(navigation_sounds()==old_continue_sound+1,"BC Continue repeated hover does not repeat its navigation sound");
    std::cout<<"Native menu mouse retained-font bounds, locale width, widescreen, native click/selector/slider binding, cancellation, stale page, authority and original navigation-sound CPU cases passed (PPC handlers stubbed).\n";
}
