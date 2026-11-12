#include "game_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "gc/sha256.h"
#include "gc/zip.h"
#include "platform.h"
#include "launcher_json.h"
#include "translation_patch.h"
#include <iterator>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}
std::string expand(std::string v, const std::string& kit) {
    for (size_t p; (p = v.find("kit:")) != std::string::npos;) v.replace(p, 4, kit + "/");
    return v;
}
bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(P(p), ec);
}
// A bare program name ("clang++") is looked up in PATH, as a shell would. Returns the name unchanged if not found.
std::string find_in_path(const std::string& name) {
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const char* env = std::getenv("PATH");
    std::string path = env ? env : "";
    for (size_t a = 0; a <= path.size();) {
        size_t b = path.find(sep, a);
        if (b == std::string::npos) b = path.size();
        if (b > a) {
            fs::path c = P(path.substr(a, b - a)) / P(name);
            std::error_code ec;
            if (fs::is_regular_file(c, ec)) return S(c);
        }
        a = b + 1;
    }
    return name;
}
} // namespace

// The unpacked compiler is marked with the archive it came from, so a kit that switches to another compiler
// release (e.g. kit 10: UCRT -> MSVCRT for Windows 7/8.1) downloads it again instead of using the old one.
static std::string toolchain_stamp_path(const BuildKit& kit) { return S(P(kit.toolchain_dir) / "toolchain-url.txt"); }
bool BuildKit::compiler_present() const {
    if (compiler.empty() || !exists(compiler)) return false;
    if(target==BuildTarget::NativeMultiplayer){std::string error;if(launcher::runtime_file_sha256(P(compiler),error)!=runtime.compiler_sha256)return false;}
    if (toolchain_url.empty()) return true;  // a compiler installed by hand
    std::ifstream f(P(toolchain_stamp_path(*this)));
    std::string url;
    std::getline(f, url);
    return url == toolchain_url;
}

std::string BuildKit::symbols_for(const std::string& game_id) const {
    for (auto& [id, file] : region_symbols)
        if (id == game_id) return file;
    return symbols;
}

std::string built_kit_version(const BuildKit& kit) {
    std::ifstream in(P(kit.dir) / "built-with.txt");
    std::string v;
    if (in) std::getline(in, v);
    return trim(v);
}

bool load_build_kit(const std::string& kit_dir, BuildKit& kit, std::string& error) {
    std::ifstream in(P(kit_dir) / "kit.ini");
    if (!in) { error = "build kit not found"; return false; }
    kit = BuildKit{};
    kit.dir = kit_dir;
    std::string line, compiler_rel;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "recomp") kit.recomp = expand(v, kit_dir);
        else if (k == "version") kit.version = v;
        else if (k.rfind("symbols.", 0) == 0) kit.region_symbols.push_back({k.substr(8), expand(v, kit_dir)});
        else if (k == "symbols") kit.symbols = expand(v, kit_dir);
        else if (k == "toolchain_dir") kit.toolchain_dir = expand(v, kit_dir);
        else if (k == "toolchain_url") kit.toolchain_url = v;
        else if (k == "toolchain_sha256") kit.toolchain_sha256 = v;
        else if (k == "toolchain_name") kit.toolchain_name = v;
        else if (k == "toolchain_skip") kit.toolchain_skip = split(v);
        else if (k == "toolchain_size_mb") kit.toolchain_size_mb = atoi(v.c_str());
        else if (k == "compiler") compiler_rel = v;
        else if (k == "compiler_flags") kit.compiler_flags = split(v);
        else if (k == "cxxflags") { kit.cxxflags.clear(); for (auto& t : split(v)) kit.cxxflags.push_back(expand(t, kit_dir)); }
        else if (k == "link") { kit.link.clear(); for (auto& t : split(v)) kit.link.push_back(expand(t, kit_dir)); }
    }
    // toolchain_dir is only needed for a downloaded compiler; a kit may instead name the system's own compiler.
    if (kit.recomp.empty() || compiler_rel.empty() || kit.link.empty() ||
        (kit.toolchain_dir.empty() && !kit.toolchain_url.empty())) {
        error = "build kit settings are incomplete (kit.ini)";
        return false;
    }
    if (P(compiler_rel).is_absolute()) kit.compiler = compiler_rel;
    else if (compiler_rel.find_first_of("/\\") == std::string::npos && kit.toolchain_url.empty())
        kit.compiler = find_in_path(compiler_rel);  // e.g. "compiler = clang++": the one installed on this system
    else kit.compiler = S(P(kit.toolchain_dir) / P(compiler_rel));
    return true;
}

