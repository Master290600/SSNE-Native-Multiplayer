#include "ssne/performance.h"
#include <type_traits>
#include <utility>
#include "native_session.h"
#include "native_lifecycle.h"
#include "native_vehicle.h"
#include "native_presentation.h"
#include "native_lobby.h"
#include "native_frontend.h"
#include "native_checkpoint.h"
#include "checkpoint_codec.h"
#include <stdexcept>
#include "afk_combat.h"
#include "ssne/game_addresses.h"
#include "ssne/native_party.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include "ssne/weapon_groups.h"
#include "ssne/gx_gpu.h"
#include "ssne/recomp_hooks.h"
#include "ssne/native_results.h"
#include "ssne/native_cutscene.h"
#include "ssne/native_music.h"
#include "ssne/native_effect.h"
#include "ssne/audio.h"
#include "ssne/input.h"
#include "ssne/mods.h"
#include "ssne/ammo_rules.h"
#include "ssne/exi.h"
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>
#include <sstream>
#include <cmath>
namespace port::coop {
namespace {
#include "native_checkpoint_probe.inc"
#include "native_campaign_probe.inc"
unsigned collider_stress_phase=0;uint32_t collider_stress_original=0;
uint64_t carry_pickups_complete_frame=0;
bool env_present(const char* name){auto value=getenv(name);return value && *value;}
bool env_requested(const char* name){auto value=getenv(name);return value && *value && std::string(value)!="0";}
#include "native_versus.inc"
// Opt-in stage timings, collected on the guest thread without GPU synchronization.
struct NativeStageTimer {
    using Clock=std::chrono::steady_clock;
    struct Totals {double total=0,max=0;unsigned count=0;Clock::time_point since=Clock::now();};
    ssne::perf::Scope performance;const char* name;const NativeSession::Log& log;Clock::time_point start;bool enabled;
    NativeStageTimer(const char* n,const NativeSession::Log& l):performance(n),name(n),log(l),start(Clock::now()),enabled(env_requested("SSNE_COOP_PROFILE")){}
    ~NativeStageTimer(){if(!enabled || !log)return;static std::map<std::string,Totals> totals;auto& t=totals[name];
        const auto now=Clock::now();const auto ms=std::chrono::duration<double,std::milli>(now-start).count();t.total+=ms;t.max=std::max(t.max,ms);++t.count;
        if(t.count>=120){log("[coop-profile] stage="+std::string(name)+" average_ms="+std::to_string(t.total/t.count)+" max_ms="+std::to_string(t.max)+" calls_per_second="+std::to_string(t.count/std::chrono::duration<double>(now-t.since).count()));t={};}
    }
};
void dump_native_profiles(const char* path){
    auto sample=read_native_configuration(native_global_configuration(),"PlayerProfiles");if(!sample.error.empty())return;
    std::ofstream file(path);if(!file)return;
    for(const auto& [key,value]:sample.values){
        file<<key<<'\t';
        if(auto n=std::get_if<int64_t>(&value))file<<"i\t"<<*n;
        else if(auto f=std::get_if<float>(&value))file<<"f\t"<<f2u(*f);
        else if(auto s=std::get_if<std::string>(&value))file<<"s\t"<<std::quoted(*s);
        else continue;
        file<<'\n';
    }
}
bool test_native_party_checkpoint(uint64_t frame,unsigned& rejected,std::string& error){
    rejected=0;std::vector<uint8_t> saved;if(!ssne::native_party_save_state(saved,error))return false;
    const auto path="native-party-checkpoint-"+std::to_string(frame)+".bin";
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(saved.data()),saved.size());if(!out){error="Party checkpoint write failed";return false;}}
    std::ifstream in(path,std::ios::binary);std::vector<uint8_t> disk((std::istreambuf_iterator<char>(in)),{});if(saved!=disk){error="Party checkpoint disk differs";return false;}
    auto canonical=[](std::vector<uint8_t> bytes){
        auto u32=[&](size_t at){uint32_t value=0;for(unsigned i=0;i<4;++i)value=(value<<8)|bytes.at(at+i);return value;};
        size_t offset=552;for(unsigned stride:{12u,8u,8u}){const auto count=u32(offset);offset+=4+count*stride;}
        const auto count=u32(offset);offset+=4;for(unsigned i=0;i<count;++i,offset+=24)std::fill_n(bytes.begin()+offset+20,4,0);
        bytes.resize(bytes.size()-8);return bytes;
    };
    const auto original=canonical(saved);
    auto reject=[&](const std::vector<uint8_t>& bad){std::string why;if(ssne::native_party_load_state(bad,why) || why.empty())return false;std::vector<uint8_t> after;if(!ssne::native_party_save_state(after,why) || canonical(after)!=original)return false;++rejected;return true;};
    auto short_file=disk;short_file.pop_back();auto corrupt=disk;corrupt[20]^=1;
    if(!reject(short_file) || !reject(corrupt)){error="Damaged party checkpoint changed live state";return false;}
    auto resign=[](std::vector<uint8_t>& b){uint64_t h=14695981039346656037ull;for(size_t i=0;i<b.size()-8;++i){h^=b[i];h*=1099511628211ull;}for(unsigned i=0;i<8;++i)b[b.size()-8+i]=uint8_t(h>>(56-i*8));};
    for(unsigned at:{8u,12u,16u,20u,24u,552u}){auto bad=disk;std::fill_n(bad.begin()+at,4,255);resign(bad);if(!reject(bad)){error="Malformed party checkpoint was accepted";return false;}}
    // Change only external input state, then restore it before the next native
    // simulation step. No guest RAM or in-game inventory is edited by this test.
    ssne::PadState probe;probe.buttons=ssne::PAD_A;probe.stick_x=23;ssne::native_party_set_input(7,probe);
    std::vector<uint8_t> changed;ssne::native_party_save_state(changed,error);
    const bool changed_input=canonical(changed)!=original;
    if(!ssne::native_party_load_state(disk,error))return false;
    std::vector<uint8_t> restored;if(!ssne::native_party_save_state(restored,error))return false;
    if(!changed_input || canonical(restored)!=original){error="Party checkpoint failed to restore changed external input state";return false;}
    return true;
}
bool test_native_lifetime_checkpoint(const NativeWorld& source,const NativeWorldSample& sample,uint64_t frame,unsigned& rejected,std::string& error){
    rejected=0;
    std::vector<uint8_t> bytes;if(!source.save_lifetimes(bytes,error))return false;
    const auto path="native-world-checkpoint-"+std::to_string(frame)+".bin";
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!out){error="Checkpoint test write failed";return false;}}
    std::ifstream in(path,std::ios::binary);std::vector<uint8_t> disk((std::istreambuf_iterator<char>(in)),{});
    if(disk!=bytes){error="Checkpoint disk roundtrip differs";return false;}
    NativeWorld restored;
    for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player)restored.set_player_owner(unsigned(std::get<int64_t>(e.properties.at("player_slot"))),e.owner);
    if(!restored.load_lifetimes(disk,sample.epoch+1,error))return false;
    const auto check=restored.capture(sample.tick);
    if(!check.error.empty() || check.entities!=sample.entities || check.epoch!=sample.epoch+1 || check.created!=sample.created || check.registered!=sample.registered || check.removed!=sample.removed || check.unmapped.size()!=sample.unmapped.size()){error="Restored native lifetime sample differs";return false;}
    for(const auto& [id,e]:sample.entities)if(source.local_address(id)!=restored.local_address(id)){error="Restored native object identity differs";return false;}
    for(unsigned i=0;i<sample.unmapped.size();++i)if(sample.unmapped[i].address!=check.unmapped[i].address || sample.unmapped[i].vtable!=check.unmapped[i].vtable){error="Unmapped native lifetime was lost";return false;}
    std::vector<uint8_t> baseline;restored.save_lifetimes(baseline,error);
    auto reject=[&](const std::vector<uint8_t>& bad,uint64_t epoch){std::string why;if(restored.load_lifetimes(bad,epoch,why) || why.empty())return false;std::vector<uint8_t> after;const bool unchanged=restored.save_lifetimes(after,why) && after==baseline;if(unchanged)++rejected;return unchanged;};
    auto short_file=disk;short_file.pop_back();auto corrupt=disk;corrupt[60]^=1;
    if(!reject(short_file,sample.epoch+2) || !reject(corrupt,sample.epoch+2) || !reject(disk,sample.epoch) || !reject(disk,sample.epoch+1)){error="Invalid checkpoint changed lifetime index";return false;}
    // Re-sign malformed metadata to exercise structural validation, not only CRC.
    auto resign=[](std::vector<uint8_t>& v){uint64_t h=14695981039346656037ull;for(size_t i=0;i<v.size()-8;++i){h^=v[i];h*=1099511628211ull;}for(unsigned i=0;i<8;++i)v[v.size()-8+i]=uint8_t(h>>(56-i*8));};
    for(unsigned test=0;test<8;++test){auto bad=disk;
        if(test==0)std::fill(bad.begin()+56,bad.begin()+60,255); // oversized count
        if(test==1)std::fill(bad.begin()+68,bad.begin()+72,0); // invalid native address
        if(test==2){if(bad.size()<140){error="Not enough native records for duplicate check";return false;}std::copy_n(bad.begin()+60,36,bad.begin()+96);}
        if(test==3)std::fill(bad.begin()+24,bad.begin()+32,0); // invalid next ID
        if(test==4)bad[8]^=1; // other game revision
        if(test==5)bad[12]^=1; // different active RAM size
        if(test==6){bad[83]=255;} // unknown record flags
        if(test==7){ // valid pointer range, but wrong class in restored RAM
            bool found=false;for(size_t offset=60;offset+36<=bad.size()-8;offset+=36)if(bad[offset+23]&4){bad[offset+12]=0x80;bad[offset+13]=bad[offset+14]=bad[offset+15]=0;found=true;break;}
            if(!found){error="No registered native object for RAM binding check";return false;}
        }
        resign(bad);if(!reject(bad,sample.epoch+2)){error="Malformed native checkpoint was accepted";return false;}
    }
    // Live connection generations are retained, not read from a historical save.
    for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player){const auto slot=unsigned(std::get<int64_t>(e.properties.at("player_slot")));restored.set_player_owner(slot,e.owner.generation?PlayerId{slot,e.owner.generation+1}:PlayerId{});}
    if(!restored.load_lifetimes(disk,sample.epoch+2,error))return false;
    const auto rebound=restored.capture(sample.tick);
    for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player){const auto expected=e.owner.generation?PlayerId{e.owner.slot,e.owner.generation+1}:PlayerId{};if(rebound.entities.at(id).owner!=expected){error="Checkpoint reverted live connection ownership";return false;}}
    return true;
}
}
#include "native_session_state.inc"
NativeSession::NativeSession(AuthoritativeServer& server,unsigned capacity,std::string level,Log log):server_(&server),log_(std::move(log)),capacity_(capacity),level_(std::move(level)) {
    ssne::set_native_results_save_host(true);
    ssne::set_native_movie_vote_gate(true);
    for(unsigned slot=0;slot<std::min(capacity,4u);++slot){setup_["GameSetup.Player"+std::to_string(slot+1)+".Profile"]=int64_t(slot);setup_["GameSetup.Player"+std::to_string(slot+1)+".Pos"]=int64_t(slot);}
    for(unsigned slot=capacity;slot<4;++slot)setup_["GameSetup.Player"+std::to_string(slot+1)+".Profile"]=int64_t(-1);
    setup_["GameSetup.RulesCoop.Level"]=level_;
    // Native head-name objects outlive network occupancy and overlap for large
    // parties. The participant roster presents network names/health/AFK instead.
    setup_["GameSetup.DisplayNamesAbovePlayersHeads"]=int64_t(0);
}
NativeSession::NativeSession(AuthoritativeClient& client,Log log):client_(&client),log_(std::move(log)){ssne::set_native_results_save_host(false);}
NativeSession::~NativeSession(){ssne::set_native_movie_vote_gate(false);clear_native_afk_manual_aim();ssne::disable_native_rocket_effect_capture();}
NativeSession::CombatFeedback NativeSession::combat_feedback() const {const auto own=server_?server_->status().player:client_->status().player;std::lock_guard lock(combat_mutex_);if(own!=combat_owner_)return {};auto result=combat_feedback_;result.flash=std::chrono::steady_clock::now()<combat_flash_until_;return result;}
std::array<NativeSession::ParticipantHealth,max_players> NativeSession::participant_health() const {std::lock_guard lock(combat_mutex_);return participant_health_;}
bool NativeSession::request_lobby_campaign(bool resume){if(!server_)return false;std::lock_guard lock(lobby_mutex_);if(lobby_campaign_request_)return false;lobby_campaign_request_=resume;return true;}
void NativeSession::accept_confirmed_hit(const ssne::NativeConfirmedHit& hit,PlayerId own){if(hit.slot!=own.slot || hit.generation!=own.generation)return;std::lock_guard lock(combat_mutex_);if(combat_owner_!=own){combat_owner_=own;combat_feedback_={};}++combat_feedback_.hits;combat_feedback_.kills+=hit.after<=0;combat_feedback_.last_damage=hit.amount;combat_flash_until_=std::chrono::steady_clock::now()+std::chrono::milliseconds(350);if(log_)log_("[coop-own-hit-feedback] sequence="+std::to_string(hit.sequence)+" slot="+std::to_string(own.slot)+" generation="+std::to_string(own.generation)+" hits="+std::to_string(combat_feedback_.hits)+" kills="+std::to_string(combat_feedback_.kills));}
void NativeSession::report(const std::string& error){if(error!=last_error_){last_error_=error;if(log_)log_("[coop-engine] "+error);}}
void NativeSession::capture_fixture(const NativeWorldSample& sample,const PPCContext& context) {
    if(fixture_dumped_ || !env_requested("SSNE_COOP_CAPTURE_RAM") || sample.scene!=4 || sample.tick<600)return;
    auto required=std::min(capacity_,ssne::native_party_capacity());
    // Private two-peer fixtures need the matching relocated party component,
    // without waiting for every configured slot to become occupied.
    if(env_requested("SSNE_TEST_HIDDEN"))if(const auto* value=std::getenv("SSNE_COOP_CAPTURE_MIN_PLAYERS")){
        char* end=nullptr;const auto count=std::strtoul(value,&end,10);
        if(end!=value && !*end && count>=1 && count<=8)required=std::min(required,unsigned(count));
    }
    if(std::count_if(sample.entities.begin(),sample.entities.end(),[](const auto& e){return e.second.kind==EntityKind::Player;})<required)return;
    auto* ram=fopen("native-capture.ram","wb");if(!ram)return;
    const bool written=fwrite(ssne::g_ram,1,ssne::guest_ram_size(),ram)==ssne::guest_ram_size();fclose(ram);if(!written)return;
    // Local regression fixtures include the actual PPC quantization registers
    // and locked cache; raw RAM alone cannot reproduce native geometry queries.
    if(auto* cpu=fopen("native-capture-context.bin","wb")){fwrite(&context,1,sizeof(context),cpu);fclose(cpu);}
    if(auto* cache=fopen("native-capture-locked-cache.bin","wb")){fwrite(ssne::g_locked_cache,1,ssne::kLockedCacheSize,cache);fclose(cache);}
    // Relocated registry/arena ownership lives outside guest RAM. Capture it at
    // this same guest safe point, rather than guessing an eight-player layout.
    std::vector<uint8_t> party;std::string party_error;
    if(!ssne::native_party_save_state(party,party_error)){report("Native fixture party capture: "+party_error);return;}
    auto* party_file=fopen("native-capture-party.bin","wb");if(!party_file)return;
    const bool party_written=fwrite(party.data(),1,party.size(),party_file)==party.size();fclose(party_file);if(!party_written)return;
    auto* index=fopen("native-capture-objects.csv","w");if(!index)return;
    for(const auto& [id,e]:sample.entities)fprintf(index,"%llu,%u,%u,%u\n",(unsigned long long)id,native_.local_address(id),unsigned(e.kind),e.definition);
    fclose(index);
    auto* classes=fopen("native-capture-unmapped.csv","w");
    if(classes) {
        fprintf(classes,"object,vtable,update_group,update_method,position_x,position_y,position_z\n");
        for(const auto& item:sample.unmapped) {
            const auto getter=mem_read32(item.vtable+0x34);
            int group=-2;
            if(getter>=0x80000000u && getter<=ssne::guest_ram_end()-8 && (mem_read32(getter)&0xFFFF0000u)==0x38600000u && mem_read32(getter+4)==0x4E800020u)group=int16_t(mem_read32(getter));
            fprintf(classes,"%08X,%08X,%d,%08X,%.9g,%.9g,%.9g\n",item.address,item.vtable,group,mem_read32(item.vtable+0x3C),u2f(mem_read32(item.address+0x20)),u2f(mem_read32(item.address+0x24)),u2f(mem_read32(item.address+0x28)));
        }
        fclose(classes);
    }
    fixture_dumped_=true;
    auto* identity=fopen("native-capture-identity.csv","w");if(!identity)return;
    for(const auto& [id,e]:sample.entities) {
        const auto& current=std::get<std::array<float,3>>(e.properties.at("position"));
        const auto spawn=e.properties.contains("spawn_position")?std::get<std::array<float,3>>(e.properties.at("spawn_position")):current;
        fprintf(identity,"%llu,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",(unsigned long long)id,unsigned(e.kind),e.definition,current[0],current[1],current[2],spawn[0],spawn[1],spawn[2]);
    }
    fclose(identity);
}
bool NativeSession::initialize(PPCContext& ctx,const NativeBootstrapRequest& request,uint32_t presentation_scene) {
    if(mem_read32(ssne::game_address(0x802DB0E0))!=3)return false;
    auto profile=read_native_configuration(native_global_configuration(),"PlayerProfiles.Profile0");
    if(!profile.values.contains("PlayerProfiles.Profile0.ID"))return false;
    if(server_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES")){static bool dumped=false;if(!dumped){dump_native_profiles("native-profiles-boot.tsv");dumped=true;}}
    auto resolved=request;
    if(server_ && request.level=="continue"){
        const auto saved=profile.values.find("PlayerProfiles.Profile0.Level");
        if(saved==profile.values.end() || !std::holds_alternative<std::string>(saved->second)){report("Saved host campaign level is unavailable");return false;}
        const auto& level=std::get<std::string>(saved->second);
        if(level.empty() || level.size()>64 || std::any_of(level.begin(),level.end(),[](unsigned char c){return !std::isalnum(c) && c!='_';})){report("Saved host campaign level is invalid");return false;}
        resolved.level=level;resolved.game_setup["GameSetup.RulesCoop.Level"]=level;
        if(log_)log_("[coop-native-campaign-resume] level="+level);
    }
    ssne::native_party_configure(request.mode==1?capacity_:4);
    std::string error;if(capacity_>2)ssne::native_party_prepare_lobby(ctx);
    if(server_ && !apply_native_lobby_skin_choices(ctx,error)){report(error);return false;}
    if(!install_native_credits(server_!=nullptr,error)){report(error);return false;}
    if(!ssne::install_native_rocket_effect_capture(server_!=nullptr,error)){report(error);return false;}
    if(server_)ssne::set_native_hit_identity_resolver(native_.hit_identity_resolver());
    if(presentation_scene){
        if(presentation_scene==3)mem_write32(ctx.r[13]-0x69d4,0);
        if(!write_native_configuration(ctx,native_global_configuration(),resolved.game_setup,"GameSetup",error)){report(error);return false;}
        const bool adopted=server_ && presentation_scene==3 && attach_existing_native_lobby(ctx);
        if(!adopted && !queue_native_coop_presentation(ctx,presentation_scene,error)){report(error);return false;}
    }else if(!queue_native_coop_level(ctx,resolved,error)){report(error);return false;}
    level_=resolved.level;if(auto prefix=native_rules_prefix(resolved.mode))setup_[std::string(prefix)+".Level"]=level_;queued_=true;report("native level queued: "+level_);return true;
}
void NativeSession::tick(PPCContext& ctx,uint64_t tick) {
    NativeStageTimer timing("tick",log_);
    // Each installation must leave its title scene before its native level loader
    // is available. This internal bootstrap pulse is never a remote menu request.
    ssne::PadState bootstrap;
    if(queued_)show_native_lobby(ctx);
    if(!queued_ && tick>=30 && mem_read32(ssne::game_address(0x802DB0E0))==2) {
        if(tick%60<3)bootstrap.buttons=ssne::PAD_START;
        else if(tick%60>=30 && tick%60<33)bootstrap.buttons=ssne::PAD_A;
    }
    if(server_) {
        if(queued_){std::string why;if(!apply_native_lobby_skin_choices(ctx,why))report(why);}
        std::optional<bool> menu_request;
        {std::lock_guard lock(lobby_mutex_);menu_request=std::exchange(lobby_campaign_request_,std::nullopt);}
        if(menu_request && queued_ && mem_read32(ssne::game_address(0x802DB0E0))==3) {
            std::string error;
            if(!*menu_request || mem_read32(ctx.r[13]-0x69d4)!=1){
                if(!open_native_lobby_level_selection(ctx,error))report("Cannot open level selection: "+error);
            }else{
                NativeBootstrapRequest request{"continue",setup_};
                auto profile=read_native_configuration(native_global_configuration(),"PlayerProfiles.Profile0");auto saved=profile.values.find("PlayerProfiles.Profile0.Level");if(saved!=profile.values.end())if(auto level=std::get_if<std::string>(&saved->second))request.level=*level;
                if(queue_native_coop_level(ctx,request,error)){level_=request.level;setup_["GameSetup.RulesCoop.Level"]=level_;report("lobby campaign queued: "+level_);}
                else report("Cannot start lobby campaign: "+error);
            }
        }
        if(auto selected=take_native_lobby_level()){
            auto selected_setup=setup_;
            auto native_rules=read_native_configuration(native_global_configuration(),"GameSetup");
            if(native_rules.error.empty())for(const auto& [key,value]:native_rules.values)if(key.starts_with(std::string(native_rules_prefix(selected->mode))+"."))selected_setup[key]=value;
            selected_setup["GameSetup.RulesCoop.LevelSet"]=selected->level_set;
            selected_setup["GameSetup.RulesCoop.Difficulty"]=selected->difficulty;
            ssne::native_party_configure(selected->mode==1?capacity_:4);
            NativeBootstrapRequest request{selected->level,std::move(selected_setup),selected->mode};
            std::string error;
            if(queue_native_coop_level(ctx,request,error)){
                level_=request.level;setup_=std::move(request.game_setup);setup_[std::string(native_rules_prefix(request.mode))+".Level"]=level_;
                report("original lobby selection queued: "+level_+" difficulty="+std::to_string(selected->difficulty));
            }else{cancel_native_lobby_level_selection();report("Cannot start selected campaign level: "+error);}
        }
        for(auto event:server_->take_events()) {
            if(log_)log_("[coop-native-membership] slot="+std::to_string(event.player.slot)+" generation="+std::to_string(event.player.generation)+" joined="+std::to_string(event.kind==ConnectionEvent::Kind::Joined));
            if(event.kind==ConnectionEvent::Kind::Joined){
                players_[event.player.slot]=event.player;native_.set_player_owner(event.player.slot,event.player);
                if(queued_ && host_ready_ && mem_read32(ssne::game_address(0x802DB0E0))==4){join_placements_[event.player.slot]={event.player};if(log_)log_("[coop-native-join-placement-pending] slot="+std::to_string(event.player.slot)+" generation="+std::to_string(event.player.generation));}
            }
            else {ssne::native_party_cancel_join_spawn(join_placements_[event.player.slot].actor);join_placements_[event.player.slot]={};players_[event.player.slot]={};characters_[event.player.slot]=0;native_.set_player_owner(event.player.slot,{});}
        }
        if(!queued_ && (tick>=30 || (level_=="lobby" && mem_read32(ssne::game_address(0x802DB0E0))==3))){
            const bool menu=level_=="lobby";
            if(initialize(ctx,{menu?"Rlevel1_1":level_,setup_},menu?3:0) && menu)show_native_lobby(ctx);
        }
        if(startup_save_){
            for(unsigned slot=0;slot<capacity_;++slot)ssne::native_party_set_input(slot,slot==0?bootstrap:ssne::PadState{});
            ssne::set_pad(0,bootstrap);ssne::si_poll();return;
        }
        // An inactive waiting actor may not receive its normal native update.
        // Retry on the authoritative guest tick independently of actor activity.
        if(queued_ && mem_read32(ssne::game_address(0x802DB0E0))==4)ssne::native_party_retry_waiting(ctx);
        auto controls=server_->take_controls();
        if(native_lobby_visible() && (controls[0].input.pressed&ssne::PAD_B)){
            back_native_lobby(ctx);controls[0].input.pad.buttons&=~ssne::PAD_B;controls[0].input.pressed&=~ssne::PAD_B;
        }
        if(native_lobby_visible())for(unsigned slot=0;slot<capacity_;++slot)
            if(controls[slot].id.generation)native_lobby_character_input(ctx,slot,controls[slot].input.pressed);
        uint8_t occupied=0,connected=0;for(unsigned slot=0;slot<capacity_;++slot){if(controls[slot].ready)occupied|=uint8_t(1u<<slot);if(controls[slot].id.generation)connected|=uint8_t(1u<<slot);}
        ssne::native_party_set_occupancy(occupied,connected);
        ssne::native_party_refresh_occupancy(ctx);
        if(queued_ && mem_read32(ssne::game_address(0x802DB0E0))==4 && !server_->skip_vote().cutscene){
            for(unsigned slot=0;slot<capacity_;++slot){auto& placement=join_placements_[slot];if(!placement.player.generation || placement.player!=players_[slot])continue;
                if(!placement.requested){
                    const auto actor=mem_read32(ssne::native_party_registry()+slot*4);if(!ssne::native_player_collision_radius(actor))continue;
                    placement.actor=actor;placement.spawn_revision=ssne::native_party_spawn_revision(actor);placement.requested=true;
                    std::string why;if(!ssne::native_party_request_join_spawn(ctx,actor,why))report(why);
                }
                if(ssne::native_party_spawn_revision(placement.actor)>placement.spawn_revision && !ssne::native_party_spawn_waiting(placement.actor)){
                    if(log_)log_("[coop-native-join-placement-complete] slot="+std::to_string(slot)+" generation="+std::to_string(placement.player.generation)+" actor="+std::to_string(placement.actor)+" spawn_revision="+std::to_string(ssne::native_party_spawn_revision(placement.actor)));placement={};
                }
            }
        }
        update_afk_controls(ctx,tick,controls);
        for(unsigned slot=0;slot<capacity_;++slot) {
            const auto actor=mem_read32(ssne::native_party_registry()+slot*4);const bool waiting=ssne::native_party_spawn_waiting(actor);
            auto input=controls[slot].afk?afk_inputs_[slot]:controls[slot].input;
            if(controls[slot].afk && afk_native_active_[slot]){
                auto& pending=afk_inputs_[slot];
                const float seconds=u2f(mem_read32(ctx.r[13]-0x7A08u));
                input.yaw=consume_afk_turn(pending.yaw,seconds);
                input.pitch=consume_afk_turn(pending.pitch,seconds);
                // Keep the requested movement in world space while turning to
                // shoot. Otherwise a cached forward input bends away from the
                // leader until the next perception decision.
                const float turn=input.yaw/1000000.f;
                const float right=int(pending.pad.stick_x)-128,forward=int(pending.pad.stick_y)-128;
                input.pad.stick_x=uint8_t(std::clamp(128+int(std::round(right*std::cos(turn)-forward*std::sin(turn))),0,255));
                input.pad.stick_y=uint8_t(std::clamp(128+int(std::round(right*std::sin(turn)+forward*std::cos(turn))),0,255));
                pending.pad.stick_x=input.pad.stick_x;pending.pad.stick_y=input.pad.stick_y;
            }
            if(slot && controls[slot].ready && !controls[slot].afk && !waiting){std::string why;if(!apply_native_preferences(ctx,actor,input.preferences,why)){report(why);return;}}
            auto pad=waiting?ssne::PadState{}:input.pad;
            if(!waiting && controls[slot].ready && !controls[slot].afk && input.weapon>=16 && input.weapon<25)
                ssne::coop_weapon_group(slot,int(input.weapon)-16);
            if(slot==0 && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_ADAPTIVE_LEVEL")) {
                const bool actor_valid=actor>=0x80000000u && actor<=ssne::guest_ram_end()-0x790u;
                const auto requested=env_present("SSNE_COOP_TEST_ADAPTIVE_WEAPON")?unsigned(atoi(getenv("SSNE_COOP_TEST_ADAPTIVE_WEAPON"))):2u;
                const bool ready=controls[0].ready && !waiting && actor_valid && requested<12 && mem_read32(ssne::game_address(0x802DB0E0))==4 && mem_read32(actor+0x5DC)==requested && !mem_read32(actor+0x370);
                static bool aiming=false;static std::chrono::steady_clock::time_point stable{};
                const auto now=std::chrono::steady_clock::now();
                const auto weapon=actor_valid && requested<12?mem_read32(actor+0x4BC+requested*24):0;
                const bool valid=weapon>=0x80000000u && weapon<=ssne::guest_ram_end()-0x228u;
                if(!aiming){if(!ready || !valid || mem_read32(weapon+0x1B4)!=0)stable={};else if(stable.time_since_epoch().count()==0)stable=now;else if(now-stable>=std::chrono::seconds(2)){aiming=true;if(log_)log_("[coop-test-adaptive-ready] tick="+std::to_string(tick));}}
                const auto pitch=actor_valid?u2f(mem_read32(actor+0x294)):0.f;
                const auto people=server_->participants();
                const bool all_ready=std::count_if(people.begin(),people.end(),[](const auto& person){return person.connected && person.ready;})==capacity_;
                if(ready && aiming && std::isfinite(pitch) && std::abs(pitch)<1.05f)pad.cstick_y=255;
                const auto limit=env_present("SSNE_COOP_TEST_ADAPTIVE_IMPACT_LIMIT")?std::strtoul(getenv("SSNE_COOP_TEST_ADAPTIVE_IMPACT_LIMIT"),nullptr,10):0;
                const auto impacts=ssne::native_bullet_impact_history();const bool finished=limit && !impacts.empty() && impacts.back().sequence>=limit;
                if(ready && aiming && all_ready && !finished && std::isfinite(pitch) && std::abs(pitch)>=1.05f){pad.trigger_r=env_requested("SSNE_COOP_TEST_ADAPTIVE_SOFT_FIRE")?180:255;if(pad.trigger_r==255)pad.buttons|=ssne::PAD_R;}
                if(tick%120==0 && log_){
                    log_("[coop-test-adaptive-aim] pitch="+std::to_string(pitch)+" all_ready="+std::to_string(all_ready)+" variant="+std::to_string(valid?mem_read32(weapon+0x1E0):~0u)+" weapon_state="+std::to_string(valid?mem_read32(weapon+0x1B4):~0u));}
            }
            if(slot==0 && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_FIRE_READY")) {
                static std::chrono::steady_clock::time_point stable{},began{};const auto now=std::chrono::steady_clock::now();
                const bool actor_valid=actor>=0x80000000u && actor<=ssne::guest_ram_end()-0x790u;const auto requested=env_present("SSNE_COOP_TEST_READY_WEAPON")?std::clamp(std::atoi(getenv("SSNE_COOP_TEST_READY_WEAPON")),0,11):6;const auto weapon=actor_valid?mem_read32(actor+0x4BC+unsigned(requested)*24):0;
                const bool ready=controls[0].ready && !waiting && mem_read32(ssne::game_address(0x802DB0E0))==4 && actor_valid && mem_read32(actor+0x5DC)==unsigned(requested) && !mem_read32(actor+0x370) && weapon>=0x80000000u && weapon<=ssne::guest_ram_end()-0x228u && mem_read32(weapon+0x1B4)==0;
                if(began.time_since_epoch().count()==0){if(!ready)stable={};else if(stable.time_since_epoch().count()==0)stable=now;else if(now-stable>=std::chrono::seconds(2)){began=now;if(log_)log_("[coop-test-ready-fire] tick="+std::to_string(tick)+" weapon="+std::to_string(weapon));}}
                ssne::native_test_fire_request(began.time_since_epoch().count()!=0 && now-began<std::chrono::seconds(12));
            }
            if(tick%120==0 && log_)log_("[coop-control] tick="+std::to_string(tick)+" slot="+std::to_string(slot)+" ready="+std::to_string(controls[slot].ready)+" sequence="+std::to_string(controls[slot].sequence)+" buttons="+std::to_string(pad.buttons)+" x="+std::to_string(pad.stick_x)+" y="+std::to_string(pad.stick_y));
            if(slot==0 && !queued_)pad=bootstrap;
            if(native_lobby_visible())pad={}; // Character edges were consumed by the owner's panel above.
            // Remote players never operate shared menus, including while loading.
            if(slot && mem_read32(ssne::game_address(0x802DB0E0))!=4)pad={};
            if(slot)pad.buttons&=~ssne::PAD_START;
            // AFK governs gameplay; the authority still owns its human result UI.
            if(slot==0 && ssne::native_results_presenting())pad=controls[0].input.pad;
            ssne::native_party_set_input(slot,pad);if(slot<4)ssne::set_pad(slot,pad);
            ssne::netplay_mouse(int(slot),waiting?0:input.yaw/1000000.,waiting?0:input.pitch/1000000.);
        }
        ssne::si_poll();
    } else {
        auto state=client_->latest_world();auto status=client_->status();
        const auto own_actor=mem_read32(ssne::native_party_registry()+status.player.slot*4);
        {std::string why;if(!refresh_native_aim_preferences(ctx,own_actor,true,why)){report(why);return;}}
        uint64_t preference_bits=0;const auto preferences=capture_native_preferences(own_actor);for(unsigned i=0;i<preferences.size();++i)preference_bits|=uint64_t(preferences[i])<<(i*8);local_preferences_.store(preference_bits);
        if(!status.connected || !state->revision || state->level.empty()){weapon_sounds_.clear();ssne::clear_native_cutscene_presentation();replica_cutscene_id_=0;std::string why;ssne::stop_native_replica_speech(ctx,why);ssne::stop_native_weapon_loops(ctx,why);stop_native_vehicle_audio(ctx,why);return;}
        capacity_=status.capacity;
        if(auto flags=state->mission.find("Session.Mods.Flags");flags!=state->mission.end())if(auto value=std::get_if<int64_t>(&flags->second)){const auto rate=state->mission.find("Session.Mods.FireMultiplier");if(rate!=state->mission.end())if(auto multiplier=std::get_if<float>(&rate->second))ssne::mod_shared_options(uint32_t(*value),*multiplier);}
        if(mem_read32(ssne::game_address(0x802DB0E0))>=3){
            std::string error;
            if(!receive_native_lobby_characters(ctx,state->mission,error)){report(error);return;}
        }
        if(status.player.slot<max_players && !state->players[status.player.slot].character)client_ready_=false;
        uint8_t occupied=0,connected=0;
        for(unsigned slot=0;slot<capacity_;++slot){const auto& player=state->players[slot];players_[slot]=player.connected?player.id:PlayerId{};native_.set_player_owner(slot,players_[slot]);if(player.connected)connected|=uint8_t(1u<<slot);if(player.connected && player.ready)occupied|=uint8_t(1u<<slot);}
        ssne::native_party_set_occupancy(occupied,connected);
        ssne::native_party_refresh_occupancy(ctx);
        const auto presentation=state->mission.find("Session.Presentation.Scene");
        const auto presentation_scene=presentation==state->mission.end()?0u:uint32_t(std::get<int64_t>(presentation->second));
        // The transport can connect before the host has prepared its character
        // profiles. Loading that first snapshot would instantiate local default
        // models; updating profile values afterwards cannot replace their live
        // skeletons. Wait for the host's model list before starting the loader.
        if(!queued_ && tick>=30 && state->mission.contains("Session.CharacterModels")) {
            // Host metadata includes GameSetup separately from mission variables.
            Properties setup;for(const auto& [key,value]:state->mission)if(key.starts_with("GameSetup."))setup[key]=value;
            if(initialize(ctx,{state->level,setup,state->mission.contains("Session.GameMode")?uint32_t(std::get<int64_t>(state->mission.at("Session.GameMode"))):1u},presentation_scene)){loading_epoch_=state->epoch;level_loading_=true;}
        }
        if(queued_ && presentation_scene) {
            if(presentation_scene==5){std::string error;if(!receive_native_credits(state->mission,error)){report(error);return;}}
            const auto scene=mem_read32(ssne::game_address(0x802DB0E0));
            const auto pending=mem_read32(ssne::game_address(0x802DB0E4));
            // A snapshot may still name credits while its authoritative exit
            // step has already completed the local fade. Do not restart it.
            const bool credits_done=presentation_scene==5 && native_credits_finishing() && (scene==3 || pending==3);
            if(!credits_done && scene!=presentation_scene && pending!=presentation_scene && (scene==3 || scene==4 || scene==5)) {
                std::string error;
                native_frontend_prepare_results_close(ctx);
                if(!ssne::clear_native_replica_results(ctx,error) || !ssne::stop_native_weapon_loops(ctx,error) || !stop_native_vehicle_audio(ctx,error)){report(error);return;}
                weapon_sounds_.clear();weapon_sound_order_=0;replica_effect_time_={};replica_hud_clock_={};
                if(!ssne::stop_native_replica_speech(ctx,error)){report(error);return;}
                ssne::clear_native_cutscene_presentation();replica_cutscene_id_=0;
                if(!queue_native_coop_presentation(ctx,presentation_scene,error)){report(error);return;}
                report("authoritative presentation queued: scene="+std::to_string(presentation_scene)+" epoch="+std::to_string(state->epoch));
            }
            loading_epoch_=state->epoch;level_loading_=true;client_ready_=false;last_revision_=0;
            if(tick%120==0 && log_)log_("[coop-client-presentation] scene="+std::to_string(scene)+" target="+std::to_string(presentation_scene)+" epoch="+std::to_string(state->epoch));
        }
        const auto local_scene=mem_read32(ssne::game_address(0x802DB0E0));
        const auto phase=state->mission.find("Session.NativePhase");
        const bool return_to_game=!presentation_scene && phase!=state->mission.end() && phase->second==Property(std::string("gameplay")) &&
            (local_scene==3 || local_scene==5) && mem_read32(ssne::game_address(0x802DB0E4))!=4;
        if(queued_ && !presentation_scene && (return_to_game || state->level!=level_ || (loading_epoch_ && state->epoch!=loading_epoch_))) {
            const auto scene=mem_read32(ssne::game_address(0x802DB0E0));
            if(scene==3 || scene==4 || scene==5) {
                Properties setup;for(const auto& [key,value]:state->mission)if(key.starts_with("GameSetup."))setup[key]=value;
                std::string error;
                // Retire the old shared result UI while its native animation
                // targets still belong to the old level. Close any result modal first.
                native_frontend_prepare_results_close(ctx);
                if(!ssne::clear_native_replica_results(ctx,error)){report(error);return;}
                // Scene==4 can remain visible while its original loader frees
                // the old scene. Stop presentation BEFORE queuing that loader.
                if(!level_loading_) {
                    if(!ssne::stop_native_weapon_loops(ctx,error) || !stop_native_vehicle_audio(ctx,error)){report(error);return;}
                    weapon_sounds_.clear();weapon_sound_order_=0;replica_effect_time_={};replica_hud_clock_={};
                }
                const bool was_loading=level_loading_;level_loading_=true;
                // Only an existing gameplay scene needs proof that its old
                // actors were destroyed. Leaving the initial lobby has no
                // LevelExit event, so its first level keeps native epoch 1.
                const auto previous_native_epoch=scene==4?native_.capture(tick).epoch:0;
                ssne::native_party_configure(state->mission.contains("Session.GameMode") && std::get<int64_t>(state->mission.at("Session.GameMode"))!=1?4:capacity_);
                if(queue_native_coop_level(ctx,{state->level,setup,state->mission.contains("Session.GameMode")?uint32_t(std::get<int64_t>(state->mission.at("Session.GameMode"))):1u},error)) {
                    ssne::clear_native_cutscene_presentation();replica_cutscene_id_=0;
                    level_=state->level;loading_epoch_=state->epoch;level_loading_=true;loading_native_epoch_=previous_native_epoch;
                    client_ready_=false;last_revision_=0;replica_.reset();enemy_pose_expected_.clear();
                    report("authoritative level reload queued: "+level_+" epoch="+std::to_string(loading_epoch_)+" old_native_epoch="+std::to_string(loading_native_epoch_));
                } else {level_loading_=was_loading;report(error);}
            }
        }
        if(queued_)ssne::set_replica_simulation(true);
        for(unsigned slot=0;slot<capacity_;++slot) {
            // Keep the GUI's local input separate from gameplay authority.
            // The replica handlers consume Start/Tab in both controller banks.
            const bool own_ui=slot==status.player.slot && queued_ && local_scene==4;
            auto pad=own_ui?ssne::get_pad(slot):slot==0?bootstrap:ssne::PadState{};
            auto native_pad=pad;if(own_ui)native_pad.buttons&=~(ssne::PAD_START|ssne::PAD_Z);
            ssne::native_party_set_input(slot,native_pad);ssne::set_pad(slot,pad);
        }
    }
}
void NativeSession::update_afk_controls(PPCContext& context,uint64_t tick,const std::array<PlayerControl,max_players>& controls){
    std::array<bool,max_players> handed_over{};
    std::array<bool,max_players> live_humans{};
    if(queued_ && mem_read32(ssne::game_address(0x802DB0E0))==4){
        for(unsigned slot=0;slot<capacity_;++slot){
            auto& wait=native_life_wait_[slot];const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
            // Loading a checkpoint temporarily revokes network readiness, but
            // the restored native death still requires its original spawn.
            if(wait.actor && (wait.player!=controls[slot].id || wait.actor!=actor)){wait={};}
            if(!controls[slot].ready || !ssne::native_player_collision_radius(actor))continue;
            const auto hp=u2f(mem_read32(actor+0xCC));const bool dead=mem_read32(actor+0x328)!=0 || hp<=0;
            if(dead && !wait.actor){
                wait={controls[slot].id,actor,ssne::native_party_spawn_revision(actor)};
                if(log_)log_("[coop-player-await-native-life] slot="+std::to_string(slot)+" actor="+std::to_string(actor)+" spawn_revision="+std::to_string(wait.spawn_revision));
            }
            if(wait.actor && ssne::native_party_spawn_revision(actor)>wait.spawn_revision){
                if(log_)log_("[coop-player-native-life-restored] slot="+std::to_string(slot)+" actor="+std::to_string(actor)+" spawn_revision="+std::to_string(ssne::native_party_spawn_revision(actor)));wait={};
            }
            live_humans[slot]=!controls[slot].afk && !wait.actor && !dead && std::isfinite(hp) && hp>0 && !ssne::native_party_spawn_waiting(actor);
        }
    }else native_life_wait_={};
    for(unsigned slot=0;slot<capacity_;++slot){auto& lease=afk_aim_leases_[slot];if(!lease.actor)continue;
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(!controls[slot].afk || !controls[slot].ready || lease.player!=controls[slot].id || lease.actor!=actor){
            std::string manual_error;set_native_afk_manual_aim(context,lease.actor,false,manual_error);
            if(lease.actor==actor && lease.player==controls[slot].id && actor>=0x80000000u && actor<=ssne::guest_ram_end()-0x790u && mem_read32(actor+0x37C)==lease.aim && log_)log_("[coop-afk-auto-aim-restored] slot="+std::to_string(slot)+" enabled="+std::to_string(mem_read32(lease.aim+0x48)));
            lease={};
        }
    }
    // Hidden integration evidence spans the exact control transfer, before
    // any subsequent native movement/combat tick can legitimately change it.
    struct TransferCapture {unsigned slot;uint32_t actor,inventory;std::vector<uint8_t> actor_bytes,inventory_bytes;};
    std::vector<TransferCapture> transfers;
    auto bytes=[](uint32_t address,unsigned count){std::vector<uint8_t> out(count);for(unsigned i=0;i<count;++i)out[i]=mem_read8(address+i);return out;};
    if(env_requested("SSNE_TEST_HIDDEN"))for(unsigned slot=0;slot<capacity_;++slot)if(controls[slot].afk!=afk_states_[slot]){
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(!ssne::native_player_collision_radius(actor))continue;
        const auto inventory=mem_read32(actor+0x5E8);
        if(inventory<0x80000000u || inventory>ssne::guest_ram_end()-0xDC)continue;
        transfers.push_back({slot,actor,inventory,bytes(actor,0x790),bytes(inventory,0xDC)});
    }
    struct OnExit {std::function<void()> action;~OnExit(){action();}} evidence{[&]{
        for(const auto& transfer:transfers){
            const auto actor=mem_read32(ssne::native_party_registry()+transfer.slot*4);
            const bool same=actor==transfer.actor && mem_read32(actor+0x5E8)==transfer.inventory;
            const bool actor_same=same && bytes(actor,0x790)==transfer.actor_bytes;
            const bool inventory_same=same && bytes(transfer.inventory,0xDC)==transfer.inventory_bytes;
            if(log_)log_("[coop-afk-transfer-preserved] tick="+std::to_string(tick)+" slot="+std::to_string(transfer.slot)+" afk="+std::to_string(controls[transfer.slot].afk)+" actor="+std::to_string(actor)+" same_actor="+std::to_string(same)+" actor_bytes_equal="+std::to_string(actor_same)+" inventory_bytes_equal="+std::to_string(inventory_same));
        }
    }};
    for(unsigned slot=0;slot<capacity_;++slot)if(controls[slot].afk!=afk_states_[slot]){
        handed_over[slot]=true;
        afk_states_[slot]=controls[slot].afk;
        if(log_)log_("[coop-afk-state] tick="+std::to_string(tick)+" slot="+std::to_string(slot)+" afk="+std::to_string(controls[slot].afk)+" ready="+std::to_string(controls[slot].ready)+" actor="+std::to_string(mem_read32(ssne::native_party_registry()+slot*4)));
    }
    for(unsigned slot=0;slot<max_players;++slot)if(!controls[slot].afk || !controls[slot].ready){
        afk_inputs_[slot]={};
        if(!controls[slot].afk)afk_respawn_wait_[slot]={};
        if(afk_native_active_[slot]){afk_native_active_[slot]=false;afk_bots_[slot]={};if(log_)log_("[coop-afk-native-control] slot="+std::to_string(slot)+" enabled=0 actor="+std::to_string(mem_read32(ssne::native_party_registry()+slot*4)));}
    }
    // Death and respawn can start between the budgeted AI decisions. Discard
    // cached movement/fire on that very guest tick, including native dead flag
    // +328 (the original kill path can set it before health/phase changes).
    for(unsigned slot=0;slot<capacity_;++slot)if(controls[slot].afk && controls[slot].ready){
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        const bool valid=ssne::native_player_collision_radius(actor).has_value();
        auto& wait=afk_respawn_wait_[slot];
        if(wait.actor && wait.actor!=actor)wait={};
        if(valid && !wait.actor && (mem_read32(actor+0x328)!=0 || native_life_wait_[slot].actor==actor)){
            const auto& life=native_life_wait_[slot];
            wait={actor,life.actor==actor?life.spawn_revision:ssne::native_party_spawn_revision(actor)};
            if(log_)log_("[coop-afk-await-native-respawn] slot="+std::to_string(slot)+" actor="+std::to_string(actor)+" spawn_revision="+std::to_string(wait.spawn_revision));
        }
        if(wait.actor && ssne::native_party_spawn_revision(actor)>wait.spawn_revision){if(log_)log_("[coop-afk-native-respawn-observed] slot="+std::to_string(slot)+" actor="+std::to_string(actor)+" spawn_revision="+std::to_string(ssne::native_party_spawn_revision(actor)));wait={};}
        const bool suspended=!valid || wait.actor || mem_read32(actor+0x328)!=0 || !(u2f(mem_read32(actor+0xCC))>0) || mem_read32(actor+0x370)!=0 || ssne::native_party_spawn_waiting(actor);
        if(suspended){
            afk_inputs_[slot]={};
            if(afk_native_active_[slot]){afk_native_active_[slot]=false;afk_bots_[slot]={};if(log_)log_("[coop-afk-native-control] slot="+std::to_string(slot)+" enabled=0 actor="+std::to_string(actor)+" suspended=1 tick="+std::to_string(tick)+" native_dead="+std::to_string(valid?mem_read32(actor+0x328):0)+" respawn="+std::to_string(valid?mem_read32(actor+0x3AC):0));}
        }
    }
    if(!queued_ || mem_read32(ssne::game_address(0x802DB0E0))!=4 || server_->skip_vote().cutscene){afk_inputs_={};return;}
    if(std::none_of(controls.begin(),controls.end(),[](const auto& input){return input.ready && input.afk;}))return;
    const auto now=std::chrono::steady_clock::now();if(now<afk_next_decision_)return;afk_next_decision_=now+std::chrono::milliseconds(125);
    const auto previous_inputs=afk_inputs_;
    afk_inputs_={};
    const auto sample=native_.capture(tick);if(!sample.error.empty())return;
    if(afk_epoch_!=sample.epoch){afk_epoch_=sample.epoch;afk_bots_={};game_bots_={};afk_native_active_={};afk_respawn_wait_={};}
    std::array<const EntityState*,max_players> actors{};
    for(const auto& [id,entity]:sample.entities)if(entity.kind==EntityKind::Player){const auto slot=std::get<int64_t>(entity.properties.at("player_slot"));if(slot>=0 && slot<max_players)actors[slot]=&entity;}
    for(unsigned slot=0;slot<capacity_;++slot){
        const auto* actor=actors[slot];
        const bool active=controls[slot].ready && controls[slot].afk && !afk_respawn_wait_[slot].actor && actor && mem_read32(native_.local_address(actor->id)+0x328)==0 && std::get<float>(actor->properties.at("health"))>0 && std::get<int64_t>(actor->properties.at("camera_frozen"))==0 && !ssne::native_party_spawn_waiting(native_.local_address(actor->id));
        if(active!=afk_native_active_[slot]){afk_native_active_[slot]=active;afk_bots_[slot]={};if(log_)log_("[coop-afk-native-control] slot="+std::to_string(slot)+" enabled="+std::to_string(active)+" actor="+std::to_string(actor?native_.local_address(actor->id):0));}
        if(!active)continue;
        if(handed_over[slot])continue; // preserve transfer boundary before the first ordinary AI action
        const auto native_actor=native_.local_address(actor->id);auto& lease=afk_aim_leases_[slot];
        const auto aim_pointer=mem_read32(native_actor+0x37C);if(aim_pointer<0x80000000u || aim_pointer>ssne::guest_ram_end()-0x58u)continue;
        if(!lease.actor){lease={controls[slot].id,native_actor,aim_pointer,mem_read32(aim_pointer+0x48)!=0};if(log_)log_("[coop-afk-auto-aim-owned] slot="+std::to_string(slot)+" saved="+std::to_string(lease.saved));}
        {std::string why;if(!set_native_afk_manual_aim(context,native_actor,true,why)){report(why);continue;}}
        const auto p=std::get<std::array<float,3>>(actor->properties.at("position"));
        const auto movement=mem_read32(native_actor+0x344);
        const bool swimming=movement>=0x80000000u && movement<=ssne::guest_ram_end()-4 && mem_read32(movement)==ssne::game_address(0x80211030u);
        const EntityState* leader=nullptr;float nearest=1000000;unsigned follow=max_players;
        for(unsigned human=0;human<capacity_;++human)if(human!=slot && live_humans[human] && actors[human]){
            const auto h=std::get<std::array<float,3>>(actors[human]->properties.at("position"));const auto d=std::hypot(h[0]-p[0],h[2]-p[2]);if(d<nearest){nearest=d;leader=actors[human];follow=human;}
        }
        // Nearby humans crossing each other must not reset the entire route and
        // target every decision. Change leader when the alternative is meaningfully closer.
        const auto previous_follow=afk_follow_slots_[slot];
        if(previous_follow<capacity_ && previous_follow!=slot && live_humans[previous_follow] && actors[previous_follow]){
            const auto h=std::get<std::array<float,3>>(actors[previous_follow]->properties.at("position"));
            const auto d=std::hypot(h[0]-p[0],h[2]-p[2]);
            if(d<=nearest+3.f){nearest=d;leader=actors[previous_follow];follow=previous_follow;}
        }
        if(afk_follow_slots_[slot]!=follow)afk_bots_[slot]={};
        afk_follow_slots_[slot]=follow;if(!leader)leader=actor; // defend self if all humans are dead/absent; never follow another AFK bot
        ssne::BotWorld world;world.host.id=native_.local_address(leader->id);world.host.position=std::get<std::array<float,3>>(leader->properties.at("position"));world.host.health=std::get<float>(leader->properties.at("health"));
        world.client.id=native_.local_address(actor->id);world.client.position=p;world.client.health=std::get<float>(actor->properties.at("health"));world.yaw=std::get<float>(actor->properties.at("yaw"));world.pitch=std::get<float>(actor->properties.at("pitch"));
        std::array<bool,12> usable{};std::array<int,12> stock{};
        for(unsigned weapon=0;weapon<12;++weapon){std::string why;auto ammo=native_owned_primary_ammo(context,native_actor,weapon,why);if(ammo){stock[weapon]=*ammo;usable[weapon]=*ammo>0;}}
        float acquisition_range=bot_acquisition_range(usable);
        // Private same-binary countercheck only; ordinary sessions always use
        // the owned weapon ranges above.
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_TEST_AFK_RANGE_LIMIT")){
            const auto limit=std::strtof(getenv("SSNE_TEST_AFK_RANGE_LIMIT"),nullptr);
            if(std::isfinite(limit) && limit>=0.f && limit<=100.f)acquisition_range=std::min(acquisition_range,limit);
        }
        // Verified native world queries exclude blocked lines and unsafe steps.
        std::array<float,3> eye;std::string origin_error;if(!native_player_fire_origin(context,world.client.id,eye,origin_error))continue;
        for(const auto& [id,enemy]:sample.entities)if(enemy.kind==EntityKind::Enemy && enemy.properties.contains("health") && std::get<float>(enemy.properties.at("health"))>0){
            const auto address=native_.local_address(id);const auto height=u2f(mem_read32(address+0xD0));if(!std::isfinite(height) || height<.05f || height>200.f)continue;
            auto point=std::get<std::array<float,3>>(enemy.properties.at("position"));const auto distance=std::hypot(point[0]-p[0],point[2]-p[2]);auto aim=point;aim[1]+=height*.5f;
            std::optional<ssne::NativeSegmentHit> obstruction;std::string why;bool query_valid=false;
            const bool visible=bot_visible_target(distance,acquisition_range,[&]{query_valid=ssne::native_world_segment(context,eye,aim,obstruction,why);return query_valid && !obstruction;});
            if(visible)world.enemies.push_back({address,point,std::get<float>(enemy.properties.at("health")),height});
            static unsigned detailed_far_sight_samples=0;
            const bool detailed_far_sight=distance>30.f && detailed_far_sight_samples<4096;
            if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_BOT_FIRE_TRACE") && (afk_decisions_[slot]%8==0 || detailed_far_sight) && distance<=acquisition_range){
                if(detailed_far_sight)++detailed_far_sight_samples;
                log_("[coop-afk-sight] slot="+std::to_string(slot)+" target="+std::to_string(address)+" distance="+std::to_string(distance)+" range="+std::to_string(acquisition_range)+" query_valid="+std::to_string(query_valid)+" obstructed="+std::to_string(bool(obstruction))+" visible="+std::to_string(visible));
            }
        }
        if(server_->bot_mask()&(1u<<slot))for(const auto& [id,enemy]:sample.entities)if(auto parts=enemy.properties.find("boss_weakpoints");parts!=enemy.properties.end()){
            const auto& v=std::get<std::vector<float>>(parts->second);
            for(unsigned i=0;i<16;++i)if(v[i*7] && v[i*7+1]==1 && v[i*7+3]>0){
                const BotPoint q{v[i*7+4],v[i*7+5],v[i*7+6]};if(bot_distance(p,q)>acquisition_range)continue;
                std::optional<ssne::NativeSegmentHit> hit;std::string why;
                const auto part=mem_read32(native_.local_address(id)+0x1FC+i*4);
                if(part && ssne::native_world_segment(context,eye,q,hit,why) && !hit)world.enemies.push_back({part,q,v[i*7+3],.1f});
            }
        }
        auto route_clear=[&](const std::array<float,3>& goal){
            const float distance=std::hypot(goal[0]-p[0],goal[2]-p[2]);
            if(distance<2.f || distance>((server_->bot_mask()&(1u<<slot))?65.f:12.f) || std::abs(goal[1]-p[1])>.6f)return false;
            const auto radius=ssne::native_player_collision_radius(world.client.id);if(!radius)return false;
            ssne::NativeSphereResult path;std::string why;
            if(!ssne::native_world_sphere(context,p,goal,*radius,path,why) || std::hypot(path.position[0]-goal[0],path.position[2]-goal[2])>.15f || std::abs(path.position[1]-goal[1])>.3f)return false;
            const unsigned steps=unsigned(std::ceil(distance/1.25f));float previous_floor=0;
            for(unsigned step=0;step<=steps;++step){
                const float t=float(step)/steps;std::array<float,3> above,below;
                for(unsigned i=0;i<3;++i)above[i]=p[i]+t*(goal[i]-p[i]);
                below=above;above[1]+=.5f;below[1]-=2.f;
                std::optional<ssne::NativeSegmentHit> floor;
                if(!ssne::native_world_segment(context,above,below,floor,why) || !floor || floor->normal[1]<.7f || above[1]-.5f-floor->position[1]>*radius+.5f || (step && std::abs(floor->position[1]-previous_floor)>.5f))return false;
                previous_floor=floor->position[1];
            }
            if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES"))log_("[coop-afk-route-shortcut] slot="+std::to_string(slot)+" distance="+std::to_string(distance)+" supported_samples="+std::to_string(steps+1));
            return true;
        };
        #include "native_game_bots.inc"
        const auto previous_target=afk_bots_[slot].target();
        const auto decision=afk_bots_[slot].update(world,8,&eye,route_clear,true,automated,acquisition_range);ControlInput input;input.pad=decision.pad;input.yaw=decision.yaw;input.pitch=decision.pitch;
        if(automated){
            const auto target=afk_bots_[slot].target();if(target!=brain.target){brain.target=target;brain.reaction=skill==1?5:skill==2?3:1;}
            if(!target && bot_distance(world.host.position,p)<.7f && bot_distance(brain.goal,p)>2){const auto yaw=std::atan2(brain.goal[0]-p[0],brain.goal[2]-p[2]);input.yaw=int32_t(std::clamp(std::remainder(yaw-world.yaw,6.283185307f),-.5f,.5f)*1000000);}
            const int turn=skill==1?220000:skill==2?350000:500000;input.yaw=std::clamp(input.yaw,-turn,turn);input.pitch=std::clamp(input.pitch,-turn,turn);
            if(swimming && !target){const auto q=world.host.position;const auto desired=-std::atan2(q[1]-p[1],std::max(.1f,std::hypot(q[0]-p[0],q[2]-p[2])));input.pitch=int32_t(-std::clamp(desired-world.pitch,-.25f,.25f)*1000000);}
        }
        const auto combat_target=std::find_if(world.enemies.begin(),world.enemies.end(),[&](const auto& enemy){return enemy.id==afk_bots_[slot].target();});
        if(combat_target!=world.enemies.end()){
            auto point=combat_target->position;point[1]+=combat_target->height*.5f;
            const auto distance=std::hypot(point[0]-eye[0],point[2]-eye[2]);unsigned nearby=0;
            // Alternate deliberate firing windows and movement. Constant close
            // strafing outruns finite mouse turning and would prevent the
            // real muzzle from ever meeting its alignment threshold.
            if(automated && distance>=4 && distance<35 && !brain.pickup && bot_firing_stance(mem_read32(native_actor+0x5DC),afk_decisions_[slot]) &&
               (sample.mode!=4 || !mem_read32(native_actor+0x6A4)) && (sample.mode!=3 || !mem_read32(native_actor+0x694)) &&
               (!cooperative || nearest<12))input.pad.stick_x=input.pad.stick_y=128;
            for(const auto& enemy:world.enemies)if(std::hypot(enemy.position[0]-point[0],enemy.position[2]-point[2])<5.f)++nearby;
            bool explosive_safe=distance>=12.f,friend_blocks=false;
            const float dx=point[0]-eye[0],dy=point[1]-eye[1],dz=point[2]-eye[2],length2=dx*dx+dy*dy+dz*dz;
            for(unsigned other=0;other<capacity_;++other)if((!automated || cooperative) && other!=slot && actors[other] && std::get<float>(actors[other]->properties.at("health"))>0){
                auto q=std::get<std::array<float,3>>(actors[other]->properties.at("position"));
                if(std::hypot(q[0]-point[0],q[2]-point[2])<12.f && std::abs(q[1]-point[1])<8.f)explosive_safe=false;
                q[1]+=.8f;if(length2>.01f){const auto t=((q[0]-eye[0])*dx+(q[1]-eye[1])*dy+(q[2]-eye[2])*dz)/length2;
                    if(t>0 && t<1){const auto ox=q[0]-(eye[0]+t*dx),oy=q[1]-(eye[1]+t*dy),oz=q[2]-(eye[2]+t*dz);if(ox*ox+oy*oy+oz*oz<1.f)friend_blocks=true;}}
            }
            const auto selected=mem_read32(native_actor+0x5DC),chosen=automated?choose_game_bot_weapon(usable,selected,distance,nearby,explosive_safe):choose_afk_weapon(usable,selected,distance,nearby,explosive_safe);
            if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && afk_decisions_[slot]%8==0)log_("[coop-afk-weapon-evidence] slot="+std::to_string(slot)+" selected="+std::to_string(selected)+" chosen="+std::to_string(chosen)+" pistol_ammo="+std::to_string(stock[1])+" usable="+std::to_string(usable[1])+" friend_blocks="+std::to_string(friend_blocks));
            const bool same_trigger=previous_target==combat_target->id && selected==chosen && previous_inputs[slot].weapon==chosen && previous_inputs[slot].pad.trigger_r==255;
            std::array<float,3> actual_direction;std::string direction_error;bool native_aligned=false;float aim_cosine=-1.f;
            if(length2>.01f && native_player_fire_direction(context,native_actor,actual_direction,direction_error)){
                const auto length=std::sqrt(length2),dl=std::sqrt(actual_direction[0]*actual_direction[0]+actual_direction[1]*actual_direction[1]+actual_direction[2]*actual_direction[2]);
                if(dl>.01f)aim_cosine=(actual_direction[0]*dx+actual_direction[1]*dy+actual_direction[2]*dz)/(dl*length);
                native_aligned=bot_fire_aligned(chosen,aim_cosine,same_trigger,distance);
            }
            bool can_fire=chosen<12 && bot_weapon_in_range(chosen,distance) && !friend_blocks && native_aligned && (!automated || brain.reaction==0);
            if(automated && brain.reaction)--brain.reaction;
            if(chosen<12 && chosen!=selected){std::string why;can_fire=false;
                if(apply_native_weapon_selection(context,native_actor,chosen,true,why)){if(log_)log_("[coop-afk-weapon-choice] slot="+std::to_string(slot)+" weapon="+std::to_string(chosen)+" owned=1 primary_ammo="+std::to_string(stock[chosen])+" target="+std::to_string(combat_target->id));}
                else report(why);
            }
            if(chosen<12){std::string why;can_fire=can_fire && request_native_primary_ammo(context,native_actor,chosen,why);}
            // The native muzzle direction is the final firing authority. The
            // controller's separate yaw/pitch thresholds must not veto a shot
            // which already passed alignment, ownership and teammate checks.
            if(can_fire){input.pad.trigger_r=255;input.pad.buttons|=ssne::PAD_R;input.weapon=uint8_t(chosen);}
            else{input.pad.trigger_r=0;input.pad.buttons&=~ssne::PAD_R;}
            if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_BOT_FIRE_TRACE")){
                const auto instance=mem_read32(native_actor+0x4BC+selected*24);
                EntityId target_entity=0;for(const auto& [id,e]:sample.entities)if(native_.local_address(id)==combat_target->id){target_entity=id;break;}
                log_("[game-bot-fire-trace] tick="+std::to_string(tick)+" slot="+std::to_string(slot)+" target="+std::to_string(combat_target->id)+" weapon="+std::to_string(selected)+" chosen="+std::to_string(chosen)+" distance="+std::to_string(distance)+" target_entity="+std::to_string(target_entity)+" acquisition_range="+std::to_string(acquisition_range)+" cosine="+std::to_string(aim_cosine)+" held="+std::to_string(same_trigger)+" fire="+std::to_string(can_fire)+" ammo="+std::to_string(selected<12?stock[selected]:0)+" state="+std::to_string(mem_read32(instance+0x1B4))+" spin="+std::to_string(selected==4?u2f(mem_read32(instance+0x290)):0.f)+" friend_blocks="+std::to_string(friend_blocks));
            }
            if(friend_blocks){input.pad.stick_x=slot%2?224:32;input.pad.stick_y=128;}
        }else{input.pad.trigger_r=0;input.pad.buttons&=~ssne::PAD_R;}
        if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && afk_decisions_[slot]%8==0){
            const auto target=std::find_if(world.enemies.begin(),world.enemies.end(),[&](const auto& enemy){return enemy.id==afk_bots_[slot].target();});
            if(target!=world.enemies.end())log_("[coop-afk-aim-native] slot="+std::to_string(slot)+" eye="+std::to_string(eye[0])+","+std::to_string(eye[1])+","+std::to_string(eye[2])+" target="+std::to_string(target->position[0])+","+std::to_string(target->position[1]+target->height*.5f)+","+std::to_string(target->position[2])+" yaw="+std::to_string(world.yaw)+" pitch="+std::to_string(world.pitch));
            std::array<float,3> direction;std::string why;if(native_player_fire_direction(context,world.client.id,direction,why))log_("[coop-afk-fire-direction] slot="+std::to_string(slot)+" x="+std::to_string(direction[0])+" y="+std::to_string(direction[1])+" z="+std::to_string(direction[2])+" auto_aim="+std::to_string(mem_read32(mem_read32(world.client.id+0x37C)+0x44)));
        }
        // Steer around other existing players instead of pushing through the
        // whole group. Native movement and the geometry guard still decide
        // whether this ordinary input can actually move the character.
        if(input.pad.stick_x!=128 || input.pad.stick_y!=128){
            float right=(int(input.pad.stick_x)-128)/128.f,forward=(int(input.pad.stick_y)-128)/128.f;
            float dx=right*std::cos(world.yaw)+forward*std::sin(world.yaw),dz=-right*std::sin(world.yaw)+forward*std::cos(world.yaw);
            for(unsigned other=0;other<capacity_;++other)if((!automated || cooperative || sample.mode!=4 || !mem_read32(native_actor+0x6A4)) && other!=slot && actors[other] && std::get<float>(actors[other]->properties.at("health"))>0){
                const auto q=std::get<std::array<float,3>>(actors[other]->properties.at("position"));
                const float ox=q[0]-p[0],oz=q[2]-p[2],distance=std::hypot(ox,oz);
                if(distance>.01f && distance<2.8f && std::abs(q[1]-p[1])<2.f && ox*dx+oz*dz>0){
                    const float side=((slot+other)%2)?1.f:-1.f,strength=(2.8f-distance)/2.8f;
                    dx+=side*(-oz/distance)*strength-ox/distance*strength;
                    dz+=side*(ox/distance)*strength-oz/distance*strength;
                }
            }
            const auto length=std::hypot(dx,dz);if(length>.01f){dx*=.8f/length;dz*=.8f/length;right=dx*std::cos(world.yaw)-dz*std::sin(world.yaw);forward=dx*std::sin(world.yaw)+dz*std::cos(world.yaw);input.pad.stick_x=uint8_t(std::clamp(128+int(right*128),0,255));input.pad.stick_y=uint8_t(std::clamp(128+int(forward*128),0,255));}
        }
        if(!swimming && (input.pad.stick_x!=128 || input.pad.stick_y!=128)){
            const float right=(int(input.pad.stick_x)-128)/128.f,forward=(int(input.pad.stick_y)-128)/128.f;
            auto next=p;next[0]+=.8f*(right*std::cos(world.yaw)+forward*std::sin(world.yaw));next[2]+=.8f*(-right*std::sin(world.yaw)+forward*std::cos(world.yaw));
            std::optional<ssne::NativeSegmentHit> floor;ssne::NativeSphereResult path;std::string why;const auto radius=ssne::native_player_collision_radius(world.client.id);
            auto ground_above=p,ground_below=p;ground_above[1]+=.5f;ground_below[1]-=2.f;std::optional<ssne::NativeSegmentHit> ground;
            const bool supported=radius && ssne::native_world_segment(context,ground_above,ground_below,ground,why) && ground && ground->normal[1]>=.7f && p[1]-ground->position[1]<=*radius+.5f;
            auto resolve=[&](const std::array<float,3>& wanted,std::array<float,3>& accepted){
                if(!supported || !ssne::native_world_sphere(context,p,wanted,*radius,path,why))return false;
                const float dx=path.position[0]-p[0],dz=path.position[2]-p[2],wx=wanted[0]-p[0],wz=wanted[2]-p[2];
                const auto travel=std::hypot(dx,dz);
                // Native collision correction may slide along a wall. Follow
                // that resolved direction using ordinary input, never teleport.
                if(travel<.05f || travel>std::hypot(wx,wz)+.05f || dx*wx+dz*wz<.02f || std::abs(path.position[1]-p[1])>.6f)return false;
                auto above=path.position,below=path.position;above[1]+=1.5f;below[1]-=2.f;
                if(!ssne::native_world_segment(context,above,below,floor,why) || !floor || floor->normal[1]<.7f || floor->position[1]<ground->position[1]-.5f || floor->position[1]>ground->position[1]+.6f)return false;
                accepted=path.position;return true;
            };
            std::array<float,3> accepted{};bool safe=resolve(next,accepted);
            if(!safe){
                const float dx=next[0]-p[0],dz=next[2]-p[2],side=slot%2?1.f:-1.f;
                // Bounded local avoidance around a blocked step. Every option
                // must pass the same native collision and support-floor checks.
                for(float angle:{side*1.04719755f,-side*1.04719755f,side*1.57079633f,-side*1.57079633f}){
                    auto candidate=p;candidate[0]+=dx*std::cos(angle)-dz*std::sin(angle);candidate[2]+=dx*std::sin(angle)+dz*std::cos(angle);
                    if(resolve(candidate,accepted)){safe=true;break;}
                }
            }
            if(safe){
                const float dx=accepted[0]-p[0],dz=accepted[2]-p[2];
                input.pad.stick_x=uint8_t(std::clamp(128+int((dx*std::cos(world.yaw)-dz*std::sin(world.yaw))*160.f),0,255));
                input.pad.stick_y=uint8_t(std::clamp(128+int((dx*std::sin(world.yaw)+dz*std::cos(world.yaw))*160.f),0,255));
                if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && log_ && std::hypot(accepted[0]-next[0],accepted[2]-next[2])>.05f)log_("[coop-afk-native-detour] slot="+std::to_string(slot)+" from="+std::to_string(p[0])+","+std::to_string(p[1])+","+std::to_string(p[2])+" resolved="+std::to_string(accepted[0])+","+std::to_string(accepted[1])+","+std::to_string(accepted[2])+" floor_y="+std::to_string(floor->position[1]));
                if(input.pad.trigger_l){auto above=p;above[1]+=1.4f;ssne::NativeSphereResult clearance;
                    if(!ssne::native_world_sphere(context,p,above,*radius,clearance,why) || clearance.position[1]-p[1]<1.2f){input.pad.buttons&=~ssne::PAD_L;input.pad.trigger_l=0;}
                }
            }
            if(!safe){
                if(afk_decisions_[slot]%8==0 && log_)log_("[coop-afk-blocked-step] slot="+std::to_string(slot)+" floor="+std::to_string(bool(floor))+" normal="+std::to_string(floor?floor->normal[1]:0)+" floor_y="+std::to_string(floor?floor->position[1]:0)+" player_y="+std::to_string(p[1])+" radius="+std::to_string(radius?*radius:0)+" path_error="+std::to_string(std::hypot(path.position[0]-next[0],path.position[2]-next[2]))+" supported="+std::to_string(supported)+" from="+std::to_string(p[0])+","+std::to_string(p[1])+","+std::to_string(p[2])+" resolved="+std::to_string(path.position[0])+","+std::to_string(path.position[1])+","+std::to_string(path.position[2])+" error="+why);
                input.pad.stick_x=input.pad.stick_y=128;input.pad.buttons&=~ssne::PAD_L;input.pad.trigger_l=0;
            }
        }
        if(automated && brain.goal_age>16 && afk_decisions_[slot]%16==0)input.pad.buttons|=ssne::PAD_A;
        input.pad.buttons&=~ssne::PAD_START;afk_inputs_[slot]=input;
        ++afk_decisions_[slot];
        if(log_ && (afk_decisions_[slot]%8==0 || input.pad.stick_x!=128 || input.pad.stick_y!=128 || input.pad.trigger_r || input.pad.trigger_l))log_("[coop-afk-bot-input] slot="+std::to_string(slot)+" follow="+std::to_string(follow)+" target="+std::to_string(afk_bots_[slot].target())+" x="+std::to_string(input.pad.stick_x)+" y="+std::to_string(input.pad.stick_y)+" fire="+std::to_string(input.pad.trigger_r)+" jump="+std::to_string(input.pad.trigger_l)+" buttons="+std::to_string(input.pad.buttons));
        if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES"))log_("[coop-afk-motion] tick="+std::to_string(tick)+" slot="+std::to_string(slot)+" actor="+std::to_string(native_actor)+" x="+std::to_string(p[0])+" y="+std::to_string(p[1])+" z="+std::to_string(p[2])+" jump="+std::to_string(input.pad.trigger_l)+" follow="+std::to_string(follow)+" follow_distance="+std::to_string(nearest));
    }
}
bool NativeSession::drain_weapon_sounds(PPCContext& context) {
    const auto state=client_->latest_world();
    if(!client_->status().connected || !state || state->epoch!=replica_loop_epoch_){weapon_sounds_.clear();return true;}
    if(state->entities.empty()){
        weapon_sounds_.clear();std::string why;if(!ssne::stop_native_weapon_loops(context,why)){report(why);return false;}return true;
    }
    // Retain host order and retry actual native resource/voice contention.
    // The batch bound matches the original 48-record voice allocator.
    for(unsigned budget=0;budget<48 && !weapon_sounds_.empty();++budget){
        std::string why;const auto order=std::visit([](const auto& event){return event.order;},weapon_sounds_.front().event);
        const bool played=std::visit([&](const auto& event){using T=std::decay_t<decltype(event)>;if constexpr(std::is_same_v<T,ssne::NativeWeaponShot>)return ssne::play_native_weapon_shot(context,event,event.slot==client_->status().player.slot,why);else{auto pending=event;if(pending.active && pending.elapsed_ms)pending.elapsed_ms=uint32_t(std::min<int64_t>(60000,int64_t(pending.elapsed_ms)+std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-weapon_sounds_.front().received).count()));return ssne::apply_native_weapon_loop(context,pending,pending.slot==client_->status().player.slot,why);}},weapon_sounds_.front().event);
        if(!played){if(why=="Native weapon-shot voice was not started" || why=="Native Uzi loop voice not started"){if(log_)log_("[coop-weapon-sound-deferred] order="+std::to_string(order)+" pending="+std::to_string(weapon_sounds_.size()));return true;}report(why);return false;}
        if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES"))log_("[coop-weapon-sound-played] order="+std::to_string(order)+" wait_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-weapon_sounds_.front().received).count()));
        weapon_sounds_.pop_front();
    }
    if(weapon_sounds_.empty() && log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES")){static uint64_t logged=0;if(logged!=weapon_sound_order_){logged=weapon_sound_order_;log_("[coop-weapon-sound-queue] received_order="+std::to_string(weapon_sound_order_)+" pending=0");}}
    const auto cue_state=state->mission.find("Session.WeaponCues.State");
    const auto cues=cue_state==state->mission.end()?nullptr:std::get_if<std::vector<int64_t>>(&cue_state->second);
    std::string why;if(!cues || !ssne::reconcile_native_weapon_cues(context,*cues,why)){report(why.empty()?"Missing authoritative weapon cue lifetimes":why);return false;}
    return true;
}
void NativeSession::render_begin(PPCContext&) {
    if(!client_)return;
    NativeStageTimer timing("render_begin",log_);
    const auto status=client_->status();
    if(!queued_ || level_loading_ || !status.connected || !presentation_applied_ || !ssne::replica_simulation() ||
       mem_read32(ssne::game_address(0x802DB0E0))!=4 || ssne::native_cutscene_presenting() || ssne::native_movie_presenting() || ssne::native_replica_results_presenting()){
        presentation_.reset();presentation_revision_=0;client_->clear_predicted_look();return;
    }
    const auto now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    Properties vehicle_visual;
    std::map<EntityId,EntityState> visual;
    {ssne::perf::detail::Scope phase(ssne::perf::detail::RenderVisualSample);visual=presentation_.sample(now,&vehicle_visual);}
    std::vector<uint32_t> first_person;
    PredictedLook pending;ssne::RenderLookSample look_sample;
    const auto* own=ClientPresentation::own(*presentation_applied_,status.player);
    if(own && status.player.slot<max_players){
        const auto actor=replica_.local_address(own->id);
        if(presentation_readable(actor,0x790) && u2f(mem_read32(actor+0xCC))>0){
            const auto weapon=mem_read32(actor+0x5DC);
            if(weapon<12)for(unsigned hand=0;hand<2;++hand){
                const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);
                auto add=[&](uint32_t proxy){
                    if(!presentation_readable(proxy,0x38))return;
                    const auto vt=mem_read32(proxy);
                    if(presentation_readable(vt,0x30) && mem_read32(vt+0x2C)==0x8013E954u)first_person.push_back(proxy);
                };
                if(presentation_readable(instance,0x228)){
                    add(mem_read32(instance));
                    for(unsigned effect=0;effect<3;++effect){const auto offset=native_weapon_flash_offset(weapon,effect);if(offset && presentation_readable(instance+offset,16))add(mem_read32(instance+offset+4));}
                }
                if(weapon==4)if(const auto barrel=native_minigun_barrel(actor,hand);barrel)add(barrel->model);
            }
        }
        for(unsigned i=0;i<std::min(4u,mem_read32(0x802DCE10u));++i){const auto proxy=mem_read32(0x802D90CCu+i*4);if(presentation_readable(proxy,0x38) && ssne::replica_bullet_trail_view(proxy))first_person.push_back(proxy);}
        const auto camera_mode=own->properties.find("camera_mode");
        const auto* mode=camera_mode==own->properties.end()?nullptr:std::get_if<int64_t>(&camera_mode->second);
        const bool active=actor && u2f(mem_read32(actor+0xCC))>0 && !mem_read32(actor+0x370) && !ssne::native_party_spawn_waiting(actor) &&
            presentation_applied_->players[status.player.slot].ready && !presentation_applied_->players[status.player.slot].afk &&
            !ssne::local_pause_menu_open() && !ssne::local_information_open() && mem_read32(actor+0x340)!=3 && mode && *mode==0;
        if(active){
            pending=client_->predicted_look(presentation_applied_->epoch,presentation_applied_->input_ack[status.player.slot]);
            auto found=visual.find(own->id);
            const auto lo=mem_read32(actor+0x194),hi=mem_read32(actor+0x198);
            if(found!=visual.end() && presentation_readable(lo,0x28) && presentation_readable(hi,0x28)){
                float low=u2f(mem_read32(lo+0x24))*.017453292f,high=u2f(mem_read32(hi+0x24))*.017453292f;if(low>high)std::swap(low,high);
                follow_local_camera(found->second,*own,pending,low,high);
                if(presentation_matrix(own->properties,"camera_row",look_sample.camera)){
                    look_sample.angles=std::get<std::array<float,3>>(own->properties.at("view_angles"));look_sample.epoch=presentation_applied_->epoch;
                    look_sample.ack=presentation_applied_->input_ack[status.player.slot];look_sample.slot=status.player.slot;look_sample.generation=status.player.generation;
                    look_sample.pitch_low=low;look_sample.pitch_high=high;look_sample.enabled=true;
                }
            }
        }else client_->clear_predicted_look();
    }
    
    
    std::string error;{ssne::perf::detail::Scope phase(ssne::perf::detail::RenderVisualApply);if(!replica_.begin_presentation_with_vehicles(visual,vehicle_visual,error)){report(error);return;}}
    if(own && env_requested("SSNE_DEV_STATIONARY_LOOK"))if(auto item=visual.find(own->id);item!=visual.end()){
        const auto actor=replica_.local_address(own->id),weapon=mem_read32(actor+0x5DC),camera=mem_read32(actor+0x368);
        const auto instance=weapon<12?mem_read32(actor+0x4BC+weapon*24):0,proxy=presentation_readable(instance,4)?mem_read32(instance):0;
        const auto root=presentation_readable(proxy,8)?mem_read32(proxy+4):0;
        PresentationMatrix c,g,rc,rg;if(presentation_matrix(item->second.properties,"camera_row",c) && presentation_matrix(item->second.properties,"weapon_model0_row",g) && presentation_readable(root,48)){
            for(unsigned i=0;i<12;++i){rc[i]=u2f(mem_read32(camera+0x24+i*4));rg[i]=u2f(mem_read32(root+i*4));}
            const auto expected=matrix_product(matrix_rigid_inverse(c),g),actual=matrix_product(matrix_rigid_inverse(rc),rg);
            std::string message="[weapon-scope-begin] proxy="+std::to_string(proxy)+" expected=";for(float v:expected)message+=std::to_string(v)+",";message+=" actual=";for(float v:actual)message+=std::to_string(v)+",";if(log_)log_(message);
        }
    }
    if(own)if(auto item=visual.find(own->id);item!=visual.end()){
        PresentationMatrix camera; (void)camera;
    }
    if(++presentation_frames_%120==0 && log_)log_("[coop-client-presentation] frames="+std::to_string(presentation_frames_)+" revision="+std::to_string(presentation_applied_->revision)+" buffer_ms="+std::to_string(presentation_.delay_ms())+" visual_objects="+std::to_string(visual.size())+" pending_look="+std::to_string(pending.yaw)+","+std::to_string(pending.pitch));
}
void NativeSession::render_end(PPCContext&){if(client_){ssne::perf::detail::Scope phase(ssne::perf::detail::RenderVisualRestore);NativeStageTimer timing("render_end",log_);if(env_requested("SSNE_DEV_STATIONARY_LOOK") && log_)log_("[weapon-scope-end]");replica_.end_presentation();}}
void NativeSession::frame(PPCContext& context,uint64_t frame) {
    if(ssne::perf::enabled.load(std::memory_order_relaxed)){
        ssne::perf::Context diagnostic;diagnostic.frame=frame;diagnostic.epoch=epoch_;diagnostic.level=level_;diagnostic.scene=mem_read32(ssne::game_address(0x802DB0E0));
        if(client_){const auto state=client_->latest_world();if(state){diagnostic.entities=state->entities.size();diagnostic.revision=state->revision;}const auto status=client_->status();diagnostic.ping_ms=status.ping_ms;diagnostic.bytes_received=status.bytes_received;diagnostic.interpolation_ms=presentation_.delay_ms();}
        for(const auto& binding:native_.player_bindings())if(binding.slot==unsigned(ssne::netplay_local_player()))for(unsigned i=0;i<3;++i)diagnostic.position[i]=u2f(mem_read32(binding.actor+0x20+i*4));
        ssne::perf::set_context(std::move(diagnostic));
    }
    NativeStageTimer timing("frame",log_);
    if(queued_ && !level_loading_ && (server_ || client_) && mem_read32(ssne::game_address(0x802DB0E0))==4 && mem_read32(context.r[13]-0x69d4)==1){
        const auto slot=ssne::netplay_local_player();
        for(const auto& player:native_.player_bindings())if(player.slot==slot){
            std::string why;if(!ssne::update_native_combo_music(context,player.actor,why))report(why);break;
        }
    }
    if(client_ && queued_ && !level_loading_ && mem_read32(ssne::game_address(0x802DB0E0))==4 && ssne::replica_simulation()) {
        const auto now=std::chrono::steady_clock::now();const auto elapsed=replica_effect_time_.time_since_epoch().count()?std::chrono::duration<float>(now-replica_effect_time_).count():0.f;replica_effect_time_=now;
        // Use the existing replica bindings even when no fresh snapshot is
        // available. A separate clock retains full elapsed HUD time and makes
        // repeated calls for one rendered frame harmless.
        const auto hud_state=client_->latest_world();std::vector<uint32_t> hud_actors;
        if(client_->status().connected && hud_state && hud_state->epoch==replica_feedback_epoch_){
            for(const auto& [id,entity]:hud_state->entities)if(entity.kind==EntityKind::Player){const auto actor=replica_.local_address(id);if(actor)hud_actors.push_back(actor);}
        }else replica_hud_clock_={};
        std::string hud_error;if(!advance_native_player_huds(context,hud_actors,replica_hud_clock_,frame,now,hud_error)){report(hud_error);return;}
        if(!replica_.advance_combo_huds(context,std::clamp(elapsed,0.f,60.f),hud_error)){report(hud_error);return;}
        std::string why;bool effects_ok;{ssne::perf::Scope effects("effects");effects_ok=ssne::update_native_replica_effects(context,std::clamp(elapsed,0.f,.1f),why);}if(!effects_ok){report(why);return;}
        if(!ssne::update_native_weapon_loops(context,why)){report(why);return;}
        if(presentation_applied_ && client_->status().connected && hud_state && presentation_applied_->epoch==hud_state->epoch && presentation_applied_->level==hud_state->level)
            if(!update_native_vehicle_audio(context,presentation_applied_->mission,why))report(why);
        if(!update_native_muzzle_flashes(context,std::clamp(elapsed,0.f,.1f),why)){report(why);return;}
        if(!replica_.advance_projectile_visuals(context,frame,why)){report(why);return;}
        if(!drain_weapon_sounds(context))return;
        if(frame%60==0 && log_)log_("[coop-rocket-effect-pool] active="+std::to_string(mem_read32(context.r[13]-0x5FB8))+" trails="+std::to_string(mem_read32(context.r[13]-0x5F38))+" particles="+std::to_string(mem_read32(context.r[13]-0x5F30)));
    }else replica_hud_clock_={};
    if(server_) {
        // A resume request must be resolved from the host's loaded native card
        // before clients receive a real asset name to initialize.
        if(!queued_ && (level_=="continue" || level_=="lobby"))return;
        if(mem_read32(ssne::game_address(0x802DB0E0))==4){std::string why;if(!refresh_native_weapon_poses(context,why)){report(why);return;}}
        if(queued_){std::string why;if(!apply_native_lobby_skin_choices(context,why)){report(why);return;}}
        for(const auto& binding:native_.player_bindings())ssne::apply_ammo_limits(mem_read32(binding.actor+0x5E8));
        auto sample=[&]{NativeStageTimer timing("capture",log_);return native_.capture(frame);}();
        if(ssne::perf::enabled.load(std::memory_order_relaxed)){std::lock_guard lock(ssne::perf::context_mutex);ssne::perf::context.entities=sample.entities.size();}
        // Remote cameras may see an actor culled from the host's own view.
        // Evaluate its animation before publishing; never depend on host draw
        // visibility to decide whether another player's pose advances.
        if(sample.scene==4 && sample.error.empty())for(auto& [id,e]:sample.entities)if(e.kind==EntityKind::Enemy || e.kind==EntityKind::Player || (e.kind==EntityKind::Physics && e.definition==3)){
            const auto object=native_.local_address(id);std::string why;
            if(!refresh_native_actor_pose(context,object,e.kind,why)){report(why);return;}
            if(e.kind==EntityKind::Physics && !capture_native_corpse_visual(context,object,e,why)){report(why);return;}
            for(unsigned part=0;part<(e.kind==EntityKind::Player?4u:1u);++part){
                const auto binding=e.kind==EntityKind::Player?native_body_pose_binding(object,part):e.kind==EntityKind::Physics?native_corpse_pose_binding(object):native_enemy_pose_binding(object);
                if(!binding || !binding->model)continue;
                const auto prefix=e.kind==EntityKind::Player?"body"+std::to_string(part)+"_bone_":std::string("enemy_bone_");
                std::vector<float> matrices;bool valid=true;for(unsigned i=0;i<binding->count*12;++i){auto value=u2f(mem_read32(binding->matrices+i*4));valid&=std::isfinite(value) && std::abs(value)<=10000.f;matrices.push_back(value);}
                if(!valid){report("Invalid prepared actor animation");return;}
                e.properties[prefix+"count"]=int64_t(binding->count);e.properties[prefix+"layout"]=int64_t(binding->layout);e.properties[prefix+"matrices"]=std::move(matrices);
            }
        }
        if(startup_save_){
            // Initialize native resources, then restore before publishing any
            // gameplay world or accepting player input into the new level.
            if(sample.scene!=4 || !sample.error.empty() || std::none_of(sample.entities.begin(),sample.entities.end(),[](const auto& item){return item.second.kind==EntityKind::Player;}))return;
            std::string why;bool restored=false;
            const bool visited=ssne::runtime_checkpoint_visit([&]{
                if(!restore_historical_checkpoint(context,startup_save_->core,startup_save_->extra,why))throw std::runtime_error("Cannot start saved cooperative session: "+why);
                restored=true;
            },why);
            if(!visited){report("Waiting to restore startup save: "+why);return;}
            if(!restored)throw std::runtime_error("Startup save restore did not run");
            startup_save_.reset();write_save_test_evidence(false,frame);
            if(log_)log_("[coop-startup-loaded] slot="+std::to_string(startup_slot_)+" epoch="+std::to_string(epoch_)+" level="+level_+" frame="+std::to_string(frame));
            return; // Discard the sample captured before RAM restoration.
        }
        if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES")){
            static std::map<EntityId,uint32_t> health_seen;
            for(const auto& [id,entity]:sample.entities)if(entity.kind==EntityKind::Enemy && entity.properties.contains("health")){const auto value=std::get<float>(entity.properties.at("health"));const auto bits=f2u(value);if(!health_seen.contains(id) || health_seen[id]!=bits){health_seen[id]=bits;log_("[coop-host-health-observed] id="+std::to_string(id)+" definition="+std::to_string(entity.definition)+" health="+std::to_string(value)+" frame="+std::to_string(frame)+" object="+std::to_string(native_.local_address(id)));}}
        }

        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && frame%120==0)for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player){const auto& p=std::get<std::array<float,3>>(e.properties.at("position"));if(log_)log_("[coop-test-player-position] slot="+std::to_string(std::get<int64_t>(e.properties.at("player_slot")))+" x="+std::to_string(p[0])+" y="+std::to_string(p[1])+" z="+std::to_string(p[2])+" weapon="+std::to_string(mem_read32(native_.local_address(id)+0x5DC)));}
        if(frame%120==0 && log_ && env_requested("SSNE_TEST_HIDDEN") && sample.scene==4){
            for(const auto& [id,entity]:sample.entities)if(entity.kind==EntityKind::Player){
                const auto actor=native_.local_address(id);PPCContext count=context,health=context;count.r[3]=health.r[3]=actor;
                ssne_call(count,0x8009C410);ssne_call(health,0x8009DBCC);
                log_("[coop-native-participant-query] frame="+std::to_string(frame)+" slot="+std::to_string(mem_read32(actor+0x17C))+" occupied="+std::to_string(ssne::native_party_actor_occupied(actor))+" spheres="+std::to_string(count.r[3])+" queried_health="+std::to_string(health.ps0[1])+" raw_health="+std::to_string(u2f(mem_read32(actor+0xCC)))+" flags="+std::to_string(mem_read32(actor+0x1C)));
            }
        }
        if(cutscene_epoch_!=sample.epoch){
            if(active_cutscene_vote_)server_->end_cutscene(active_cutscene_vote_);
            active_cutscene_object_=0;active_cutscene_vote_=0;active_movie_sequence_=0;cutscene_skip_applied_=false;cutscene_trace_states_.clear();cutscene_epoch_=sample.epoch;
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_CUTSCENE")) {
            for(const auto& item:sample.unmapped)if(item.vtable==0x80203078u) {
                const auto object=item.address,reference=mem_read32(object+0x50);
                const auto active=mem_read32(object+0x60),pending=mem_read32(object+0x5C),ending=mem_read32(object+0x130);
                const auto signature=active|(pending<<1)|(ending<<2);
                if(!cutscene_trace_states_.contains(object) || cutscene_trace_states_[object]!=signature || (active && frame%60==0)) {
                    cutscene_trace_states_[object]=signature;
                    const auto config=mem_read32(object+0x54);
                    const auto kind=config>=0x80000000u && config<=ssne::guest_ram_end()-0x24?mem_read32(config+0xC):999u;
                    // Original800799E8 writes the affine matrix at CTransform+4.
                    std::string camera;for(unsigned i=0;i<12;++i)camera+=(camera.empty()?"":",")+std::to_string(u2f(mem_read32(object+0x140+i*4)));
                    if(log_)log_("[coop-native-cutscene] frame="+std::to_string(frame)+" level_ref="+std::to_string(reference)+" active="+std::to_string(active)+" pending="+std::to_string(pending)+" ending="+std::to_string(ending)+" kind="+std::to_string(kind)+" camera="+camera);
                }
            }
        }
        if(!sample.error.empty()){if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_CAMPAIGN_COMMAND"))campaign_probe(context,sample,native_,level_,log_);report(sample.error);return;}
        if(sample.scene==4)for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player){
            std::string why;if(!refresh_native_bomb_ammo_hud(context,native_.local_address(id),why)){report(why);return;}
        }
        capture_fixture(sample,context);
        uint32_t active_cutscene=0;
        const bool trace_camera_selection=frame%120==0 && env_requested("SSNE_COOP_PROFILE") && log_;
        for(const auto& item:sample.unmapped)if(item.vtable==0x80203078u) {
            const bool selected=ssne::native_cutscene_is_presenting(item.address);
            if(trace_camera_selection && mem_read32(item.address+0x60))log_("[coop-camera-selection] frame="+std::to_string(frame)+" level_ref="+std::to_string(mem_read32(item.address+0x50))+" native_mask="+std::to_string(mem_read32(0x802ABAD0u))+" selected="+std::to_string(selected));
            if(!selected)continue;
            if(active_cutscene){report("Multiple active native cutscene entities; synchronization unavailable");return;}
            active_cutscene=item.address;
        }
        // Credits have no world camera entity or player actors. Count every
        // connected human, including late joins still loading presentation.
        if(sample.scene==5)active_cutscene=0x80292560u;
        const auto movie_before_vote=ssne::native_movie_state();
        const auto movie_sequence=movie_before_vote.active?movie_before_vote.sequence:0;
        if(movie_sequence)active_cutscene=0; // Video playback owns the presentation until it ends.
        if(active_cutscene_object_!=active_cutscene || active_movie_sequence_!=movie_sequence) {
            if(active_cutscene_vote_)server_->end_cutscene(active_cutscene_vote_);
            active_cutscene_object_=active_cutscene;active_movie_sequence_=movie_sequence;active_cutscene_vote_=0;cutscene_skip_applied_=false;
            if(active_cutscene || movie_sequence){active_cutscene_vote_=server_->begin_cutscene(sample.scene==5 || movie_sequence);if(log_)log_("[coop-cutscene-vote-start] id="+std::to_string(active_cutscene_vote_)+" movie="+std::to_string(movie_sequence)+" level_ref="+std::to_string(sample.scene==5 || movie_sequence?0:mem_read32(active_cutscene+0x50)));}
            else if(log_)log_("[coop-cutscene-vote-end] native sequence ended");
        }
        const auto decision=active_cutscene_vote_ && !cutscene_skip_applied_?server_->skip_decision():std::optional<SkipDecision>{};
        if(active_cutscene_vote_ && !cutscene_skip_applied_ && decision && decision->vote.cutscene==active_cutscene_vote_) {
            const auto vote=decision->vote;
            if(log_)log_("[coop-cutscene-vote-commit] id="+std::to_string(vote.cutscene)+" received="+std::to_string(vote.received)+" required="+std::to_string(vote.required));
            if(log_)log_("[coop-cutscene-vote-decision] id="+std::to_string(vote.cutscene)+" connected_mask="+std::to_string(decision->connected_mask)+" eligible_mask="+std::to_string(decision->eligible_mask)+" afk_mask="+std::to_string(decision->afk_mask));
            std::string why;
            if(active_movie_sequence_){if(ssne::request_native_movie_skip(context,active_movie_sequence_,why))cutscene_skip_applied_=true;else report(why);}
            else if(sample.scene==5){request_native_credits_skip();cutscene_skip_applied_=true;}
            else if(ssne::request_native_cutscene_skip(context,active_cutscene_object_,why))cutscene_skip_applied_=true;
            else report(why);
        }
        if(active_cutscene_vote_ && frame%30==0 && log_) {
            const auto vote=server_->skip_vote();log_("[coop-cutscene-vote] id="+std::to_string(vote.cutscene)+" received="+std::to_string(vote.received)+" required="+std::to_string(vote.required)+" committed="+std::to_string(vote.skip_requested));
        }
        if(campaign_probe(context,sample,native_,level_,log_))return;
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_LEVEL_DURING_CUTSCENE")) {
            static uint64_t first_active_frame=0;static bool requested=false;
            if(active_cutscene_object_ && mem_read32(active_cutscene_object_+0x50)==1953 && !first_active_frame)first_active_frame=frame;
            if(!requested && first_active_frame && active_cutscene_object_ && mem_read32(active_cutscene_object_+0x50)==1953 && mem_read32(active_cutscene_object_+0x60) && frame>=first_active_frame+90) {
                const auto next=std::string(getenv("SSNE_COOP_TEST_LEVEL_DURING_CUTSCENE"));std::string why;
                if(queue_native_coop_level(context,{next,setup_},why)) {
                    requested=true;if(log_)log_("[coop-test-level-during-cutscene] from_vote="+std::to_string(active_cutscene_vote_)+" reference=1953 target="+next);return;
                }
                report(why);
            }
        }
        const auto block_spawn_at=env_present("SSNE_COOP_TEST_BLOCK_SPAWN_AT")?uint64_t(std::clamp(atoi(getenv("SSNE_COOP_TEST_BLOCK_SPAWN_AT")),240,1500)):1000u;
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_BLOCK_SPAWN") && sample.scene==4 && frame>=block_spawn_at) {
            if(!collider_stress_phase){collider_stress_original=mem_read32(0x8020E6C4);mem_write32(0x8020E6C4,f2u(2.75f));collider_stress_phase=1;if(log_)log_("[coop-test-collider-block] native_player_radius=2.75");}
            if(collider_stress_phase==1 && frame>=1800 && !env_requested("SSNE_COOP_TEST_WORLD_LINK_DURING_WAIT")){mem_write32(0x8020E6C4,collider_stress_original);collider_stress_phase=2;if(log_)log_("[coop-test-collider-release] native_player_radius="+std::to_string(u2f(collider_stress_original)));}
        }
        if(env_requested("SSNE_TEST_HIDDEN") && (env_present("SSNE_COOP_TEST_KILL_PLAYER") || env_present("SSNE_COOP_TEST_KILL_PLAYERS")) && sample.scene==4 && frame>=1200) {
            static std::set<unsigned> killed;std::set<unsigned> targets;
            if(env_present("SSNE_COOP_TEST_KILL_PLAYER"))targets.insert(unsigned(atoi(getenv("SSNE_COOP_TEST_KILL_PLAYER"))));
            if(env_present("SSNE_COOP_TEST_KILL_PLAYERS")){std::istringstream input(getenv("SSNE_COOP_TEST_KILL_PLAYERS"));std::string item;while(std::getline(input,item,','))targets.insert(unsigned(atoi(item.c_str())));}
            for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player && targets.contains(unsigned(std::get<int64_t>(e.properties.at("player_slot")))) && !killed.contains(unsigned(std::get<int64_t>(e.properties.at("player_slot"))))) {
                const unsigned slot=unsigned(std::get<int64_t>(e.properties.at("player_slot")));
                const auto actor=native_.local_address(id);const auto* kill=ssne_find_func(0x8009B370);
                if(!kill || mem_read32(kill->address)!=0x9421FFD0u || !ssne::native_player_collision_radius(actor)){report("Native debug kill function unavailable");break;}
                std::vector<uint8_t> stack(0x2000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x2000+i);
                PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);call.r[3]=actor;call.r[4]=call.r[5]=0;
                try{ssne_call(call,kill->address);}catch(...){for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);throw;}
                for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);killed.insert(slot);
                if(log_)for(const auto& [enemy_id,enemy]:sample.entities)if(enemy.kind==EntityKind::Enemy) {
                    const auto object=native_.local_address(enemy_id),table=mem_read32(object+0x48);PPCContext bounds=context;bounds.msr&=~0x8000u;bounds.r[1]-=0x200;mem_write32(bounds.r[1],context.r[1]);bounds.r[3]=object+uint32_t(int32_t(int16_t(mem_read16(table+0x190))));
                    const auto getter=mem_read32(table+0x194);if(ssne_find_func(getter)){ssne_call(bounds,getter);const auto& p=std::get<std::array<float,3>>(enemy.properties.at("position"));log_("[coop-test-blocker-bounds] actor="+std::to_string(object)+" flags="+std::to_string(mem_read32(object+0x1C))+" health="+std::to_string(std::get<float>(enemy.properties.at("health")))+" radius="+std::to_string(bounds.ps0[1])+" x="+std::to_string(p[0])+" z="+std::to_string(p[2]));}
                }
                for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x2000+i,stack[i]);
                if(log_)log_("[coop-test-player-kill] slot="+std::to_string(slot)+" actor="+std::to_string(actor)+" native_dead="+std::to_string(mem_read32(actor+0x328)));return;
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_ENEMY_TYPE") && sample.scene==4 && frame>=900) {
            const auto spawn_slot=env_present("SSNE_COOP_TEST_ENEMY_SLOT")?atoi(getenv("SSNE_COOP_TEST_ENEMY_SLOT")):-1;
            const auto people=server_->participants();
            const bool source_ready=spawn_slot<0 || (unsigned(spawn_slot)<capacity_ && people[spawn_slot].connected && people[spawn_slot].ready);
            static bool requested=false,repeated=false;
            static std::chrono::steady_clock::time_point next_wave{};
            const auto repeat_frame=env_present("SSNE_COOP_TEST_ENEMY_AGAIN_FRAME")?std::strtoul(getenv("SSNE_COOP_TEST_ENEMY_AGAIN_FRAME"),nullptr,10):0;
            const auto repeat_ms=env_present("SSNE_COOP_TEST_ENEMY_REPEAT_MS")?std::clamp<unsigned long>(std::strtoul(getenv("SSNE_COOP_TEST_ENEMY_REPEAT_MS"),nullptr,10),10000,300000):0;
            const auto now=std::chrono::steady_clock::now();
            if(source_ready && (!requested || (repeat_frame>=900 && frame>=repeat_frame && !repeated) || (repeat_ms && now>=next_wave))){const bool repeat=requested;const auto type=unsigned(atoi(getenv("SSNE_COOP_TEST_ENEMY_TYPE")));const auto count=env_present("SSNE_COOP_TEST_ENEMY_COUNT")?atoi(getenv("SSNE_COOP_TEST_ENEMY_COUNT")):3;
                const auto distance=env_present("SSNE_COOP_TEST_ENEMY_DISTANCE")?float(atof(getenv("SSNE_COOP_TEST_ENEMY_DISTANCE"))):15.f;if(spawn_slot>=0)ssne::mod_test_enemy_at_player(type,unsigned(spawn_slot),count,distance);else ssne::mod_spawn_type(true,type,count,distance);requested=true;repeated|=repeat;next_wave=now+std::chrono::milliseconds(repeat_ms);if(log_)log_("[coop-test-enemy-wave] type="+std::to_string(type)+" requested="+std::to_string(count)+" source_slot="+std::to_string(spawn_slot)+" repeat="+std::to_string(repeat)+" frame="+std::to_string(frame));}
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_WEAPON") && sample.scene==4 && frame>=uint64_t(env_present("SSNE_COOP_TEST_WEAPON_AT")?std::clamp(atoi(getenv("SSNE_COOP_TEST_WEAPON_AT")),900,36000):900)){
            static bool selected=false;
            const auto slot=env_present("SSNE_COOP_TEST_WEAPON_SLOT")?unsigned(atoi(getenv("SSNE_COOP_TEST_WEAPON_SLOT"))):0u;
            const auto people=server_->participants();
            if(!selected && slot<capacity_ && people[slot].connected && people[slot].ready){const auto weapon=unsigned(atoi(getenv("SSNE_COOP_TEST_WEAPON")));const auto actor=mem_read32(ssne::native_party_registry()+slot*4);std::string why;
                const bool available=weapon<12 && (mem_read32(actor+0x4CC+weapon*24)>0 || ssne::mod_current_options().weapons);
                if(apply_native_weapon_selection(context,actor,weapon,available,why)){selected=true;if(log_)log_("[coop-test-weapon-selection] slot="+std::to_string(slot)+" weapon="+std::to_string(weapon));}else report(why);
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_PICKUP") && sample.scene==4 && frame>=700){static bool spawned=false;static uint8_t seeded=0;const auto type=unsigned(atoi(getenv("SSNE_COOP_TEST_PICKUP")));if(env_requested("SSNE_COOP_TEST_PICKUP_AT_PLAYERS")){const auto people=server_->participants();for(unsigned slot=0;slot<capacity_;++slot)if(people[slot].connected && people[slot].ready && !(seeded&(1u<<slot)) && (!env_present("SSNE_COOP_TEST_PICKUP_MASK") || (std::strtoul(getenv("SSNE_COOP_TEST_PICKUP_MASK"),nullptr,0)&(1u<<slot)))){ssne::mod_test_pickup_at_player(type,slot);seeded|=uint8_t(1u<<slot);if(log_)log_("[coop-test-pickup-player] type="+std::to_string(type)+" slot="+std::to_string(slot));}}else if(!spawned){ssne::mod_spawn_type(false,type,1,2.f);spawned=true;if(log_)log_("[coop-test-pickup-spawn] type="+std::to_string(type));}}
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_REMOVE_ENEMIES") && sample.scene==4 && frame>=uint64_t(atoi(getenv("SSNE_COOP_TEST_REMOVE_ENEMIES")))) {
            static bool cleared=false;
            if(!cleared){unsigned removed=0;for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Enemy){std::string why;if(!remove_native_entity(context,native_.local_address(id),why)){report(why);return;}++removed;}cleared=true;if(log_)log_("[coop-test-enemy-clear] removed="+std::to_string(removed));return;}
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_TOGGLE_SCRIPTS") && sample.scene==4 && frame>=900) {
            static unsigned phase=0;const auto requested=frame>=1200?2u:1u;
            if(phase<requested) {
                bool trigger=false,button=false;
                for(const auto& [id,e]:sample.entities)if((e.kind==EntityKind::Mission && !trigger) || (e.kind==EntityKind::Switch && !button)) {
                    std::string why;if(!set_native_script_enabled(context,native_.local_address(id),requested==2,why)){report(why);break;}
                    if(e.kind==EntityKind::Mission)trigger=true;else button=true;
                    if(log_)log_("[coop-test-script] index="+std::to_string(std::get<int64_t>(e.properties.at("script_index")))+" enabled="+std::to_string(requested==2));
                }
                if(trigger && button)phase=requested;
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_SPAWN_PROBE") && sample.scene==4 && frame>=900) {
            static bool probed=false;
            if(!probed)for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player) {
                auto from=std::get<std::array<float,3>>(e.properties.at("position")),to=from;from[1]+=1.f;to[1]-=20.f;
                std::optional<ssne::NativeSegmentHit> hit;std::string why;
                const bool valid=ssne::native_world_segment(context,from,to,hit,why);
                if(log_)log_("[coop-spawn-probe] slot="+std::to_string(std::get<int64_t>(e.properties.at("player_slot")))+" valid="+std::to_string(valid)+" hit="+std::to_string(bool(hit))+" floor="+std::to_string(hit?hit->position[1]:0)+" normal="+std::to_string(hit?hit->normal[1]:0)+" error="+why);
                if(std::get<int64_t>(e.properties.at("player_slot"))==0) {
                    std::optional<ssne::NativeSegmentHit> wall;
                    auto across=from;across[0]+=1000.f;
                    const PPCContext before=context;
                    std::vector<uint8_t> stack(0x2000);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x2000+i);
                    const bool wall_valid=ssne::native_world_segment(context,from,across,wall,why);
                    bool preserved=memcmp(&context,&before,sizeof(context))==0;
                    for(unsigned i=0;i<stack.size();++i)preserved&=stack[i]==mem_read8(context.r[1]-0x2000+i);
                    auto bad=context;bad.r[13]=0;
                    const bool rejected=!ssne::native_world_segment(bad,from,across,hit,why);
                    if(log_)log_("[coop-spawn-probe-wall] valid="+std::to_string(wall_valid)+" hit="+std::to_string(bool(wall))+" preserved_context_stack="+std::to_string(preserved)+" rejected_bad_context="+std::to_string(rejected));
                }

                if(auto radius=ssne::native_player_collision_radius(native_.local_address(id))) {
                    auto origin=std::get<std::array<float,3>>(e.properties.at("position")),dest=origin;dest[1]-=.01f;
                    ssne::NativeSphereResult sphere;const bool ok=ssne::native_world_sphere(context,origin,dest,*radius,sphere,why);
                    if(log_)log_("[coop-spawn-sphere-floor] slot="+std::to_string(std::get<int64_t>(e.properties.at("player_slot")))+" valid="+std::to_string(ok)+" radius="+std::to_string(*radius)+" contacts="+std::to_string(sphere.contacts.size())+" corrected_y="+std::to_string(sphere.position[1])+" requested_y="+std::to_string(dest[1])+" error="+why);
                    if(std::get<int64_t>(e.properties.at("player_slot"))==0) {
                        auto high=origin;high[1]+=2.f;auto beyond=high;beyond[0]+=1000.f;
                        ssne::NativeSphereResult wall_sphere;const bool wall_ok=ssne::native_world_sphere(context,high,beyond,*radius,wall_sphere,why);
                        if(log_)log_("[coop-spawn-sphere-wall] valid="+std::to_string(wall_ok)+" contacts="+std::to_string(wall_sphere.contacts.size())+" corrected_x="+std::to_string(wall_sphere.position[0])+" requested_x="+std::to_string(beyond[0])+" error="+why);
                    }
                } else report("Native player collision sphere unavailable");
                probed=true;
            }
        }
        // Hidden integration harness uses the actual host-side activation method.
        // A replica never dispatches this native mission interaction.
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_OPEN_BRUSH") && sample.scene==4 && frame>=900) {
            static bool activated=false;
            const auto resource=int64_t(atoi(getenv("SSNE_COOP_TEST_OPEN_BRUSH")));
            if(!activated)for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Door && std::get<int64_t>(e.properties.at("brush_resource"))==resource) {
                std::string why;if(!activate_native_brush(context,native_.local_address(id),why)){report(why);break;}
                activated=true;if(log_)log_("[coop-test-brush] activated resource="+std::to_string(resource));break;
            }
        }
        if(frame%(env_present("SSNE_COOP_TEST_OPEN_BRUSH")?10:60)==0 && log_)for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Door && std::get<int64_t>(e.properties.at("brush_active")))log_("[coop-host-brush] resource="+std::to_string(std::get<int64_t>(e.properties.at("brush_resource")))+" segment="+std::to_string(std::get<int64_t>(e.properties.at("brush_segment")))+" progress="+std::to_string(std::get<float>(e.properties.at("brush_progress"))));
        if(frame%60==0 && log_) {
            unsigned count=0,disabled_triggers=0,disabled_switches=0;
            for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Mission || e.kind==EntityKind::Switch) {
                ++count;if(e.kind==EntityKind::Mission)disabled_triggers+=!std::get<int64_t>(e.properties.at("trigger_enabled"));else disabled_switches+=!std::get<int64_t>(e.properties.at("switch_enabled"));
            }
            log_("[coop-host-scripts] count="+std::to_string(count)+" disabled_triggers="+std::to_string(disabled_triggers)+" disabled_switches="+std::to_string(disabled_switches));
        }
        if(frame%60==0 && log_ && env_present("SSNE_COOP_TEST_ENEMY_TYPE")) {
            const auto mods=ssne::mod_state();log_("[coop-test-spawn-status] "+mods.status);
            for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Enemy && e.definition==unsigned(atoi(getenv("SSNE_COOP_TEST_ENEMY_TYPE")))) {
                const auto& pos=std::get<std::array<float,3>>(e.properties.at("position"));
                log_("[coop-host-enemy] id="+std::to_string(id)+" health="+std::to_string(std::get<float>(e.properties.at("health")))+" position="+std::to_string(pos[0])+","+std::to_string(pos[1])+","+std::to_string(pos[2]));
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_LEVEL_DURING_WAIT") && sample.scene==4 && frame>=1600) {
            static bool requested=false;
            if(!requested) {
                std::vector<unsigned> slots;for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player && ssne::native_party_spawn_waiting(native_.local_address(id)))slots.push_back(unsigned(std::get<int64_t>(e.properties.at("player_slot"))));
                if(!slots.empty()) {
                    if(collider_stress_phase==1){mem_write32(0x8020E6C4,collider_stress_original);collider_stress_phase=2;}
                    std::string list;for(auto slot:slots)list+=(list.empty()?"":",")+std::to_string(slot);
                    const std::string next=getenv("SSNE_COOP_TEST_LEVEL_DURING_WAIT");std::string why;
                    if(log_)log_("[coop-test-level-during-wait] slots="+list+" target="+next+" old_epoch="+std::to_string(sample.epoch));
                    if(queue_native_coop_level(context,{next,setup_},why)){requested=true;report("host native level change queued: "+next);return;}report(why);
                }
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_WORLD_LINK_DURING_WAIT") && sample.scene==4 && frame>=1600) {
            static bool requested=false;
            if(!requested) {
                std::string slots;for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player && ssne::native_party_spawn_waiting(native_.local_address(id)))slots+=(slots.empty()?"":",")+std::to_string(std::get<int64_t>(e.properties.at("player_slot")));
                if(!slots.empty())for(const auto& item:sample.unmapped)if(item.vtable==0x8020A750u) {
                    std::string target,why;
                    if(log_)log_("[coop-test-world-link-during-wait] slots="+slots+" old_epoch="+std::to_string(sample.epoch));
                    if(collider_stress_phase==1)ssne::native_party_restore_test_collider_at_exit(collider_stress_original);
                    if(activate_native_world_link(context,item.address,target,why)){requested=true;report("host native WorldLink activated: "+target);return;}report(why);
                }
            }
        }
        // Exercise the real debug pickup factory and native collection logic.
        // Different subsets per slot reveal loadouts copied from another player.
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_CARRY_PICKUPS") && sample.scene==4 && frame>=900 && !ssne::native_cutscene_presenting()) {
            static constexpr unsigned types[]={35,36,37,38,39,40,41,42,43,44,45,55,36,37,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,4,9};
            static unsigned wave=0;static uint64_t next_frame=0;
            const auto people=server_->participants();bool ready=true;
            for(unsigned slot=0;slot<capacity_;++slot)ready&=people[slot].connected && people[slot].ready;
            if(ready && wave<std::size(types) && frame>=next_frame) {
                if(!wave){
                    // Install the existing native selection trace before pickups;
                    // selecting the current weapon returns without changing it.
                    const auto actor=mem_read32(ssne::native_party_registry());std::string why;
                    apply_native_weapon_selection(context,actor,mem_read32(actor+0x5DC),true,why);
                }
                for(unsigned slot=0;slot<capacity_;++slot)if((types[wave]+slot)%4!=0) {
                    ssne::mod_test_pickup_at_player(types[wave],slot);
                    if(log_)log_("[coop-carry-pickup] frame="+std::to_string(frame)+" wave="+std::to_string(wave)+" type="+std::to_string(types[wave])+" slot="+std::to_string(slot));
                }
                ++wave;next_frame=frame+24;
                if(wave==std::size(types)){carry_pickups_complete_frame=frame+120;if(log_)log_("[coop-carry-pickups-complete] wait_until="+std::to_string(carry_pickups_complete_frame));}
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_WORLD_LINK") && sample.scene==4 && frame>=1200 && (!env_requested("SSNE_COOP_TEST_CARRY_PICKUPS") || (carry_pickups_complete_frame && frame>=carry_pickups_complete_frame))) {
            static bool requested=false;
            if(!requested)for(const auto& item:sample.unmapped)if(item.vtable==0x8020A750u) {
                std::string target,why;
                if(activate_native_world_link(context,item.address,target,why,env_requested("SSNE_COOP_TEST_WORLD_END"))){requested=true;report("host native WorldLink activated: "+target);return;}
                report(why);
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_CUTSCENE") && sample.scene==4 && frame>=1200) {
            static bool requested=false;
            const auto wanted=std::strtoul(getenv("SSNE_COOP_TEST_CUTSCENE"),nullptr,10);
            if(!requested)for(const auto& item:sample.unmapped)if(item.vtable==0x80203078u && mem_read32(item.address+0x50)==wanted) {
                std::string why;
                if(activate_native_cutscene(context,item.address,why)){requested=true;report("host native cutscene activated: "+std::to_string(wanted));break;}
                report(why);
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_MUSIC_CHANGER") && sample.scene==4 && frame>=1200){
            static bool requested=false;
            if(!requested)for(const auto& object:sample.unmapped)if(object.vtable==0x80206FF0u){
                std::string why;if(activate_native_music_changer(context,object.address,why)){requested=true;if(log_)log_("[coop-test-music-changer] reference="+std::to_string(mem_read32(object.address+0x50)));break;}report(why);
            }
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_NEXT_LEVEL") && sample.scene==4 && (env_requested("SSNE_COOP_TEST_ADAPTIVE_LEVEL")?ssne::native_bullet_impact_history().size()>=12:frame>=uint64_t(env_present("SSNE_COOP_TEST_NEXT_LEVEL_AT")?std::clamp(std::atoi(getenv("SSNE_COOP_TEST_NEXT_LEVEL_AT")),1200,36000):1200))) {
            static bool requested=false;
            if(!requested){if(env_requested("SSNE_COOP_TEST_ADAPTIVE_LEVEL")){const auto people=server_->participants();if(std::count_if(people.begin(),people.end(),[](const auto& person){return person.connected;})!=capacity_)return;if(log_)log_("[coop-test-adaptive-level] actual_impacts="+std::to_string(ssne::native_bullet_impact_history().size())+" connected="+std::to_string(capacity_));}std::string why;auto next=std::string(getenv("SSNE_COOP_TEST_NEXT_LEVEL"));
                if(queue_native_coop_level(context,{next,setup_},why)){requested=true;report("host native level change queued: "+next);return;}report(why);}
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_SPEECH") && sample.scene==4 && frame>=1200){
            static bool requested=false;const auto reference=uint32_t(std::strtoul(getenv("SSNE_COOP_TEST_SPEECH"),nullptr,10));
            if(!requested)for(const auto& object:sample.unmapped)if(object.vtable==0x802087A8u && mem_read32(object.address+0x50)==reference){
                std::string why;if(activate_native_sound_holder(context,object.address,why)){requested=true;if(log_)log_("[coop-test-sound-holder] reference="+std::to_string(reference));break;}report(why);
            }
        }
        if(epoch_!=sample.epoch){
            epoch_=sample.epoch;characters_={};host_ready_=false;join_placements_={};
            const auto prefix=native_rules_prefix(sample.mode);
            if(prefix){const auto key=std::string(prefix)+".Level";auto rules=read_native_configuration(native_global_configuration(),key);
            if(auto next=rules.values.find(key);next!=rules.values.end() && std::holds_alternative<std::string>(next->second)){level_=std::get<std::string>(next->second);setup_[key]=level_;}}
        }
        if(sample.scene==4)if(auto active=native_active_level();active && *active!=level_) {
            level_=*active;if(auto prefix=native_rules_prefix(sample.mode))setup_[std::string(prefix)+".Level"]=level_;
            if(log_)log_("[coop-host-level] active="+level_+" epoch="+std::to_string(epoch_));
        }
        if(sample.scene==4 && frame%60==0 && log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES"))if(auto actual=native_active_level()){
            const auto count=std::count_if(sample.entities.begin(),sample.entities.end(),[](const auto& item){return item.second.kind==EntityKind::Player;});
            log_("[coop-native-level-observed] level="+*actual+" epoch="+std::to_string(epoch_)+" players="+std::to_string(count));
        }
        WorldState state;state.epoch=epoch_;state.tick=frame;state.level=level_;state.mission=setup_;state.mission["Session.GameMode"]=int64_t(sample.mode);state.entities=sample.entities;
        state.mission["Session.Motion.Time"]=std::chrono::duration<float>(std::chrono::steady_clock::now()-motion_started_).count();
        const auto mods=ssne::mod_current_options();state.mission["Session.Mods.Flags"]=int64_t(unsigned(mods.god)|(unsigned(mods.ammo)<<1)|(unsigned(mods.weapons)<<2)|(unsigned(mods.no_enemies)<<3)|(unsigned(mods.rapid_fire)<<4));state.mission["Session.Mods.FireMultiplier"]=mods.fire_multiplier;
        state.mission["Session.EnemyMultiplier"]=int64_t(ssne::enemy_multiplier());
        const auto ammo_rules=ssne::ammo_rules();
        state.mission["Session.AmmoCapacity"]=int64_t(ammo_rules.capacity);
        state.mission["Session.AmmoPickupMultiplier"]=int64_t(ammo_rules.pickup_multiplier);
        publish_native_lobby_characters(state.mission);
        if(sample.scene==4 && sample.mode>=2 && sample.mode<=4)capture_versus(context,sample,state.mission);
        const bool loading=sample.scene!=4 || std::none_of(sample.entities.begin(),sample.entities.end(),[](const auto& item){return item.second.kind==EntityKind::Player;});
        if(!loading){std::vector<int64_t> models;std::string why;if(!capture_native_model_states(models,why) || !capture_native_vehicles(state.mission,why)){report(why);return;}state.mission["Session.Geometry.Models"]=std::move(models);}
        if(loading){state.entities.clear();characters_={};host_ready_=false;}
        const bool presentation=sample.scene==5 || (sample.scene==3 && queued_);
        if(presentation)state.mission["Session.Presentation.Scene"]=int64_t(sample.scene);
        if(sample.scene==5)publish_native_credits(state.mission);
        state.mission["Session.NativePhase"]=std::string(presentation?(sample.scene==5?"credits":"menu"):loading?"loading":"gameplay");
        ssne::NativeEventProducerSnapshot producer;
        NativeProjectileVisualSnapshot projectile_producer;
        ssne::NativeProducerCursors accepted{};
        bool source_publish=true;
        if(!loading) {
            producer=ssne::native_event_producer_snapshot();
            projectile_producer=native_.projectile_visuals();
            if(!projectile_producer.error.empty()){report(projectile_producer.error);return;}
            if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_EVENT_INTERVAL_MS")){
                static uint64_t seen_epoch=0;static std::chrono::steady_clock::time_point next{};
                const auto now=std::chrono::steady_clock::now();
                if(seen_epoch!=producer.epoch){seen_epoch=producer.epoch;next={};}
                source_publish=now>=next;
                if(source_publish)next=now+std::chrono::milliseconds(std::clamp(atoi(getenv("SSNE_COOP_TEST_EVENT_INTERVAL_MS")),1000,120000));
            }
            if(source_publish){
                accepted=producer.cursors;
            }
            std::vector<int64_t> hit_ids,hit_shots,hit_sources,hit_targets,hit_generations,hit_codes,hit_pellets;std::vector<float> hit_values;
            const auto& hit_history=producer.hits;for(const auto& hit:hit_history){if(!source_publish)break;hit_ids.push_back(int64_t(hit.sequence));hit_shots.push_back(int64_t(hit.shot));hit_sources.push_back(int64_t(hit.source));hit_targets.push_back(int64_t(hit.target));hit_generations.push_back(int64_t(hit.generation));hit_codes.push_back(hit.slot*16+hit.weapon);hit_pellets.push_back(hit.pellet);hit_values.insert(hit_values.end(),{hit.before,hit.after,hit.amount});}
            state.mission["Session.ConfirmedHits.Ids"]=std::move(hit_ids);state.mission["Session.ConfirmedHits.Shots"]=std::move(hit_shots);state.mission["Session.ConfirmedHits.Sources"]=std::move(hit_sources);state.mission["Session.ConfirmedHits.Targets"]=std::move(hit_targets);state.mission["Session.ConfirmedHits.Generations"]=std::move(hit_generations);state.mission["Session.ConfirmedHits.Codes"]=std::move(hit_codes);state.mission["Session.ConfirmedHits.Values"]=std::move(hit_values);state.mission["Session.ConfirmedHits.Pellets"]=std::move(hit_pellets);
            if(replica_hit_epoch_!=epoch_){replica_hit_epoch_=epoch_;replica_hit_sequence_=0;std::lock_guard lock(combat_mutex_);combat_feedback_={};}for(const auto& hit:hit_history)if(hit.sequence>replica_hit_sequence_){accept_confirmed_hit(hit,server_->status().player);replica_hit_sequence_=hit.sequence;}
            std::vector<int64_t> ids,styles;std::vector<float> values;
            for(const auto& effect:producer.rockets){if(!source_publish)break;ids.push_back(int64_t(effect.sequence));styles.push_back(effect.style);values.insert(values.end(),effect.values.begin(),effect.values.end());}
            state.mission["Session.RocketEffects.Ids"]=std::move(ids);state.mission["Session.RocketEffects.Styles"]=std::move(styles);state.mission["Session.RocketEffects.Values"]=std::move(values);
            std::vector<int64_t> shot_ids,weapons,cues,shot_orders,shot_flags;std::vector<float> positions;
            for(const auto& shot:producer.shots){if(!source_publish)break;shot_ids.push_back(int64_t(shot.sequence));shot_ids.push_back(shot.slot);weapons.push_back(shot.weapon);cues.push_back(shot.cue_sound);shot_flags.push_back(shot.cue_flags);shot_orders.push_back(int64_t(shot.order));positions.insert(positions.end(),shot.position.begin(),shot.position.end());}
            state.mission["Session.WeaponShots.Ids"]=std::move(shot_ids);state.mission["Session.WeaponShots.Positions"]=std::move(positions);state.mission["Session.WeaponShots.Weapons"]=std::move(weapons);state.mission["Session.WeaponShots.Cues"]=std::move(cues);state.mission["Session.WeaponShots.Flags"]=std::move(shot_flags);state.mission["Session.WeaponShots.Orders"]=std::move(shot_orders);
            auto store_loops=[&](const std::string& prefix,const std::vector<ssne::NativeWeaponLoop>& loops){std::vector<int64_t> ids,codes,durations,origins,orders,ages;std::vector<float> positions;for(const auto& loop:loops){ids.push_back(int64_t(loop.sequence));codes.push_back(loop.slot*4+loop.lane*2+unsigned(loop.active)+(loop.weapon<<5));origins.push_back(int64_t(loop.origin));orders.push_back(int64_t(loop.order));durations.push_back(loop.duration_ms);ages.push_back(loop.elapsed_ms);positions.insert(positions.end(),loop.position.begin(),loop.position.end());}state.mission[prefix+"Ids"]=std::move(ids);state.mission[prefix+"Origins"]=std::move(origins);state.mission[prefix+"Orders"]=std::move(orders);state.mission[prefix+"Codes"]=std::move(codes);state.mission[prefix+"Durations"]=std::move(durations);state.mission[prefix+"Elapsed"]=std::move(ages);state.mission[prefix+"Positions"]=std::move(positions);};
            const std::vector<ssne::NativeWeaponLoop> empty_loops;store_loops("Session.WeaponLoops.",source_publish?producer.loops:empty_loops);store_loops("Session.WeaponLoops.Active.",ssne::native_weapon_active_loops());
            state.mission["Session.WeaponCues.State"]=ssne::native_weapon_cue_state();
            std::vector<int64_t> feedback_ids,feedback_codes;std::vector<float> feedback_values;
            if(source_publish)for(const auto& event:producer.feedback){feedback_ids.push_back(int64_t(event.sequence));feedback_codes.insert(feedback_codes.end(),{event.kind,event.code,event.variant});feedback_values.insert(feedback_values.end(),event.values.begin(),event.values.end());}
            state.mission["Session.Feedback.Ids"]=std::move(feedback_ids);state.mission["Session.Feedback.Codes"]=std::move(feedback_codes);state.mission["Session.Feedback.Values"]=std::move(feedback_values);
            std::vector<int64_t> trail_ids,trail_masks;std::vector<float> trail_vectors;
            for(const auto& trail:producer.trails){
                if(!source_publish)break;
                trail_ids.push_back(int64_t(trail.sequence));trail_masks.push_back(trail.mask);
                trail_vectors.insert(trail_vectors.end(),trail.vectors.begin(),trail.vectors.end());
                trail_vectors.insert(trail_vectors.end(),trail.view.begin(),trail.view.end());
            }
            state.mission["Session.BulletTrails.Ids"]=std::move(trail_ids);
            state.mission["Session.BulletTrails.Masks"]=std::move(trail_masks);
            state.mission["Session.BulletTrails.Vectors"]=std::move(trail_vectors);
            std::vector<int64_t> emitter_ids,emitter_tags;std::vector<float> emitter_values;
            for(const auto& event:producer.emitters){
                if(!source_publish)break;
                emitter_ids.push_back(int64_t(event.sequence));
                emitter_tags.insert(emitter_tags.end(),event.tags.begin(),event.tags.end());
                emitter_values.insert(emitter_values.end(),event.values.begin(),event.values.end());
            }
            state.mission["Session.WeaponEmitters.Ids"]=std::move(emitter_ids);
            state.mission["Session.WeaponEmitters.Tags"]=std::move(emitter_tags);
            state.mission["Session.WeaponEmitters.Values"]=std::move(emitter_values);
            std::vector<int64_t> casing_ids,casing_tags;std::vector<float> casing_values;
            for(const auto& event:producer.casings){
                if(!source_publish)break;
                casing_ids.push_back(int64_t(event.sequence));
                casing_tags.insert(casing_tags.end(),{event.slot,event.weapon,event.type,event.mask});
                casing_values.insert(casing_values.end(),event.values.begin(),event.values.end());
            }
            state.mission["Session.WeaponCasings.Ids"]=std::move(casing_ids);
            state.mission["Session.WeaponCasings.Tags"]=std::move(casing_tags);
            state.mission["Session.WeaponCasings.Values"]=std::move(casing_values);
            std::vector<int64_t> projectile_ids,projectile_tags;std::vector<float> projectile_values;
            if(source_publish)for(const auto& event:projectile_producer.events){
                projectile_ids.push_back(int64_t(event.sequence));
                projectile_tags.insert(projectile_tags.end(),event.tags.begin(),event.tags.end());
                projectile_values.insert(projectile_values.end(),event.values.begin(),event.values.end());
            }
            state.mission["Session.ProjectileVisuals.Ids"]=std::move(projectile_ids);
            state.mission["Session.ProjectileVisuals.Tags"]=std::move(projectile_tags);
            state.mission["Session.ProjectileVisuals.Values"]=std::move(projectile_values);
            auto store_visuals=[&](const char* prefix,const auto& events,auto tags){
                std::vector<int64_t> ids,codes;std::vector<float> values;
                if(source_publish)for(const auto& event:events){ids.push_back(int64_t(event.sequence));const auto row=tags(event);codes.insert(codes.end(),row.begin(),row.end());values.insert(values.end(),event.values.begin(),event.values.end());}
                state.mission[std::string(prefix)+"Ids"]=std::move(ids);state.mission[std::string(prefix)+"Tags"]=std::move(codes);state.mission[std::string(prefix)+"Values"]=std::move(values);
            };
            store_visuals("Session.WeaponSurfaces.",producer.surfaces,[](const auto& e){return std::array<int64_t,2>{e.weapon,e.kind};});
            store_visuals("Session.WeaponBeams.",producer.beams,[](const auto& e){return std::array<int64_t,4>{e.slot,e.weapon,e.hand,e.flags};});
            std::vector<int64_t> impact_ids,impact_tags;std::vector<float> impact_vectors;for(const auto& impact:producer.impacts){if(!source_publish)break;impact_ids.push_back(int64_t(impact.sequence));const auto& t=impact.tags;impact_tags.push_back(int64_t(uint64_t(t[0])|(uint64_t(t[1])<<8)|(uint64_t(t[2])<<16)|(uint64_t(t[3])<<17)|(uint64_t(t[4])<<18)));impact_vectors.insert(impact_vectors.end(),impact.vectors.begin(),impact.vectors.end());}state.mission["Session.BulletImpacts.Ids"]=std::move(impact_ids);state.mission["Session.BulletImpacts.Tags"]=std::move(impact_tags);state.mission["Session.BulletImpacts.Vectors"]=std::move(impact_vectors);
        }
        if(!loading)if(auto music=ssne::native_music_pending_request()){
            state.mission["Session.Music.Sequence"]=int64_t(music->sequence);
            state.mission["Session.Music.Track"]=int64_t(music->track);
            state.mission["Session.Music.Force"]=int64_t(music->force);
            state.mission["Session.Music.Fade"]=music->fade;
        }
        if(!loading)if(auto speech=ssne::native_speech()){
            state.mission["Session.Speech.Sequence"]=int64_t(speech->sequence);
            state.mission["Session.Speech.Track"]=int64_t(speech->track);
            state.mission["Session.Speech.Reference"]=int64_t(speech->reference);
            state.mission["Session.Speech.OutputFrames"]=int64_t(speech->output_frames);
            state.mission["Session.Speech.OutputRate"]=int64_t(32000);
            state.mission["Session.Speech.Phase"]=int64_t(speech->started?1:0);
            if(frame%30==0 && log_)log_("[coop-speech-position-host] sequence="+std::to_string(speech->sequence)+" output_frames="+std::to_string(speech->output_frames));
        }
        {const auto movie=ssne::native_movie_state();
            state.mission["Session.Movie.Sequence"]=int64_t(movie.sequence);
            state.mission["Session.Movie.Asset"]=movie.asset;
            state.mission["Session.Movie.Frame"]=int64_t(movie.frame);
            state.mission["Session.Movie.Active"]=int64_t(movie.active);
        }
        if(!loading && active_cutscene_vote_ && active_cutscene_object_ && sample.scene!=5) {
            std::vector<float> camera;for(unsigned i=0;i<14;++i)camera.push_back(u2f(mem_read32(active_cutscene_object_+0x140+i*4)));
            state.mission["Session.Cutscene.Id"]=int64_t(active_cutscene_vote_);
            state.mission["Session.Cutscene.Reference"]=int64_t(mem_read32(active_cutscene_object_+0x50));
            state.mission["Session.Cutscene.Camera"]=std::move(camera);
        }
        std::vector<int64_t> waiting_slots;for(const auto& [id,e]:sample.entities)if(e.kind==EntityKind::Player && ssne::native_party_spawn_waiting(native_.local_address(id)))waiting_slots.push_back(std::get<int64_t>(e.properties.at("player_slot")));
        for(unsigned slot=0;slot<capacity_;++slot)if(join_placements_[slot].player.generation && std::find(waiting_slots.begin(),waiting_slots.end(),int64_t(slot))==waiting_slots.end())waiting_slots.push_back(slot);
        state.mission["Session.PendingSpawnSlots"]=waiting_slots;
        if(auto results=ssne::native_results();results && results->active) {
            state.mission["Session.Results.Active"]=int64_t(1);state.mission["Session.Results.Sequence"]=int64_t(results->sequence);
            state.mission["Session.Results.Level"]=level_;state.mission["Session.Results.Score"]=int64_t(results->score);
            state.mission["Session.Results.HighScore"]=int64_t(results->high_score);
            state.mission["Session.Results.Values"]=std::vector<float>(results->values.begin(),results->values.end());state.mission["Session.Results.Coefficients"]=std::vector<float>(results->coefficients.begin(),results->coefficients.end());
            if(frame%60==0 && log_)log_("[coop-host-results] sequence="+std::to_string(results->sequence)+" score="+std::to_string(results->score)+" elapsed_seconds="+std::to_string(results->values[1]*60.f));
        }


        std::string error;
        std::vector<WorldState> native_batches;
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_CARD_OBSERVE") && frame%300==0 && ssne::exi_memcard_writes()) {
            const auto card=ssne::exi_memcard_snapshot();
            std::ofstream out("native-card-observed.bin",std::ios::binary|std::ios::trunc);
            out.write(reinterpret_cast<const char*>(card.data()),card.size());
            if(log_)log_("[coop-card-observed] frame="+std::to_string(frame)+" writes="+std::to_string(ssne::exi_memcard_writes())+" bytes="+std::to_string(card.size())+" stored="+std::to_string(bool(out)));
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && ssne::exi_memcard_writes() && mem_read32(0x802DAC90u)>=0x80000000u){
            static bool dumped=false;const auto manager=mem_read32(0x802DAC90u);
            if(!dumped && manager<=ssne::guest_ram_end()-16 && !mem_read32(manager+0xC)){dump_native_profiles("native-profiles-saved.tsv");dumped=true;if(log_)log_("[coop-native-card-save-observed] writes="+std::to_string(ssne::exi_memcard_writes()));}
        }
        {
            std::array<ParticipantHealth,max_players> health{};
            for(const auto& [id,entity]:state.entities)if(entity.kind==EntityKind::Player && entity.owner.slot<max_players){
                if(auto value=entity.properties.find("health");value!=entity.properties.end())if(auto hp=std::get_if<float>(&value->second))health[entity.owner.slot]={entity.owner,*hp,!loading};
            }
            std::lock_guard lock(combat_mutex_);participant_health_=health;
        }
        try{native_batches=NativeEventJournal::producer_batches(state);}catch(const std::exception& e){report(e.what());return;}
        // state owns independent copies of sample.entities/setup_. The loops
        // above retain no mutable references; producer_batches also owns its
        // values. No state reference is used after this ownership transfer.
        if(![&]{NativeStageTimer timing("publish",log_);return server_->publish_owned(std::move(state),error,loading,native_batches);}()){report(error);return;}
        if(!loading && source_publish){
            const bool acknowledged=ssne::acknowledge_native_event_publication(producer.epoch,accepted);
            if(!native_.acknowledge_projectile_visuals(projectile_producer.epoch,projectile_producer.tail)){
                report("Native projectile visual producer changed during publication");return;
            }
            if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES"))for(const auto& event:projectile_producer.events)
                log_("[coop-projectile-visual-host] sequence="+std::to_string(event.sequence)+" id="+std::to_string(event.tags[0])+" definition="+std::to_string(event.tags[1]));
            if(!native_batches.empty() && log_)log_("[coop-native-producer-burst] epoch="+std::to_string(producer.epoch)+" batches="+std::to_string(native_batches.size())+" hits="+std::to_string(producer.hits.size())+" rockets="+std::to_string(producer.rockets.size())+" shots="+std::to_string(producer.shots.size())+" loops="+std::to_string(producer.loops.size())+" impacts="+std::to_string(producer.impacts.size())+" acknowledged="+std::to_string(acknowledged));
            if(acknowledged && !native_batches.empty() && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && log_){
                const auto pending=ssne::native_event_producer_snapshot();
                if(pending.epoch==producer.epoch)log_("[coop-native-producer-ack] epoch="+std::to_string(pending.epoch)+" hits="+std::to_string(pending.hits.size())+" rockets="+std::to_string(pending.rockets.size())+" shots="+std::to_string(pending.shots.size())+" loops="+std::to_string(pending.loops.size())+" impacts="+std::to_string(pending.impacts.size()));
            }
        }
        const auto save_epoch=epoch_;
        // The original result screen has already destroyed the level's player
        // actors. It is still a complete checkpoint state, with native GUI,
        // result counters and the pre-continuation card retained together.
        const auto save_result=ssne::native_results();
        const bool result_screen=save_result && save_result->active;
        if(save_result && env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_TEST_RESULTS_POST_SAVE")){
            static bool requested=false;
            if(!requested){
                std::vector<uint8_t> story;std::string why;
                if(!ssne::native_story_save_state(story,why))throw std::runtime_error(why);
                uint64_t committed=0;for(unsigned i=40;i<48;++i)committed=(committed<<8)|story.at(i);
                if(committed && committed==save_result->sequence){
                    requested=true;
                    const bool accepted=request_save(true,1,getenv("SSNE_TEST_RESULTS_POST_SAVE"),why);
                    if(log_)log_("[coop-result-post-save-test] frame="+std::to_string(frame)+" sequence="+std::to_string(committed)+" active="+std::to_string(result_screen)+" loading="+std::to_string(loading)+" accepted="+std::to_string(accepted)+" error="+why);
                    if(!accepted)throw std::runtime_error(why);
                }
            }
        }
        if(result_screen && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_CARRY_PICKUPS") && ssne::native_party_capacity()>=capacity_){
            static uint64_t recorded=0;
            if(recorded!=epoch_){recorded=epoch_;for(unsigned slot=0;slot<capacity_;++slot){
                const auto record=ssne::native_party_loadouts()+44*slot;std::string ammo;
                for(unsigned i=0;i<18;++i)ammo+=(ammo.empty()?"":",")+std::to_string(mem_read16(record+8+2*i));
                if(log_)log_("[coop-carry-record] epoch="+std::to_string(epoch_)+" slot="+std::to_string(slot)+" mask="+std::to_string(mem_read32(record))+" selected="+std::to_string(mem_read32(record+4))+" ammo="+ammo);
            }}
        }
        process_save_request(context,(!loading || result_screen) && sample.scene==4 && sample.error.empty(),frame);
        if(epoch_!=save_epoch)return;
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_TEST_HISTORICAL_CHECKPOINT") && !loading && sample.error.empty()){
            std::string why;const auto before=epoch_;
            ssne::runtime_checkpoint_visit([&]{if(!test_historical_checkpoint(context,frame,why))ssne::fatal(("Historical checkpoint failed: "+why).c_str());},why);
            if(epoch_!=before)return; // The pre-load sample no longer describes RAM.
        }
        const auto checkpoint_result=ssne::native_results();
        const bool checkpoint_results_ready=env_present("SSNE_TEST_STORY_CHECKPOINT") && std::string(getenv("SSNE_TEST_STORY_CHECKPOINT"))=="results" && checkpoint_result && checkpoint_result->active;
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_LIFETIME_CHECKPOINT") && env_requested("SSNE_TEST_RUNTIME_CHECKPOINT") && sample.scene==4 && sample.error.empty() && (!loading || checkpoint_results_ready) && frame>=600){
            static bool done=false;
            const auto effect_mode=getenv("SSNE_TEST_EFFECT_CHECKPOINT");
            const auto effects=ssne::native_effect_checkpoint_stats();
            const bool effect_ready=!effect_mode || (std::string(effect_mode)=="rocket"?effects.rockets>0:effects.loops>0);
            const bool music_test=env_requested("SSNE_TEST_MUSIC_CHECKPOINT");
            const auto voice=music_test?ssne::native_speech():std::nullopt;
            const bool music_ready=!music_test || (voice && voice->started && voice->output_frames>3200);
            const auto story_mode=getenv("SSNE_TEST_STORY_CHECKPOINT");
            const auto result=ssne::native_results();
            const bool story_ready=!story_mode || (std::string(story_mode)=="cutscene"?ssne::native_cutscene_presenting():(result && result->active));
            const auto session_mode=getenv("SSNE_TEST_SESSION_CHECKPOINT");
            const auto current_vote=server_->skip_vote();
            const bool session_ready=!session_mode || (std::string(session_mode)=="cutscene" ? (active_cutscene_object_ && mem_read32(active_cutscene_object_+0x50)==1953 && current_vote.received && current_vote.received<current_vote.required) : std::string(session_mode)=="respawn" ? std::any_of(afk_respawn_wait_.begin(),afk_respawn_wait_.end(),[](const auto& wait){return wait.actor!=0;}) : (frame>=900 && std::any_of(afk_bots_.begin(),afk_bots_.end(),[](const auto& bot){return bot.checkpoint().trail.size()>1;})));
            if(!done && effect_ready && music_ready && story_ready && session_ready){std::string why;done=ssne::runtime_checkpoint_visit([&]{
                auto session=session_checkpoint_hooks();
                if(effect_mode && !ssne::native_effect_checkpoint_probe(why))ssne::fatal(("Effect checkpoint failed: "+why).c_str());
                if(music_test && !ssne::native_music_checkpoint_probe(why))ssne::fatal(("Music checkpoint failed: "+why).c_str());
                if(story_mode && !ssne::native_story_checkpoint_probe(why))ssne::fatal(("Story checkpoint failed: "+why).c_str());
                if(session_mode && !test_session_checkpoint(why))ssne::fatal(("Session checkpoint failed: "+why).c_str());
                if(!test_composed_checkpoint(native_,context,frame,why,session))ssne::fatal(("Composed checkpoint failed: "+why).c_str());
                if(log_)log_("[coop-composed-checkpoint] passed=1 frame="+std::to_string(frame));
            },why);}
        }
        if(env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TEST_LIFETIME_CHECKPOINT") && sample.scene==4 && sample.error.empty() && !loading && frame>=600 && frame<=2400 && (frame%600==0 || (frame==1500 && env_requested("SSNE_COOP_TEST_BLOCK_SPAWN")))){
            std::string why;unsigned rejected=0;const auto checkpoint_sample=native_.capture(sample.tick);
            const bool ok=test_native_lifetime_checkpoint(native_,checkpoint_sample,frame,rejected,why);
            if(log_)log_("[coop-native-checkpoint-index-test] frame="+std::to_string(frame)+" epoch="+std::to_string(checkpoint_sample.epoch)+" entities="+std::to_string(checkpoint_sample.entities.size())+" unmapped="+std::to_string(checkpoint_sample.unmapped.size())+" rejected="+std::to_string(rejected)+" passed="+std::to_string(ok)+" error="+why);
            why.clear();const bool party_ok=test_native_party_checkpoint(frame,rejected,why);
            if(log_)log_("[coop-native-checkpoint-party-test] frame="+std::to_string(frame)+" rejected="+std::to_string(rejected)+" passed="+std::to_string(party_ok)+" error="+why);
        }
        if(sample.scene==4)for(const auto& [id,entity]:sample.entities)if(entity.kind==EntityKind::Player && entity.owner.slot<max_players && entity.owner.generation) {
            if(!join_placements_[entity.owner.slot].player.generation && !ssne::native_party_spawn_waiting(native_.local_address(id)) && characters_[entity.owner.slot]!=id && server_->attach_character(entity.owner,id))characters_[entity.owner.slot]=id;
            if(characters_[entity.owner.slot]==id && entity.owner==server_->status().player && !host_ready_)host_ready_=server_->ready_host();
            if(frame%120==0 && log_) {
                const auto& position=std::get<std::array<float,3>>(entity.properties.at("position"));
                log_("[coop-player-position] frame="+std::to_string(frame)+" slot="+std::to_string(entity.owner.slot)+" x="+std::to_string(position[0])+" y="+std::to_string(position[1])+" z="+std::to_string(position[2])+" weapon="+std::to_string(std::get<int64_t>(entity.properties.at("selected_weapon")))+" health="+std::to_string(std::get<float>(entity.properties.at("health")))+" respawn="+std::to_string(std::get<int64_t>(entity.properties.at("combo_phase")))+" native_dead="+std::to_string(mem_read32(native_.local_address(id)+0x328))+" spawn_waiting="+std::to_string(ssne::native_party_spawn_waiting(native_.local_address(id))));
                if(env_requested("SSNE_TEST_HIDDEN")) {
                    std::string owned,ammo;
                    for(auto value:std::get<std::vector<int64_t>>(entity.properties.at("weapon_owned_counts")))owned+=(owned.empty()?"":",")+std::to_string(value);
                    if(auto found=entity.properties.find("ammo");found!=entity.properties.end())for(auto value:std::get<std::vector<int64_t>>(found->second))ammo+=(ammo.empty()?"":",")+std::to_string(value);
                    std::string active;for(auto value:std::get<std::vector<int64_t>>(entity.properties.at("weapon_active_counts")))active+=(active.empty()?"":",")+std::to_string(value);
                    log_("[coop-player-inventory] frame="+std::to_string(frame)+" epoch="+std::to_string(epoch_)+" slot="+std::to_string(entity.owner.slot)+" health="+std::to_string(std::get<float>(entity.properties.at("health")))+" owned="+owned+" ammo="+ammo+" armor="+std::to_string(std::get<float>(entity.properties.at("armor")))+" active="+active+" selected="+std::to_string(std::get<int64_t>(entity.properties.at("selected_weapon"))));
                    if(env_requested("SSNE_COOP_TEST_CARRY_PICKUPS")){
                        const auto actor=native_.local_address(id);auto call=context;call.r[3]=actor;ssne_call(call,0x800C6758);
                        std::string saved_ammo,model_counts,ammo_channels;
                        for(unsigned i=0;i<18;++i)saved_ammo+=(saved_ammo.empty()?"":",")+std::to_string(mem_read16(actor+0x47C+2*i));
                        for(unsigned i=0;i<12;++i)model_counts+=(model_counts.empty()?"":",")+std::to_string(mem_read32(actor+0x4C4+24*i));
                        // Read the original getters' data (800A0998 / 8009FED8).
                        // No simulated ownership or ammo writes in this probe.
                        for(unsigned i=0;i<12;++i){
                            if(i)ammo_channels+=';';
                            const auto weapon=mem_read32(actor+0x4BC+24*i);
                            if(weapon<0x80000000u || weapon>ssne::guest_ram_end()-0x1D4u){ammo_channels+='?';continue;}
                            const auto count=mem_read32(weapon+0x1BC),channels=mem_read32(weapon+0x1D0);
                            if(count>18 || channels<0x80000000u || channels>ssne::guest_ram_end()-60*count){ammo_channels+='?';continue;}
                            for(unsigned channel=0;channel<count;++channel){if(channel)ammo_channels+=',';ammo_channels+=std::to_string(int32_t(mem_read32(channels+60*channel+0x38)));}
                        }
                        log_("[coop-carry-channels] frame="+std::to_string(frame)+" epoch="+std::to_string(epoch_)+" slot="+std::to_string(entity.owner.slot)+" channels="+ammo_channels);
                        log_("[coop-carry-native] frame="+std::to_string(frame)+" epoch="+std::to_string(epoch_)+" slot="+std::to_string(entity.owner.slot)+" export_mask="+std::to_string(call.r[3])+" import_mask="+std::to_string(mem_read32(actor+0x478))+" grant_mask="+std::to_string(mem_read32(actor+0x4B4))+" remove_mask="+std::to_string(mem_read32(actor+0x4B8))+" import_selected="+std::to_string(mem_read8(actor+0x4A0))+" import_ammo="+saved_ammo+" model_counts="+model_counts);
                        static std::array<unsigned,max_players> post_level_samples{};
                        if(epoch_==2 && ++post_level_samples[entity.owner.slot]==3){
                            std::string why;const bool ok=apply_native_weapon_selection(context,actor,1,true,why);
                            log_("[coop-carry-equip-pistol] slot="+std::to_string(entity.owner.slot)+" ok="+std::to_string(ok)+" error="+why);
                        }
                    }
                }

            }
        }
        if(frame%60==0 && log_)log_("[coop-host-frame] frame="+std::to_string(frame)+" scene="+std::to_string(sample.scene)+" mode="+std::to_string(sample.mode)+" revision="+std::to_string(server_->status().world_revision)+" phase="+std::string(presentation?(sample.scene==5?"credits":"menu"):loading?"loading":"gameplay")+" epoch="+std::to_string(epoch_)+" clients="+std::to_string(server_->status().joined)+" network="+server_->status().message+" unmapped="+std::to_string(sample.unmapped.size()));
        {std::string why;if(!ssne::tick_native_results_auto_continue(context,native_frontend_prepare_results_close,why)){report(why);return;}}
    } else {
        if(frame%30==0 && log_ && replica_cutscene_id_ && ssne::native_cutscene_presenting())log_("[coop-client-cutscene-camera-after-render] id="+std::to_string(replica_cutscene_id_)+" host_tick="+std::to_string(replica_cutscene_tick_)+" differences="+std::to_string(ssne::native_cutscene_camera_differences(replica_cutscene_camera_)));
        if(frame%60==0 && log_ && !enemy_pose_expected_.empty()) {
            unsigned models=0,changed=0;
            for(const auto& [id,pose]:enemy_pose_expected_) {
                const auto binding=native_enemy_pose_binding(replica_.local_address(id));
                if(!binding || binding->count*12!=pose.size()){++changed;continue;}++models;
                for(unsigned i=0;i<pose.size();++i)changed+=mem_read32(binding->matrices+i*4)!=f2u(pose[i]);
            }
            log_("[coop-enemy-pose-after-render] models="+std::to_string(models)+" changed_coefficients="+std::to_string(changed));
        }
        auto results_state=client_->latest_world();
        if(queued_ && !level_loading_ && mem_read32(ssne::game_address(0x802DB0E0))==4){
            std::string why;
            if(results_state->mission.contains("Session.Results.Active")){
                ssne::NativeResults desired;desired.sequence=uint64_t(std::get<int64_t>(results_state->mission.at("Session.Results.Sequence")));desired.score=uint32_t(std::get<int64_t>(results_state->mission.at("Session.Results.Score")));
                desired.high_score=uint32_t(std::get<int64_t>(results_state->mission.at("Session.Results.HighScore")));
                const auto& values=std::get<std::vector<float>>(results_state->mission.at("Session.Results.Values"));if(values.size()!=13){report("Invalid original result values");return;}std::copy(values.begin(),values.end(),desired.values.begin());
                const auto coefficients=results_state->mission.find("Session.Results.Coefficients");const auto* factors=coefficients==results_state->mission.end()?nullptr:std::get_if<std::vector<float>>(&coefficients->second);if(!factors || factors->size()!=26){report("Invalid original result coefficients");return;}std::copy(factors->begin(),factors->end(),desired.coefficients.begin());
                if(!ssne::apply_native_replica_results(context,desired,why)){report(why);return;}
            }else if(!ssne::clear_native_replica_results(context,why)){report(why);return;}
        }
        if(frame%60==0 && log_ && results_state->mission.contains("Session.Results.Active"))log_("[coop-client-results] sequence="+std::to_string(std::get<int64_t>(results_state->mission.at("Session.Results.Sequence")))+" score="+std::to_string(std::get<int64_t>(results_state->mission.at("Session.Results.Score")))+" elapsed_seconds="+std::to_string(std::get<std::vector<float>>(results_state->mission.at("Session.Results.Values"))[1]*60.f));
        const auto received=client_->world_sample();auto state=received.world;if(!queued_ || !state->revision || state->entities.empty() || state->revision==last_revision_ || mem_read32(ssne::game_address(0x802DB0E0))!=4)return;
        if(level_loading_) {
            const auto active=native_active_level();const auto local=native_.capture(frame);
            // A same-level rewind leaves the old level name and all its players
            // visible until the queued native loader starts. They are not proof
            // of completion: resuming effects there can dereference freed assets.
            if(local.epoch<=loading_native_epoch_){
                if(env_requested("SSNE_TEST_HIDDEN") && log_){static uint64_t observed=0;if(observed!=loading_epoch_){observed=loading_epoch_;log_("[coop-client-reload-wait] epoch="+std::to_string(loading_epoch_)+" native_epoch="+std::to_string(local.epoch)+" floor="+std::to_string(loading_native_epoch_));}}
                return;
            }
            if(env_requested("SSNE_TEST_HIDDEN") && frame%120==0 && log_)log_("[coop-client-load-state] active="+active.value_or("none")+" expected="+level_+" players="+std::to_string(std::count_if(local.entities.begin(),local.entities.end(),[](const auto& item){return item.second.kind==EntityKind::Player;}))+" error="+local.error);
            if(!active || *active!=level_ || !local.error.empty() || std::count_if(local.entities.begin(),local.entities.end(),[](const auto& item){return item.second.kind==EntityKind::Player;})<std::min(capacity_,ssne::native_party_capacity()))return;
            level_loading_=false;report("authoritative level loaded: "+level_+" epoch="+std::to_string(loading_epoch_)+" native_epoch="+std::to_string(local.epoch)+" floor="+std::to_string(loading_native_epoch_));
        }
        // Validate complete weapon histories before snapshot-driven native changes.
        NativeEventCursors event_tails{};bool journaled=false;
        if(auto tails=state->mission.find("Session.NativeEventTails");tails!=state->mission.end()) {
            const auto* values=std::get_if<std::vector<int64_t>>(&tails->second);
            if(!values || values->size()!=native_event_channels || std::any_of(values->begin(),values->end(),[](auto value){return value<0;})){report("Invalid native event journal tails");return;}
            for(unsigned i=0;i<native_event_channels;++i)event_tails[i]=uint64_t((*values)[i]);journaled=true;
        }
        const auto effect_ids=state->mission.find("Session.RocketEffects.Ids"),effect_values=state->mission.find("Session.RocketEffects.Values");
        if(effect_ids==state->mission.end() || effect_values==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(effect_ids->second) || !std::holds_alternative<std::vector<float>>(effect_values->second)){report("Incomplete authoritative rocket effect history");return;}
        const auto& ids=std::get<std::vector<int64_t>>(effect_ids->second);const auto& values=std::get<std::vector<float>>(effect_values->second);
        const auto style_field=state->mission.find("Session.RocketEffects.Styles");if(style_field==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(style_field->second)){report("Incomplete authoritative explosion styles");return;}const auto& styles=std::get<std::vector<int64_t>>(style_field->second);
        if(ids.size()>256 || values.size()!=ids.size()*10 || styles.size()!=ids.size()){report("Invalid authoritative rocket effect history size");return;}
        std::vector<ssne::NativeRocketEffect> effects;
        for(unsigned index=0;index<ids.size();++index){if(ids[index]<=0 || styles[index]<0 || styles[index]>4 || (index && ids[index]!=ids[index-1]+1)){report("Invalid authoritative rocket effect sequence");return;}ssne::NativeRocketEffect effect;effect.sequence=uint64_t(ids[index]);effect.style=unsigned(styles[index]);std::copy_n(values.begin()+index*10,10,effect.values.begin());std::string why;if(!ssne::validate_native_rocket_effect(effect,why)){report(why);return;}effects.push_back(effect);}
        if(replica_effect_epoch_==state->epoch && !ids.empty() && uint64_t(ids.front())>replica_effect_sequence_+1){report("Authoritative rocket effect history overrun");return;}
        const auto shot_ids=state->mission.find("Session.WeaponShots.Ids"),shot_positions=state->mission.find("Session.WeaponShots.Positions");
        if(shot_ids==state->mission.end() || shot_positions==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(shot_ids->second) || !std::holds_alternative<std::vector<float>>(shot_positions->second)){report("Incomplete authoritative weapon shot history");return;}
        const auto& shot_metadata=std::get<std::vector<int64_t>>(shot_ids->second);const auto& positions=std::get<std::vector<float>>(shot_positions->second);
        const auto shot_types=state->mission.find("Session.WeaponShots.Weapons");
        if(shot_types==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(shot_types->second)){report("Incomplete authoritative weapon shot types");return;}
        const auto& weapons=std::get<std::vector<int64_t>>(shot_types->second);
        const auto cue_types=state->mission.find("Session.WeaponShots.Cues");if(cue_types==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(cue_types->second)){report("Incomplete authoritative weapon cues");return;}const auto& cues=std::get<std::vector<int64_t>>(cue_types->second);
        const auto shot_order_field=state->mission.find("Session.WeaponShots.Orders");if(shot_order_field==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(shot_order_field->second)){report("Incomplete authoritative weapon sound order");return;}const auto& shot_orders=std::get<std::vector<int64_t>>(shot_order_field->second);
        const auto shot_flags_field=state->mission.find("Session.WeaponShots.Flags");
        if(shot_flags_field==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(shot_flags_field->second)){report("Incomplete authoritative weapon sound flags");return;}
        const auto& shot_flags=std::get<std::vector<int64_t>>(shot_flags_field->second);
        if(shot_metadata.size()%2 || shot_metadata.size()>256 || positions.size()!=shot_metadata.size()/2*3 || weapons.size()!=shot_metadata.size()/2 || cues.size()!=weapons.size() || shot_orders.size()!=weapons.size() || shot_flags.size()!=weapons.size()){report("Invalid authoritative weapon shot history size");return;}
        std::vector<ssne::NativeWeaponShot> shots;
        for(unsigned index=0;index<shot_metadata.size()/2;++index){if(shot_metadata[index*2]<=0 || shot_metadata[index*2+1]<0 || shot_metadata[index*2+1]>=capacity_ || weapons[index]<0 || weapons[index]>=12 || cues[index]<0 || cues[index]>=600 || shot_flags[index]<0 || shot_flags[index]>1 || shot_orders[index]<=0 || (index && shot_orders[index]<=shot_orders[index-1]) || (index && shot_metadata[index*2]!=shot_metadata[index*2-2]+1)){report("Invalid authoritative weapon shot sequence/slot/type");return;}ssne::NativeWeaponShot shot;shot.sequence=uint64_t(shot_metadata[index*2]);shot.order=uint64_t(shot_orders[index]);shot.slot=unsigned(shot_metadata[index*2+1]);shot.weapon=unsigned(weapons[index]);shot.cue_sound=unsigned(cues[index]);shot.cue_flags=unsigned(shot_flags[index]);std::copy_n(positions.begin()+index*3,3,shot.position.begin());std::string why;if(!ssne::validate_native_weapon_shot(shot,why)){report(why);return;}shots.push_back(shot);}
        if(replica_shot_epoch_==state->epoch && !shots.empty() && shots.front().sequence>replica_shot_sequence_+1){report("Authoritative weapon shot history overrun");return;}
        std::vector<ssne::NativeWeaponLoop> loops,active_loops;
        auto read_loops=[&](const std::string& prefix,std::vector<ssne::NativeWeaponLoop>& result,bool history){const auto ids=state->mission.find(prefix+"Ids"),codes=state->mission.find(prefix+"Codes"),durations=state->mission.find(prefix+"Durations"),positions=state->mission.find(prefix+"Positions"),origins=state->mission.find(prefix+"Origins"),orders=state->mission.find(prefix+"Orders"),ages=state->mission.find(prefix+"Elapsed");if(ages==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(ages->second))return false;if(orders==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(orders->second))return false;if(origins==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(origins->second))return false;if(ids==state->mission.end() || codes==state->mission.end() || durations==state->mission.end() || positions==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(ids->second) || !std::holds_alternative<std::vector<int64_t>>(codes->second) || !std::holds_alternative<std::vector<int64_t>>(durations->second) || !std::holds_alternative<std::vector<float>>(positions->second))return false;const auto& age=std::get<std::vector<int64_t>>(ages->second);const auto& order=std::get<std::vector<int64_t>>(orders->second);const auto& o=std::get<std::vector<int64_t>>(origins->second);const auto& n=std::get<std::vector<int64_t>>(ids->second);const auto& c=std::get<std::vector<int64_t>>(codes->second);const auto& d=std::get<std::vector<int64_t>>(durations->second);const auto& p=std::get<std::vector<float>>(positions->second);if(n.size()>(history?128u:16u) || age.size()!=n.size() || order.size()!=n.size() || o.size()!=n.size() || c.size()!=n.size() || d.size()!=n.size() || p.size()!=n.size()*3)return false;std::set<unsigned> keys;for(unsigned i=0;i<n.size();++i){if(n[i]<=0 || c[i]<0 || c[i]>=160 || (c[i]&31)>=int64_t(capacity_*4) || o[i]<=0 || age[i]<0 || age[i]>60000 || (history && age[i]) || order[i]<=0 || (history && i && order[i]<=order[i-1]) || d[i]<0 || d[i]>60000 || (history && i && n[i]!=n[i-1]+1))return false;ssne::NativeWeaponLoop loop;loop.sequence=uint64_t(n[i]);loop.order=uint64_t(order[i]);loop.weapon=unsigned(c[i])>>5;loop.origin=uint64_t(o[i]);loop.slot=(unsigned(c[i])&31)/4;loop.lane=(unsigned(c[i])/2)%2;loop.active=unsigned(c[i])%2;loop.duration_ms=uint32_t(d[i]);loop.elapsed_ms=uint32_t(age[i]);std::copy_n(p.begin()+i*3,3,loop.position.begin());std::string why;if(!ssne::validate_native_weapon_loop(loop,why) || (!history && (!loop.active || !keys.insert(loop.slot*2+loop.lane).second)))return false;result.push_back(loop);}return true;};
        if(!read_loops("Session.WeaponLoops.",loops,true) || !read_loops("Session.WeaponLoops.Active.",active_loops,false)){report("Invalid authoritative Uzi loop state/history");return;}
        if(replica_loop_epoch_==state->epoch && !loops.empty() && loops.front().sequence>replica_loop_sequence_+1){report("Authoritative Uzi loop history overrun");return;}
        const auto impact_ids=state->mission.find("Session.BulletImpacts.Ids"),impact_tags=state->mission.find("Session.BulletImpacts.Tags"),impact_vectors=state->mission.find("Session.BulletImpacts.Vectors");if(impact_ids==state->mission.end() || impact_tags==state->mission.end() || impact_vectors==state->mission.end() || !std::holds_alternative<std::vector<int64_t>>(impact_ids->second) || !std::holds_alternative<std::vector<int64_t>>(impact_tags->second) || !std::holds_alternative<std::vector<float>>(impact_vectors->second)){report("Incomplete authoritative bullet impact history");return;}const auto& impact_n=std::get<std::vector<int64_t>>(impact_ids->second);const auto& impact_t=std::get<std::vector<int64_t>>(impact_tags->second);const auto& impact_v=std::get<std::vector<float>>(impact_vectors->second);if(impact_n.size()>128 || impact_t.size()!=impact_n.size() || impact_v.size()!=impact_n.size()*9){report("Invalid authoritative bullet impact history size");return;}std::vector<ssne::NativeBulletImpact> impacts;for(unsigned i=0;i<impact_n.size();++i){if(impact_n[i]<=0 || (i && impact_n[i]!=impact_n[i-1]+1) || impact_t[i]<0 || uint64_t(impact_t[i])>=1ull<<26){report("Invalid authoritative bullet impact identity/tags");return;}const auto t=uint64_t(impact_t[i]);ssne::NativeBulletImpact impact;impact.sequence=uint64_t(impact_n[i]);impact.tags={uint32_t(t&255),uint32_t(t>>8&255),uint32_t(t>>16&1),uint32_t(t>>17&1),uint32_t(t>>18&255)};std::copy_n(impact_v.begin()+i*9,9,impact.vectors.begin());std::string why;if(!ssne::validate_native_bullet_impact(impact,why)){report(why);return;}impacts.push_back(impact);}if(replica_impact_epoch_==state->epoch && !impacts.empty() && impacts.front().sequence>replica_impact_sequence_+1){report("Authoritative bullet impact history overrun");return;}
        auto hit_ints=[&](const char* name)->const std::vector<int64_t>*{const auto found=state->mission.find(std::string("Session.ConfirmedHits.")+name);return found==state->mission.end()?nullptr:std::get_if<std::vector<int64_t>>(&found->second);};
        const auto hit_ids=hit_ints("Ids"),hit_shots=hit_ints("Shots"),hit_sources=hit_ints("Sources"),hit_targets=hit_ints("Targets"),hit_generations=hit_ints("Generations"),hit_codes=hit_ints("Codes"),hit_pellets=hit_ints("Pellets");const auto hit_value_field=state->mission.find("Session.ConfirmedHits.Values");const auto hit_values=hit_value_field==state->mission.end()?nullptr:std::get_if<std::vector<float>>(&hit_value_field->second);
        if(!hit_ids || !hit_shots || !hit_sources || !hit_targets || !hit_generations || !hit_codes || !hit_pellets || !hit_values || hit_ids->size()>128 || hit_shots->size()!=hit_ids->size() || hit_sources->size()!=hit_ids->size() || hit_targets->size()!=hit_ids->size() || hit_generations->size()!=hit_ids->size() || hit_codes->size()!=hit_ids->size() || hit_pellets->size()!=hit_ids->size() || hit_values->size()!=hit_ids->size()*3){report("Incomplete authoritative confirmed hit history");return;}
        std::vector<ssne::NativeConfirmedHit> confirmed_hits;std::set<std::tuple<uint64_t,unsigned,uint64_t>> hit_pairs;
        for(unsigned i=0;i<hit_ids->size();++i){if((*hit_ids)[i]<=0 || (*hit_shots)[i]<=0 || (*hit_sources)[i]<=0 || (*hit_targets)[i]<=0 || (*hit_generations)[i]<=0 || (*hit_codes)[i]<0 || (*hit_codes)[i]>=int64_t(capacity_*16) || (*hit_pellets)[i]<0 || (*hit_pellets)[i]>13 || (i && (*hit_ids)[i]!=(*hit_ids)[i-1]+1)){report("Invalid authoritative confirmed hit identity");return;}ssne::NativeConfirmedHit hit;hit.sequence=uint64_t((*hit_ids)[i]);hit.shot=uint64_t((*hit_shots)[i]);hit.source=uint64_t((*hit_sources)[i]);hit.target=uint64_t((*hit_targets)[i]);hit.generation=uint64_t((*hit_generations)[i]);hit.slot=unsigned((*hit_codes)[i])/16;hit.weapon=unsigned((*hit_codes)[i])%16;hit.pellet=unsigned((*hit_pellets)[i]);hit.before=(*hit_values)[i*3];hit.after=(*hit_values)[i*3+1];hit.amount=(*hit_values)[i*3+2];std::string why;if(!ssne::validate_native_confirmed_hit(hit,why) || !hit_pairs.insert({hit.shot,hit.pellet,hit.target}).second){report(why.empty()?"Duplicate authoritative shot-pellet-victim hit":why);return;}confirmed_hits.push_back(hit);}
        if(replica_hit_epoch_==state->epoch && !confirmed_hits.empty() && confirmed_hits.front().sequence>replica_hit_sequence_+1){report("Authoritative confirmed hit history overrun");return;}
        std::vector<ssne::NativeFeedbackEvent> feedback;
        const auto feedback_ids=state->mission.find("Session.Feedback.Ids"),feedback_codes=state->mission.find("Session.Feedback.Codes"),feedback_values=state->mission.find("Session.Feedback.Values");
        if(feedback_ids==state->mission.end() || feedback_codes==state->mission.end() || feedback_values==state->mission.end()){report("Incomplete native feedback history");return;}
        const auto* fn=std::get_if<std::vector<int64_t>>(&feedback_ids->second);const auto* fc=std::get_if<std::vector<int64_t>>(&feedback_codes->second);const auto* fv=std::get_if<std::vector<float>>(&feedback_values->second);
        if(!fn || !fc || !fv || fn->size()>128 || fc->size()!=fn->size()*3 || fv->size()!=fn->size()*12){report("Invalid feedback history shape");return;}
        for(size_t i=0;i<fn->size();++i){
            if((*fn)[i]<=0 || (i && (*fn)[i]!=(*fn)[i-1]+1)){report("Invalid feedback identity");return;}
            for(unsigned n=0;n<3;++n)if((*fc)[i*3+n]<0 || uint64_t((*fc)[i*3+n])>UINT32_MAX){report("Invalid feedback code");return;}
            ssne::NativeFeedbackEvent event;event.sequence=uint64_t((*fn)[i]);event.kind=uint32_t((*fc)[i*3]);event.code=uint32_t((*fc)[i*3+1]);event.variant=uint32_t((*fc)[i*3+2]);std::copy_n(fv->begin()+i*12,12,event.values.begin());std::string why;if(!ssne::validate_native_feedback(event,why)){report(why);return;}
            if((event.kind==2 || event.kind==7) && event.code>=capacity_){report("Native screen flash targets a slot outside this session");return;}
            if((event.kind==8 || event.kind==9) && event.variant>=capacity_){report("Native combo sound targets a slot outside this session");return;}
            feedback.push_back(event);
        }
        if(replica_feedback_epoch_==state->epoch && !feedback.empty() && feedback.front().sequence>replica_feedback_sequence_+1){report("Native feedback history overrun");return;}
        // A ray can visibly cross the world without hitting anything. Its
        // original streak is an independent event, not an impact or sound.
        std::vector<ssne::NativeBulletTrail> trails;
        const auto trail_ids=state->mission.find("Session.BulletTrails.Ids");
        const auto trail_masks=state->mission.find("Session.BulletTrails.Masks");
        const auto trail_vectors=state->mission.find("Session.BulletTrails.Vectors");
        if(trail_ids==state->mission.end() || trail_masks==state->mission.end() || trail_vectors==state->mission.end()){report("Incomplete native bullet trail history");return;}
        const auto* tn=std::get_if<std::vector<int64_t>>(&trail_ids->second);
        const auto* tm=std::get_if<std::vector<int64_t>>(&trail_masks->second);
        const auto* tv=std::get_if<std::vector<float>>(&trail_vectors->second);
        if(!tn || !tm || !tv || tn->size()>NativeEventJournal::batch_size || tm->size()!=tn->size() || tv->size()!=tn->size()*19){report("Invalid native bullet trail history shape");return;}
        for(size_t i=0;i<tn->size();++i){
            if((*tn)[i]<=0 || (i && (*tn)[i]!=(*tn)[i-1]+1) || (*tm)[i]<0 || uint64_t((*tm)[i])>UINT32_MAX){report("Invalid native bullet trail identity/mask");return;}
            ssne::NativeBulletTrail trail;trail.sequence=uint64_t((*tn)[i]);trail.mask=uint32_t((*tm)[i]);
            std::copy_n(tv->begin()+i*19,6,trail.vectors.begin());
            std::copy_n(tv->begin()+i*19+6,13,trail.view.begin());
            std::string why;if(!ssne::validate_native_bullet_trail(trail,why)){report(why);return;}
            trails.push_back(trail);
        }
        if(replica_trail_epoch_==state->epoch && !trails.empty() && trails.front().sequence>replica_trail_sequence_+1){report("Native bullet trail history overrun");return;}
        std::vector<ssne::NativeWeaponEmitter> emitters;
        std::vector<ssne::NativeWeaponCasing> casings;
        std::vector<NativeProjectileVisual> projectile_visuals;
        std::vector<ssne::NativeWeaponSurface> surfaces;
        std::vector<ssne::NativeWeaponBeam> beams;
        // Stage and validate whole event batches before changing native RAM.
        auto read_visual_events=[&](const char* prefix,auto& events,unsigned tag_width,unsigned value_width,auto unpack,auto validate,uint64_t tag_limit=UINT32_MAX){
            const auto ids=state->mission.find(std::string(prefix)+"Ids");
            const auto tags=state->mission.find(std::string(prefix)+"Tags");
            const auto values=state->mission.find(std::string(prefix)+"Values");
            if(ids==state->mission.end() || tags==state->mission.end() || values==state->mission.end())return false;
            const auto* n=std::get_if<std::vector<int64_t>>(&ids->second);
            const auto* t=std::get_if<std::vector<int64_t>>(&tags->second);
            const auto* v=std::get_if<std::vector<float>>(&values->second);
            if(!n || !t || !v || n->size()>NativeEventJournal::batch_size || t->size()!=n->size()*tag_width || v->size()!=n->size()*value_width)return false;
            for(size_t i=0;i<n->size();++i){
                if((*n)[i]<=0 || (i && (*n)[i]!=(*n)[i-1]+1))return false;
                for(unsigned j=0;j<tag_width;++j)if((*t)[i*tag_width+j]<0 || uint64_t((*t)[i*tag_width+j])>tag_limit)return false;
                typename std::decay_t<decltype(events)>::value_type event;event.sequence=uint64_t((*n)[i]);
                unpack(event,t->data()+i*tag_width);std::copy_n(v->begin()+i*value_width,value_width,event.values.begin());
                std::string why;if(!validate(event,why))return false;
                events.push_back(event);
            }
            return true;
        };
        if(!read_visual_events("Session.WeaponEmitters.",emitters,5,17,[](auto& e,const auto* tags){std::copy_n(tags,5,e.tags.begin());},ssne::validate_native_weapon_emitter) ||
           !read_visual_events("Session.WeaponCasings.",casings,4,19,[](auto& e,const auto* tags){e.slot=unsigned(tags[0]);e.weapon=unsigned(tags[1]);e.type=unsigned(tags[2]);e.mask=uint32_t(tags[3]);},ssne::validate_native_weapon_casing) ||
           !read_visual_events("Session.ProjectileVisuals.",projectile_visuals,4,30,[](auto& e,const auto* tags){std::copy_n(tags,4,e.tags.begin());},validate_native_projectile_visual,uint64_t(INT64_MAX)) ||
           !read_visual_events("Session.WeaponSurfaces.",surfaces,2,7,[](auto& e,const auto* tags){e.weapon=unsigned(tags[0]);e.kind=unsigned(tags[1]);},ssne::validate_native_weapon_surface) ||
           !read_visual_events("Session.WeaponBeams.",beams,4,13,[](auto& e,const auto* tags){e.slot=unsigned(tags[0]);e.weapon=unsigned(tags[1]);e.hand=unsigned(tags[2]);e.flags=unsigned(tags[3]);},ssne::validate_native_weapon_beam)){
            report("Invalid or incomplete native weapon visual event history");return;
        }
        if((replica_emitter_epoch_==state->epoch && !emitters.empty() && emitters.front().sequence>replica_emitter_sequence_+1) ||
           (replica_casing_epoch_==state->epoch && !casings.empty() && casings.front().sequence>replica_casing_sequence_+1) ||
           (replica_projectile_epoch_==state->epoch && !projectile_visuals.empty() && projectile_visuals.front().sequence>replica_projectile_sequence_+1) ||
           (replica_surface_epoch_==state->epoch && !surfaces.empty() && surfaces.front().sequence>replica_surface_sequence_+1) ||
           (replica_beam_epoch_==state->epoch && !beams.empty() && beams.front().sequence>replica_beam_sequence_+1)){
            report("Native weapon visual event history overrun");return;
        }
        std::vector<std::variant<ssne::NativeWeaponShot,ssne::NativeWeaponLoop>> sound_events;
        const bool new_sound_epoch=replica_loop_epoch_!=state->epoch;
        if(new_sound_epoch){for(const auto& loop:active_loops)sound_events.emplace_back(loop);}
        else for(const auto& loop:loops)if(loop.sequence>replica_loop_sequence_)sound_events.emplace_back(loop);
        if(replica_shot_epoch_==state->epoch)for(const auto& shot:shots)if(shot.sequence>replica_shot_sequence_)sound_events.emplace_back(shot);
        auto sound_order=[](const auto& event){return std::visit([](const auto& value){return value.order;},event);};
        std::sort(sound_events.begin(),sound_events.end(),[&](const auto& left,const auto& right){return sound_order(left)<sound_order(right);});
        if((new_sound_epoch?0:weapon_sounds_.size())+sound_events.size()>1024){report("Authoritative weapon sound queue overflow");return;}
        for(size_t i=0;i<sound_events.size();++i)if(sound_order(sound_events[i])<=(new_sound_epoch?0:weapon_sound_order_) || (i && sound_order(sound_events[i])<=sound_order(sound_events[i-1]))){report("Duplicate or reordered authoritative weapon sound event");return;}
        if(replica_music_epoch_!=state->epoch){replica_music_epoch_=state->epoch;replica_music_sequence_=0;replica_speech_sequence_=0;replica_speech_running_sequence_=0;}
        if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_REPLICA_INTERVAL_MS") && replica_shot_epoch_==state->epoch) {
            const auto interval=std::clamp(std::atoi(getenv("SSNE_COOP_TEST_REPLICA_INTERVAL_MS")),0,120000);const auto now=std::chrono::steady_clock::now();
            if(now<test_replica_next_)return;test_replica_next_=now+std::chrono::milliseconds(interval);
            if(log_)log_("[coop-test-replica-consumption] interval_ms="+std::to_string(interval)+" revision="+std::to_string(state->revision));
        }
        if(auto sequence=state->mission.find("Session.Speech.Sequence");sequence!=state->mission.end()){
            test_speech_waiting_=false;
            const auto id=uint64_t(std::get<int64_t>(sequence->second));
            const auto expected_track=uint32_t(std::get<int64_t>(state->mission.at("Session.Speech.Track")));
            const bool running=std::get<int64_t>(state->mission.at("Session.Speech.Phase"))==1;
            if(speech_phase_sequence_!=id || speech_phase_!=int(running)){
                speech_phase_sequence_=id;speech_phase_=int(running);
                if(log_)log_("[coop-speech-client-phase] sequence="+std::to_string(id)+" phase="+std::to_string(speech_phase_)+" revision="+std::to_string(state->revision));
            }
            if(replica_speech_running_sequence_==id && running && ssne::native_speech_playing_track()==expected_track)
                ssne::audio_reconcile_speech(id,uint64_t(std::get<int64_t>(state->mission.at("Session.Speech.OutputFrames"))));
            if(frame%30==0 && log_ && ssne::native_speech_playing_track()==expected_track)
                log_("[coop-speech-position-client] sequence="+std::to_string(id)+" host_frames="+std::to_string(std::get<int64_t>(state->mission.at("Session.Speech.OutputFrames")))+" local_frames="+std::to_string(ssne::audio_speech_output_frames(id))+" skipped_frames="+std::to_string(ssne::audio_speech_skipped_frames(id)));
            if(id>replica_speech_sequence_){
                const auto track=uint32_t(std::get<int64_t>(state->mission.at("Session.Speech.Track"))),reference=uint32_t(std::get<int64_t>(state->mission.at("Session.Speech.Reference")));
                const auto object=native_script_reference_address(reference);std::string why;
                bool source_valid=object && mem_read32(*object+0x48)==0x802087A8u;
                const auto config=source_valid?mem_read32(*object+0x54):0;
                source_valid=source_valid && config>=0x80000000u && config<=ssne::guest_ram_end()-0x24 && mem_read32(config+0x1C)==track;
                if(!source_valid)report("Authoritative speech source does not match local level");
                else if(ssne::stop_native_replica_speech(context,why) && ssne::prepare_native_speech(context,{id,track,reference},why))replica_speech_sequence_=id;
                else report(why);
            }
            if(running && replica_speech_sequence_==id && replica_speech_running_sequence_!=id){
                const auto reference=uint32_t(std::get<int64_t>(state->mission.at("Session.Speech.Reference")));
                if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_TEST_SPEECH_DELAY_MS") && reference==2809){
                    const auto delay=std::clamp(std::atoi(getenv("SSNE_COOP_TEST_SPEECH_DELAY_MS")),0,2000);
                    if(test_delayed_speech_!=id){test_delayed_speech_=id;test_speech_received_=std::chrono::steady_clock::now();if(log_)log_("[coop-test-speech-delay-start] sequence="+std::to_string(id)+" requested_ms="+std::to_string(delay)+" revision="+std::to_string(state->revision));}
                    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-test_speech_received_).count();
                    test_speech_waiting_=elapsed<delay;
                    if(!test_speech_waiting_ && log_)log_("[coop-test-speech-delay-end] sequence="+std::to_string(id)+" elapsed_ms="+std::to_string(elapsed));
                }
                if(!test_speech_waiting_){std::string why;if(ssne::start_prepared_native_speech(context,id,why))replica_speech_running_sequence_=id;else report(why);}
            }
        } else {test_speech_waiting_=false;std::string why;if(!ssne::stop_native_replica_speech(context,why))report(why);}
        if(auto sequence=state->mission.find("Session.Music.Sequence");sequence!=state->mission.end()){
            const auto id=uint64_t(std::get<int64_t>(sequence->second));
            if(id>replica_music_sequence_){
                ssne::NativeMusicRequest music{id,uint32_t(std::get<int64_t>(state->mission.at("Session.Music.Track"))),bool(std::get<int64_t>(state->mission.at("Session.Music.Force"))),std::get<float>(state->mission.at("Session.Music.Fade"))};
                std::string why;if(!ssne::apply_native_music_request(context,music,why)){report(why);return;}replica_music_sequence_=id;
            }
        }
        if(auto cutscene=state->mission.find("Session.Cutscene.Reference");cutscene!=state->mission.end()) {
            const auto object=native_script_reference_address(std::get<int64_t>(cutscene->second));
            const auto& values=std::get<std::vector<float>>(state->mission.at("Session.Cutscene.Camera"));
            std::array<float,14> camera;std::copy(values.begin(),values.end(),camera.begin());std::string why;
            if(!object || !ssne::present_native_cutscene(context,*object,camera,why)){report(object?why:"Authoritative cutscene entity missing in local level");return;}
            replica_cutscene_camera_=camera;replica_cutscene_id_=std::get<int64_t>(state->mission.at("Session.Cutscene.Id"));replica_cutscene_tick_=state->tick;
        } else {ssne::clear_native_cutscene_presentation();replica_cutscene_id_=0;}
        if(auto movie=state->mission.find("Session.Movie.Active");movie!=state->mission.end()){
            ssne::NativeMovieState desired;
            desired.active=std::get<int64_t>(movie->second)!=0;
            desired.sequence=uint64_t(std::get<int64_t>(state->mission.at("Session.Movie.Sequence")));
            desired.asset=std::get<std::string>(state->mission.at("Session.Movie.Asset"));
            desired.frame=uint32_t(std::get<int64_t>(state->mission.at("Session.Movie.Frame")));
            std::string why;if(!ssne::apply_native_movie(context,desired,why)){report(why);return;}
        }
        if(!fixture_dumped_ && env_requested("SSNE_COOP_CAPTURE_RAM") && frame>=600)capture_fixture(native_.capture(frame),context);
        std::set<unsigned> waiting_slots;
        if(auto pending=state->mission.find("Session.PendingSpawnSlots");pending!=state->mission.end())if(auto slots=std::get_if<std::vector<int64_t>>(&pending->second))for(auto slot:*slots)if(slot>=0 && slot<max_players)waiting_slots.insert(unsigned(slot));
        const auto local_players=native_.player_bindings();
        for(const auto& local:local_players) {
            const auto slot=local.slot;
            for(const auto& [id,e]:state->entities)if(e.kind==EntityKind::Player && std::get<int64_t>(e.properties.at("player_slot"))==slot) {
                if(!ssne::native_party_replica_spawn_state(context,local.actor,waiting_slots.contains(slot),std::get<std::array<float,3>>(e.properties.at("position")))){report("Replica spawn lifecycle incomplete");return;}break;
            }
        }
        const auto model_field=state->mission.find("Session.Geometry.Models");const auto* models=model_field==state->mission.end()?nullptr:std::get_if<std::vector<int64_t>>(&model_field->second);
        std::string model_error;if(!models || !validate_native_model_states(*models,model_error)){report(model_error.empty()?"Missing authoritative level model states":model_error);return;}
        auto result=[&]{NativeStageTimer timing("replica_apply",log_);return replica_.apply_with_vehicles(context,state->epoch,state->entities,state->mission);}();
        if(!result.applied){report(result.error);return;}
        if(mem_read32(context.r[13]-0x69d4)>=2 && mem_read32(context.r[13]-0x69d4)<=4){std::string why;if(!apply_versus(context,native_.capture_bindings(),state->mission,why)){report(why);return;}}
        if(!apply_native_model_states(context,*models,model_error)){report(model_error);return;}
        if(replica_feedback_epoch_!=state->epoch){replica_feedback_epoch_=state->epoch;replica_feedback_sequence_=journaled?event_tails[5]:feedback.empty()?0:feedback.back().sequence;}
        else for(const auto& event:feedback)if(event.sequence>replica_feedback_sequence_){std::string why;if(!ssne::play_native_feedback(context,event,why)){report(why);return;}replica_feedback_sequence_=event.sequence;}
        if(replica_trail_epoch_!=state->epoch){
            replica_trail_epoch_=state->epoch;replica_trail_sequence_=journaled?event_tails[6]:trails.empty()?0:trails.back().sequence;
            if(log_)log_("[coop-bullet-trail-baseline] epoch="+std::to_string(state->epoch)+" sequence="+std::to_string(replica_trail_sequence_));
        }else for(const auto& trail:trails)if(trail.sequence>replica_trail_sequence_){
            std::string why;if(!ssne::play_native_bullet_trail(context,trail,why)){report(why);return;}
            replica_trail_sequence_=trail.sequence;
        }
        auto play_visual_events=[&](const auto& events,unsigned channel,uint64_t& epoch,uint64_t& sequence,auto play){
            if(epoch!=state->epoch){epoch=state->epoch;sequence=journaled?event_tails[channel]:events.empty()?0:events.back().sequence;return true;}
            for(const auto& event:events)if(event.sequence>sequence){std::string why;if(!play(context,event,why)){report(why);return false;}sequence=event.sequence;}
            return true;
        };
        if(!play_visual_events(emitters,7,replica_emitter_epoch_,replica_emitter_sequence_,ssne::play_native_weapon_emitter) ||
           !play_visual_events(casings,8,replica_casing_epoch_,replica_casing_sequence_,ssne::play_native_weapon_casing) ||
           !play_visual_events(projectile_visuals,9,replica_projectile_epoch_,replica_projectile_sequence_,[&](PPCContext& c,const auto& event,std::string& why){return replica_.play_projectile_visual(c,state->epoch,event,frame,why);}) ||
           !play_visual_events(surfaces,10,replica_surface_epoch_,replica_surface_sequence_,ssne::play_native_weapon_surface) ||
           !play_visual_events(beams,11,replica_beam_epoch_,replica_beam_sequence_,ssne::play_native_weapon_beam))return;

        if(replica_hit_epoch_!=state->epoch){replica_hit_epoch_=state->epoch;replica_hit_sequence_=journaled?event_tails[0]:confirmed_hits.empty()?0:confirmed_hits.back().sequence;{std::lock_guard lock(combat_mutex_);combat_feedback_={};combat_flash_until_={};}if(log_)log_("[coop-confirmed-hit-baseline] epoch="+std::to_string(state->epoch)+" sequence="+std::to_string(replica_hit_sequence_)+" slot="+std::to_string(client_->status().player.slot)+" generation="+std::to_string(client_->status().player.generation));}else for(const auto& hit:confirmed_hits)if(hit.sequence>replica_hit_sequence_){accept_confirmed_hit(hit,client_->status().player);if(log_)log_("[coop-confirmed-hit-client] sequence="+std::to_string(hit.sequence)+" shot="+std::to_string(hit.shot)+" source="+std::to_string(hit.source)+" target="+std::to_string(hit.target)+" slot="+std::to_string(hit.slot)+" generation="+std::to_string(hit.generation)+" before="+std::to_string(hit.before)+" after="+std::to_string(hit.after)+" amount="+std::to_string(hit.amount)+" weapon="+std::to_string(hit.weapon)+" pellet="+std::to_string(hit.pellet)+" retired_target="+std::to_string(!state->entities.contains(hit.target)));replica_hit_sequence_=hit.sequence;}
        if(replica_impact_epoch_!=state->epoch){replica_impact_epoch_=state->epoch;replica_impact_sequence_=journaled?event_tails[4]:impacts.empty()?0:impacts.back().sequence;if(log_)log_("[coop-bullet-impact-baseline] epoch="+std::to_string(state->epoch)+" sequence="+std::to_string(replica_impact_sequence_));}else for(const auto& impact:impacts)if(impact.sequence>replica_impact_sequence_){std::string why;if(!ssne::play_native_bullet_impact(context,impact,why)){report(why);return;}replica_impact_sequence_=impact.sequence;}
        if(replica_loop_epoch_!=state->epoch){weapon_sounds_.clear();weapon_sound_order_=0;std::string why;if(!ssne::stop_native_weapon_loops(context,why)){report(why);return;}replica_loop_epoch_=state->epoch;replica_loop_sequence_=journaled?event_tails[3]:loops.empty()?0:loops.back().sequence;if(log_)log_("[coop-uzi-loop-baseline] epoch="+std::to_string(state->epoch)+" sequence="+std::to_string(replica_loop_sequence_));}
        if(replica_shot_epoch_!=state->epoch){replica_shot_epoch_=state->epoch;replica_shot_sequence_=journaled?event_tails[2]:shots.empty()?0:shots.back().sequence;if(log_)log_("[coop-weapon-shot-baseline] epoch="+std::to_string(state->epoch)+" sequence="+std::to_string(replica_shot_sequence_));}
        for(const auto& event:sound_events){weapon_sounds_.push_back({event,std::chrono::steady_clock::now()});weapon_sound_order_=sound_order(event);std::visit([&](const auto& value){using T=std::decay_t<decltype(value)>;if constexpr(std::is_same_v<T,ssne::NativeWeaponShot>)replica_shot_sequence_=std::max(replica_shot_sequence_,value.sequence);else replica_loop_sequence_=std::max(replica_loop_sequence_,value.sequence);},event);}
        if(replica_effect_epoch_!=state->epoch){replica_effect_epoch_=state->epoch;replica_effect_sequence_=journaled?event_tails[1]:ids.empty()?0:uint64_t(ids.back());if(log_)log_("[coop-rocket-effect-baseline] epoch="+std::to_string(state->epoch)+" sequence="+std::to_string(replica_effect_sequence_));}
        else {
            for(const auto& effect:effects)if(effect.sequence>replica_effect_sequence_){std::string why;if(!ssne::play_native_rocket_effect(context,effect,why)){report(why);return;}replica_effect_sequence_=effect.sequence;}
        }
        for(const auto& [id,entity]:state->entities)if(entity.kind==EntityKind::Player) {
            std::string why;if(!refresh_native_player_hud(context,replica_.local_address(id),why)){report(why);return;}
        }
        if((frame%60==0 || (env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES"))) && log_) {
            const auto reconciled=native_.capture(frame);
            std::map<uint32_t,const EntityState*> by_address;
            for(const auto& [id,e]:reconciled.entities)by_address[native_.local_address(id)]=&e;
            if(env_requested("SSNE_TEST_HIDDEN") && env_present("SSNE_COOP_CAMPAIGN_VERIFY")){
                unsigned fields=0,differences=0,enemies=0;
                for(const auto& [id,e]:state->entities)if(e.kind==EntityKind::Enemy || e.kind==EntityKind::Mission || e.kind==EntityKind::Switch || e.kind==EntityKind::Destructible || e.kind==EntityKind::Door){
                    enemies+=e.kind==EntityKind::Enemy;
                    const auto found=by_address.find(replica_.local_address(id));
                    for(const auto& [key,value]:e.properties){
                        if(key=="spawn_position")continue;++fields;
                        const bool mismatch=found==by_address.end() || !found->second->properties.contains(key) || found->second->properties.at(key)!=value;
                        if(mismatch){++differences;static std::set<std::pair<unsigned,std::string>> reported;if(reported.emplace(e.definition,key).second)log_("[campaign-field-difference] definition="+std::to_string(e.definition)+" key="+key);}
                    }
                }
                log_("[campaign-verify] level="+state->level+" epoch="+std::to_string(state->epoch)+" revision="+std::to_string(state->revision)+" enemies="+std::to_string(enemies)+" fields="+std::to_string(fields)+" differences="+std::to_string(differences));
            }
            unsigned render_objects=0,render_differences=0,marker_objects=0,marker_differences=0;
            for(const auto& [id,e]:state->entities){
                const auto found=by_address.find(replica_.local_address(id));
                if(e.properties.contains("render_visible"))++render_objects;
                if(e.properties.contains("marker_active"))++marker_objects;
                for(const auto& [key,value]:e.properties)if(key.starts_with("render_") || key=="marker_active" || key.starts_with("body") || key.starts_with("weapon_model") || key.starts_with("enemy_bone_") || key=="pickup_view_mask" || key=="pickup_glow" || key=="powerups"){
                    const bool differs=found==by_address.end() || !found->second->properties.contains(key) || found->second->properties.at(key)!=value;
                    if(differs && env_requested("SSNE_TEST_HIDDEN"))log_("[coop-presentation-difference] id="+std::to_string(id)+" key="+key);
                    if(key=="marker_active")marker_differences+=differs;else render_differences+=differs;
                }
            }
            log_("[coop-client-presentation] revision="+std::to_string(state->revision)+" render_objects="+std::to_string(render_objects)+" render_differences="+std::to_string(render_differences)+" markers="+std::to_string(marker_objects)+" marker_differences="+std::to_string(marker_differences));
            unsigned inventory_players=0,inventory_differences=0,pose_differences=0,name_options=0,name_objects=0;
            for(const auto& [id,e]:state->entities)if(e.kind==EntityKind::Player) {
                ++inventory_players;const auto found=by_address.find(replica_.local_address(id));
                if(found==by_address.end()){++inventory_differences;++pose_differences;continue;}
                const auto actor=replica_.local_address(id),name_option=mem_read32(actor+0x278);
                name_objects+=mem_read32(actor+0x160)!=0;
                name_options+=name_option>=0x80000000u && name_option<=ssne::guest_ram_end()-0x20u?mem_read32(name_option+0x1C)!=0:1;
                for(const auto* key:{"position","yaw","pitch","view_angles","movement_mode"})if(auto expected=e.properties.find(key);expected!=e.properties.end()){
                    const auto actual=found->second->properties.find(key);pose_differences+=actual==found->second->properties.end() || actual->second!=expected->second;
                }
                for(const auto* key:{"health","armor","score","mode_score","weapon_owned_counts","weapon_active_counts","ammo","ammo_secondary","ammo_maximum","selected_weapon"})if(auto expected=e.properties.find(key);expected!=e.properties.end()) {
                    const auto actual=found->second->properties.find(key);const bool differs=actual==found->second->properties.end() || actual->second!=expected->second;inventory_differences+=differs;
                    if(differs && env_requested("SSNE_TEST_HIDDEN")){
                        static std::set<std::pair<unsigned,std::string>> reported;
                        if(reported.emplace(e.owner.slot,key).second)log_("[coop-client-inventory-field] epoch="+std::to_string(state->epoch)+" slot="+std::to_string(e.owner.slot)+" field="+key);
                    }
                }
            }
            log_("[coop-client-inventory] epoch="+std::to_string(state->epoch)+" players="+std::to_string(inventory_players)+" differences="+std::to_string(inventory_differences));
            log_("[coop-client-player-pose] epoch="+std::to_string(state->epoch)+" revision="+std::to_string(state->revision)+" players="+std::to_string(inventory_players)+" differences="+std::to_string(pose_differences));
            log_("[coop-client-name-policy] players="+std::to_string(inventory_players)+" enabled="+std::to_string(name_options)+" objects="+std::to_string(name_objects));
            unsigned hud_differences=0,armor_hud_differences=0,armor_hud_positive=0,score_hud_differences=0;
            for(const auto& [id,e]:state->entities)if(e.kind==EntityKind::Player) {
                const auto actor=replica_.local_address(id),hud=mem_read32(actor+0x374),number=mem_read32(hud+0x88);
                std::string text;for(unsigned i=0;i<128 && mem_read8(number+0x1C+i);++i)text+=char(mem_read8(number+0x1C+i));
                hud_differences+=text!=std::to_string(int32_t(std::get<float>(e.properties.at("health"))));
                if(mem_read32(hud+0x20)!=1 && mem_read32(hud+0x20)!=3){
                    const auto key=mem_read32(context.r[13]-0x69D4)==3?"mode_score":"score";
                    if(auto score=e.properties.find(key);score!=e.properties.end()){
                        const auto number=mem_read32(hud+0xA4);std::string value;for(unsigned i=0;i<128 && mem_read8(number+0x1C+i);++i)value+=char(mem_read8(number+0x1C+i));
                        score_hud_differences+=value!=std::to_string(int32_t(std::get<float>(score->second)));
                    }
                }
                if(auto armor=e.properties.find("armor");armor!=e.properties.end() && std::get<float>(armor->second)>=1.f){
                    ++armor_hud_positive;const auto armor_number=mem_read32(hud+0x94);std::string armor_text;
                    for(unsigned i=0;i<128 && mem_read8(armor_number+0x1C+i);++i)armor_text+=char(mem_read8(armor_number+0x1C+i));
                    armor_hud_differences+=armor_text!=std::to_string(int32_t(std::get<float>(armor->second)));
                }
            }
            log_("[coop-client-health-hud] players="+std::to_string(inventory_players)+" differences="+std::to_string(hud_differences));
            log_("[coop-client-score-hud] players="+std::to_string(inventory_players)+" differences="+std::to_string(score_hud_differences));
            log_("[coop-client-armor-hud] positive="+std::to_string(armor_hud_positive)+" differences="+std::to_string(armor_hud_differences));

            unsigned script_count=0,script_differences=0,disabled_triggers=0,disabled_switches=0;
            for(const auto& [id,e]:state->entities)if(e.kind==EntityKind::Mission || e.kind==EntityKind::Switch) {
                ++script_count;
                if(e.kind==EntityKind::Mission)disabled_triggers+=!std::get<int64_t>(e.properties.at("trigger_enabled"));else disabled_switches+=!std::get<int64_t>(e.properties.at("switch_enabled"));
                const auto address=replica_.local_address(id);const EntityState* local=nullptr;
                if(auto found=by_address.find(address);found!=by_address.end())local=found->second;
                if(!local){++script_differences;continue;}
                for(const auto& [key,value]:e.properties)if(key.starts_with("script_") || key.starts_with("trigger_") || key.starts_with("switch_")) {
                    auto found=local->properties.find(key);script_differences+=found==local->properties.end() || found->second!=value;
                }
            }
            log_("[coop-client-scripts] count="+std::to_string(script_count)+" disabled_triggers="+std::to_string(disabled_triggers)+" disabled_switches="+std::to_string(disabled_switches)+" differences="+std::to_string(script_differences));
            unsigned brushes=0,differences=0,active=0;
            for(const auto& [id,e]:state->entities)if(e.kind==EntityKind::Door) {
                ++brushes;const auto object=replica_.local_address(id);
                const auto level=mem_read32(0x802DACC0),manager=mem_read32(level+0x298);
                const auto entry=mem_read32(manager+0x10)+unsigned(std::get<int64_t>(e.properties.at("brush_resource")))*80;
                differences+=mem_read32(object+0xC8)!=std::get<int64_t>(e.properties.at("brush_segment")) || mem_read32(entry+0xC)!=mem_read32(object+0xC8) || u2f(mem_read32(entry+0x10))!=std::get<float>(e.properties.at("brush_progress"));
                active+=std::get<int64_t>(e.properties.at("brush_active"))!=0;
                if(std::get<int64_t>(e.properties.at("brush_active")))log_("[coop-client-brush-state] resource="+std::to_string(std::get<int64_t>(e.properties.at("brush_resource")))+" segment="+std::to_string(mem_read32(entry+0xC))+" progress="+std::to_string(u2f(mem_read32(entry+0x10))));
            }
            log_("[coop-client-brush] count="+std::to_string(brushes)+" active="+std::to_string(active)+" differences="+std::to_string(differences));
        }
        enemy_pose_expected_.clear();
        for(const auto& [id,e]:state->entities)if(auto pose=e.properties.find("enemy_bone_matrices");pose!=e.properties.end())enemy_pose_expected_[id]=std::get<std::vector<float>>(pose->second);
        if(!last_revision_ && log_)log_("[coop-client-first-apply] revision="+std::to_string(state->revision)+" objects="+std::to_string(result.objects));
        last_revision_=state->revision;
        presentation_applied_=state;
        bool cut=false;const auto presentation_owner=client_->status().player;
        for(const auto& sample:client_->motion_samples(presentation_revision_,state->revision)){cut|=presentation_.push(sample.world,sample.received,presentation_owner);if(ssne::perf::enabled.load(std::memory_order_relaxed))ssne::perf::snapshot_received.store(sample.received,std::memory_order_relaxed);}
        presentation_revision_=state->revision;
        if(cut){client_->clear_predicted_look();++presentation_generation_;}
        if(journaled)client_->acknowledge_native_events(state->epoch,{replica_hit_sequence_,replica_effect_sequence_,replica_shot_sequence_,replica_loop_sequence_,replica_impact_sequence_,replica_feedback_sequence_,replica_trail_sequence_,replica_emitter_sequence_,replica_casing_sequence_,replica_projectile_sequence_,replica_surface_sequence_,replica_beam_sequence_});
        if(test_speech_waiting_ && log_)log_("[coop-test-speech-delay-world-applied] sequence="+std::to_string(test_delayed_speech_)+" revision="+std::to_string(last_revision_)+" objects="+std::to_string(result.objects));
        if(!fixture_dumped_ && env_requested("SSNE_COOP_CAPTURE_RAM") && frame>=600)capture_fixture(native_.capture(frame),context);
        if(epoch_!=state->epoch){epoch_=state->epoch;client_ready_=false;}
        const auto owner=client_->status().player;
        if(owner.slot<max_players) {
            const auto& player=state->players[owner.slot];
            if(ready_retry_epoch_!=state->epoch || ready_retry_owner_!=owner) {
                ready_retry_epoch_=state->epoch;ready_retry_owner_=owner;ready_retry_next_={};
            }
            const bool previously_ready=client_ready_;
            client_ready_=player.id==owner && player.ready;
            if(client_ready_ && !previously_ready && log_)
                log_("[coop-client-ready-confirmed] slot="+std::to_string(owner.slot)+" generation="+std::to_string(owner.generation)+" epoch="+std::to_string(state->epoch)+" revision="+std::to_string(state->revision)+" frame="+std::to_string(frame));
            // Snapshot consumption need not coincide with a particular render
            // frame. Modulo-frame retry can starve a slow client indefinitely.
            const auto now=std::chrono::steady_clock::now();
            const auto character=state->entities.find(player.character);
            if(!client_ready_ && player.id==owner && player.character &&
               character!=state->entities.end() && character->second.owner==owner &&
               now>=ready_retry_next_) {
                client_->ready(state->epoch);ready_retry_next_=now+std::chrono::milliseconds(500);
                if(log_)log_("[coop-client-ready-request] slot="+std::to_string(owner.slot)+" generation="+std::to_string(owner.generation)+" epoch="+std::to_string(state->epoch)+" revision="+std::to_string(state->revision)+" frame="+std::to_string(frame));
            }
        }
        if(frame%30==0 && log_ && owner.slot<max_players && state->players[owner.slot].character) {
            const auto character=state->players[owner.slot].character;
            if(auto entity=state->entities.find(character);entity!=state->entities.end()) {
                log_("[coop-client-weapon] slot="+std::to_string(owner.slot)+" authoritative="+std::to_string(std::get<int64_t>(entity->second.properties.at("selected_weapon")))+" local="+std::to_string(mem_read32(replica_.local_address(character)+0x5DC)));
                unsigned camera_mismatches=0;
                const auto camera=mem_read32(replica_.local_address(character)+0x368);
                for(unsigned row=0;row<3;++row)if(auto value=entity->second.properties.find("camera_row"+std::to_string(row));value!=entity->second.properties.end()) {
                    const auto& matrix=std::get<std::array<float,4>>(value->second);
                    for(unsigned column=0;column<4;++column)camera_mismatches+=mem_read32(camera+0x24+row*16+column*4)!=f2u(matrix[column]);
                }
                if(entity->second.properties.contains("camera_row2") && entity->second.properties.contains("view_angles")){
                    const auto& forward=std::get<std::array<float,4>>(entity->second.properties.at("camera_row2"));
                    const auto& angles=std::get<std::array<float,3>>(entity->second.properties.at("view_angles"));
                    log_("[coop-client-camera] slot="+std::to_string(owner.slot)+" mismatched_values="+std::to_string(camera_mismatches)+" forward="+std::to_string(forward[0])+","+std::to_string(forward[1])+","+std::to_string(forward[2])+" pitch="+std::to_string(std::get<float>(entity->second.properties.at("pitch")))+" view_pitch="+std::to_string(angles[1]));
                }else log_("[coop-client-camera-pending] slot="+std::to_string(owner.slot)+" epoch="+std::to_string(state->epoch));
            }
        }
        if((frame%30==0 || (env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES"))) && log_){auto skipped=ssne::replica_simulation_stats();log_("[coop-client-frame] frame="+std::to_string(frame)+" host_frame="+std::to_string(state->tick)+" revision="+std::to_string(last_revision_)+" objects="+std::to_string(result.objects)+" mode="+std::to_string(mem_read32(ssne::game_address(0x802DB1BC)))+" replica="+std::to_string(ssne::replica_simulation())+" skipped_game="+std::to_string(skipped.game_updates)+" skipped_world="+std::to_string(skipped.world_updates));}
    }
}
}
