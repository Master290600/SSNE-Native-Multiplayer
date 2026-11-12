// Runs the actual updater guard/install and runtime-kit verification. Only the
// HTTPS transport and archive extraction are local fixture adapters; this test
// does not validate a real archive, execute a tool, or run a game.
#include "../launcher/src/updater.h"
#include "../launcher/src/launcher_json.h"
#include "../launcher/src/runtime_kit.h"
#include "../launcher/src/platform.h"
#include "gc/zip.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace fs=std::filesystem;
using J=launcher::JSONValue;using O=J::Object;using A=J::Array;
namespace {
#ifdef _WIN32
constexpr const char* launcher_name="SSNE-Native.exe";
#else
constexpr const char* launcher_name="SSNE-Native";
#endif
fs::path archive_fixture;unsigned downloads=0,extractions=0;
std::string feed_fixture;
void check(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
std::string S(const fs::path& p){auto s=p.u8string();return {s.begin(),s.end()};}
void put(const fs::path& p,const std::string& text){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f<<text;f.close();check(bool(f),"fixture write: "+S(p));}
std::string get(const fs::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"fixture read: "+S(p));return {std::istreambuf_iterator<char>(f),{}};}
std::string hash(const fs::path& p){std::string error;auto result=launcher::runtime_file_sha256(p,error);check(!result.empty(),error);return result;}
void json(const fs::path& p,const J& j){put(p,launcher::write_json(j,true));}
std::map<std::string,std::string> snapshot(const fs::path& root){
    std::map<std::string,std::string> files;
    for(const auto& e:fs::recursive_directory_iterator(root))if(e.is_regular_file()){
        const auto rel=e.path().lexically_relative(root);if(*rel.begin()!="update")files.emplace(S(rel),hash(e.path()));
    }return files;
}
void extract_fixture(const fs::path& root,const std::function<std::string(const std::string&)>& map){
    check(fs::is_directory(archive_fixture),"archive fixture is unset");++extractions;
    for(const auto& e:fs::recursive_directory_iterator(archive_fixture))if(e.is_regular_file()){
        const auto name=S(fs::path("Package")/e.path().lexically_relative(archive_fixture));const auto target=map(name);
        if(!target.empty()){const auto dest=root/fs::u8path(target);fs::create_directories(dest.parent_path());fs::copy_file(e.path(),dest);}
    }
}
struct Temp {
    fs::path path;
    Temp():path(fs::temp_directory_path()/("ssne-launcher-update-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))){check(fs::create_directory(path),"fixture directory");}
    ~Temp(){if(fs::weakly_canonical(path.parent_path())==fs::weakly_canonical(fs::temp_directory_path())&&path.filename().string().starts_with("ssne-launcher-update-test-")){std::error_code ec;fs::remove_all(path,ec);}}
};
void seed_base(const fs::path& base,bool integrated){
    put(base/launcher_name,"installed launcher");put(base/"launcher.ini","personal launcher config");put(base/"settings.ini","personal settings");put(base/"saves/card_a.raw","private card");put(base/"states/slot0.state","private state");
    put(base/"Multiplayer/settings.ini","personal native settings");put(base/"Multiplayer/settings-netplay.ini","personal network settings");put(base/"Multiplayer/saves/card_a.raw","private native card");put(base/"Multiplayer/SSNE-Native-Multiplayer-Game.exe","installed private generated game");
    put(base/"game_data/sys/boot.bin","G3BP9G");put(base/"game_data/sys/main.dol","synthetic disc DOL placeholder, never executed");
    if(integrated)put(base/"build-kit/multiplayer/launcher-kit.json","installed integration marker");
}
void marker(const fs::path& incoming,const std::string& exe_hash={},const std::string& kit_hash={}){
    json(incoming/"launcher-integration.json",O{{"schema",1},{"target","native-multiplayer"},{"launcher_sha256",exe_hash.empty()?hash(incoming/launcher_name):exe_hash},{"launcher_kit_sha256",kit_hash.empty()?hash(incoming/"build-kit/multiplayer/launcher-kit.json"):kit_hash}});
}
void native_package(const fs::path& incoming,const fs::path& base,int protocol=63){
    put(incoming/launcher_name,"synthetic incoming launcher");const auto kit=incoming/"build-kit/multiplayer";
    put(incoming/"build-kit/ssne-recomp.exe","synthetic pinned recompiler, never executed");put(incoming/"build-kit/G3BP9G_r0.sym","synthetic symbols");
    const std::vector<std::string> special={"lib/CMakeFiles/ssne-port.dir/port/src/native_menu_carousel.cpp.obj","lib/CMakeFiles/ssne-port.dir/port/src/native_menu_mouse.cpp.obj","lib/CMakeFiles/ssne-port.dir/port/src/main.cpp.obj"};
    A files,link;
    for(unsigned i=0;i<58;++i){const auto path=i<special.size()?special[i]:"include/synthetic/fixture_"+std::to_string(i)+".h";put(kit/path,"synthetic runtime payload "+std::to_string(i));files.emplace_back(O{{"path",path},{"sha256",hash(kit/path)}});if(i<special.size())link.emplace_back("file:"+path);}
    link.emplace_back("{game_objects}");link.emplace_back("-lbcrypt");link.emplace_back("-liphlpapi");
    const auto patcher=std::string(64,'e');
    json(kit/"runtime-kit.json",O{{"build",202},{"protocol",protocol},{"dol_sha256",hash(base/"game_data/sys/main.dol")},{"compiler_sha256",std::string(64,'c')},{"recompiler_sha256",hash(incoming/"build-kit/ssne-recomp.exe")},{"patcher_sha256",patcher},{"compile_flags",A{"-std=gnu++20","-O2"}},{"link_args",link},{"files",files}});
    const auto descriptor=hash(kit/"runtime-kit.json"),symbols=hash(incoming/"build-kit/G3BP9G_r0.sym");
    json(kit/"translation-recipe.json",O{{"schema",1},{"kind","ssne-pal-translation-edits"},{"format","ascii-lf"},{"output_newline","crlf"},{"runtime_kit_sha256",descriptor},{"symbols_sha256",symbols},{"patcher_sha256",patcher},{"function_count",0},{"edit_count",0},{"changed_unit_count",0},{"files",A{}}});
    json(kit/"launcher-kit.json",O{{"schema",1},{"runtime_kit_sha256",descriptor},{"patch_recipe","build-kit/multiplayer/translation-recipe.json"},{"patch_recipe_sha256",hash(kit/"translation-recipe.json")},{"symbols","build-kit/G3BP9G_r0.sym"},{"symbols_sha256",symbols}});marker(incoming);
}
void refusal_before_replacement(const fs::path& base,const fs::path& incoming,const std::string& expected){
    const auto before=snapshot(base);archive_fixture=incoming;const auto d=downloads,e=extractions;bool installing=false;std::string launched="untouched",error;
    updater::Release release{"99.0.0","fixture://package","fixture.zip"};
    const bool ok=updater::install(release,S(base),[&](const std::string& step,double){if(step=="Installing")installing=true;return true;},launched,error);
    check(!ok&&!error.empty()&&error.find(expected)!=std::string::npos,"update refused for wrong reason: "+error);
    check(!installing&&launched=="untouched"&&snapshot(base)==before,"invalid package changed installed files or reached replacement");
    check(downloads==d+1&&extractions==e+1,"install did not exercise local transport/extraction adapters");
}
}
namespace platform {
bool download(const std::string& url,const std::string& path,const std::function<bool(uint64_t,uint64_t)>& progress,std::string& error){
    ++downloads;if(!feed_fixture.empty() && url==updater::release_feed_url()){put(fs::u8path(path),feed_fixture);error.clear();return true;}if(url!="fixture://package"){error="network is disabled in CPU fixture";return false;}if(progress&&!progress(1,1)){error="cancelled";return false;}put(fs::u8path(path),"local archive adapter placeholder");error.clear();return true;
}
#ifndef _WIN32
struct Process {int code=0;};
Process* spawn(const std::vector<std::string>& args,const std::string&,const std::string&,std::string& error){try{check(args.size()>=5&&args[3]=="-C","unexpected tar adapter invocation");extract_fixture(fs::u8path(args[4]),[](const std::string& n){return n;});error.clear();return new Process{};}catch(const std::exception& e){error=e.what();return nullptr;}}
bool poll(Process* p,int& code){code=p->code;return true;}
void release(Process* p){delete p;}
#endif
}
namespace gc::zip {
bool extract(const std::string&,const std::string& destination,const std::function<std::string(const std::string&)>& map,const std::function<bool(uint64_t,uint64_t)>& progress,std::string& error){try{if(progress&&!progress(1,1)){error="cancelled";return false;}extract_fixture(fs::u8path(destination),map);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
}
int main(int argc,char** argv){try{
    if(argc==3){std::string error;const bool ok=updater::validate_package(argv[1],argv[2],error);if(!ok)std::cerr<<error<<'\n';return ok?0:2;}
    check(std::string(updater::release_feed_url())=="https://api.github.com/repos/" SSNE_UPDATE_REPOSITORY "/releases/latest","Launcher release feed differs from configured repository");
#ifdef _WIN32
    const std::string feed_asset="SSNE-Native-Multiplayer-1.2.7.202-Windows-x64.zip";
#else
    const std::string feed_asset="SSNE-Native-Multiplayer-1.2.7.202-Linux-x86_64.tar.gz";
#endif
    feed_fixture="{\"tag_name\":\"v1.2.7.202\",\"assets\":[{\"browser_download_url\":\"fixture://"+feed_asset+"\"}]}";
    updater::Release feed_release;std::string feed_error;
    check(updater::check(feed_release,feed_error) && feed_release.version=="1.2.7.202" && feed_release.asset_name==feed_asset,"Own-project feed did not resolve the expected version and OS asset: "+feed_error);
    feed_fixture.clear();
    Temp temp;std::string error;const auto base=temp.path/"standard",incoming=temp.path/"incoming";seed_base(base,false);fs::create_directory(incoming);
    check(!updater::validate_package(S(base),S(incoming),error)&&error.find("no SSNE-Native")!=std::string::npos,"missing launcher accepted");
    put(incoming/launcher_name,"new standard launcher");check(updater::validate_package(S(base),S(incoming),error),"ordinary standard package refused: "+error);
    const std::vector<std::string> forbidden={"launcher.ini","SETTINGS.INI","Game_Data/private.bin","saves/card_a.raw","states/slot0.state","Multiplayer/saves/card_a.raw","Multiplayer/states/slot0.state","Multiplayer/settings.ini","Multiplayer/settings-netplay.ini","Multiplayer/multiplayer-launcher.ini","build-kit/work/generated.cpp","build-kit/native-multiplayer-work/generated.cpp","SSNE-Native-Game.exe","Multiplayer/SSNE-Native-Multiplayer.exe","Multiplayer/SSNE-Native-Multiplayer-Game.exe","SSNE-Native-Game","Multiplayer/SSNE-Native-Multiplayer","Multiplayer/SSNE-Native-Multiplayer-Game","update-leftovers.txt","update/files/SSNE-Native.exe"};
    for(const auto& path:forbidden){const auto package=temp.path/("blocked-"+std::to_string(&path-forbidden.data()));put(package/launcher_name,"new standard launcher");put(package/fs::u8path(path),"forbidden package content");check(!updater::validate_package(S(base),S(package),error)&&error.find("local game data")!=std::string::npos,"personal/generated path accepted: "+path+"; "+error);}
    const auto before=get(base/"settings.ini");archive_fixture=incoming;std::string launched;
    check(updater::install({"99.0.0","fixture://package","standard.zip"},S(base),[](const std::string&,double){return true;},launched,error),"standard passthrough install failed: "+error);
    check(get(base/launcher_name)=="new standard launcher"&&get(base/"settings.ini")==before&&launched==S(base/launcher_name),"standard update changed personal data or did not replace launcher");
    const auto integrated=temp.path/"integrated";seed_base(integrated,true);
    refusal_before_replacement(integrated,incoming,"native multiplayer build support");
    const auto complete=temp.path/"complete";native_package(complete,integrated);
    check(updater::validate_package(S(integrated),S(complete),error),"complete synthetic native kit refused: "+error);
    // The same integrated launcher also handles fresh and non-PAL installs.
    // Update payload coherence must not depend on a PAL disc being installed.
    const auto no_data=temp.path/"integrated-no-data",usa=temp.path/"integrated-usa";
    put(no_data/launcher_name,"installed launcher");put(no_data/"build-kit/multiplayer/launcher-kit.json","installed marker");
    check(updater::validate_package(S(no_data),S(complete),error),"complete native update wrongly requires installed game_data: "+error);
    archive_fixture=complete;std::string fresh_launcher;
    check(updater::install({"99.0.0","fixture://package","fresh.zip"},S(no_data),[](const std::string&,double){return true;},fresh_launcher,error)&&!fs::exists(no_data/"game_data"),"fresh integrated update failed or fabricated game_data: "+error);
    seed_base(usa,true);put(usa/"game_data/sys/boot.bin","G3BE9G");put(usa/"game_data/sys/main.dol","synthetic USA disc placeholder");
    const auto usa_boot=hash(usa/"game_data/sys/boot.bin"),usa_dol=hash(usa/"game_data/sys/main.dol"),usa_settings=get(usa/"settings.ini");
    check(updater::validate_package(S(usa),S(complete),error),"complete native update wrongly rejects non-PAL installed data: "+error);
    archive_fixture=complete;check(updater::install({"99.0.0","fixture://package","usa.zip"},S(usa),[](const std::string&,double){return true;},fresh_launcher,error),"non-PAL integrated update failed: "+error);
    check(hash(usa/"game_data/sys/boot.bin")==usa_boot&&hash(usa/"game_data/sys/main.dol")==usa_dol&&get(usa/"settings.ini")==usa_settings,"native update altered non-PAL data/settings");

    marker(complete,std::string(64,'0'));refusal_before_replacement(integrated,complete,"launcher does not match");
    marker(complete,{},std::string(64,'0'));refusal_before_replacement(integrated,complete,"native launcher kit does not match");marker(complete);
    const auto payload=complete/"build-kit/multiplayer/include/synthetic/fixture_57.h";const auto payload_text=get(payload);put(payload,"tampered payload");refusal_before_replacement(integrated,complete,"incomplete native multiplayer update");put(payload,payload_text);
    const auto unsupported=temp.path/"unsupported";native_package(unsupported,integrated,999);refusal_before_replacement(integrated,unsupported,"Unsupported native multiplayer runtime build/protocol");
    const auto malformed=temp.path/"malformed";put(malformed/launcher_name,"incoming");put(malformed/"launcher-integration.json","{\"schema\":1,\"schema\":1}");refusal_before_replacement(integrated,malformed,"invalid launcher integration metadata");
    const auto saved_card=get(integrated/"Multiplayer/saves/card_a.raw"),saved_game=get(integrated/"Multiplayer/SSNE-Native-Multiplayer-Game.exe"),saved_settings=get(integrated/"Multiplayer/settings.ini");archive_fixture=complete;
    check(updater::install({"99.0.0","fixture://package","native.zip"},S(integrated),[](const std::string&,double){return true;},launched,error),"complete synthetic native package install refused: "+error);
    check(get(integrated/launcher_name)=="synthetic incoming launcher"&&get(integrated/"Multiplayer/saves/card_a.raw")==saved_card&&get(integrated/"Multiplayer/SSNE-Native-Multiplayer-Game.exe")==saved_game&&get(integrated/"Multiplayer/settings.ini")==saved_settings,"native update changed personal game data/output");

    // An incoming stale cleanup list must never reach the installed folder.
    const auto danger=temp.path/"danger",outside_card=temp.path/"outside-card.raw";put(outside_card,"outside private card");put(danger/launcher_name,"incoming");put(danger/"update-leftovers.txt",S(outside_card)+"\n");refusal_before_replacement(base,danger,"local game data");check(get(outside_card)=="outside private card","rejected update touched outside card");
    // Cleanup accepts only canonical in-base .old descendants. Neither a raw
    // personal card, an outside .old card, nor a path prefix lookalike qualifies.
    const auto cleanup_base=temp.path/"CleanupBase",outside_old=temp.path/"outside-card.raw.old",prefix_old=temp.path/"CleanupBase-sibling/card.old";
    const auto inside_card=cleanup_base/"saves/card_a.raw",inside_old=cleanup_base/"launcher.exe.old",nested_old=cleanup_base/"lib/runtime.a.old";
    put(outside_old,"outside old card");put(prefix_old,"outside prefix card");put(inside_card,"inside private card");put(inside_old,"retired launcher");put(nested_old,"retired library");put(cleanup_base/"update/staged.tmp","stale staging");
    std::string list=S(outside_card)+"\n"+S(outside_old)+"\n"+S(prefix_old)+"\n"+S(inside_card)+"\n"+S(inside_old)+"\n"+S(nested_old)+"\n"+S(cleanup_base)+"\n";
#ifndef _WIN32
    const auto case_sibling=temp.path/"cleanupbase/card.old";put(case_sibling,"outside case sibling");list+=S(case_sibling)+"\n";
#endif
    put(cleanup_base/"update-leftovers.txt",list);updater::cleanup(S(cleanup_base));
    check(get(outside_card)=="outside private card"&&get(outside_old)=="outside old card"&&get(prefix_old)=="outside prefix card"&&get(inside_card)=="inside private card","cleanup deleted private/outside file");
    check(!fs::exists(inside_old)&&!fs::exists(nested_old)&&!fs::exists(cleanup_base/"update-leftovers.txt")&&!fs::exists(cleanup_base/"update"),"cleanup did not remove valid retired files/staging");
#ifndef _WIN32
    check(get(case_sibling)=="outside case sibling","cleanup treated case-distinct Linux sibling as descendant");
#endif
    std::cout<<"Real updater guard/install CPU fixture passed: standard passthrough, missing launcher, "<<forbidden.size()<<" personal/generated paths, incompatible native updates before replacement, marker hashes, tampered payload, complete synthetic58 kit, fresh/non-PAL updates, personal data preservation and bounded cleanup. Transport/archive are stubbed; no game/tool executed.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
