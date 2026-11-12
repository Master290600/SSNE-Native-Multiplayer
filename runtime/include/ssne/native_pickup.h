#pragma once
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include <string_view>

namespace ssne {
// The native factory retains this configuration at pickup+DC. PAL 8010C6EC
// (USA 8010C734) allocates 0x40 bytes and transfers ownership with pickup+E0.
// Keeping ownership in guest RAM also makes checkpoint restoration atomic.
// The caller supplies a scratch guest frame, just as for the native factory.
class NativePickupConfig {
    PPCContext context_;
    uint32_t address_=0,heap_=0,bytes_=0,counter_=0,free_=0;
public:
    NativePickupConfig(const PPCContext& context,uint32_t type,std::string_view model={},uint32_t value=0):context_(context) {
        const bool pal=mem_read32(0x80000000)==0x47334250u;
        const auto alloc=pal?0x80116084u:0x801160CCu;
        free_=pal?0x801161ECu:0x80116234u;
        if(!ssne_find_func(alloc) || !ssne_find_func(free_))return;
        heap_=mem_read32(context.r[13]-(pal?0x6DE0u:0x6DE8u));
        counter_=context.r[13]-(pal?0x6DF0u:0x6DF8u);
        // Custom treasure54 retains an extra {model name, score} at +2C.
        // Keep both metadata and the string in this same owned allocation:
        // 800C6B60 frees exactly config, using its actual heap block size.
        const auto size=uint32_t(0x40+(model.empty()?0:8+model.size()+1));
        auto call=context_;call.r[3]=heap_;call.r[4]=size;ssne_call(call,alloc);
        address_=call.r[3];
        if(!address_)return;
        bytes_=mem_read32(address_-0x18);
        mem_write32(counter_,mem_read32(counter_)-bytes_);
        for(unsigned i=0;i<0x40;i+=4)mem_write32(address_+i,0);
        mem_write32(address_,type);
        mem_write32(address_+8,~0u); // no linked mission script
        mem_write32(address_+0x30,~0u); // original dynamic-spawn sentinel
        mem_write32(address_+0x34,f2u(1.f));
        if(!model.empty()){
            mem_write32(address_+0x2C,address_+0x40);
            mem_write32(address_+0x40,address_+0x48);mem_write32(address_+0x44,value);
            for(unsigned i=0;i<model.size();++i)mem_write8(address_+0x48+i,uint8_t(model[i]));
            mem_write8(address_+0x48+uint32_t(model.size()),0);
        }
    }
    NativePickupConfig(const NativePickupConfig&)=delete;
    NativePickupConfig& operator=(const NativePickupConfig&)=delete;
    ~NativePickupConfig() {
        if(!address_)return;
        auto call=context_;call.r[3]=heap_;call.r[4]=address_;
        ssne_call(call,free_);
        mem_write32(counter_,mem_read32(counter_)+bytes_);
    }
    uint32_t address()const{return address_;}
    void transfer_to(uint32_t pickup) {
        // The factory has already installed address_ at pickup+DC. Its original
        // destructor now releases the allocation, including heap accounting.
        mem_write32(pickup+0xE0,1);
        address_=0;
    }
};
}
