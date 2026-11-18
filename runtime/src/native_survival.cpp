#include "ssne/native_survival.h"
#include "ssne/native_party.h"
#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
namespace ssne {namespace {
constexpr uint32_t preload_address=0x8009786Cu,resource_flags=0x80277A34u;
constexpr std::array<unsigned,10> roster{0,12,18,13,14,5,7,10,8,11};
std::atomic<bool> enabled{false},prepared{false};std::atomic<uint64_t> resource_revision{0};
bool project_resources=false;
bool installed=false;PPCFunc preload_previous=nullptr,level_release_previous=nullptr,shutdown_previous=nullptr;std::string resource_error;
thread_local bool preparing=false;
bool range(uint32_t p,uint64_t n){return p>=0x80000000u && n<=guest_ram_size() && uint64_t(p)+n<=guest_ram_end();}
bool point_valid(const NativeSurvivalPoint& point){return std::all_of(point.begin(),point.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1000000.f;});}
bool pal(){return mem_read32(0x80000000u)==0x47334250u;}
bool fingerprint(uint32_t a,uint32_t n,uint64_t wanted){if(!range(a,n))return false;uint64_t h=14695981039346656037ull;for(uint32_t i=0;i<n;++i){h^=mem_read8(a+i);h*=1099511628211ull;}return h==wanted;}

// The original 8009786C path prepares voices in StreamData.dat. Enemy models
// live in an embedded SSG group inside the user's campaign SSW, not loose SSM
// files. Import the group through the original resource APIs; never construct
// the campaign level or run its actors, scripts, player starts or progress.
struct TextureLease {uint32_t node=0,instance=0;};
struct ModelPack {uint32_t owner=0,group=0,bank=0,engine=0,level=0;std::vector<TextureLease> textures;} models;
constexpr uint32_t resource_pack_bytes=11995808u;
constexpr std::array<const char*,10> model_names{"KamikazeMarines","Grunt","KleerKnight","Firecracker_Grunt","Grenade_Grunt","Werebull","Scorpions","mechA","demonA","mechB"};
struct ResourceStack {
    uint32_t start;std::array<uint8_t,0x4000> bytes;
    explicit ResourceStack(uint32_t sp):start(sp-0x4000u){std::memcpy(bytes.data(),host_ptr(start),bytes.size());}
    ~ResourceStack(){std::memcpy(host_ptr(start),bytes.data(),bytes.size());}
};
bool resource_context(const PPCContext& c){return pal() && c.r[13]==0x802E22A0u && c.r[1]>=0x80004000u && range(c.r[1]-0x4000,0x4000);}
// Blocking original DVD/import work needs its entry interrupt state. The
// runtime dispatches IRQs on its separate IRQ CPU/stack and resumes this private
// call on the same host stack; preserving EE does not leak caller registers.
PPCContext resource_call(const PPCContext& c){auto call=c;call.r[1]-=0x400;mem_write32(call.r[1],c.r[1]);return call;}
bool engine_valid(uint32_t engine){return range(engine,0xD4) && mem_read32(engine)==0x8021C600u;}
bool bank_registered(uint32_t engine,uint32_t bank){if(!engine_valid(engine))return false;for(unsigned i=0;i<10;++i)if(mem_read32(engine+0x8C+i*4)==bank)return true;return false;}
bool pack_range(uint32_t owner,uint32_t p,uint32_t n){return p>=owner && uint64_t(p)+n<=uint64_t(owner)+resource_pack_bytes && range(p,n);}
bool bank_valid(const ModelPack& p){return p.owner && range(p.owner,resource_pack_bytes) && pack_range(p.owner,p.group,0x20) && range(p.bank,0x18) && mem_read32(p.bank)==0x8021DA98u && mem_read32(p.bank+4)==p.group && mem_read32(p.bank+8)==p.engine && mem_read32(p.bank+0xC)==p.engine+4 && mem_read32(p.bank+0x10)==1 && bank_registered(p.engine,p.bank);}
bool native_model(uint32_t model){return range(model,0x34) && mem_read32(model)==0x8021DC50u && range(mem_read32(model+0xC),0x50);}
bool models_valid(uint32_t level){if(!range(level,0x298))return false;for(unsigned type:roster)if(!native_model(mem_read32(level+4+type*4)))return false;return true;}
bool resource_heap(uint32_t sda,std::string& error){
    const auto handle=int32_t(mem_read32(sda-0x6DE0));const auto count=mem_read32(sda-0x5CB8),table=mem_read32(sda-0x5CBC);
    if(handle<0 || count>64 || unsigned(handle)>=count || !range(table,uint64_t(count)*24)){error="Survival native resource heap is unavailable";return false;}
    const auto entry=table+unsigned(handle)*24,total=mem_read32(entry);uint64_t available=0;uint32_t largest=0,second_largest=0;std::set<uint32_t> visited;
    for(auto block=mem_read32(entry+4);block;block=mem_read32(block+4)){
        if(!range(block,32) || block%32 || !visited.insert(block).second || visited.size()>4096){error="Survival resource heap free list is invalid";return false;}
        const auto size=mem_read32(block+8);if(size<32 || size%32 || !range(block,size) || size>total){error="Survival resource heap block is invalid";return false;}
        if(size>=largest){second_largest=largest;largest=size;}
        else second_largest=std::max(second_largest,size);
        available+=size;if(available>total){error="Survival resource heap accounting is invalid";return false;}
    }
    // PAL 80116084 uses 32-byte alignment and a 32-byte block header.
    constexpr uint32_t owner_bytes=(resource_pack_bytes+63u)&~31u;
    // The original importer streams through 128 KiB. Its optional separate
    // relocation table (0xFC46 entries), inflate state and block headers fit
    // alongside that buffer in 512 KiB. Models reference the retained owner.
    constexpr uint32_t import_workspace=512u*1024;
    constexpr uint32_t scene_and_actor_reserve=4u*1024*1024;
    const bool owner_fits=largest>=owner_bytes;
    // Best-fit can leave the workspace in the owner's tail or another block.
    const bool workspace_fits=largest>=owner_bytes+import_workspace ||
                              second_largest>=import_workspace;
    log_message("[survival-native-heap] largest="+std::to_string(largest)+" free="+std::to_string(available));
    if(!owner_fits || !workspace_fits || available<owner_bytes+import_workspace+scene_and_actor_reserve){
        error="Insufficient native heap for the Survival models, import workspace and live players";
        return false;
    }
    return true;
}
bool packed_roster(uint32_t owner,uint32_t group,std::string& error){
    if(!range(owner-32,resource_pack_bytes+32) || mem_read32(owner)!=1 || mem_read32(owner+4)!=100 || mem_read32(owner-0x18)<resource_pack_bytes+32 || mem_read32(owner-0x18)>resource_pack_bytes+128 || !pack_range(owner,group,0x20) || mem_read32(group)!=1 || mem_read32(group+4)!=100){error="Survival campaign resource group has an unsupported original format or extent";return false;}
    uint16_t found=0;std::set<uint32_t> visited;
    for(auto node=mem_read32(group+8);node;node=mem_read32(node)){
        if(!pack_range(owner,node,0x20) || !visited.insert(node).second || visited.size()>512){error="Survival campaign model list is invalid";return false;}
        const auto text=mem_read32(node+4);std::string name;
        for(unsigned i=0;i<96;++i){if(!pack_range(owner,text+i,1)){error="Survival packed model name is unavailable";return false;}const auto ch=mem_read8(text+i);if(!ch)break;if(ch<32 || ch>126 || i==95){error="Survival packed model name is invalid";return false;}name+=char(ch>='A' && ch<='Z'?ch+'a'-'A':ch);}
        if(mem_read32(node+8))continue;
        if(!pack_range(owner,mem_read32(node+0xC),0x50)){error="Survival packed model descriptor is unavailable";return false;}
        for(unsigned i=0;i<model_names.size();++i){std::string wanted=model_names[i];for(auto& ch:wanted)if(ch>='A' && ch<='Z')ch=char(ch+'a'-'A');if(name==wanted)found|=uint16_t(1u<<i);}
    }
    if(found!=uint16_t((1u<<model_names.size())-1)){error="Original campaign resource group does not contain the complete Survival roster";return false;}
    return true;
}
bool native_texture(uint32_t texture){return range(texture,0x64) && mem_read32(texture)==0x8021E1D0u && mem_read32(texture+0x60)>0 && mem_read32(texture+0x60)<1000000;}
bool collect_main_textures(uint32_t owner,std::vector<uint32_t>& nodes,std::string& error){
    std::set<uint32_t> visited;
    for(auto node=mem_read32(owner+0xC);node;node=mem_read32(node)){
        if(!pack_range(owner,node,0x20) || !visited.insert(node).second || visited.size()>512){error="Survival original main texture list is invalid";return false;}
        const auto width=mem_read32(node+8),height=mem_read32(node+0xC);
        if(mem_read32(node+4)>4 || !width || !height || width>2048 || height>2048){error="Survival original texture format or dimensions are unsupported";return false;}
        nodes.push_back(node);
    }
    if(nodes.empty()){error="Survival original main texture list is missing";return false;}return true;
}
bool prepare_models(PPCContext& c,std::string& error){
    if(!resource_context(c)){error="Survival original resource context is unavailable";return false;}
    if(!(c.msr&0x8000u)){error="Survival original resource import requires an interrupt-enabled load context";return false;}
    const auto level=mem_read32(0x802DACC0),engine=mem_read32(0x802ABAA0);
    if(!range(level,0x298) || !engine_valid(engine)){error="Survival original level or resource engine is unavailable";return false;}
    if(models.owner){if(models.level==level && bank_valid(models) && models_valid(level))return true;error="Previous Survival model ownership has not finished native cleanup";return false;}
    bool free_slot=false;for(unsigned i=0;i<10;++i)free_slot|=!mem_read32(engine+0x8C+i*4);
    if(!free_slot){error="Native resource engine has no free Survival bank slot";return false;}
    if(!resource_heap(c.r[13],error))return false;
    ResourceStack scratch(c.r[1]);auto call=resource_call(c);const auto path=c.r[1]-0x200;
    const char* donor=project_resources?"EditorSurvivalResources.ssw":"Levels/Clevel5_4.ssw";for(unsigned i=0;i<=std::strlen(donor);++i)mem_write8(path+i,uint8_t(donor[i]));
    log_message("[survival-native-import-context] msr="+std::to_string(call.msr)+" stack="+std::to_string(call.r[1]));
    call.r[3]=path;call.r[4]=1;ssne_call(call,0x80120F9Cu);const auto owner=call.r[3];
    if(!range(owner,0x84)){error="The original campaign resource pack could not be imported";return false;}
    const auto group=mem_read32(owner+0x7C);
    if(!packed_roster(owner,group,error)){call=resource_call(c);call.r[3]=owner;ssne_call(call,0x80120EF0u);return false;}
    std::vector<uint32_t> texture_nodes;
    if(!collect_main_textures(owner,texture_nodes,error)){call=resource_call(c);call.r[3]=owner;ssne_call(call,0x80120EF0u);return false;}
    models={owner,group,0,engine,level,{}};
    // Exact texture-only work from original PAL80134754. These descriptors are
    // shared by SSW models but live on the outer root+0C list, not SSG+18.
    // Serialized +1C holds a checksum, which must never be treated as an object.
    // No CcWorld, campaign geometry, actors or scripts are constructed here.
    for(const auto node:texture_nodes){
        mem_write32(node+0x1C,0);call=resource_call(c);call.r[3]=node;call.r[4]=engine;ssne_call(call,0x80131C6Cu);
        const auto instance=mem_read32(node+0x1C);
        if(!native_texture(instance)){error="Original Survival texture initialization failed";return false;}
        models.textures.push_back({node,instance});
    }
    log_message("[survival-native-textures] original outer SSW textures initialized="+std::to_string(models.textures.size()));
    call=resource_call(c);call.r[3]=group;call.r[4]=engine+4;call.r[5]=engine;call.r[6]=1;ssne_call(call,0x8012D8A0u);
    models.bank=call.r[3];
    if(!bank_valid(models)){error="Original campaign resource bank could not be registered";return false;}
    for(unsigned type:roster){call=resource_call(c);call.r[3]=level;call.r[4]=type;ssne_call(call,0x8010B9A0u);if(!native_model(mem_read32(level+4+type*4))){error="Original Survival model load failed for definition "+std::to_string(type);return false;}}
    log_message("[survival-native-models] original campaign embedded SSG mounted; ten original enemy models verified; no campaign world constructed");return true;
}
void release_models(PPCContext& c){
    if(!models.owner)return;prepared=false;
    if(!resource_context(c) || (models.bank && (!bank_valid(models) || mem_read32(models.bank+0x14)!=1))){log_message("[survival-native-models] retained pack until original model references are released");return;}
    // Texture/effect holders retain texture instances independently of the
    // bank. Require only our initial references before freeing any backing.
    for(const auto& texture:models.textures)if(!pack_range(models.owner,texture.node,0x20) || mem_read32(texture.node+0x1C)!=texture.instance || !native_texture(texture.instance) || mem_read32(texture.instance+0x60)!=1){log_message("[survival-native-models] retained pack until original texture references are released");return;}
    ResourceStack scratch(c.r[1]);auto call=resource_call(c);
    if(models.bank){call.r[3]=models.bank;ssne_call(call,0x8012D6A8u);
        if(bank_registered(models.engine,models.bank)){log_message("[survival-native-models] original bank cleanup is pending; source pack retained");return;}models.bank=0;
    }
    // Exact release counterpart of original PAL801347B0, after all models.
    for(const auto& texture:models.textures){call=resource_call(c);call.r[3]=texture.instance;ssne_call(call,0x8013196Cu);}
    models.textures.clear();call=resource_call(c);call.r[3]=models.owner;ssne_call(call,0x80120EF0u);models={};log_message("[survival-native-models] original bank, main textures and owning import released after native teardown");
}
void level_release(PPCContext& c){const auto level=c.r[3];const auto entry=c;ssne_call_original(c,0x8010AC40u,level_release_previous);if(models.owner && models.level==level){auto cleanup=entry;release_models(cleanup);}}
void shutdown(PPCContext& c){const auto entry=c;ssne_call_original(c,0x80166480u,shutdown_previous);if(models.owner){auto cleanup=entry;release_models(cleanup);}}

void preload(PPCContext& c){
    const auto entry=c;
    const bool use=enabled.load(std::memory_order_acquire) && !preparing && pal() && c.r[13]==0x802E22A0u && range(resource_flags,60*4);
    struct Guard {bool before;explicit Guard(bool use):before(preparing){if(use)preparing=true;}~Guard(){preparing=before;}} guard(use);
    if(use){prepared=false;for(unsigned definition:roster)mem_write32(resource_flags+definition*4,1);}
    ssne_call_original(c,preload_address,preload_previous);
    if(use){auto load=entry;std::string error;const bool ready=prepare_models(load,error);resource_error=ready?std::string{}:error;prepared.store(ready,std::memory_order_release);if(ready)resource_revision.fetch_add(1,std::memory_order_release);log_message(ready?"[survival-native-resources] original audio and ten native enemy models are ready":"[survival-native-resources] Survival unavailable: "+error);}
}
float distance(const NativeSurvivalPoint& a,const NativeSurvivalPoint& b){return std::hypot(std::hypot(a[0]-b[0],a[2]-b[2]),a[1]-b[1]);}
bool anchor(uint32_t node,NativeSurvivalPoint& p){
    if(!range(node,0x88))return false;
    for(unsigned i=0;i<12;++i){const float v=u2f(mem_read32(node+0x54+i*4));if(!std::isfinite(v) || std::abs(v)>1000000.f)return false;}
    for(unsigned i=0;i<3;++i)p[i]=u2f(mem_read32(node+0x60+i*16));return point_valid(p);
}
}
void configure_native_survival_project_resources(){project_resources=true;}
void configure_native_survival(bool value){const bool old=enabled.exchange(value,std::memory_order_acq_rel);if(!value || !old)prepared.store(false,std::memory_order_release);}
bool native_survival_enabled(){return enabled.load(std::memory_order_acquire);}
bool native_survival_available(){return installed && pal();}
bool native_survival_resources_ready(){return native_survival_available() && enabled.load(std::memory_order_acquire) && prepared.load(std::memory_order_acquire) && bank_valid(models) && mem_read32(0x802DACC0u)==models.level && models_valid(models.level);}
std::string native_survival_resource_error(){return resource_error.empty() && native_survival_enabled() && !native_survival_resources_ready()?"Survival native model resources are not ready":resource_error;}
uint64_t native_survival_resource_revision(){return resource_revision.load(std::memory_order_acquire);}
void install_native_survival(){
    if(installed || !pal())return;
    const auto* entry=ssne_find_func(preload_address);
    if(!entry || !entry->func || !ssne_find_func(0x8010B514u) ||
       !fingerprint(preload_address,0x168,0x42B94ACA0D798CA6ull) || !fingerprint(0x80097854u,0x18,0xCBF7242F71F3234Full) ||
       !fingerprint(0x80120F9Cu,0x20,0x5A2FA0FF3C29BBEAull) || !fingerprint(0x80120EF0u,0xAC,0x684B3999C9E20338ull) ||
       !fingerprint(0x8012D8A0u,0x94,0x40AEE1C2296DF5D7ull) || !fingerprint(0x8012D544u,0xA4,0x243B53BB742AA162ull) ||
       !fingerprint(0x80118C64u,0x34,0x669BD134A1A8A3A1ull) || !fingerprint(0x80118C98u,0x38,0x7B88BFFF579141CDull) ||
       !fingerprint(0x8012D6A8u,0x58,0x844966D699F810D6ull) || !fingerprint(0x8012D710u,0x5C,0xDD652AF65CE023A2ull) ||
       !fingerprint(0x80134754u,0x5C,0x17222951F92EA9D1ull) || !fingerprint(0x801347B0u,0x60,0xFA1D444EFE6EAFEBull) ||
       !fingerprint(0x80131C6Cu,0x320,0x3106241ECCB5D108ull) || !fingerprint(0x80131740u,0x98,0xDCE75446A90583CAull) ||
       !fingerprint(0x801317D8u,0x190,0x50207F4825434236ull) || !fingerprint(0x8013196Cu,0x84,0x6010086737D6E703ull) ||
       !fingerprint(0x801319F0u,0x58,0xD8A7137AD76C270Full) || !fingerprint(0x8010B9A0u,0x840,0xEB31F1A98D388E23ull) || !fingerprint(0x8010AC40u,0x204,0x25CF3E85A656CC51ull) ||
       !fingerprint(0x80166480u,0x2A4,0x9505A42F32AAF915ull) || !fingerprint(0x8010B0ACu,0x468,0xB8CD23C5139E9F19ull) || !fingerprint(0x8010B514u,0x78,0x8C6682061792AB6Aull)){
        resource_error="Original PAL Survival model-resource revision is unavailable";log_message("[survival-native-resources] "+resource_error);return;
    }
    const auto previous=*ssne_hook_slot(preload_address);preload_previous=previous?previous:entry->func;
    const auto level_previous=*ssne_hook_slot(0x8010AC40u),scene_previous=*ssne_hook_slot(0x80166480u);
    const auto* level_entry=ssne_find_func(0x8010AC40u);const auto* scene_entry=ssne_find_func(0x80166480u);
    if(!level_entry || !scene_entry)return;
    level_release_previous=level_previous?level_previous:level_entry->func;shutdown_previous=scene_previous?scene_previous:scene_entry->func;
    ssne_install_hook(preload_address,preload);ssne_install_hook(0x8010AC40u,level_release);ssne_install_hook(0x80166480u,shutdown);installed=true;log_message("[survival-native-resources] guarded original PAL preload installed");
}
bool prepare_native_survival_player_start(PPCContext& c,uint32_t actor,std::string& error){
    error.clear();
    if(!native_survival_enabled() || mem_read32(0x802DB8CCu)!=1)return true;
    if(!native_survival_available() || c.r[13]!=0x802E22A0u || c.r[1]<0x80000800u || !range(c.r[1]-0x800,0x800) || !native_player_collision_radius(actor)){
        error="Survival native cooperative player-start context unavailable";return false;
    }
    const auto level=mem_read32(0x802DACC0u);if(!range(level,0x2DC)){error="Survival player-start level unavailable";return false;}
    auto checked=[&](uint32_t node){NativeSurvivalPoint p;return anchor(node,p) && range(mem_read32(node+0x84),8);};
    const auto primary=mem_read32(level+0x2A8);
    if(primary){if(checked(primary))return true;error="Survival primary-start descriptor is invalid";return false;}
    const auto count=mem_read32(level+0x2D8);if(!count || count>10){error="Survival map has no bounded multiplayer spawn list";return false;}
    uint32_t selected=0;NativeSurvivalPoint selected_point{};float lowest=std::numeric_limits<float>::max();
    const auto player_radius=*native_player_collision_radius(actor);
    for(unsigned i=0;i<count;++i){
        const auto node=mem_read32(level+0x2B0+i*4);NativeSurvivalPoint p;
        if(!checked(node) || !anchor(node,p) || p[1]>=lowest)continue;
        auto above=p,below=p;above[1]+=4.f;below[1]-=8.f;std::optional<NativeSegmentHit> floor;std::string why;
        if(!native_world_segment(c,above,below,floor,why)){error="Survival primary floor query failed: "+why;return false;}
        if(!floor || floor->normal[1]<.725f || std::abs(floor->position[1]-p[1])>2.f)continue;
        auto centre=p;centre[1]=floor->position[1]+player_radius/floor->normal[1]+.05f;auto down=centre;down[1]-=.05f;NativeSphereResult volume;
        if(!native_world_sphere(c,centre,down,player_radius,volume,why)){error="Survival primary clearance query failed: "+why;return false;}
        if(distance(volume.position,centre)>.15f)continue;
        selected=node;selected_point=p;lowest=p[1];
    }
    if(!selected){error="Survival multiplayer map has no clear grounded authored player start";return false;}
    // 8010B514 is the original borrowed-pointer setter and notification path,
    // not an owning resource constructor. Its anchor remains owned by the map.
    struct Scratch {uint32_t start;std::array<uint8_t,0x800> bytes;explicit Scratch(uint32_t sp):start(sp-0x800){std::memcpy(bytes.data(),host_ptr(start),bytes.size());}~Scratch(){std::memcpy(host_ptr(start),bytes.data(),bytes.size());}} scratch(c.r[1]);
    auto call=c;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],c.r[1]);call.r[3]=level;call.r[4]=selected;
    ssne_call(call,0x8010B514u);
    if(mem_read32(level+0x2A8)!=selected){error="Original Survival primary-start setter did not complete";return false;}
    log_message("[survival-native-start] lowest clear authored multiplayer anchor bound through original cooperative start setter x="+std::to_string(selected_point[0])+" y="+std::to_string(selected_point[1])+" z="+std::to_string(selected_point[2]));return true;
}
bool capture_native_survival_anchors(std::vector<NativeSurvivalPoint>& out,std::string& error){
    out.clear();error.clear();
    if(!native_survival_available() || !native_survival_enabled() || mem_read32(0x8010B0ACu)!=0x9421FF20u){error="Native Survival spawn schema unavailable";return false;}
    const auto level=mem_read32(0x802DACC0u);if(!range(level,0x2DC)){error="Native Survival level unavailable";return false;}
    const auto count=mem_read32(level+0x2D8);if(count>10){error="Native multiplayer spawn count is invalid";return false;}
    for(unsigned i=0;i<count;++i){NativeSurvivalPoint p;if(!anchor(mem_read32(level+0x2B0+i*4),p)){error="Native multiplayer spawn matrix is invalid";out.clear();return false;}if(std::none_of(out.begin(),out.end(),[&](const auto& q){return distance(p,q)<.1f;}))out.push_back(p);}
    if(out.empty()){NativeSurvivalPoint p;if(anchor(mem_read32(level+0x2A8),p))out.push_back(p);}
    if(out.empty()){error="This map has no supported native spawn anchors";return false;}return true;
}
float native_survival_spawn_radius(unsigned definition){
    if(std::find(roster.begin(),roster.end(),definition)==roster.end())return 0.f;
    if(definition==5 || definition==11)return 3.f;
    if(definition==7 || definition==8 || definition==10)return 2.f;
    return 1.25f;
}
bool choose_native_survival_spawn(PPCContext& c,unsigned definition,const std::vector<NativeSurvivalPoint>& players,const std::vector<NativeSurvivalPoint>& occupied,uint32_t selection,NativeSurvivalPoint& chosen,std::string& error){
    error.clear();const auto radius=native_survival_spawn_radius(definition);
    if(!native_survival_resources_ready() || !radius || c.r[13]!=0x802E22A0u || players.empty() || players.size()>8 || occupied.size()>256){error="Survival spawn context, resources or participants unavailable";return false;}
    for(const auto& p:players)if(!point_valid(p)){error="Invalid Survival player position";return false;}
    for(const auto& p:occupied)if(!point_valid(p)){error="Invalid Survival occupied position";return false;}
    std::vector<NativeSurvivalPoint> anchors;if(!capture_native_survival_anchors(anchors,error))return false;
    // Eight samples per authored anchor, capped independently of map size.
    const unsigned attempts=std::min(48u,unsigned(anchors.size())*8u);
    for(unsigned attempt=0;attempt<attempts;++attempt){
        auto p=anchors[(selection+attempt)%anchors.size()];const unsigned ring=attempt/unsigned(anchors.size());const float angle=6.28318530718f*float((selection+attempt*5u+ring*3u)%16u)/16.f;
        // The first local ring must clear the12m+radius minimum. Distinct
        // ring angles avoid sampling only the same two compass directions.
        const float spread=ring?16.f+8.f*float((ring-1)/2):0.f;p[0]+=std::sin(angle)*spread;p[2]+=std::cos(angle)*spread;
        auto above=p,below=p;above[1]+=8.f;below[1]-=16.f;std::optional<NativeSegmentHit> floor;
        if(!native_world_segment(c,above,below,floor,error))return false;
        if(!floor || floor->normal[1]<.725f)continue;
        p[1]=floor->position[1]+radius/floor->normal[1]+.05f;
        float nearest=std::numeric_limits<float>::max();unsigned near_player=0;
        for(unsigned i=0;i<players.size();++i){const auto d=distance(p,players[i]);if(d<nearest){nearest=d;near_player=i;}}
        // Use the nearest actual target's floor, rather than spawning under a
        // distant raised platform. A ground enemy needs a reachable height and
        // a direct unobstructed attack route; blocked fallbacks caused idle AI.
        if(std::abs(floor->position[1]-players[near_player][1])>4.f)continue;
        if(nearest<12.f+radius || nearest>100.f)continue;
        if(std::any_of(occupied.begin(),occupied.end(),[&](const auto& q){return distance(p,q)<2*radius+1.f;}))continue;
        // A nonzero native sweep detects an embedded sphere, unlike a zero
        // length query. The native correction must leave this location intact.
        auto down=p;down[1]-=.05f;NativeSphereResult volume;
        if(!native_world_sphere(c,p,down,radius,volume,error))return false;
        if(distance(volume.position,p)>.15f)continue;
        auto target=players[near_player];target[1]+=.5f;std::optional<NativeSegmentHit> sight;
        if(!native_world_segment(c,p,target,sight,error))return false;
        if(!sight){chosen=p;return true;}
    }
    error="No visible same-floor Survival spawn point away from living players";return false;
}
}
