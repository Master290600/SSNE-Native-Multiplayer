#include "updater.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#include "gc/zip.h"
#include "platform.h"
#include "launcher_json.h"
#include "runtime_kit.h"

namespace fs = std::filesystem;

namespace updater {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

#ifndef SSNE_UPDATE_REPOSITORY
#define SSNE_UPDATE_REPOSITORY "Master290600/SSNE-Native-Multiplayer"
#endif
const char* kLatestUrl = "https://api.github.com/repos/" SSNE_UPDATE_REPOSITORY "/releases/latest";
#ifdef _WIN32
const char* kAssetSuffix = "-Windows-x64.zip";
const char* kLauncherName = "SSNE-Native.exe";
#else
const char* kAssetSuffix = "-Linux-x86_64.tar.gz";
const char* kLauncherName = "SSNE-Native";
#endif
const char* kLeftovers = "update-leftovers.txt";

// The string value of the next "key": "..." at or after `from` (GitHub's JSON: no escapes in the values we read).
std::string json_string(const std::string& js, const std::string& key, size_t from, size_t* at = nullptr) {
    size_t k = js.find("\"" + key + "\"", from);
    if (k == std::string::npos) return {};
    size_t q = js.find('"', js.find(':', k) + 1);
    if (q == std::string::npos) return {};
    size_t e = js.find('"', q + 1);
    if (e == std::string::npos) return {};
    if (at) *at = e;
    return js.substr(q + 1, e - q - 1);
}

std::vector<int> parse_version(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v[0] == 'v' ? v.substr(1) : v);
    std::string part;
    while (std::getline(ss, part, '.')) out.push_back(atoi(part.c_str()));
    return out;
}
bool newer(const std::string& a, const std::string& b) {  // a > b
    std::vector<int> x = parse_version(a), y = parse_version(b);
    x.resize(std::max(x.size(), y.size())); y.resize(x.size());
    return x > y;
}

// Moves `from` to `to`, replacing it. A file in use (the running launcher on Windows) can't be replaced but can be
// renamed: it's moved aside first, and deleted now or, failing that, by cleanup() after the restart.
bool replace_file(const fs::path& from, const fs::path& to, std::vector<std::string>& leftovers, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (fs::exists(to, ec)) {
        fs::path old = to;
        old += ".old";
        fs::remove(old, ec);
        fs::rename(to, old, ec);
        if (ec) { error = "cannot replace " + S(to) + ": " + ec.message(); return false; }
        fs::remove(old, ec);
        if (ec) leftovers.push_back(S(old));
    }
    fs::rename(from, to, ec);
    if (ec) { error = "cannot move " + S(to) + " into place: " + ec.message(); return false; }
    return true;
}

} // namespace

const char* current_version() { return SSNE_VERSION; }
const char* release_feed_url() { return kLatestUrl; }

