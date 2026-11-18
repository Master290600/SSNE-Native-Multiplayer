#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace port::coop::invite {

struct MappingStatus {
    bool finished=false,mapped=false;
    std::string external_ip,error;
    uint16_t external_port=0;
};

// Optional IPv4 IGD helper for an already bound ICE UDP socket. start() does
// not wait for discovery. Failure never prevents the caller from using ICE.
// stop() cancels pending work, joins the worker and conditionally removes only
// this object's uniquely marked mapping. A successful mapping is renewed with
// a finite lease; no permanent-lease fallback is used.
class PortMapping {
public:
    PortMapping();
    ~PortMapping();
    PortMapping(const PortMapping&)=delete;
    PortMapping& operator=(const PortMapping&)=delete;
    bool start(std::string local_ipv4,uint16_t udp_port);
    MappingStatus status()const;
    void request_stop(); // Nonblocking; cancel several helpers before joining.
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef SSNE_INVITE_UPNP_TEST
// Present only in the loopback-only fake-IGD test build, never in the game.
struct UpnpTestOptions {
    uint16_t ssdp_port=0;
    unsigned renewal_ms=100,discovery_ms=600,http_timeout_ms=300;
};
void set_upnp_test_options(UpnpTestOptions);
#endif

} // namespace port::coop::invite
