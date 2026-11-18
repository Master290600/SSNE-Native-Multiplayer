#define SDL_MAIN_HANDLED
#include "settings.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main() try {
    static_assert(port::Settings::kKbSlots==3);
    static_assert(port::Settings::kMpSlots==2 && port::MP_COUNT==4);
    static_assert(port::ACT_WEAPON9-port::ACT_WEAPON1==8);
    const auto path=(std::filesystem::current_path()/"settings-roundtrip-private.ini").string();
    port::Settings original;
    using port::Bind;using port::MultiplayerBindConflict;
    require(!original.team_hud,"Team HUD must be opt-in");
    require(original.multiplayer[port::MP_CHAT][0]==Bind::key(SDL_SCANCODE_T),"Chat default changed");
    require(original.multiplayer[port::MP_PING][0]==Bind::key(SDL_SCANCODE_G) && original.multiplayer[port::MP_PING][1]==Bind::mouse(SDL_BUTTON_MIDDLE),"Ping defaults changed");
    require(original.multiplayer[port::MP_ROSTER][0]==Bind::key(SDL_SCANCODE_H),"Roster default changed");
    require(original.multiplayer[port::MP_AFK][0].type==Bind::NONE && original.multiplayer[port::MP_AFK][1].type==Bind::NONE,"AFK must remain unbound by default");
    original.team_hud=true;
    require(port::assign_multiplayer_bind(original,port::MP_CHAT,0,Bind::key(SDL_SCANCODE_Y)),"Cannot assign custom chat");
    require(port::assign_multiplayer_bind(original,port::MP_CHAT,1,Bind::mouse(SDL_BUTTON_X1)),"Cannot assign secondary mouse chat");
    require(port::assign_multiplayer_bind(original,port::MP_PING,0,Bind::key(SDL_SCANCODE_V)),"Cannot assign custom ping");
    require(port::assign_multiplayer_bind(original,port::MP_ROSTER,0,Bind::key(SDL_SCANCODE_B)),"Cannot assign custom roster");
    require(port::assign_multiplayer_bind(original,port::MP_ROSTER,1,Bind::mouse(SDL_BUTTON_X2)),"Cannot assign secondary mouse roster");
    require(port::assign_multiplayer_bind(original,port::MP_AFK,0,Bind::key(SDL_SCANCODE_P)),"Cannot assign custom AFK");
    const auto before_conflict=original;
    require(port::multiplayer_bind_conflict(original,port::MP_AFK,0,Bind::key(SDL_SCANCODE_W))==MultiplayerBindConflict::Gameplay,"Movement conflict not detected");
    require(port::multiplayer_bind_conflict(original,port::MP_AFK,0,Bind::key(SDL_SCANCODE_Y))==MultiplayerBindConflict::Multiplayer,"Other shortcut conflict not detected");
    require(port::multiplayer_bind_conflict(original,port::MP_AFK,0,Bind::key(SDL_SCANCODE_F12))==MultiplayerBindConflict::Reserved,"F12 conflict not detected");
    for(const auto binding:{Bind::key(SDL_SCANCODE_W),Bind::key(SDL_SCANCODE_Y),Bind::key(SDL_SCANCODE_F12),Bind::wheel(1),Bind::button(SDL_CONTROLLER_BUTTON_A),Bind::key(SDL_Scancode(SDL_NUM_SCANCODES)),Bind::mouse(6)})
        require(!port::assign_multiplayer_bind(original,port::MP_AFK,0,binding),"Conflicting or invalid input accepted");
    for(int a=0;a<port::MP_COUNT;++a)for(int k=0;k<port::Settings::kMpSlots;++k)require(original.multiplayer[a][k]==before_conflict.multiplayer[a][k],"Rejected assignment changed a shortcut");
    for(int a=0;a<port::ACT_COUNT;++a)for(int k=0;k<port::Settings::kKbSlots;++k)require(original.kb[a][k]==before_conflict.kb[a][k],"Rejected assignment changed gameplay controls");
    require(port::multiplayer_bind_in_use(original,Bind::mouse(SDL_BUTTON_X1)) && !port::multiplayer_bind_in_use(original,Bind{}),"Gameplay capture conflict lookup failed");
    require(port::assign_multiplayer_bind(original,port::MP_AFK,0,Bind::key(SDL_SCANCODE_P)),"Reassigning the same slot must be allowed");
    require(!port::assign_multiplayer_bind(original,-1,0,Bind{}) && !port::assign_multiplayer_bind(original,port::MP_CHAT,2,Bind{}),"Invalid shortcut index accepted");
    original.scale=-1;original.aspect=3;original.shader_notice=false;
    original.kb[port::ACT_A][2]=port::Bind::key(SDL_SCANCODE_F);
    original.kb[port::ACT_WEAPON9][2]=port::Bind::key(SDL_SCANCODE_KP_9);
    original.kbm_player=3;original.pad_player[0]=0;original.pad_player[2]=4;
    original.split_two=2;original.music_volume=37;original.sfx_volume=61;
    original.cheat_jump=275;original.cheat_damage=350;
    require(port::save_settings(original,path),"Cannot write private settings fixture");
    port::Settings loaded;
    require(port::load_settings(loaded,path),"Cannot read private settings fixture");
    require(loaded.scale==-1 && loaded.aspect==3 && !loaded.shader_notice,"Native resolution / automatic aspect lost");
    require(loaded.kb[port::ACT_A][2]==original.kb[port::ACT_A][2],"Third ordinary binding lost");
    require(loaded.kb[port::ACT_WEAPON9][2]==original.kb[port::ACT_WEAPON9][2],"Third weapon binding lost");
    require(loaded.kbm_player==3 && loaded.pad_player[0]==0 && loaded.pad_player[2]==4 && loaded.split_two==2,"Player assignments lost");
    require(loaded.music_volume==37 && loaded.sfx_volume==61,"Audio mix lost");
    require(loaded.cheat_jump==275 && loaded.cheat_damage==350,"Upstream settings fields lost");
    require(loaded.team_hud,"Team HUD choice lost");
    for(int a=0;a<port::MP_COUNT;++a)for(int k=0;k<port::Settings::kMpSlots;++k)require(loaded.multiplayer[a][k]==original.multiplayer[a][k],"Multiplayer keyboard/mouse slot lost");
    std::ofstream(path)<<"settings_version=5\n[graphics]\nrenderer=dx11\nscale=-1\naspect=3\nfps_limit=1\n";
    port::Settings legacy;require(port::load_settings(legacy,path),"Legacy settings rejected");
    require(legacy.renderer=="gpu" && legacy.scale==-1 && legacy.aspect==3 && legacy.fps_limit==port::kMinFpsLimit,"Legacy input revives unsupported renderer or invalid limits");
    require(!legacy.team_hud && legacy.multiplayer[port::MP_CHAT][0]==Bind::key(SDL_SCANCODE_T) && legacy.multiplayer[port::MP_PING][1]==Bind::mouse(SDL_BUTTON_MIDDLE),"Older settings must retain multiplayer defaults");
    std::ofstream(path)<<"settings_version=8\nteam_hud=1\nkb.move_up=key:T, key:Up, none\nmp.chat=key:T, mouse:6\nmp.ping=key:Y, mouse:3tail\nmp.roster=key:Y, key:F12\nmp.afk=scancode:-1, scancode:999999\n";
    port::Settings invalid;require(port::load_settings(invalid,path),"Invalid shortcut fixture rejected wholesale");
    require(invalid.team_hud && invalid.kb[port::ACT_MOVE_UP][0]==Bind::key(SDL_SCANCODE_T),"Loading shortcuts changed ordinary settings");
    require(invalid.multiplayer[port::MP_CHAT][0].type==Bind::NONE && invalid.multiplayer[port::MP_CHAT][1].type==Bind::NONE,"Gameplay-conflicting or invalid mouse shortcut survived loading");
    require(invalid.multiplayer[port::MP_PING][0]==Bind::key(SDL_SCANCODE_Y) && invalid.multiplayer[port::MP_PING][1].type==Bind::NONE,"Malformed numeric mouse binding accepted");
    require(invalid.multiplayer[port::MP_ROSTER][0].type==Bind::NONE && invalid.multiplayer[port::MP_ROSTER][1].type==Bind::NONE,"Duplicate or reserved loaded shortcut accepted");
    require(invalid.multiplayer[port::MP_AFK][0].type==Bind::NONE && invalid.multiplayer[port::MP_AFK][1].type==Bind::NONE,"Out-of-range scancode accepted");
    std::ofstream(path)<<"settings_version=8\nmp.chat=scancode:28tail, wheel:up\nmp.ping=button:a, axis:righttrigger\nmp.roster=key:Return, key:Left Ctrl\nmp.afk=none, none\n";
    port::Settings unsupported;require(port::load_settings(unsupported,path),"Unsupported shortcut fixture rejected wholesale");
    for(const auto& slots:unsupported.multiplayer)for(const auto& binding:slots)require(binding.type==Bind::NONE,"Unsupported or malformed shortcut survived loading");
    require(port::save_settings(unsupported,path),"Cannot save explicitly unbound shortcuts");
    port::Settings unbound;require(port::load_settings(unbound,path),"Cannot reload unbound shortcuts");
    for(const auto& slots:unbound.multiplayer)for(const auto& binding:slots)require(binding.type==Bind::NONE,"Explicitly unbound shortcut reverted to a default");
    port::Settings reset;reset.team_hud=true;reset.kb[port::ACT_MOVE_UP][0]={};
    require(port::assign_multiplayer_bind(reset,port::MP_AFK,0,Bind::key(SDL_SCANCODE_W)),"Freed gameplay key unavailable");
    reset.reset_keyboard();
    require(reset.kb[port::ACT_MOVE_UP][0]==Bind::key(SDL_SCANCODE_W) && reset.multiplayer[port::MP_AFK][0].type==Bind::NONE,"Keyboard reset created a multiplayer conflict");
    reset.kb[port::ACT_LOOK_UP][0]=Bind::key(SDL_SCANCODE_G);reset.reset_multiplayer();
    require(reset.kb[port::ACT_LOOK_UP][0]==Bind::key(SDL_SCANCODE_G) && reset.multiplayer[port::MP_PING][0].type==Bind::NONE && reset.multiplayer[port::MP_PING][1]==Bind::mouse(SDL_BUTTON_MIDDLE),"Multiplayer reset stole a gameplay key");
    require(reset.team_hud,"Resetting shortcuts changed the HUD preference");
    std::filesystem::remove(path);
    std::cout<<"1.2.7 settings: third slots, weapon actions, native resolution, automatic aspect, players, mix and legacy migration preserved.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
