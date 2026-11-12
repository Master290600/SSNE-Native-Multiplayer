#include "../runtime/src/native_vehicle_input.h"
#include <iostream>
#include <stdexcept>
using ssne::detail::NativeVehicleInputCache;
using ssne::detail::native_vehicle_controls;
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
int main() try {
    NativeVehicleInputCache cache;
    require(!cache.any(1000),"New session inherited a vehicle");
    cache.observe(0,true,1000);
    const bool host_vehicle=cache.local(true,0,3,1199);
    // The host does not call the on-foot look hook while driving. Its expired
    // aim timer must not turn fire/Space into the native menu's A/use action.
    const bool expired_look=false;
    require(expired_look || native_vehicle_controls(host_vehicle,true,true,false,false,false),
            "Network host driving without look updates was classified as menu control");
    require(!cache.active(0,1200),"Stale driver input did not expire");
    require(!cache.active(0,999),"Clock rewind revived future vehicle state");
    cache.observe(0,false,1050);
    require(!cache.active(0,1050),"Vehicle exit kept controls active for the TTL");
    cache.observe(4,true,2000);
    require(cache.local(true,4,0,2000),"Canonical player4 could not drive");
    require(!cache.local(true,0,4,2000),"Player4 vehicle aliased player0");
    cache.observe(7,true,2000);
    require(cache.local(true,7,0,2000),"Canonical player7 could not drive");
    cache.observe(8,true,2000);cache.observe(-1,true,2000);
    require(!cache.local(true,8,0,2000) && !cache.local(true,-1,0,2000),"Invalid slot selected another driver");
    require(!cache.local(false,4,0,2000),"Single player borrowed a network vehicle");
    cache.observe(2,true,2000);
    require(cache.local(false,7,2,2000),"Single player ignored its selected keyboard port");
    require(!cache.local(false,2,0,2000),"Another split-screen driver enabled local gameplay");
    require(!cache.local(false,0,4,2000),"Invalid single-player port was accepted");
    require(!native_vehicle_controls(true,true,false,false,false,false),"Loading/world-reset enabled vehicle controls");
    require(!native_vehicle_controls(true,true,true,true,false,false),"Pause menu enabled vehicle controls");
    require(!native_vehicle_controls(true,true,true,false,true,false),"Information panel enabled vehicle controls");
    require(!native_vehicle_controls(true,true,true,false,false,true),"Movie/cutscene/results enabled vehicle controls");
    require(!native_vehicle_controls(false,true,true,false,false,false),"On-foot player inherited vehicle controls");
    require(native_vehicle_controls(true,false,false,false,false,false),"Offline driving required a network world");
    cache.reset();
    require(!cache.any(2000),"Leave/rehost/world reset retained driver state");
    cache.observe(1,true,2000);cache.observe(1,false,2001);
    require(!cache.local(true,1,0,2001),"Received client exit/death did not invalidate vehicle state");
    cache.observe(1,true,2002);
    require(native_vehicle_controls(cache.local(true,1,0,2002),true,true,false,false,false),
            "Received client driver failed the same local control predicate");
    std::cout<<"Native vehicle input: expired-look host, eight independent slots, local device selection, immediate exits, TTL/rewind, presentation gates and reset/rehost passed.\n";
    return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
