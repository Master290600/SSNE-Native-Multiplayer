#pragma once
#include "invite_signalling.h"
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    std::function<void(const std::string&)> diagnostic;
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
    void request_stop();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The guest owns the active invitation; this worker owns retired invitations.
// The owner must call prepare() before starting an invitation, then retire()
// promptly when it stops using that transport. Neither call waits for I/O.
class InvitationRetirement {
public:
    ~InvitationRetirement();
    bool prepare();
    void retire(std::unique_ptr<InviteTransport> invitation);
    void drain();
private:
    void run();
    static constexpr std::size_t limit_ = 4;
    std::array<std::unique_ptr<InviteTransport>, limit_> queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::size_t next_ = 0, queued_ = 0, pending_ = 0;
    bool closing_ = false;
};
}
