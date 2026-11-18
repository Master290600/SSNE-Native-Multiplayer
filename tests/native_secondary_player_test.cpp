// The real NativeParty TU and real hook-bypass token; only guest RAM and the
// recompiled predicate entry are collaborators. No game data is required.
#include "../runtime/src/native_party.cpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t query=0x800B6630u,caller=0x801511B8u,ram_size=24u*1024u*1024u;
constexpr uint32_t reg=0x80300000u,first=0x80400000u;
bool replica=false,entry_available=true,recurse=false,world_ready=true;int local_player=0;
std::array<bool,5> presentation{};
unsigned installs=0,original_calls=0,previous_calls=0,assertions=0;
std::map<uint32_t,PPCFunc> hooks;
void require(bool value,const char* why){++assertions;if(!value)throw std::runtime_error(why);}
void unused(PPCContext&){throw std::runtime_error("Unexpected unrelated original call");}
void predicate_original(PPCContext& ctx){
    if(!SSNEOriginalCall::consume(ctx,query) && *ssne_hook_slot(query)){
        (*ssne_hook_slot(query))(ctx);return;
    }
    ++original_calls;
    if(recurse){recurse=false;const auto lr=ctx.lr,actor=ctx.r[3];ctx.lr=0x80100000u;predicate_original(ctx);ctx.lr=lr;ctx.r[3]=actor;}
    ctx.r[0]=mem_read32(ctx.r[3]+0x340u);
    cr_set_cmp(ctx,0,int32_t(ctx.r[0])<0,int32_t(ctx.r[0])>0,ctx.r[0]==0);
    if(ctx.r[0]){
        cr_set_cmp(ctx,0,int32_t(ctx.r[0])<3,int32_t(ctx.r[0])>3,ctx.r[0]==3);
        if(ctx.r[0]!=3){
            ctx.r[0]=mem_read32(ctx.r[3]+0x370u);
            cr_set_cmp(ctx,0,int32_t(ctx.r[0])<0,int32_t(ctx.r[0])>0,ctx.r[0]==0);
            if(!ctx.r[0]){ctx.r[3]=1;return;}
        }
    }
    ctx.r[3]=0;
}
void previous_hook(PPCContext& ctx){++previous_calls;ctx.r[9]^=0x12345678u;ssne_call_original(ctx,query,predicate_original);}
bool same_cpu(const PPCContext& a,const PPCContext& b){
    return !std::memcmp(a.r,b.r,sizeof(a.r)) && !std::memcmp(a.ps0,b.ps0,sizeof(a.ps0)) &&
        !std::memcmp(a.ps1,b.ps1,sizeof(a.ps1)) && !std::memcmp(a.cr,b.cr,sizeof(a.cr)) &&
        a.lr==b.lr && a.ctr==b.ctr && a.xer_so==b.xer_so && a.xer_ov==b.xer_ov && a.xer_ca==b.xer_ca &&
        a.fpscr==b.fpscr && !std::memcmp(a.gqr,b.gqr,sizeof(a.gqr)) && a.hid2==b.hid2 && a.msr==b.msr &&
        a.srr0==b.srr0 && a.srr1==b.srr1 && a.dec==b.dec && !std::memcmp(a.sprg,b.sprg,sizeof(a.sprg));
}
PPCContext context(unsigned slot=2){
    PPCContext ctx;
    for(unsigned i=0;i<32;++i){ctx.r[i]=0x88000000u+i;ctx.ps0[i]=u2d(0x7FF8000000000010ull+i);ctx.ps1[i]=-double(i)-.125;}
    for(unsigned i=0;i<8;++i){ctx.cr[i]=uint8_t(i+1);ctx.gqr[i]=0x01020304u+i;}
    for(unsigned i=0;i<4;++i)ctx.sprg[i]=0xAABBCCDDu+i;
    ctx.r[1]=0x817FF000u;ctx.r[3]=first+slot*0x1000u;ctx.r[13]=0x802E22A0u;ctx.lr=caller;ctx.ctr=0x12345678u;
    ctx.xer_so=1;ctx.xer_ov=1;ctx.xer_ca=1;ctx.fpscr=0x87654321u;ctx.hid2=0xABCDEF00u;
    ctx.msr=0x2032u;ctx.srr0=0x80001234u;ctx.srr1=0x99887766u;ctx.dec=123;
    return ctx;
}
void write_signature(){
    // Independent PAL instruction bytes, not the production signature array.
    constexpr uint32_t words[]={0x80030340,0x2C000000,0x41820018,0x2C000003,0x41820010,0x80030370,
                               0x2C000000,0x4182000C,0x38600000,0x4E800020,0x38600001,0x4E800020};
    for(unsigned i=0;i<std::size(words);++i)mem_write32(query+i*4,words[i]);
}
void fixture(){
    replica=false;local_player=0;world_ready=true;presentation.fill(false);
    ssne::pending_spawns.clear();ssne::prepared_spawns.clear();
    ssne::native_party_configure(8);ssne::arena=0x80500000u;ssne::registry=reg;
    mem_write32(0x80000000u,0x47334250u);mem_write32(ssne::game_address(0x802DB0E0u),4);
    mem_write32(ssne::game_address(0x802DB0E4u),0);mem_write32(ssne::game_address(0x802DB1BCu),1);
    for(unsigned slot=0;slot<8;++slot){const auto actor=first+slot*0x1000u;
        mem_write32(reg+slot*4,actor);mem_write32(actor+0x48,0x802121E8u);mem_write32(actor+0x17C,slot);
        mem_write32(actor+0x340,1);mem_write32(actor+0x370,0);
    }
    ssne::native_party_set_occupancy(3,3);
}
void client_fixture(unsigned local=1){
    fixture();replica=true;local_player=int(local);
    const auto members=uint8_t(1u|(1u<<local));ssne::native_party_set_occupancy(members,members);
}
void expect(PPCContext input,bool skip,const char* why,const std::vector<uint8_t>& ram){
    const auto before=ram;const auto n=original_calls,p=previous_calls;
    const auto movement=mem_read32(input.r[3]+0x340u);
    const bool original_result=movement!=0 && movement!=3 && mem_read32(input.r[3]+0x370u)==0;
    auto expected=input;
    if(skip)expected.r[3]=0;
    else {expected.r[9]^=0x12345678u;ssne_call_original(expected,query,predicate_original);--original_calls;}
    predicate_original(input);
    require(same_cpu(input,expected),why);
    require(input.r[3]==unsigned(!skip && original_result),"Independent predicate boolean oracle differs");
    require(original_calls==n+unsigned(!skip) && previous_calls==p+unsigned(!skip),"Wrong original/previous-hook count");
    require(ram==before,"Read-only predicate or filter changed guest RAM");
}
}
namespace ssne {
uint8_t* g_ram=nullptr;uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return ram_size;}uint32_t guest_ram_end(){return 0x80000000u+ram_size;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("Unexpected MMIO read");}
void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("Unexpected MMIO write");}
bool replica_simulation(){return replica;}
[[noreturn]] void fatal(const char* why){throw std::runtime_error(why);}
void log_message(const std::string&){}
uint64_t observe_native_entities(NativeEntityObserver){return 1;}
uint16_t injected_buttons(int){return 0;}
bool netplay_world_ready(){return world_ready;}int netplay_local_player(){return local_player;}
bool native_results_presenting(){return presentation[0];}bool native_cutscene_presenting(){return presentation[1];}
bool native_movie_presenting(){return presentation[2];}bool local_information_open(){return presentation[3];}
bool local_pause_menu_open(){return presentation[4];}
}
const SSNEFuncEntry g_ssne_funcs[]={
    {0x8009C410u,unused,"spheres"},{0x8009DBCCu,unused,"health"},{0x800A7AB0u,unused,"contact"},
    {0x800B2568u,unused,"render"},{0x800B4B28u,unused,"bomb"},{query,predicate_original,"secondary"},
    {0x800B7618u,unused,"active"},{0x80095EE8u,unused,"target"},{0x80129E00u,unused,"visibility"}};
