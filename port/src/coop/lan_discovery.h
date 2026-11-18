#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include <optional>
namespace port::coop {
struct FrontendRoom {
    std::string address,name;unsigned port=0,players=0,capacity=0;
    std::string session_id,join_ticket;uint64_t join_expires_ms=0;
    // Optional host metadata; legacy replies retain unknown details. These
    // fields never authorize joins or change the authoritative game rules.
    bool details_known=false,running=false;
    unsigned mode=0,difficulty=0,humans=0,bots=0,spectators=0;
    unsigned enemies=1,ammo_capacity=0,pickups=1;
    std::string map,map_name;
    uint32_t ping_ms=UINT32_MAX; // Locally measured RTT; unknown sorts last.
    uint64_t seen_ms=0,probe_ms=0; // Browser-owned monotonic observation times.
};
// Full bot-filled rooms still admit a human by replacing a bot.
bool lan_room_has_human_slot(const FrontendRoom&);
bool valid_lan_room_details(const FrontendRoom&);
uint64_t lan_clock_ms();
inline constexpr uint64_t lan_room_fresh_ms=30000;
bool lan_room_fresh(const FrontendRoom&,uint64_t now);
enum class LanRoomState {Unknown,Lobby,Running,Stale};
LanRoomState lan_room_state(const FrontendRoom&,uint64_t now);
std::string lan_room_key(const FrontendRoom&);
// Match the displayed identity again before joining; a removed row must never
// turn into the next server when replies, filters or refreshes change the list.
std::optional<FrontendRoom> selected_lan_room(const FrontendRoom&,const std::vector<FrontendRoom>&,uint64_t now);
std::vector<FrontendRoom> stable_lan_room_view(const std::vector<FrontendRoom>& previous,std::vector<FrontendRoom> fresh);
enum class LanRoomSort {Ping,Name,Players,Mode,Map};
struct LanRoomFilter {unsigned mode=0;bool hide_full=false,hide_running=false;};
// Return a presentation copy; the live discovery order remains stable while
// replies arrive. Re-sort on an explicit UI action or completed refresh.
std::vector<FrontendRoom> sorted_lan_rooms(std::vector<FrontendRoom>,LanRoomSort=LanRoomSort::Ping,LanRoomFilter={},uint64_t now=lan_clock_ms());
struct LanJoinGrant {std::string ticket;uint32_t valid_ms=0;};
using LanJoinIssuer=std::function<LanJoinGrant(const std::string&,uint32_t)>;
inline constexpr uint16_t lan_discovery_port=27342;
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
