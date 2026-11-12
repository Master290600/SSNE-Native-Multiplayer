#pragma once
#include "ssne/input.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace port::netplay {
inline constexpr uint32_t protocol_version = 16;
inline constexpr unsigned max_players=8;
struct ModControls {
    uint32_t flags = 0, fire_tenths = 30;
    uint32_t spawn_sequence = 0, spawn_id = 0, spawn_count = 1, distance_tenths = 50;
};
struct Input { ssne::PadState pad; int32_t yaw = 0, pitch = 0; ModControls mods; uint32_t view_x=1000000, view_y=1000000; };
struct Status { bool connected = false; uint16_t port = 0; uint64_t tick = 0; unsigned local_player=0, player_count=0, joined=0; std::string message; };
// Lockstep transport, with a one-time host card snapshot before boot.
// No images, audio, native pointers, or guest RAM on the wire.
class Session {
public:
    Session(); ~Session();
    bool start(bool host, const std::string& address, uint16_t port,
               const std::string& pin, uint64_t content, const std::vector<uint8_t>& host_card = {}, unsigned players=2);
    bool wait_ready();
    std::vector<uint8_t> initial_card() const;
    void submit_view(uint32_t horizontal, uint32_t vertical);
    void submit_pad(const ssne::PadState& pad);
    void submit_mouse(double yaw, double pitch);
    void submit_mods(uint32_t flags, uint32_t fire_tenths);
    void submit_spawn(uint32_t id, uint32_t count, uint32_t distance_tenths);
    ModControls requested_mods() const;
    bool advance(uint64_t tick, uint64_t checksum, std::array<Input, max_players>& inputs, bool world_ready = true, bool* wait_for_world = nullptr);
    Status status() const;
    void stop();
private:
    struct Impl; std::unique_ptr<Impl> impl;
};
std::string local_ipv4_addresses();
uint64_t content_hash(const std::string& path);
}
