#include "ssne/custom_level_mount.h"
#include "gc/bytes.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ssne {
bool append_custom_level_fst(const std::vector<uint8_t>& old,
                             const std::string& alias, uint32_t offset,
                             uint32_t size, std::vector<uint8_t>& result,
                             uint32_t& index, std::string& error) {
    auto fail = [&](const char* why) { error = why; return false; };
    if (alias.size() != 31 || !alias.starts_with("Custom_") ||
        !std::all_of(alias.begin()+7, alias.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }) || !size || (offset & 31) || uint64_t(offset)+size > UINT32_MAX)
        return fail("Invalid custom map virtual disc identity");
    if (old.size() < 12 || old.size() > 2*1024*1024 ||
        gc::be32(old.data()) != 0x01000000u || gc::be32(old.data()+4) != 0)
        return fail("Invalid running virtual disc root");
    const auto count = gc::be32(old.data()+8);
    const uint64_t strings = uint64_t(count)*12;
    if (!count || count >= 100000 || strings >= old.size())
        return fail("Invalid running virtual disc entry count");
    // Validate directory nesting and bounded names before copying anything.
    std::vector<uint32_t> ends{count}, parents{0};
    const auto private_name = "__SSNE_" + alias + ".ssw";
    for (uint32_t n=1;n<count;++n) {
        while (ends.size()>1 && n>=ends.back()) { ends.pop_back(); parents.pop_back(); }
        const auto p=old.data()+n*12;
        const auto tag=gc::be32(p), name=tag&0xffffffu;
        const uint64_t at=strings+name;
        if ((tag>>24)>1 || at>=old.size()) return fail("Invalid running virtual disc entry");
        auto end=std::find(old.begin()+size_t(at),old.end(),uint8_t(0));
        if (end==old.end() || end-(old.begin()+size_t(at))>255 || end==old.begin()+size_t(at))
            return fail("Invalid running virtual disc name");
        const std::string filename(old.begin()+size_t(at),end);
        if (filename=="." || filename==".." || filename.find_first_of("/\\")!=std::string::npos ||
            filename==private_name) return fail("Invalid or duplicate running virtual disc name");
        if (tag>>24) {
            const auto next=gc::be32(p+8);
            if (gc::be32(p+4)!=parents.back() || next<=n || next>ends.back())
                return fail("Invalid running virtual disc directory");
            ends.push_back(next); parents.push_back(n);
        } else if (uint64_t(gc::be32(p+4))+gc::be32(p+8)>offset) {
            return fail("Downloaded map would overlap an existing file");
        }
    }
    const auto name_offset=old.size()-size_t(strings);
    if (name_offset+private_name.size()+1>=0x1000000)
        return fail("Running virtual disc name table is too large");
    std::vector<uint8_t> next(size_t(strings)+12);
    std::copy_n(old.begin(),size_t(strings),next.begin());
    auto put=[](uint8_t* p,uint32_t v){for(unsigned n=0;n<4;++n)p[n]=uint8_t(v>>(24-8*n));};
    put(next.data()+8,count+1);
    put(next.data()+strings,uint32_t(name_offset));
    put(next.data()+strings+4,offset);
    put(next.data()+strings+8,size);
    next.insert(next.end(),old.begin()+size_t(strings),old.end());
    next.insert(next.end(),private_name.begin(),private_name.end());
    next.push_back(0);
    result=std::move(next); index=count;
    return true;
}
} // namespace ssne
