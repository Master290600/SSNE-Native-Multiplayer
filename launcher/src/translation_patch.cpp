#include "translation_patch.h"
#include "launcher_json.h"
#include "gc/sha256.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace launcher { namespace {
namespace fs=std::filesystem;
std::string path_utf8(const fs::path& p){const auto text=p.u8string();return {text.begin(),text.end()};}
constexpr size_t file_limit=16u*1024*1024,total_limit=256u*1024*1024;
constexpr const char* includes="#include \"ssne/native_party.h\"\n#include \"ssne/replica_simulation.h\"\n#include \"ssne/native_results.h\"\n#include <algorithm>\n";
[[noreturn]]void fail(const std::string& text){throw std::runtime_error("Translation recipe: "+text);}
const JSONValue& field(const JSONValue& v,const char* name){const auto p=v.find(name);if(!p)fail(std::string("missing ")+name);return *p;}
std::string text(const JSONValue& v,const char* name){const auto p=field(v,name).string();if(!p)fail(std::string("expected string ")+name);return *p;}
size_t count(const JSONValue& v,const char* name,size_t limit){int64_t n;if(!field(v,name).integer(n)||n<0||uint64_t(n)>limit)fail(std::string("invalid ")+name);return size_t(n);}
const JSONValue::Array& array(const JSONValue& v,const char* name){const auto p=field(v,name).array();if(!p)fail(std::string("expected array ")+name);return *p;}
bool hex(std::string_view s,size_t n,bool upper=false){if(s.size()!=n)return false;for(char c:s)if(!((c>='0'&&c<='9')||(upper?(c>='A'&&c<='F'):(c>='a'&&c<='f'))))return false;return true;}
std::string hash(std::string_view s){gc::Sha256 h;h.update(s.data(),s.size());return gc::Sha256::hex(h.finish());}
void verify(std::string_view bytes,const JSONValue& v,const char* name,const std::string& context){const auto expected=text(v,name);if(!hex(expected,64)||hash(bytes)!=expected)fail(context+" "+name+" mismatch");}
std::string read(const fs::path& p,size_t limit){
    std::error_code ec;const auto status=fs::symlink_status(p,ec);if(ec||!fs::is_regular_file(status)||fs::is_symlink(status))fail("not a regular file: "+path_utf8(p.filename()));
    const auto n=fs::file_size(p,ec);if(ec||n>limit)fail("file size limit: "+path_utf8(p.filename()));
    std::string out(size_t(n),'\0');std::ifstream f(p,std::ios::binary);if(!f||(n&&!f.read(out.data(),std::streamsize(n)))||f.peek()!=std::char_traits<char>::eof())fail("cannot read: "+path_utf8(p.filename()));return out;
}
bool name_ok(const std::string& name){
    if(name.empty()||name.size()>120||name[0]=='.'||!((name[0]>='A'&&name[0]<='Z')||(name[0]>='a'&&name[0]<='z')||(name[0]>='0'&&name[0]<='9')||name[0]=='_'))return false;
    for(char c:name)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.'))return false;
    return name.ends_with(".cpp")||name.ends_with(".h");
}
std::string lower(std::string s){
#ifdef _WIN32
    for(char& c:s)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
#endif
    return s;
}
bool contains(const fs::path& root,const fs::path& p){auto a=root.begin(),b=p.begin();for(;a!=root.end();++a,++b)if(b==p.end()||lower(path_utf8(*a))!=lower(path_utf8(*b)))return false;return true;}
void check_tree(const fs::path& dir,const std::set<std::string>& expected,bool exact){
    std::error_code ec;if(!fs::exists(dir,ec)){if(exact)fail("input directory missing");return;}
    if(fs::is_symlink(fs::symlink_status(dir,ec))||!fs::is_directory(dir,ec))fail("unsafe generated directory");
    std::set<std::string> actual;
    for(const auto& entry:fs::directory_iterator(dir)){
        const auto name=path_utf8(entry.path().filename());const auto status=entry.symlink_status();
        if(!fs::is_regular_file(status)||fs::is_symlink(status)||!expected.contains(name))fail("unexpected generated path: "+name);
        actual.insert(name);
    }
    if(exact&&actual!=expected)fail("generated file set differs");
}
std::string lf_ascii(std::string_view raw){
    std::string out;out.reserve(raw.size());for(size_t i=0;i<raw.size();++i){const auto c=uint8_t(raw[i]);if(!c||c>127)fail("generated source is not ASCII");if(c=='\r'){if(i+1==raw.size()||raw[i+1]!='\n')fail("standalone CR in generated source");++i;out+='\n';}else out+=char(c);}return out;
}
void edit_text(const std::string& s){for(unsigned char c:s)if(!c||c>127||c=='\r')fail("edit must be LF ASCII");}
std::string physical(std::string_view s,bool crlf){if(!crlf)return std::string(s);std::string out;out.reserve(s.size()+s.size()/30);for(char c:s){if(c=='\n')out+='\r';out+=c;}return out;}
struct Stage {fs::path path;~Stage(){if(!path.empty()){std::error_code ec;fs::remove_all(path,ec);}}};
}
bool apply_translation_recipe(const fs::path& input_dir,const fs::path& output_dir,const fs::path& recipe,std::atomic<bool>& cancel,std::string& error){
    error.clear();try{
        auto cancelled=[&]{if(cancel.load())fail("cancelled");};cancelled();
        const auto input=fs::weakly_canonical(fs::absolute(input_dir)),output=fs::weakly_canonical(fs::absolute(output_dir));
        if(contains(input,output)||contains(output,input)||output==output.root_path())fail("input/output trees must be separate");
        JSONValue root;std::string parse_error;if(!parse_json(read(recipe,16u*1024*1024),root,parse_error))fail(parse_error);
        if(count(root,"schema",1)!=1||text(root,"kind")!="ssne-pal-translation-edits"||text(root,"format")!="ascii-lf")fail("unsupported schema");
        const auto newline=text(root,"output_newline");if(newline!="lf"&&newline!="crlf")fail("invalid output newline");
        if(text(root,"header_prefix")!=includes)fail("unexpected header includes");
        for(auto key:{"runtime_kit_sha256","symbols_sha256","patcher_sha256"})if(!hex(text(root,key),64))fail(std::string("invalid ")+key);
        const auto& files=array(root,"files");if(files.empty()||files.size()>256)fail("file count limit");
        std::set<std::string> names,folded;
        for(const auto& f:files){const auto name=text(f,"name");if(!name_ok(name)||!names.insert(name).second||!folded.insert(lower(name)).second)fail("invalid/duplicate unit path");}
        check_tree(input,names,true);check_tree(output,names,false);
        std::map<std::string,std::string> generated;size_t total=0,functions=0,edits=0,units=0;
        for(const auto& f:files){
            cancelled();const auto name=text(f,"name"),raw=read(input/fs::u8path(name),file_limit);verify(raw,f,"input_bytes_sha256",name);auto source=lf_ascii(raw);verify(source,f,"before_sha256",name);
            const auto& changes=array(f,"functions");if(changes.size()>10000||(!changes.empty()&&!name.ends_with(".cpp")))fail("invalid function list");std::set<std::string> addresses;
            if(!changes.empty())++units;
            for(const auto& fn:changes){
                cancelled();const auto address=text(fn,"address");if(!hex(address,8,true)||!addresses.insert(address).second)fail("invalid/duplicate function address");
                const auto marker="\nvoid f_"+address;const auto start=source.find(marker);if(start==std::string::npos||source.find(marker,start+1)!=std::string::npos)fail(name+" missing/ambiguous function "+address);
                auto end=source.find("\nvoid f_",start+1);if(end==std::string::npos)end=source.size();auto body=source.substr(start,end-start);verify(body,fn,"before",name+":"+address);
                const auto& replacements=array(fn,"edits");if(replacements.empty()||replacements.size()>4096)fail("edit count limit");
                struct Edit {size_t start,length;std::string text;};std::vector<Edit> parsed;size_t previous_end=0;bool first=true;
                for(const auto& e:replacements){const auto pos=count(e,"start",body.size()),length=count(e,"length",body.size());const auto replacement=text(e,"text");edit_text(replacement);
                    if(length>body.size()-pos||(!first&&pos<previous_end))fail(name+":"+address+" overlapping/out-of-range edit");
                    previous_end=pos+std::max(size_t(1),length);first=false;parsed.push_back({pos,length,replacement});
                }
                for(auto it=parsed.rbegin();it!=parsed.rend();++it){body.replace(it->start,it->length,it->text);if(body.size()>file_limit)fail("patched function size limit");}
                verify(body,fn,"after",name+":"+address);source.replace(start,end-start,body);++functions;edits+=parsed.size();if(functions>100000||edits>100000)fail("recipe operation limit");
            }
            if(!changes.empty())source=std::string(includes)+source;verify(source,f,"after_sha256",name);auto bytes=physical(source,newline=="crlf");verify(bytes,f,"output_bytes_sha256",name);
            if(bytes.size()>file_limit||bytes.size()>total_limit-total)fail("generated output size limit");total+=bytes.size();generated.emplace(name,std::move(bytes));
        }
        if(functions!=count(root,"function_count",100000)||edits!=count(root,"edit_count",100000)||units!=count(root,"changed_unit_count",256))fail("operation counts differ");cancelled();
        fs::create_directories(output.parent_path());Stage stage;static std::atomic<uint64_t> serial{0};
        for(unsigned tries=0;tries<100;++tries){const auto suffix=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial);const auto p=output.parent_path()/fs::u8path(".patch-"+suffix);if(fs::create_directory(p)){stage.path=p;break;}}
        if(stage.path.empty())fail("cannot stage output directory");
        for(const auto& [name,bytes]:generated){cancelled();std::ofstream file(stage.path/fs::u8path(name),std::ios::binary|std::ios::trunc);file.write(bytes.data(),std::streamsize(bytes.size()));file.close();if(!file)fail("cannot write "+name);}
        cancelled();auto backup=stage.path;backup+=".previous";bool moved_old=false;
        try{if(fs::exists(output)){check_tree(output,names,false);fs::rename(output,backup);moved_old=true;}fs::rename(stage.path,output);stage.path.clear();}
        catch(...){if(moved_old){std::error_code ec;if(!fs::exists(output,ec))fs::rename(backup,output,ec);if(ec)fail("output commit failed; previous output retained at "+path_utf8(backup));}throw;}
        if(moved_old){std::error_code ec;fs::remove_all(backup,ec);}return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
