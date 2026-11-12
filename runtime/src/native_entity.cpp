#include "ssne/native_entity.h"
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <stdexcept>
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
namespace ssne {
namespace {
std::mutex mutex;
std::map<uint64_t,NativeEntityObserver> observers;
uint64_t next=1;
std::atomic<bool> enabled{false},available{false};
struct StaticModel {uint32_t file,data;std::string name;};
std::map<uint32_t,StaticModel> static_models;
PPCFunc model_lookup_original=nullptr;
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=guest_ram_size() && p<=guest_ram_end()-n;}
void model_lookup(PPCContext& c){
 const auto output=c.r[4],key=c.r[5],manager=c.r[3];std::string name,directory;
 auto text=[&](uint32_t p){std::string s;for(unsigned i=0;i<192 && readable(p+i,1);++i){const auto ch=mem_read8(p+i);if(!ch)return s;if(ch<32 || ch>126)return std::string{};s+=char(ch);}return std::string{};};
 name=text(key);if(readable(manager,0x6C))directory=text(mem_read32(manager+0x68));
 ssne_call_original(c,0x8011967C,model_lookup_original);
 if(c.r[3] || !readable(output,4))return;
 const auto resource=mem_read32(output);
 if(!readable(resource,0x34) || mem_read32(resource)!=0x8021DC50u || name.empty() || name.size()>=192)return;
 if(!directory.empty())name=directory+"/"+name;name+=".ssm";
 std::lock_guard guard(mutex);static_models[resource]={mem_read32(resource+4),mem_read32(resource+0xC),std::move(name)};
}
}
uint64_t observe_native_entities(NativeEntityObserver observer) {
    if(!observer)throw std::invalid_argument("Empty native entity observer");
    std::lock_guard lock(mutex);auto id=next++;observers.emplace(id,std::move(observer));enabled=true;return id;
}
void stop_observing_native_entities(uint64_t id){std::lock_guard lock(mutex);observers.erase(id);enabled=!observers.empty();}
bool native_entity_hooks_available(){return available;}
void native_entity_hooks_installed(bool value){
 available=value;
 if(value && mem_read32(0x80000000)==0x47334250u){
  const auto* fn=ssne_find_func(0x8011967C);
  if(fn && mem_read32(fn->address)==0x9421FEE0u){model_lookup_original=fn->func;ssne_install_hook(fn->address,model_lookup);}
 }
}
std::string native_static_model_name(uint32_t resource){
 std::lock_guard guard(mutex);const auto it=static_models.find(resource);
 if(it==static_models.end() || !readable(resource,0x34) || mem_read32(resource)!=0x8021DC50u || mem_read32(resource+4)!=it->second.file || mem_read32(resource+0xC)!=it->second.data)return {};
 return it->second.name;
}
uint32_t native_static_model_resource(const std::string& name){
 std::lock_guard guard(mutex);
 for(const auto&[resource,s]:static_models)if(s.name==name && readable(resource,0x34) && mem_read32(resource)==0x8021DC50u && mem_read32(resource+4)==s.file && mem_read32(resource+0xC)==s.data)return resource;
 return 0;
}
void notify_native_entity(NativeEntityNotice notice) {
    if(!enabled)return;
    std::vector<NativeEntityObserver> callbacks;
    {std::lock_guard lock(mutex);for(const auto& [id,callback]:observers)callbacks.push_back(callback);}
    for(auto& callback:callbacks)callback(notice);
}
}
