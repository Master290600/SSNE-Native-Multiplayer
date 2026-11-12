#pragma once
#include "checkpoint_file.h"
#include "world_state.h"
#include <cctype>

namespace port::coop::savefile {
// Read the bootstrap level from the existing authenticated session section.
// This is metadata only; the runtime still validates every component on restore.
inline std::string startup_level(const State& state) {
    checkpoint::Reader bundle(state.extra,max_extra);
    if(bundle.u32()!=0x4E434335)throw std::runtime_error("Save has no cooperative session");
    size_t before=0;for(unsigned i=0;i<5;++i)before+=bundle.u32();
    const auto size=bundle.u32();bundle.need(before);bundle.at+=before;bundle.need(size);
    std::vector<uint8_t> bytes(state.extra.begin()+bundle.at,state.extra.begin()+bundle.at+size);
    bundle.at+=size;bundle.done();
    checkpoint::Reader session(bytes,2*1024*1024);
    if(session.u32()!=0x4E534331 || session.u32()!=state.identity.game || session.u32()!=state.identity.ram || session.u32()!=state.identity.capacity)
        throw std::runtime_error("Save session identity differs");
    session.need(64);session.at+=64; // epochs, lifecycle, cutscene and AFK timing
    WorldDelta delta;WorldState configuration;std::string error;
    if(!decode_delta(session.blob(1024*1024),delta,error) || !delta.full || !apply_delta(configuration,delta,error))
        throw std::runtime_error("Invalid saved campaign configuration: "+error);
    const auto& level=configuration.level;
    if(level.empty() || level.size()>64 || std::any_of(level.begin(),level.end(),[](unsigned char c){return !std::isalnum(c) && c!='_';}))
        throw std::runtime_error("Invalid saved campaign level");
    return level;
}
}
