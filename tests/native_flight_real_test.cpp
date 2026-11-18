// Optional owned-PAL regression. Includes the adapter to exercise its private
// cheat apply/restore path, then runs the original player update: flag-only
// assertions cannot prove that held flight controls actually move the player.
// No window, network peer, card, or extracted-data write is performed.
#include "../runtime/src/hle.cpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::array<float,3> position(uint32_t actor){
    std::array<float,3> p{};for(unsigned i=0;i<3;++i)p[i]=u2f(mem_read32(actor+0x20+4*i));return p;
}
float distance(const std::array<float,3>& a,const std::array<float,3>& b){
    float result=0;for(unsigned i=0;i<3;++i){require(std::isfinite(a[i]) && std::isfinite(b[i]),"Flight produced a non-finite position");result+=(a[i]-b[i])*(a[i]-b[i]);}return std::sqrt(result);
}
}
int main(int argc,char** argv)try{
    require(argc==3,"Pass owned PAL game_data and a native fixture directory");
    ssne::native_party_extended_memory(true);
    PPCContext context;std::string error;
    require(ssne::boot(context,argv[1],error)!=0,error.c_str());
    const std::string fixture=argv[2];
    std::ifstream memory(fixture+"/native-capture.ram",std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(memory)),{});
    require(bytes.size()==ssne::guest_ram_size(),"Fixture guest memory size differs");
    std::memcpy(ssne::g_ram,bytes.data(),bytes.size());
    std::ifstream cpu(fixture+"/native-capture-context.bin",std::ios::binary);
    cpu.read(reinterpret_cast<char*>(&context),sizeof(context));require(bool(cpu),"PPC fixture context missing");
    context.msr&=~0x8000u;context.r[1]=ssne::guest_ram_end()-0x40000;
    mem_write32(context.r[1],0);
    require(mem_read32(0x80000000u)==0x47334250u,"This regression requires the owned PAL revision");
    ssne::select_version();
    ssne::install_cheats();require(ssne::cheats_available(),"PAL cheat signatures unavailable");
    ssne::install_mouse_aim();require(ssne::mouse_aim_available(),"Verified PAL look/flight adapter unavailable");
    const auto* player_update=ssne_find_func(0x800beec8u);
    require(player_update && player_update->func,"Native player update unavailable");
    const auto registry=ssne::native_party_registry();const auto actor=mem_read32(registry);
    require(actor>=0x80000000u && actor<=ssne::guest_ram_end()-0x790 && mem_read32(actor+0x48)==0x802121e8u,"Real host player unavailable");
    require(mem_read32(actor+0x178)==0 && mem_read32(actor+0x17c)==0,"Fixture host must occupy native player slot zero");
    require(!mem_read32(actor+0x370) && !mem_read32(actor+0x328) && !mem_read32(actor+0x6c4) && u2f(mem_read32(actor+0xcc))>0,"Fixture needs an alive, unfrozen host on foot");
    const auto bank=mem_read32(0x802aba9cu);
    require(bank>=0x80000000u && bank<=ssne::guest_ram_end()-0x70c,"Native PAL action bank unavailable");
    const auto allow=mem_read32(actor+0x358);
    require(allow>=0x80000000u && allow<=ssne::guest_ram_end()-0x30,"Native AllowFlyCheat setting unavailable");
    const auto original_allow=mem_read32(allow+0x1c),original_flags=mem_read32(actor+0x1c);
    require(!mem_read32(actor+0x35c),"Fixture must start with native flight off");
    const auto other=mem_read32(registry+4);
    const bool has_other=other>=0x80000000u && other<=ssne::guest_ram_end()-0x790 && mem_read32(other+0x48)==0x802121e8u;
    const auto other_flags=has_other?mem_read32(other+0x1c):0,other_fly=has_other?mem_read32(other+0x35c):0;
    // A pre-existing collision bit belongs to the game, not to this cheat.
    const auto retained_flags=original_flags|0x4000u;
    mem_write32(actor+0x1c,retained_flags);
    mem_write32(context.r[13]-0x7a08,f2u(1.f/60.f));
    ssne::set_replica_simulation(false);ssne::set_authoritative_gameplay(true);ssne::netplay_set_local_player(0);ssne::set_cheat_host(true);
    mem_write32(ssne::game_address(ssne::kGameMode),1);
    ssne::Cheats cheats;cheats.authoritative_host=true;cheats.fly=true;
    ssne::set_cheats(cheats);ssne::cheats_frame(false);
    require(ssne::cheat_flying() && mem_read32(actor+0x35c)==1 && (mem_read32(actor+0x1c)&0xc000u)==0xc000u,"Host F12 flight did not enable in cooperative gameplay");
    auto clear_actions=[&](){
        for(uint32_t offset:{0x38u,0x54u,0x70u,0x8cu,0xa8u,0xc4u,0xe0u,0xfcu,0x604u,0x63cu})mem_write32(bank+0xc+offset,0);
        // The original hidden D-pad toggle must not compete with F12 selection.
        for(uint32_t offset:{0x578u,0x594u,0x5ccu,0x5b0u})mem_write32(bank+0x10+offset,0);
    };
    const auto status=mem_read32(0x802abad8u);
    require(status>=0x80000000u && status<=ssne::guest_ram_end()-48,"Native PAL pad status unavailable");
    const auto* clamp=ssne_find_func(ssne::g_ver->pad_clamp);
    const auto* process=ssne_find_func(ssne::g_ver->pad_process);
    require(clamp && process,"Original PAL pad processor unavailable");
    unsigned sampled_frames=0;
    auto step=[&](float jump,float fire,unsigned frames){
        for(unsigned i=0;i<frames;++i){
            // Drive real PADRead -> original PADClamp -> original action
            // processing, rather than manufacturing flight-only action floats.
            ssne::PadState pad;
            if(jump){pad.buttons|=ssne::PAD_L;pad.trigger_l=255;}
            if(fire){pad.buttons|=ssne::PAD_R;pad.trigger_r=255;}
            ssne::set_pad(0,pad);clear_actions();
            auto call=context;call.r[3]=status;
            ssne::g_netplay_pad_refresh=true;ssne::hle_netplay_pad_read(call);ssne::g_netplay_pad_refresh=false;
            call=context;call.r[3]=status;ssne_call_original(call,ssne::g_ver->pad_clamp,clamp->func);
            call=context;call.r[3]=bank;call.ps0[1]=call.ps1[1]=1.f/60.f;
            ssne_call_original(call,ssne::g_ver->pad_process,process->func);
            require((mem_read8(status+6)!=0)==bool(jump) && (mem_read8(status+7)!=0)==bool(fire),"Flight changed the native L/R pad meanings");
            require((u2f(mem_read32(bank+12))>.5f)==bool(jump),"Raw Space did not retain the original jump action");
            require((u2f(mem_read32(bank+12+28*10))>.5f)==bool(fire),"Raw Space became native primary fire, or real R/fire was blocked");
            ++sampled_frames;
            ssne::cheats_frame(false);
            call=context;call.r[3]=actor;
            ssne_call_original(call,0x800beec8u,player_update->func);
        }
    };
    // The movement adapter must neither change another caller's nonvolatile
    // register nor another player's flight direction.
    auto probe=context;probe.r[3]=actor;probe.lr=0x800A8660u;probe.ps0[29]=probe.ps1[29]=.75;
    ssne::hle_look_update(probe);require(probe.ps0[29]==.75,"Normal look caller changed flight movement register");
    if(has_other){probe=context;probe.r[3]=other;probe.lr=0x800BF410u;probe.ps0[29]=probe.ps1[29]=.75;
        ssne::hle_look_update(probe);require(probe.ps0[29]==.75,"Host cheat reversed another player's flight scalar");}
    const auto before=position(actor);step(1,0,8);const auto above=position(actor);
    require(above[1]>before[1]+.01f,"Held raw L/Space did not move the host upwards");
    step(0,0,8);const auto stationary=position(actor);
    require(distance(stationary,above)<.0001f,"Neutral flight drifted or fell");
    step(0,1,8);const auto below=position(actor);
    require(below[1]<stationary[1]-.01f,"Held raw R/fire did not move the host downwards");
    const auto both_before=position(actor);step(1,1,8);
    require(distance(position(actor),both_before)<.0001f,"Simultaneous raw jump/fire did not cancel vertical flight movement");
    if(has_other)require(mem_read32(other+0x1c)==other_flags && mem_read32(other+0x35c)==other_fly,"Host flight changed another player's physics flags");
    clear_actions();ssne::set_cheats({});ssne::cheats_frame(false);
    require(!ssne::cheat_flying() && mem_read32(actor+0x35c)==0 && mem_read32(actor+0x1c)==retained_flags && mem_read32(allow+0x1c)==original_allow,"Host flight did not restore the game's original flags and setting");
    ssne::set_cheat_host(false);cheats.authoritative_host=false;ssne::set_cheats(cheats);ssne::cheats_frame(false);
    require(!ssne::cheat_flying() && !mem_read32(actor+0x35c),"Client-side F12 request bypassed host authority");
    ssne::set_cheat_host(true);cheats.authoritative_host=true;ssne::set_cheats(cheats);ssne::set_replica_simulation(true);ssne::cheats_frame(false);
    require(!ssne::cheat_flying() && !mem_read32(actor+0x35c) && mem_read32(actor+0x1c)==retained_flags,"Replica simulation enabled authoritative flight");
    ssne::set_replica_simulation(false);
    mem_write32(registry,0);ssne::cheats_frame(false);
    require(!ssne::cheat_flying(),"Missing host actor reused a stale local player pointer");
    mem_write32(registry,actor);
    ssne::set_cheats({});ssne::cheats_frame(false);
    ssne::set_authoritative_gameplay(false);mem_write32(ssne::game_address(ssne::kGameMode),0);
    ssne::g_port_player[0]=actor;ssne::g_port_player_ms[0]=ssne::now_ms();
    ssne::set_cheat_host(false);cheats.authoritative_host=false;ssne::set_cheats(cheats);ssne::cheats_frame(false);
    require(ssne::cheat_flying() && mem_read32(actor+0x35c)==1,"Single-player F12 flight regressed");
    const auto single=position(actor);step(1,0,8);require(position(actor)[1]>single[1]+.01f,"Single-player native ascent regressed");
    ssne::set_cheats({});ssne::cheats_frame(false);
    require(mem_read32(actor+0x1c)==retained_flags && mem_read32(allow+0x1c)==original_allow,"Single-player flight failed to restore original settings");
    require(sampled_frames==40,"Expected all original pad/action flight frames");
    std::cout<<"PAL flight: original PADRead/clamp/actions keep Space out of fire and R/fire intact; raw L ascent, raw R descent, neutral/both, scoped caller/actor, host-only physics, original flag restoration, client/replica/missing-player gates and single-player regression passed.\n"<<std::flush;
    std::_Exit(0);
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