const size_t g_ssne_func_count=std::size(g_ssne_funcs);const uint32_t g_ssne_entry=0;const bool g_ssne_has_game_code=true;
const SSNEFuncEntry* ssne_find_func(uint32_t address){
    if(address==query && !entry_available)return nullptr;
    for(const auto& entry:g_ssne_funcs)if(entry.address==address)return &entry;return nullptr;
}
PPCFunc* ssne_hook_slot(uint32_t address){return &hooks[address];}
void ssne_install_hook(uint32_t address,PPCFunc hook){hooks[address]=hook;++installs;}
void ssne_call(PPCContext& ctx,uint32_t address){
    const auto* entry=ssne_find_func(address);if(!entry)throw std::runtime_error("Unexpected guest address");entry->func(ctx);
}

int main(int argc,char** argv)try{
#ifdef _WIN32
    _putenv_s("SSNE_TEST_HIDDEN","");_putenv_s("SSNE_TEST_SECONDARY_PLAYER_TRACE","");
#else
    unsetenv("SSNE_TEST_HIDDEN");unsetenv("SSNE_TEST_SECONDARY_PLAYER_TRACE");
#endif
    const bool reference=argc==2 && std::string(argv[1])=="--reference";
    const bool client_reference=argc==2 && std::string(argv[1])=="--client-reference";
    require(argc==1 || reference || client_reference,"Unknown secondary-player test configuration");
    const auto* global_value=std::getenv("SSNE_SECONDARY_PLAYER_FILTER");
    const auto* client_value=std::getenv("SSNE_SECONDARY_PLAYER_CLIENT_FILTER");
    require((reference ? global_value && !std::strcmp(global_value,"0") : !global_value) &&
            (client_reference ? client_value && !std::strcmp(client_value,"0") : !client_value),
            "CTest did not select an independent default/global-reference/client-reference process");
    const bool client_skip=!reference && !client_reference;
    std::vector<uint8_t> ram(ram_size);ssne::g_ram=ram.data();fixture();write_signature();hooks[query]=previous_hook;
    for(unsigned word=0;word<12;++word){
        const auto code=mem_read32(query+word*4);mem_write32(query+word*4,code^1u);
        ssne::install_secondary_player_filter();
        require(!installs && hooks.at(query)==previous_hook && !ssne::secondary_player_previous,"Partial/unverified PAL signature changed hook chain");
        mem_write32(query+word*4,code);
    }
    entry_available=false;ssne::install_secondary_player_filter();entry_available=true;
    require(!installs && hooks.at(query)==previous_hook,"Missing translated predicate changed hooks");
    mem_write32(0x80000000u,0x47334245u);ssne::install_secondary_player_filter();
    require(!installs && hooks.at(query)==previous_hook,"Unknown disc revision changed hooks");
    mem_write32(0x80000000u,0x47334250u);ssne::install_secondary_player_filter();
    require(installs==1 && hooks.at(query)==ssne::secondary_player_predicate && ssne::secondary_player_previous==previous_hook,"Verified predicate did not preserve prior hook");
    ssne::install_secondary_player_filter();require(installs==1,"Secondary hook installed twice");
    // Exercise the public installer, not only its isolated signature helper.
    for(auto [address,word]:{std::pair{0x8009C410u,0x38600001u},std::pair{0x8009DBCCu,0xC02300CCu},
        std::pair{0x800B2568u,0x9421FFF8u},std::pair{0x800A7AB0u,0x9421FEF8u},
        std::pair{0x800B7618u,0x80030340u},std::pair{0x80095EE8u,0x9421FF90u},
        std::pair{0x80129E00u,0x28040003u},std::pair{0x800B4B28u,0x9421FFF0u}})mem_write32(address,word);
    ssne::native_party_install_occupancy_hooks();const auto all_installs=installs;
    ssne::native_party_install_occupancy_hooks();require(installs==all_installs && hooks.at(query)==ssne::secondary_player_predicate,"Occupancy installation overwrote or duplicated secondary hook");
    expect(context(),!reference,"Default did not skip only the reserved walking player",ram);
    for(unsigned slot=0;slot<8;++slot){
        fixture();ssne::native_party_set_occupancy(uint8_t(1u<<slot),uint8_t(1u<<slot));
        ssne::PadState pad;pad.buttons=0x111;ssne::native_party_set_input(slot,pad);
        expect(context(slot),false,"Every occupied slot must retain original predicate regardless of pad input",ram);
    }
    fixture();ssne::native_party_set_occupancy(1,3);
    expect(context(1),!reference,"Connected-unready slot did not follow canonical occupancy",ram);
    ssne::native_party_set_occupancy(3,3);expect(context(1),false,"Fresh ready/rejoin occupancy was ignored",ram);
    ssne::native_party_set_occupancy(1,1);expect(context(1),!reference,"Fresh leave occupancy was ignored at same actor address",ram);
    ssne::native_party_set_occupancy(3,3);expect(context(1),false,"Same-address rejoin remained filtered",ram);
    fixture();auto ctx=context();ctx.lr=caller+4;expect(ctx,false,"Unrelated caller was filtered",ram);
    for(unsigned mode:{0u,2u,3u,4u,5u}){
        fixture();mem_write32(0x802DB8CCu,mode);expect(context(),false,"Non-cooperative game mode was filtered",ram);
    }
    fixture();mem_write32(ssne::game_address(0x802DB0E0u),3);expect(context(),false,"Scene transition was filtered",ram);
    fixture();replica=true;expect(context(),false,"Replica with a Host local index was filtered",ram);
    fixture();local_player=1;expect(context(),false,"Pre-replica client warmup behavior was changed",ram);
    fixture();ssne::occupancy_enabled=false;expect(context(),false,"Unknown occupancy was filtered",ram);
    fixture();ssne::configured=false;expect(context(),false,"Unconfigured party was filtered",ram);
    fixture();mem_write32(first+2*0x1000u+0x48,0x802121E0u);expect(context(),false,"Unknown player class was filtered",ram);
    fixture();mem_write32(first+2*0x1000u+0x17C,8);expect(context(),false,"Unknown slot was filtered",ram);
    fixture();mem_write32(reg+2*4,first);expect(context(),false,"Stale registry identity was filtered",ram);
    fixture();ssne::registry=ssne::guest_ram_end()-4;expect(context(),false,"Invalid registry range was filtered",ram);
    for(unsigned state:{0u,2u,3u,4u}){
        fixture();mem_write32(first+2*0x1000u+0x340,state);expect(context(),false,"Non-walking actor state was filtered",ram);
    }
    fixture();mem_write32(first+2*0x1000u+0x370,1);expect(context(),false,"Frozen actor predicate changed",ram);
    fixture();ssne::native_party_leave_network();expect(context(),false,"Network teardown did not restore original predicate",ram);
    fixture();expect(context(),!reference,"Reconfigured party retained stale leave state",ram);
    // Client filtering requires a ready local participant and fully ready
    // membership. Every occupied Host, local player and ready peer stays native.
    for(unsigned local=1;local<8;++local)for(unsigned slot=0;slot<8;++slot){
        client_fixture(local);
        expect(context(slot),client_skip && slot!=0 && slot!=local,
               "Client reservation/active participant selection differs",ram);
    }
    client_fixture();ssne::native_party_set_occupancy(3,7);
    expect(context(),false,"Connected-unready Client target was filtered",ram);
    ssne::native_party_set_occupancy(7,7);expect(context(),false,"Fresh Client ready/rejoin occupancy was ignored",ram);
    ssne::native_party_set_occupancy(3,3);expect(context(),client_skip,"Fresh Client leave at same actor address was ignored",ram);
    ssne::native_party_set_occupancy(7,7);expect(context(),false,"Client same-address rejoin remained filtered",ram);
    ssne::native_party_set_occupancy(3,3);expect(context(),client_skip,"Client all-ready leave did not restore reservation filtering",ram);
    for(auto [occupied,connected]:{std::pair{0u,0u},std::pair{1u,1u},std::pair{2u,2u},
            std::pair{3u,1u},std::pair{1u,3u},std::pair{7u,3u},std::pair{3u,7u},
            std::pair{11u,3u},std::pair{3u,11u}}){
        client_fixture();ssne::native_party_set_occupancy(uint8_t(occupied),uint8_t(connected));
        expect(context(),false,"Unknown, incomplete or mismatched Client membership was filtered",ram);
    }
    for(unsigned high:{16u,32u,64u,128u}){
        client_fixture();ssne::native_party_configure(4);
        ssne::native_party_set_occupancy(uint8_t(3u|high),uint8_t(3u|high));
        expect(context(),false,"Out-of-roster Client membership was filtered",ram);
    }
    client_fixture();ssne::native_party_set_occupancy(255,255);
    expect(context(),false,"Fully occupied Client party was filtered",ram);
    client_fixture();ctx=context();ctx.lr=caller+4;expect(ctx,false,"Unrelated Client caller was filtered",ram);
    client_fixture();replica=false;expect(context(),false,"Pre-replica Client warmup was filtered",ram);
    client_fixture();world_ready=false;expect(context(),false,"Client world warmup was filtered",ram);
    client_fixture();ssne::occupancy_enabled=false;expect(context(),false,"Unknown Client occupancy was filtered",ram);
    client_fixture();ssne::configured=false;expect(context(),false,"Unconfigured Client party was filtered",ram);
    for(int local:{-1,0,8,9}){
        client_fixture();local_player=local;expect(context(),false,"Unknown Client local index was filtered",ram);
    }
    client_fixture();ssne::native_party_configure(1);expect(context(),false,"Client local index outside party was filtered",ram);
    client_fixture();mem_write32(0x80000000u,0x47334245u);expect(context(),false,"Unknown Client disc revision was filtered",ram);
    client_fixture();mem_write32(ssne::game_address(0x802DB0E0u),3);expect(context(),false,"Client scene transition was filtered",ram);
    client_fixture();mem_write32(ssne::game_address(0x802DB0E4u),1);expect(context(),false,"Client pending scene was filtered",ram);
    for(unsigned mode:{0u,2u,3u,4u,5u}){
        client_fixture();mem_write32(ssne::game_address(0x802DB1BCu),mode);
        expect(context(),false,"Non-cooperative Client mode was filtered",ram);
    }
    client_fixture();mem_write32(0x802DB0E4u,1);mem_write32(0x802DB1BCu,7);
    expect(context(),client_skip,"Client scene/mode guard used unmapped NTSC globals",ram);
    for(unsigned flag=0;flag<presentation.size();++flag){
        client_fixture();presentation[flag]=true;expect(context(),false,"Client presentation was filtered",ram);
    }
    for(unsigned pending_slot:{2u,3u}){
        client_fixture();ssne::pending_spawns[first+pending_slot*0x1000u]={};
        expect(context(),client_skip,"Canonical disconnected pending reservation blocked Client filtering",ram);
    }
    client_fixture();for(unsigned slot=2;slot<8;++slot)ssne::pending_spawns[first+slot*0x1000u]={};
    expect(context(),client_skip,"Canonical disconnected pending reservations blocked Client filtering",ram);
    for(unsigned pending_slot:{0u,1u,3u}){
        client_fixture();ssne::native_party_set_occupancy(11,11);ssne::pending_spawns[first+pending_slot*0x1000u]={};
        expect(context(),false,"Connected participant pending spawn was filtered",ram);
    }
    client_fixture();ssne::pending_spawns[0]={};expect(context(),false,"Unknown pending actor was filtered",ram);
    client_fixture();ssne::pending_spawns[ssne::guest_ram_end()-0x380u]={};
    expect(context(),false,"Invalid pending actor range was filtered",ram);
    client_fixture();ssne::pending_spawns[first+3*0x1000u]={};mem_write32(first+3*0x1000u+0x48,0x802121E0u);
    expect(context(),false,"Unknown pending actor class was filtered",ram);
    client_fixture();ssne::pending_spawns[first+3*0x1000u]={};mem_write32(first+3*0x1000u+0x17C,8);
    expect(context(),false,"Unknown pending actor slot was filtered",ram);
    client_fixture();ssne::pending_spawns[first+3*0x1000u]={};mem_write32(reg+3*4,first);
    expect(context(),false,"Stale pending actor identity was filtered",ram);
    client_fixture();for(unsigned slot=0;slot<9;++slot)ssne::pending_spawns[first+slot*0x1000u]={};
    expect(context(),false,"Unbounded pending actor map was filtered",ram);
    client_fixture();ssne::prepared_spawns[first]={0,0,0};expect(context(),false,"Client prepared spawn was filtered",ram);
    client_fixture();ssne::registry=ssne::guest_ram_end()-4;expect(context(),false,"Invalid Client registry range was filtered",ram);
    for(uint32_t self:{0u,ssne::guest_ram_end()-0x380u,first}){
        client_fixture();mem_write32(reg+4,self);expect(context(),false,"Missing, invalid or aliased Client local actor was filtered",ram);
    }
    const auto self=first+0x1000u;
    client_fixture();mem_write32(self+0x48,0x802121E0u);expect(context(),false,"Unknown Client local class was filtered",ram);
    client_fixture();mem_write32(self+0x17C,2);expect(context(),false,"Stale Client local slot was filtered",ram);
    for(unsigned state:{0u,2u,3u,4u}){
        client_fixture();mem_write32(self+0x340,state);expect(context(),false,"Non-walking Client local actor was filtered",ram);
    }
    client_fixture();mem_write32(self+0x370,1);expect(context(),false,"Frozen Client local actor was filtered",ram);
    for(unsigned slot=2;slot<8;++slot){
        client_fixture();mem_write32(first+slot*0x1000u+0x48,0x802121E0u);
        expect(context(slot),false,"Unknown Client target class was filtered",ram);
    }
    client_fixture();mem_write32(first+2*0x1000u+0x17C,8);expect(context(),false,"Unknown Client target slot was filtered",ram);
    for(uint32_t alias:{first,self}){
        client_fixture();mem_write32(reg+2*4,alias);expect(context(),false,"Stale or aliased Client target registry was filtered",ram);
    }
    for(unsigned state:{0u,2u,3u,4u}){
        client_fixture();mem_write32(first+2*0x1000u+0x340,state);expect(context(),false,"Non-walking Client target was filtered",ram);
    }
    client_fixture();mem_write32(first+2*0x1000u+0x370,1);expect(context(),false,"Frozen Client target was filtered",ram);
    client_fixture();const auto edge=ssne::guest_ram_end()-0x380u;ctx=context();ctx.r[3]=edge;
    mem_write32(edge+0x340,1);mem_write32(edge+0x370,0);
    expect(ctx,false,"Client target outside full player range was filtered",ram);
    const auto replacement=first+0x10000u;
    client_fixture();mem_write32(reg+4,replacement);mem_write32(replacement+0x48,0x802121E8u);
    mem_write32(replacement+0x17C,1);mem_write32(replacement+0x340,1);mem_write32(replacement+0x370,0);
    expect(context(),client_skip,"Fresh canonical Client local actor was not read",ram);
    client_fixture();mem_write32(reg+2*4,replacement);mem_write32(replacement+0x17C,2);
    expect(context(),false,"Removed Client target identity was filtered",ram);
    ctx=context();ctx.r[3]=replacement;expect(ctx,client_skip,"Fresh canonical Client target was not read",ram);
    client_fixture();ssne::native_party_leave_network();expect(context(),false,"Client network teardown did not restore original predicate",ram);
    client_fixture();expect(context(),client_skip,"Client reconfiguration retained stale leave state",ram);
    client_fixture();ctx=context();auto client_before=original_calls,client_prev=previous_calls;
    ssne_call_original(ctx,query,predicate_original);
    require(original_calls==client_before+1 && previous_calls==client_prev && ctx.r[3]==1,
            "Client original bypass was not consumed at exactly its first entry");
    expect(context(),client_skip,"Client original bypass token leaked into next normal call",ram);
    // A direct original call bypasses exactly its first entry. A subsequent
    // normal entry and a recursive call must still see the installed hook.
    fixture();ctx=context();auto before=original_calls,prev=previous_calls;
    ssne_call_original(ctx,query,predicate_original);
    require(original_calls==before+1 && previous_calls==prev && ctx.r[3]==1,"Original bypass was not consumed at exactly its first entry");
    expect(context(),!reference,"Original bypass token leaked into the next normal call",ram);
    fixture();ctx=context(0);const auto ram_before=ram;before=original_calls;prev=previous_calls;
    recurse=true;predicate_original(ctx);
    require(original_calls==before+2 && previous_calls==prev+2,"Recursive original call bypassed the hook chain more than once");
    require(!recurse && ram==ram_before,"Recursive hook forwarding changed RAM");
    std::cout<<"Native secondary player "<<(reference?"global-reference":client_reference?"client-reference":"default")<<": "<<assertions
             <<" assertions; full PAL signature, exact caller, Host/Client guards, fresh occupancy, independent boolean oracle, original/previous-hook forwarding, CPU and 24-MiB RAM preservation passed.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}