#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace port::coop::checkpoint {
inline uint64_t hash(const std::vector<uint8_t>& bytes,size_t end) {
    uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<end;++i){h^=bytes[i];h*=1099511628211ull;}return h;
}
struct Writer {
    std::vector<uint8_t> bytes;
    void u32(uint32_t v){for(int s=24;s>=0;s-=8)bytes.push_back(uint8_t(v>>s));}
    void u64(uint64_t v){u32(uint32_t(v>>32));u32(uint32_t(v));}
    void flag(bool v){u32(v?1:0);}
    void real(float v){u32(std::bit_cast<uint32_t>(v));}
    void blob(const std::vector<uint8_t>& b){u32(uint32_t(b.size()));bytes.insert(bytes.end(),b.begin(),b.end());}
    void text(const std::string& s){blob({s.begin(),s.end()});}
    std::vector<uint8_t> finish(){u64(hash(bytes,bytes.size()));return std::move(bytes);}
};
struct Reader {
    const std::vector<uint8_t>& bytes;size_t at=0,end;
    explicit Reader(const std::vector<uint8_t>& b,size_t limit):bytes(b),end(b.size()) {
        if(end<8 || end>limit)throw std::runtime_error("Invalid checkpoint size");
        at=end-8;auto digest=u64();end-=8;at=0;
        if(digest!=hash(b,end))throw std::runtime_error("Checkpoint checksum differs");
    }
    void need(size_t n){if(n>end-at)throw std::runtime_error("Truncated checkpoint");}
    uint32_t u32(){need(4);uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|bytes[at++];return v;}
    uint64_t u64(){const auto hi=u32(),lo=u32();return (uint64_t(hi)<<32)|lo;}
    bool flag(){const auto v=u32();if(v>1)throw std::runtime_error("Invalid checkpoint boolean");return v!=0;}
    float real(){const auto v=std::bit_cast<float>(u32());if(!std::isfinite(v) || std::abs(v)>1e9f)throw std::runtime_error("Invalid checkpoint coordinate");return v;}
    std::vector<uint8_t> blob(size_t limit){const auto n=u32();if(n>limit)throw std::runtime_error("Checkpoint field too large");need(n);std::vector<uint8_t> v(bytes.begin()+at,bytes.begin()+at+n);at+=n;return v;}
    std::string text(size_t limit){const auto b=blob(limit);return {b.begin(),b.end()};}
    void done(){if(at!=end)throw std::runtime_error("Trailing checkpoint data");}
};
}
