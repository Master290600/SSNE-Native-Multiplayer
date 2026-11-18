#pragma once
#include "gc/sha256.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace port::coop {
inline std::atomic<unsigned> installed_skin_count{11};
inline bool valid_skin_choice(unsigned skin) {
    return skin==255 || skin<installed_skin_count.load(std::memory_order_relaxed);
}
inline uint64_t ps2_content_fingerprint(const std::filesystem::path& data,uint64_t original) {
    const auto root=data/"files";
    const auto manifest=root/"ps2-skin-content.txt";
    if(!std::filesystem::exists(manifest)) {
        installed_skin_count.store(11,std::memory_order_relaxed);
        return original;
    }
    if(!std::filesystem::is_regular_file(manifest) || std::filesystem::file_size(manifest)>8192)
        throw std::runtime_error("Invalid PS2 content manifest");
    std::ifstream input(manifest,std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(input),{}};
    std::istringstream lines(contents);
    std::string magic;std::getline(lines,magic);
    if(magic!="SSNE-PS2-SKINS-1")throw std::runtime_error("Unknown PS2 content manifest");
    const std::set<std::string> required{
        "base.cfg","Multiplayer/skin_all.ssg","Multiplayer/skin_ballwraith.ssg",
        "Multiplayer/skin_nesbot.ssg","Multiplayer/skin_superfella.ssg",
        "Multiplayer/skin_wrestler.ssg","Multiplayer/skin_drivethrough.ssg",
        "Multiplayer/skin_fisherman.ssg"};
    std::set<std::string> seen;
    std::string line;
    while(std::getline(lines,line)) {
        if(line.empty())continue;
        const auto split=line.find(' ');
        if(split!=64)throw std::runtime_error("Invalid PS2 content checksum record");
        const auto expected=line.substr(0,split),name=line.substr(split+1);
        const bool text=name=="EnglishEUR.tdb"||name=="EnglishUSA.tdb"||name=="German.tdb"||name=="french.tdb";
        if((!required.contains(name)&&!text)||!seen.insert(name).second ||
           expected.find_first_not_of("0123456789abcdef")!=std::string::npos)
            throw std::runtime_error("Invalid PS2 content path/checksum");
        const auto path=root/std::filesystem::path(name);
        if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>64u*1024*1024)
            throw std::runtime_error("PS2 content file missing: "+name);
        auto actual=gc::sha256_file(path.string());
        std::transform(actual.begin(),actual.end(),actual.begin(),[](unsigned char c){return char(c>='A'&&c<='F'?c+32:c);});
        if(actual!=expected)throw std::runtime_error("PS2 content checksum differs: "+name);
    }
    for(const auto& name:required)if(!seen.contains(name))throw std::runtime_error("Incomplete PS2 content manifest");
    if(!seen.contains("German.tdb")||!seen.contains("french.tdb")||!seen.contains("EnglishEUR.tdb"))
        throw std::runtime_error("PS2 content localization is incomplete");
    uint64_t result=original;
    for(unsigned char ch:contents){result^=ch;result*=1099511628211ull;}
    installed_skin_count.store(17,std::memory_order_relaxed);
    return result;
}
}
