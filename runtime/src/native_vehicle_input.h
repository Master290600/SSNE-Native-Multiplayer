#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace ssne::detail {
// Guest code publishes canonical player slots; device/UI code only reads these
// atomics. Slot4 must never borrow slot0's vehicle state through a port mask.
class NativeVehicleInputCache {
public:
    static constexpr int slots=8;
    static constexpr int64_t lifetime_ms=200;
    NativeVehicleInputCache() { reset(); }
    void reset() { for(auto& seen:last_)seen.store(-1); }
    void observe(int slot,bool driving,int64_t now) {
        if(slot>=0 && slot<slots)last_[slot].store(driving && now>=0?now:-1);
    }
    bool active(int slot,int64_t now) const {
        if(slot<0 || slot>=slots)return false;
        const auto seen=last_[slot].load();
        return seen>=0 && now>=seen && now-seen<lifetime_ms;
    }
    bool local(bool network,int network_slot,int keyboard_port,int64_t now) const {
        const int slot=network?network_slot:keyboard_port;
        if(!network && (slot<0 || slot>=4))return false;
        return active(slot,now);
    }
    bool any(int64_t now) const {
        for(int slot=0;slot<slots;++slot)if(active(slot,now))return true;
        return false;
    }
private:
    std::array<std::atomic<int64_t>,slots> last_;
};

// Keep vehicle gameplay separate from menu/presentation duties even while a
// driver remains seated. This is the actual predicate used by the runtime query.
inline bool native_vehicle_controls(bool driving,bool network,bool world_ready,
                                    bool paused,bool information,bool presentation) {
    return driving && (!network || world_ready) && !paused && !information && !presentation;
}
}