bool check(Release& r, std::string& error) {
    const char* url = getenv("SSNE_UPDATE_URL");  // testing: another release feed
    fs::path tmp = fs::temp_directory_path() / "ssne-native-latest.json";
    if (!platform::download(url ? url : kLatestUrl, S(tmp), nullptr, error)) return false;
    std::ifstream in(tmp, std::ios::binary);
    std::string js((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::remove(S(tmp) + ".log", ec);
    std::string tag = json_string(js, "tag_name", 0);
    if (tag.empty()) { error = "no release information"; return false; }
    r.version = tag[0] == 'v' ? tag.substr(1) : tag;
    if (!newer(r.version, current_version())) return false;
    for (size_t at = 0;;) {
        std::string u = json_string(js, "browser_download_url", at, &at);
        if (u.empty()) break;
        if (u.size() > strlen(kAssetSuffix) && u.compare(u.size() - strlen(kAssetSuffix), std::string::npos, kAssetSuffix) == 0) {
            r.asset_url = u;
            r.asset_name = u.substr(u.rfind('/') + 1);
            return true;
        }
    }
    return false;  // no package for this OS
}

bool validate_package(const std::string& base_dir, const std::string& incoming_dir, std::string& error) {
    try {
        auto need = [](bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); };
        const auto base=P(base_dir), incoming=P(incoming_dir);
        need(fs::is_regular_file(incoming/kLauncherName),"the package has no "+std::string(kLauncherName));
        for (const auto& entry : fs::recursive_directory_iterator(incoming)) {
            const auto relative=entry.path().lexically_relative(incoming);
            std::string path=S(relative); std::transform(path.begin(),path.end(),path.begin(),[](unsigned char c){return char(std::tolower(c));});
            std::replace(path.begin(),path.end(),'\\','/');
            need(!fs::is_symlink(entry.symlink_status()),"update package contains a symbolic link");
            need(path!="update-leftovers.txt" && path!="update" && !path.starts_with("update/") &&
                path!="launcher.ini" && path!="settings.ini" && path!="game_data" && !path.starts_with("game_data/") &&
                path!="saves" && !path.starts_with("saves/") && path!="states" && !path.starts_with("states/") &&
                !path.starts_with("multiplayer/saves") && !path.starts_with("multiplayer/states") &&
                path!="multiplayer/settings.ini" && path!="multiplayer/settings-netplay.ini" && path!="multiplayer/multiplayer-launcher.ini" &&
                !path.starts_with("build-kit/work") && !path.starts_with("build-kit/native-multiplayer-work") &&
                path!="ssne-native-game.exe" && path!="ssne-native-game" &&
                path!="multiplayer/ssne-native-multiplayer.exe" && path!="multiplayer/ssne-native-multiplayer-game.exe" &&
                path!="multiplayer/ssne-native-multiplayer" && path!="multiplayer/ssne-native-multiplayer-game",
                "the update package contains local game data, settings or build outputs: "+S(relative));
        }
        if (fs::exists(base/"build-kit/multiplayer/launcher-kit.json")) {
            const auto marker=incoming/"launcher-integration.json";
            need(fs::is_regular_file(marker),"The downloaded update does not include this launcher's native multiplayer build support. No installed files were replaced.");
            need(fs::file_size(marker)<=65536,"launcher integration metadata is too large");
            std::ifstream file(marker,std::ios::binary); std::string text((std::istreambuf_iterator<char>(file)),{});
            launcher::JSONValue value; std::string why;
            need(launcher::parse_json(text,value,why),"invalid launcher integration metadata: "+why);
            auto str=[&](const char* key){auto item=value.find(key);need(item&&item->string(),std::string("missing launcher integration field: ")+key);return *item->string();};
            int64_t schema=0; auto field=value.find("schema");
            need(field&&field->integer(schema)&&schema==1&&str("target")=="native-multiplayer","unsupported launcher integration package");
            need(launcher::runtime_file_sha256(incoming/kLauncherName,why)==str("launcher_sha256"),"launcher does not match integration metadata");
            need(launcher::runtime_file_sha256(incoming/"build-kit/multiplayer/launcher-kit.json",why)==str("launcher_kit_sha256"),"native launcher kit does not match integration metadata");
            launcher::RuntimeKit kit;
            if(!launcher::load_runtime_kit(incoming_dir,S(base/"game_data"),kit,why,false))
                throw std::runtime_error("incomplete native multiplayer update: "+why);
        }
        error.clear(); return true;
    } catch(const std::exception& exception) { error=exception.what(); return false; }
}

bool install(const Release& r, const std::string& base_dir,
             const std::function<bool(const std::string&, double)>& progress, std::string& launcher, std::string& error) {
    std::error_code ec;
    fs::path base = P(base_dir), dir = base / "update", files = dir / "files";
    fs::remove_all(dir, ec);
    fs::create_directories(files, ec);
    if (ec) { error = "cannot create " + S(dir) + ": " + ec.message(); return false; }
    fs::path pkg = dir / P(r.asset_name), part = pkg;
    part += ".part";
    std::string label = "Downloading SSNE-Native " + r.version;
    if (!platform::download(r.asset_url, S(part), [&](uint64_t d, uint64_t t) {
            char buf[64];
            snprintf(buf, sizeof(buf), "  %.1f MB", d / 1048576.0);
            return progress(label + buf, t ? double(d) / double(t) : 0.0);
        }, error)) {
        error = "download failed: " + error;
        return false;
    }
    fs::rename(part, pkg, ec);
    if (!progress("Unpacking", 0)) { error = "cancelled"; return false; }
#ifdef _WIN32
    if (!gc::zip::extract(S(pkg), S(files), [](const std::string& n) { return n; },
                          [&](uint64_t d, uint64_t t) { return progress("Unpacking", t ? double(d) / double(t) : 0.0); }, error))
        return false;
#else
    {
        std::string tar = fs::exists("/usr/bin/tar") ? "/usr/bin/tar" : "/bin/tar";
        platform::Process* p = platform::spawn({tar, "-xzf", S(pkg), "-C", S(files)}, S(dir), S(dir / "tar.log"), error);
        if (!p) return false;
        int code = 1;
        while (!platform::poll(p, code)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        platform::release(p);
        if (code != 0) { error = "unpacking failed (tar exit " + std::to_string(code) + ")"; return false; }
    }
#endif
    // The package has one top folder (SSNE-Native-<version>-<os>/).
    fs::path root = files;
    {
        int n = 0;
        fs::path only;
        for (auto& e : fs::directory_iterator(files, ec)) { n++; only = e.path(); }
        if (n == 1 && fs::is_directory(only, ec)) root = only;
    }
    if (!fs::exists(root / kLauncherName, ec)) { error = "the package has no " + std::string(kLauncherName); return false; }
    if (!validate_package(base_dir,S(root),error)) return false;
    if (!progress("Installing", 0)) { error="cancelled"; return false; }
    std::vector<std::string> leftovers;
    std::vector<fs::path> moves;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) moves.push_back(it->path());
    for (const fs::path& f : moves)
        if (!replace_file(f, base / f.lexically_relative(root), leftovers, error)) return false;
    if (!leftovers.empty()) {
        std::ofstream out(base / kLeftovers, std::ios::app);
        for (auto& l : leftovers) out << l << "\n";
    }
    fs::remove_all(dir, ec);
    launcher = S(base / kLauncherName);
    return true;
}

void cleanup(const std::string& base_dir) {
    std::error_code ec;
    fs::path base = P(base_dir), list = base / kLeftovers;
    fs::remove_all(base / "update", ec);
    std::ifstream in(list);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto target=fs::weakly_canonical(P(line),ec);
        if(ec || target.extension()!=".old") { ec.clear(); continue; }
        const auto root=fs::weakly_canonical(base,ec); if(ec) { ec.clear(); continue; }
        auto a=root.begin(),b=target.begin();
        for(;a!=root.end()&&b!=target.end();++a,++b) {
            auto x=S(*a),y=S(*b);
#ifdef _WIN32
            std::transform(x.begin(),x.end(),x.begin(),[](unsigned char c){return char(std::tolower(c));});
            std::transform(y.begin(),y.end(),y.begin(),[](unsigned char c){return char(std::tolower(c));});
#endif
            if(x!=y)break;
        }
        if(a==root.end()&&b!=target.end()) fs::remove(target,ec);
        ec.clear();
    }
    in.close();
    fs::remove(list, ec);
}

} // namespace updater