void GameBuilder::log(const std::string& line) {
#ifdef _WIN32
    FILE* f = _wfopen(P(log_path).c_str(), L"a");
#else
    FILE* f = fopen(log_path.c_str(), "a");
#endif
    if (f) {
        std::time_t t = std::time(nullptr);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
        fprintf(f, "[%s] %s\n", ts, line.c_str());
        fclose(f);
    }
}

bool GameBuilder::run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& logfile,
                      std::atomic<bool>& cancel, std::string& error) {
    std::string cmd;
    for (auto& a : argv) cmd += (cmd.empty() ? "" : " ") + a;
    log("run: " + cmd);
    platform::Process* p = platform::spawn(argv, cwd, logfile, error);
    if (!p) return false;
    int code = 0;
    while (!platform::poll(p, code)) {
        if (cancel) { platform::kill(p); platform::release(p); error = "cancelled"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    platform::release(p);
    if (code != 0) { error = "exit code " + std::to_string(code); return false; }
    return true;
}

bool GameBuilder::get_toolchain(const BuildKit& kit, const Report& report, std::atomic<bool>& cancel,
                                std::string& error) {
    if (kit.compiler_present()) { log("compiler: " + kit.compiler); return true; }
    if (kit.toolchain_url.empty()) {
        error = "compiler not found: " + kit.compiler;
#ifndef _WIN32
        error += ". Install " + (kit.toolchain_name.empty() ? std::string("a C++ compiler") : kit.toolchain_name) +
                 " with your package manager, or change \"compiler\" in " + S(P(kit.dir) / "kit.ini");
#endif
        return false;
    }
    std::error_code ec;
    fs::path dl_dir = P(kit.dir) / "downloads";
    fs::create_directories(dl_dir, ec);
    std::string name = kit.toolchain_url.substr(kit.toolchain_url.rfind('/') + 1);
    fs::path zip = dl_dir / P(name), part = dl_dir / P(name + ".part");
    if (!fs::exists(zip, ec)) {
        log("downloading " + kit.toolchain_url);
        std::string label = "Downloading the compiler (" + kit.toolchain_name + ")";
        bool ok = platform::download(kit.toolchain_url, S(part), [&](uint64_t d, uint64_t t) {
            double total = t ? double(t) : kit.toolchain_size_mb * 1048576.0;
            char buf[64];
            snprintf(buf, sizeof(buf), "  %.0f / %.0f MB", d / 1048576.0, total / 1048576.0);
            report(label + buf, total > 0 ? std::min(1.0, d / total) : 0.0);
            return !cancel.load();
        }, error);
        if (!ok) {
            fs::remove(part, ec);
            error = "download failed: " + error + ". If it keeps failing (e.g. Windows 7 without TLS 1.2), download " +
                    kit.toolchain_url + " in a web browser and put the .zip file into " + S(dl_dir) +
                    ", then build again.";
            return false;
        }
        fs::rename(part, zip, ec);
        if (ec) { error = "cannot save " + S(zip); return false; }
    }
    report("Checking the download", 0);
    std::string sha = gc::sha256_file(S(zip));
    log("toolchain sha256: " + sha);
    if (!kit.toolchain_sha256.empty() && sha != kit.toolchain_sha256) {
        fs::remove(zip, ec);
        error = "the downloaded compiler does not match the expected checksum; try again";
        return false;
    }
    // Unpack, dropping the archive's top-level folder and the parts we don't need.
    std::vector<gc::zip::Entry> entries;
    if (!gc::zip::list(S(zip), entries, error)) return false;
    std::string root;
    if (!entries.empty()) {
        root = entries[0].name.substr(0, entries[0].name.find('/') + 1);
        for (auto& e : entries)
            if (e.name.compare(0, root.size(), root) != 0) { root.clear(); break; }
    }
    fs::path tmp = P(kit.toolchain_dir + ".tmp");
    fs::remove_all(tmp, ec);
    log("unpacking to " + kit.toolchain_dir);
    bool ok = gc::zip::extract(S(zip), S(tmp), [&](const std::string& n) -> std::string {
        std::string rel = n.substr(root.size());
        for (auto& s : kit.toolchain_skip)
            if (rel.compare(0, s.size(), s) == 0) return {};
        return rel;
    }, [&](uint64_t d, uint64_t t) {
        report("Unpacking the compiler", t ? double(d) / double(t) : 0.0);
        return !cancel.load();
    }, error);
    if (!ok) { fs::remove_all(tmp, ec); error = "unpacking the compiler failed: " + error; return false; }
    fs::remove_all(P(kit.toolchain_dir), ec);
    fs::rename(tmp, P(kit.toolchain_dir), ec);
    if (ec) { error = "cannot move the compiler into place: " + ec.message(); return false; }
    { std::ofstream st(P(toolchain_stamp_path(kit))); st << kit.toolchain_url << "\n"; }
    if (!kit.compiler_present()) { error = "the compiler download does not contain " + kit.compiler; return false; }
    fs::remove(zip, ec);  // 150+ MB no longer needed
    return true;
}

bool GameBuilder::build(const BuildKit& kit, const std::string& data_dir, const std::string& out_exe,
                        const Report& report, std::atomic<bool>& cancel, std::string& error) {
    if(kit.target==BuildTarget::NativeMultiplayer)return build_native(kit,data_dir,out_exe,report,cancel,error);
    std::error_code ec;
    fs::path work = P(kit.dir) / "work";
    fs::path logs = work / "logs";
    fs::path src = work / "recompiled", obj = work / "obj";
    fs::remove_all(src, ec);
    fs::remove_all(obj, ec);
    fs::remove_all(logs, ec);
    fs::create_directories(logs, ec);
    fs::create_directories(obj, ec);
    log_path = S(logs / "build.log");
    log("SSNE-Native game build");

    // 1. Compiler
    report("Preparing the compiler", 0);
    if (!get_toolchain(kit, report, cancel, error)) { log("error: " + error); return false; }

    // 2. Translate the game's executable from the installed disc files.
    report("Translating the game code", 0);
    std::string dol = S(P(data_dir) / "sys" / "main.dol");
    if (!exists(dol)) { error = "game data is not installed (sys/main.dol missing)"; return false; }
    std::vector<std::string> rargv = {kit.recomp, dol, S(src), "--no-comments"};
    std::string game_id;  // the installed disc's ID picks the function-name map (USA / Europe)
    {
        std::ifstream boot(P(data_dir) / "sys" / "boot.bin", std::ios::binary);
        char id[6] = {};
        if (boot.read(id, 6)) game_id.assign(id, 6);
    }
    std::string symbols = kit.symbols_for(game_id);
    log("game " + (game_id.empty() ? std::string("?") : game_id) + ", symbols " + symbols);
    if (!symbols.empty()) { rargv.push_back("--map"); rargv.push_back(symbols); }
    if (!run(rargv, kit.dir, S(logs / "recomp.log"), cancel, error)) {
        error = "translating the game code failed (" + error + "); see " + S(logs / "recomp.log");
        log("error: " + error);
        return false;
    }
    if (cancel) { error = "cancelled"; return false; }

    // 3. Compile the generated sources in parallel.
    std::vector<fs::path> sources;
    for (auto& e : fs::directory_iterator(src, ec))
        if (e.path().extension() == ".cpp") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end());
    if (sources.empty()) { error = "the translator produced no code; see " + S(logs / "recomp.log"); return false; }
    unsigned jobs = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    log("compiling " + std::to_string(sources.size()) + " files with " + std::to_string(jobs) + " jobs");
    struct Job { platform::Process* p; size_t idx; };
    std::vector<Job> running;
    std::vector<std::string> objects(sources.size());
    size_t next = 0, done = 0;
    bool failed = false;
    std::string fail_log;
    auto status = [&] {
        report("Compiling the game (" + std::to_string(done) + " of " + std::to_string(sources.size()) + " files)",
               double(done) / double(sources.size()));
    };
    status();
    while ((next < sources.size() && !failed) || !running.empty()) {
        while (!failed && !cancel && next < sources.size() && running.size() < jobs) {
            std::string stem = S(sources[next].stem());
            objects[next] = S(obj / P(stem + ".o"));
            std::vector<std::string> argv = {kit.compiler};
            argv.insert(argv.end(), kit.compiler_flags.begin(), kit.compiler_flags.end());
            argv.insert(argv.end(), kit.cxxflags.begin(), kit.cxxflags.end());
            argv.push_back("-I" + S(src));
            argv.insert(argv.end(), {"-c", S(sources[next]), "-o", objects[next]});
            std::string lg = S(logs / P(stem + ".log")), err;
            platform::Process* p = platform::spawn(argv, S(src), lg, err);
            if (!p) { failed = true; error = err; break; }
            running.push_back({p, next});
            next++;
        }
        for (size_t i = 0; i < running.size();) {
            int code = 0;
            if (cancel) { platform::kill(running[i].p); platform::release(running[i].p); running.erase(running.begin() + i); continue; }
            if (platform::poll(running[i].p, code)) {
                platform::release(running[i].p);
                if (code != 0 && !failed) {
                    failed = true;
                    fail_log = S(logs / P(S(sources[running[i].idx].stem()) + ".log"));
                    error = "compiling " + S(sources[running[i].idx].filename()) + " failed; see " + fail_log;
                }
                running.erase(running.begin() + i);
                done++;
                status();
            } else {
                i++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (cancel) { error = "cancelled"; return false; }
    if (failed) { log("error: " + error); return false; }

    // 4. Link with the prebuilt port, runtime and library code.
    report("Linking", 0.5);
    fs::path tmp_exe = P(out_exe + ".new");
    fs::remove(tmp_exe, ec);
    std::vector<std::string> argv = {kit.compiler};
    argv.insert(argv.end(), kit.compiler_flags.begin(), kit.compiler_flags.end());
    for (auto& t : kit.link) {
        if (t == "{objs}") argv.insert(argv.end(), objects.begin(), objects.end());
        else argv.push_back(t);
    }
    argv.insert(argv.end(), {"-o", S(tmp_exe)});
    if (!run(argv, S(work), S(logs / "link.log"), cancel, error)) {
        error = "linking failed (" + error + "); see " + S(logs / "link.log");
        log("error: " + error);
        return false;
    }
    fs::remove(P(out_exe), ec);
    if (ec && exists(out_exe)) { error = "cannot replace " + out_exe + " (is the game still running?)"; return false; }
    fs::rename(tmp_exe, P(out_exe), ec);
    if (ec) { error = "cannot create " + out_exe + ": " + ec.message(); return false; }

    // Remember which kit version built the game: a newer kit (engine update) makes the launcher rebuild it.
    {
        std::ofstream stamp(P(kit.dir) / "built-with.txt", std::ios::trunc);
        stamp << kit.version << "\n";
    }
    // The generated code and objects came from the player's own disc and are only needed for the build.
    fs::remove_all(src, ec);
    fs::remove_all(obj, ec);
    log("done: " + out_exe);
    report("Done", 1);
    return true;
}

// Native multiplayer target: user-owned translated code stays in a local cache.
namespace {
using NativeJSON=launcher::JSONValue;
void native_need(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
std::string native_hash(const fs::path& p){std::string error;auto h=launcher::runtime_file_sha256(p,error);native_need(!h.empty(),error);return h;}
NativeJSON native_read_json(const fs::path& p){
    std::error_code ec;const auto size=fs::file_size(p,ec);native_need(!ec&&size<=16u*1024*1024,"build metadata is missing or too large: "+S(p));
    std::ifstream in(p,std::ios::binary);native_need(bool(in),"cannot open "+S(p));
    std::string bytes((std::istreambuf_iterator<char>(in)),{});native_need(in.eof()||bool(in),"cannot read "+S(p));
    NativeJSON v;std::string error;if(!launcher::parse_json(bytes,v,error))throw std::runtime_error("invalid "+S(p)+": "+error);return v;
}
const std::string* native_string(const NativeJSON& v,const char* key){auto p=v.find(key);return p?p->string():nullptr;}
std::string native_required_string(const NativeJSON& v,const char* key){auto p=native_string(v,key);native_need(p!=nullptr,std::string("missing build metadata field ")+key);return *p;}
bool native_true(const NativeJSON& v,const char* key){auto p=v.find(key);bool b=false;return p&&p->boolean(b)&&b;}
bool native_contained(const fs::path& root,const fs::path& path){
    std::error_code ec;auto base=fs::weakly_canonical(root,ec);if(ec)return false;auto full=fs::weakly_canonical(path,ec);if(ec)return false;
    auto a=base.begin(),b=full.begin();for(;a!=base.end()&&b!=full.end();++a,++b)if(*a!=*b)return false;return a==base.end()&&b!=full.end();
}
void native_remove_tree(const fs::path& root,const fs::path& path){native_need(native_contained(root,path),"refusing to remove a cache path outside "+S(root));std::error_code ec;fs::remove_all(path,ec);native_need(!ec,"cannot clear cache stage "+S(path)+": "+ec.message());}
void native_replace(const fs::path& from,const fs::path& to){
#ifdef _WIN32
    native_need(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"cannot replace "+S(to)+" (Windows error "+std::to_string(GetLastError())+")");
#else
    std::error_code ec;fs::rename(from,to,ec);native_need(!ec,"cannot replace "+S(to)+": "+ec.message());
#endif
}
void native_write(const fs::path& path,const std::string& bytes){
    auto pending=path;pending+=".pending";std::ofstream out(pending,std::ios::binary|std::ios::trunc);native_need(bool(out),"cannot create "+S(pending));
    out.write(bytes.data(),std::streamsize(bytes.size()));out.flush();native_need(bool(out),"cannot write "+S(pending));out.close();native_replace(pending,path);
}
void native_write_json(const fs::path& p,const NativeJSON& v){native_write(p,launcher::write_json(v,true));}
struct NativeBuildLock {
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit NativeBuildLock(const fs::path& path){handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);native_need(handle!=INVALID_HANDLE_VALUE,"another native game build is running or build cache is not writable");}
    ~NativeBuildLock(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
#else
    int handle=-1;
    explicit NativeBuildLock(const fs::path& path){handle=::open(path.c_str(),O_CREAT|O_RDWR,0600);if(handle<0||::flock(handle,LOCK_EX|LOCK_NB)!=0){if(handle>=0)::close(handle);handle=-1;throw std::runtime_error("another native game build is running or build cache is not writable");}}
    ~NativeBuildLock(){if(handle>=0){::flock(handle,LOCK_UN);::close(handle);}}
#endif
};
std::vector<fs::path> native_generated_files(const fs::path& root){
    std::vector<fs::path> files;for(const auto& e:fs::directory_iterator(root))if(e.is_regular_file()&&(e.path().extension()==".cpp"||e.path().extension()==".h"))files.push_back(e.path());
    std::sort(files.begin(),files.end());native_need(!files.empty()&&files.size()<4096&&fs::is_regular_file(root/"recomp_funcs.h"),"native translator produced incomplete output");return files;
}
NativeJSON native_source_record(const fs::path& root,const std::string& generation,std::atomic<bool>& cancel){
    NativeJSON::Array files;for(const auto& path:native_generated_files(root)){native_need(!cancel,"cancelled");files.emplace_back(NativeJSON::Object{{"name",S(path.filename())},{"sha256",native_hash(path)}});}
    return NativeJSON::Object{{"schema",1},{"generation",generation},{"files",std::move(files)}};
}
bool native_source_cache(const fs::path& root,const std::string& generation,std::atomic<bool>& cancel){
    try{auto record=native_read_json(root/"source-cache.json");auto key=native_string(record,"generation");auto list=record.find("files");auto files=list?list->array():nullptr;if(!key||*key!=generation||!files)return false;
        const auto actual=native_generated_files(root);if(actual.size()!=files->size())return false;std::set<std::string> expected;
        for(const auto& f:*files){if(cancel)return false;const auto name=native_required_string(f,"name");if(fs::path(name).filename()!=fs::path(name)||!expected.insert(name).second)return false;if(native_hash(root/P(name))!=native_required_string(f,"sha256"))return false;}return true;
    }catch(...){return false;}
}
const std::vector<std::string> native_publish_names={"SSNE-Native-Multiplayer-Game.exe","SSNE-Native-Multiplayer.exe","release-manifest.json","native-build.json"};
bool native_publish_name(const std::string& name){return std::find(native_publish_names.begin(),native_publish_names.end(),name)!=native_publish_names.end();}
void native_recover_publish(const fs::path& dir){
    const auto stage=dir/".native-build-transaction";if(!fs::exists(stage))return;
    // All old/new filenames are a fixed allowlist; never follow journal paths.
    const auto journal=stage/"journal.json";
    if(!fs::exists(journal)){native_need(!fs::exists(stage/"old")||fs::is_empty(stage/"old"),"unfinished native publication has rollback files without a journal; keep them for repair review: "+S(stage));native_remove_tree(dir,stage);return;}
    auto record=native_read_json(journal);const auto items_value=record.find("items");auto items=items_value?items_value->array():nullptr;native_need(items&&items->size()==native_publish_names.size(),"invalid native rollback journal");
    const auto state=native_required_string(record,"state");native_need(state=="prepared"||state=="committed","invalid native publication state");std::set<std::string> names;
    for(const auto& item:*items){const auto name=native_required_string(item,"name");native_need(native_publish_name(name)&&names.insert(name).second,"unsafe/duplicate native rollback filename");const auto target=dir/P(name),backup=stage/"old"/P(name);const auto old=native_required_string(item,"old_sha256"),fresh=native_required_string(item,"new_sha256");
        if(state=="committed"){native_need(fs::is_regular_file(target)&&native_hash(target)==fresh,"committed native target changed; preserve rollback files for review");continue;}
        if(native_true(item,"old_existed")){
            if(fs::exists(backup))native_need(native_hash(backup)==old&&(!fs::exists(target)||native_hash(target)==fresh||native_hash(target)==old),"native rollback files changed; stop for review");
            else native_need(fs::exists(target)&&native_hash(target)==old,"native rollback copy is missing; stop for review");
        }else native_need(!fs::exists(target)||native_hash(target)==fresh,"new native target changed; stop for review");
    }
    if(state!="committed")for(auto it=items->rbegin();it!=items->rend();++it){const auto name=native_required_string(*it,"name");const auto target=dir/P(name),backup=stage/"old"/P(name);
        if(native_true(*it,"old_existed")){if(fs::exists(backup))native_replace(backup,target);}else if(fs::exists(target)){std::error_code ec;fs::remove(target,ec);native_need(!ec,"cannot remove rolled-back target "+S(target));}}
    native_remove_tree(dir,stage);
}
void native_publish(const launcher::RuntimeKit& kit,const fs::path& candidate,const fs::path& dir,std::atomic<bool>& cancel){
    native_recover_publish(dir);native_need(!cancel,"cancelled");const auto stage=dir/".native-build-transaction";fs::create_directories(stage/"new");fs::create_directories(stage/"old");
    const auto hash=native_hash(candidate);auto manifest=NativeJSON(NativeJSON::Object{});
    if(fs::exists(dir/"release-manifest.json")){try{auto old=native_read_json(dir/"release-manifest.json");if(old.object())manifest=std::move(old);}catch(...){}}
    auto& m=*manifest.object();m["build"]=kit.build;m["protocol"]=kit.protocol;m["entrypoint"]="native";m["destination"]="SSNE-Native-Multiplayer-Game.exe";m["renderer"]="gpu";m["product_sha256"]=hash;m["entrypoint_sha256"]=hash;m["kit_descriptor_sha256"]=kit.descriptor_sha256;
    m["acceptance"]="Locally rebuilt from the owned PAL disc; native gameplay verification is separate from build success.";
    NativeJSON receipt=NativeJSON::Object{{"schema",1},{"fingerprint",kit.fingerprint},{"exe_sha256",hash},{"descriptor_sha256",kit.descriptor_sha256},{"build",kit.build},{"protocol",kit.protocol}};
    bool journal_written=false;
    try{
        for(unsigned i=0;i<2;++i){native_need(!cancel,"cancelled");fs::copy_file(candidate,stage/"new"/P(native_publish_names[i]));}
        native_write_json(stage/"new/release-manifest.json",manifest);native_write_json(stage/"new/native-build.json",receipt);
        NativeJSON::Array entries;for(const auto& name:native_publish_names){const auto target=dir/P(name);const bool existed=fs::exists(target);entries.emplace_back(NativeJSON::Object{{"name",name},{"old_existed",existed},{"old_sha256",existed?native_hash(target):std::string{}},{"new_sha256",native_hash(stage/"new"/P(name))}});}
        NativeJSON journal=NativeJSON::Object{{"state","prepared"},{"items",entries}};native_write_json(stage/"journal.json",journal);journal_written=true;
        for(const auto& name:native_publish_names){native_need(!cancel,"cancelled");const auto target=dir/P(name);if(fs::exists(target))fs::rename(target,stage/"old"/P(name));native_need(!cancel,"cancelled");fs::rename(stage/"new"/P(name),target);}
        native_need(!cancel,"cancelled");for(const auto& entry:entries)native_need(native_hash(dir/P(native_required_string(entry,"name")))==native_required_string(entry,"new_sha256"),"published native file differs");
        (*journal.object())["state"]="committed";native_write_json(stage/"journal.json",journal);
    }catch(...){if(journal_written)native_recover_publish(dir);else native_remove_tree(dir,stage);throw;}
    // Committed targets are verified. A cleanup failure may leave only a
    // committed journal, which the next build verifies and retires safely.
    try{native_recover_publish(dir);}catch(...){}
}
std::string native_rsp(std::string arg){native_need(arg.find_first_of("\r\n\0",0,3)==std::string::npos,"invalid newline/NUL in linker argument");std::replace(arg.begin(),arg.end(),'\\','/');std::string out="\"";for(char c:arg){if(c=='"')out+='\\';out+=c;}return out+"\"\n";}
}

std::string BuildKit::output_exe(const std::string& project) const{return target==BuildTarget::NativeMultiplayer?S(P(runtime.project_dir)/"Multiplayer/SSNE-Native-Multiplayer-Game.exe"):S(P(project)/platform::port_exe_name());}
std::string BuildKit::working_directory(const std::string& project) const{return target==BuildTarget::NativeMultiplayer?S(P(runtime.project_dir)/"Multiplayer"):project;}
std::string BuildKit::target_fingerprint() const{return target==BuildTarget::NativeMultiplayer?runtime.fingerprint:"standard:"+version;}
bool select_build_target(const BuildKit& base,const std::string& project,const std::string& data,BuildKit& selected,std::string& error){
    selected=base;selected.target=BuildTarget::Standard;selected.runtime={};
#ifndef _WIN32
    // The verified native kit is a Windows PAL target; upstream Linux keeps
    // its ordinary regional build path and executable naming.
    error.clear();return true;
#else
    std::ifstream boot(P(data)/"sys/boot.bin",std::ios::binary);char id[6]{};
    if(!boot){std::error_code ec;const bool installed=fs::exists(P(data)/"sys/main.dol",ec);if(installed||ec){error="installed boot.bin is missing or unreadable; reinstall the disc data";return false;}error.clear();return true;}
    if(!boot.read(id,6)){error="installed boot.bin is incomplete";return false;}if(std::string(id,6)!="G3BP9G"){error.clear();return true;}
    launcher::RuntimeKit native;if(!launcher::load_runtime_kit(project,data,native,error))return false;selected.target=BuildTarget::NativeMultiplayer;selected.runtime=std::move(native);return true;
#endif
}
bool game_build_ready(const BuildKit& kit,const std::string& output,std::string& error,bool force){
    if(kit.target==BuildTarget::NativeMultiplayer)return launcher::runtime_build_ready(kit.runtime,output,error,force);
    if(!exists(output)){error="game has not been built";return false;}if(!kit.version.empty()&&built_kit_version(kit)!=kit.version){error="game was built with another standard runtime kit";return false;}error.clear();return true;
}
bool GameBuilder::build_native(const BuildKit& kit,const std::string& data,const std::string& output,const Report& report,std::atomic<bool>& cancel,std::string& error){
    try{
        const auto& native=kit.runtime;native_need(S(fs::weakly_canonical(P(data)))==native.data_dir,"installed data changed after native target selection");native_need(P(output).lexically_normal()==P(kit.output_exe(native.project_dir)).lexically_normal()&&native_contained(P(native.project_dir),P(output)),"native output must stay within its selected Multiplayer target");
        native_need(fs::weakly_canonical(P(kit.compiler))==fs::weakly_canonical(P(native.compiler))&&fs::weakly_canonical(P(kit.toolchain_dir))==fs::weakly_canonical(P(native.project_dir)/"build-kit/toolchain"),"base compiler/toolchain path differs from the pinned native toolchain");
        const auto work=P(kit.dir)/"native-multiplayer-work";fs::create_directories(work/"logs");NativeBuildLock lock(work/"build.lock");log_path=S(work/"logs/build.log");log("Native multiplayer build "+native.fingerprint);
        native_need(!cancel,"cancelled");report("Verifying the native runtime",0);native_need(launcher::verify_runtime_kit(native,false,error),error);
        const auto destination=P(output).parent_path();fs::create_directories(destination);native_recover_publish(destination);
        report("Preparing the compiler",0);native_need(get_toolchain(kit,report,cancel,error),error);native_need(launcher::verify_runtime_kit(native,true,error),error);
        const auto generation=launcher::runtime_text_sha256("native-source-v1\n"+native.dol_sha256+"\n"+native.recompiler_sha256+"\n"+native.recipe_sha256+"\n"+native.symbols_sha256);
        // Full SHA256 remains in source/object receipts; short folder names keep Windows paths below MAX_PATH.
        const auto generated=work/"generated"/P(generation.substr(0,16));fs::create_directories(generated.parent_path());
        if(!native_source_cache(generated,generation,cancel)){
            native_need(!cancel,"cancelled");const auto base=work/"base-pending";if(fs::exists(base))native_remove_tree(work,base);
            report("Translating the game code",0);if(!run({native.recompiler,S(P(data)/"sys/main.dol"),S(base),"--map",native.symbols,"--no-comments"},native.project_dir,S(work/"logs/recomp.log"),cancel,error))throw std::runtime_error("translating the game code failed: "+error);
            native_need(!cancel,"cancelled");report("Applying the native multiplayer integration",0);
            const auto fresh=generated.parent_path()/P(generation.substr(0,16)+".pending"),previous=generated.parent_path()/P(generation.substr(0,16)+".previous");
            if(fs::exists(fresh))native_remove_tree(work,fresh);
            // The patcher accepts source-only trees. Cache metadata belongs to
            // our committed generation, so regenerate into a clean sibling.
            if(!launcher::apply_translation_recipe(base,fresh,P(native.recipe),cancel,error))throw std::runtime_error("native translation recipe failed: "+error);
            native_write_json(fresh/"source-cache.json",native_source_record(fresh,generation,cancel));native_need(!cancel,"cancelled");
            if(fs::exists(previous))native_remove_tree(work,previous);
            const bool had_previous=fs::exists(generated);if(had_previous)fs::rename(generated,previous);
            try{fs::rename(fresh,generated);}catch(...){if(had_previous)fs::rename(previous,generated);throw;}
            if(had_previous)native_remove_tree(work,previous);native_remove_tree(work,base);
        }else log("Reused verified native translation cache");
        auto all=native_generated_files(generated);std::vector<fs::path> sources;for(const auto& path:all)if(path.extension()==".cpp")sources.push_back(path);native_need(!sources.empty(),"native translation has no compile units");
        std::string common="native-objects-v1\n"+native.compiler_sha256+"\n"+generation;for(const auto& arg:native.compile_flags)common+='\n'+arg;
        for(const auto& f:native.files)if(f.path.starts_with("include/")||f.path.starts_with("include\\"))common+='\n'+f.path+":"+f.sha256;
        for(const auto& path:all)if(path.extension()==".h")common+='\n'+S(path.filename())+":"+native_hash(path);
        const auto cache_key=launcher::runtime_text_sha256(common);const auto cache=work/"objects"/P(cache_key.substr(0,16));fs::create_directories(cache);
        NativeJSON::Object index;try{auto old=native_read_json(cache/"compiled.json");if(old.object())index=*old.object();}catch(...){}
        std::vector<std::string> objects(sources.size()),keys(sources.size());std::vector<size_t> pending;
        for(size_t i=0;i<sources.size();++i){native_need(!cancel,"cancelled");objects[i]=S(cache/(sources[i].stem().string()+".obj"));keys[i]=launcher::runtime_text_sha256(common+"\n"+S(sources[i].filename())+"\n"+native_hash(sources[i]));
            auto found=index.find(S(sources[i].filename()));bool reuse=false;if(found!=index.end()&&fs::is_regular_file(P(objects[i]))){auto key=native_string(found->second,"key"),hash=native_string(found->second,"sha256");reuse=key&&hash&&*key==keys[i]&&*hash==native_hash(P(objects[i]));}if(!reuse)pending.push_back(i);}
        const auto jobs=std::max(1u,std::min(8u,std::thread::hardware_concurrency()?std::thread::hardware_concurrency():2u));
        struct Job {platform::Process* process;size_t index;fs::path pending;};std::vector<Job> running;size_t next=0,done=sources.size()-pending.size();bool failed=false;std::string failure;
        auto stop=[&](){for(auto& j:running){platform::kill(j.process);platform::release(j.process);std::error_code ec;fs::remove(j.pending,ec);}running.clear();};
        try{
            while(next<pending.size()||!running.empty()){
                if(cancel){failure="cancelled";failed=true;}if(failed){stop();break;}
                while(next<pending.size()&&running.size()<jobs){const auto i=pending[next++];auto obj=P(objects[i]);obj+=".pending";std::error_code ec;fs::remove(obj,ec);std::vector<std::string> args{native.compiler};args.insert(args.end(),native.compile_flags.begin(),native.compile_flags.end());args.insert(args.end(),{"-I"+S(P(native.kit_dir)/"include"),"-I"+S(generated),"-c",S(sources[i]),"-o",S(obj)});
                    auto process=platform::spawn(args,S(generated),S(work/"logs"/(sources[i].stem().string()+".log")),failure);if(!process){failed=true;break;}running.push_back({process,i,obj});}
                for(size_t j=0;j<running.size()&&!failed;){int code=0;if(!platform::poll(running[j].process,code)){++j;continue;}auto job=running[j];platform::release(job.process);running.erase(running.begin()+j);if(code){failed=true;failure="compiling "+S(sources[job.index].filename())+" failed; see "+S(work/"logs"/(sources[job.index].stem().string()+".log"));break;}
                    native_need(!cancel,"cancelled");native_replace(job.pending,P(objects[job.index]));index[S(sources[job.index].filename())]=NativeJSON::Object{{"key",keys[job.index]},{"sha256",native_hash(P(objects[job.index]))}};native_write_json(cache/"compiled.json",index);++done;}
                report("Compiling the native game ("+std::to_string(done)+" of "+std::to_string(sources.size())+")",double(done)/sources.size());if(!failed&&!running.empty())std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }catch(...){stop();throw;}
        native_need(!failed,failure);native_need(!cancel,"cancelled");
        report("Linking the native game",.5);std::vector<std::string> link;for(const auto& token:native.link_args){if(token=="{game_objects}")link.insert(link.end(),objects.begin(),objects.end());else if(token.starts_with("file:"))link.push_back(S(P(native.kit_dir)/P(token.substr(5))));else link.push_back(token);}
        std::string response;for(const auto& arg:link)response+=native_rsp(arg);native_write(work/"link.rsp",response);const auto candidate=work/"game.pending.exe";std::error_code ec;fs::remove(candidate,ec);
        if(!run({native.compiler,"@"+S(work/"link.rsp"),"-o",S(candidate)},S(work),S(work/"logs/link.log"),cancel,error))throw std::runtime_error("linking the native game failed: "+error);
        std::ifstream pe(candidate,std::ios::binary);char mz[2]{};native_need(bool(pe.read(mz,2))&&mz[0]=='M'&&mz[1]=='Z',"compiler did not produce a native executable");pe.close();native_need(!cancel,"cancelled");
        report("Verifying and publishing the native game",.9);native_need(launcher::verify_runtime_kit(native,true,error),error);native_publish(native,candidate,destination,cancel);log("Native game published: "+output);report("Done",1);error.clear();return true;
    }catch(const std::exception& e){error=e.what();if(!log_path.empty())log("error: "+error);return false;}
}
