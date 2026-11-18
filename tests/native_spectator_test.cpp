// Real render lease implementation with guest RAM / native lifecycle queries as
// collaborators. The oracle compares the entire RAM and complete caller CPU.
#include "../runtime/src/native_spectator.cpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
constexpr uint32_t size=24u*1024u*1024u,registry=0x80300000u,engine=0x80310000u,views=0x80320000u;
constexpr uint32_t local_actor=0x80400000u,target_actor=0x80401000u,local_camera=0x80500000u,target_camera=0x80501000u;
constexpr uint32_t proxy=0x80600000u,model=0x80601000u,table=0x80201000u,gui=0x80604000u;
constexpr uint32_t pause_mask=0x802E22A0u-0x765Cu,world_pause=0x802E22A0u-0x6AACu;
bool replica_role=false,fail_open=false,fail_close=false;int party_owner=5;unsigned opens=0,closes=0,binds=0,forwarded=0,frontend_opens=0;
PPCFunc hooks[11]{};bool fail_release=false,incomplete_release=false;unsigned flags_released=0,bombs_released=0;
constexpr uint32_t objective=0x80700000u,objective_render=0x80701000u,hud=0x80702000u,held_visual=0x80703000u,held_vtable=0x80203000u,local_hud_root=0x80704000u,scene=0x80705000u,shared_hud_root=0x80706000u;
void original_flag_release(PPCContext&);void original_bomb_release(PPCContext&);void internal_objective(PPCContext&){}
void original_open(PPCContext&);void original_close(PPCContext&);void original_gui_close(PPCContext&);void original_bind(PPCContext&);
void frontend_open(PPCContext& c){++frontend_opens;ssne_call_original(c,0x800F9978u,original_open);}
const SSNEFuncEntry entries[]={{0x800F9978u,original_open,"open"},{0x800F9AACu,original_close,"close"},{0x800F643Cu,original_gui_close,"gui-close"},{0x800F56E0u,original_bind,"bind"},{0x800B4A18u,original_flag_release,"flag-drop"},{0x800B4B28u,original_bomb_release,"bomb-clear"},{0x800808E0u,internal_objective,"flag-world"},{0x80080F5Cu,internal_objective,"flag-reactivate"},{0x80168778u,internal_objective,"bomb-owner"},{0x800B72A0u,internal_objective,"held-visual"},{0x8009C300u,internal_objective,"flag-position"}};
unsigned function_index(uint32_t address){for(unsigned i=0;i<std::size(entries);++i)if(entries[i].address==address)return i;throw std::runtime_error("Unexpected native menu callback");}
bool extended=true,layout_safe=true,world_ready=true;unsigned capacity=8;int local_slot=0;uint8_t occupied=2;
std::array<bool,6> presentation{};unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
uint32_t handle(unsigned i){return 0x80602000u+i*0x100u;}
PPCContext context(){PPCContext c;for(unsigned i=0;i<32;++i){c.r[i]=0xA0000000u+i;c.ps0[i]=i+.25;c.ps1[i]=-double(i)-.5;}c.r[1]=0x817FF000u;c.r[2]=0x802F22A0;c.r[13]=0x802E22A0;c.msr=0x8000;return c;}
void code(uint32_t address,const char* hex){auto digit=[](char ch){return ch<='9'?unsigned(ch-'0'):unsigned(ch-'a'+10);};for(unsigned i=0;hex[i*2];++i)mem_write8(address+i,uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1])));}
void fixture(std::vector<uint8_t>& ram){
    std::fill(ram.begin(),ram.end(),0);ssne::g_ram=ram.data();extended=layout_safe=world_ready=true;capacity=8;local_slot=0;occupied=2;presentation={};replica_role=false;fail_open=fail_close=false;party_owner=5;opens=closes=binds=forwarded=frontend_opens=0;
    hooks[0]=frontend_open; // NativeFrontend owns an earlier open hook.
    fail_release=incomplete_release=false;flags_released=bombs_released=0;
    code(0x800B4A18u,"9421ffe87c0802a6bfa1000c9001001c7c7f1b783bc000017c9d2379408200083bc00000801f06947c1e00004182007c2c1e00004182003438800000386001d948059d59807f03743880005938a0000148006f353c8080217fe3fb7838841fcc480025294800002438800000386001d848059d29807f0698389f00204bfcbe4d7fe3fb7848002805807f03747fc4f37893bf069893df069448007cd17fe3fb7880830694480b37e98001001c7c0803a6bba1000c382100184e800020");
    code(0x800B4B28u,"9421fff07c0802a6bfc10008900100147c7f1b787c9e23794182004038800000386001da48059c65813f06ac3880005a807f037438a00006c0090024d01f06a848006e353c8080217fe3fb7838841fd8480024294800000c7fe3fb784800271d7fe3fb787fc4f378480b3be9807f03746bc40001200400007c80211448007d2593df06a4800100147c0803a6bbc10008382100104e800020");
    code(0x800808E0u,"9421ffd87c0802a6bfa1001c9001002c7c9d23787c7e1b783c8080203861000838846c004cc63182480da2493861000838800002480da015813e00487fa4eb78800900c4a86900c07c0803a67c7e1a144e8000217fc3f3784800062538000000901e00688001002c7c0803a6bba1001c382100284e800020");
    code(0x80080F5Cu,"9421fff07c0802a6bfc10008900100143d20802b7c7e1b788169baa4389e0060812b00008009001ca86900187c0803a67c6b1a144e800021817e0060809e005c812b00008009004ca86900487c0803a67c6b1a144e800021380000007fc3f378900d84d44bfffdf57fc3f3784801b555800100147c0803a6bbc10008382100104e800020");
    code(0x80168778u,"7c8423794182000c906dab4c48000014800dab4c7c00180040820008908dab4c38000001900dab684e800020");
    code(0x800B72A0u,"9421fff07c0802a693e1000c900100147c7f1b78801f01642c000000418200403c608021386321844809a541817f01642c0b000041820020812b000038800003a8690010800900147c6b1a147c0803a64e80002138000000901f0164800100147c0803a683e1000c382100104e800020");
    mem_write32(0x80206C80u+0xC4,0x8009C300u);mem_write32(objective+0x48,0x80206C80u);
    mem_write32(local_actor+0x374,hud);mem_write32(target_actor+0x374,hud+0x1000);mem_write32(held_visual,held_vtable);mem_write32(held_vtable+0x14,0x800B72A0u);
    mem_write32(0x800F9978u,0x9421FFE8u);mem_write32(0x800F9AACu,0x9421FFF0u);mem_write32(0x800F643Cu,0x9421FFF0u);mem_write32(0x800F56E0u,0x38000000u);
    mem_write32(local_actor+0x71C,gui);mem_write16(gui+0x84,0);mem_write32(world_pause,0x12345678u);
    mem_write32(hud,local_hud_root);mem_write32(local_hud_root,0x8021CAB0u);mem_write32(local_hud_root+4,engine+4);mem_write32(local_hud_root+8,engine);mem_write32(local_hud_root+0x1C,0xA5A50004u);
    mem_write32(scene+0x78,shared_hud_root);mem_write32(0x8021CAB0u+0x44,0x8011D458u);mem_write32(0x8011D458u,0x2C040002u);mem_write32(0x8011D4A0u,0xA003001Eu);mem_write32(0x8011D4A4u,0x60000001u);mem_write32(0x8011D4A8u,0xB003001Eu);
    mem_write32(0x80000000u,0x47334250u);mem_write32(0x800A6A8Cu,0x9421FFF8u);mem_write32(0x80210880u+0x14,0x800A5EDCu);
    mem_write32(0x801527C0u,0x9421FFF0u);mem_write32(0x801527DCu,0x93DF0018u);mem_write32(0x8013E814u,0x9421FFF0u);mem_write32(0x8013E82Cu,0x90830030u);
    mem_write32(0x802DB7E8u,4);mem_write32(0x802ABAA0u,engine);mem_write32(engine,0x8021C600u);
    for(unsigned slot=0;slot<2;++slot){const auto actor=slot?target_actor:local_actor,cam=slot?target_camera:local_camera;
        mem_write32(registry+slot*4,actor);mem_write32(actor+0x48,0x802121E8u);mem_write32(actor+0x17C,slot);mem_write32(actor+0x368,cam);mem_write32(actor+0xCC,f2u(100.f));
        mem_write32(cam,0x80210880u);mem_write32(cam+4,actor);mem_write32(views+0xDC+slot*76,cam);mem_write32(views+0xDC+slot*76+0x30,1);
        for(unsigned i=0;i<12;++i){const float value=i%5==0?1.f:i%4==3?float(slot*20+i):0.f;mem_write32(cam+0x24+i*4,f2u(value));mem_write32(cam+0x54+i*4,f2u(value+float(slot==0?100:0)));}
        mem_write32(cam+0x84,f2u(slot?.15f:.5f));mem_write32(cam+0x88,f2u(slot?.25f:.7f));mem_write32(cam+0xAC,slot?0:2);
    }
    mem_write32(target_actor+0x148,proxy);mem_write32(proxy+0x10,model);mem_write32(model,0x8021D668u);mem_write32(proxy+0x18,0xABCDEF00u);
    mem_write32(table+0xD4,0x8013E814u);
    for(unsigned i=0;i<4;++i){mem_write32(proxy+i*4,handle(i));mem_write32(handle(i),table);mem_write32(handle(i)+0x30,0x11000000u+i);}
}
void put_expected(std::vector<uint8_t>& ram,uint32_t at,uint32_t value){for(unsigned i=0;i<4;++i)ram[(at&0x3FFFFFFu)+i]=uint8_t(value>>(24-i*8));}
void callback_scope(const PPCContext& c){require(!(c.msr&0x8000u) && party_owner==int(c.r[3]==target_actor?1:0),"Menu callback leaked interrupt/owner context");mem_write32(c.r[1]-32,0xDEADBEEFu);}
void original_open(PPCContext& c){
    ++opens;callback_scope(c);require(c.r[3]==gui && c.r[4]==0x129 && c.r[5]==0x134 && c.r[6]==c.r[13]-0x53A0u,"Original pause GUI arguments changed");
    require((mem_read32(pause_mask)&1)!=0,"Original pause open would run shared-world pause head");
    if(fail_open)throw std::runtime_error("deliberate open failure");mem_write32(gui+0x7C,0x129);c.r[18]=0xFA17u;
}
void original_gui_close(PPCContext& c){++closes;callback_scope(c);require(c.r[3]==gui && (mem_read32(pause_mask)&1),"Local GUI close lost original owner bit");if(fail_close)throw std::runtime_error("deliberate close failure");mem_write32(gui+0x7C,0);c.r[9]=0xFA17u;}
void original_close(PPCContext& c){++forwarded;require(c.r[3]!=gui || !ssne::native_spectator_menu_open(),"Owned observer used shared-world pause close");}
void original_bind(PPCContext& c){++binds;callback_scope(c);require(c.r[3]==gui && c.r[4]==0,"Local GUI bound to wrong controller");mem_write16(gui+0x84,uint16_t(c.r[4]));}
void original_flag_release(PPCContext& c){
    ++flags_released;callback_scope(c);require((c.r[3]==local_actor || c.r[3]==target_actor) && c.r[4]==0,"Flag release changed original arguments");
    if(fail_release)throw std::runtime_error("deliberate objective failure");if(incomplete_release)return;
    const auto actor=c.r[3],flag=mem_read32(actor+0x698);for(unsigned i=0;i<3;++i)mem_write32(flag+0x20+i*4,mem_read32(actor+0x20+i*4));
    mem_write32(flag+0x60,objective_render);mem_write32(actor+0x694,0);mem_write32(actor+0x698,0);mem_write32(actor+0x164,0);mem_write32(c.r[13]-0x5498u,1);c.r[27]=0xFA17u;
}
void original_bomb_release(PPCContext& c){
    ++bombs_released;callback_scope(c);require((c.r[3]==local_actor || c.r[3]==target_actor) && c.r[4]==0,"Bomb release changed original arguments");
    if(fail_release)throw std::runtime_error("deliberate objective failure");if(incomplete_release)return;
    mem_write32(c.r[3]+0x6A4,0);mem_write32(c.r[3]+0x164,0);mem_write32(c.r[13]-0x54B4u,0);mem_write32(c.r[13]-0x5498u,1);c.r[29]=0xFA17u;
}
}
const SSNEFuncEntry* ssne_find_func(uint32_t address){for(const auto& entry:entries)if(entry.address==address)return &entry;return nullptr;}
PPCFunc* ssne_hook_slot(uint32_t address){return &hooks[function_index(address)];}
void ssne_install_hook(uint32_t address,PPCFunc hook){*ssne_hook_slot(address)=hook;}
void ssne_call(PPCContext& c,uint32_t address){const auto index=function_index(address);if(!SSNEOriginalCall::consume(c,address) && hooks[index])hooks[index](c);else entries[index].func(c);}

namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_end(){return 0x80000000u+size;}uint32_t guest_ram_size(){return size;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO read");}void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO write");}
uint32_t native_game_scene(){return scene;}bool replica_simulation(){return replica_role;}int native_party_owner(){return party_owner;}void native_party_owner_set(int owner){party_owner=owner;}
bool netplay_world_ready(){return world_ready;}int netplay_local_player(){return local_slot;}
unsigned native_party_capacity(){return capacity;}bool native_party_extended(){return extended;}uint32_t native_party_registry(){return registry;}
uint32_t native_party_view_engine(uint32_t e){return extended?views:e;}bool native_party_render_layout_safe(uint32_t e){return layout_safe && e==engine;}
bool native_party_actor_occupied(uint32_t actor){if(actor==local_actor)return occupied&1;if(actor==target_actor)return occupied&2;return false;}
bool native_results_presenting(){return presentation[0];}bool native_replica_results_presenting(){return presentation[1];}bool native_cutscene_presenting(){return presentation[2];}
bool native_movie_presenting(){return presentation[3];}bool local_information_open(){return presentation[4];}bool local_pause_menu_open(){return presentation[5] || native_spectator_menu_open();}
}
int main(){try{
    std::vector<uint8_t> ram(size);std::string error;fixture(ram);auto c=context();const auto cpu=c;const auto before=ram;auto expected=ram;
    for(unsigned i=0;i<12;++i){const auto value=mem_read32(target_camera+0x24+i*4);put_expected(expected,local_camera+0x24+i*4,value);put_expected(expected,local_camera+0x54+i*4,value);}
    for(unsigned offset:{0x84u,0x88u,0xACu})put_expected(expected,local_camera+offset,mem_read32(target_camera+offset));
    put_expected(expected,local_hud_root+0x1C,0xA5A50005u);put_expected(expected,proxy+0x18,0);for(unsigned i=0;i<4;++i)put_expected(expected,handle(i)+0x30,0);
    {ssne::NativeSpectatorView view;require(view.begin(c,0,1,error),error.c_str());require(view.active() && view.target()==1,"Lease did not expose selected target");require(ram==expected,"Render scope changed RAM outside visual camera/body masks");require(!std::memcmp(&c,&cpu,sizeof(c)),"Render scope changed caller CPU");
     const auto mask=ssne::native_spectator_filter_render_mask(0xFFFFFFFFu);require(!(mask&(1u<<8)) && !(mask&(1u<<16)) && (mask&1) && (mask&(1u<<24)),"Watch mask hid world/shared GUI or retained own weapon/HUD");
     ssne::NativeSpectatorView nested;require(!nested.begin(c,0,1,error) && ram==expected,"Nested lease changed live presentation");
     require(view.end(error) && ram==before,"Render restore did not retain heterogeneous original handle masks");require(ssne::native_spectator_filter_render_mask(0xFFFFFFFFu)==0xFFFFFFFFu,"Global watch mask leaked after lease");}
    fixture(ram);const auto waiting=ram;{auto watching=ram;put_expected(watching,local_hud_root+0x1C,0xA5A50005u);ssne::NativeSpectatorView view;require(view.begin(c,0,ssne::no_spectator_target,error),"Waiting spectator view rejected");require(ram==watching,"Waiting view changed actor/camera or shared GUI outside owned HUD");require(view.end(error) && ram==waiting,"Waiting lease changed RAM");}
    fixture(ram);mem_write32(views+0xDC,0);mem_write32(views+0xDC+0x30,0);const auto retired=ram;{ssne::NativeSpectatorView view;require(view.begin(c,0,1,error),"Retired local world view was not borrowed for watching");require(mem_read32(views+0xDC)==local_camera && mem_read32(views+0xDC+0x30)==1 && (view.filter_render_mask(0)&1),"Borrowed observer world view stayed inactive");require(!ssne::native_party_actor_occupied(local_actor),"World view reactivated spectator gameplay body");require(view.end(error) && ram==retired,"Retired world view did not restore exactly");}
    // A solo eliminated host has no living camera target. Rendering borrows
    // only its own retired viewport/HUD and must not revive or move the body.
    fixture(ram);c=context();capacity=1;occupied=0;mem_write32(registry+4,0);mem_write32(local_actor+0xCC,f2u(0.f));mem_write32(local_actor+0x328,1);mem_write32(local_actor+0x370,1);
    mem_write32(views+0xDC,0);mem_write32(views+0xDC+0x30,0);const auto solo_dead=ram;const auto solo_cpu=c;
    {ssne::NativeSpectatorView view;for(unsigned frame=0;frame<3;++frame){
       require(view.begin(c,0,ssne::no_spectator_target,error),"Solo dead host lost its no-target world view");
       require(mem_read32(views+0xDC)==local_camera && mem_read32(views+0xDC+0x30)==1 && !ssne::native_party_actor_occupied(local_actor),"Solo watch presentation reactivated gameplay or omitted world camera");
       require(mem_read32(local_actor+0xCC)==f2u(0.f) && mem_read32(local_actor+0x328)==1 && mem_read32(local_actor+0x370)==1 && mem_read32(local_camera+0xAC)==2,"No-target view changed native death or camera state");
       require(view.end(error) && ram==solo_dead && !std::memcmp(&c,&solo_cpu,sizeof(c)),"Repeated no-target render left stale camera, viewport, HUD, body or CPU state");}
     // The integration ends/drains the render lease BEFORE the original full
     // respawn. Emulate its externally owned state writes to the SAME camera;
     // a later render-end/destructor must not roll those new values back.
     mem_write32(local_actor+0xCC,f2u(100.f));mem_write32(local_actor+0x328,0);mem_write32(local_actor+0x370,0);mem_write32(local_camera+0xAC,0);
     for(unsigned i=0;i<12;++i){const auto value=f2u(float(i%5==0?1:i%4==3?50+i:0));mem_write32(local_camera+0x24+i*4,value);mem_write32(local_camera+0x54+i*4,value);}
     mem_write32(local_camera+0x84,0);mem_write32(views+0xDC,local_camera);mem_write32(views+0xDC+0x30,1);occupied=1;const auto respawned=ram;
     require(view.end(error) && ram==respawned,"Late render end overwrote a same-camera original respawn after lease termination");
     require(!view.begin(c,0,ssne::no_spectator_target,error) && ram==respawned,"Respawned playing host retained spectator rendering or lost native HUD");}
    fixture(ram);extended=false;capacity=4;mem_write32(engine+0xDC,local_camera);mem_write32(engine+0xDC+0x30,1);const auto classic=ram;{ssne::NativeSpectatorView view;require(view.begin(c,0,1,error),"Four-player native view rejected");const auto mask=ssne::native_spectator_filter_render_mask(0xFFFFFFFFu);require(!(mask&(1u<<6)) && !(mask&(1u<<10)) && (mask&1) && (mask&(1u<<14)),"Four-player watch mask changed shared GUI");require(view.end(error) && ram==classic,"Four-player render restore failed");}
    for(unsigned test=0;test<23;++test){fixture(ram);c=context();switch(test){
      case 0:mem_write32(0x80000000u,0);break;case 1:occupied|=1;break;case 2:mem_write32(target_actor+0x328,1);break;case 3:mem_write32(target_actor+0xCC,0x7FC00000u);break;
      case 4:mem_write32(target_camera+0x24,0);break;case 5:mem_write32(table+0xD4,0);break;case 6:world_ready=false;break;case 7:layout_safe=false;break;case 8:mem_write32(engine,0);break;
      case 9:mem_write32(views+0xDC,0x1234u);break;case 10:mem_write32(target_actor+0x368,local_camera);break;case 11:mem_write16(table+0xD0,4);break;case 12:mem_write32(0x800A6A8Cu,0);break;
      case 13:c.r[13]=0;break;case 14:local_slot=1;break;case 15:mem_write32(target_camera+4,local_actor);break;case 16:occupied=0;break;
      default:presentation[test-17]=true;break;}
      const auto unchanged=ram;const auto registers=c;ssne::NativeSpectatorView view;require(!view.begin(c,0,1,error),"Invalid transition/schema admitted spectator view");require(ram==unchanged && !std::memcmp(&c,&registers,sizeof(c)),"Rejected view changed guest state");}
    fixture(ram);{ssne::NativeSpectatorView view;const auto unchanged=ram;require(!view.begin(c,0,0,error) && !view.begin(c,0,9,error) && ram==unchanged,"Invalid/self target accepted");}
    fixture(ram);c=context();const auto unwind=ram;bool entered_unwind=false;try{ssne::NativeSpectatorView view;require(view.begin(c,0,1,error),"Unwind lease rejected");entered_unwind=true;throw std::runtime_error("expected unwind");}catch(const std::runtime_error& e){require(std::string(e.what())=="expected unwind","Unwind fixture failed before its deliberate exception");}require(entered_unwind && ram==unwind,"RAII exception unwind did not restore full RAM");
    fixture(ram);{ssne::NativeSpectatorView view;require(view.begin(c,0,1,error),"Stale-camera lease rejected");mem_write32(local_actor+0x368,0x80502000u);require(!view.end(error),"Retired camera lifetime accepted");require(mem_read32(proxy+0x18)==0xABCDEF00u,"A live teammate body remained hidden when observer camera retired");for(unsigned i=0;i<4;++i)require(mem_read32(handle(i)+0x30)==0x11000000u+i,"Independent body-mask restore lost native state");}
    for(unsigned invalid=0;invalid<6;++invalid){fixture(ram);c=context();switch(invalid){case 0:mem_write32(hud,0);break;case 1:mem_write32(local_hud_root,0);break;case 2:mem_write32(local_hud_root+8,engine+4);break;case 3:mem_write32(scene+0x78,local_hud_root);break;case 4:mem_write32(hud+0x1000,local_hud_root);break;case 5:mem_write32(0x8011D4A4u,0);break;}const auto initial=ram;ssne::NativeSpectatorView view;
      require(!view.begin(c,0,1,error) && ram==initial,"Unowned/shared/foreign/revision-mismatched HUD was hidden");}
    fixture(ram);c=context();{mem_write32(local_hud_root+0x1C,0xA5A50005u);const auto hidden=ram;ssne::NativeSpectatorView view;require(view.begin(c,0,1,error) && (mem_read32(local_hud_root+0x1C)&1u),"Existing hidden HUD was unhidden while watching");require(view.end(error) && ram==hidden,"HUD lease lost original flags/anchor bits");}
    fixture(ram);c=context();{ssne::NativeSpectatorView view;require(view.begin(c,0,1,error),"HUD independent restore setup failed");mem_write32(local_actor+0x368,0x80502000u);require(!view.end(error) && mem_read32(local_hud_root+0x1C)==0xA5A50004u,"Live HUD stayed hidden when only observer camera was retired");}
    fixture(ram);c=context();{ssne::NativeSpectatorView view;require(view.begin(c,0,1,error),"HUD stale-binding setup failed");mem_write32(hud,shared_hud_root);require(!view.end(error) && mem_read32(local_hud_root+0x1C)==0xA5A50005u && mem_read32(proxy+0x18)==0xABCDEF00u,"Stale HUD was written or live target body restore skipped");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=false;const auto initial=ram;const auto saved_cpu=c;
      pad.buttons=ssne::PAD_START;require(menu.update(c,1,0,pad,true,consume,error) && consume && ssne::native_spectator_menu_open(),"Observer Start did not open local original Pause");
      auto open_expected=initial;put_expected(open_expected,pause_mask,1);put_expected(open_expected,gui+0x7C,0x129);
      require(ram==open_expected && !std::memcmp(&c,&saved_cpu,sizeof(c)) && party_owner==5,"Pause open changed world/other GUI, stack, caller CPU or owner");
      require(opens==1 && frontend_opens==1 && forwarded==0 && mem_read32(world_pause)==0x12345678u,"Pause bypassed frontend or entered global world callback");
      require(menu.update(c,1,0,pad,true,consume,error) && opens==1 && closes==0 && consume,"Held Start immediately closed observer Pause");
      pad.buttons=0;require(menu.update(c,1,0,pad,true,consume,error) && !consume && ssne::native_spectator_menu_open(),"Start release closed observer Pause");
      pad.buttons=ssne::PAD_START;require(menu.update(c,1,0,pad,true,consume,error) && consume && !ssne::native_spectator_menu_open() && closes==1,"Second Start did not close only local GUI");
      require(ram==initial && !std::memcmp(&c,&saved_cpu,sizeof(c)) && party_owner==5,"Pause close did not restore whole RAM/CPU");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=false;mem_write32(pause_mask,4);mem_write16(gui+0x84,1);const auto initial=ram;
      pad.buttons=ssne::PAD_START;require(menu.update(c,2,0,pad,true,consume,error) && binds==1 && mem_read32(pause_mask)==5,"Observer did not bind native GUI or preserve other pause owner");
      auto mouse=c;mouse.r[3]=gui;ssne_call(mouse,0x800F9AACu);require(closes==1 && forwarded==0 && mem_read32(pause_mask)==4 && !ssne::native_spectator_menu_open(),"Mouse/native Continue entered shared-world cleanup");
      auto expected=initial;expected[(gui+0x84)&0x3FFFFFFu]=0;expected[(gui+0x85)&0x3FFFFFFu]=0;require(ram==expected,"Local GUI close changed another GUI/world byte");
      auto other=c;other.r[3]=gui+0x1000;ssne_call(other,0x800F9AACu);require(forwarded==1,"Unrelated native Pause close hook was swallowed");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=false;const auto initial=ram;pad.buttons=ssne::PAD_START;
      require(menu.update(c,3,0,pad,true,consume,error),"Return cleanup setup failed");occupied|=1;pad.buttons=0;
      require(menu.update(c,3,0,pad,false,consume,error) && !ssne::native_spectator_menu_open() && ram==initial,"Returning player left an owned watcher Pause open");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=false;pad.buttons=ssne::PAD_START;
      require(menu.update(c,4,0,pad,true,consume,error),"Epoch cleanup setup failed");mem_write32(local_actor+0x71C,gui+0x1000);const auto retired_gui=ram;
      require(menu.update(c,5,0,pad,false,consume,error) && !ssne::native_spectator_menu_open() && ram==retired_gui,"Retired actor/epoch cleanup wrote old GUI or world state");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=true;replica_role=true;pad.buttons=ssne::PAD_START;const auto initial=ram;
      require(menu.update(c,6,0,pad,true,consume,error) && !consume && !ssne::native_spectator_menu_open() && ram==initial && opens==0,"Client observer stole Start from existing replica Pause path");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=false;mem_write32(local_actor+0xCC,0);occupied=0;pad.buttons=ssne::PAD_START;
      require(menu.update(c,7,0,pad,true,consume,error) && ssne::native_spectator_menu_open(),"All-dead/no-target observer could not open original Pause");pad.buttons=0;menu.update(c,7,0,pad,true,consume,error);pad.buttons=ssne::PAD_START;require(menu.update(c,7,0,pad,true,consume,error),"All-dead observer could not close Pause");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=false;fail_open=true;pad.buttons=ssne::PAD_START;const auto initial=ram;const auto saved=c;
      require(!menu.update(c,8,0,pad,true,consume,error) && error=="deliberate open failure" && ram==initial && !std::memcmp(&c,&saved,sizeof(c)) && party_owner==5 && !ssne::native_spectator_menu_open(),"Throwing Pause open leaked native stack, CPU, pause bit or owner");}
    fixture(ram);c=context();{ssne::NativeSpectatorMenu menu;ssne::PadState pad{};bool consume=false;pad.buttons=ssne::PAD_START;require(menu.update(c,9,0,pad,true,consume,error),"Close exception setup failed");pad.buttons=0;menu.update(c,9,0,pad,true,consume,error);fail_close=true;pad.buttons=ssne::PAD_START;const auto initial=ram;const auto saved=c;
      require(!menu.update(c,9,0,pad,true,consume,error) && error=="deliberate close failure" && ram==initial && !std::memcmp(&c,&saved,sizeof(c)) && party_owner==5 && ssne::native_spectator_menu_open(),"Throwing Pause close lost local ownership or guest state");fail_close=false;pad.buttons=0;require(menu.update(c,9,0,pad,false,consume,error),"Failed close could not recover on return");}
    for(unsigned invalid=0;invalid<4;++invalid){fixture(ram);c=context();ssne::NativeSpectatorMenu menu;ssne::PadState pad{};pad.buttons=ssne::PAD_START;bool consume=false;
      if(invalid==0)c.r[1]=0x80001000u;if(invalid==1)mem_write32(0x800F9978u,0);if(invalid==2)mem_write32(local_actor+0x71C,0);if(invalid==3)occupied|=1;const auto initial=ram;
      require(!menu.update(c,10,0,pad,true,consume,error) && ram==initial && !ssne::native_spectator_menu_open(),"Invalid spectator GUI/schema admitted or mutated Pause");}
    fixture(ram);c=context();{mem_write32(0x800B4A18u,0);mem_write32(local_actor+0x698,0x1234);const auto initial=ram;const auto saved=c;
      require(ssne::release_native_spectator_objectives(c,local_actor,error) && ram==initial && !std::memcmp(&c,&saved,sizeof(c)) && !flags_released && !bombs_released,"Coop/no-objective transition depended on irrelevant objective fields");}
    for(unsigned remote=0;remote<2;++remote){fixture(ram);c=context();const auto actor=remote?target_actor:local_actor;mem_write32(0x802DB8CCu,3);mem_write32(actor+0x694,1);mem_write32(actor+0x698,objective);mem_write32(actor+0x164,held_visual);
      for(unsigned i=0;i<3;++i)mem_write32(actor+0x20+i*4,f2u(float(i*7+2)));const auto initial=ram;const auto saved=c;auto expected=ram;
      put_expected(expected,actor+0x694,0);put_expected(expected,actor+0x698,0);put_expected(expected,actor+0x164,0);put_expected(expected,objective+0x60,objective_render);put_expected(expected,c.r[13]-0x5498u,1);for(unsigned i=0;i<3;++i)put_expected(expected,objective+0x20+i*4,mem_read32(actor+0x20+i*4));
      require(ssne::release_native_spectator_objectives(c,actor,error) && flags_released==1 && bombs_released==0,"Flag carrier did not use original drop");
      require(ram==expected && !std::memcmp(&c,&saved,sizeof(c)) && party_owner==5,"Flag drop touched other inventory/pose/world, stack, caller CPU or owner");
      require(ssne::release_native_spectator_objectives(c,actor,error) && flags_released==1 && ram==expected,"Repeated flag cleanup called original drop twice");}
    fixture(ram);c=context();{mem_write32(0x802DB8CCu,4);mem_write32(local_actor+0x6A4,1);mem_write32(local_actor+0x164,held_visual);mem_write32(c.r[13]-0x54B4u,local_actor);const auto saved=c;auto expected=ram;
      put_expected(expected,local_actor+0x6A4,0);put_expected(expected,local_actor+0x164,0);put_expected(expected,c.r[13]-0x54B4u,0);put_expected(expected,c.r[13]-0x5498u,1);
      require(ssne::release_native_spectator_objectives(c,local_actor,error) && bombs_released==1 && !flags_released && ram==expected && !std::memcmp(&c,&saved,sizeof(c)) && party_owner==5,"Bomb carrier did not release native owner/held visual without unrelated writes");}
    for(unsigned invalid=0;invalid<15;++invalid){fixture(ram);c=context();mem_write32(0x802DB8CCu,3);mem_write32(local_actor+0x694,1);mem_write32(local_actor+0x698,objective);mem_write32(local_actor+0x164,held_visual);
      switch(invalid){case 0:replica_role=true;break;case 1:c.r[1]=0;break;case 2:mem_write32(registry,target_actor);break;case 3:mem_write32(0x802DB8CCu,2);break;case 4:mem_write32(local_actor+0x6A4,1);break;case 5:mem_write32(local_actor+0x694,2);break;case 6:mem_write32(local_actor+0x698,0);break;case 7:mem_write32(objective+0x48,0);break;case 8:mem_write32(local_actor+0x374,0);break;case 9:mem_write32(0x800B4A18u+0x44,0);break;case 10:mem_write32(0x800808E0u+0x50,0);break;case 11:mem_write32(held_vtable+0x14,0);break;case 12:mem_write32(local_actor+0x20,0x7FC00000);break;case 13:world_ready=false;break;case 14:mem_write32(0x802DB7E8u,3);break;}
      const auto initial=ram;const auto saved=c;require(!ssne::release_native_spectator_objectives(c,local_actor,error) && ram==initial && !std::memcmp(&c,&saved,sizeof(c)) && flags_released==0 && bombs_released==0,"Invalid/stale objective carrier modified guest state or entered original release");}
    for(unsigned invalid=0;invalid<2;++invalid){fixture(ram);c=context();mem_write32(0x802DB8CCu,4);mem_write32(local_actor+0x6A4,1);mem_write32(c.r[13]-0x54B4u,invalid?local_actor:target_actor);if(invalid)mem_write32(0x80168778u+8,0);const auto initial=ram;
      require(!ssne::release_native_spectator_objectives(c,local_actor,error) && ram==initial && !bombs_released,"Mismatched bomb owner/revision entered native release");}
    for(unsigned failure=0;failure<2;++failure){fixture(ram);c=context();mem_write32(0x802DB8CCu,4);mem_write32(local_actor+0x6A4,1);mem_write32(c.r[13]-0x54B4u,local_actor);fail_release=failure==0;incomplete_release=failure==1;const auto initial=ram;const auto saved=c;
      require(!ssne::release_native_spectator_objectives(c,local_actor,error) && ram==initial && !std::memcmp(&c,&saved,sizeof(c)) && party_owner==5 && bombs_released==1,"Original objective exception/incomplete release was accepted or leaked private stack/CPU/owner");}
    std::cout<<"Native spectator render lease passed "<<checks<<" whole-RAM/CPU, guarded camera, visual-mask, local Pause, objective-release, HUD ownership, UI and lifetime assertions\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
