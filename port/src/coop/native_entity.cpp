#include "ssne/native_entity.h"
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <stdexcept>
namespace ssne {
namespace {
std::mutex mutex;
std::map<uint64_t,NativeEntityObserver> observers;
uint64_t next=1;
std::atomic<bool> enabled{false},available{false};
}
uint64_t observe_native_entities(NativeEntityObserver observer) {
    if(!observer)throw std::invalid_argument("Empty native entity observer");
    std::lock_guard lock(mutex);auto id=next++;observers.emplace(id,std::move(observer));enabled=true;return id;
}
void stop_observing_native_entities(uint64_t id){std::lock_guard lock(mutex);observers.erase(id);enabled=!observers.empty();}
bool native_entity_hooks_available(){return available;}
void native_entity_hooks_installed(bool value){available=value;}
void notify_native_entity(NativeEntityNotice notice) {
    if(!enabled)return;
    std::vector<NativeEntityObserver> callbacks;
    {std::lock_guard lock(mutex);for(const auto& [id,callback]:observers)callbacks.push_back(callback);}
    for(auto& callback:callbacks)callback(notice);
}
}
