#include "runtime_kit.h"
#include "launcher_json.h"
#include "gc/sha256.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>

namespace launcher {
namespace fs=std::filesystem;
namespace {
fs::path P(const std::string& s){return fs::u8path(s);}
std::string S(const fs::path& p){auto u=p.u8string();return {u.begin(),u.end()};}
void need(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
std::string lower(std::string s){for(char& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;}
bool digest(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isxdigit(c)!=0;});}
const JSONValue& field(const JSONValue& v,const char* key){auto p=v.find(key);need(p!=nullptr,std::string("Missing native kit field: ")+key);return *p;}
std::string str(const JSONValue& v,const char* key){auto p=field(v,key).string();need(p!=nullptr,std::string("Invalid native kit string: ")+key);return *p;}
int number(const JSONValue& v,const char* key){int64_t n=0;need(field(v,key).integer(n)&&n>=0&&n<=1000000,std::string("Invalid native kit integer: ")+key);return int(n);}
std::string hash_field(const JSONValue& v,const char* key){auto s=lower(str(v,key));need(digest(s),std::string("Invalid native kit SHA256: ")+key);return s;}
std::string bytes(const fs::path& p,size_t limit=4u*1024*1024){
    std::error_code ec;const auto n=fs::file_size(p,ec);need(!ec&&n<=limit,"Cannot read native kit metadata: "+S(p));
    std::ifstream f(p,std::ios::binary);need(bool(f),"Cannot open native kit metadata: "+S(p));
    std::string s(size_t(n),'\0');if(n)f.read(s.data(),std::streamsize(n));need(bool(f)||!n,"Cannot read complete native kit metadata: "+S(p));return s;
}
JSONValue json(const fs::path& p){auto s=bytes(p);if(s.starts_with("\xef\xbb\xbf"))s.erase(0,3);JSONValue v;std::string error;if(!parse_json(s,v,error))throw std::runtime_error("Invalid "+S(p)+": "+error);need(v.object()!=nullptr,"Native kit metadata must be an object: "+S(p));return v;}
fs::path relative(const std::string& s){
    need(!s.empty()&&s.find(':')==std::string::npos&&s.find('\0')==std::string::npos,"Unsafe native kit relative path");
    auto normalized=s;std::replace(normalized.begin(),normalized.end(),'\\','/');auto p=P(normalized);
    need(!p.is_absolute()&&!p.has_root_path(),"Absolute native kit path is forbidden: "+s);
    for(const auto& part:p)need(part!="."&&part!=".."&&!part.empty(),"Unsafe native kit path: "+s);
    return p;
}
fs::path contained(const fs::path& root,const fs::path& rel){
    std::error_code ec;const auto base=fs::weakly_canonical(root,ec);need(!ec,"Cannot resolve native kit root: "+S(root));
    const auto path=fs::weakly_canonical(base/rel,ec);need(!ec,"Cannot resolve native kit path: "+S(rel));
    auto a=base.begin(),b=path.begin();for(;a!=base.end()&&b!=path.end();++a,++b)need(lower(S(*a))==lower(S(*b)),"Native kit path escapes its root: "+S(rel));
    need(a==base.end()&&b!=path.end(),"Native kit path escapes its root: "+S(rel));return path;
}
void matches(const fs::path& path,const std::string& expected){std::string error;auto actual=runtime_file_sha256(path,error);need(!actual.empty()&&actual==expected,"Native build file differs or is missing: "+S(path)+(error.empty()?"":" ("+error+")"));}
std::vector<std::string> strings(const JSONValue& v,const char* key){
    auto a=field(v,key).array();need(a&&!a->empty()&&a->size()<=8192,std::string("Invalid native kit list: ")+key);std::vector<std::string> out;
    for(const auto& item:*a){auto p=item.string();need(p&&!p->empty()&&p->find_first_of("\r\n") == std::string::npos&&p->find('\0')==std::string::npos,"Invalid native build argument");out.push_back(*p);}return out;
}
void verify(const RuntimeKit& k,bool compiler,bool disc=true){
    const auto project=P(k.project_dir),kit=P(k.kit_dir);
    matches(kit/"launcher-kit.json",k.sidecar_sha256);matches(kit/"runtime-kit.json",k.descriptor_sha256);
    matches(P(k.recipe),k.recipe_sha256);matches(P(k.symbols),k.symbols_sha256);matches(P(k.recompiler),k.recompiler_sha256);
    if(compiler)matches(P(k.compiler),k.compiler_sha256);
    if(disc){const auto data=P(k.data_dir);std::ifstream boot(data/"sys/boot.bin",std::ios::binary);char id[6]{};need(bool(boot.read(id,6))&&std::string(id,6)=="G3BP9G","Native multiplayer requires the supported original European G3BP9G disc");
        matches(data/"sys/main.dol",k.dol_sha256);}

    for(const auto& f:k.files)matches(contained(kit,relative(f.path)),f.sha256);
}
}
std::string runtime_text_sha256(const std::string& text){gc::Sha256 h;h.update(text.data(),text.size());return gc::Sha256::hex(h.finish());}
std::string runtime_file_sha256(const fs::path& path,std::string& error){
    std::ifstream f(path,std::ios::binary);if(!f){error="cannot open "+S(path);return {};}
    gc::Sha256 h;std::vector<char> buffer(1u<<20);
    while(f){f.read(buffer.data(),std::streamsize(buffer.size()));const auto n=f.gcount();if(n>0)h.update(buffer.data(),size_t(n));}
    if(!f.eof()){error="cannot read complete file "+S(path);return {};}
    error.clear();return gc::Sha256::hex(h.finish());
}
bool load_runtime_kit(const std::string& project_dir,const std::string& data_dir,RuntimeKit& out,std::string& error,bool verify_disc){
    try{
        RuntimeKit k;k.project_dir=S(fs::weakly_canonical(P(project_dir)));k.kit_dir=S(P(k.project_dir)/"build-kit/multiplayer");k.data_dir=S(fs::weakly_canonical(P(data_dir)));
        const auto project=P(k.project_dir),kit=P(k.kit_dir);auto side=json(kit/"launcher-kit.json");need(number(side,"schema")==1,"Unsupported native launcher kit schema");
        k.sidecar_sha256=runtime_text_sha256(bytes(kit/"launcher-kit.json"));k.descriptor_sha256=hash_field(side,"runtime_kit_sha256");matches(kit/"runtime-kit.json",k.descriptor_sha256);
        auto d=json(kit/"runtime-kit.json");k.build=number(d,"build");k.protocol=number(d,"protocol");need(k.build>=202&&k.protocol==63,"Unsupported native multiplayer runtime build/protocol");
        k.recipe=S(contained(project,relative(str(side,"patch_recipe"))));k.recipe_sha256=hash_field(side,"patch_recipe_sha256");
        k.symbols=S(contained(project,relative(str(side,"symbols"))));k.symbols_sha256=hash_field(side,"symbols_sha256");
        k.compiler=S(project/"build-kit/toolchain/bin/clang++.exe");k.compiler_sha256=hash_field(d,"compiler_sha256");
        k.recompiler=S(project/"build-kit/ssne-recomp.exe");k.recompiler_sha256=hash_field(d,"recompiler_sha256");k.dol_sha256=hash_field(d,"dol_sha256");
        matches(P(k.recipe),k.recipe_sha256);auto recipe=json(P(k.recipe));
        need(number(recipe,"schema")==1&&str(recipe,"kind")=="ssne-pal-translation-edits"&&hash_field(recipe,"runtime_kit_sha256")==k.descriptor_sha256
             &&hash_field(recipe,"symbols_sha256")==k.symbols_sha256&&hash_field(recipe,"patcher_sha256")==hash_field(d,"patcher_sha256"),"Native translation recipe is bound to another runtime/symbols/patcher");
        k.compile_flags=strings(d,"compile_flags");k.link_args=strings(d,"link_args");
        auto files=field(d,"files").array();need(files&&files->size()>=58&&files->size()<=128,"Native runtime payload manifest is incomplete or exceeds the supported limit");std::set<std::string> seen;
        for(const auto& f:*files){auto path=str(f,"path");auto rel=relative(path);need(rel.begin()!=rel.end()&&(*rel.begin()=="include"||*rel.begin()=="lib"),"Unexpected native runtime payload root");
            auto normalized=S(rel);need(seen.insert(lower(normalized)).second,"Duplicate native runtime payload");k.files.push_back({normalized,hash_field(f,"sha256")});}
        int game_objects=0;for(const auto& arg:k.link_args){if(arg=="{game_objects}")++game_objects;else if(arg.starts_with("file:"))need(seen.contains(lower(S(relative(arg.substr(5))))),"Native linker file is absent from descriptor: "+arg);else need(arg.find('{')==std::string::npos,"Unknown native linker placeholder");}
        need(game_objects==1,"Expected exactly one native game-object link placeholder");
        for(const auto& lib:{"-lbcrypt","-liphlpapi"})need(std::count(k.link_args.begin(),k.link_args.end(),lib)==1,"Native LAN system library missing/duplicated");
        const std::vector<std::string> order={"file:lib/CMakeFiles/ssne-port.dir/port/src/native_menu_carousel.cpp.obj","file:lib/CMakeFiles/ssne-port.dir/port/src/native_menu_mouse.cpp.obj","file:lib/CMakeFiles/ssne-port.dir/port/src/main.cpp.obj"};
        auto first=std::find(k.link_args.begin(),k.link_args.end(),order[0]);need(first!=k.link_args.end()&&size_t(k.link_args.end()-first)>=order.size()&&std::equal(order.begin(),order.end(),first),"Native carousel/mouse/main link order differs");
        k.fingerprint=runtime_text_sha256("ssne-native-launcher-build-v1\n"+k.sidecar_sha256+"\n"+k.descriptor_sha256+"\n"+k.recipe_sha256+"\n"+k.symbols_sha256+"\n"+k.dol_sha256);
        verify(k,false,verify_disc);out=std::move(k);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool verify_runtime_kit(const RuntimeKit& k,bool compiler,std::string& error){try{verify(k,compiler);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool runtime_build_ready(const RuntimeKit& k,const std::string& output,std::string& error,bool force){
    try{
        // A verified existing game does not execute a compiler. Missing or
        // retired toolchains are repaired only when a new build is required.
        if(force)verify(k,false);
        const auto project=P(k.project_dir),exe=P(output),dir=exe.parent_path(),alias=dir/"SSNE-Native-Multiplayer.exe";
        need(fs::weakly_canonical(exe)==contained(project,relative("Multiplayer/SSNE-Native-Multiplayer-Game.exe")),"native game output path differs from selected project");
        contained(project,relative("Multiplayer/SSNE-Native-Multiplayer.exe"));
        need(!fs::exists(dir/".native-build-transaction"),"An interrupted native build needs recovery; choose Build Game");
        std::string failure;const auto hash=runtime_file_sha256(exe,failure);need(!hash.empty(),"Native multiplayer game has not been built");matches(alias,hash);
        auto release=json(dir/"release-manifest.json");need(number(release,"build")==k.build&&number(release,"protocol")==k.protocol&&str(release,"entrypoint")=="native"&&hash_field(release,"kit_descriptor_sha256")==k.descriptor_sha256&&hash_field(release,"product_sha256")==hash&&hash_field(release,"entrypoint_sha256")==hash,"Native release manifest does not match its verified runtime kit/executable");
        const auto receipt=dir/"native-build.json";
        if(fs::exists(receipt)){
            auto v=json(receipt);need(number(v,"schema")==1&&str(v,"fingerprint")==k.fingerprint&&hash_field(v,"exe_sha256")==hash&&hash_field(v,"descriptor_sha256")==k.descriptor_sha256,"Native game was built with another kit or changed; choose Build Game");
        }else{
            auto v=json(dir/"release-manifest.json");need(number(v,"build")==k.build&&number(v,"protocol")==k.protocol&&str(v,"entrypoint")=="native"&&hash_field(v,"kit_descriptor_sha256")==k.descriptor_sha256&&hash_field(v,"product_sha256")==hash&&hash_field(v,"entrypoint_sha256")==hash,"Existing native release does not match its verified runtime kit");
        }
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
