#include "host_capture_clock.h"
#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <chrono>
#include <cmath>
using port::coop::HostCaptureClock;
void require(bool ok){if(!ok)std::abort();}
int main(){
    using namespace std::chrono;
    const auto zero=HostCaptureClock::Clock::time_point{};
    // High render rates produce only the transport budget, without burst catchup.
    for(int fps:{30,45,60,120,240,300,1000}){
        HostCaptureClock clock;int captures=0;
        for(int frame=0;frame<fps*10;++frame)captures+=clock.due(zero+nanoseconds(int64_t(frame)*1000000000/fps));
        require(std::abs(captures-std::min(fps*10,625))<=1);
    }
    HostCaptureClock clock;require(clock.due(zero));require(!clock.due(zero+milliseconds(1)));
    require(clock.due(zero+seconds(60)));require(!clock.due(zero+seconds(60)));
    require(clock.due(zero-milliseconds(1))); // Restored/backward clock resets safely.
    require(!clock.due(zero));clock.reset();require(clock.due(zero));
    // Idle publication is a second clock, never the maintenance/input clock.
    // At any render rate, preserve the original16ms work budget while the
    // expensive world publication alone runs at10Hz without burst catchup.
    for(int fps:{30,45,60,120,240,300,1000}){
        HostCaptureClock maintenance,publication;int local=0,worlds=0;
        for(int frame=0;frame<fps*10;++frame){const auto now=zero+nanoseconds(int64_t(frame)*1000000000/fps);
            if(maintenance.due(now)){++local;worlds+=publication.due(now,milliseconds(100));}}
        require(std::abs(local-std::min(fps*10,625))<=1);
        require(std::abs(worlds-100)<=1);
    }
    HostCaptureClock maintenance,publication;
    require(maintenance.due(zero)&&publication.due(zero,milliseconds(100)));
    require(maintenance.due(zero+milliseconds(16))&&!publication.due(zero+milliseconds(16),milliseconds(100)));
    // Accepted socket/membership, unready, epoch/save or presentation changes
    // reset both budgets. A wake never waits for the old100ms deadline.
    for(int wake:{17,19,24,31,44}){maintenance.reset();publication.reset();const auto now=zero+milliseconds(wake);
        require(maintenance.due(now)&&publication.due(now,milliseconds(100)));}
    require(publication.due(zero+seconds(90),milliseconds(100)));
    require(!publication.due(zero+seconds(90),milliseconds(100)));
    require(publication.due(zero-milliseconds(1),milliseconds(100)));
    // Explicit cadence changes rearm immediately; old callers remain16ms.
    HostCaptureClock cadence;require(cadence.due(zero,milliseconds(100)));
    require(cadence.due(zero+milliseconds(1),milliseconds(16)));
    require(!cadence.due(zero+milliseconds(2),milliseconds(16)));
    // Steady60FPS must remain60; a fresh relative deadline would halve it.
    clock.reset();for(int f=0;f<600;++f)require(clock.due(zero+nanoseconds(int64_t(f)*1000000000/60)));
}
