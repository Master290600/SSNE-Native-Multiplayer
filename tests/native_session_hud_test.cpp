// Production native CcText adapter with bounded guest RAM and original-call
// collaborators. This verifies ownership/control effects, not rendered pixels.
#include "../runtime/src/native_session_hud.cpp"
#include <iostream>
#include <map>
#include <stdexcept>
#include <limits>
namespace fixture {
using namespace ssne;
constexpr uint32_t ram_size=24u*1024u*1024u,scene=0x80400000u,root=0x80401000u,engine=0x80402000u,heap=0,heap_table=0x80403000u,font=0x80404000u,font_source_table=0x80211000u,font_instance_table=0x80211100u;
unsigned checks=0,allocations=0,deletions=0,shutdowns=0,prior_calls=0;uint32_t next=0x80500000u,current_scene=scene,throw_at=0;float wide=1.f;bool pause_flag=false,info=false,movie=false,cutscene=false,results=false,replica_results=false,reenter=false;
std::vector<uint8_t> ram(ram_size);std::map<uint32_t,unsigned> live;std::map<uint32_t,PPCFunc> hooks;std::vector<uint32_t> calls;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
uint32_t available(){return 0x802E22A0u-0x6DF0u;}
uint32_t allocate(unsigned size,bool accounted){const auto count=(size+15)&~15u;const auto p=next+32;next+=count+64;mem_write32(p-24,count);mem_write32(p-20,heap_table+heap*24);for(unsigned i=0;i<count;++i)mem_write8(p+i,0);live[p]=count;++allocations;if(accounted)mem_write32(available(),mem_read32(available())-count);return p;}
void destroy(uint32_t p,bool accounted){const auto it=live.find(p);require(it!=live.end(),"Double or foreign allocation freed");if(accounted)mem_write32(available(),mem_read32(available())+it->second);mem_write32(p,0xFEEEFEEEu);live.erase(it);++deletions;}
void unlink(uint32_t draw){const auto parent=mem_read32(draw+0x14);auto at=parent+0x18;for(unsigned n=0;n<256;++n){const auto child=mem_read32(at);if(!child)break;if(child==draw){mem_write32(at,mem_read32(child+0x10));mem_write32(child+0x14,0);return;}at=child+0x10;}throw std::runtime_error("Native parent/child unlink failed");}
PPCContext context(){PPCContext c{};for(unsigned i=0;i<32;++i){c.r[i]=0x11000000u+i;c.ps0[i]=double(i)+.2;c.ps1[i]=-double(i)-.3;}c.r[1]=0x817FF000u;c.r[13]=0x802E22A0u;c.r[2]=0x802F22A0u;c.lr=0x80100000u;c.msr=0xA032u;return c;}
void service(uint32_t address,PPCContext& c){
 calls.push_back(address);if(address!=0x80166480u)require(!(c.msr&0x8000u),"Private HUD original call allowed scheduler IRQs");
 // Make private stack/CPU pollution visible to the adapter's restoration oracle.
 mem_write32(c.r[1]-0x500,0xAABBCCDDu);
 if(throw_at==address){throw_at=0;throw std::runtime_error("fixture native callback");}
 const auto a=c.r[3],b=c.r[4];
 switch(address){
 case 0x80116084u:require(a==heap,"Wrong heap allocation handle");c.r[3]=allocate(b,false);break;
 case 0x801161ECu:require(a==heap,"Wrong heap free owner");destroy(b,false);break;
 case 0x800AB5ACu:{
  const auto draw=allocate(0x44,true);mem_write32(draw,ssne::native_session_hud::drawable_vtable);mem_write32(draw+4,engine+4);mem_write32(draw+8,engine);mem_write32(draw+0x14,b);mem_write32(draw+0x10,mem_read32(b+0x18));mem_write32(b+0x18,draw);mem_write32(a,draw);mem_write32(a+4,f2u(float(c.ps0[1])));mem_write32(a+8,f2u(float(c.ps0[2])));mem_write32(a+0xC,c.r[5]);
  if(reenter){reenter=false;auto nested=context();require(!update_native_session_hud(nested,"nested","nested",true),"Nested HUD initialization was not rejected");}break;
 }
 case 0x800AB7C8u:{require(c.ps0[1]>.1 && c.ps0[1]<1.5,"Native font expects scale, not pixel size");const auto handle=allocate(0x40,true);mem_write32(handle,font_instance_table);mem_write32(a+0x14,handle);mem_write32(a+0x18,0);mem_write32(a+0x10,f2u(float(c.ps0[1])));mem_write32(a+0xA0,c.r[5]);mem_write32(a+0xB8,1);mem_write16(mem_read32(a)+0x1E,1);break;}
 case 0x8011D15Cu:mem_write32(a+0x40,b);break;
 case 0x800AB6ECu:mem_write16(mem_read32(a)+0x1E,mem_read16(mem_read32(a)+0x1E)|1);break;
 case 0x800AB6B0u:mem_write16(mem_read32(a)+0x1E,mem_read16(mem_read32(a)+0x1E)&~1u);break;
 case 0x800ABBE4u:{unsigned i=0;for(;i<127 && mem_read8(b+i);++i)mem_write8(a+0x1C+i,mem_read8(b+i));mem_write8(a+0x1C+i,0);mem_write16(mem_read32(a)+0x1E,mem_read16(mem_read32(a)+0x1E)&~1u);break;}
 case 0x800AB728u:mem_write32(a+4,mem_read32(b));mem_write32(a+8,mem_read32(b+4));break;
 case 0x800AB994u:destroy(mem_read32(a+0x14),true);mem_write32(a+0x14,0);break;
 case 0x8011D164u:require(c.r[4]==3,"Native drawable not fully retired");unlink(a);destroy(a,true);break;
 case 0x80166480u:++shutdowns;require(!mem_read32(root+0x18),"Original shutdown ran before own HUD rows retired");c.r[6]=0x600D600Du;break;
 default:break;
 }
 c.r[8]^=0x55667788u;c.ps0[3]=123.5;c.lr=address+8;
}
template<uint32_t Address>void original(PPCContext& c){if(!SSNEOriginalCall::consume(c,Address) && *ssne_hook_slot(Address)){(*ssne_hook_slot(Address))(c);return;}service(Address,c);}
void prior(PPCContext& c){++prior_calls;c.r[5]^=0x9876u;ssne_call_original(c,0x80166480u,original<0x80166480u>);}
void initialize(){
 using namespace ssne::native_session_hud;std::fill(ram.begin(),ram.end(),0);ssne::g_ram=ram.data();live.clear();hooks.clear();calls.clear();rows={};previous_shutdown=nullptr;installed=busy=false;allocations=deletions=shutdowns=prior_calls=0;next=0x80500000u;current_scene=scene;wide=1;pause_flag=info=movie=cutscene=results=replica_results=reenter=false;throw_at=0;
 mem_write32(0x80000000u,0x47334250u);mem_write32(0x802DB7E8u,4);mem_write32(0x802DB0E0u,0);mem_write32(available(),4u*1024u*1024u);mem_write32(0x802E22A0u-0x6DE0u,heap);mem_write32(0x802E22A0u-0x5CBCu,heap_table);mem_write32(0x802E22A0u-0x5CB8u,1);mem_write32(heap_table,ram_size/2);mem_write32(0x802ABAA0u,engine);mem_write32(0x802ABAC4u,font);mem_write32(engine,0x8021C600u);mem_write32(font,font_source_table);mem_write32(font_source_table+0x2C,0x800AB6A4u);mem_write32(font_source_table+0x34,0x800AB6A4u);mem_write32(font_instance_table+0x14,0x800AB6A4u);
 mem_write32(scene+0x78,root);mem_write32(root,drawable_vtable);mem_write32(root+4,engine+4);mem_write32(root+8,engine);mem_write32(drawable_vtable+0xC,0x8011D164u);mem_write32(drawable_vtable+0x44,0x8011D458u);mem_write32(text_vtable+0x1C,0x800ABBE4u);for(auto [a,op]:functions)mem_write32(a,op);
}
std::string text(uint32_t w){std::string out;for(unsigned i=0;i<128 && mem_read8(w+0x1C+i);++i)out+=char(mem_read8(w+0x1C+i));return out;}
template<class F>void preserved(PPCContext& c,F&& f){const auto cpu=c;std::array<uint8_t,ssne::native_session_hud::stack_bytes> stack;std::memcpy(stack.data(),host_ptr(c.r[1]-stack.size()),stack.size());f();require(!std::memcmp(&c,&cpu,sizeof(c)),"HUD leaked private native CPU changes");require(!std::memcmp(stack.data(),host_ptr(c.r[1]-stack.size()),stack.size()),"HUD leaked private guest-stack changes");}
}
namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};uint32_t guest_ram_size(){return fixture::ram_size;}uint32_t guest_ram_end(){return 0x80000000u+fixture::ram_size;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("MMIO read");}void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("MMIO write");}
void log_message(const std::string&){}uint32_t native_game_scene(){return fixture::current_scene;}bool local_pause_menu_open(){return fixture::pause_flag;}bool local_information_open(){return fixture::info;}bool native_movie_presenting(){return fixture::movie;}bool native_cutscene_presenting(){return fixture::cutscene;}bool native_results_presenting(){return fixture::results;}bool native_replica_results_presenting(){return fixture::replica_results;}float widescreen(){return fixture::wide;}
}
const SSNEFuncEntry g_ssne_funcs[]={
 {0x800AB5ACu,fixture::original<0x800AB5ACu>,"base"},{0x800AB7C8u,fixture::original<0x800AB7C8u>,"font"},{0x800AB994u,fixture::original<0x800AB994u>,"text destructor"},{0x800ABBE4u,fixture::original<0x800ABBE4u>,"text"},{0x800AB728u,fixture::original<0x800AB728u>,"position"},{0x800AB6B0u,fixture::original<0x800AB6B0u>,"show"},{0x800AB6ECu,fixture::original<0x800AB6ECu>,"hide"},{0x80116084u,fixture::original<0x80116084u>,"allocate"},{0x801161ECu,fixture::original<0x801161ECu>,"free"},{0x8011D15Cu,fixture::original<0x8011D15Cu>,"user data"},{0x8011D164u,fixture::original<0x8011D164u>,"drawable destructor"},{0x80166480u,fixture::original<0x80166480u>,"shutdown"},{0x800AB6A4u,fixture::original<0x800AB6A4u>,"font resource"}
};
const size_t g_ssne_func_count=sizeof(g_ssne_funcs)/sizeof(g_ssne_funcs[0]);const uint32_t g_ssne_entry=0;const bool g_ssne_has_game_code=true;
const SSNEFuncEntry* ssne_find_func(uint32_t a){for(const auto& f:g_ssne_funcs)if(f.address==a)return &f;return nullptr;}PPCFunc* ssne_hook_slot(uint32_t a){return &fixture::hooks[a];}void ssne_install_hook(uint32_t a,PPCFunc f){fixture::hooks[a]=f;}void ssne_call(PPCContext& c,uint32_t a){const auto f=ssne_find_func(a);if(!f)throw std::runtime_error("Unknown native HUD callback");f->func(c);}
int main()try{
 using namespace fixture;using namespace ssne;using namespace ssne::native_session_hud;
 initialize();auto c=context();hooks[shutdown_address]=prior;install_native_session_hud();require(installed && previous_shutdown==prior,"Shutdown did not retain prior hook");auto installed_hook=hooks[shutdown_address];install_native_session_hud();require(hooks[shutdown_address]==installed_hook && previous_shutdown==prior,"Shutdown install not idempotent");
 const auto budget=mem_read32(available());reenter=true;preserved(c,[&]{require(update_native_session_hud(c,"Wave 1","00:12 | 4 enemies",true),"Native text initialization failed");});
 require(live.size()==6 && rows.widget[0] && rows.widget[1] && mem_read32(root+0x18),"HUD did not create exactly two native text rows");require(text(rows.widget[0])=="Wave 1" && text(rows.widget[1])=="00:12 | 4 enemies","Wrong native text payload");require(row_visible(rows.widget[0]) && row_visible(rows.widget[1]),"Rows not visible");
 require(mem_read32(rows.widget[0]+0x10)==f2u(.55f) && mem_read32(rows.widget[1]+0x10)==f2u(.45f),"Original native HUD font scales are not the approved compact sizes");
 require(mem_read32(rows.widget[0]+4)==f2u(-300.f) && mem_read32(rows.widget[1]+4)==f2u(-300.f) && mem_read32(rows.widget[0]+8)==f2u(-160.f) && mem_read32(rows.widget[1]+8)==f2u(-140.f),"HUD rows overlap original top-center score/timer band");
 const auto first=rows.widget;const auto count=allocations;calls.clear();preserved(c,[&]{require(update_native_session_hud(c,"Wave 1","00:12 | 4 enemies",true),"Unchanged HUD update failed");});require(calls.empty() && allocations==count,"Unchanged text did native work or allocation");
 wide=4.f/3.f;preserved(c,[&]{require(update_native_session_hud(c,"Wave 2\nJ\366rg","\twatching\0hidden",true),"HUD update failed");});require(text(first[0])=="Wave 2 J\366rg" && text(first[1])==" watching","Control characters escaped sanitization");require(std::abs(u2f(mem_read32(first[0]+4))-(-300.f))<.001f,"Original HUD canvas was expanded twice");
 for(float aspect:{1.f,4.f/3.f,21.f/9.f,4.f,std::numeric_limits<float>::quiet_NaN()}){wide=aspect;calls.clear();preserved(c,[&]{require(update_native_session_hud(c,"Wave 2 J\366rg"," watching",true),"Aspect HUD update failed");});require(mem_read32(first[0]+4)==f2u(-300.f) && mem_read32(first[1]+4)==f2u(-300.f),"Output aspect moved HUD outside original GUI canvas");require(calls.empty(),"Aspect-only change rewrote original HUD transforms");require(mem_read32(first[0]+8)==f2u(-160.f) && mem_read32(first[1]+8)==f2u(-140.f),"Aspect change returned HUD to original timer band");}
 preserved(c,[&]{require(update_native_session_hud(c,std::string(200,'a'),"",true),"Bounded text failed");});require(text(first[0]).size()==96 && !row_visible(first[1]),"Text bound or empty-detail hiding failed");
 for(unsigned kind=0;kind<8;++kind){if(kind==7){mem_write32(0x802DB7E8u,3);mem_write32(0x802DB0E0u,4);}pause_flag=kind==0;info=kind==1;movie=kind==2;cutscene=kind==3;results=kind==4;replica_results=kind==5;preserved(c,[&]{require(update_native_session_hud(c,"shown","hint",kind!=6),"Suppressed HUD update failed");});require(!row_visible(first[0]) && !row_visible(first[1]),"HUD leaked into a native menu/movie/result");pause_flag=info=movie=cutscene=results=replica_results=false;mem_write32(0x802DB7E8u,4);mem_write32(0x802DB0E0u,0);preserved(c,[&]{require(update_native_session_hud(c,"shown","hint",true),"HUD resume failed");});require(rows.widget==first && allocations==count,"Suppression/reentry replaced native objects");}
 // Restoring a local checkpoint can invalidate every C++ cache. Re-adopt only
 // the native child -> user_data -> tagged-wrapper -> child ownership cycle.
 auto saved_ram=ram;auto saved_live=live;const auto saved_next=next;preserved(c,[&]{end_native_session_hud(c);});require(live.empty() && !mem_read32(root+0x18) && mem_read32(available())==budget,"End leaked native allocation/accounting");ram=saved_ram;g_ram=ram.data();live=saved_live;next=saved_next;rows={};const auto old_allocations=allocations;
 preserved(c,[&]{require(update_native_session_hud(c,"Restored","Follow Sam",true),"Checkpoint HUD adoption failed");});require(rows.widget==first && allocations==old_allocations && text(first[0])=="Restored","Checkpoint rebuild duplicated or trusted stale text");
 // Invalid call contexts/signatures must leave all guest RAM and native state.
 for(unsigned kind=0;kind<8;++kind){auto bad=c;uint32_t address=0,saved=0;switch(kind){case 0:bad.r[13]+=4;break;case 1:bad.r[2]+=4;break;case 2:bad.r[1]=0;break;case 3:bad.r[1]|=1;break;case 4:address=0x800ABBE4u;break;case 5:address=root;break;case 6:address=font_source_table+0x2C;break;case 7:address=0x80000000u;break;}if(address){saved=mem_read32(address);mem_write32(address,0);}const auto old=ram;calls.clear();require(!update_native_session_hud(bad,"invalid","invalid",true),"Malformed HUD context accepted");require(ram==old && calls.empty(),"Malformed context changed native state");if(address)mem_write32(address,saved);}
 // A heap handle is bounded by the original descriptor count; it must not
 // be mistaken for a guest pointer or permit a stale allocator descriptor.
 for(unsigned kind=0;kind<4;++kind){const uint32_t address=kind==0?c.r[13]-0x6DE0u:kind==1?c.r[13]-0x5CB8u:kind==2?c.r[13]-0x5CBCu:heap_table;const auto saved=mem_read32(address);mem_write32(address,kind==0?1u:kind==3?ram_size+1u:0u);const auto before=ram;calls.clear();require(!update_native_session_hud(c,"bad heap","bad heap",true),"Invalid heap handle/table accepted");require(ram==before && calls.empty(),"Invalid heap metadata changed native state");mem_write32(address,saved);}
 const auto saved_descriptor=mem_read32(first[0]-20);mem_write32(first[0]-20,heap_table+24);require(!owned(rows.owner,first[0],0),"Reused allocation from another heap accepted");mem_write32(first[0]-20,saved_descriptor);
 // Reused/unowned text wrappers must not be hidden, destructed or freed.
 const auto altered=first[0];mem_write32(altered+0xC8,0);const auto old_bytes=std::vector<uint8_t>(host_ptr(altered),host_ptr(altered)+wrapper_bytes);preserved(c,[&]{end_native_session_hud(c);});require(std::equal(old_bytes.begin(),old_bytes.end(),host_ptr(altered)),"Retired an unowned/reused wrapper");require(live.contains(altered),"Unowned wrapper freed");
 // Restore fixture solely to finish the owned lifecycle and original shutdown.
 ram=saved_ram;g_ram=ram.data();live=saved_live;next=saved_next;rows={};preserved(c,[&]{require(update_native_session_hud(c,"Last wave","",true),"Final HUD adoption failed");});auto shutdown_cpu=c;shutdown_cpu.r[3]=scene;auto expected=shutdown_cpu;expected.r[5]^=0x9876u;expected.r[6]=0x600D600Du;expected.r[8]^=0x55667788u;expected.ps0[3]=123.5;expected.lr=shutdown_address+8;ssne_call(shutdown_cpu,shutdown_address);require(prior_calls==1 && shutdowns==1 && live.empty() && !mem_read32(root+0x18),"Original shutdown/prior hook or HUD lifetime failed");require(!std::memcmp(&shutdown_cpu,&expected,sizeof(expected)),"Cleanup changed original shutdown CPU results");
 initialize();c=context();throw_at=0x800AB7C8u;bool threw=false;preserved(c,[&]{try{update_native_session_hud(c,"failure","failure",true);}catch(const std::runtime_error&){threw=true;}});require(threw && !busy && live.empty() && !mem_read32(root+0x18),"Constructor exception leaked ownership or reentry state");require(mem_read32(available())==budget,"Constructor exception leaked native heap accounting");
 initialize();mem_write32(0x800AB7C8u,0);hooks[shutdown_address]=prior;install_native_session_hud();require(!installed && hooks[shutdown_address]==prior && !previous_shutdown,"Partial PAL signature installed HUD hook");
 std::cout<<checks<<" native session HUD assertions passed: original-font object lifecycle, local checkpoint adoption, text bounds, visibility, ownership guards, prior shutdown, EE and CPU/stack restoration\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
