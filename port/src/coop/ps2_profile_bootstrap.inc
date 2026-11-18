#include <algorithm>
#include <iterator>
#include <string_view>
#include <tuple>
// PAL's default-profile loop creates one profile per character. Its existing
// sixteen profile slots must remain compatible with saved user profiles even
// when the independent character catalog grows beyond sixteen entries.
namespace port::coop {
inline PPCFunc ps2_profile_create_original = nullptr;
inline PPCFunc ps2_character_append_original = nullptr;
inline bool ps2_validate_character_revision() {
    // Hook registration precedes guest loading. Validate only on the first
    // matching actual Player initialization, after guest code is available.
    static const bool verified = [] {
        for (auto [at, length, expected] : {
                std::tuple{0x8015A774u, 32u, 0xf7011684cffba044ull},
                std::tuple{0x800B12F0u, 48u, 0x642a247bca14a413ull}}) {
            uint64_t hash = 14695981039346656037ull;
            for (unsigned i = 0; i < length; ++i) { hash ^= mem_read8(at + i); hash *= 1099511628211ull; }
            if (hash != expected) throw std::runtime_error("PAL singleplayer character initialization differs");
        }
        return true;
    }();
    return verified;
}
inline bool ps2_profile_readable(uint32_t p, unsigned n) {
    return p >= 0x80000000u && n <= ssne::guest_ram_size() && p <= ssne::guest_ram_end() - n;
}
inline void ps2_profile_create(PPCContext& context) {
    const auto limit = mem_read32(context.r[13] - 0x78BCu);
    if (context.lr == 0x800C208Cu && context.r[5] >= limit) {
        context.r[3] = 0;
        return;
    }
    ssne_call_original(context, 0x800C2414u, ps2_profile_create_original);
}
inline void ps2_character_append(PPCContext& context) {
    // Only the skin-name append inside original Player::Initialize. PAL
    // ordinarily substitutes a fixed Sam name for every solo-game profile.
    // Keep mode, profile, camera, inventory and all other append callers intact.
    if (context.lr == 0x800B1320u && installed_skin_count.load(std::memory_order_relaxed) == 17 && ps2_validate_character_revision() &&
        mem_read32(context.r[13] - 0x69D4u) == 0 && ps2_profile_readable(context.r[31], 0x190)) {
        const auto actor = context.r[31], profile = mem_read32(actor + 0x18Cu);
        if (ps2_profile_readable(profile, 4)) {
            auto call = context;
            call.r[1] -= 0x100;
            call.msr &= ~0x8000u;
            mem_write32(call.r[1], context.r[1]);
            call.r[3] = profile;
            ssne_call(call, 0x800C2314u); // Original profile model-name lookup.
            std::string name;
            for (unsigned i = 0; i < 32 && ps2_profile_readable(call.r[3] + i, 1); ++i) {
                const auto ch = mem_read8(call.r[3] + i);
                if (!ch) break;
                name += char(ch);
            }
            constexpr std::string_view names[]{"Sam","baby","cheerleader","clown","cowboy","elvis",
                "hippy","robot","skeleton","spaceboy","vampire","ballwraith","nesbot",
                "superfella","wrestler","drivethrough","fisherman"};
            if (std::find(std::begin(names), std::end(names), std::string_view{name}) != std::end(names)) {
                context.r[4] = call.r[3];
                ssne::log_message("[ps2-singleplayer-character] actor=" + std::to_string(actor) + " model=" + name);
            }
        }
    }
    ssne_call_original(context, 0x8015A774u, ps2_character_append_original);
}
inline void install_ps2_profile_defaults() {
    constexpr uint32_t address = 0x800C2414u, append = 0x8015A774u;
    const auto entry = ssne_find_func(address), append_entry = ssne_find_func(append);
    if (!entry || !append_entry) throw std::runtime_error("PAL profile initialization unavailable");
    const auto previous = *ssne_hook_slot(address), append_previous = *ssne_hook_slot(append);
    ps2_profile_create_original = previous ? previous : entry->func;
    ps2_character_append_original = append_previous ? append_previous : append_entry->func;
    ssne_install_hook(address, ps2_profile_create);
    ssne_install_hook(append, ps2_character_append);
}
}
