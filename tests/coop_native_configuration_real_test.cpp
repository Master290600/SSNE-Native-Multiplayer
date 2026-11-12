#include "native_configuration.h"
#include "ssne/runtime.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
using namespace port::coop;
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
uint64_t digest(unsigned size){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<size;++i){h^=ssne::g_ram[i];h*=1099511628211ull;}return h;}
int main(int argc,char** argv) {
    require(argc==3,"Pass game_data and native capture directory");PPCContext ctx;std::string error;require(ssne::boot(ctx,argv[1],error)!=0,error.c_str());
    std::ifstream f(std::string(argv[2])+"/native-capture.ram",std::ios::binary);std::vector<uint8_t> ram((std::istreambuf_iterator<char>(f)),{});require(ram.size()==0x01800000,"Real fixture missing");memcpy(ssne::g_ram,ram.data(),ram.size());ctx.r[13]=0x802E22A0;ctx.msr&=~0x8000u;
    auto root=native_global_configuration();auto original=read_native_configuration(root,"GameSetup");require(original.error.empty(),"Configuration traversal");
    Properties next={{"GameSetup.RulesCoop.Difficulty",int64_t(2)},{"GameSetup.RulesCoop.FriendlyFire",int64_t(0)},{"GameSetup.RulesCoop.Level",std::string("Rlevel1_2")}};
    auto invalid=next;invalid["SystemSetup.SFXVolume"]=int64_t(0);auto before=digest(unsigned(ram.size()));
    require(!write_native_configuration(ctx,root,invalid,"GameSetup",error) && digest(unsigned(ram.size()))==before,"Write scope rejection changed native data");
    invalid=next;invalid["GameSetup.RulesCoop.Difficulty"]=int64_t(99);require(!write_native_configuration(ctx,root,invalid,"GameSetup",error) && digest(unsigned(ram.size()))==before,"Invalid range partially applied");
    require(validate_native_configuration(ctx,root,next,"GameSetup",error) && digest(unsigned(ram.size()))==before,"Configuration preflight changed native RAM");
    auto weapon_rule=read_native_configuration(root,"PuzzleConfig.HideWeapons");require(weapon_rule.values.contains("PuzzleConfig.HideWeapons"),"Weapon visibility rule missing from real fixture");
    Properties invalid_flag={{"PuzzleConfig.HideWeapons",int64_t(2)}};
    require(!validate_native_configuration(ctx,root,invalid_flag,"PuzzleConfig.HideWeapons",error) && digest(unsigned(ram.size()))==before,"Invalid weapon visibility preflight changed RAM");
    require(write_native_configuration(ctx,root,next,"GameSetup",error),error.c_str());auto read=read_native_configuration(root,"GameSetup");
    for(const auto& [key,value]:next)require(read.values.at(key)==value,"Native bootstrap field not applied");
    next["GameSetup.RulesCoop.Level"]=std::string("NativeLongerLevelStringForAllocationCheck");require(write_native_configuration(ctx,root,next,"GameSetup",error),error.c_str());read=read_native_configuration(root,"GameSetup");require(read.values.at("GameSetup.RulesCoop.Level")==next.at("GameSetup.RulesCoop.Level"),"Native string allocation/copy failed");
    require(write_native_configuration(ctx,root,original.values,"GameSetup",error),error.c_str());read=read_native_configuration(root,"GameSetup");require(read.values==original.values,"Original gameplay configuration not restored");
    std::cout<<"Real native configuration: scoped atomic validation, integer ranges, native string assignment/reallocation and gameplay-field restoration passed.\n";std::cout.flush();std::_Exit(0);
}
