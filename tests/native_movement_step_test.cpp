#include "ssne/native_movement_step_detail.h"
#include "ssne/native_movement_input.h"
#include <bit>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
using namespace ssne;
using namespace ssne::movement_step_detail;
namespace {
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
constexpr uint32_t actor=0x81000000,controller=0x81001000,registry=0x802D9048,end=0x81800000,stepcfg=0x81002000;
// Independent addresses reproduce the live PAL regression: the USA address
// contains an unrelated293 while the actual PAL scene global contains4.
constexpr uint32_t pal_scene=0x802DB7E8u,unrelated_usa_scene_word=0x802DB0E0u;
using Memory=std::map<uint32_t,uint32_t>;
uint32_t bits(float v){return std::bit_cast<uint32_t>(v);}
Memory initial(){
    Memory m{{0x80000000,0x47334250},{unrelated_usa_scene_word,293},{pal_scene,4},{registry,0},{registry+4,actor},{registry+8,0},{registry+12,0},
        {actor+0x48,0x802121E8},{actor+0x17C,1},{actor+0x178,1},{actor+0x340,1},{actor+0x344,controller},
        {actor+0x328,0},{actor+0x370,0},{actor+0x6C4,0},{actor+0xCC,bits(100.f)},{controller,0x80210E80},
        {controller+4,0},{controller+0xC,0},{actor+0x6C8,1},{actor+0x6D0,1},{actor+0x6E4,0},{actor+0x23C,stepcfg},{stepcfg+0x24,bits(.01f)}};
    for(unsigned i=0;i<3;++i){m[actor+0x20+i*4]=bits(float(i));m[actor+0x298+i*4]=bits(.5f*float(i));}
    for(unsigned i=0;i<7;++i)m[controller+0x10+i*4]=i<5?bits(std::numeric_limits<float>::quiet_NaN()):bits(.05f*float(i-5));
    for(unsigned i=0;i<5;++i)m[actor+0x708+i*4]=bits(.03f*float(i));
    return m;
}
auto fixture_binding(const Memory& m,bool waiting=false){return binding(actor,registry,4,end,waiting,[&](uint32_t p){auto i=m.find(p);return i==m.end()?0u:i->second;});}
void binding_and_state(){
    const auto base=initial();const auto valid=fixture_binding(base);check(bool(valid) && valid->slot==1 && valid->controller==controller,"Real registered on-foot binding rejected");
    auto usa_changes=base;usa_changes[unrelated_usa_scene_word]=4;
    check(bool(fixture_binding(usa_changes)),"Unrelated USA scene word affected PAL binding");
    usa_changes[pal_scene]=3;
    check(!fixture_binding(usa_changes),"USA scene4 bypassed non-gameplay PAL scene");
    check(bool(binding(actor,registry,8,end,false,[&](uint32_t p){auto it=base.find(p);return it==base.end()?0u:it->second;})),"Real eight-slot native registry rejected");
    auto s=state(actor,*valid,[&](uint32_t p){return base.at(p);});check(bool(s),"Fresh constructor's unused jump words rejected");
    for(unsigned i=0;i<5;++i)check(s->curve[i]==0.f,"Uninitialized inactive jump data leaked");
    check(s->curve[5]==0.f && s->curve[6]==.05f,"Native head/fall state was not retained");
    for(unsigned i=0;i<5;++i)check(bits(s->ground[i])==base.at(actor+0x708+i*4),"Original pending step/cooldown changed");
    check(s->grounded && !s->sliding && s->pending_jump==0,"Original discrete walking state changed");
    check(s->contact_kind==1 && *s==*s,"Original contact kind or state equality changed");
    for(auto [address,value]:{std::pair{0x80000000u,0u},std::pair{pal_scene,3u},std::pair{actor+0x48,0x8020C6B8u},std::pair{registry+4,actor+4},std::pair{actor+0x17C,9u},std::pair{actor+0x178,9u},std::pair{actor+0x340,2u},std::pair{actor+0x328,1u},std::pair{actor+0x370,1u},std::pair{actor+0x6C4,0x81003000u},std::pair{actor+0x6FC,1u},std::pair{actor+0x330,0x81004000u},std::pair{actor+0x344,0x80000001u},std::pair{controller,0x80210FB0u},std::pair{controller+4,2u},std::pair{controller+4,4u},std::pair{controller+0xC,2u},std::pair{actor+0xCC,bits(0.f)},std::pair{actor+0xCC,bits(std::numeric_limits<float>::infinity())},std::pair{stepcfg+0x24,bits(.02f)}}){
        auto m=base;m[address]=value;check(!fixture_binding(m),"Unsupported actor/schema/time was accepted");
    }
    check(!fixture_binding(base,true),"Waiting spawn was accepted");
    check(!binding(actor+4,registry,4,end,false,[&](uint32_t p){auto i=base.find(p);return i==base.end()?0u:i->second;}),"Unregistered scratch actor was accepted");
    auto falling=base;falling[controller+4]=3;falling[controller+0xC]=1;falling[actor+0x6D0]=0;falling[actor+0x6E4]=1;
    auto fb=fixture_binding(falling);check(bool(fb),"Original fall phase rejected");auto fs=state(actor,*fb,[&](uint32_t p){return falling.at(p);});check(fs && fs->phase==3 && fs->pending_jump==1 && !fs->grounded && fs->sliding,"Original fall/jump flags not preserved");
    auto jumping=base;jumping[controller+4]=1;
    const std::array<float,7> curve{3.2f,.8f,.25f,-.25f,2.1f,.0625f,.01f};
    for(unsigned i=0;i<7;++i)jumping[controller+0x10+i*4]=bits(curve[i]);
    auto jb=fixture_binding(jumping);auto js=state(actor,*jb,[&](uint32_t p){return jumping.at(p);});check(js && js->curve==curve,"Active original jump phase was not captured exactly");
    for(auto [address,value]:{std::pair{actor+0x20,bits(std::numeric_limits<float>::quiet_NaN())},std::pair{actor+0x298,bits(160.f)},std::pair{controller+0x14,bits(0.f)},std::pair{controller+0x24,bits(.21f)},std::pair{controller+0x28,bits(std::numeric_limits<float>::infinity())},std::pair{actor+0x708,bits(std::numeric_limits<float>::quiet_NaN())}}){
        auto m=jumping;m[address]=value;check(!state(actor,*jb,[&](uint32_t p){return m.at(p);}),"Invalid native checkpoint value accepted");
    }
}
void scope_and_commands(){
    NativeMovementCommand command{41,-128,127,true,3.1415927f};check(command_valid(command),"Valid queued command rejected");
    command.sequence=0;check(!command_valid(command),"No-sequence execute command accepted");command.sequence=41;
    command.yaw=std::numeric_limits<float>::quiet_NaN();check(!command_valid(command),"NaN command yaw accepted");
    check(step_time(.01f) && !step_time(.02f) && !step_time(0.f) && !step_time(std::numeric_limits<float>::infinity()),"Unsupported substep time accepted");
    Memory m{{10,101},{20,202},{30,303},{40,404},{50,505},{60,606},{70,0xDEADBEEF}};const auto saved=m;
    auto read=[&](uint32_t p){return m.at(p);};auto write=[&](uint32_t p,uint32_t v){m.at(p)=v;};
    try{ScopedWords scope(read,write);for(unsigned i=1;i<=6;++i)scope.set(i*10,900+i);check(m.at(70)==0xDEADBEEF,"Movement scope changed a weapon action");throw std::runtime_error("native exception");}catch(const std::runtime_error&){}
    check(m==saved,"Exceptional native return left temporary inputs or yaw");
    {ScopedWords scope(read,write);scope.set(10,901);scope.set(60,906);m[60]=999;}
    check(m.at(10)==101 && m.at(60)==999,"Scope clobbered a legitimate native yaw/input change");
    for(unsigned x=0;x<256;++x)for(unsigned y=0;y<256;++y){
        const auto raw=native_movement_actions(uint8_t(x),uint8_t(y));
        const float forward=std::max(raw[1],0.f)-std::max(-raw[1],0.f);
        const float world_right=std::max(raw[0],0.f)-std::max(-raw[0],0.f);
        const auto once=normalize_native_movement_actions({world_right,forward});
        const auto expected=native_movement_axes(uint8_t(x),uint8_t(y));
        check(bits(once[0])==bits(expected[0]) && bits(once[1])==bits(expected[1]),"Logical action injection changed exact single-clamp native axes");
    }
}
}
int main(){try{binding_and_state();scope_and_commands();std::cout<<"PASS native movement runtime guard, exact state, scoped restore and 65536 action mappings: "<<checks<<" checks\n";}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}

