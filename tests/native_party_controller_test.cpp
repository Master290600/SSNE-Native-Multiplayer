// Eight logical inputs need two native four-controller banks. Simply indexing
// controller 4 after controller 3 would overwrite the original rumble records.
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "../experimental/native_party/controller_banks.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void require(bool condition,const char* message) {
    if(!condition){std::fprintf(stderr,"%s\n",message);std::exit(1);}
}
static std::vector<uint8_t> bytes(uint32_t base,unsigned size) {
    std::vector<uint8_t> result(size);
    for(unsigned i=0;i<size;++i)result[i]=mem_read8(base+i);
    return result;
}
int main(int argc,char**argv) {
    require(argc==2,"Pass PAL game_data");
    PPCContext base;std::string error;
    require(ssne::boot(base,argv[1],error)!=0,error.c_str());
    require(mem_read32(0x80000000)==0x47334250,"This native-layout fixture audits PAL only");
    const auto constructor=ssne_find_func(0x8014F750);
    const auto process=ssne_find_func(0x8014F7F4);
    require(constructor && process,"Native controller functions missing");
    constexpr unsigned stride=ssne::native_party::ControllerBanks::stride, size=ssne::native_party::ControllerBanks::bank_size;
    constexpr std::array<uint32_t,2> banks{0x81000040,0x81004040};
    constexpr std::array<uint32_t,2> statuses{0x81008000,0x81008100};
    ssne::native_party::ControllerBanks logical{banks};
    require(logical.controller(8)==0,"Invalid logical input must not wrap to another player");
    for(unsigned slot=0;slot<8;++slot) {
        require(logical.controller(slot)==banks[slot/4]+(slot%4)*stride,"Logical slot maps to the wrong native bank");
        for(unsigned other=0;other<slot;++other)require(logical.controller(slot)!=logical.controller(other),"Logical inputs alias each other");
    }
    constexpr uint32_t status_pointer=0x802ABAD8;
    const auto old_status=mem_read32(status_pointer);
    for(unsigned bank=0;bank<2;++bank) {
        const auto address=banks[bank];
        for(unsigned i=0;i<size+8;++i)mem_write8(address-4+i,0xA5);
        PPCContext call=base;call.msr&=~0x8000u;call.r[3]=address;
        constructor->func(call);
        require(call.r[3]==address,"Native constructor returned another bank");
        require(mem_read32(address-4)==0xA5A5A5A5 && mem_read32(address+size)==0xA5A5A5A5,
                "Native constructor exceeded its controller/rumble bank");
        for(unsigned slot=0;slot<4;++slot) {
            const auto controller=address+slot*stride;
            require(mem_read32(controller)==0 && mem_read32(controller+8)==1,"Native controller initialization differs");
            mem_write32(controller,1); // virtual pads are connected, no physical SI reset
            const auto status=statuses[bank]+slot*12;
            for(unsigned i=0;i<12;++i)mem_write8(status+i,0);
            mem_write16(status,uint16_t(1u<<(bank*4+slot)));
            mem_write8(status+2,uint8_t(int(bank*4+slot)*20-70));
            mem_write8(status+3,uint8_t(60-int(bank*4+slot)*15));
        }
    }
    auto run=[&](unsigned bank) {
        mem_write32(status_pointer,statuses[bank]);
        PPCContext call=base;call.msr&=~0x8000u;call.r[3]=banks[bank];call.ps0[1]=call.ps1[1]=1./59.94;
        process->func(call);
        require(mem_read32(banks[bank]-4)==0xA5A5A5A5 && mem_read32(banks[bank]+size)==0xA5A5A5A5,
                "Native input processing exceeded its bank");
    };
    for(unsigned tick=0;tick<60;++tick) {
        run(0);
        auto first=bytes(banks[0],size), first_status=bytes(statuses[0],48);
        run(1);
        require(bytes(banks[0],size)==first && bytes(statuses[0],48)==first_status,
                "Players 5..8 changed players 1..4 input or rumble state");
    }
    mem_write32(status_pointer,old_status);
    std::puts("Two native controller banks processed eight independent input slots without overlapping rumble/state fields");
    std::fflush(stdout);
    std::_Exit(0);
}
