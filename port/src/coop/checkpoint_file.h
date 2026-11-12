#pragma once
#include "checkpoint_codec.h"
#include "wire_protocol.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <ctime>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace port::coop::savefile {
using Digest=std::array<uint8_t,32>;
// Resolve the system provider dynamically so every existing build entry point
// uses the same implementation without an optional link-time dependency.
class Sha256 {
    struct Api {
        HMODULE module=LoadLibraryExW(L"bcrypt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&BCryptOpenAlgorithmProvider) open=nullptr;
        decltype(&BCryptCloseAlgorithmProvider) close=nullptr;
        decltype(&BCryptCreateHash) create=nullptr;
        decltype(&BCryptDestroyHash) destroy=nullptr;
        decltype(&BCryptHashData) update=nullptr;
        decltype(&BCryptFinishHash) finish=nullptr;
        Api(){
            if(!module)throw std::runtime_error("Cannot open system SHA-256 provider");
#define SSNE_HASH_API(field,name) field=reinterpret_cast<decltype(field)>(GetProcAddress(module,#name));
            SSNE_HASH_API(open,BCryptOpenAlgorithmProvider)
            SSNE_HASH_API(close,BCryptCloseAlgorithmProvider)
            SSNE_HASH_API(create,BCryptCreateHash)
            SSNE_HASH_API(destroy,BCryptDestroyHash)
            SSNE_HASH_API(update,BCryptHashData)
            SSNE_HASH_API(finish,BCryptFinishHash)
#undef SSNE_HASH_API
            if(!open || !close || !create || !destroy || !update || !finish)throw std::runtime_error("Incomplete system SHA-256 provider");
        }
        ~Api(){if(module)FreeLibrary(module);}
    };
    static Api& api(){static Api a;return a;}
    BCRYPT_ALG_HANDLE algorithm_=nullptr;
    BCRYPT_HASH_HANDLE hash_=nullptr;
public:
    Sha256(){
        auto& a=api();
        if(a.open(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot initialize SHA-256");
        if(a.create(algorithm_,&hash_,nullptr,0,nullptr,0,0)<0){a.close(algorithm_,0);algorithm_=nullptr;throw std::runtime_error("Cannot initialize SHA-256 state");}
    }
    Sha256(const Sha256&)=delete;
    ~Sha256(){auto& a=api();if(hash_)a.destroy(hash_);if(algorithm_)a.close(algorithm_,0);}
    void add(const void* bytes,size_t size){
        auto p=static_cast<const uint8_t*>(bytes);
        while(size){auto n=ULONG((std::min)(size,size_t(1024*1024)));if(api().update(hash_,const_cast<uint8_t*>(p),n,0)<0)throw std::runtime_error("SHA-256 update failed");p+=n;size-=n;}
    }
    Digest finish(){Digest result{};if(api().finish(hash_,result.data(),ULONG(result.size()),0)<0)throw std::runtime_error("SHA-256 finalization failed");return result;}
};
inline Digest digest(const std::vector<uint8_t>& bytes,size_t size){Sha256 h;h.add(bytes.data(),size);return h.finish();}
inline Digest hash_file(const std::filesystem::path& path){
    const auto size=std::filesystem::file_size(path);const auto stamp=std::filesystem::last_write_time(path);
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot read checkpoint identity file");
    Sha256 h;std::array<char,65536> block{};uint64_t read=0;
    while(file){file.read(block.data(),block.size());const auto n=file.gcount();h.add(block.data(),size_t(n));read+=uint64_t(n);}
    if(!file.eof() || read!=size || std::filesystem::file_size(path)!=size || std::filesystem::last_write_time(path)!=stamp)throw std::runtime_error("Checkpoint identity file changed while reading");
    return h.finish();
}
struct Identity {
    Digest executable{},data{};
    uint32_t game=0,ram=0,capacity=0,protocol=wire::version;
    bool operator==(const Identity&)const=default;
};
inline Identity identify(const std::filesystem::path& data_root){
    std::array<wchar_t,32768> exe{};const auto n=GetModuleFileNameW(nullptr,exe.data(),DWORD(exe.size()));
    if(!n || n>=exe.size())throw std::runtime_error("Cannot locate current game build");
    Identity id;id.executable=hash_file(std::filesystem::path(exe.data()));
    // Bind to actual contents, not timestamps or paths on this computer.
    std::vector<std::filesystem::path> paths;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(data_root))if(entry.is_regular_file())paths.push_back(entry.path().lexically_relative(data_root));
    std::sort(paths.begin(),paths.end());
    if(paths.empty() || !std::filesystem::is_regular_file(data_root/"sys/main.dol"))throw std::runtime_error("Missing checkpoint game data");
    Sha256 content;
    for(const auto& path:paths){const auto name=path.generic_u8string();checkpoint::Writer w;w.u64(name.size());content.add(w.bytes.data(),w.bytes.size());content.add(name.data(),name.size());const auto d=hash_file(data_root/path);content.add(d.data(),d.size());}
    id.data=content.finish();return id;
}
inline constexpr size_t max_core=128*1024*1024,max_extra=42*1024*1024,header_size=108,max_file=header_size+max_core+max_extra+32;
struct State {Identity identity;uint64_t timestamp=0;std::vector<uint8_t> core,extra;};
inline std::vector<uint8_t> encode(const State& state){
    if(state.core.empty() || state.core.size()>max_core || state.extra.empty() || state.extra.size()>max_extra)throw std::runtime_error("Invalid cooperative save section sizes");
    checkpoint::Writer w;w.bytes.reserve(header_size+state.core.size()+state.extra.size()+32);
    w.u32(0x534E4350);w.u32(1);w.u64(state.timestamp);
    const auto& id=state.identity;w.u32(id.game);w.u32(id.ram);w.u32(id.capacity);w.u32(id.protocol);
    w.bytes.insert(w.bytes.end(),id.executable.begin(),id.executable.end());w.bytes.insert(w.bytes.end(),id.data.begin(),id.data.end());
    w.u32(uint32_t(state.core.size()));w.u32(uint32_t(state.extra.size()));w.u32(0);
    w.bytes.insert(w.bytes.end(),state.core.begin(),state.core.end());w.bytes.insert(w.bytes.end(),state.extra.begin(),state.extra.end());
    auto h=digest(w.bytes,w.bytes.size());w.bytes.insert(w.bytes.end(),h.begin(),h.end());return std::move(w.bytes);
}
inline uint32_t u32(const std::vector<uint8_t>& bytes,size_t at){uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|bytes.at(at+i);return v;}
inline State decode(const std::vector<uint8_t>& bytes,const Identity& expected){
    if(bytes.size()<header_size+34 || bytes.size()>max_file || u32(bytes,0)!=0x534E4350 || u32(bytes,4)!=1 || u32(bytes,104)!=0)throw std::runtime_error("Invalid cooperative save format");
    const auto core=u32(bytes,96),extra=u32(bytes,100);
    if(!core || core>max_core || !extra || extra>max_extra || uint64_t(core)+extra+header_size+32!=bytes.size())throw std::runtime_error("Truncated or oversized cooperative save");
    auto h=digest(bytes,bytes.size()-32);if(!std::equal(h.begin(),h.end(),bytes.end()-32))throw std::runtime_error("Cooperative save checksum differs");
    State state;auto& id=state.identity;state.timestamp=(uint64_t(u32(bytes,8))<<32)|u32(bytes,12);
    id.game=u32(bytes,16);id.ram=u32(bytes,20);id.capacity=u32(bytes,24);id.protocol=u32(bytes,28);
    std::copy_n(bytes.begin()+32,32,id.executable.begin());std::copy_n(bytes.begin()+64,32,id.data.begin());
    if(id.executable!=expected.executable)throw std::runtime_error("Save belongs to a different game build");
    if(id.data!=expected.data || id.game!=expected.game)throw std::runtime_error("Save belongs to different game data");
    if(id.ram!=expected.ram || id.capacity!=expected.capacity || id.protocol!=expected.protocol)throw std::runtime_error("Save uses a different multiplayer configuration");
    state.core.assign(bytes.begin()+header_size,bytes.begin()+header_size+core);state.extra.assign(bytes.begin()+header_size+core,bytes.end()-32);return state;
}
inline State read(const std::filesystem::path& path,const Identity& expected){
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open cooperative save");
    const auto length=file.tellg();if(length<0 || uint64_t(length)>max_file)throw std::runtime_error("Cooperative save is too large");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if(!file || file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Cannot read complete cooperative save");return decode(bytes,expected);
}
inline void write_atomic(const std::filesystem::path& path,const std::vector<uint8_t>& bytes){
    if(bytes.size()>max_file)throw std::runtime_error("Cooperative save is too large");
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    static std::atomic<uint64_t> sequence{0};auto tmp=path;
    tmp+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence);
    struct Temporary {std::filesystem::path path;HANDLE handle=INVALID_HANDLE_VALUE;~Temporary(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);DeleteFileW(path.c_str());}} file{tmp};
    file.handle=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(file.handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create cooperative save temporary file");
    for(size_t offset=0;offset<bytes.size();){DWORD done=0,n=DWORD((std::min)(bytes.size()-offset,size_t(1024*1024)));
        if(!WriteFile(file.handle,bytes.data()+offset,n,&done,nullptr) || !done)throw std::runtime_error("Cannot write cooperative save");offset+=done;}
    if(!FlushFileBuffers(file.handle))throw std::runtime_error("Cannot flush cooperative save");
    CloseHandle(file.handle);file.handle=INVALID_HANDLE_VALUE;
    if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot replace cooperative save; previous slot retained");
}
// The menu only needs a cheap timestamp. Full integrity/compatibility validation
// always occurs on load before any live state is touched.
inline bool read_info(const std::filesystem::path& path,uint64_t& timestamp){
    std::ifstream file(path,std::ios::binary);std::vector<uint8_t> header(header_size);file.read(reinterpret_cast<char*>(header.data()),header.size());
    if(!file || u32(header,0)!=0x534E4350 || u32(header,4)!=1)return false;
    timestamp=(uint64_t(u32(header,8))<<32)|u32(header,12);return true;
}
}
