// Production backend with synthetic translators/compilers; never boots a game.
#include "../launcher/src/game_builder.cpp"
#include <iostream>

namespace fixture {
unsigned translations=0,compiles=0,links=0,kills=0;
bool fail_compile=false,fail_link=false,cancel_poll=false;
bool fail_content=false;
std::atomic<bool>* cancellation=nullptr;
const std::string body="\nvoid f_80000000() {\n    int synthetic_value = 0;\n}\n";
const std::string header="// Synthetic fixture header, no game code.\n";
const std::string prefix="#include \"ssne/native_party.h\"\n#include \"ssne/replica_simulation.h\"\n#include \"ssne/native_results.h\"\n#include <algorithm>\n";
void check(bool v,const std::string& why){if(!v)throw std::runtime_error(why);}
std::string get(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void put(const fs::path& p,const std::string& bytes){fs::create_directories(p.parent_path());std::ofstream out(p,std::ios::binary|std::ios::trunc);out<<bytes;out.close();check(bool(out),"fixture write "+S(p));}
std::string crlf(const std::string& s){std::string out;for(char c:s){if(c=='\n')out+='\r';out+=c;}return out;}
std::string hash(const std::string& s){return launcher::runtime_text_sha256(s);}
}
namespace platform {
struct Process {int code=0;};
Process* spawn(const std::vector<std::string>& args,const std::string&,const std::string& log,std::string& error){
    using namespace fixture;error.clear();put(P(log),"Synthetic process collaborator; no executable run.\n");
    if (P(args.front()).filename()=="python.exe") {
        check(args.size()==7 && args[1]=="-I" && args[2]=="-B" && args[3]=="-u" &&
              P(args[4]).filename()=="import_ps2.py" && args[5]=="--game",
              "Content import must use the bundled installer and only the local installation path");
        if (fail_content) return new Process{9};
        put(P(args[6])/"game_data/files/maps/fixture.ssw","map");
        return new Process;
    }
    auto output=std::find(args.begin(),args.end(),"-o");
    if(output==args.end()){
        ++translations;check(args.size()>=3,"translator argv");const auto dir=P(args[2]);fs::create_directories(dir);
        put(dir/"funcs_000.cpp",crlf("// Synthetic translation fixture.\n"+body));put(dir/"recomp_funcs.h",crlf(header));return new Process;
    }
    check(output+1!=args.end(),"compiler output argv");const auto target=P(*(output+1));
    auto source=std::find(args.begin(),args.end(),"-c");
    if(source!=args.end()){
        ++compiles;check(source+1!=args.end(),"compiler source argv");auto text=get(P(*(source+1)));
        check(text.find("synthetic_value = 1;")!=std::string::npos&&text.starts_with(crlf(prefix)),"actual native patch was not applied before compilation");
        put(target,"synthetic object "+hash(text));return new Process{fail_compile?7:0};
    }
    ++links;check(args.size()>=2&&args[1].starts_with("@"),"link must use response file");
    const auto rsp=get(P(args[1].substr(1)));check(rsp.find("native_menu_mouse.cpp.obj")!=std::string::npos&&rsp.find("-lbcrypt")!=std::string::npos&&rsp.find("-liphlpapi")!=std::string::npos,"native descriptor link was not retained");
    put(target,"MZ synthetic linked image "+hash(rsp));return new Process{fail_link?8:0};
}
bool poll(Process* p,int& code){if(fixture::cancel_poll&&fixture::cancellation){fixture::cancel_poll=false;*fixture::cancellation=true;}code=p->code;return true;}
void kill(Process*){++fixture::kills;}void release(Process* p){delete p;}
bool download(const std::string&,const std::string&,const std::function<bool(uint64_t,uint64_t)>&,std::string& error){error="unexpected fixture download";return false;}
std::string port_exe_name(){return "SSNE-Native-Game.exe";}
}
namespace gc::zip {
bool list(const std::string&,std::vector<Entry>&,std::string& error){error="unexpected fixture unzip";return false;}
bool extract(const std::string&,const std::string&,const std::function<std::string(const std::string&)>&,const std::function<bool(uint64_t,uint64_t)>&,std::string& error){error="unexpected fixture unzip";return false;}
}
namespace fixture {
using O=NativeJSON::Object;using A=NativeJSON::Array;
NativeJSON recipe(const std::string& descriptor,const std::string& symbols){
    auto changed=body;const auto pos=body.find("0;");changed.replace(pos,1,"1");const auto before="// Synthetic translation fixture.\n"+body,after=prefix+"// Synthetic translation fixture.\n"+changed;
    NativeJSON fn=O{{"address","80000000"},{"before",hash(body)},{"after",hash(changed)},{"edits",A{O{{"start",int64_t(pos)},{"length",1},{"text","1"}}}}};
    auto file=[&](const std::string& name,const std::string& old,const std::string& fresh,A functions){return NativeJSON(O{{"name",name},{"before_sha256",hash(old)},{"after_sha256",hash(fresh)},{"input_bytes_sha256",hash(crlf(old))},{"output_bytes_sha256",hash(crlf(fresh))},{"functions",std::move(functions)}});};
    return O{{"schema",1},{"kind","ssne-pal-translation-edits"},{"format","ascii-lf"},{"output_newline","crlf"},{"header_prefix",prefix},{"runtime_kit_sha256",descriptor},{"symbols_sha256",symbols},{"patcher_sha256",std::string(64,'2')},{"function_count",1},{"edit_count",1},{"changed_unit_count",1},{"files",A{file("funcs_000.cpp",before,after,A{fn}),file("recomp_funcs.h",header,header,{})}}};
}
struct Temp {
    fs::path root;
    ~Temp(){std::error_code ec;auto parent=fs::weakly_canonical(fs::temp_directory_path(),ec);if(!ec&&fs::weakly_canonical(root.parent_path())==parent&&root.filename().string().starts_with("ssne-native-backend-test-"))fs::remove_all(root,ec);}
};
std::vector<std::pair<std::string,std::string>> product(const fs::path& dir){std::vector<std::pair<std::string,std::string>> out;for(const auto& n:native_publish_names)out.emplace_back(n,get(native_target_path(dir,n)));return out;}
void unchanged(const fs::path& dir,const std::vector<std::pair<std::string,std::string>>& old){for(const auto& [name,bytes]:old)check(get(native_target_path(dir,name))==bytes,"failed operation changed product "+name);}
fs::path one_file(const fs::path& dir,const std::string& extension){for(const auto& e:fs::recursive_directory_iterator(dir))if(e.is_regular_file()&&e.path().extension()==extension)return e.path();throw std::runtime_error("fixture cache missing "+extension);}
}
int main(){using namespace fixture;try{
    Temp temp{fs::temp_directory_path()/P("ssne-native-backend-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+" space-ä")};fs::create_directory(temp.root);
    const auto project=temp.root,kitdir=project/"build-kit",native=kitdir/"multiplayer",data=project/"game_data",dir=project/"Multiplayer";fs::create_directories(native);fs::create_directories(dir);
    put(data/"sys/boot.bin","G3BP9G");put(data/"sys/main.dol","synthetic-owned-disc");put(kitdir/"SSNE-RECOOP-Native.exe","synthetic translator");put(kitdir/"toolchain/bin/clang++.exe","synthetic compiler");put(kitdir/"G3BP9G_r0.sym","synthetic symbols");put(kitdir/"built-with.txt","33\n");
    A files;const std::vector<std::string> first={"lib/CMakeFiles/ssne-port.dir/port/src/native_menu_carousel.cpp.obj","lib/CMakeFiles/ssne-port.dir/port/src/native_menu_mouse.cpp.obj","lib/CMakeFiles/ssne-port.dir/port/src/main.cpp.obj"};
    for(const auto& name:first){put(native/P(name),"synthetic prebuilt "+name);files.emplace_back(O{{"path",name},{"sha256",native_hash(native/P(name))}});}
    for(int i=0;i<55;++i){const auto name="include/fixture/header"+std::to_string(i)+".h";put(native/P(name),"// synthetic verified header"+std::to_string(i));files.emplace_back(O{{"path",name},{"sha256",native_hash(native/P(name))}});}
    A link{"-static"};for(const auto& path:first)link.emplace_back("file:"+path);for(const auto& arg:{"{game_objects}","-lws2_32","-lbcrypt","-liphlpapi"})link.emplace_back(arg);
    NativeJSON descriptor=O{{"build",202},{"protocol",63},{"dol_sha256",native_hash(data/"sys/main.dol")},{"compiler_sha256",native_hash(kitdir/"toolchain/bin/clang++.exe")},{"recompiler_sha256",native_hash(kitdir/"SSNE-RECOOP-Native.exe")},{"patcher_sha256",std::string(64,'2')},{"compile_flags",A{"-std=gnu++20","-O2","-DNDEBUG","-w"}},{"link_args",link},{"files",files}};
    put(native/"runtime-kit.json",launcher::write_json(descriptor));auto r=recipe(native_hash(native/"runtime-kit.json"),native_hash(kitdir/"G3BP9G_r0.sym"));put(native/"translation-recipe.json",launcher::write_json(r));
    NativeJSON side=O{{"schema",1},{"runtime_kit_sha256",native_hash(native/"runtime-kit.json")},{"patch_recipe","build-kit/multiplayer/translation-recipe.json"},{"patch_recipe_sha256",native_hash(native/"translation-recipe.json")},{"symbols","build-kit/G3BP9G_r0.sym"},{"symbols_sha256",native_hash(kitdir/"G3BP9G_r0.sym")}};put(native/"launcher-kit.json",launcher::write_json(side));
    BuildKit base;base.dir=S(kitdir);base.compiler=S(kitdir/"toolchain/bin/clang++.exe");base.toolchain_dir=S(kitdir/"toolchain");base.recomp=S(kitdir/"SSNE-RECOOP-Native.exe");base.version="33";base.link={"{objs}"};BuildKit selected;std::string error;
#ifdef _WIN32
    check(select_build_target(base,S(project),S(project/"not-extracted"),selected,error)&&selected.target==BuildTarget::Standard,"fresh pre-extract target must be Standard");
    fs::remove(data/"sys/boot.bin");check(!select_build_target(base,S(project),S(data),selected,error)&&!error.empty(),"installed DOL with missing boot silently selected Standard");put(data/"sys/boot.bin","G3");check(!select_build_target(base,S(project),S(data),selected,error),"truncated boot silently selected Standard");
    put(data/"sys/boot.bin","G3BE9G");check(select_build_target(base,S(project),S(data),selected,error)&&selected.target==BuildTarget::Standard,"non-PAL standard target regressed");put(data/"sys/boot.bin","G3BP9G");
    check(select_build_target(base,S(project),S(data),selected,error)&&selected.target==BuildTarget::NativeMultiplayer,error);
#else
    check(select_build_target(base,S(project),S(data),selected,error)&&selected.target==BuildTarget::Standard,"non-Windows native kit selection must remain Standard");
    selected=base;selected.target=BuildTarget::NativeMultiplayer;check(launcher::load_runtime_kit(S(project),S(data),selected.runtime,error),error);
#endif
    // New optional renderer modules extend the hash-bound manifest without
    // weakening validation of every listed payload or the native base kit.
    const auto descriptor_original=get(native/"runtime-kit.json");
    const auto recipe_original=get(native/"translation-recipe.json"),side_original=get(native/"launcher-kit.json");
    auto extended=descriptor;
    for(int n=0;n<3;++n) {
        const auto name="lib/optional-crosshair-"+std::to_string(n)+".obj";
        put(native/P(name),"synthetic optional renderer "+std::to_string(n));
        extended.object()->at("files").array()->push_back(O{{"path",name},{"sha256",native_hash(native/P(name))}});
    }
    auto bind_extended=[&](const NativeJSON& value) {
        put(native/"runtime-kit.json",launcher::write_json(value));auto bound_recipe=r;
        bound_recipe.object()->at("runtime_kit_sha256")=native_hash(native/"runtime-kit.json");
        put(native/"translation-recipe.json",launcher::write_json(bound_recipe));auto bound_side=side;
        bound_side.object()->at("runtime_kit_sha256")=native_hash(native/"runtime-kit.json");
        bound_side.object()->at("patch_recipe_sha256")=native_hash(native/"translation-recipe.json");
        put(native/"launcher-kit.json",launcher::write_json(bound_side));
    };
    auto chat_descriptor=descriptor;chat_descriptor.object()->at("build")=205;chat_descriptor.object()->at("protocol")=64;
    bind_extended(chat_descriptor);launcher::RuntimeKit chat_kit;
    check(launcher::load_runtime_kit(S(project),S(data),chat_kit,error) && chat_kit.protocol==64 && chat_kit.build==205,"local chat protocol64 kit rejected: "+error);
    auto performance_descriptor=chat_descriptor;performance_descriptor.object()->at("protocol")=65;
    bind_extended(performance_descriptor);launcher::RuntimeKit performance_kit;
    check(launcher::load_runtime_kit(S(project),S(data),performance_kit,error) && performance_kit.protocol==65 && performance_kit.build==205,"local performance protocol65 kit rejected: "+error);
    auto survival_descriptor=performance_descriptor;survival_descriptor.object()->at("build")=210;survival_descriptor.object()->at("protocol")=66;
    bind_extended(survival_descriptor);launcher::RuntimeKit survival_kit;
    check(launcher::load_runtime_kit(S(project),S(data),survival_kit,error) && survival_kit.protocol==66 && survival_kit.build==210,"Survival protocol66 kit rejected: "+error);
#ifdef _WIN32
    BuildKit survival_selected;
    check(select_build_target(base,S(project),S(data),survival_selected,error) && survival_selected.target==BuildTarget::NativeMultiplayer && survival_selected.runtime.protocol==66 && survival_selected.runtime.build==210,"normal launcher did not select native Survival210: "+error);
#endif
    const auto survival_output=dir/"SSNE-Native-Multiplayer-Game.exe";const std::string survival_image="MZ accepted synthetic210";
    put(survival_output,survival_image);put(dir/"SSNE-Native-Multiplayer.exe",survival_image);
    put(dir/"release-manifest.json",launcher::write_json(O{{"build",210},{"protocol",66},{"entrypoint","native"},{"kit_descriptor_sha256",survival_kit.descriptor_sha256},{"product_sha256",hash(survival_image)},{"entrypoint_sha256",hash(survival_image)}}));
    check(launcher::runtime_build_ready(survival_kit,S(survival_output),error,true),"verified Survival210 product not ready to Play: "+error);
#ifdef _WIN32
    check(game_build_ready(survival_selected,S(survival_output),error,true),"normal launcher cannot Play verified Survival210: "+error);
#endif
    auto qol_descriptor=survival_descriptor;qol_descriptor.object()->at("build")=215;qol_descriptor.object()->at("protocol")=67;
    bind_extended(qol_descriptor);launcher::RuntimeKit qol_kit;
    check(launcher::load_runtime_kit(S(project),S(data),qol_kit,error) && qol_kit.protocol==67 && qol_kit.build==215,"Multiplayer QoL protocol67 kit rejected: "+error);
#ifdef _WIN32
    BuildKit qol_selected;
    check(select_build_target(base,S(project),S(data),qol_selected,error) && qol_selected.runtime.protocol==67 && qol_selected.runtime.build==215,"normal launcher did not select native QoL215: "+error);
#endif
    const std::string qol_image="MZ accepted synthetic215";
    put(survival_output,qol_image);put(dir/"SSNE-Native-Multiplayer.exe",qol_image);
    put(dir/"release-manifest.json",launcher::write_json(O{{"build",215},{"protocol",67},{"entrypoint","native"},{"kit_descriptor_sha256",qol_kit.descriptor_sha256},{"product_sha256",hash(qol_image)},{"entrypoint_sha256",hash(qol_image)}}));
    check(launcher::runtime_build_ready(qol_kit,S(survival_output),error,true),"verified QoL215 product not ready to Play: "+error);
    auto custom_descriptor=qol_descriptor;custom_descriptor.object()->at("build")=222;custom_descriptor.object()->at("protocol")=68;
    bind_extended(custom_descriptor);launcher::RuntimeKit custom_kit;
    check(launcher::load_runtime_kit(S(project),S(data),custom_kit,error) && custom_kit.protocol==68 && custom_kit.build==222,"Custom Level protocol68 kit rejected: "+error);
    auto download_descriptor=custom_descriptor;download_descriptor.object()->at("build")=226;download_descriptor.object()->at("protocol")=69;
    bind_extended(download_descriptor);launcher::RuntimeKit download_kit;
    check(launcher::load_runtime_kit(S(project),S(data),download_kit,error) && download_kit.protocol==69 && download_kit.build==226,"Custom map download protocol69 kit rejected: "+error);
    auto editor_descriptor=download_descriptor;editor_descriptor.object()->at("build")=228;editor_descriptor.object()->at("protocol")=70;
    bind_extended(editor_descriptor);
    launcher::RuntimeKit editor_kit;
    check(launcher::load_runtime_kit(S(project),S(data),editor_kit,error) && editor_kit.protocol==70 && editor_kit.build==228,"Editor logic protocol70 kit rejected: "+error);
    auto vehicle_descriptor=editor_descriptor;vehicle_descriptor.object()->at("build")=246;vehicle_descriptor.object()->at("protocol")=71;
    bind_extended(vehicle_descriptor);launcher::RuntimeKit vehicle_kit;
    check(launcher::load_runtime_kit(S(project),S(data),vehicle_kit,error) && vehicle_kit.protocol==71 && vehicle_kit.build==246,"Campaign vehicle kit rejected: "+error);
    auto unknown_descriptor=qol_descriptor;unknown_descriptor.object()->at("protocol")=72;
    bind_extended(unknown_descriptor);
    check(!launcher::load_runtime_kit(S(project),S(data),survival_kit,error) && error.find("Unsupported native multiplayer runtime build/protocol")!=std::string::npos,"unknown future protocol not rejected by protocol validation");
    bind_extended(extended);launcher::RuntimeKit extended_kit;
    check(launcher::load_runtime_kit(S(project),S(data),extended_kit,error)&&extended_kit.files.size()==61,"extended renderer payload refused: "+error);
    put(native/"lib/optional-crosshair-2.obj","tampered optional renderer");
    check(!launcher::load_runtime_kit(S(project),S(data),extended_kit,error),"optional renderer payload was not hash-verified");
    auto incomplete=descriptor;incomplete.object()->at("files").array()->pop_back();bind_extended(incomplete);
    check(!launcher::load_runtime_kit(S(project),S(data),extended_kit,error),"incomplete native manifest accepted");
    auto excessive=extended;while(excessive.object()->at("files").array()->size()<=128)excessive.object()->at("files").array()->push_back(files.front());
    bind_extended(excessive);check(!launcher::load_runtime_kit(S(project),S(data),extended_kit,error),"excessive native manifest accepted");
    put(native/"runtime-kit.json",descriptor_original);put(native/"translation-recipe.json",recipe_original);put(native/"launcher-kit.json",side_original);
    const auto output=selected.output_exe(S(project));check(P(output)==fs::weakly_canonical(project)/launcher::native_project_exe&&P(selected.working_directory(S(project)))==fs::weakly_canonical(dir),"native output/CWD target mismatch: "+output+" for "+S(project));
    const auto old="MZ accepted synthetic197";put(dir/launcher::native_legacy_exe,old);put(dir/launcher::native_legacy_alias,old);
    put(P(output),"unrelated older standalone game");
    put(dir/"release-manifest.json",launcher::write_json(O{{"build",202},{"protocol",63},{"entrypoint","native"},{"kit_descriptor_sha256",selected.runtime.descriptor_sha256},{"product_sha256",hash(old)},{"entrypoint_sha256",hash(old)}}));
    check(P(selected.play_exe(S(project)))==fs::weakly_canonical(dir)/launcher::native_legacy_exe&&game_build_ready(selected,selected.play_exe(S(project)),error,true),"existing verified197 adoption failed: "+error);
    const auto compiler_path=kitdir/"toolchain/bin/clang++.exe";const auto compiler_bytes=get(compiler_path);
    fs::remove(compiler_path);
    check(game_build_ready(selected,selected.play_exe(S(project)),error,true),"accepted installed game must play without a compiler: "+error);
    put(compiler_path,compiler_bytes);
    const auto side_bytes=get(native/"launcher-kit.json"),recipe_bytes=get(native/"translation-recipe.json"),payload_bytes=get(native/P(first[0]));
    put(native/P(first[0]),"corrupt");launcher::RuntimeKit probe;check(!launcher::load_runtime_kit(S(project),S(data),probe,error),"tampered runtime payload accepted");put(native/P(first[0]),payload_bytes);
    auto wrong=r;wrong.object()->at("symbols_sha256")=std::string(64,'3');put(native/"translation-recipe.json",launcher::write_json(wrong));auto rebound=side;rebound.object()->at("patch_recipe_sha256")=native_hash(native/"translation-recipe.json");put(native/"launcher-kit.json",launcher::write_json(rebound));check(!launcher::load_runtime_kit(S(project),S(data),probe,error),"recipe bound to different symbols accepted");put(native/"translation-recipe.json",recipe_bytes);put(native/"launcher-kit.json",side_bytes);
    auto escape=side;escape.object()->at("patch_recipe")="../outside.json";put(native/"launcher-kit.json",launcher::write_json(escape));check(!launcher::load_runtime_kit(S(project),S(data),probe,error),"sidecar path traversal accepted");put(native/"launcher-kit.json",side_bytes);
    std::atomic<bool> cancel{false};cancellation=&cancel;GameBuilder builder;auto progress=[](const std::string&,double){};
    check(builder.build(selected,S(data),output,progress,cancel,error),error);check(translations==1&&compiles==1&&links==1,"cold pipeline did not translate/patch/compile/link exactly once");check(game_build_ready(selected,output,error,true),"published native receipt/aliases/manifest invalid: "+error);check(get(kitdir/"built-with.txt")=="33\n","native build overwrote standard kit33 stamp");
    check(selected.play_exe(S(project))==output&&get(P(output))==get(dir/launcher::native_legacy_exe)&&get(P(output))==get(dir/launcher::native_legacy_alias),"root game or compatibility aliases differ");
    const auto root_game=get(P(output));fs::remove(P(output));
    check(selected.play_exe(S(project))==output&&!game_build_ready(selected,output,error,true),"missing canonical root game was hidden by a compatibility alias");put(P(output),root_game);
    check(builder.build(selected,S(data),output,progress,cancel,error),error);check(translations==1&&compiles==1&&links==2,"warm cache retranslated/recompiled valid objects");
    const auto work=kitdir/"native-multiplayer-work",obj=one_file(work/"objects",".obj");put(obj,"corrupted cached object");check(builder.build(selected,S(data),output,progress,cancel,error),error);check(translations==1&&compiles==2,"tampered object was reused");
    auto generated=one_file(work/"generated",".cpp");put(generated,"corrupted generated source");check(builder.build(selected,S(data),output,progress,cancel,error),"corrupt cache failed source-only regeneration: "+error);check(translations==2&&compiles==2,"source cache repair did not reuse corrected verified object");
    auto stable=product(dir);fail_link=true;check(!builder.build(selected,S(data),output,progress,cancel,error)&&!error.empty(),"link failure accepted");fail_link=false;unchanged(dir,stable);
    put(obj,"force compiler failure");fail_compile=true;check(!builder.build(selected,S(data),output,progress,cancel,error),"compiler failure accepted");fail_compile=false;unchanged(dir,stable);
    cancel_poll=true;check(!builder.build(selected,S(data),output,progress,cancel,error)&&cancel,"mid-compiler cancellation accepted");cancel=false;unchanged(dir,stable);check(builder.build(selected,S(data),output,progress,cancel,error),error);
    stable=product(dir);const auto candidate=work/"publish-candidate.exe";put(candidate,"MZ replacement candidate");cancel=true;try{native_publish(selected.runtime,candidate,dir,cancel);check(false,"cancelled publication accepted");}catch(const std::exception&){}cancel=false;unchanged(dir,stable);
#ifdef _WIN32
    // An alias or the running root game can block publication after earlier
    // files have moved; each previous executable and receipt must be restored.
    for(const auto& path:{dir/launcher::native_legacy_alias,P(output)}){
        HANDLE held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(held!=INVALID_HANDLE_VALUE,"cannot hold synthetic game");bool rejected=false;
        try{native_publish(selected.runtime,candidate,dir,cancel);}catch(const std::exception&){rejected=true;}
        CloseHandle(held);check(rejected,"locked game publication succeeded");unchanged(dir,stable);
        check(!fs::exists(dir/".native-build-transaction"),"rollback journal left incomplete after locked game");
    }
#endif
    for(bool legacy_journal:{true,false}) {
        const auto stage=dir/".native-build-transaction";fs::create_directories(stage/"old");A entries;
        for(const auto& name:native_publish_names){
            if(legacy_journal&&name==launcher::native_project_exe)continue;
            const auto target=native_target_path(dir,name);const auto old_bytes=get(target),new_bytes="new synthetic "+name;
            entries.emplace_back(O{{"name",name},{"old_existed",true},{"old_sha256",hash(old_bytes)},{"new_sha256",hash(new_bytes)}});
            if(name==launcher::native_legacy_exe||name==launcher::native_project_exe){fs::rename(target,stage/"old"/P(name));put(target,new_bytes);}
        }
        put(stage/"journal.json",launcher::write_json(O{{"state","prepared"},{"items",entries}}));native_recover_publish(dir);unchanged(dir,stable);check(!fs::exists(stage),"interrupted publication was not recovered");
    }
    // Upgrading an already-built GC-only installation must enable the content
    // stage even when the runtime library and the executable did not change.
    const auto content_recipe=project/"Extras/PS2/checksums.json";
    put(content_recipe,launcher::write_json(O{{"rows",O{{"game_data/files/maps/fixture.ssw",O{{"bytes",3}}}}}}));
    auto with_content=side;
    (*with_content.object())["ps2_content_sha256"]=native_hash(content_recipe);
    put(native/"launcher-kit.json",launcher::write_json(with_content));
    check(launcher::load_runtime_kit(S(project),S(data),selected.runtime,error),error);
    check(!game_build_ready(selected,output,error,true),"old receipt accepted without required PS2 content");
    fail_content=true;
    check(!builder.build(selected,S(data),output,progress,cancel,error),"content installer error ignored");
    unchanged(dir,stable);
    check(!game_build_ready(selected,output,error,true),"failed content import marked ready");
    fail_content=false;
    check(builder.build(selected,S(data),output,progress,cancel,error),error);
    check(game_build_ready(selected,output,error,true),"content stage retry did not publish a valid receipt");
    put(data/"files/maps/fixture.ssw","truncated fixture");
    check(!game_build_ready(selected,output,error,true),"incomplete content accepted");
    std::cout<<"Native backend tests passed, including automatic content installation and recovery.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
