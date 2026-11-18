#include "custom_map_transfer.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace port::coop {
namespace {
bool slug(const std::string& s) {
    return !s.empty() && s.size() <= 48 && std::all_of(s.begin(),s.end(),[](unsigned char c){
        return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='-';
    });
}
bool hash_string(const std::string& s) {
    return s.size()==64 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9') || (c>='a'&&c<='f');});
}
std::string digest(std::span<const uint8_t> bytes) {
    gc::Sha256 h;h.update(bytes.data(),bytes.size());return gc::Sha256::hex(h.finish());
}
std::string alias(const std::string& id,const std::string& sha) {
    const auto s=id+'\n'+sha;
    return "Custom_"+digest({reinterpret_cast<const uint8_t*>(s.data()),s.size()}).substr(0,24);
}
bool title_valid(const std::string& s) {
    if(s.empty() || s.size()>128)return false;
    for(size_t i=0;i<s.size();) {
        const auto first=uint8_t(s[i++]);
        if(first<128){if(first<32 || first==127)return false;continue;}
        unsigned n=0;uint32_t c=0,min=0;
        if(first>=0xc2&&first<=0xdf){n=1;c=first&31;min=0x80;}
        else if(first>=0xe0&&first<=0xef){n=2;c=first&15;min=0x800;}
        else if(first>=0xf0&&first<=0xf4){n=3;c=first&7;min=0x10000;}else return false;
        if(n>s.size()-i)return false;
        for(unsigned k=0;k<n;++k){const auto b=uint8_t(s[i++]);if((b&0xc0)!=0x80)return false;c=(c<<6)|(b&63);}
        if(c<min || c>0x10ffff || (c>=0xd800&&c<=0xdfff))return false;
    }
    return true;
}
bool reject(std::string& error,const char* text){error=text;return false;}
void put32(std::vector<uint8_t>& out,uint32_t n){for(int i=3;i>=0;--i)out.push_back(uint8_t(n>>(8*i)));}
void put64(std::vector<uint8_t>& out,uint64_t n){put32(out,uint32_t(n>>32));put32(out,uint32_t(n));}
void text(std::vector<uint8_t>& out,const std::string& s){put32(out,uint32_t(s.size()));out.insert(out.end(),s.begin(),s.end());}
struct Reader {
    std::span<const uint8_t> b;size_t p=0;bool ok=true;
    uint32_t u32(){if(p>b.size() || b.size()-p<4){ok=false;return 0;}uint32_t n=0;for(unsigned k=0;k<4;++k)n=(n<<8)|b[p++];return n;}
    uint64_t u64(){auto a=u32(),v=u32();return uint64_t(a)<<32|v;}
    std::string str(unsigned limit){auto n=u32();if(!ok || n>limit || n>b.size()-p){ok=false;return {};}std::string s(reinterpret_cast<const char*>(b.data()+p),n);p+=n;return s;}
};
} // namespace
bool custom_map_request_valid(const CustomMapRequest& r,std::string& error) {
    error.clear();
    if(!r.epoch || !slug(r.id) || !hash_string(r.map_sha256) || r.alias!=alias(r.id,r.map_sha256) || (!r.known_package_sha256.empty() && !hash_string(r.known_package_sha256)))
        return reject(error,"Invalid custom map request identity.");
    return true;
}
bool custom_map_manifest_valid(const CustomMapManifest& m,std::string& error) {
    error.clear();
    if(!slug(m.id) || !slug(m.base_level) || !title_valid(m.title) || !hash_string(m.map_sha256) || m.alias!=alias(m.id,m.map_sha256))
        return reject(error,"Invalid custom map metadata.");
    if(!m.modes || m.modes>63 || !m.map_bytes || m.map_bytes>custom_map_max_bytes || m.preview_bytes>custom_preview_max_bytes)
        return reject(error,"Custom map package exceeds its size or mode limits.");
    if((m.preview_bytes && !hash_string(m.preview_sha256)) || (!m.preview_bytes && !m.preview_sha256.empty()))
        return reject(error,"Invalid custom preview identity.");
    if(!hash_string(m.package_sha256) || m.package_sha256!=custom_map_package_fingerprint(m))
        return reject(error,"Invalid custom map package fingerprint.");
    return true;
}
bool custom_map_package_valid(const CustomMapPackage& p,std::string& error,bool verify_hashes) {
    if(!custom_map_manifest_valid(p.manifest,error))return false;
    if(!p.map || p.map->size()!=p.manifest.map_bytes || (p.preview?p.preview->size():0)!=p.manifest.preview_bytes)
        return reject(error,"Custom map payload length differs from its metadata.");
    if(verify_hashes && (digest(*p.map)!=p.manifest.map_sha256 ||
       (p.manifest.preview_bytes && digest(*p.preview)!=p.manifest.preview_sha256)))
        return reject(error,"Custom map or preview checksum differs.");
    return true;
}
bool custom_map_request_matches(const CustomMapRequest& r,const CustomMapManifest& m) {
    return r.alias==m.alias && r.id==m.id && r.map_sha256==m.map_sha256;
}
std::vector<uint8_t> encode_custom_map_manifest(const CustomMapManifest& m) {
    std::string error;if(!custom_map_manifest_valid(m,error))throw std::invalid_argument(error);
    std::vector<uint8_t> out;put32(out,1);text(out,m.id);text(out,m.alias);text(out,m.title);text(out,m.base_level);
    text(out,m.map_sha256);text(out,m.preview_sha256);put32(out,m.modes);put64(out,m.map_bytes);put64(out,m.preview_bytes);text(out,m.package_sha256);return out;
}
std::string custom_map_package_fingerprint(const CustomMapManifest& m) {
    std::vector<uint8_t> out;put32(out,1);text(out,m.id);text(out,m.alias);text(out,m.title);text(out,m.base_level);
    text(out,m.map_sha256);text(out,m.preview_sha256);put32(out,m.modes);put64(out,m.map_bytes);put64(out,m.preview_bytes);return digest(out);
}
bool decode_custom_map_manifest(std::span<const uint8_t> bytes,CustomMapManifest& output,std::string& error) {
    if(bytes.size()>1024)return reject(error,"Custom map offer is too large.");
    Reader r{bytes};if(r.u32()!=1)return reject(error,"Unknown custom map package schema.");
    CustomMapManifest m;m.id=r.str(48);m.alias=r.str(31);m.title=r.str(128);m.base_level=r.str(48);
    m.map_sha256=r.str(64);m.preview_sha256=r.str(64);m.modes=r.u32();m.map_bytes=r.u64();m.preview_bytes=r.u64();m.package_sha256=r.str(64);
    if(!r.ok || r.p!=bytes.size())return reject(error,"Incomplete custom map offer.");
    if(!custom_map_manifest_valid(m,error))return false;output=std::move(m);return true;
}
void CustomMapReceiver::release(){map_.reset();preview_.reset();complete_.reset();}
bool CustomMapReceiver::request(uint64_t token,const CustomMapRequest& r,std::string& error) {
    if(!token)return reject(error,"Invalid custom map request token.");
    if(!custom_map_request_valid(r,error))return false;
    release();token_=token;status_={};status_.phase=CustomMapTransferPhase::Requesting;status_.request=r;
    map_digest_=gc::Sha256{};preview_digest_=gc::Sha256{};return true;
}
bool CustomMapReceiver::offer(uint64_t token,uint64_t epoch,const CustomMapManifest& m,std::string& error) {
    if(token!=token_ || epoch!=status_.request.epoch || status_.phase==CustomMapTransferPhase::Cancelled || status_.phase==CustomMapTransferPhase::Failed || status_.phase==CustomMapTransferPhase::Complete)return true; // Retired transfer.
    if(status_.phase!=CustomMapTransferPhase::Requesting || !custom_map_manifest_valid(m,error) || !custom_map_request_matches(status_.request,m)) {
        fail(error.empty()?"Custom map offer does not match the selected world.":error);error=status_.error;return false;
    }
    try {
        map_=std::make_shared<std::vector<uint8_t>>();preview_=std::make_shared<std::vector<uint8_t>>();
        if(status_.request.known_package_sha256!=m.package_sha256){map_->reserve(size_t(m.map_bytes));preview_->reserve(size_t(m.preview_bytes));}
    } catch(const std::exception&) {fail("Not enough memory for the custom map package.");error=status_.error;return false;}
    status_.manifest=m;status_.total=m.map_bytes+m.preview_bytes;status_.received=0;status_.phase=CustomMapTransferPhase::Receiving;return true;
}
bool CustomMapReceiver::chunk(uint64_t token,uint64_t epoch,uint64_t offset,std::span<const uint8_t> data,std::string& error) {
    if(token!=token_ || epoch!=status_.request.epoch || status_.phase==CustomMapTransferPhase::Cancelled || status_.phase==CustomMapTransferPhase::Failed || status_.phase==CustomMapTransferPhase::Complete)return true;
    if(status_.phase!=CustomMapTransferPhase::Receiving || data.empty() || data.size()>custom_map_chunk_bytes || offset!=status_.received ||
       status_.received>status_.total || data.size()>status_.total-status_.received) {
        fail("Invalid custom map chunk offset or length.");error=status_.error;return false;
    }
    const auto map_remaining=status_.manifest.map_bytes-map_->size();
    const auto map_count=size_t(std::min<uint64_t>(map_remaining,data.size()));
    if(map_count){map_->insert(map_->end(),data.begin(),data.begin()+map_count);map_digest_.update(data.data(),map_count);}
    if(map_count<data.size()){const auto tail=data.subspan(map_count);preview_->insert(preview_->end(),tail.begin(),tail.end());preview_digest_.update(tail.data(),tail.size());}
    status_.received+=data.size();return true;
}
bool CustomMapReceiver::finish(uint64_t token,uint64_t epoch,std::string& error,bool cached) {
    if(token!=token_ || epoch!=status_.request.epoch || status_.phase==CustomMapTransferPhase::Cancelled || status_.phase==CustomMapTransferPhase::Failed || status_.phase==CustomMapTransferPhase::Complete)return true;
    if(cached && status_.phase==CustomMapTransferPhase::Receiving && !status_.received && status_.request.known_package_sha256==status_.manifest.package_sha256){
        auto package=std::make_shared<CustomMapPackage>();package->manifest=status_.manifest;release();complete_=std::move(package);status_.received=status_.total;status_.phase=CustomMapTransferPhase::Complete;return true;
    }
    if(cached){fail("Host reported an unverified cached map package.");error=status_.error;return false;}
    if(status_.phase!=CustomMapTransferPhase::Receiving || status_.received!=status_.total) {
        fail("Custom map transfer ended before all bytes arrived.");error=status_.error;return false;
    }
    status_.phase=CustomMapTransferPhase::Verifying;
    if(gc::Sha256::hex(map_digest_.finish())!=status_.manifest.map_sha256 ||
       (status_.manifest.preview_bytes && gc::Sha256::hex(preview_digest_.finish())!=status_.manifest.preview_sha256)) {
        fail("Custom map or preview checksum differs.");error=status_.error;return false;
    }
    auto package=std::make_shared<CustomMapPackage>();package->manifest=status_.manifest;package->map=std::move(map_);package->preview=std::move(preview_);complete_=std::move(package);
    status_.phase=CustomMapTransferPhase::Complete;status_.error.clear();return true;
}
void CustomMapReceiver::fail(const std::string& reason){release();status_.phase=CustomMapTransferPhase::Failed;status_.error=reason;}
void CustomMapReceiver::cancel(const std::string& reason){release();status_.phase=CustomMapTransferPhase::Cancelled;status_.error=reason;}
std::optional<CustomMapDownload> CustomMapReceiver::take(){if(!complete_)return {};const bool cached=!complete_->map;CustomMapDownload out{status_.request,std::move(complete_),cached};return out;}
} // namespace port::coop
