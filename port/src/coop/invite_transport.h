#pragma once
#include "invite_signalling.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace port::coop::invite {
enum class Phase { Idle,Preparing,Ready,Connecting,Connected,Failed };
enum class Route { Unknown,Direct,Relay };
struct Relay {
    std::string host;
    uint16_t port=3478;
    std::string username,password;
};
struct Config {
    std::string signal_url="https://ntfy.sh",stun_host="stun.cloudflare.com";
    uint16_t stun_port=3478;
    std::vector<Relay> relays;
    bool automatic_mapping=true;
    std::string bind_address;
    bool testing_loopback_candidates=false;
    uint32_t connect_timeout_ms=45000;
    // Empty selects the production WinHTTP factory. Tests supply a private bus.
    std::function<std::unique_ptr<Signalling>()> signalling_factory;
};
struct Status {
    Phase phase=Phase::Idle;
    // Relay means at least one direction of the selected peer path uses TURN.
    Route route=Route::Unknown;
    std::string code,message;
    uint16_t local_port=0;
    unsigned peers=0;
    // ICE can become usable before the existing TCP client connects locally.
    bool network_ready=false;
};
class InviteTransport {
public:
    InviteTransport();
    ~InviteTransport();
    InviteTransport(const InviteTransport&)=delete;
    InviteTransport& operator=(const InviteTransport&)=delete;
    // These allocate local resources and start a worker; no HTTP/ICE wait here.
    bool host(uint16_t local_tcp_port,const std::string& pin,uint64_t fingerprint,const Config&,std::string& error);
    bool join(const std::string& code,uint64_t fingerprint,const Config&,std::string& error);
    Status status() const;
    std::string invitation() const;
    std::string pin() const;
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
