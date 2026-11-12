#include "native_world.h"
#include "native_configuration.h"
#include "native_presentation.h"
#include "client_presentation.h"
#include "native_lifecycle.h"
#include "ssne/native_entity.h"
#include "ssne/runtime.h"
#include "ssne/game_addresses.h"
#include "ssne/native_party.h"
#include "ssne/replica_simulation.h"
#include <mutex>
#include <cmath>
#include <algorithm>
#include <optional>
#include <set>
#include <cstdlib>
#include <limits>
#include <deque>
namespace port::coop {
namespace {
bool readable(uint32_t address,unsigned bytes){return address>=0x80000000u && bytes<=ssne::guest_ram_size() && address<=ssne::guest_ram_end()-bytes;}
uint32_t player_vtable(){return mem_read32(0x80000000u)==0x47334250u?0x802121E8u:0x80212268u;}
bool pickup_vtable(uint32_t table){return table==ssne::game_address(0x80213D48u) || table==ssne::game_address(0x80214110u) || table==ssne::game_address(0x802144C8u);}
std::optional<std::array<float,3>> position(uint32_t object) {
    std::array<float,3> result;for(unsigned i=0;i<3;++i){result[i]=u2f(mem_read32(object+0x20+4*i));if(!std::isfinite(result[i]) || std::abs(result[i])>1000000.f)return std::nullopt;}return result;
}
std::array<float,12> energy_visual(uint32_t object){
    std::array<float,12> result{};const auto phase=mem_read32(object+0x148);
    result[6]=u2f(mem_read32(object+0x14C));
    // 80071004 leaves later-phase storage uninitialized. 80071754 initializes
    // charge jitter/light, 8005A6C0 supplies explosion coefficients on release,
    // and collision fills the direction used by 80071CD0. Never serialize heap
    // residue from a phase that does not consume those fields.
    if(phase==0 && result[6]>0)for(unsigned i=0;i<3;++i)result[i]=u2f(mem_read32(object+native_energy_visual_offsets[i]));
    if(phase<2){
        result[7]=u2f(mem_read32(object+0x150));
        // Native charge update uses charge*802004E4; the initial light uses
        // 802004A4. The replica setter adds 802004E0 in both cases.
        result[8]=phase==0 && result[6]==0?u2f(mem_read32(0x802004A4))-u2f(mem_read32(0x802004E0)):result[7]*u2f(mem_read32(0x802004E4));
    }else{
        for(unsigned i=3;i<6;++i)result[i]=u2f(mem_read32(object+native_energy_visual_offsets[i]));
        for(unsigned i=9;i<12;++i)result[i]=u2f(mem_read32(object+native_energy_visual_offsets[i]));
    }
    return result;
}
}
namespace {thread_local NativePoseReadScope* pose_read_scope=nullptr;}
NativePoseReadScope::NativePoseReadScope() noexcept:previous_(pose_read_scope){pose_read_scope=this;}
NativePoseReadScope::~NativePoseReadScope(){pose_read_scope=previous_;}
struct NativePoseLayoutCache {
    static bool find(uint32_t data,unsigned& count,uint64_t& hash){
        if(!pose_read_scope)return false;
        for(unsigned i=0;i<pose_read_scope->count_;++i){const auto& item=pose_read_scope->layouts_[i];
            if(item.data==data){count=item.count;hash=item.hash;return true;}
        }
        return false;
    }
    static void remember(uint32_t data,unsigned count,uint64_t hash){
        if(pose_read_scope && pose_read_scope->count_<pose_read_scope->layouts_.size())
            pose_read_scope->layouts_[pose_read_scope->count_++]={data,count,hash};
    }
};
static std::optional<NativeEnemyPoseBinding> native_model_pose_binding(uint32_t model,uint32_t dormant_resource=0) {
    if(model && (!readable(model,0xB0) || mem_read32(model)!=0x8021D668u))return std::nullopt;
    const auto expected=model?mem_read32(model+0x7C):0,matrices=model?mem_read32(model+0x78):0,resource=model?mem_read32(model+0xC):dormant_resource;
    if((model && (!expected || expected>256 || !readable(matrices,expected*48))) || !readable(resource,0x10))return std::nullopt;
    const auto data=mem_read32(resource+0xC);if(!readable(data,0x34))return std::nullopt;
    auto node=mem_read32(data+0x30);uint64_t hash=14695981039346656037ull;
    unsigned count=0;
    if(NativePoseLayoutCache::find(data,count,hash)){
        if(model && count!=expected)return std::nullopt;
        return NativeEnemyPoseBinding{model,matrices,count,hash};
    }
    while(node) {
        // The exact expected length also bounds cycles: a cyclic chain cannot
        // terminate within this limit. No per-bone tree allocation is needed.
        if(++count>(model?expected:256) || !readable(node,0x34))return std::nullopt;
        const auto name=mem_read32(node+4);bool terminated=false;
        for(unsigned k=0;k<96;++k){if(!readable(name+k,1))return std::nullopt;const auto c=mem_read8(name+k);hash^=c;hash*=1099511628211ull;if(!c){terminated=true;break;}if(c<32 || c>126)return std::nullopt;}
        if(!terminated)return std::nullopt;node=mem_read32(node);
    }
    if(!count || (model && count!=expected))return std::nullopt;
    NativePoseLayoutCache::remember(data,count,hash);
    return NativeEnemyPoseBinding{model,matrices,count,hash};
}
std::optional<NativeEnemyPoseBinding> native_enemy_pose_binding(uint32_t object) {
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(object,0x100))return std::nullopt;
    const auto proxy=mem_read32(object+0xFC);
    return native_model_pose_binding(readable(proxy,0x40)?mem_read32(proxy+0x20):0);
}
std::optional<NativeEnemyPoseBinding> native_corpse_pose_binding(uint32_t object){
    if(!readable(object,0x188) || mem_read32(object+0x48)!=0x8020C6B8u)return {};return native_model_pose_binding(mem_read32(object+0xF4));
}
std::optional<NativeEnemyPoseBinding> native_weapon_pose_binding(uint32_t actor,unsigned weapon,unsigned index) {
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(actor,0x790) || mem_read32(actor+0x48)!=player_vtable() || weapon>=12 || index>=2)return std::nullopt;
    const auto instance=mem_read32(actor+0x4BC+weapon*24+index*4);
    if(!readable(instance,0x228))return std::nullopt;
    return native_model_pose_binding(mem_read32(instance+0x28),mem_read32(instance+4));
}
std::optional<NativeEnemyPoseBinding> native_body_pose_binding(uint32_t actor,unsigned index){
    if(mem_read32(0x80000000u)!=0x47334250u || !readable(actor,0x14C) || mem_read32(actor+0x48)!=player_vtable())return {};
    const auto binding=native_render_binding(actor,EntityKind::Player,index);
    return binding && binding->model?native_model_pose_binding(mem_read32(binding->model+0x20)):std::nullopt;
}
std::optional<uint32_t> native_script_reference_address(int64_t reference) {
    if(reference==-1)return 0u;
    if(mem_read32(0x80000000u)!=0x47334250u)return std::nullopt;
    if(reference<=-2 && reference>=-9) {
        const auto slot=unsigned(-reference-2);if(slot>=ssne::native_party_capacity())return std::nullopt;
        const auto actor=mem_read32(ssne::native_party_registry()+slot*4);
        if(!readable(actor,0x790) || mem_read32(actor+0x48)!=player_vtable() || mem_read32(actor+0x17C)!=slot)return std::nullopt;
        return actor;
    }
    if(reference<=0 || reference>32767)return std::nullopt;
    // PAL 801514E8 inserts {u16 level index, native object} into this sorted map.
    const auto table=mem_read32(0x802DB7C4),count=mem_read32(0x802DB7C8),capacity=mem_read32(0x802DB7CC);
    if(count>capacity || count>32768 || !readable(table,count*8))return std::nullopt;
    unsigned lo=0,hi=count;
    while(lo<hi){auto middle=lo+(hi-lo)/2;if(mem_read16(table+middle*8)<reference)lo=middle+1;else hi=middle;}
    if(lo==count || mem_read16(table+lo*8)!=reference || (lo+1<count && mem_read16(table+(lo+1)*8)==reference))return std::nullopt;
    const auto object=mem_read32(table+lo*8+4),vt=readable(object,0x54)?mem_read32(object+0x48):0;
    if(!readable(vt,0x280) || mem_read32(vt+0x27C)!=0x8007A4CCu || mem_read32(object+0x50)!=reference)return std::nullopt;
    return object;
}
std::optional<int64_t> native_script_reference(uint32_t object) {
    if(!object)return int64_t(-1);
    if(!readable(object,0x54))return std::nullopt;
    if(mem_read32(object+0x48)==player_vtable()) {
        if(!readable(object,0x790))return std::nullopt;
        const auto slot=mem_read32(object+0x17C);if(slot>=ssne::native_party_capacity() || mem_read32(ssne::native_party_registry()+slot*4)!=object)return std::nullopt;
        return -2-int64_t(slot);
    }
    const auto reference=int64_t(int32_t(mem_read32(object+0x50)));
    auto address=native_script_reference_address(reference);
    return address && *address==object?std::optional<int64_t>(reference):std::nullopt;
}
struct NativeWorld::Impl {
    struct Record {EntityId id=0;uint32_t address=0,vtable=0,definition=0;bool actor=false,pickup=false,registered=false;std::optional<std::array<float,3>> spawn;bool factory_spawn=false;};
    mutable std::mutex mutex;std::map<uint32_t,Record> records;std::map<EntityId,uint32_t> by_id;
    std::array<PlayerId,max_players> owners{};
    uint64_t next=1,epoch=1,created=0,registered=0,removed=0;
    uint64_t projectile_tail=0,projectile_accepted=0;
    std::deque<NativeProjectileVisual> projectile_events;
    std::string projectile_error;
    void retire_projectile(const Record& record){
        if(ssne::replica_simulation() || mem_read32(0x80000000u)!=0x47334250u)return;
        const auto* type=native_projectile_class_by_vtable(record.vtable);
        if(!type || !record.registered || !record.spawn || !readable(record.address,type->size))return;
        if(!projectile_error.empty())return;
        if(projectile_events.size()>=32768 || projectile_tail>=uint64_t(INT64_MAX)){projectile_error="Native projectile visual producer overflow";return;}
        const auto object=record.address;NativeProjectileVisual event;event.sequence=projectile_tail+1;
        event.tags={int64_t(record.id),int64_t(type->definition),0,type->definition==6?int64_t(mem_read32(object+0x148)):type->definition==4?int64_t(mem_read32(object+0x13C)!=0):0};
        if(type->definition==10 || type->definition==15)event.tags[3]=mem_read16(object+0xD4);
        if(const auto source=records.find(mem_read32(object+0xC8));source!=records.end() && source->second.registered)event.tags[2]=int64_t(source->second.id);
        for(unsigned i=0;i<3;++i){event.values[i]=(*record.spawn)[i];event.values[3+i]=u2f(mem_read32(object+0x20+i*4));event.values[6+i]=u2f(mem_read32(object+0x104+i*4));event.values[9+i]=type->definition?u2f(mem_read32(object+0x2C+i*4)):0.f;event.values[12+i]=type->definition?u2f(mem_read32(object+0x68+i*4)):1.f;}
        if(type->definition==6){const auto visual=energy_visual(object);std::copy(visual.begin(),visual.end(),event.values.begin()+15);}
        if(type->definition==12)for(unsigned i=0;i<3;++i)event.values[27+i]=u2f(mem_read32(object+0x12C+i*4));
        if(type->definition==17)event.values[27]=u2f(mem_read32(object+0x16C));
        {const auto offsets=native_projectile_extra_offsets(type->definition);for(unsigned i=0;i<offsets.size();++i)event.values[15+i]=u2f(mem_read32(object+offsets[i]));}
        if(!validate_native_projectile_visual(event,projectile_error))return;
        projectile_events.push_back(event);++projectile_tail;
    }
    void notice(const ssne::NativeEntityNotice& notice) {
        std::lock_guard lock(mutex);
        using Event=ssne::NativeEntityNotice::Event;
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES") && mem_read32(ssne::game_address(0x802DB0E0))==4 && (notice.event==Event::Registered || notice.event==Event::CreatedActor || notice.event==Event::Removing)){
            const auto table=readable(notice.object,0x4C)?mem_read32(notice.object+0x48):0;
            const auto known=records.find(notice.object);const auto definition=known!=records.end()?known->second.definition:notice.definition;
            ssne::log_message("[coop-native-lifetime] event="+std::to_string(unsigned(notice.event))+" object="+std::to_string(notice.object)+" table="+std::to_string(table)+" definition="+std::to_string(definition)+" group="+std::to_string(notice.update_group));
            if(notice.event==Event::Registered && table==0x802009D8u && readable(notice.object,0x174)){const auto source=mem_read32(notice.object+0xC8);ssne::log_message("[coop-native-spinning-source] object="+std::to_string(notice.object)+" source="+std::to_string(source)+" source_table="+std::to_string(readable(source,0x4C)?mem_read32(source+0x48):0));}
            if(notice.event==Event::Registered && (table==0x801FEB30u || table==0x80201AF0u) && readable(notice.object,table==0x80201AF0u?0x144:0x134))ssne::log_message("[coop-native-grenade-register] object="+std::to_string(notice.object)+" source="+std::to_string(mem_read32(notice.object+0xC8))+" x="+std::to_string(u2f(mem_read32(notice.object+0x20)))+" y="+std::to_string(u2f(mem_read32(notice.object+0x24)))+" z="+std::to_string(u2f(mem_read32(notice.object+0x28))));
            if(notice.event==Event::Registered && table==0x802022E0u && readable(notice.object,0x164)){
                static unsigned dumped=0;if(dumped<4){char name[80];std::snprintf(name,sizeof(name),"native-rocket-registered-%u.bin",dumped++);if(auto* file=std::fopen(name,"wb")){for(unsigned i=0;i<0x164;++i){const auto byte=mem_read8(notice.object+i);std::fwrite(&byte,1,1,file);}std::fclose(file);}}
                ssne::log_message("[coop-native-rocket-register] object="+std::to_string(notice.object)+" source="+std::to_string(mem_read32(notice.object+0xC8))+" x="+std::to_string(u2f(mem_read32(notice.object+0x20)))+" y="+std::to_string(u2f(mem_read32(notice.object+0x24)))+" z="+std::to_string(u2f(mem_read32(notice.object+0x28)))+" vx="+std::to_string(u2f(mem_read32(notice.object+0x104)))+" vy="+std::to_string(u2f(mem_read32(notice.object+0x108)))+" vz="+std::to_string(u2f(mem_read32(notice.object+0x10C))));
            }
        }
        if(notice.event==Event::LevelExit){records.clear();by_id.clear();projectile_events.clear();projectile_tail=projectile_accepted=0;projectile_error.clear();++epoch;return;}
        if(notice.event==Event::Removing){if(auto found=records.find(notice.object);found!=records.end()){retire_projectile(found->second);by_id.erase(found->second.id);records.erase(found);++removed;}return;}
        if(!readable(notice.object,0x4C))return;
        auto table=mem_read32(notice.object+0x48);
        if(!readable(table,4))return;
        auto [it,fresh]=records.try_emplace(notice.object);
        auto& record=it->second;
        if(fresh || record.vtable!=table){by_id.erase(record.id);record={next++,notice.object,table,0,false,false,false,{}};by_id[record.id]=notice.object;}
        if(notice.event==Event::CreatedActor){record.definition=notice.definition;record.actor=true;++created;}
        if(notice.event==Event::CreatedPickup){record.definition=notice.definition;record.pickup=true;++created;}
        if(notice.event==Event::Spawned){record.spawn=notice.spawn_position;record.factory_spawn=true;}
        if(notice.event==Event::Registered){
            if(notice.update_group==-1)return;
            // CDD4/E0E4 leave the dissolve's angular velocity (+150) unset.
            // D40C/DAA0 integrate it later. A newly allocated corpse can reuse
            // a projectile block containing pointer bits at that offset.
            // Registration occurs before E0E4 clears the active flag, so that
            // flag itself may still contain allocator bytes here. Initialize
            // only the first registration; preserve subsequent registration
            // and checkpoint-restored angular velocity.
            if(!record.registered && table==0x8020C6B8u && readable(notice.object,0x188) && mem_read32(notice.object+0x174)<2)mem_write32(notice.object+0x150,f2u(0.f));
            if(!record.registered)++registered;record.registered=true;if(!record.factory_spawn && !record.spawn)record.spawn=position(notice.object);
        }
    }
};
EntityState native_projectile_visual_state(const NativeProjectileVisual& event,bool final_pose){
    EntityState e;e.id=uint64_t(event.tags[0]);e.kind=EntityKind::Projectile;e.definition=uint32_t(event.tags[1]);
    auto vector=[&](unsigned offset){return std::array<float,3>{event.values[offset],event.values[offset+1],event.values[offset+2]};};
    e.properties["spawn_position"]=vector(0);e.properties["position"]=vector(final_pose?3:0);e.properties["velocity"]=vector(6);e.properties["projectile_source"]=EntityReference{uint64_t(event.tags[2])};
    if(e.definition){e.properties["projectile_rotation"]=vector(9);e.properties["projectile_scale"]=vector(12);}
    if(e.definition==6){e.properties["projectile_energy_phase"]=event.tags[3];e.properties["projectile_energy_visual"]=std::vector<float>(event.values.begin()+15,event.values.begin()+27);}
    if(e.definition==12)e.properties["projectile_visual"]=vector(27);
    if(e.definition==17)e.properties["projectile_spin"]=event.values[27];
    if(e.definition==4)e.properties["projectile_trail_active"]=event.tags[3];
    if(e.definition==10 || e.definition==15)e.properties["projectile_model"]=event.tags[3];
    {const auto offsets=native_projectile_extra_offsets(e.definition);if(!offsets.empty())e.properties["projectile_extra_visual"]=std::vector<float>(event.values.begin()+15,event.values.begin()+15+offsets.size());}
    return e;
}
bool validate_native_projectile_visual(const NativeProjectileVisual& event,std::string& error){
    error.clear();if(!event.sequence || event.sequence>uint64_t(INT64_MAX) || event.tags[0]<=0 || event.tags[1]<0 || event.tags[1]>20 || !native_projectile_class(unsigned(event.tags[1])) || event.tags[2]<0 || event.tags[3]>(event.tags[1]==6?2:event.tags[1]==4?1:(event.tags[1]==10 || event.tags[1]==15)?164:0) || event.tags[3]<0){error="Invalid native projectile visual identity";return false;}
    for(unsigned i=0;i<event.values.size();++i){if(!std::isfinite(event.values[i]) || std::abs(event.values[i])>1000000.f){error="Invalid native projectile visual coefficient";return false;}if(((i>=15 && i<27 && event.tags[1]!=6 && i>=15+native_projectile_extra_offsets(unsigned(event.tags[1])).size()) || (i>=27 && event.tags[1]!=12 && !(event.tags[1]==17 && i==27))) && event.values[i]!=0){error="Unexpected native projectile visual class fields";return false;}}
    return validate_native_projectile_creation(native_projectile_visual_state(event,true),error);
}
NativeProjectileVisualSnapshot NativeWorld::projectile_visuals() const {
    std::lock_guard lock(impl_->mutex);return {impl_->epoch,impl_->projectile_tail,{impl_->projectile_events.begin(),impl_->projectile_events.end()},impl_->projectile_error};
}
bool NativeWorld::acknowledge_projectile_visuals(uint64_t epoch,uint64_t tail){
    std::lock_guard lock(impl_->mutex);if(epoch!=impl_->epoch || tail<impl_->projectile_accepted || tail>impl_->projectile_tail)return false;
    impl_->projectile_accepted=tail;while(!impl_->projectile_events.empty() && impl_->projectile_events.front().sequence<=tail)impl_->projectile_events.pop_front();return true;
}
EntityId NativeWorld::local_entity_id(uint32_t address)const{std::lock_guard lock(impl_->mutex);const auto found=impl_->records.find(address);return found==impl_->records.end()?0:found->second.id;}
namespace {
constexpr uint32_t lifetime_magic=0x4E574350u,lifetime_version=2;
constexpr size_t lifetime_header=80,lifetime_record=36,lifetime_max_records=65536,lifetime_visual_size=160,lifetime_max_visuals=32768;
uint64_t lifetime_hash(const uint8_t* data,size_t length){uint64_t h=14695981039346656037ull;for(size_t i=0;i<length;++i){h^=data[i];h*=1099511628211ull;}return h;}
void lifetime_u32(std::vector<uint8_t>& out,uint32_t v){for(int shift=24;shift>=0;shift-=8)out.push_back(uint8_t(v>>shift));}
void lifetime_u64(std::vector<uint8_t>& out,uint64_t v){lifetime_u32(out,uint32_t(v>>32));lifetime_u32(out,uint32_t(v));}
struct LifetimeReader {
    const std::vector<uint8_t>& bytes;size_t offset=0;
    uint32_t u32(){uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|bytes.at(offset++);return v;}
    uint64_t u64(){const uint64_t hi=u32();return (hi<<32)|u32();}
};
}
bool NativeWorld::save_lifetimes(std::vector<uint8_t>& output,std::string& error) const {
    error.clear();std::lock_guard lock(impl_->mutex);const auto& s=*impl_;
    if(s.records.size()>lifetime_max_records || !s.projectile_error.empty()){error=s.projectile_error.empty()?"Too many native lifetimes for checkpoint":s.projectile_error;return false;}
    std::vector<uint8_t> bytes;bytes.reserve(lifetime_header+lifetime_record*s.records.size()+8);
    for(auto v:{lifetime_magic,lifetime_version,mem_read32(0x80000000u),ssne::guest_ram_size()})lifetime_u32(bytes,v);
    for(auto v:{s.epoch,s.next,s.created,s.registered,s.removed,s.projectile_tail,s.projectile_accepted})lifetime_u64(bytes,v);
    lifetime_u32(bytes,uint32_t(s.records.size()));
    lifetime_u32(bytes,uint32_t(s.projectile_events.size()));
    for(const auto& [address,r]:s.records){
        lifetime_u64(bytes,r.id);lifetime_u32(bytes,address);lifetime_u32(bytes,r.vtable);lifetime_u32(bytes,r.definition);
        lifetime_u32(bytes,unsigned(r.actor)|(unsigned(r.pickup)<<1)|(unsigned(r.registered)<<2)|(unsigned(bool(r.spawn))<<3)|(unsigned(r.factory_spawn)<<4));
        for(float v:r.spawn.value_or(std::array<float,3>{}))lifetime_u32(bytes,f2u(v));
    }
    for(const auto& event:s.projectile_events){lifetime_u64(bytes,event.sequence);for(auto tag:event.tags)lifetime_u64(bytes,uint64_t(tag));for(auto value:event.values)lifetime_u32(bytes,f2u(value));}
    lifetime_u64(bytes,lifetime_hash(bytes.data(),bytes.size()));output=std::move(bytes);return true;
}
struct NativeWorld::PreparedLifetimes::State {
    std::shared_ptr<Impl> owner;
    std::unique_lock<std::mutex> lock;
    std::map<uint32_t,Impl::Record> records;
    std::map<EntityId,uint32_t> by_id;
    uint64_t epoch=0,next=0,created=0,registered=0,removed=0,projectile_tail=0,projectile_accepted=0;
    std::deque<NativeProjectileVisual> projectile_events;
    bool committed=false;
    explicit State(std::shared_ptr<Impl> world):owner(std::move(world)),lock(owner->mutex){}
};
NativeWorld::PreparedLifetimes::PreparedLifetimes(std::unique_ptr<State> state):state_(std::move(state)){}
NativeWorld::PreparedLifetimes::~PreparedLifetimes()=default;
uint64_t NativeWorld::PreparedLifetimes::projectile_visual_tail() const noexcept{return state_->projectile_tail;}
void NativeWorld::PreparedLifetimes::commit() noexcept {
    auto& s=*state_;if(s.committed)return;
    s.owner->records.swap(s.records);s.owner->by_id.swap(s.by_id);
    s.owner->epoch=s.epoch;s.owner->next=s.next;s.owner->created=s.created;
    s.owner->registered=s.registered;s.owner->removed=s.removed;
    s.owner->projectile_tail=s.projectile_tail;s.owner->projectile_accepted=s.projectile_accepted;s.owner->projectile_events.swap(s.projectile_events);s.owner->projectile_error.clear();
    s.committed=true;s.lock.unlock();
}
std::unique_ptr<NativeWorld::PreparedLifetimes> NativeWorld::prepare_lifetimes(const std::vector<uint8_t>& bytes,uint64_t fresh_epoch,std::string& error) {
    error.clear();auto fail=[&](const char* message)->std::unique_ptr<PreparedLifetimes>{error=message;return {};};
    if(bytes.size()<60+8 || bytes.size()>lifetime_header+lifetime_record*lifetime_max_records+lifetime_visual_size*lifetime_max_visuals+8)return fail("Invalid native lifetime checkpoint size");
    LifetimeReader checksum{bytes,bytes.size()-8};if(checksum.u64()!=lifetime_hash(bytes.data(),bytes.size()-8))return fail("Native lifetime checkpoint checksum differs");
    LifetimeReader in{bytes};
    if(in.u32()!=lifetime_magic)return fail("Unsupported native lifetime checkpoint");const auto version=in.u32();if(version!=1 && version!=lifetime_version)return fail("Unsupported native lifetime checkpoint");
    if(version>=2 && bytes.size()<lifetime_header+8)return fail("Truncated native lifetime checkpoint header");
    if(in.u32()!=mem_read32(0x80000000u) || in.u32()!=ssne::guest_ram_size())return fail("Native lifetime checkpoint game or memory differs");
    const auto saved_epoch=in.u64(),next=in.u64(),created=in.u64(),registered=in.u64(),removed=in.u64();const auto projectile_tail=version>=2?in.u64():0,projectile_accepted=version>=2?in.u64():0;const auto count=in.u32(),visual_count=version>=2?in.u32():0;
    if(count>lifetime_max_records || visual_count>lifetime_max_visuals || projectile_accepted>projectile_tail || projectile_tail-projectile_accepted!=visual_count || bytes.size()!=(version>=2?lifetime_header:60)+lifetime_record*size_t(count)+lifetime_visual_size*visual_count+8 || !next || !saved_epoch || projectile_tail>uint64_t(INT64_MAX) || fresh_epoch<=saved_epoch || fresh_epoch==std::numeric_limits<uint64_t>::max())return fail("Invalid native lifetime checkpoint metadata");
    std::map<uint32_t,Impl::Record> records;std::map<EntityId,uint32_t> by_id;
    for(unsigned i=0;i<count;++i){
        Impl::Record r;r.id=in.u64();r.address=in.u32();r.vtable=in.u32();r.definition=in.u32();const auto flags=in.u32();
        r.actor=flags&1;r.pickup=flags&2;r.registered=flags&4;r.factory_spawn=flags&16;
        std::array<float,3> spawn;for(auto& v:spawn)v=u2f(in.u32());if(flags&8)r.spawn=spawn;
        if(!r.id || r.id>=next || flags>31 || (r.factory_spawn && !r.spawn) || !readable(r.address,0x4C) || !readable(r.vtable,4))return fail("Invalid saved native lifetime");
        if(r.registered && mem_read32(r.address+0x48)!=r.vtable)return fail("Saved native lifetime does not match restored game memory");
        if(r.spawn)for(float v:spawn)if(!std::isfinite(v) || std::abs(v)>1000000.f)return fail("Invalid saved native spawn position");
        if(!records.emplace(r.address,r).second || !by_id.emplace(r.id,r.address).second)return fail("Duplicate saved native lifetime identity");
    }
    std::deque<NativeProjectileVisual> projectile_events;for(unsigned i=0;i<visual_count;++i){NativeProjectileVisual event;event.sequence=in.u64();for(auto& tag:event.tags)tag=int64_t(in.u64());for(auto& value:event.values)value=u2f(in.u32());if(event.sequence!=projectile_accepted+i+1 || !validate_native_projectile_visual(event,error))return fail("Invalid saved native projectile visual");projectile_events.push_back(event);}
    auto candidate=std::make_unique<PreparedLifetimes::State>(impl_);
    if(fresh_epoch<=impl_->epoch)return fail("Checkpoint needs a new native world epoch");
    candidate->records=std::move(records);candidate->by_id=std::move(by_id);
    candidate->epoch=fresh_epoch;candidate->next=next;candidate->created=created;
    candidate->registered=registered;candidate->removed=removed;
    candidate->projectile_tail=projectile_tail;candidate->projectile_accepted=projectile_accepted;candidate->projectile_events=std::move(projectile_events);
    return std::unique_ptr<PreparedLifetimes>(new PreparedLifetimes(std::move(candidate)));
}

