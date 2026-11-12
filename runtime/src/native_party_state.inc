// Included by native_party.cpp so the checkpoint covers its external state.
namespace ssne {
namespace {
struct PartyCheckpoint {
    uint32_t game=0,ram=0,count=0,flags=0,occupied=0,connected=0;
    std::array<uint32_t,10> pointers{};
    std::array<uint32_t,8> profile{},actors{};
    std::array<std::array<uint32_t,11>,4> loadouts{};
    std::array<PadState,8> inputs{};
    std::map<uint32_t,ReservedBodyMask> bodies;
    std::map<uint32_t,uint32_t> pickups;
    std::map<uint32_t,unsigned> owners;
    std::map<uint32_t,PendingSpawn> pending;
    std::map<uint32_t,uint32_t> delays;
    std::map<uint32_t,std::array<float,3>> prepared;
    std::map<uint32_t,uint64_t> completed;
    uint64_t revision=0;
    std::optional<uint32_t> collider;
};
uint64_t party_state_hash(const uint8_t* p,size_t n){uint64_t h=14695981039346656037ull;for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}return h;}
struct PartyStateWriter {
    std::vector<uint8_t> bytes;
    void u32(uint32_t v){for(int shift=24;shift>=0;shift-=8)bytes.push_back(uint8_t(v>>shift));}
    void u64(uint64_t v){u32(uint32_t(v>>32));u32(uint32_t(v));}
};
struct PartyStateReader {
    const std::vector<uint8_t>& bytes;size_t pos=0;bool ok=true;
    uint32_t u32(){if(!ok || bytes.size()-pos<4){ok=false;return 0;}uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|bytes[pos++];return v;}
    uint64_t u64(){uint64_t hi=u32();return (hi<<32)|u32();}
    uint32_t count(uint32_t max){auto n=u32();if(n>max){ok=false;return 0;}return n;}
};
bool party_state_safe(std::string& error){
    if(owner!=-1 || probing_spawn || force_safe_spawn){error="Native party checkpoint requested inside a scoped game call";return false;}
    return true;
}
bool validate_party_checkpoint(const PartyCheckpoint& s,std::string& error){
    auto fail=[&](const char* message){error=message;return false;};
    if(s.game!=mem_read32(0x80000000u) || s.game!=0x47334250u || s.ram!=guest_ram_size() || s.count<1 || s.count>8 || s.flags>31 || s.occupied>255 || s.connected>255)return fail("Native party checkpoint configuration differs");
    const auto a=s.pointers[0],bank=s.pointers[1],second=s.pointers[2],status=s.pointers[3],reg=s.pointers[4],view=s.pointers[5],engine=s.pointers[6],pick=s.pointers[7],heap=s.pointers[8],size=s.pointers[9];
    if(a){
        if(s.count<=4 || !valid(a,0x10000) || a<0x80000018u || second!=a || status!=a+0x2000 || reg!=a+0x2040 || view!=a+0x2200 || pick<a+0x2D80 || pick>a+0x10000 || (pick-a-0x2D80)%32 || !valid(bank,0x1C30) || !valid(engine,0x600) || (heap && !valid(heap,4)) || mem_read32(a-0x18)!=size)return fail("Native party checkpoint arena binding differs");
    }else if(second || status || reg || view || pick || !s.pickups.empty() || !s.owners.empty())return fail("Native party checkpoint has storage without an arena");
    auto player=[&](uint32_t p){return valid(p,0x790) && mem_read32(p+0x48)==0x802121E8u && mem_read32(p+0x17C)<s.count;};
    for(unsigned slot=0;slot<8;++slot)if(s.actors[slot] && (!player(s.actors[slot]) || mem_read32(s.actors[slot]+0x17C)!=slot))return fail("Saved native player binding differs");
    for(const auto& [actor,b]:s.bodies)if(!player(actor) || !valid(b.proxy,0x20) || mem_read32(actor+0x148)!=b.proxy)return fail("Saved reserved player body differs");
    std::set<uint32_t> pickup_storage;
    for(const auto& [object,storage]:s.pickups)if(!valid(object,0x184) || storage<a+0x2D80 || storage>pick || pick-storage<32 || (storage-a-0x2D80)%32 || !pickup_storage.insert(storage).second)return fail("Invalid saved pickup ownership storage");
    for(const auto& [object,slot]:s.owners)if(!valid(object,4) || slot>=s.count)return fail("Invalid saved native view owner");
    for(const auto& [actor,p]:s.pending)if(!player(actor) || s.delays.at(actor)>1000000)return fail("Invalid saved pending respawn");
    for(const auto& [actor,point]:s.prepared){if(!player(actor))return fail("Invalid saved prepared respawn");for(float v:point)if(!std::isfinite(v) || std::abs(v)>1000000.f)return fail("Invalid saved respawn coordinates");}
    for(const auto& [actor,revision]:s.completed)if(!player(actor) || !revision || revision>s.revision)return fail("Invalid saved completed respawn");
    return true;
}
}
bool native_party_save_state(std::vector<uint8_t>& output,std::string& error){
    error.clear();if(!party_state_safe(error))return false;
    PartyStateWriter w;w.u32(0x4E504350);w.u32(1);w.u32(mem_read32(0x80000000u));w.u32(guest_ram_size());w.u32(roster);
    w.u32(unsigned(configured)|(unsigned(profiles_prepared)<<1)|(unsigned(extended_memory)<<2)|(unsigned(occupancy_enabled)<<3)|(unsigned(extra_loadouts_initialized)<<4));
    w.u32(occupied_slots);w.u32(connected_slots);
    for(auto v:{arena,original_bank,second_bank,second_status,registry,views,engine_base,pickup_next,arena_heap,arena_size})w.u32(v);
    for(auto v:profiles)w.u32(v);for(const auto& record:extra_loadouts)for(auto v:record)w.u32(v);
    for(auto v:known_player_actors)w.u32(v);
    for(const auto& p:pads){w.u32(p.buttons);w.u32(p.stick_x);w.u32(p.stick_y);w.u32(p.cstick_x);w.u32(p.cstick_y);w.u32(p.trigger_l);w.u32(p.trigger_r);}
    w.u64(next_spawn_revision);w.u32(test_collider_exit_restore.has_value());w.u32(test_collider_exit_restore.value_or(0));
    w.u32(reserved_body_masks.size());for(const auto& [actor,b]:reserved_body_masks){w.u32(actor);w.u32(b.proxy);w.u32(b.mask);}
    w.u32(pickup_flags.size());for(const auto& [object,storage]:pickup_flags){w.u32(object);w.u32(storage);}
    w.u32(view_owners.size());for(const auto& [object,slot]:view_owners){w.u32(object);w.u32(slot);}
    const auto now=std::chrono::steady_clock::now();
    w.u32(pending_spawns.size());for(const auto& [actor,p]:pending_spawns){w.u32(actor);w.u32(p.request);w.u32(p.flags);w.u32(p.attempts);w.u32(unsigned(p.local)|(unsigned(p.force_safe)<<1));const auto us=std::chrono::duration_cast<std::chrono::microseconds>(p.next-now).count();w.u32(uint32_t(std::clamp<int64_t>(us,0,1000000)));}
    w.u32(prepared_spawns.size());for(const auto& [actor,point]:prepared_spawns){w.u32(actor);for(float v:point)w.u32(f2u(v));}
    w.u32(completed_spawns.size());for(const auto& [actor,revision]:completed_spawns){w.u32(actor);w.u64(revision);}
    w.u64(party_state_hash(w.bytes.data(),w.bytes.size()));output=std::move(w.bytes);return true;
}
bool native_party_load_state(const std::vector<uint8_t>& bytes,std::string& error){
    error.clear();if(!party_state_safe(error))return false;
    auto fail=[&](const char* message){error=message;return false;};
    if(bytes.size()<584 || bytes.size()>1024*1024)return fail("Invalid native party checkpoint size");
    PartyStateReader checksum{bytes,bytes.size()-8};if(checksum.u64()!=party_state_hash(bytes.data(),bytes.size()-8))return fail("Native party checkpoint checksum differs");
    PartyStateReader r{bytes};if(r.u32()!=0x4E504350 || r.u32()!=1)return fail("Unsupported native party checkpoint");
    PartyCheckpoint s;s.game=r.u32();s.ram=r.u32();s.count=r.u32();s.flags=r.u32();s.occupied=r.u32();s.connected=r.u32();
    for(auto& v:s.pointers)v=r.u32();for(auto& v:s.profile)v=r.u32();for(auto& record:s.loadouts)for(auto& v:record)v=r.u32();for(auto& v:s.actors)v=r.u32();
    for(auto& p:s.inputs){const auto buttons=r.u32();if(buttons>65535)r.ok=false;p.buttons=uint16_t(buttons);for(auto field:{&p.stick_x,&p.stick_y,&p.cstick_x,&p.cstick_y,&p.trigger_l,&p.trigger_r}){const auto v=r.u32();if(v>255)r.ok=false;*field=uint8_t(v);}}
    s.revision=r.u64();const auto has_collider=r.u32(),collider=r.u32();if(has_collider>1)r.ok=false;if(has_collider)s.collider=collider;
    auto n=r.count(8);for(unsigned i=0;i<n;++i){const auto actor=r.u32(),proxy=r.u32(),mask=r.u32();if(!s.bodies.emplace(actor,ReservedBodyMask{proxy,mask}).second)r.ok=false;}
    n=r.count(1684);for(unsigned i=0;i<n;++i){const auto object=r.u32(),storage=r.u32();if(!s.pickups.emplace(object,storage).second)r.ok=false;}
    n=r.count(256);for(unsigned i=0;i<n;++i){const auto object=r.u32(),slot=r.u32();if(!s.owners.emplace(object,slot).second)r.ok=false;}
    n=r.count(8);for(unsigned i=0;i<n;++i){const auto actor=r.u32();PendingSpawn p;p.request=r.u32();p.flags=r.u32();p.attempts=r.u32();const auto flags=r.u32();if(flags>3)r.ok=false;p.local=flags&1;p.force_safe=flags&2;const auto delay=r.u32();if(!s.pending.emplace(actor,p).second)r.ok=false;s.delays[actor]=delay;}
    n=r.count(8);for(unsigned i=0;i<n;++i){const auto actor=r.u32();std::array<float,3> point;for(auto& v:point)v=u2f(r.u32());if(!s.prepared.emplace(actor,point).second)r.ok=false;}
    n=r.count(8);for(unsigned i=0;i<n;++i){const auto actor=r.u32();const auto revision=r.u64();if(!s.completed.emplace(actor,revision).second)r.ok=false;}
    if(!r.ok || r.pos!=bytes.size()-8)return fail("Malformed native party checkpoint");
    if(!validate_party_checkpoint(s,error))return false;
    const auto now=std::chrono::steady_clock::now();for(auto& [actor,p]:s.pending)p.next=now+std::chrono::microseconds(s.delays.at(actor));
    // Observer identity and installed hooks belong to this process. Reattach the
    // observer before publishing decoded state; never deserialize host pointers.
    ensure_pending_observer();
    roster=s.count;configured=s.flags&1;profiles_prepared=s.flags&2;extended_memory=s.flags&4;occupancy_enabled=s.flags&8;extra_loadouts_initialized=s.flags&16;
    occupied_slots=uint8_t(s.occupied);connected_slots=uint8_t(s.connected);
    arena=s.pointers[0];original_bank=s.pointers[1];second_bank=s.pointers[2];second_status=s.pointers[3];registry=s.pointers[4];views=s.pointers[5];engine_base=s.pointers[6];pickup_next=s.pointers[7];arena_heap=s.pointers[8];arena_size=s.pointers[9];
    profiles=s.profile;extra_loadouts=s.loadouts;known_player_actors=s.actors;pads=s.inputs;next_spawn_revision=s.revision;test_collider_exit_restore=s.collider;
    reserved_body_masks=std::move(s.bodies);pickup_flags=std::move(s.pickups);view_owners=std::move(s.owners);pending_spawns=std::move(s.pending);prepared_spawns=std::move(s.prepared);completed_spawns=std::move(s.completed);
    probe_success=false;probing_actor=0;probe_error.clear();return true;
}
}
