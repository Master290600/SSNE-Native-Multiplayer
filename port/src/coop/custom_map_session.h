#pragma once
#include "authoritative_session.h"
#include "ssne/ppc_context.h"
#include <filesystem>
#include <functional>
#include <memory>

namespace port::coop {
// During menu presentation the proposal is metadata only: it must never
// replace WorldState::level or advance the authoritative world epoch.
inline std::string selected_custom_map_alias(const WorldState& world) {
    const auto phase=world.mission.find("Session.NativePhase");
    if(phase!=world.mission.end() && phase->second==Property(std::string("menu")))
        if(const auto it=world.mission.find("Session.Lobby.Level");it!=world.mission.end()) {
            if(const auto alias=std::get_if<std::string>(&it->second))return *alias;
            return {};
        }
    return world.level;
}
// Configure before starting the guest. Separate from user-authored CustomLevels.
void configure_custom_map_downloads(std::filesystem::path);
class CustomMapSession {
public:
    using Log = std::function<void(const std::string&)>;
    CustomMapSession(AuthoritativeServer*, AuthoritativeClient*, Log);
    ~CustomMapSession();
    void host_update(const WorldState&);
    bool client_ready(PPCContext&, const WorldState&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
