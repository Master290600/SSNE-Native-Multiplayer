#include "local_native_weapon_presentation.h"
#include "ssne/ppc_context.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
bool sample_available=true;
void require(bool condition,const char* message){
    if(!condition)throw std::runtime_error(message);
}
}

namespace ssne {
std::optional<NativeLocalWeaponClip> native_local_weapon_clip(uint32_t actor,unsigned weapon,unsigned hand,unsigned logical){
    if(!actor || weapon!=1 || hand>1)return {};
    return NativeLocalWeaponClip{1.f,actor+hand,12,14,logical,logical,25};
}
bool sample_native_local_weapon_clip(PPCContext&,uint32_t,unsigned,unsigned,const NativeLocalWeaponClip& clip,
                                     float age,std::vector<float>& pose,uint32_t* frame){
    if(!sample_available)return false;
    pose={float(clip.logical),age};
    if(frame)*frame=unsigned(age*24);
    return true;
}
}

using namespace port::coop;

void check_reload(bool automatic,unsigned hands){
    EntityState own;own.id=11;
    own.properties["weapon_model_slot"]=int64_t{1};
    own.properties["weapon_modes"]=std::vector<int64_t>{0,0};
    for(unsigned hand=0;hand<2;++hand)
        own.properties["weapon_model"+std::to_string(hand)+"_bone_matrices"]=std::vector<float>{-1.f,-1.f};
    auto visible=own;
    LocalNativeWeaponPresentation motion;
    motion.bind(1,{1,1},own.id,1,0.);
    const LocalWeaponStatus status{1,hands,0,0};
    LocalPistolState state{hands};
    const unsigned selected=hands==3?1:0;
    const auto event=hands==1?NativeLocalWeaponClipEvent::reload_single_pistol:NativeLocalWeaponClipEvent::reload_dual_pistol;
    const auto key="weapon_model"+std::to_string(selected)+"_bone_matrices";
    const auto pose=[&]()->const auto&{return std::get<std::vector<float>>(visible.properties.at(key));};
    constexpr uint64_t input=12;
    const uint64_t fire=automatic?input:9;
    constexpr double start=1.;
    const double began=start+(automatic?.01:0.);
    if(automatic){
        require(motion.start(11,own,1,selected,NativeLocalWeaponClipEvent::last_round,fire,start,true),"missing final round");
        require(motion.predict_pistol_reload(11,own,hands,selected,input,fire,50,start,true),"queue final round reload");
    }else{
        require(motion.manual_pistol_reload(11,own,status,state,input,fire,10,50,start)==hands,"local R edge for each hand");
        require(!motion.manual_pistol_reload(11,own,status,state,input,fire,10,50,start+.02),"held R restarted");
    }
    PPCContext context{};
    for(int i=0;i<=120;++i){
        const double t=began+i/120.;
        if(i==36){
            require(!motion.confirm_pistol_reload(selected,event,fire,50,input),"accepted prior event");
            require(!motion.confirm_pistol_reload(selected,event,fire+1,51,input),"accepted wrong trigger");
            require(motion.confirm_pistol_reload(selected,event,fire,51,input),"rejected matching cue");
            require(!motion.confirm_pistol_reload(selected,event,fire,51,input),"reused matching cue");
        }
        sample_available=i!=60;
        state.reloading=i>36?hands:0;
        visible=own;
        motion.draw(context,11,own,visible,status,t,{},state,i>36?input:input-1);
        require(pose()[0]==(hands==1?3.f:5.f),"wrong native reload clip");
        require(std::abs(pose()[1]-float(i==60?59:i)/120.f)<.0001f,"time discontinuity or stalled reload");
    }
    visible=own;
    motion.draw(context,11,own,visible,status,began+1.15,{},state,input);
    require(pose()[1]==1.f,"replayed the lagging host pose");
    state.reloading=0;
    visible=own;
    motion.draw(context,11,own,visible,status,began+1.3,{},state,input);
    require(pose()[1]==-1.f,"blocked native idle after host completion");
    motion.bind(1,{1,1},own.id,3,began+1.31);
    require(!motion.manual_pistol_reload(11,own,{3,1,1},state,input+1,fire,input,52,began+1.32),"reload on unequipped weapon");
    motion.bind(1,{1,1},own.id,1,began+1.33);
    require(!motion.manual_pistol_reload(11,own,status,state,input+1,fire,input,52,began+1.33),"previous weapon's button edge survived equip");
}

int main()try{
    for(auto hands:{1u,3u})for(bool automatic:{false,true})check_reload(automatic,hands);
    return 0;
}catch(const std::exception& e){
    std::cerr<<e.what()<<'\n';
    return 1;
}
