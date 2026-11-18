// Production opacity hook with guest RAM and the original alpha-mode helper as
// collaborators. Uses the real one-entry original-call bypass token.
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
namespace ssne {
#include "../runtime/src/native_opacity_coverage.inc"
}
namespace {
using namespace ssne::native_opacity_coverage;
constexpr uint32_t ram_size=24u*1024u*1024u,material=0x80300000u,base=0x80301000u,opacity=0x80302000u;
std::map<uint32_t,PPCFunc> hooks;unsigned assertions=0,prior_calls=0,original_calls=0,installs=0;
bool throw_extra=false;
struct Call {uint32_t mode,stage,msr;};std::vector<Call> calls;
std::array<uint32_t,16> emitted{};
void require(bool value,const char* why){++assertions;if(!value)throw std::runtime_error(why);}
uint32_t word(uint32_t mode){
    switch(mode){case 1001:return 0x08FFC0u;case 1002:return 0x08FF80u;case 2000:return 0x08F2F0u;case 2001:return 0x08E270u;default:return 0;}
}
void original(PPCContext& c){
    if(!SSNEOriginalCall::consume(c,alpha_address) && *ssne_hook_slot(alpha_address)){(*ssne_hook_slot(alpha_address))(c);return;}
    const auto mode=c.r[3],stage=mem_read32(stage_address);++original_calls;calls.push_back({mode,stage,c.msr});
    if(throw_extra && stage==0){throw_extra=false;throw std::runtime_error("fixture exception");}
    require(stage<16,"Original fixture got invalid stage");
    mem_write32(alpha_modes+stage*4,mode);emitted[stage]=word(mode);
    // Distinct caller-save effects make leaking the nested context observable.
    c.r[0]=0xA0A00000u|stage;c.r[3]=stage;c.r[4]=mode^0x5566u;c.r[11]=stage*4;
    c.lr=alpha_address+0x334;c.cr[0]=uint8_t(stage&15);
}
void prior(PPCContext& c){++prior_calls;c.r[10]^=0x12345678u;ssne_call_original(c,alpha_address,original);}
PPCContext context(unsigned stage=1,unsigned ordinal=4){
    PPCContext c{};
    for(unsigned i=0;i<32;++i){c.r[i]=0x77000000u+i;c.ps0[i]=double(i)+.25;c.ps1[i]=-double(i)-.5;}
    c.r[1]=0x817FF000u;c.r[3]=1001;c.r[13]=0x802E22A0u;c.r[23]=material+12;c.r[25]=ordinal;c.r[26]=material;c.r[29]=stage;c.lr=0x80124DF4u;c.msr=0xA032u;
    return c;
}
void fixture(unsigned stage=1,unsigned ordinal=4){
    mem_write32(0x80000000u,0x47334250u);mem_write32(layer_address,0x9421FF98u);mem_write32(alpha_address,0x9421FFF8u);mem_write32(0x80124DF0u,0x4BFFEB65u);
    mem_write32(stage_address,stage);mem_write32(material+12,base);mem_write32(material+12+ordinal,opacity);mem_write32(opacity+4,2);
    for(unsigned i=0;i<16;++i){mem_write32(alpha_modes+i*4,i?1002:2000);mem_write32(color_modes+i*4,i==stage?2007:2000);emitted[i]=word(i?1002:2000);}
    calls.clear();prior_calls=original_calls=0;throw_extra=false;active=false;
    previous=prior;hooks[alpha_address]=alpha;
}
void expect(PPCContext c,bool changed,std::vector<uint8_t>& ram){
    const auto stage=mem_read32(stage_address);const auto before=ram;const auto original_msr=c.msr;auto expected_cpu=c;
    // A normal forwarding call is the CPU oracle; only the final helper's mode
    // changes for eligible chains. The extra coverage helper uses private CPU.
    expected_cpu.r[3]=changed?2001:c.r[3];
    ssne_call_original(expected_cpu,alpha_address,prior);
    auto expected_ram=ram;expected_ram=before;
    auto write_expected=[&](uint32_t at,uint32_t value){for(unsigned i=0;i<4;++i)expected_ram[(at&0x3ffffffu)+i]=uint8_t(value>>(24-8*i));};
    write_expected(alpha_modes+stage*4,changed?2001:c.r[3]);if(changed)write_expected(alpha_modes,1001);
    ram=before;ssne::g_ram=ram.data();calls.clear();prior_calls=original_calls=0;
    original(c);
    require(!std::memcmp(&c,&expected_cpu,sizeof(c)),"Nested coverage call leaked CPU effects or forwarding changed");
    require(ram==expected_ram,"Hook changed RAM outside the two original mode-cache writes");
    require(mem_read32(stage_address)==stage && !active,"Stage selector or reentry guard leaked");
    require(prior_calls==unsigned(changed?2:1) && original_calls==unsigned(changed?2:1),"Prior/original hook chain was skipped or repeated");
    require(calls.size()==unsigned(changed?2:1),"Wrong helper call count");
    if(changed){require(!(calls[0].msr&0x8000u) && calls[1].msr==original_msr,"Temporary selector allowed IRQ/reschedule or changed caller MSR");require(calls[0].stage==0 && calls[0].mode==1001 && calls[1].stage==stage && calls[1].mode==2001,"Wrong coverage/fade call order");require(emitted[0]==0x08FFC0u && emitted[stage]==0x08E270u,"Actual helper emitted wrong alpha operands");}
}
unsigned evaluate(uint32_t w,unsigned texture,unsigned raster,unsigned prev){
    const unsigned in[]={prev,0,0,0,texture,raster,0,0};
    const auto a=in[(w>>13)&7],b=in[(w>>10)&7],cc=in[(w>>7)&7],d=in[(w>>4)&7];
    return d+((a*256+(int(b)-int(a))*int(cc+(cc>>7))+128)>>8);
}
}
namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return ram_size;}uint32_t guest_ram_end(){return 0x80000000u+ram_size;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("MMIO read");}void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("MMIO write");}
void log_message(const std::string&){}
}
const SSNEFuncEntry g_ssne_funcs[]={{alpha_address,original,"alpha"},{layer_address,original,"layer"}};
const size_t g_ssne_func_count=2;const uint32_t g_ssne_entry=0;const bool g_ssne_has_game_code=true;
const SSNEFuncEntry* ssne_find_func(uint32_t a){for(const auto& e:g_ssne_funcs)if(e.address==a)return &e;return nullptr;}
PPCFunc* ssne_hook_slot(uint32_t a){return &hooks[a];}void ssne_install_hook(uint32_t a,PPCFunc f){hooks[a]=f;++installs;}
int main()try{
    std::vector<uint8_t> ram(ram_size);ssne::g_ram=ram.data();
    fixture();previous=nullptr;hooks[alpha_address]=prior;install();
    require(installs==0 && !installed && hooks[alpha_address]==prior && !previous,"Partial PAL signature installed the hook");
    for(unsigned stage:{1u,2u,15u})for(unsigned ordinal:{4u,8u,12u}){fixture(stage,ordinal);expect(context(stage,ordinal),true,ram);}
    for(unsigned texture:{0u,64u,128u,255u})for(unsigned raster:{0u,96u,255u})for(unsigned fade:{0u,1u,128u,255u}){
        fixture();expect(context(),true,ram);const auto coverage=evaluate(emitted[0],texture,raster,0);const auto result=evaluate(emitted[1],fade,0,coverage);
        require(coverage==texture,"Coverage depends on lit raster alpha");
        const auto wanted=(texture*(fade+(fade/128))+128)/256;
        require(result==wanted,"Zero/partial/opaque coverage or authored opacity fade changed");
        if(!texture || !fade)require(result==0,"Transparent coverage/fade became opaque");
        if(texture==255)require(result==fade,"Opaque texel changed authored fade");
    }
    for(unsigned kind=0;kind<23;++kind){
        fixture(2);auto c=context(2);
        switch(kind){
        case 0:c.lr+=4;break;case 1:c.r[3]=2001;break;case 2:c.r[13]+=4;break;case 3:c.r[1]|=1;break;case 4:c.r[1]=0;break;
        case 5:mem_write32(0x80000000u,0x47334245u);break;case 6:mem_write32(0x80124DF0u,0x60000000u);break;
        case 7:mem_write32(layer_address,0);break;case 8:mem_write32(alpha_address,0);break;case 9:c.r[26]=0;break;
        case 10:c.r[23]+=4;break;case 11:c.r[25]=16;break;case 12:mem_write32(stage_address,0);c.r[29]=0;break;
        case 13:c.r[29]=1;break;case 14:mem_write32(material+12,0);break;case 15:mem_write32(material+16,ssne::guest_ram_end()-4);break;
        case 16:mem_write32(material+16,base);break;case 17:mem_write32(opacity+4,1);break;case 18:mem_write32(opacity+4,3);break;
        case 19:mem_write32(alpha_modes,1001);break;case 20:mem_write32(alpha_modes+4,2001);break;case 21:mem_write32(color_modes+8,2008);break;
        case 22:active=true;break;
        }
        // Forwarding retains an already-active outer guard, so test it directly.
        if(kind==22){auto old=c;ssne_call_original(old,alpha_address,prior);calls.clear();original(c);require(!std::memcmp(&old,&c,sizeof(c)) && active,"Nested call did not forward unchanged");active=false;}
        else expect(c,false,ram);
    }
    fixture();auto c=context(),before=c;throw_extra=true;bool threw=false;
    try{original(c);}catch(const std::runtime_error&){threw=true;}
    require(threw && !active && mem_read32(stage_address)==1 && !std::memcmp(&c,&before,sizeof(c)),"Exceptional extra call leaked selector/guard/CPU state");
    fixture();c=context();ssne_call_original(c,alpha_address,original);require(calls.size()==1 && calls[0].mode==1001,"Original bypass was not one-entry");
    fixture();expect(context(),true,ram);
    std::cout<<"Native opacity coverage: "<<assertions<<" assertions; exact caller/context/modes, malformed/stale fallback, full CPU/RAM preservation, prior hook, cleanup and zero/partial/opaque fade passed.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
