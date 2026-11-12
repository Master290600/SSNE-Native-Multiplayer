#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return kRamSize;}uint32_t guest_ram_end(){return 0x80000000u+kRamSize;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO read");}
void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO write");}
void log_message(const std::string&){}
#include "../runtime/src/native_animation_lifetime.inc"
}
namespace {
PPCFunc installed=nullptr;unsigned installs=0,setters=0,destructors=0;
PPCContext expected_context{};std::vector<uint32_t> expected_retired;
void set_callback(PPCContext& c){++setters;mem_write32(c.r[3]+0x74,c.r[6]);mem_write32(c.r[3]+0x6C,c.r[4]);mem_write32(c.r[3]+0x70,c.r[5]);c.r[3]=0;}
bool same_context(const PPCContext& a,const PPCContext& b){
 return !std::memcmp(a.r,b.r,sizeof(a.r)) && !std::memcmp(a.ps0,b.ps0,sizeof(a.ps0)) && !std::memcmp(a.ps1,b.ps1,sizeof(a.ps1)) && !std::memcmp(a.cr,b.cr,sizeof(a.cr)) &&
  a.lr==b.lr && a.ctr==b.ctr && a.xer_so==b.xer_so && a.xer_ov==b.xer_ov && a.xer_ca==b.xer_ca && a.fpscr==b.fpscr && !std::memcmp(a.gqr,b.gqr,sizeof(a.gqr)) &&
  a.hid2==b.hid2 && a.msr==b.msr && a.srr0==b.srr0 && a.srr1==b.srr1 && a.dec==b.dec && !std::memcmp(a.sprg,b.sprg,sizeof(a.sprg));
}
void original_destructor(PPCContext& c){
 check(same_context(c,expected_context),"Callback retirement changed original destructor context");
 for(const auto model:expected_retired)check(!mem_read32(model+0x6C) && !mem_read32(model+0x70) && !mem_read32(model+0x74),"Native destructor ran before its callbacks were retired");
 ++destructors;c.r[3]=0x12345678; // Preserve the original function's observable result.
}
void code(uint32_t address,const char* hex){for(unsigned i=0;hex[i];i+=2)mem_write8(address+i/2,uint8_t(std::stoul(std::string(hex+i,2),nullptr,16)));}
constexpr uint32_t manager=0x80300000,first=0x80400000,control=0x80500000;
void model(uint32_t address,uint32_t next,uint32_t callback,uint32_t context,uint32_t mode){
 for(unsigned i=0;i<0xB0;i+=4)mem_write32(address+i,0xA5000000u+i);
 mem_write32(address,0x8021D668);mem_write32(address+4,next);mem_write32(address+8,manager);mem_write32(address+0x6C,callback);mem_write32(address+0x70,context);mem_write32(address+0x74,mode);
}
void fixture(){
 mem_write32(0x802ABAA4,manager);mem_write32(manager,0x8021EB70);mem_write32(manager+0x14,first);
 model(first,first+0x100,0x80003774,control,1);model(first+0x100,first+0x200,0x80003774,control,2);
 model(first+0x200,first+0x300,0x80003774,control+0x100,1);model(first+0x300,0,0x80004000,control,3);
}
void call_dtor(uint32_t owner,std::vector<uint32_t> retired){
 PPCContext c;for(unsigned i=0;i<32;++i){c.r[i]=0x90000000+i;c.ps0[i]=double(i)+.25;c.ps1[i]=-double(i)-.5;}
 c.r[1]=0x802EE000;c.r[3]=owner;c.r[13]=0x802E22A0;c.lr=0x8012AABC;c.gqr[3]=0x1234;c.cr[1]=7;
 expected_context=c;expected_retired=std::move(retired);installed(c);check(c.r[3]==0x12345678,"Original destructor result lost");
}
}
const SSNEFuncEntry* ssne_find_func(uint32_t address){static const SSNEFuncEntry d{0x800039E8,original_destructor,"destructor"},s{0x801286A8,set_callback,"setter"};return address==d.address?&d:address==s.address?&s:nullptr;}
void ssne_install_hook(uint32_t address,PPCFunc fn){check(address==0x800039E8,"Unexpected hook address");installed=fn;++installs;}
int main(int argc,char** argv){try{
 PPCContext layout_a,layout_b;std::memcpy(&layout_b,&layout_a,sizeof(layout_a));
 constexpr auto fields_end=offsetof(PPCContext,sprg)+sizeof(layout_a.sprg);
 std::fill(reinterpret_cast<unsigned char*>(&layout_b)+fields_end,reinterpret_cast<unsigned char*>(&layout_b)+sizeof(layout_b),0xA5);
 check(same_context(layout_a,layout_b),"CPU state comparison depends on non-state padding bytes");
 layout_b.r[13]^=1;check(!same_context(layout_a,layout_b),"CPU state comparison missed a changed native context register");
 std::vector<uint8_t> memory(ssne::kRamSize);ssne::g_ram=memory.data();mem_write32(0x80000000,0x47334250);
 code(0x800039E8,"9421fff07c0802a6bfc10008900100147c7e1b78480007c17fc3f3784bfffc11");
 code(0x801286A8,"7c691b7890c90074386000009089006c90a900704e800020");
 mem_write8(0x801286A8,0);ssne::native_animation_lifetime::install();check(!installs,"Unverified setter accepted");mem_write8(0x801286A8,0x7c);
 ssne::native_animation_lifetime::install();ssne::native_animation_lifetime::install();check(installs==1 && installed,"Verified lifecycle hook not installed exactly once");
 fixture();std::array<uint8_t,0x400> before{};std::memcpy(before.data(),ssne::host_ptr(first),before.size());
 call_dtor(control,{first,first+0x100});check(setters==2 && destructors==1,"Shared callbacks not both retired");
 for(unsigned i=0;i<before.size();++i){const bool changed=i<0x200 && i%0x100>=0x6C && i%0x100<0x78;check(changed || mem_read8(first+i)==before[i],"Unrelated model state, foreign callback or resource changed");}
 call_dtor(control,{});check(setters==2 && destructors==2,"Repeat destruction touched foreign callbacks");
 fixture();call_dtor(control+0x200,{});check(setters==2,"Stale control address retired another owner's callback");
 for(unsigned malformed=0;malformed<4;++malformed){fixture();
  if(malformed==0)mem_write32(first+0x304,first);
  if(malformed==1)mem_write32(first+0x208,manager+0x100);
  if(malformed==2)mem_write32(first+0x200,0x8021D670);
  if(malformed==3)mem_write32(first+0x304,0x83FFFFF0);
  call_dtor(control,{});check(setters==2 && mem_read32(first+0x6C)==0x80003774,"Invalid registry caused partial or foreign retirement");
 }
 if(argc==3){
  std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> capture((std::istreambuf_iterator<char>(file)),{});check(capture.size()==memory.size(),"Capture must contain complete 64-MiB RAM");memory.swap(capture);ssne::g_ram=memory.data();
  check(mem_read32(0x80000000)==0x47334250,"Capture is not PAL");const auto owner=uint32_t(std::stoul(argv[2],nullptr,0));const auto matches=ssne::native_animation_lifetime::captured_models(owner);
  check(matches.size()>=2,"Capture lacks multiple callbacks retaining the reported controller");const auto prior=setters;call_dtor(owner,matches);check(setters-prior==matches.size(),"Captured registry callback retirement differs");
  std::cout<<"Captured native registry: retired "<<matches.size()<<" exact stale captures using the production walk; setter/destructor collaborators are CPU stubs.\n";
 }
 std::cout<<"Animation callback ownership: exact shared captures, foreign/stale owners, invalid registries, context and model preservation passed.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