bool NativeWorld::load_lifetimes(const std::vector<uint8_t>& bytes,uint64_t fresh_epoch,std::string& error) {
    auto candidate=prepare_lifetimes(bytes,fresh_epoch,error);
    if(!candidate)return false;
    candidate->commit();return true;
}
NativeWorld::NativeWorld():impl_(std::make_shared<Impl>()) {
    std::weak_ptr<Impl> weak=impl_;
    subscription_=ssne::observe_native_entities([weak](const auto& notice){if(auto state=weak.lock())state->notice(notice);});
}
NativeWorld::~NativeWorld(){ssne::stop_observing_native_entities(subscription_);}
void NativeWorld::set_player_owner(unsigned slot,PlayerId id){if(slot>=max_players || (id.generation && id.slot!=slot))return;std::lock_guard lock(impl_->mutex);impl_->owners[slot]=id;}
uint32_t NativeWorld::local_address(EntityId id) const {
    std::lock_guard lock(impl_->mutex);auto index=impl_->by_id.find(id);if(index==impl_->by_id.end())return 0;
    auto found=impl_->records.find(index->second);if(found==impl_->records.end())return 0;
    const auto& record=found->second;const auto address=index->second;
    return record.id==id && record.registered && readable(address,0x4C) && mem_read32(address+0x48)==record.vtable?address:0;
}
std::function<ssne::NativeHitIdentity(uint32_t)> NativeWorld::hit_identity_resolver() const {
    std::weak_ptr<Impl> weak=impl_;
    return [weak](uint32_t object){ssne::NativeHitIdentity result;if(auto state=weak.lock()){std::lock_guard lock(state->mutex);auto found=state->records.find(object);if(found==state->records.end() || !found->second.registered || !readable(object,0x4C) || mem_read32(object+0x48)!=found->second.vtable)return result;result.entity=found->second.id;if(found->second.vtable==player_vtable() && readable(object,0x790)){const auto slot=mem_read32(object+0x17C);if(slot<max_players)result.generation=state->owners[slot].generation;}}return result;};
}
std::vector<NativeWorld::PlayerBinding> NativeWorld::player_bindings() const {
    auto& state=*impl_;std::lock_guard lock(state.mutex);std::vector<PlayerBinding> out;
    for(const auto& [address,record]:state.records){
        if(!record.registered || record.vtable!=player_vtable() || !readable(address,0x790) || mem_read32(address+0x48)!=player_vtable())continue;
        const auto slot=mem_read32(address+0x17C);
        if(slot<ssne::native_party_capacity() && (mem_read32(ssne::native_party_registry()+slot*4)==address || ssne::native_party_spawn_waiting(address)))out.push_back({address,slot});
    }return out;
}
NativeWorldSample NativeWorld::capture(uint64_t tick) const {return capture(tick,false);}
NativeWorldSample NativeWorld::capture_bindings() const {return capture(0,true);}
NativeWorldSample NativeWorld::capture(uint64_t tick,bool bindings_only) const {
    auto& state=*impl_;std::lock_guard lock(state.mutex);NativeWorldSample sample;
    sample.epoch=state.epoch;sample.tick=tick;sample.created=state.created;sample.registered=state.registered;sample.removed=state.removed;
    sample.mode=mem_read32(ssne::game_address(0x802DB1BC));sample.scene=mem_read32(ssne::game_address(0x802DB0E0));
    const auto presentation_rules=bindings_only?NativeConfigurationSample{}:read_native_configuration(native_global_configuration(),"PuzzleConfig.HideWeapons");
    for(const auto& [address,record]:state.records) {
        if(!record.registered || !readable(address,0x4C) || mem_read32(address+0x48)!=record.vtable)continue;
        const bool trigger=mem_read32(0x80000000u)==0x47334250u && record.vtable==0x80209758u;
        const bool button=mem_read32(0x80000000u)==0x47334250u && record.vtable==0x80208B60u;
        const bool script=trigger || button;
        const bool marker=mem_read32(0x80000000u)==0x47334250u && native_marker_class(record.vtable);
        const bool brush=mem_read32(0x80000000u)==0x47334250u && record.vtable==0x80206868u;
        const bool model_destructible=record.vtable==0x80206348u;
        const bool destructible=record.vtable==0x80204190u || model_destructible;
        const auto* projectile_type=mem_read32(0x80000000u)==0x47334250u?native_projectile_class_by_vtable(record.vtable):nullptr;
        const bool projectile=projectile_type!=nullptr;
        const bool corpse=record.vtable==0x8020C6B8u && readable(address,0x188) && mem_read32(address+0x124)!=2;
        const bool piece=record.vtable==0x8020B990u && readable(address,0x130);
        const bool detached_rotor=record.vtable==0x8020B228u && readable(address,0x118) && !mem_read32(address+0xDC);
        bool player=record.vtable==player_vtable();bool pickup=record.pickup || pickup_vtable(record.vtable);
        if(!player && !record.actor && !pickup && !brush && !script && !projectile && !marker && !corpse && !detached_rotor && !piece && !destructible){sample.unmapped.push_back({address,record.vtable});continue;}
        if(!readable(address,player?0x790:pickup?0x184:projectile?projectile_type->size:brush?0x13C:trigger?0xA8:button?0x68:0xD4))continue;
        auto xyz=position(address);if(!xyz)continue;
        EntityState entity;entity.id=record.id;entity.kind=destructible?EntityKind::Destructible:corpse || detached_rotor || piece?EntityKind::Physics:player?EntityKind::Player:pickup?EntityKind::Pickup:projectile?EntityKind::Projectile:brush?EntityKind::Door:trigger?EntityKind::Mission:button?EntityKind::Switch:marker?EntityKind::Physics:EntityKind::Enemy;
        entity.definition=piece?5:destructible?(model_destructible?2:1):detached_rotor?4:corpse?3:player?2:pickup?mem_read32(address+0x180):projectile?projectile_type->definition:brush || script?1:marker?(record.vtable==0x802054A0u?1:2):record.definition;
        if((corpse || detached_rotor || piece) && !record.factory_spawn){auto& stored=state.records.at(address);stored.spawn=*xyz;stored.factory_spawn=true;}
        entity.properties["position"]=*xyz;
        if(brush)entity.properties["brush_index"]=int64_t(mem_read32(address+0x50));
        if(record.spawn)entity.properties["spawn_position"]=*record.spawn;
        if(bindings_only){
            if(player){
                const auto slot=mem_read32(address+0x17C);
                if(slot>=max_players || slot>=ssne::native_party_capacity() || (mem_read32(ssne::native_party_registry()+slot*4)!=address && !ssne::native_party_spawn_waiting(address)))continue;
                entity.owner=state.owners[slot];entity.properties["player_slot"]=int64_t(slot);
            }
            if(destructible){
                // Index zero is an authored, unindexed geometry owner in
                // Clevel6_2. Destructibles also bind by resource and spawn
                // position; they need no script-map lookup for that sentinel.
                const auto index=mem_read32(address+0x50)==0?std::optional<int64_t>(0):native_script_reference(address);const auto config=mem_read32(address+0x54);
                if(!index || !readable(config,0x24)){sample.error="Native destructible identity is unavailable: object="+std::to_string(address)+" class="+std::to_string(record.vtable)+" index="+std::to_string(mem_read32(address+0x50))+" config="+std::to_string(config);return sample;}
                entity.properties["destruct_index"]=*index;entity.properties["destruct_resource"]=int64_t(mem_read32(config));
            }
            if(script || marker){
                const auto index=script && mem_read32(address+0x50)==0?std::optional<int64_t>(0):native_script_reference(address);
                if(!index || *index<0 || (marker && *index==0)){sample.error="Native level identity not ready";return sample;}
                entity.properties[script?"script_index":"marker_index"]=*index;
            }
            if(brush){
                const auto config=mem_read32(address+0x5C);if(!readable(config,0x38))continue;
                const auto progress=u2f(mem_read32(address+0xC0));if(!std::isfinite(progress) || progress<0 || progress>1)continue;
                entity.properties["brush_resource"]=int64_t(mem_read32(config+0xC));
            }
            sample.entities.emplace(entity.id,std::move(entity));continue;
        }
        if(destructible){
            const auto index=mem_read32(address+0x50)==0?std::optional<int64_t>(0):native_script_reference(address);const auto config=mem_read32(address+0x54);
            if(!index || !readable(config,0x24)){sample.error="Native destructible identity is unavailable: object="+std::to_string(address)+" class="+std::to_string(record.vtable)+" index="+std::to_string(mem_read32(address+0x50))+" config="+std::to_string(config);return sample;}
            entity.properties["destruct_index"]=*index;entity.properties["destruct_resource"]=int64_t(mem_read32(config));
            if(model_destructible){
                const auto manager=mem_read32(mem_read32(0x802DACC0)+0x298),resource=mem_read32(config);
                if(!readable(manager,0x20) || resource>=mem_read32(manager+0x1C) || !readable(mem_read32(manager+0x18)+resource*20,20)){sample.error="Native model destruction geometry unavailable";return sample;}
                const auto geometry=int32_t(mem_read32(mem_read32(manager+0x18)+resource*20+8));
                entity.properties["destruct_state"]=int64_t(geometry);
                entity.properties["destruct_active"]=int64_t(mem_read32(address+0x84));
                entity.properties["destruct_stage"]=int64_t(mem_read32(address+0x88));
                entity.properties["destruct_destroyed"]=int64_t(geometry<0);
            }else{
                entity.properties["destruct_destroyed"]=int64_t(mem_read32(address+0x60));
                entity.properties["destruct_health"]=u2f(mem_read32(address+0x58));entity.properties["destruct_damage"]=u2f(mem_read32(address+0x5C));
            }
            if(!validate_native_destructible(address,entity,sample.error))return sample;
        }
        if(marker){
            const auto index=native_script_reference(address);
            if(!index || *index<=0){sample.error="Native marker level index not ready";return sample;}
            entity.properties["marker_index"]=*index;
            entity.properties["marker_active"]=int64_t(mem_read32(address+0x8C)!=0);
            if(entity.definition==2){
                std::vector<float> animation;
                for(unsigned off:{0x90u,0x94u,0xA0u,0xA4u,0xA8u,0xACu})animation.push_back(u2f(mem_read32(address+off)));
                const auto proxy=mem_read32(address+0x8C),matrix=proxy && readable(proxy,8)?mem_read32(proxy+4):0;
                if(proxy && !readable(matrix,48)){sample.error="Original arrow render matrix is unavailable";return sample;}
                for(unsigned i=0;i<12;++i)animation.push_back(matrix?u2f(mem_read32(matrix+i*4)):0.f);
                if(!validate_native_arrow_animation(address,animation,sample.error))return sample;
                entity.properties["marker_animation"]=std::move(animation);
            }
        }
        if(player || pickup || record.actor || corpse || detached_rotor || piece)for(unsigned model_index=0;model_index<(player?4u:1u);++model_index){
            const std::string prefix=model_index?"body_render"+std::to_string(model_index)+"_":"render_";
            auto binding=native_render_binding(address,entity.kind,model_index);
            if(!binding){sample.error="Native render proxy is unsupported: object="+std::to_string(address)+" kind="+std::to_string(unsigned(entity.kind))+" definition="+std::to_string(entity.definition);return sample;}
            entity.properties[prefix+"visible"]=int64_t(binding->model?mem_read8(binding->model+0x2D):0);
            if(binding->model){
                for(unsigned row=0;row<3;++row){std::array<float,4> values{};
                    for(unsigned col=0;col<4;++col){values[col]=u2f(mem_read32(binding->matrix+row*16+col*4));if(!std::isfinite(values[col]) || std::abs(values[col])>1000000.f){sample.error="Invalid native render matrix";return sample;}}
                    entity.properties[prefix+"row"+std::to_string(row)]=values;
                }
            }
        }
        if(piece){
            const auto binding=native_render_binding(address,EntityKind::Physics);
            const auto name=binding && binding->model?ssne::native_static_model_name(mem_read32(binding->model+0x1C)):std::string{};
            if(name.empty()){sample.unmapped.push_back({address,record.vtable});continue;}
            entity.properties["part_model"]=name;entity.properties["part_side"]=int64_t(mem_read32(address+0xD8));
            entity.properties["part_mask"]=int64_t(mem_read32(binding->model+0x30));entity.properties["part_flags"]=int64_t(mem_read32(address+0x1C));
            if(!validate_native_npc_piece(entity,sample.error))return sample;
        }
        if(player)for(unsigned part=0;part<4;++part)if(auto binding=native_body_pose_binding(address,part);binding && binding->model){
            const auto prefix="body"+std::to_string(part)+"_bone_";
            std::vector<float> values;bool finite=true;
            for(unsigned i=0;i<binding->count*12;++i){auto f=u2f(mem_read32(binding->matrices+i*4));finite&=std::isfinite(f) && std::abs(f)<=10000.f;values.push_back(f);}
            if(finite){entity.properties[prefix+"count"]=int64_t(binding->count);entity.properties[prefix+"layout"]=int64_t(binding->layout);entity.properties[prefix+"matrices"]=std::move(values);}
        }
        if(player){const auto body=mem_read32(address+0x148);if(readable(body,0xA8)){const auto resource=int32_t(mem_read32(body+0xA4));if(resource< -1 || resource>12){sample.error="Invalid original held weapon resource";return sample;}entity.properties["body_weapon_resource"]=int64_t(resource);}}
        if(projectile) {
            std::array<float,3> velocity;
            for(unsigned i=0;i<3;++i){velocity[i]=u2f(mem_read32(address+0x104+i*4));if(!std::isfinite(velocity[i]) || std::abs(velocity[i])>10000.f){sample.error="Invalid native rocket velocity";return sample;}}
            entity.properties["velocity"]=velocity;
            {const auto offsets=native_projectile_extra_offsets(projectile_type->definition);if(!offsets.empty()){std::vector<float> v;for(auto offset:offsets)v.push_back(u2f(mem_read32(address+offset)));entity.properties["projectile_extra_visual"]=std::move(v);}}
            if(projectile_type->definition==10 || projectile_type->definition==15)entity.properties["projectile_model"]=int64_t(mem_read16(address+0xD4));
            if(projectile_type->definition==12){std::array<float,3> visual;for(unsigned i=0;i<3;++i)visual[i]=u2f(mem_read32(address+0x12C+i*4));entity.properties["projectile_visual"]=visual;}
            if(projectile_type->definition==17)entity.properties["projectile_spin"]=u2f(mem_read32(address+0x16C));
            if(projectile_type->definition==4)entity.properties["projectile_trail_active"]=int64_t(mem_read32(address+0x13C)!=0);
            if(projectile_type->definition==6){const auto visual=energy_visual(address);entity.properties["projectile_energy_visual"]=std::vector<float>(visual.begin(),visual.end());entity.properties["projectile_energy_phase"]=int64_t(mem_read32(address+0x148));}
            if(projectile_type->definition!=0){std::array<float,3> rotation;for(unsigned i=0;i<3;++i){rotation[i]=u2f(mem_read32(address+0x2C+i*4));if(!std::isfinite(rotation[i]) || std::abs(rotation[i])>10000.f){sample.error="Invalid native grenade rotation";return sample;}}entity.properties["projectile_rotation"]=rotation;std::array<float,3> scale;for(unsigned i=0;i<3;++i){scale[i]=u2f(mem_read32(address+0x68+i*4));if(!std::isfinite(scale[i]) || scale[i]<0 || scale[i]>100){sample.error="Invalid native grenade scale";return sample;}}entity.properties["projectile_scale"]=scale;}
            const auto source=mem_read32(address+0xC8);EntityId source_id=0;
            if(source){const auto found=state.records.find(source);if(found!=state.records.end() && found->second.registered && (found->second.actor || found->second.vtable==player_vtable()))source_id=found->second.id;}
            entity.properties["projectile_source"]=EntityReference{source_id};
            if(!validate_native_projectile_creation(entity,sample.error))return sample;
        }
        if(script) {
            // Alevel12_2 also has an authored trigger with no script-map
            // entry. Its own identity uses the original zero plus spawn;
            // every outgoing reference still requires a resolvable target.
            const auto index=mem_read32(address+0x50)==0?std::optional<int64_t>(0):native_script_reference(address);
            if(!index || *index<0){sample.error="Native script level index not ready";return sample;}
            entity.properties["script_index"]=*index;
            entity.properties["script_enabled"]=int64_t(mem_read32(address+0x4C));
            std::vector<int64_t> references;
            for(unsigned i=0;i<(trigger?11u:1u);++i) {
                auto reference=native_script_reference(mem_read32(address+0x58+i*4));
                if(!reference){sample.error="Native script reference is not indexed";return sample;}
                references.push_back(*reference);
            }
            entity.properties["script_references"]=references;
            if(trigger) {
                // Verified initializer 800846D0, event 800848C0 and update 800847C4.
                for(auto [name,offset]:{std::pair{"trigger_previous_enabled",0x88u},std::pair{"trigger_enabled",0x8Cu},std::pair{"trigger_auto_start",0x9Cu},std::pair{"trigger_completed",0xA0u},std::pair{"trigger_dispatched",0xA4u},std::pair{"trigger_pulses",0x90u},std::pair{"trigger_firings",0x94u}})entity.properties[name]=int64_t(mem_read32(address+offset));
                entity.properties["trigger_timer"]=u2f(mem_read32(address+0x98));
            } else {
                entity.properties["switch_enabled"]=int64_t(mem_read32(address+0x60));
                entity.properties["switch_available"]=int64_t(mem_read32(address+0x64));
            }
        }
        if(brush) {
            // PAL MovingBrush initializer 8007EDCC and update 8007F0B8.
            // Geometry resource indices are level-local identifiers, never pointers.
            const auto configuration=mem_read32(address+0x5C);
            if(!readable(configuration,0x38))continue;
            entity.properties["brush_resource"]=int64_t(mem_read32(configuration+0xC));
            entity.properties["brush_segment"]=int64_t(mem_read32(address+0xC8));
            const auto progress=u2f(mem_read32(address+0xC0));
            if(!std::isfinite(progress) || progress<0 || progress>1)continue;
            entity.properties["brush_progress"]=progress;
            entity.properties["brush_active"]=int64_t(mem_read32(address+0xB0));
            entity.properties["brush_completed"]=int64_t(mem_read32(address+0x58));
        }
        if((record.actor && !player) || corpse) {
            std::array<float,3> rotation,scale;bool valid_pose=true;
            for(unsigned i=0;i<3;++i){rotation[i]=u2f(mem_read32(address+0x2C+i*4));scale[i]=u2f(mem_read32(address+0x68+i*4));valid_pose&=std::isfinite(rotation[i]) && std::abs(rotation[i])<=10000 && std::isfinite(scale[i]) && scale[i]>0 && scale[i]<=100;}
            if(valid_pose){entity.properties["enemy_rotation"]=rotation;entity.properties["enemy_scale"]=scale;}
            if(auto binding=corpse?native_corpse_pose_binding(address):native_enemy_pose_binding(address)) {
                std::vector<float> pose;bool finite=true;
                for(unsigned i=0;i<binding->count*12;++i){const auto f=u2f(mem_read32(binding->matrices+i*4));finite&=std::isfinite(f) && std::abs(f)<=10000.f;pose.push_back(f);}
                if(finite){entity.properties["enemy_bone_count"]=int64_t(binding->count);entity.properties["enemy_bone_layout"]=int64_t(binding->layout);entity.properties["enemy_bone_matrices"]=std::move(pose);}
            }
        }
        if(corpse){
            entity.properties["corpse_definition"]=int64_t(mem_read32(address+0x124));entity.properties["corpse_flags"]=int64_t(mem_read32(address+0x1C));entity.properties["corpse_mode"]=int64_t(mem_read32(address+0x174));
            std::vector<float> matrix;for(unsigned i=0;i<12;++i)matrix.push_back(u2f(mem_read32(address+0x98+i*4)));entity.properties["corpse_matrix"]=std::move(matrix);
            // Exploded bodies use the reliable gibs journal, never the native
            // dissolve state. Those unused fields can contain allocator bytes.
            const bool exploded=mem_read32(address+0x174)==2;
            const auto active=exploded?0u:mem_read32(address+0x134);std::vector<float> life;
            for(unsigned offset:{0x128u,0x12Cu,0x130u,0x140u,0x144u,0x148u,0x14Cu,0x150u,0x154u,0x118u})life.push_back(!exploded && (active || offset==0x128 || offset==0x12C)?u2f(mem_read32(address+offset)):0.f);
            // +180 is the source material category, not a dissolve phase.
            // Original80093190 copies a signed16-bit category here; D40C
            // selects styles14/13/15 for1/2/3 and style17 for every other
            // category. Encode that same default as0 in the existing wire
            // selector, without modifying the host's native material state.
            const auto material=mem_read32(address+0x180);
            const auto selector=material>=1 && material<=3?material:0u;
            life.push_back(float(active));life.push_back(active?float(mem_read32(address+0x17C)):0.f);life.push_back(active?float(selector):0.f);entity.properties["corpse_lifecycle"]=std::move(life);
            if(!validate_native_corpse_creation(entity,sample.error))return sample;
        }
        if(!capture_native_boss_parts(address,entity,sample.error))return sample;
        if(player || record.actor){float health=u2f(mem_read32(address+0xCC));if(std::isfinite(health))entity.properties["health"]=health;}
        if(player && (!capture_native_interaction_hint(address,entity,sample.error) || (sample.mode==1 && !capture_native_player_notices(address,entity,sample.error))))return sample;
        if(player && sample.mode==1 && !capture_native_combo_hud(address,entity,sample.error))return sample;
        if(entity.kind==EntityKind::Enemy && entity.definition==20 && mem_read32(mem_read32(address+0x24C)+0xDC)){
            const auto child=mem_read32(address+0x24C);const auto rotor=native_render_binding(child,EntityKind::Pickup);
            if(!readable(child,0x118) || mem_read32(child+0x48)!=0x8020B228u || !rotor || !rotor->model){sample.error="Live native propeller binding unavailable";return sample;}
            entity.properties["rotor_visible"]=int64_t(mem_read8(rotor->model+0x2D));
            entity.properties["rotor_mask"]=int64_t(mem_read32(rotor->model+0x30));
            for(unsigned row=0;row<3;++row){std::array<float,4> values;for(unsigned col=0;col<4;++col)values[col]=u2f(mem_read32(rotor->matrix+row*16+col*4));entity.properties["rotor_row"+std::to_string(row)]=values;}
        }
        if(player && !capture_native_reentry_hud(address,entity,sample.error))return sample;
        if(player && !capture_native_weapon_sidebar(address,entity,sample.error))return sample;
        if(player && !capture_native_weapon_bar(address,entity,sample.error))return sample;
        if(player) {
            // Native score award 800B473C and HUD initialization 800B29F8
            // read the score at +68C; mode 3 displays the tally at +6A0.
            for(auto [name,offset]:{std::pair{"score",0x68Cu},std::pair{"mode_score",0x6A0u}}){
                const auto value=u2f(mem_read32(address+offset));
                if(std::isfinite(value) && std::abs(value)<=1000000000.f)entity.properties[name]=value;
            }
            // Native armor pickup 800C1A94 calls the setter 800B3C74;
            // it stores the authoritative armor amount at player+390.
            const auto armor=u2f(mem_read32(address+0x390));
            if(std::isfinite(armor) && armor>=0 && armor<=1000000.f)entity.properties["armor"]=armor;
            auto slot=mem_read32(address+0x17C);if(slot>=max_players)continue;
            if(slot>=ssne::native_party_capacity() || (mem_read32(ssne::native_party_registry()+slot*4)!=address && !ssne::native_party_spawn_waiting(address)))continue;
            entity.owner=state.owners[slot];entity.properties["player_slot"]=int64_t(slot);
            entity.properties["movement_mode"]=int64_t(mem_read32(address+0x340));
            entity.properties["camera_frozen"]=int64_t(mem_read32(address+0x370));
            // 800A6A8C copies the view matrix from the native camera at +24.
            // Replicate the host's prepared presentation state; replicas must
            // not run the world/player update to rebuild camera caches.
            const auto camera=mem_read32(address+0x368);
            if(mem_read32(0x80000000u)==0x47334250u && readable(camera,0x100) && mem_read32(camera)==0x80210880u && mem_read32(camera+4)==address) {
                std::array<std::array<float,4>,3> matrices;
                bool valid=true;
                for(unsigned row=0;row<3;++row) {
                    for(unsigned column=0;column<4;++column) {
                        auto& value=matrices[row][column];
                        value=u2f(mem_read32(camera+0x24+row*16+column*4));
                        valid&=std::isfinite(value) && std::abs(value)<=(column==3?1000000.f:10000.f);
                    }
                }
                // Newly constructed/waiting actors can have an unfinished
                // camera cache. Publish the whole pose atomically: one valid
                // row is not a usable camera and rejects the entire replica.
                if(valid) {
                    for(unsigned row=0;row<3;++row)entity.properties["camera_row"+std::to_string(row)]=matrices[row];
                    entity.properties["camera_mode"]=int64_t(mem_read32(camera+0xAC));
                    for(auto [name,offset]:{std::pair{"camera_bob",0x84u},std::pair{"camera_shake",0x88u}}) {
                        auto value=u2f(mem_read32(camera+offset));if(std::isfinite(value) && std::abs(value)<=10000.f)entity.properties[name]=value;
                    }
                }
            }
            std::array<float,3> view_angles;
            bool valid_angles=true;
            for(unsigned axis=0;axis<3;++axis){view_angles[axis]=u2f(mem_read32(address+0x2EC+axis*4));valid_angles&=std::isfinite(view_angles[axis]) && std::abs(view_angles[axis])<=10000.f;}
            if(valid_angles)entity.properties["view_angles"]=view_angles;
            for(auto [name,offset]:{std::pair{"yaw",0x290u},std::pair{"pitch",0x294u}}) {
                auto value=u2f(mem_read32(address+offset));if(std::isfinite(value))entity.properties[name]=value;
            }
            // Six original power-up slots: active, remaining time, untimed flag.
            // HUD handles at +0C belong to each process and must never cross peers.
            std::vector<float> powerups;
            for(unsigned i=0;i<6;++i){const auto at=address+0x61C+i*16;
                const bool active=mem_read32(at)!=0;
                powerups.push_back(float(active));powerups.push_back(active?u2f(mem_read32(at+4)):0.f);
                powerups.push_back(active?float(bool(mem_read32(at+8))):0.f);
            }
            entity.properties["powerups"]=std::move(powerups);
            entity.properties["combo_phase"]=int64_t(mem_read32(address+0x3AC));
            auto phase=mem_read32(address+0x3AC);auto timer=u2f(mem_read32(address+0x3B4));
            // +3AC/+3B4 belong to the original combo state, not respawning.
            if(!phase)entity.properties["combo_timer"]=0.f;
            else if(std::isfinite(timer) && std::abs(timer)<10000.f)entity.properties["combo_timer"]=timer;
            // 800C6170 reads availability at +10 within each 24-byte slot;
            // +0C is the active model count, not pickup ownership.
            std::vector<int64_t> owned,active;
            for(unsigned i=0;i<12;++i){owned.push_back(mem_read32(address+0x4BC+24*i+16));active.push_back(mem_read32(address+0x4BC+24*i+12));}
            std::vector<int64_t> available;
            // Native availability getter 800C6170 includes the host's all-weapons rule.
            const bool all_weapons=mem_read32(ssne::game_address(0x802DA280u))!=0;
            for(auto ownership:owned)available.push_back(ownership>0 || all_weapons);
            entity.properties["weapon_available"]=available;
            entity.properties["weapon_owned_counts"]=owned;
            entity.properties["weapon_active_counts"]=active;
            if(auto hidden=presentation_rules.values.find("PuzzleConfig.HideWeapons");hidden!=presentation_rules.values.end())entity.properties["weapons_hidden"]=hidden->second;
            const auto selected=mem_read32(address+0x5DC);
            entity.properties["selected_weapon"]=int64_t(selected);
            if(selected<12 && mem_read32(0x80000000u)==0x47334250u) {
                std::vector<int64_t> modes;
                if(!capture_native_weapon_modes(address,selected,modes,sample.error))return sample;
                entity.properties["weapon_modes"]=std::move(modes);
                entity.properties["weapon_model_slot"]=int64_t(selected);
                std::vector<int64_t> visibility(2,-1),view_masks(2,0);
                std::array<std::optional<PresentationMatrix>,2> camera_adjustments;
                for(unsigned index=0;index<2;++index) {
                    const auto instance=mem_read32(address+0x4BC+selected*24+index*4);
                    if(!readable(instance,0x228))continue;
                    const auto model=mem_read32(instance),vt=readable(model,0x40)?mem_read32(model):0;
                    if(!readable(vt,0x30) || mem_read32(vt+0x2C)!=0x8013E954u)continue;
                    const auto matrix=mem_read32(model+4);
                    if(!readable(matrix,48) || mem_read8(model+0x2D)>1)continue;
                    std::array<std::array<float,4>,3> rows{};bool valid=true;
                    for(unsigned row=0;row<3;++row)for(unsigned column=0;column<4;++column){rows[row][column]=u2f(mem_read32(matrix+row*16+column*4));valid&=std::isfinite(rows[row][column]) && std::abs(rows[row][column])<=1000000.f;}
                    if(!valid)continue;
                    // The original weapon root can be prepared before the
                    // current world camera. Rebase its visual snapshot from
                    // the exact camera returned to800A032C, without ticking
                    // the weapon again or changing any host RAM/gameplay.
                    if(!ssne::replica_simulation())if(const auto reference=ssne::native_weapon_camera_reference(instance)){
                        PresentationMatrix current,gun;
                        if(presentation_matrix(entity.properties,"camera_row",current)){
                            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)gun[r*4+c]=rows[r][c];
                            camera_adjustments[index]=matrix_product(current,matrix_rigid_inverse(*reference));
                            gun=matrix_product(*camera_adjustments[index],gun);
                            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)rows[r][c]=gun[r*4+c];
                        }
                    }
                    visibility[index]=mem_read8(model+0x2D);
                    view_masks[index]=mem_read32(model+0x30);
                    if(getenv("SSNE_DEV_STATIONARY_LOOK") && index==0 && mem_read32(address+0x17C)==1 && tick%30==0){
                        PresentationMatrix current,gun;for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)gun[r*4+c]=rows[r][c];
                        if(presentation_matrix(entity.properties,"camera_row",current)){const auto relative=matrix_product(matrix_rigid_inverse(current),gun);std::string message="[weapon-source-space] ref="+std::to_string(bool(camera_adjustments[index]))+" state="+std::to_string(mem_read32(instance+0x1B4))+" relative=";for(float v:relative)message+=std::to_string(v)+",";ssne::log_message(message);}
                    }
                    for(unsigned row=0;row<3;++row)entity.properties["weapon_model"+std::to_string(index)+"_row"+std::to_string(row)]=rows[row];
                    if(auto binding=native_weapon_pose_binding(address,selected,index);binding && binding->model) {
                        std::vector<float> bones;bool finite=true;
                        for(unsigned i=0;i<binding->count*12;++i){const auto f=u2f(mem_read32(binding->matrices+i*4));finite&=std::isfinite(f) && std::abs(f)<=10000.f;bones.push_back(f);}
                        if(finite){const auto prefix="weapon_model"+std::to_string(index)+"_bone_";
                            entity.properties[prefix+"count"]=int64_t(binding->count);
                            entity.properties[prefix+"layout"]=int64_t(binding->layout);
                            entity.properties[prefix+"matrices"]=std::move(bones);}
                    }
                }
                entity.properties["weapon_model_visibility"]=visibility;
                entity.properties["weapon_model_masks"]=view_masks;
                if(selected==11)for(unsigned hand=0;hand<2;++hand){
                    std::vector<float> visual;std::array<uint32_t,2> masks{};
                    if(!ssne::capture_native_bomb_visual(address,hand,visual,masks,sample.error))return sample;
                    const auto key="weapon_bomb"+std::to_string(hand);entity.properties[key]=std::move(visual);
                    entity.properties[key+"_masks"]=std::vector<int64_t>{masks[0],masks[1]};
                }
                for(unsigned hand=0;hand<2;++hand){
                    const auto body=mem_read32(address+0x148);
                    std::vector<float> flash(17,0.f);uint32_t mask=0;
                    if(readable(body,0xE8)){
                        const auto owner=body+0xC0+hand*0x14,proxy=mem_read32(owner+4);
                        const auto matrix=readable(proxy,0x38)?mem_read32(proxy+4):0;
                        if(mem_read32(owner+12) && readable(matrix,48)){
                            flash[0]=1;flash[1]=std::max(0.f,u2f(mem_read32(owner+8)));mask=mem_read32(proxy+0x30);
                            for(unsigned i=0;i<12;++i)flash[i+2]=u2f(mem_read32(matrix+i*4));
                            const auto resource=native_radial_flash_resource(0);
                            if(!resource){sample.error="Native body flash shape resource unavailable";return sample;}
                            for(unsigned i=0;i<3;++i)flash[14+i]=u2f(mem_read32(resource+0x18+i*4));
                        }
                    }
                    entity.properties["body_flash"+std::to_string(hand)]=std::move(flash);
                    entity.properties["body_flash"+std::to_string(hand)+"_mask"]=int64_t(mask);
                }
                for(unsigned effect=0;effect<3;++effect)if(const auto offset=native_weapon_flash_offset(selected,effect))for(unsigned hand=0;hand<2;++hand){
                    const auto instance=mem_read32(address+0x4BC+selected*24+hand*4);
                    std::vector<float> flash(17,0.f);uint32_t mask=0;
                    if(readable(instance,offset+16)){
                        const auto owner=instance+offset,proxy=mem_read32(owner+4);
                        const auto matrix=readable(proxy,0x38)?mem_read32(proxy+4):0;
                        if(mem_read32(owner+12) && readable(matrix,48)){
                            flash[0]=1;flash[1]=native_weapon_flash_timed(selected)?std::max(0.f,u2f(mem_read32(owner+8))):0.f;mask=mem_read32(proxy+0x30);
                            for(unsigned i=0;i<12;++i)flash[i+2]=u2f(mem_read32(matrix+i*4));
                            if(camera_adjustments[hand]){
                                PresentationMatrix pose;std::copy_n(flash.begin()+2,12,pose.begin());
                                pose=matrix_product(*camera_adjustments[hand],pose);
                                std::copy(pose.begin(),pose.end(),flash.begin()+2);
                            }
                            if(native_weapon_flash_radial(selected,effect)){
                                const auto resource=native_radial_flash_resource(selected==9?1:0);
                                if(!resource){sample.error="Native weapon flash shape resource unavailable";return sample;}
                                for(unsigned i=0;i<3;++i)flash[14+i]=u2f(mem_read32(resource+0x18+i*4));
                            }
                        }
                    }
                    const auto prefix=native_weapon_flash_key(hand,effect);
                    entity.properties[prefix]=std::move(flash);
                    entity.properties[prefix+"_mask"]=int64_t(mask);
                }
                if(selected==4)for(unsigned hand=0;hand<2;++hand){
                    const auto barrel=native_minigun_barrel(address,hand);
                    if(!barrel){sample.error="Invalid native minigun barrel binding";return sample;}
                    std::vector<float> values{float(barrel->model?mem_read8(barrel->model+0x2D):0)};
                    if(barrel->model)for(unsigned i=0;i<12;++i)values.push_back(u2f(mem_read32(barrel->matrix+i*4)));
                    if(barrel->model && camera_adjustments[hand]){PresentationMatrix pose;std::copy_n(values.begin()+1,12,pose.begin());pose=matrix_product(*camera_adjustments[hand],pose);std::copy(pose.begin(),pose.end(),values.begin()+1);}
                    entity.properties["weapon_model"+std::to_string(hand)+"_barrel"]=std::move(values);
                }
            }
            auto inventory=mem_read32(address+0x5E8);
            if(readable(inventory,0xDC)) {
                std::vector<int64_t> ammo,secondary,maximum;
                // Native refill 800B6698/8009F41C copies these eighteen u16
                // slots from +B4 to the two arrays at +0 and +24.
                for(unsigned i=0;i<18;++i){ammo.push_back(mem_read16(inventory+2*i));secondary.push_back(mem_read16(inventory+0x24+2*i));maximum.push_back(mem_read16(inventory+0xB4+2*i));}
                entity.properties["ammo"]=ammo;entity.properties["ammo_secondary"]=secondary;entity.properties["ammo_maximum"]=maximum;
            }
        }
        if(pickup) {
            if(!capture_native_pickup_metadata(address,entity,sample.error))return sample;
            // A coop pickup remains globally alive for other players. Its
            // per-view mask, changed by native collection 800C7F78, must follow
            // ownership even though the client does not run pickup updates.
            const auto binding=native_render_binding(address,entity.kind);
            entity.properties["pickup_view_mask"]=int64_t(binding && binding->model?mem_read32(binding->model+0x30):0);
            const auto glow=native_pickup_glow_proxy(address);
            if(!glow){sample.error="Unsupported native pickup glow proxy";return sample;}
            entity.properties["pickup_glow"]=std::vector<int64_t>{*glow?int64_t(mem_read8(*glow+0x2D)):0,*glow?int64_t(mem_read32(*glow+0x30)):0};
            const auto glow_binding=native_pickup_glow_binding(address);
            if(!glow_binding){sample.error="Unsupported native pickup glow matrix";return sample;}
            std::vector<float> glow_pose;
            if(glow_binding->model)for(unsigned i=0;i<12;++i){const auto v=u2f(mem_read32(glow_binding->matrix+i*4));if(!std::isfinite(v) || std::abs(v)>1000000.f){sample.error="Invalid native pickup glow pose";return sample;}glow_pose.push_back(v);}
            entity.properties["pickup_glow_pose"]=std::move(glow_pose);
            if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES") && entity.definition>60){static std::set<EntityId> traced;if(traced.insert(entity.id).second)ssne::log_message("[coop-special-pickup] id="+std::to_string(entity.id)+" definition="+std::to_string(entity.definition)+" object="+std::to_string(address)+" table="+std::to_string(record.vtable));}
            std::vector<int64_t> ownership;
            for(unsigned slot=0;slot<ssne::native_party_capacity();++slot)ownership.push_back(ssne::native_party_pickup_value(address,slot));entity.properties["pickup_ownership"]=ownership;
        }
        sample.entities.emplace(entity.id,std::move(entity));
    }
    // AI attack25EB8 uses controller+14 as def17's native source. This may be
    // an unmapped AI component rather than a replicated actor. Presentation
    // needs no local damage owner; never publish an unresolved logical source.
    if(!bindings_only)for(auto& [id,entity]:sample.entities)if(entity.kind==EntityKind::Projectile){auto& source=std::get<EntityReference>(entity.properties.at("projectile_source"));if(source.id && !sample.entities.contains(source.id))source.id=0;}
    return sample;
}
}
