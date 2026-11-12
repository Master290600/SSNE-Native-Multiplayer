#define SDL_MAIN_HANDLED
#include "settings.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string utf8(const fs::path& p){const auto s=p.u8string();return {s.begin(),s.end()};}
struct Temp {
    fs::path root;
    ~Temp(){std::error_code ec;const auto target=fs::weakly_canonical(root,ec),parent=fs::weakly_canonical(fs::temp_directory_path(),ec);
        if(!ec&&target.parent_path()==parent&&target.filename().u8string().starts_with(u8"ssne-crosshair-settings-")&&!fs::is_symlink(fs::symlink_status(root,ec)))fs::remove_all(root,ec);}
};
int main()try{
    Temp temp{fs::temp_directory_path()/("ssne-crosshair-settings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};require(fs::create_directory(temp.root),"private fixture directory");const auto file=temp.root/"settings.ini";const auto path=utf8(file);
    port::Settings defaults;require(defaults.game_language==-1,"default language must keep the saved game choice");require(!defaults.crosshair_enabled&&defaults.crosshair_file.empty()&&defaults.crosshair_size==32&&defaults.crosshair_opacity==100&&defaults.crosshair_native_color,"original crosshair must remain the default");
    {std::ofstream f(file);f<<"settings_version=8\n[graphics]\naspect=3\n[general]\npause_unfocused=0\n";}
    require(port::load_settings(defaults,path),"legacy settings read");require(!defaults.crosshair_enabled&&defaults.crosshair_file.empty()&&defaults.aspect==3&&!defaults.pause_unfocused&&defaults.crosshair_native_color,"legacy settings unexpectedly enable a custom image or change unrelated fields");require(defaults.game_language==-1,"legacy settings must preserve automatic game language");
    port::Settings chosen;chosen.crosshair_enabled=true;chosen.crosshair_file="mein Fadenkreuz #=100% \xc3\x9c.png";chosen.crosshair_size=14;chosen.crosshair_opacity=67;chosen.crosshair_native_color=false;chosen.volume=79;chosen.kbm_player=3;chosen.cheat_invulnerable=false;
    require(port::save_settings(chosen,path),"custom settings write");port::Settings loaded;require(port::load_settings(loaded,path),"custom settings read");require(loaded.crosshair_enabled&&loaded.crosshair_file==chosen.crosshair_file&&loaded.crosshair_size==14&&loaded.crosshair_opacity==67&&!loaded.crosshair_native_color,"UTF-8/space/INI-reserved filename or custom values lost in roundtrip");require(loaded.volume==79&&loaded.kbm_player==3&&!loaded.cheat_invulnerable,"crosshair fields changed unrelated audio/player/cheat settings");
    loaded.crosshair_enabled=false;require(port::save_settings(loaded,path),"disabled write");port::Settings disabled;require(port::load_settings(disabled,path)&&!disabled.crosshair_enabled&&disabled.crosshair_file==chosen.crosshair_file&&!disabled.crosshair_native_color,"disabling custom crosshair loses selected file");
    loaded.crosshair_native_color=true;require(port::save_settings(loaded,path),"native color write");port::Settings tinted;tinted.crosshair_native_color=false;require(port::load_settings(tinted,path)&&tinted.crosshair_native_color,"enabled original color lost in roundtrip");
    // A restart-only language preference must roundtrip independently of local
    // image/audio/player settings, including the original automatic mode.
    for(int language:{-1,0,1,2}) {
        chosen.game_language=language;require(port::save_settings(chosen,path),"language write");
        port::Settings selected;selected.game_language=99;
        require(port::load_settings(selected,path)&&selected.game_language==language,"language selection lost in roundtrip");
        require(selected.crosshair_file==chosen.crosshair_file&&selected.crosshair_enabled&&selected.volume==79&&selected.kbm_player==3,"language changed unrelated image/audio/player settings");
    }
    for(const char* invalid:{"", "fr", "2junk", "-2", "3", "99", "2147483648", "-2147483649"}) {
        std::ofstream f(file);f<<"settings_version=8\n[general]\ngame_language="<<invalid<<"\n";f.close();
        port::Settings rejected;rejected.game_language=2;rejected.volume=79;rejected.kbm_player=3;
        require(port::load_settings(rejected,path)&&rejected.game_language==-1,"malformed or out-of-range language must use automatic mode");
        require(rejected.volume==79&&rejected.kbm_player==3,"invalid language changed unrelated settings");
    }
    for(int invalid:{-2,3,99}) {
        chosen.game_language=invalid;require(port::save_settings(chosen,path),"invalid direct language write");
        port::Settings normalized;require(port::load_settings(normalized,path)&&normalized.game_language==-1,"invalid direct language was not normalized when saved");
    }
    chosen.game_language=-1;
    for(const auto* bad:{"../outside.png","C:\\outside.png","folder/reticle.png","invalid.bmp","bad%2Fescape.png"}) {
        std::ofstream f(file);f<<"settings_version=8\n[crosshair]\ncrosshair_enabled=1\ncrosshair_file="<<bad<<"\n";f.close();port::Settings rejected;require(port::load_settings(rejected,path),"invalid leaf fixture read");require(!rejected.crosshair_enabled&&rejected.crosshair_file.empty(),"external/non-PNG/control filename was accepted");
    }
    // A direct Settings value is never allowed to inject another INI setting.
    chosen.crosshair_file="reticle.png\ncheat_invulnerable=1";require(port::save_settings(chosen,path),"invalid direct value write");port::Settings protected_fields;require(port::load_settings(protected_fields,path)&&!protected_fields.cheat_invulnerable&&!protected_fields.crosshair_enabled&&protected_fields.crosshair_file.empty(),"filename injection changed another setting");
    {std::ofstream f(file);f<<"settings_version=8\n[crosshair]\ncrosshair_size=-200\ncrosshair_opacity=300\n";}
    port::Settings bounded;require(port::load_settings(bounded,path)&&bounded.crosshair_size==8&&bounded.crosshair_opacity==100,"crosshair limits not enforced");
    {std::ofstream f(file);f<<"settings_version=8\n[crosshair]\ncrosshair_size=9999999999999999999\ncrosshair_opacity=12junk\n";}
    port::Settings malformed;require(port::load_settings(malformed,path)&&malformed.crosshair_size==32&&malformed.crosshair_opacity==100,"malformed/overflowing numeric values override valid defaults");
    chosen.crosshair_file="dot.png";chosen.crosshair_enabled=true;chosen.crosshair_size=99;chosen.crosshair_opacity=20;chosen.reset_graphics();require(!chosen.crosshair_enabled&&chosen.crosshair_file.empty()&&chosen.crosshair_size==32&&chosen.crosshair_opacity==100&&chosen.crosshair_native_color,"Reset Graphics does not restore original crosshair defaults");
    std::cout<<"Custom crosshair defaults/roundtrip, restart language defaults/all choices/invalid input, local enable/disable, original color, INI/path safety, bounds and reset passed.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
