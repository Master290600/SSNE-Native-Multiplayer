#pragma once
#include <cstdint>
#include <functional>
namespace ssne {
struct NativeEntityNotice {
    enum class Event { CreatedActor,CreatedPickup,Registered,Removing,LevelExit };
    Event event;
    uint32_t object=0,definition=0;
    int32_t update_group=-2; // -2: nonconstant getter; -1: native nonregistered object
};
using NativeEntityObserver=std::function<void(const NativeEntityNotice&)>;
uint64_t observe_native_entities(NativeEntityObserver);
void stop_observing_native_entities(uint64_t);
bool native_entity_hooks_available();
// Runtime hooks emit on the guest thread without changing the native object.
void notify_native_entity(NativeEntityNotice);
void native_entity_hooks_installed(bool available);
}
