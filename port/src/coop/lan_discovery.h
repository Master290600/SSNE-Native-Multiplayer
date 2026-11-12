#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
namespace port::coop {
struct FrontendRoom {
    std::string address,name;unsigned port=0,players=0,capacity=0;
    std::string session_id,join_ticket;uint64_t join_expires_ms=0;
};
struct LanJoinGrant {std::string ticket;uint32_t valid_ms=0;};
using LanJoinIssuer=std::function<LanJoinGrant(const std::string&,uint32_t)>;
inline constexpr uint16_t lan_discovery_port=27342;
uint64_t lan_clock_ms();
std::string new_lan_secret(); // 128-bit OS random, hex; empty on failure.
bool valid_lan_secret(const std::string&);
bool local_lan_peer(const std::string&); // Loopback or directly attached IPv4 subnet.
// Merge the same host session reached over multiple interfaces. Endpoint and
// its IP-bound credential always move together; separate sessions/ports remain.
void merge_lan_room(std::vector<FrontendRoom>&,FrontendRoom);
class LanAdvertisement {
public:
    LanAdvertisement();~LanAdvertisement();
    bool start(uint64_t fingerprint,std::function<FrontendRoom()> provider,uint16_t discovery_port=lan_discovery_port,bool loopback=false,LanJoinIssuer issuer={});
    void stop();
private:struct Impl;std::unique_ptr<Impl> impl_;
};
class LanBrowser {
public:
    LanBrowser();~LanBrowser();
    bool search(uint64_t fingerprint,uint16_t discovery_port=lan_discovery_port,bool loopback=false);
    void stop();bool searching()const;
    std::vector<FrontendRoom> rooms()const;
    bool can_join(const FrontendRoom&)const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
