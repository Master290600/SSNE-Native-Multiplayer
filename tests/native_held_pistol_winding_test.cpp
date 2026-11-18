#include "../runtime/src/native_held_pistol_winding.h"
#include "ssne/recomp_hooks.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using ssne::detail::HeldPistolBinding;
using ssne::detail::HeldPistolWinding;
constexpr uint32_t material_address=0x8012680C,caller=0x8012F234,proxy_stack=0x817FF000;
constexpr HeldPistolBinding left{1,2,3,4,5,6},another_left{7,8,9,10,11,12};
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}

// Exercise the production lease and argument wrapper through the real
// SSNEOriginalCall bypass mechanism. The external driver's cull/cache contract
// is the verified original 80126ABC..80126B5C, including two-sided materials.
struct Fixture {
    HeldPistolWinding policy;
    PPCContext context;
    PPCFunc installed=nullptr,previous=nullptr;
    bool live=true,throw_driver=false;
    int cached=-1;
    unsigned hook_calls=0,driver_calls=0,chain_calls=0;
    std::vector<uint32_t> cull_calls,arguments;
    std::function<void()> inside_driver;
};
Fixture* fixture=nullptr;
void driver(PPCContext& c){
    auto& f=*fixture;
    if(!SSNEOriginalCall::consume(c,material_address) && f.installed){f.installed(c);return;}
    ++f.driver_calls;f.arguments.push_back(c.r[4]);
    const auto flags=c.r[3],flipped=c.r[4];
    auto inside=std::move(f.inside_driver);f.inside_driver={};if(inside)inside();
    if(f.throw_driver){c.r[4]=99;c.r[3]=0xBAD;throw std::runtime_error("original driver failure");}
    const int cull=flags&1?0:flags&8?(flipped?2:1):(flipped?1:2);
    if(f.cached!=cull){f.cached=cull;f.cull_calls.push_back(uint32_t(cull));}
    c.r[3]=100;c.r[4]=0x12345678;
}
void previous_hook(PPCContext& c){++fixture->chain_calls;ssne_call_original(c,material_address,driver);}
void hook(PPCContext& c){
    auto& f=*fixture;++f.hook_calls;
    f.policy.material(c,f.live,uint16_t(c.r[3]),[&](PPCContext& call){ssne_call_original(call,material_address,f.previous?f.previous:driver);});
}
struct Setup {
    Fixture f;
    Setup(){fixture=&f;f.installed=hook;f.context.r[1]=proxy_stack-0x198;f.context.lr=caller;}
    ~Setup(){fixture=nullptr;}
    void material(uint32_t flags=0,uint32_t flip=0){
        f.context.r[3]=flags;f.context.r[4]=flip;driver(f.context);
        require(f.context.r[3]==100 && f.context.r[4]==0x12345678,"Wrapper changed original return registers");
    }
};
void cached_hand_transitions(){
    Setup s;auto& f=s.f;
    s.material();
    {
        HeldPistolWinding::Scope draw(f.policy,&f.context,proxy_stack,left);
        s.material();s.material();
    }
    s.material();
    {
        HeldPistolWinding::Scope draw(f.policy,&f.context,proxy_stack,left);
        s.material(1);s.material(1);s.material(8);s.material();
    }
    s.material(8);s.material();
    require(f.cull_calls==std::vector<uint32_t>({2,1,2,0,1,2}),"Original cull cache did not follow left/right/two-sided transitions");
    require(f.arguments==std::vector<uint32_t>({0,1,1,0,0,0,0,1,0,0}),"Handedness argument escaped its draw lease");
    require(f.hook_calls==10 && f.driver_calls==10,"Stable hook bypass skipped or repeated an original call");
}
void nested_draws_and_interrupts(){
    Setup s;auto& f=s.f;
    HeldPistolWinding::Scope outer(f.policy,&f.context,proxy_stack,left);
    s.material();
    {
        HeldPistolWinding::Scope right(f.policy,&f.context,proxy_stack);
        s.material();
        {
            HeldPistolWinding::Scope nested(f.policy,&f.context,proxy_stack,another_left);
            require(f.policy.binding(&f.context,f.context.r[1],caller)==another_left,"Nested target was not bound");s.material();
        }
        s.material();
    }
    s.material();
    auto saved=f.context;
    f.context.r[1]-=0x100;s.material();f.context=saved;
    f.context.lr=0x8013D930;s.material();f.context=saved;
    PPCContext interrupt=f.context;interrupt.r[3]=0;interrupt.r[4]=0;driver(interrupt);
    s.material();
    require(f.arguments==std::vector<uint32_t>({1,0,1,0,1,0,0,0,1}),"Nested draw or interrupt inherited left winding");
    require(f.policy.binding(&f.context,f.context.r[1],caller)==left,"Nested draw lost its outer binding");
    // A late lifetime rejection must leave the original driver's argument alone.
    f.live=false;s.material();require(f.arguments.back()==0,"Removed/rebound resource retained the correction");
}
void exception_cleanup(){
    Setup s;auto& f=s.f;
    bool caught=false;
    try {
        HeldPistolWinding::Scope draw(f.policy,&f.context,proxy_stack,left);
        f.context.r[3]=0;f.context.r[4]=0;f.throw_driver=true;driver(f.context);
    }catch(const std::runtime_error&){caught=true;}
    require(caught && f.context.r[4]==0 && f.context.r[3]==0xBAD,"Original exception lost its incoming argument or failure state");
    require(!f.policy.binding(&f.context,f.context.r[1],caller),"Exception retained a draw scope");
    f.throw_driver=false;s.material();require(f.arguments.back()==0,"Failed draw changed the following normal draw");
    {
        HeldPistolWinding::Scope outer(f.policy,&f.context,proxy_stack,left);
        try {HeldPistolWinding::Scope nested(f.policy,&f.context,proxy_stack);throw std::runtime_error("nested draw");}catch(const std::runtime_error&){}
        s.material();require(f.arguments.back()==1,"Nested exception did not restore outer left scope");
    }
}
void chained_hooks_and_callers(){
    Setup s;auto& f=s.f;f.previous=previous_hook;
    HeldPistolWinding::Scope outer(f.policy,&f.context,proxy_stack,left);
    for(auto address:{0x8012F098u,0x8012F18Cu,0x8012F234u}){f.context.lr=address;s.material();}
    f.inside_driver=[&]{
        const auto saved=f.context;
        HeldPistolWinding::Scope other(f.policy,&f.context,proxy_stack);
        s.material();f.context=saved;
    };
    s.material();
    require(f.arguments==std::vector<uint32_t>({1,1,1,1,0}),"Original chained/nested material dispatch used the wrong scope");
    require(f.hook_calls==5 && f.chain_calls==5 && f.driver_calls==5,"Previous stable-slot hook was bypassed or recursively reentered");
    s.material(0,1);require(f.arguments.back()==1,"Already flipped original material was corrected twice");
}
void exact_asset_keys(){
    using ssne::detail::held_left_pistol_name;
    require(held_left_pistol_name("mp_left_deserthawk.ssm") && held_left_pistol_name("Models/items/mp_left_deserthawk.ssm"),"Original resource key was rejected");
    for(auto name:{"Models/items/mp_right_deserthawk.ssm","Models/items/left_deserthawk.ssm","Models/items/other_mp_left_deserthawk.ssm","mp_left_deserthawk.ssm.bak","mp_left_deserthawk"})
        require(!held_left_pistol_name(name),"Another weapon resource matched the original left MP asset");
}
}
int main(){try{cached_hand_transitions();nested_draws_and_interrupts();exception_cleanup();chained_hooks_and_callers();exact_asset_keys();std::cout<<"native held pistol winding tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
