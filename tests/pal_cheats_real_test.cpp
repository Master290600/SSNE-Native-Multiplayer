// Include the adapter to exercise its private apply/lift path against an owned
// PAL RAM capture. This executable never opens a window or writes a card.
#include "../runtime/src/hle.cpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
// Exercise the real inventory consumer used by native weapons. Merely checking
// +D8 did not prove that firing was free, or that disabling restored ammo costs.
void check_native_ammo_consumer(const PPCContext& context,uint32_t inventory,bool infinite){
    const auto* consume=ssne_find_func(0x8009F484);require(consume,"Native ammunition consumer missing");
    struct RestoreCounts {
        uint32_t inventory;std::array<uint16_t,36> values{};
        explicit RestoreCounts(uint32_t inv):inventory(inv){for(unsigned i=0;i<values.size();++i)values[i]=mem_read16(inv+2*i);}
        ~RestoreCounts(){for(unsigned i=0;i<values.size();++i)mem_write16(inventory+2*i,values[i]);}
    } restore(inventory);
    require(bool(mem_read32(inventory+0xD8))==infinite,"Unexpected inventory cheat policy before native consumption");
    for(unsigned type=0;type<18;++type)for(unsigned bank=0;bank<2;++bank){
        const auto target=inventory+bank*0x24+type*2,other=inventory+(1-bank)*0x24+type*2;
        // Includes empty/insufficient inventories: unlimited fire must not fail
        // when the weapon's ammo count reaches zero or the mode costs more.
        for(unsigned available:{7u,2u,0u})for(unsigned cost:{1u,3u}){
            mem_write16(target,uint16_t(available));mem_write16(other,11);
            auto call=context;call.r[3]=inventory;call.r[4]=type;call.r[5]=cost;call.r[6]=bank;
            consume->func(call);
            const bool success=infinite || available>=cost;
            require(call.r[3]==uint32_t(success),"Native ammunition consume success differs");
            require(mem_read16(target)==(success && !infinite?available-cost:available),"Native ammunition cost differs");
            require(mem_read16(other)==11,"Native ammunition consumer changed the other gun's bank");
        }
    }
}
void check_native_weapon_ownership(const PPCContext& context,uint32_t actor,bool all_weapons){
    const auto* owned=ssne_find_func(0x800C6170);require(owned,"Native weapon ownership query missing");
    for(unsigned weapon=0;weapon<12;++weapon){
        const auto address=actor+0x4CC+weapon*24,saved=mem_read32(address);
        mem_write32(address,0);auto call=context;call.r[3]=actor;call.r[4]=weapon;owned->func(call);
        mem_write32(address,saved);
        require(call.r[3]==uint32_t(all_weapons),"All-weapons cheat did not reach the native ownership query");
    }
}
void check_refilled(uint32_t inventory){
    for(unsigned type=0;type<18;++type)for(unsigned bank:{0u,0x24u})
        require(mem_read16(inventory+bank+type*2)==mem_read16(inventory+0xB4+type*2),"Enabled cheat did not fill both native ammunition banks");
}
int main(int argc,char** argv) try {
    require(argc==3,"Pass PAL game_data and native fixture directory");
    ssne::native_party_extended_memory(true);PPCContext ctx;std::string error;
    require(ssne::boot(ctx,argv[1],error)!=0,error.c_str());
    const auto root=std::string(argv[2]);
    std::ifstream input(root+"/native-capture.ram",std::ios::binary);
    std::vector<uint8_t> ram((std::istreambuf_iterator<char>(input)),{});
    require(ram.size()==ssne::guest_ram_size(),"Fixture memory size differs");
    std::memcpy(ssne::g_ram,ram.data(),ram.size());
    std::ifstream cpu(root+"/native-capture-context.bin",std::ios::binary);
    cpu.read(reinterpret_cast<char*>(&ctx),sizeof(ctx));require(bool(cpu),"PPC fixture missing");ctx.msr&=~0x8000u;
    require(mem_read32(0x80000000u)==0x47334250u,"PAL fixture required");
    ssne::install_cheats();require(ssne::cheats_available(),"PAL signatures rejected");
    const auto actor=mem_read32(0x802D9048),inv=mem_read32(actor+0x5e8);
    require(mem_read32(actor+0x48)==0x802121e8,"Native player unavailable");
    ssne::g_port_player[0]=actor;ssne::g_port_player_ms[0]=ssne::now_ms();
    mem_write32(ssne::game_address(ssne::kGameMode),0);
    std::array<uint32_t,3> pal{},usa{};
    for(unsigned i=0;i<3;++i){pal[i]=mem_read32(ssne::game_address(ssne::kCheatGlobals[i]));usa[i]=mem_read32(ssne::kCheatGlobals[i]);}
    const auto ammo=mem_read32(inv+0xd8),allow=mem_read32(actor+0x358),allow_before=mem_read32(allow+0x1c);
    const auto flags=mem_read32(actor+0x1c);
    std::array<uint32_t,5> settings{};for(unsigned i=0;i<5;++i)settings[i]=mem_read32(mem_read32(actor+ssne::g_scaled_settings[i].offset)+0x24);
    const auto weapon=mem_read32(actor+0x4bc+24),mode=mem_read32(weapon+0x1d0);const auto damage=mem_read32(mode+0x10);
    ssne::Cheats cheats;cheats.invulnerable=cheats.infinite_ammo=cheats.all_weapons=cheats.unlock_all=cheats.fly=true;cheats.speed=2;cheats.jump=2;cheats.damage=3;
    ssne::set_cheats(cheats);ssne::cheats_frame(false);
    for(unsigned i=0;i<3;++i){require(mem_read32(ssne::game_address(ssne::kCheatGlobals[i]))==1,"PAL cheat global not set");require(mem_read32(ssne::kCheatGlobals[i])==usa[i],"USA global touched on PAL");}
    require(mem_read32(inv+0xd8)==1,"Infinite ammo inventory flag");
    check_refilled(inv);check_native_ammo_consumer(ctx,inv,true);check_native_weapon_ownership(ctx,actor,true);
    for(unsigned i=0;i<5;++i)require(u2f(mem_read32(mem_read32(actor+ssne::g_scaled_settings[i].offset)+0x24))==u2f(settings[i])*2,"Speed/jump scale");
    require(u2f(mem_read32(mode+0x10))==u2f(damage)*3,"Weapon damage scale");
    require(mem_read32(actor+0x35c)==1 && (mem_read32(actor+0x1c)&0xc000)==0xc000,"Native flight enable");
    auto call=ctx;call.r[3]=actor;const auto health=mem_read32(actor+0xcc);ssne::hle_damage(call);require(f2u(float(call.ps0[1]))==health,"Invulnerable damage gate");
    const auto text=ctx.r[1]-0x200;const char name[]="Gallery";for(unsigned i=0;i<sizeof(name);++i)mem_write8(text+i,name[i]);call=ctx;call.r[3]=text;ssne::hle_unlocked(call);require(call.r[3]==1,"PAL unlock override");
    ssne::set_cheats({});ssne::cheats_frame(false);
    for(unsigned i=0;i<3;++i)require(mem_read32(ssne::game_address(ssne::kCheatGlobals[i]))==pal[i],"PAL cheat global restore");
    require(mem_read32(inv+0xd8)==ammo,"Ammo flag restore");
    check_native_ammo_consumer(ctx,inv,ammo!=0);check_native_weapon_ownership(ctx,actor,pal[2]!=0);
    for(unsigned i=0;i<5;++i)require(mem_read32(mem_read32(actor+ssne::g_scaled_settings[i].offset)+0x24)==settings[i],"Movement scale restore");
    require(mem_read32(mode+0x10)==damage,"Weapon damage restore");
    require(!mem_read32(actor+0x35c) && mem_read32(actor+0x1c)==flags && mem_read32(allow+0x1c)==allow_before,"Native flight restore");
    // Exercise the multiplayer mod layer with all native PAL party inventories.
    ssne::set_authoritative_gameplay(true);ssne::mod_netplay_player(0);mem_write32(ssne::game_address(ssne::kGameMode),2);
    ssne::ModOptions mods;mods.god=mods.ammo=mods.weapons=mods.rapid_fire=true;mods.fire_multiplier=3;
    std::vector<std::pair<uint32_t,uint32_t>> inventories,rates;
    std::vector<uint32_t> party_actors;
    for(unsigned slot=0;slot<ssne::native_party_capacity();++slot){
        const auto a=mem_read32(ssne::native_party_registry()+slot*4);if(!a || !ssne::native_party_actor_occupied(a))continue;
        party_actors.push_back(a);
        const auto v=mem_read32(a+0x5e8);inventories.push_back({v,mem_read32(v+0xd8)});
        const auto w=mem_read32(a+0x4bc+24),m=mem_read32(w+0x1d0);rates.push_back({m+0x1c,mem_read32(m+0x1c)});
    }
    require(inventories.size()>=2,"Multiplayer fixture needs multiple players");
    ssne::mod_options(mods);ssne::mod_tick(ctx,actor);
    for(auto [v,saved]:inventories){require(mem_read32(v+0xd8)==1,"Shared PAL infinite ammunition");check_refilled(v);check_native_ammo_consumer(ctx,v,true);}
    for(auto a:party_actors)check_native_weapon_ownership(ctx,a,true);
    for(auto [a,saved]:rates)require(u2f(mem_read32(a))>=u2f(saved),"Shared PAL rapid fire");
    call=ctx;call.r[3]=actor;ssne::hle_damage(call);require(f2u(float(call.ps0[1]))==health,"Host mod god damage gate");
    auto local=mods;local.god=false;local.esp=true;ssne::mod_local_options(local);require(ssne::mod_current_options().god && ssne::mod_current_options().esp,"Local options preserve host cheats");
    ssne::mod_shared_options(0,3);ssne::mod_tick(ctx,actor);
    require(ssne::mod_current_options().esp && !ssne::mod_current_options().god,"Host updates preserve local ESP");
    for(auto [v,saved]:inventories){require(mem_read32(v+0xd8)==saved,"Shared ammunition flag restore");check_native_ammo_consumer(ctx,v,saved!=0);}
    for(auto a:party_actors)check_native_weapon_ownership(ctx,a,pal[2]!=0);
    for(auto [a,saved]:rates)require(mem_read32(a)==saved,"Shared rapid fire restore");
    for(unsigned i=0;i<3;++i)require(mem_read32(ssne::game_address(ssne::kCheatGlobals[i]))==pal[i],"Shared PAL global restore");
    std::cout<<"PAL multiplayer mods passed: native ammunition costs, both gun banks, all-weapon ownership, refill, rapid fire, god, local/shared separation and restoration.\n";
    std::cout<<"PAL native cheats passed: signatures, PAL-only globals, god, ammo, weapons, unlock, speed, jump, damage, flight and restoration.\n";std::cout.flush();std::_Exit(0);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
