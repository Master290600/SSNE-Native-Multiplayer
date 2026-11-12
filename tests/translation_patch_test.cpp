#include "../launcher/src/translation_patch.h"
#include "../launcher/src/launcher_json.h"
#include "gc/sha256.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
using namespace launcher;
using O=JSONValue::Object;using A=JSONValue::Array;
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
std::string hash(const std::string& s){gc::Sha256 h;h.update(s.data(),s.size());return gc::Sha256::hex(h.finish());}
void put(const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f<<s;f.close();check(bool(f),"fixture write");}
std::string get(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
std::string crlf(const std::string& s){std::string o;for(char c:s){if(c=='\n')o+='\r';o+=c;}return o;}
const std::string prefix="#include \"ssne/native_party.h\"\n#include \"ssne/replica_simulation.h\"\n#include \"ssne/native_results.h\"\n#include <algorithm>\n";
const std::string body="\nvoid f_80000000() {\n    int synthetic_value = 0;\n}\n",header="// Synthetic fixture header, not translated game code.\n";
JSONValue recipe(){
    auto changed=body;const auto pos=body.find("0;");changed.replace(pos,1,"1");const auto old="// Synthetic translation fixture.\n"+body,new_text=prefix+"// Synthetic translation fixture.\n"+changed;
    JSONValue fn=O{{"address","80000000"},{"before",hash(body)},{"after",hash(changed)},{"edits",A{O{{"start",int64_t(pos)},{"length",1},{"text","1"}}}}};
    auto file=[&](const std::string& name,const std::string& before,const std::string& after,A functions){return JSONValue(O{{"name",name},{"before_sha256",hash(before)},{"after_sha256",hash(after)},{"input_bytes_sha256",hash(crlf(before))},{"output_bytes_sha256",hash(crlf(after))},{"functions",std::move(functions)}});};
    return O{{"schema",1},{"kind","ssne-pal-translation-edits"},{"format","ascii-lf"},{"output_newline","crlf"},{"header_prefix",prefix},{"runtime_kit_sha256",std::string(64,'0')},{"symbols_sha256",std::string(64,'1')},{"patcher_sha256",std::string(64,'2')},{"function_count",1},{"edit_count",1},{"changed_unit_count",1},{"files",A{file("funcs_000.cpp",old,new_text,A{fn}),file("recomp_funcs.h",header,header,{})}}};
}
JSONValue& file(JSONValue& r){return r.object()->at("files").array()->at(0);}
JSONValue& fn(JSONValue& r){return file(r).object()->at("functions").array()->at(0);}
struct Temp {fs::path p;~Temp(){const auto temp=fs::weakly_canonical(fs::temp_directory_path());if(fs::weakly_canonical(p.parent_path())==temp&&p.filename().u8string().starts_with(u8"ssne-launcher-patch-test-")){std::error_code ec;fs::remove_all(p,ec);}}};
int main(int argc,char** argv){try{
    std::atomic<bool> cancel{false};std::string error;
    if(argc==4){const bool ok=apply_translation_recipe(fs::u8path(argv[1]),fs::u8path(argv[2]),fs::u8path(argv[3]),cancel,error);if(!ok)std::cerr<<error<<'\n';return ok?0:2;}
    Temp temp{fs::temp_directory_path()/fs::u8path("ssne-launcher-patch-test-\xc3\x9c spaces-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};check(fs::create_directory(temp.p),"fixture directory");const auto fixture=temp.p/std::string(40,'x');fs::create_directory(fixture);const auto in=fixture/"base",out=fixture/std::string(64,'a'),rp=fixture/"recipe.json";fs::create_directory(in);put(in/"funcs_000.cpp",crlf("// Synthetic translation fixture.\n"+body));put(in/"recomp_funcs.h",crlf(header));auto r=recipe();put(rp,write_json(r));
    check(apply_translation_recipe(in,out,rp,cancel,error),error.c_str());auto expected=body;expected.replace(expected.find("0;"),1,"1");check(get(out/"funcs_000.cpp")==crlf(prefix+"// Synthetic translation fixture.\n"+expected)&&get(out/"recomp_funcs.h")==crlf(header),"exact native output including CRLF/header copy");const auto committed=get(out/"funcs_000.cpp");
    auto rejects=[&](JSONValue bad){put(rp,write_json(bad));check(!apply_translation_recipe(in,out,rp,cancel,error)&&!error.empty(),"bad recipe accepted");check(get(out/"funcs_000.cpp")==committed,"failed recipe replaced previous output");};
    auto bad=r;fn(bad).object()->at("before")=std::string(64,'0');rejects(bad);bad=r;fn(bad).object()->at("after")=std::string(64,'0');rejects(bad);bad=r;file(bad).object()->at("before_sha256")=std::string(64,'0');rejects(bad);bad=r;file(bad).object()->at("output_bytes_sha256")=std::string(64,'0');rejects(bad);
    bad=r;file(bad).object()->at("name")="../escape.cpp";rejects(bad);bad=r;bad.object()->at("edit_count")=2;rejects(bad);bad=r;fn(bad).object()->at("edits").array()->at(0).object()->at("start")=999999;rejects(bad);
    bad=r;auto& e=*fn(bad).object()->at("edits").array();e.push_back(e.front());rejects(bad);bad=r;fn(bad).object()->at("edits").array()->at(0).object()->at("text")="\r";rejects(bad);
    put(rp,write_json(r));cancel=true;check(!apply_translation_recipe(in,out,rp,cancel,error)&&get(out/"funcs_000.cpp")==committed,"cancel preserves previous output");cancel=false;
    check(!apply_translation_recipe(in,in,rp,cancel,error)&&!apply_translation_recipe(in,in/"nested",rp,cancel,error),"same/nested tree rejected");put(out/"personal.txt","keep");check(!apply_translation_recipe(in,out,rp,cancel,error)&&get(out/"personal.txt")=="keep","unknown destination content preserved");fs::remove(out/"personal.txt");put(in/"unexpected.cpp","extra");check(!apply_translation_recipe(in,out,rp,cancel,error),"unexpected generated source rejected");fs::remove(in/"unexpected.cpp");put(in/"recomp_funcs.h",crlf(header)+"drift");check(!apply_translation_recipe(in,out,rp,cancel,error)&&get(out/"funcs_000.cpp")==committed,"header drift rejected transactionally");put(in/"recomp_funcs.h",crlf(header));
    check(apply_translation_recipe(in,out,rp,cancel,error)&&get(out/"funcs_000.cpp")==committed,"successful rebuild replaces only validated generated output");
    std::cout<<"Native sparse translation recipe exact bytes, function/file hashes, paths, edit bounds, cancellation and transactional preservation passed.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
