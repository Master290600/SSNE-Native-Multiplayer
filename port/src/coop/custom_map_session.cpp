#include "custom_map_session.h"
#include "custom_levels_runtime.h"
#include "custom_level_menu.h"
#include "native_frontend.h"
#include "../crosshair_image.h"
#include "ssne/custom_level_mount.h"
#include <atomic>
#include <fstream>
#include <future>
#include <thread>

namespace port::coop {
namespace {
std::filesystem::path download_root;
template<class F> auto background(F job) {
    using T = std::invoke_result_t<F>;
    std::packaged_task<T()> task(std::move(job));
    auto result = task.get_future();
    std::thread(std::move(task)).detach();
    return result;
}
template<class T> bool done(std::future<T>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
std::vector<uint8_t> read_bounded(const std::filesystem::path& path, uint64_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() <= 0 || uint64_t(in.tellg()) > limit)
        throw std::runtime_error("Custom map file is missing, empty or too large");
    std::vector<uint8_t> bytes(size_t(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot read custom map file");
    return bytes;
}
std::string digest(std::span<const uint8_t> bytes) {
    gc::Sha256 hash; hash.update(bytes.data(), bytes.size());
    return gc::Sha256::hex(hash.finish());
}
CustomMapManifest manifest_for(const gc::custom::Level& level) {
    CustomMapManifest m;
    m.id = level.id; m.alias = level.alias; m.title = level.title;
    m.base_level = level.base_level; m.map_sha256 = level.map_sha256; m.modes = level.modes;
    return m;
}
std::shared_ptr<const CustomMapPackage> prepare_package(const gc::custom::Level& level) {
    auto p = std::make_shared<CustomMapPackage>();
    p->manifest = manifest_for(level);
    p->map = std::make_shared<const std::vector<uint8_t>>(read_bounded(level.map_path, custom_map_max_bytes));
    p->preview = std::make_shared<const std::vector<uint8_t>>();
    if (!level.preview_path.empty()) {
        auto bytes = read_bounded(level.preview_path, custom_preview_max_bytes);
        port::CrosshairImage image; std::string error;
        if (!port::decode_crosshair_png(bytes, image, error)) throw std::runtime_error(error);
        p->manifest.preview_sha256 = digest(bytes);
        p->preview = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    }
    p->manifest.map_bytes = p->map->size(); p->manifest.preview_bytes = p->preview->size();
    p->manifest.package_sha256 = custom_map_package_fingerprint(p->manifest);
    std::string error;
    if (!custom_map_package_valid(*p, error)) throw std::runtime_error(error);
    return p;
}
gc::custom::Level level_for(const CustomMapManifest& m) {
    gc::custom::Level l;
    l.id=m.id; l.alias=m.alias; l.title=m.title; l.base_level=m.base_level;
    l.map_sha256=m.map_sha256; l.modes=m.modes;
    return l;
}
std::string text(const Properties& p, const char* key) {
    auto it=p.find(key);
    if(it!=p.end()) if(auto v=std::get_if<std::string>(&it->second)) return *v;
    return {};
}
struct Candidate { gc::custom::Level level; std::string fingerprint; };
struct Installed { gc::custom::Level level; std::shared_ptr<const ssne::CustomLevelMountPlan> mount; };
}
void configure_custom_map_downloads(std::filesystem::path root) { download_root=std::move(root); }
struct CustomMapSession::Impl {
    AuthoritativeServer* server;
    AuthoritativeClient* client;
    Log log;
    uint64_t epoch=0;
    std::string alias, failed;
    bool requested=false, activated=false;
    CustomMapRequest request;
    Candidate candidate;
    std::future<std::shared_ptr<const CustomMapPackage>> offer;
    std::future<Candidate> lookup;
    std::future<Installed> install;
    std::chrono::steady_clock::time_point started;
    void report(const std::string& message) { if(log)log("[custom-map-transfer] "+message); }
    void reset() {
        if(client)client->cancel_custom_map();
        // Jobs own value copies and touch only private cache folders. Destroying
        // a packaged-task future never waits on disk I/O or retains this session.
        offer={}; lookup={}; install={}; candidate={}; request={};
        epoch=0;alias.clear();failed.clear();requested=activated=false;
        if(client)native_frontend_custom_map_progress({});
    }
    bool failure(const std::string& error) {
        if(failed.empty()) { failed=error; report(error); native_frontend_custom_level_error(error); }
        return false;
    }
    void progress(NativeCustomMapPhase phase, const CustomMapTransferStatus* s=nullptr) {
        NativeCustomMapProgress p; p.phase=phase; p.alias=alias;
        if(s) {p.title=s->manifest.title;p.received=s->received;p.total=s->total;}
        else p.title=candidate.level.title;
        native_frontend_custom_map_progress(std::move(p));
    }
};
CustomMapSession::CustomMapSession(AuthoritativeServer* s,AuthoritativeClient* c,Log log)
    :impl_(std::make_unique<Impl>()) {
    impl_->server=s;impl_->client=c;impl_->log=std::move(log);
    if(c)native_frontend_custom_map_progress({});
}
CustomMapSession::~CustomMapSession() { impl_->reset(); }
void CustomMapSession::host_update(const WorldState& world) {
    auto& s=*impl_;
    const auto selected=selected_custom_map_alias(world);
    if(!s.server)return;
    if(world.epoch!=s.epoch || selected!=s.alias) {
        s.reset();s.epoch=world.epoch;s.alias=selected;s.server->clear_custom_map_package();
        if(!is_custom_level_alias(selected))return;
        const auto* level=find_custom_level(selected);
        if(!level){s.failed="Selected custom map is unavailable";s.server->fail_custom_map_package(s.epoch,s.alias,s.failed);return;}
        const auto copy=*level;
        s.offer=background([copy]{return prepare_package(copy);});
        s.report("preparing host package "+copy.id);
    }
    if(done(s.offer)) {
        try {
            auto p=s.offer.get();std::string error;
            if(!s.server->set_custom_map_package(s.epoch,p,error))throw std::runtime_error(error);
            s.report("host package ready: "+p->manifest.title);
        } catch(const std::exception& e) {s.failed=e.what();s.server->fail_custom_map_package(s.epoch,s.alias,s.failed);s.report(s.failed);}
    }
}
bool CustomMapSession::client_ready(PPCContext& ctx,const WorldState& world) {
    auto& s=*impl_;
    const auto selected=selected_custom_map_alias(world);
    if(!s.client)return true;
    if(native_frontend_take_custom_map_cancel() || !s.client->status().connected){s.reset();return false;}
    if(!is_custom_level_alias(selected)) {if(s.epoch)s.reset();return true;}
    if(world.epoch!=s.epoch || selected!=s.alias) {
        s.reset();s.epoch=world.epoch;s.alias=selected;s.started=std::chrono::steady_clock::now();
        s.request={world.epoch,selected,text(world.mission,"Session.Custom.Id"),text(world.mission,"Session.Custom.SHA256"),{}};
        std::string error;
        if(!custom_map_request_valid(s.request,error))return s.failure(error);
        s.progress(NativeCustomMapPhase::Requesting);
        const auto request=s.request;const auto root=download_root;
        const auto* local=find_custom_level(selected);
        const auto copy=local?std::optional<gc::custom::Level>(*local):std::nullopt;
        s.lookup=background([root,request,copy] {
            if(auto hit=gc::custom::find_downloaded_package(root,request.alias,request.id,request.map_sha256))
                return Candidate{hit->level,hit->package_sha256};
            if(copy)try {return Candidate{*copy,prepare_package(*copy)->manifest.package_sha256};}catch(...){}
            return Candidate{};
        });
        s.report("checking "+selected);
    }
    if(!s.failed.empty())return false;
    if(s.activated)return native_frontend_custom_map_progress().phase==NativeCustomMapPhase::None;
    if(std::chrono::steady_clock::now()-s.started>std::chrono::minutes(5))return s.failure("Custom map transfer timed out");
    if(done(s.lookup)) {
        try {s.candidate=s.lookup.get();}catch(const std::exception& e){return s.failure(e.what());}
        s.request.known_package_sha256=s.candidate.fingerprint;
        std::string error;
        if(!s.client->request_custom_map(s.request,error))return s.failure(error);
        s.requested=true;
    }
    if(!s.requested)return false;
    const auto status=s.client->custom_map_transfer_status();
    if(status.phase==CustomMapTransferPhase::Failed)return s.failure(status.error);
    if(status.phase==CustomMapTransferPhase::Cancelled)return s.failure("Custom map transfer was cancelled");
    if(!s.install.valid()) {
        const auto phase=status.phase==CustomMapTransferPhase::Receiving?NativeCustomMapPhase::Downloading:
            status.phase==CustomMapTransferPhase::Verifying?NativeCustomMapPhase::Verifying:NativeCustomMapPhase::Requesting;
        s.progress(phase,&status);
        if(auto download=s.client->take_custom_map_package()) {
            auto package=download->package;const auto root=download_root;const auto candidate=s.candidate;
            const bool reused=download->cache_hit;
            s.progress(NativeCustomMapPhase::Installing,&status);
            s.install=background([package,root,candidate,reused] {
                gc::custom::Level level;
                if(reused) {
                    if(candidate.fingerprint!=package->manifest.package_sha256 || candidate.level.map_path.empty())
                        throw std::runtime_error("Cached custom map identity changed");
                    // Recheck bytes before mapping: the cache can be edited while connected.
                    const auto verified=prepare_package(candidate.level);
                    if(verified->manifest!=package->manifest)
                        throw std::runtime_error("Cached custom map was modified during transfer");
                    level=gc::custom::write_downloaded_package(root,level_for(package->manifest),*verified->map,*verified->preview,package->manifest.package_sha256,package->manifest.preview_sha256);
                } else {
                    std::string error;
                    if(!custom_map_package_valid(*package,error))throw std::runtime_error(error);
                    if(!package->preview->empty()) {port::CrosshairImage image;if(!port::decode_crosshair_png(*package->preview,image,error))throw std::runtime_error(error);}
                    level=gc::custom::write_downloaded_package(root,level_for(package->manifest),*package->map,*package->preview,package->manifest.package_sha256,package->manifest.preview_sha256);
                }
                std::string error;
                auto mount=ssne::prepare_custom_level_mount(level,error);
                if(!mount)throw std::runtime_error(error);
                return Installed{level,std::move(mount)};
            });
            s.report(reused?"verified cached package":"download verified; installing package");
        }
    }
    if(done(s.install)) {
        try {
            auto installed=s.install.get();std::string error;
            if(!ssne::activate_custom_level_mount(ctx,installed.mount,error))return s.failure(error);
            register_downloaded_custom_level(installed.level);
            custom_level_menu_catalog_changed(ctx);
            NativeCustomMapProgress p;p.phase=NativeCustomMapPhase::Ready;p.title=installed.level.title;p.alias=s.alias;p.preview_path=installed.level.preview_path.string();
            native_frontend_custom_map_progress(std::move(p));s.activated=true;
            s.report("ready: "+installed.level.title);
        } catch(const std::exception& e) {return s.failure(e.what());}
    }
    return false;
}
}
