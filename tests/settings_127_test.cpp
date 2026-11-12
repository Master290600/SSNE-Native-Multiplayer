#define SDL_MAIN_HANDLED
#include "settings.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main() try {
    static_assert(port::Settings::kKbSlots==3);
    static_assert(port::ACT_WEAPON9-port::ACT_WEAPON1==8);
    const auto path=(std::filesystem::current_path()/"settings-roundtrip-private.ini").string();
    port::Settings original;
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
    std::ofstream(path)<<"settings_version=5\n[graphics]\nrenderer=dx11\nscale=-1\naspect=3\nfps_limit=1\n";
    port::Settings legacy;require(port::load_settings(legacy,path),"Legacy settings rejected");
    require(legacy.renderer=="gpu" && legacy.scale==-1 && legacy.aspect==3 && legacy.fps_limit==port::kMinFpsLimit,"Legacy input revives unsupported renderer or invalid limits");
    std::filesystem::remove(path);
    std::cout<<"1.2.7 settings: third slots, weapon actions, native resolution, automatic aspect, players, mix and legacy migration preserved.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
