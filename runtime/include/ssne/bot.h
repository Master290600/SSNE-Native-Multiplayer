#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace ssne {
struct BotActor {
    uint32_t id = 0;
    std::array<float,3> position{};
    float health = 0, height = 1.6f;
};
struct BotWorld {
    BotActor host, client;
    float yaw = 0, pitch = 0;
    std::vector<BotActor> enemies;
};
// Read only on the guest thread, at the agreed netplay boundary.
BotWorld netplay_bot_world();
std::vector<BotActor> netplay_bot_enemies();
}
