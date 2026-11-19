// The production full-respawn/preflight implementation, with only original
// native callbacks and the memory backend replaced. No game/ROM is launched.
#include "../runtime/src/native_party.cpp"
#include <iostream>
#include <stdexcept>
namespace {
constexpr uint32_t size=24u*1024u*1024u,actor=0x80400000u,camera=0x80500000u,proxy=0x80600000u,model=0x80601000u;
constexpr uint32_t level=0x80700000u,primary=0x80701000u,manager=0x80702000u;
constexpr uint32_t fresh_mask=0xFFFFFFFEu;
unsigned checks=0,full_calls=0,start_calls=0,segments=0,spheres=0,mask_calls=0;
bool replica=false,blocked=false,throw_full=false,omit_health=false,omit_death=false,omit_camera=false,placement_only=false;
ssne::NativeEntityObserver observer;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void full(PPCContext&);void start(PPCContext&);void segment(PPCContext&);void sphere(PPCContext&);void player_start(PPCContext&);void mask(PPCContext&);
namespace ground_cases { void ground_entry(PPCContext&); void sphere_entry(PPCContext&); }
const SSNEFuncEntry entries[]={{0x800B2950u,full,"full-respawn"},{0x800B2E5Cu,start,"placement-only"},{0x800C04ACu,ground_cases::ground_entry,"actor-move-sphere"},{0x8010B0ACu,player_start,"player-start"},{0x80140214u,segment,"segment"},{0x80140620u,ground_cases::sphere_entry,"sphere"},{0x801527C0u,mask,"body-mask"}};
PPCContext context(){PPCContext c;for(unsigned i=0;i<32;++i){c.r[i]=0xAA000000u+i;c.ps0[i]=i+.25;c.ps1[i]=-double(i)-.5;c.gqr[i%8]=i*27;}c.r[1]=0x817F0000u;c.r[2]=0x802F22A0u;c.r[13]=0x802E22A0u;c.msr=0x8032;c.lr=0x80405678;c.ctr=17;c.fpscr=1234;return c;}
void put_code(uint32_t at,const char* s){auto digit=[](char ch){return ch<='9'?unsigned(ch-'0'):unsigned(ch-'a'+10);};for(unsigned i=0;s[i*2];++i)mem_write8(at+i,uint8_t(digit(s[i*2])*16+digit(s[i*2+1])));}
void fixture(std::vector<uint8_t>& ram){
 std::fill(ram.begin(),ram.end(),0);ssne::g_ram=ram.data();replica=blocked=throw_full=omit_health=omit_death=omit_camera=placement_only=false;full_calls=start_calls=segments=spheres=mask_calls=0;
 ssne::roster=1;ssne::configured=true;ssne::arena=ssne::registry=0;ssne::owner=-1;ssne::occupancy_enabled=true;ssne::occupied_slots=0;ssne::connected_slots=1;
 ssne::pending_spawns.clear();ssne::prepared_spawns.clear();ssne::completed_spawns.clear();ssne::reserved_body_masks.clear();ssne::known_player_actors.fill(0);ssne::force_safe_spawn=false;ssne::probing_spawn=false;ssne::next_spawn_revision=0;
 mem_write32(0x80000000u,0x47334250u);mem_write32(0x802DB8CCu,1);mem_write32(0x802DB7E8u,4);mem_write32(0x802D9048u,actor);
 mem_write32(actor+0x48,0x802121E8u);mem_write32(actor+0x17C,0);mem_write32(actor+0x1C,3);mem_write32(actor+0xCC,f2u(0));mem_write32(actor+0x328,1);mem_write32(actor+0x370,1);mem_write32(actor+0x368,camera);
 mem_write32(camera,0x80210880u);mem_write32(camera+4,actor);mem_write32(camera+0xAC,2);mem_write32(actor+0x148,proxy);mem_write32(proxy+0x10,model);mem_write32(model,0x8021D668u);mem_write32(proxy+0x18,0xFFFFFFFFu);
 mem_write32(0x802121E8u+0x194,0x8009C404u);mem_write32(0x802121E8u+0x19C,0x8009C410u);mem_write32(0x802121E8u+0x1AC,0x8009C44Cu);mem_write32(0x8009C410u,0x38600001u);mem_write32(0x8020E6C4u,f2u(1));
 mem_write32(0x802DACC0u,level);mem_write32(level+0x2A8,primary);mem_write32(0x802ABAA4u,manager);mem_write32(manager,0x8021EB70u);mem_write32(0x8010B0ACu,0x9421FF20u);mem_write32(0x80140214u,0x9421FE60u);mem_write32(0x80140620u,0x9421FDF8u);mem_write32(0x801527C0u,0x9421FFF0u);mem_write32(0x800B2E5Cu,0x9421FFB0u);
 put_code(0x800B2950u,"9421ffa07c0802a6dbe10058bee10034900100647c7f1b787c9e2378801f05fc2c00000041820014386000014809de1538000000901f05fc38000000807f0374901f0328480088253d608021807f0374c3eb1e5c3d20802139610018c0291e60d3e100187d645b78d3eb00044800b08d807f03743880000048008ccd800d962c2c00000340820024c01f06a0807f0374fc20f890fda0001ed9a100288081002c48009b8148000020c01f068c807f0374fc20f890fda0001ed9a100288081002c48009b612c1e00007fe3fb787fa000264bfe94197fe3fb78480048e9388000017fe3fb784800042938000000813f0380901f03707fe3fb78c0290024480011893d2080217fe3fb78c0291e5c480012197fa801204082010c800d962c2c0000004182000c2c000001408200f83d20802b3c8080218069bb6038841e3838a0000048060c9983c300242c1e00004182009c3d208021c01f04a4c3e91e5cfc00f800408100887fc4f37838600001480a70d57c651b783880003b386100084cc63182480a7f1d807f03743881000838a0000148008e716fc080009001002c3d6043303d408021c19f04a491610028c9aa1e68c8010028fc006828fc000018ed8c0028fc0cf800d19f068c40800008d3ff068cc01f068c3861000838800002d01f04a4480a7df94800001c807f03743880003c38a0000148008e59c01f04a4d01f068cc01f068c3d208021807f0374c0291e5cfda0001ed9a100288081002c48009a05800d962c391f061c2c00000040820030380000083d20802d7c0903a639600000392968f8395f04707c0a58ae396b000190090000392900044200fff039600007392800607d6903a638000000900900003929fff04200fff8800d962c2c000000418200142c000001408201187fa80120418200f8807f05e8388000014bfec661831f04787fe3fb78801f04b483df04b87f0003787c1ef0787fc4f37848013c29889f04a07fc5f3787fe3fb7848013e112c03000041820014889f04a07fe3fb7848013331480000107fe3fb7838800001480133213bc000003aff04bc7fe3fb787fc4f3787f05c3783b3e000148013dd12c030000418200641c1e00183b5f04bc3bc000007c77002e7c1b03784bfedd1d7c7d1b787c1ee800408000403b9f047c7c7ad82e7fc4f3784bfed2417c641b782c04ffff418200187c042214807f05e87cdc022e38a000004bfec7a13bde00017c1ee8004180ffcc7f3ecb782c1e000b4081ff7c48000040807f05e8388000004bfec56d807f05e84bfec60948000028807f05e8388000014bfec5557fe3fb783880000248013b317fe3fb783880000148013251388000007fe3fb78480113e9813f00488009038ca86903887c0803a67c7f1a144e800021807f037c388000014bffd0217fe3fb78480137357fe3fb7848002e25807f0174480049b1813f004838a00000388000008009022ca86902287c0803a67c7f1a144e8000217fe3fb7848001e657c641b787fe3fb7848001d9d7fe3fb783880000048001c813d2080213d60802bc0091e5c809f0178d01f0190806bba9c4809d6117fe3fb78388000014800eeb93d2080217fe3fb78c0291e70388000014800f02d38000000806d86e8901f06bc7fe4fb78480038b1806d86e04800387138000001901f032c800100647c0803a6bae10034cbe10058382100604e800020");
 auto c=context();for(unsigned i=0;i<0x4000;++i)mem_write8(c.r[1]-0x4000+i,uint8_t(i*13+29));
 ssne::reserved_body_masks[actor]={proxy,0xFFFFFFFFu};
}
void callback(PPCContext& c){require(!(c.msr&0x8000u),"Native reset ran with interrupts exposed");require(ssne::force_safe_spawn,"Native reset lost safe placement scope");mem_write32(c.r[1]-0x70,0xFACEB00Cu);c.r[27]=0xDEAD1234u;c.ps1[9]=-9876.;c.cr[4]=15;c.lr=0xCAFE0001u;}
void player_start(PPCContext& c){
 ssne::NativePartySpawnScope scope(c);for(unsigned i=0;i<3;++i){mem_write32(c.r[5]+i*4,f2u(i==0?-10.f:i==1?1.f:5.f));mem_write32(c.r[6]+i*4,f2u(0));}scope.prepare();
}
void segment(PPCContext& c){++segments;const auto out=c.r[6];for(unsigned i=0;i<3;++i){mem_write32(out+i*4,i==1?f2u(0):mem_read32(c.r[4]+i*4));mem_write32(out+12+i*4,f2u(i==1?1.f:0.f));}mem_write32(out+32,7);c.r[3]=1;}
void sphere(PPCContext& c){++spheres;for(unsigned i=0;i<3;++i)mem_write32(c.r[6]+i*4,mem_read32(c.r[5]+i*4));if(blocked)mem_write32(c.r[6],f2u(u2f(mem_read32(c.r[6]))+10));c.r[3]=0;}
void start(PPCContext& c){
 ++start_calls;const auto who=c.r[3];require(who==actor,"Placement received another player");
 if(!ssne::native_party_prepare_spawn(c,who,c.r[4]))return;
 const auto point=ssne::prepared_spawns.at(who);for(unsigned i=0;i<3;++i)mem_write32(who+0x20+i*4,f2u(point[i]));
 if(!omit_camera)mem_write32(camera+0xAC,0);mem_write32(proxy+0x18,fresh_mask);ssne::native_party_spawn_completed(who);
}
void full(PPCContext& c){
 ++full_calls;require(c.r[3]==actor && c.r[4]==0,"Full native respawn did not use the original(actor,0) contract");
 const auto who=c.r[3];callback(c);if(throw_full)throw std::runtime_error("deliberate original reset failure");
 c.r[3]=who;c.r[4]=1;start(c);
 if(!placement_only){if(!omit_death)mem_write32(who+0x328,0);mem_write32(who+0x370,0);if(!omit_health)mem_write32(who+0xCC,f2u(100));}
}
void mask(PPCContext& c){++mask_calls;require(c.r[3]==proxy,"Unexpected body-mask callback");mem_write32(proxy+0x18,c.r[4]);}
void expect_cpu_and_stack(const PPCContext& c,const PPCContext& before,const std::vector<uint8_t>& stack){require(!std::memcmp(&c,&before,sizeof c),"Full reset changed caller CPU registers");require(!std::memcmp(ssne::host_ptr(c.r[1]-0x4000),stack.data(),stack.size()),"Full reset leaked scratch stack writes");require(!ssne::force_safe_spawn,"Full reset leaked safe placement scope");}
std::vector<uint8_t> scratch(const PPCContext& c){return {ssne::host_ptr(c.r[1]-0x4000),ssne::host_ptr(c.r[1])};}
void checksum(std::vector<uint8_t>& v){const auto hash=ssne::party_state_hash(v.data(),v.size()-8);for(unsigned i=0;i<8;++i)v[v.size()-8+i]=uint8_t(hash>>(56-i*8));}
#include "native_party_ground_cases.h"
}
const SSNEFuncEntry* ssne_find_func(uint32_t address){for(const auto& e:entries)if(e.address==address)return &e;return nullptr;}
PPCFunc* ssne_hook_slot(uint32_t address){
 if(address==0x800C04ACu)return &ground_cases::ground_hook;
 if(address==0x80140620u)return &ground_cases::sphere_hook;
 throw std::runtime_error("Unexpected hook lookup");
}
void ssne_install_hook(uint32_t address,PPCFunc fn){*ssne_hook_slot(address)=fn;}
void ssne_call(PPCContext& c,uint32_t address){const auto* entry=ssne_find_func(address);if(!entry)throw std::runtime_error("Unexpected original callback "+std::to_string(address));entry->func(c);}
namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return size;}uint32_t guest_ram_end(){return 0x80000000u+size;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO read");}void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO write");}
bool replica_simulation(){return replica;}void log_message(const std::string&){}
bool netplay_world_ready(){return true;}int netplay_local_player(){return 0;}
bool native_results_presenting(){return false;}bool native_cutscene_presenting(){return false;}bool native_movie_presenting(){return false;}
bool local_information_open(){return false;}bool local_pause_menu_open(){return false;}uint16_t injected_buttons(int){return 0;}
[[noreturn]] void fatal(const char* why){throw std::runtime_error(why);}
uint64_t observe_native_entities(NativeEntityObserver fn){observer=std::move(fn);return 1;}void stop_observing_native_entities(uint64_t){}
}
int main(){try{
 std::vector<uint8_t> ram(size);std::string error;
 for(unsigned buttons:{0u,unsigned(ssne::PAD_A|ssne::PAD_R)}){
  fixture(ram);ssne::pads[0].buttons=uint16_t(buttons);auto c=context(),before=c;auto stack=scratch(c);
  require(ssne::native_party_request_respawn(c,actor,error),error.c_str());
  require(full_calls==1 && start_calls==1 && segments>=2 && spheres>=2,"Reset did not run actual safe preflight then one full original lifecycle");
  require(u2f(mem_read32(actor+0xCC))==100 && !mem_read32(actor+0x328) && !mem_read32(actor+0x370) && !mem_read32(camera+0xAC),"Native health/death/camera restoration missing");
  require(ssne::native_party_spawn_revision(actor)==1 && !ssne::native_party_spawn_waiting(actor),"Successful reset not committed as one native spawn");
  require(!ssne::reserved_body_masks.contains(actor),"Same-address native body retained old death visibility mask");
  ssne::occupied_slots=1;ssne::native_party_refresh_occupancy(c);require(mem_read32(proxy+0x18)==fresh_mask && mask_calls==0,"Active refresh restored stale same-proxy death mask over original self-hidden mask");
  require(ssne::pads[0].buttons==buttons,"Full respawn synthesized or consumed Fire/Jump input");expect_cpu_and_stack(c,before,stack);
 }
 for(unsigned failure=0;failure<8;++failure){
  fixture(ram);auto c=context(),before=c;
  if(failure==0)mem_write8(0x800B2950u+0x110,mem_read8(0x800B2950u+0x110)^1);
  if(failure==1)mem_write32(0x802D9048u,actor+0x1000);
  if(failure==2)replica=true;if(failure==3)c.r[13]=0;
  if(failure==4)mem_write32(camera+4,actor+0x1000);
  if(failure==5)ssne::connected_slots=0;if(failure==6)mem_write32(0x802DB8CCu,0);if(failure==7)mem_write32(0x802DB7E8u,3);
  before=c;auto all=ram;
  require(!ssne::native_party_request_respawn(c,actor,error) && !error.empty() && full_calls==0,"Unsupported reset context ran native lifecycle");
  require(ram==all && !std::memcmp(&c,&before,sizeof c),"Rejected full respawn changed RAM or CPU");
 }
 for(unsigned failure=0;failure<4;++failure){
  fixture(ram);auto c=context(),before=c;auto stack=scratch(c);
  placement_only=failure==0;omit_health=failure==1;omit_death=failure==2;omit_camera=failure==3;
  require(!ssne::native_party_request_respawn(c,actor,error),"Placement/partial reset was mistaken for a healthy first-person respawn");
  require(full_calls==1 && ssne::native_party_spawn_revision(actor)==1,"Partial lifecycle negative control did not execute");expect_cpu_and_stack(c,before,stack);
 }
 fixture(ram);auto c=context(),before=c;auto stack=scratch(c);throw_full=true;bool threw=false;
 try{ssne::native_party_request_respawn(c,actor,error);}catch(const std::runtime_error&){threw=true;}
 require(threw && full_calls==1,"Original reset exception was swallowed");expect_cpu_and_stack(c,before,stack);require(!ssne::prepared_spawns.contains(actor),"Failed lifecycle retained a stale prepared spawn");
 fixture(ram);c=context();before=c;stack=scratch(c);blocked=true;
 require(!ssne::native_party_request_respawn(c,actor,error) && full_calls==0,"Blocked safe placement partially restored native player");
 require(ssne::native_party_spawn_waiting(actor) && ssne::pending_spawns.at(actor).full_respawn && u2f(mem_read32(actor+0xCC))==0 && mem_read32(actor+0x328)==1,"Blocked full reset lost pending lifecycle or revived dead player");
 require(ssne::reserved_body_masks.contains(actor),"Unsuccessful preflight discarded original inactive mask state");expect_cpu_and_stack(c,before,stack);
 std::vector<uint8_t> state;require(ssne::native_party_save_state(state,error),error.c_str());require(state[7]==2,"Full pending lifecycle was not checkpointed as v2");
 ssne::pending_spawns.clear();require(ssne::native_party_load_state(state,error) && ssne::pending_spawns.at(actor).full_respawn,"Pending full lifecycle was lost across checkpoint roundtrip");
 auto invalid=state;invalid[7]=1;checksum(invalid);require(!ssne::native_party_load_state(invalid,error) && ssne::pending_spawns.at(actor).full_respawn,"Legacy v1 accepted a new full-respawn flag or rejection mutated state");
 blocked=false;ssne::pending_spawns.at(actor).next={};require(!ssne::native_party_retry_spawn(c,actor),"Unblocked pending full lifecycle did not retire");
 require(full_calls==1 && u2f(mem_read32(actor+0xCC))==100 && !mem_read32(actor+0x328) && !mem_read32(camera+0xAC) && ssne::native_party_spawn_revision(actor)==1,"Deferred retry ran placement only instead of full native respawn");expect_cpu_and_stack(c,before,stack);
 require(ssne::native_party_save_state(state,error),error.c_str());state[7]=1;checksum(state);require(ssne::native_party_load_state(state,error),"Backward-compatible original v1 party state was rejected");
 fixture(ram);c=context();blocked=true;require(!ssne::native_party_request_respawn(c,actor,error),"Cancellation positive control did not defer");
 require(bool(observer),"Pending lifecycle has no entity lifetime observer");observer({ssne::NativeEntityNotice::Event::Removing,actor});require(!ssne::native_party_spawn_waiting(actor),"Removed actor retained a pending full reset");
 ground_cases::check(ram);
 std::cout<<"Native party passed "<<checks<<" assertions covering full respawn, checkpoint v1/v2 and ground collision.\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
