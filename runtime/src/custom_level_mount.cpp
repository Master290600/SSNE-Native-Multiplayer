#include "ssne/custom_level_mount.h"
#include "ssne/hw.h"
#include "ssne/ppc_context.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include "gc/sha256.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ssne { namespace {
constexpr uint32_t convert_path=0x801bad7c, heap_allocate=0x80116084, heap_free=0x801161ec;
struct Mounted { uint32_t index; std::string sha; };
std::map<std::string,Mounted> mounted;
PPCFunc original_convert=nullptr;
std::vector<uint32_t> retained_fsts;
size_t mounted_bytes=0;
bool readable(uint32_t p,size_t n) {
    return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;
}
std::string guest_text(uint32_t p) {
    std::string value;
    for(unsigned n=0;n<256 && readable(p+n,1);++n) {
        const auto c=mem_read8(p+n); if(!c)return value; value+=char(c);
    }
    return {};
}
void path_hook(PPCContext& c) {
    auto path=guest_text(c.r[3]);
    if(!path.empty() && path.front()=='/')path.erase(0,1);
    for(auto& ch:path)if(ch>='A' && ch<='Z')ch=char(ch-'A'+'a');
    // Map loading uses a full Levels path. Do not redirect arbitrary suffixes,
    // other folders, cwd changes, traversal or any original DVD request.
    if(path.starts_with("levels/custom_") && path.ends_with(".ssw")) {
        auto alias=path.substr(7,path.size()-11);
        alias.replace(0,7,"Custom_");
        if(const auto found=mounted.find(alias);found!=mounted.end()) {
            c.r[3]=found->second.index; return;
        }
    }
    ssne_call_original(c,convert_path,original_convert);
}
uint32_t call(PPCContext& base,uint32_t addr,uint32_t a,uint32_t b) {
    auto c=base; c.r[1]-=0x400; c.msr&=~0x8000u;
    mem_write32(c.r[1],base.r[1]); c.r[3]=a; c.r[4]=b;
    ssne_call(c,addr);return c.r[3];
}
} // namespace
void reset_custom_level_mounts() {
    if(*ssne_hook_slot(convert_path)==path_hook)
        ssne_install_hook(convert_path,nullptr);
    mounted.clear();retained_fsts.clear();mounted_bytes=0;
    original_convert=nullptr;
}
std::shared_ptr<const CustomLevelMountPlan>
prepare_custom_level_mount(const gc::custom::Level& level,std::string& error) {
    try {
        if(level.alias!=gc::custom::map_alias(level.id,level.map_sha256))
            throw std::runtime_error("Downloaded map alias differs from its SHA");
        const auto path=std::filesystem::absolute(level.map_path);
        if(path.filename()!="map.ssw")
            throw std::runtime_error("Runtime downloads must use the fixed cache map.ssw");
        // Check every component: a trusted cache directory cannot redirect a
        // mount through a local reparse point after the package was validated.
        std::filesystem::path checked;
        for(const auto& part:path) {
            checked/=part;
            if(std::filesystem::is_symlink(checked))
                throw std::runtime_error("Downloaded map path contains a symlink");
#ifdef _WIN32
            const auto attrs=GetFileAttributesW(checked.c_str());
            if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Downloaded map path contains a reparse point");
#endif
        }
        std::ifstream in(path,std::ios::binary|std::ios::ate);
        const auto length=in.tellg();
        if(!in || length<64 || uint64_t(length)>128u*1024*1024)
            throw std::runtime_error("Downloaded map size is invalid");
        auto bytes=std::make_shared<std::vector<uint8_t>>(size_t(length));
        in.seekg(0);in.read(reinterpret_cast<char*>(bytes->data()),bytes->size());
        if(!in || std::memcmp(bytes->data(),"CLIMAX-\0",8))
            throw std::runtime_error("Downloaded map is incomplete");
        gc::Sha256 hash;hash.update(bytes->data(),bytes->size());
        if(gc::Sha256::hex(hash.finish())!=level.map_sha256)
            throw std::runtime_error("Downloaded map SHA differs before activation");
        auto plan=std::make_shared<CustomLevelMountPlan>();
        plan->alias=level.alias;plan->map_sha256=level.map_sha256;plan->bytes=std::move(bytes);
        return plan;
    } catch(const std::exception& e) { error=e.what();return {}; }
}
bool activate_custom_level_mount(PPCContext& c,
                                const std::shared_ptr<const CustomLevelMountPlan>& plan,
                                std::string& error) {
    auto fail=[&](const char* why){error=why;return false;};
    if(!plan || !plan->bytes || plan->bytes->empty())return fail("Downloaded map mount plan is missing");
    if(const auto old=mounted.find(plan->alias);old!=mounted.end()) {
        if(old->second.sha!=plan->map_sha256)return fail("Downloaded map alias collision");
        return true;
    }
    if(mounted.size()>=256 || plan->bytes->size()>512u*1024*1024-mounted_bytes)
        return fail("Downloaded map runtime cache is full; restart the game");
    if(!readable(c.r[13]-0x6de0,4) || !readable(c.r[13]-0x5a14,4))
        return fail("Game DVD filesystem is not initialized");
    const auto fst=mem_read32(c.r[13]-0x5a14), count=mem_read32(c.r[13]-0x5a0c);
    const auto bytes=mem_read32(0x8000003c);
    if(fst!=mem_read32(0x80000038) || !readable(fst,bytes) || bytes<12 ||
        bytes>2*1024*1024 || count!=mem_read32(fst+8) ||
        mem_read32(c.r[13]-0x5a10)!=fst+count*12)
        return fail("Unsupported running PAL DVD filesystem state");
    if(!original_convert) {
        const auto original=ssne_find_func(convert_path);
        if(!original || !original->func)return fail("PAL DVD path function is unavailable");
        original_convert=original->func;
        if(*ssne_hook_slot(convert_path) && *ssne_hook_slot(convert_path)!=path_hook)
            return fail("PAL DVD path function already has an incompatible hook");
    }
    const auto offset=disc_append_offset();
    if(offset>UINT32_MAX || plan->bytes->size()>UINT32_MAX-offset)
        return fail("Downloaded virtual disc exceeds its supported size");
    const std::vector<uint8_t> old(host_ptr(fst),host_ptr(fst)+bytes);
    std::vector<uint8_t> next;uint32_t index=0;
    if(!append_custom_level_fst(old,plan->alias,uint32_t(offset),uint32_t(plan->bytes->size()),next,index,error))return false;
    const auto heap=mem_read32(c.r[13]-0x6de0);
    const auto memory=call(c,heap_allocate,heap,uint32_t((next.size()+31)&~size_t(31)));
    if(!memory || !readable(memory,next.size()))return fail("Not enough game memory to activate the downloaded map");
    // Preallocate host containers before committing any guest-visible state.
    try {
        retained_fsts.reserve(retained_fsts.size()+1);
        mounted.emplace(plan->alias,Mounted{index,plan->map_sha256});
        if(!disc_append_memory(offset,plan->bytes,error)) {
            mounted.erase(plan->alias);call(c,heap_free,heap,memory);return false;
        }
    } catch(...) {mounted.erase(plan->alias);call(c,heap_free,heap,memory);throw;}
    std::memcpy(host_ptr(memory),next.data(),next.size());
    // Guest execution holds the scheduler baton. Existing DVDFileInfo objects
    // retain indices/offsets, old string pointers retain their allocated FST.
    mem_write32(0x80000038,memory);mem_write32(0x8000003c,uint32_t(next.size()));
    mem_write32(c.r[13]-0x5a14,memory);
    mem_write32(c.r[13]-0x5a0c,count+1);
    mem_write32(c.r[13]-0x5a10,memory+(count+1)*12);
    ssne_install_hook(convert_path,path_hook);
    retained_fsts.push_back(memory);mounted_bytes+=plan->bytes->size();
    log_message("[custom-map-transfer] mounted "+plan->alias+" entry="+std::to_string(index)+" bytes="+std::to_string(plan->bytes->size()));
    return true;
}
} // namespace ssne
