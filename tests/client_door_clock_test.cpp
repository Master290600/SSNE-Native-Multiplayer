#include "native_door_clock.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void expect(float actual, float expected, const char* scenario) {
    if (!(std::abs(actual - expected) < 0.0001f)) {
        std::cerr << scenario << " expected " << expected << ", got " << actual << '\n';
        throw std::runtime_error(scenario);
    }
}
}

int main() try {
    port::coop::NativeDoorClock clock;
    constexpr unsigned object = 1, segments = 3;
    constexpr double seconds_per_segment = 2.0;
    auto confirm = [&](unsigned segment, float progress, double now) {
        clock.confirm(object, segments, {segment, progress}, seconds_per_segment, now);
    };
    confirm(0, 0.f, 1.0);
    expect(clock.collision_progress(1.4, .25), 0.f, "closed leaf");
    confirm(0, .20f, 1.4);
    expect(clock.collision_progress(1.5, .20), .35f, "travelling leaf");
    expect(clock.authority().progress, .20f, "collision query preserves the confirmed pose");
    expect(clock.collision_progress(2.4, 1.0), .40f, "bounded extrapolation after delayed snapshots");
    clock.stop_collision_prediction();
    expect(clock.collision_progress(2.5, .25), .20f, "leaf confirmed stopped mid-segment");
    confirm(0, .95f, 2.5);
    expect(clock.collision_progress(2.6, .25), 1.f, "end of opening segment");
    confirm(1, .80f, 3.0);
    expect(clock.collision_progress(3.1, .25), .80f, "new segment waits for observed motion");
    confirm(1, .70f, 3.2);
    expect(clock.collision_progress(3.3, .20), .55f, "reversed direction");
    confirm(1, .05f, 4.0);
    expect(clock.collision_progress(4.1, .25), 0.f, "end of reverse travel");
    clock.confirm(object + 1, segments, {0, .45f}, seconds_per_segment, 4.2);
    expect(clock.collision_progress(4.4, .25), .45f, "replacement starts at its own pose");
    return 0;
} catch (const std::runtime_error&) {
    return 1;
}
