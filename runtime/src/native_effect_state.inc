// Included inside namespace ssne; never serializes callbacks or host addresses.
namespace {
struct EffectWriter {
    std::vector<uint8_t> bytes;bool ok=true;
    void word(uint32_t v){for(int s=24;s>=0;s-=8)bytes.push_back(uint8_t(v>>s));}
    void wide(uint64_t v){word(uint32_t(v>>32));word(uint32_t(v));}
    void real(float v){word(f2u(v));}
};
struct EffectReader {
    const std::vector<uint8_t>& bytes;size_t pos=0;bool ok=true;uint32_t version=0;
    void word(uint32_t& v){v=0;if(!ok || pos>bytes.size() || bytes.size()-pos<4){ok=false;return;}for(unsigned i=0;i<4;++i)v=(v<<8)|bytes[pos++];}
    void wide(uint64_t& v){uint32_t hi=0,lo=0;word(hi);word(lo);v=(uint64_t(hi)<<32)|lo;}
    void real(float& v){uint32_t bits=0;word(bits);v=u2f(bits);}
    uint32_t count(unsigned maximum){uint32_t n=0;word(n);if(n>maximum || n>(bytes.size()-pos)/4){ok=false;return 0;}return n;}
};
template<class A>void effect_fields(A& a,NativeConfirmedHit& e){for(auto p:{&e.sequence,&e.shot,&e.source,&e.target,&e.generation})a.wide(*p);a.word(e.slot);a.word(e.weapon);a.word(e.pellet);a.real(e.before);a.real(e.after);a.real(e.amount);}
template<class A>void effect_fields(A& a,NativeRocketEffect& e){a.wide(e.sequence);a.word(e.style);for(auto& v:e.values)a.real(v);}
template<class A>void effect_fields(A& a,NativeWeaponShot& e){a.wide(e.sequence);a.wide(e.order);a.word(e.slot);for(auto& v:e.position)a.real(v);a.word(e.weapon);a.word(e.cue_sound);if constexpr(std::is_same_v<A,EffectReader>){if(a.version>=5)a.word(e.cue_flags);}else a.word(e.cue_flags);}
template<class A>void effect_fields(A& a,NativeWeaponLoop& e){a.wide(e.sequence);a.wide(e.order);a.word(e.slot);a.word(e.lane);a.word(e.weapon);a.wide(e.origin);uint32_t active=e.active;a.word(active);if(active>1)a.ok=false;e.active=active!=0;a.word(e.duration_ms);a.word(e.elapsed_ms);for(auto& v:e.position)a.real(v);}
template<class A>void effect_fields(A& a,NativeBulletImpact& e){a.wide(e.sequence);for(auto& v:e.tags)a.word(v);for(auto& v:e.vectors)a.real(v);}
template<class A>void effect_fields(A& a,NativeFeedbackEvent& e){a.wide(e.sequence);a.word(e.kind);a.word(e.code);a.word(e.variant);if constexpr(std::is_same_v<A,EffectReader>){const auto count=a.version>=8?12u:8u;for(unsigned i=0;i<count;++i)a.real(e.values[i]);}else for(auto& v:e.values)a.real(v);}
template<class A>void effect_fields(A& a,NativeBulletTrail& e){a.wide(e.sequence);a.word(e.mask);for(auto& v:e.vectors)a.real(v);}
template<class A>void effect_fields(A& a,NativeWeaponEmitter& e){a.wide(e.sequence);for(auto& v:e.tags)a.word(v);for(auto& v:e.values)a.real(v);}
template<class A>void effect_fields(A& a,NativeWeaponCasing& e){a.wide(e.sequence);a.word(e.slot);a.word(e.weapon);a.word(e.type);a.word(e.mask);for(auto& v:e.values)a.real(v);}
template<class A>void effect_fields(A& a,NativeWeaponSurface& e){a.wide(e.sequence);a.word(e.weapon);a.word(e.kind);for(auto& v:e.values)a.real(v);}
template<class A>void effect_fields(A& a,NativeWeaponBeam& e){a.wide(e.sequence);a.word(e.slot);a.word(e.weapon);a.word(e.hand);a.word(e.flags);for(auto& v:e.values)a.real(v);}
template<class A>void effect_source(A& a,RocketSource& e){a.word(e.attack.actor);a.word(e.attack.slot);a.word(e.attack.weapon);a.word(e.attack.pellet);a.wide(e.attack.shot);a.wide(e.identity.entity);a.wide(e.identity.generation);}
uint64_t effect_hash(const std::vector<uint8_t>& b,size_t size){uint64_t h=14695981039346656037ull;for(size_t i=0;i<size;++i){h^=b[i];h*=1099511628211ull;}return h;}
bool effect_safe(std::string& error){
    if(presenting || attack_trace.actor || rocket_damage_trace || area_damage_trace){error="Effect checkpoint requested inside a native attack/presentation call";return false;}
    if(!replica_loops.empty() || !replica_cue_voices.empty() || !replica_trails.empty() || !replica_casings.empty() || !replica_surfaces.empty() || !replica_beams.empty()){error="Replica presentation state is rebuilt from the host, not loaded as a host checkpoint";return false;}
    return true;
}
struct EffectState {
    uint64_t epoch=0,order=0,ray=0;
    NativeProducerCursors tails{},accepted{};
    std::deque<NativeConfirmedHit> hits;
    std::deque<NativeRocketEffect> rockets;
    std::deque<NativeWeaponShot> shots;
    std::deque<NativeWeaponLoop> loops;
    std::deque<NativeBulletImpact> impacts;
    std::deque<NativeFeedbackEvent> feedback;
    std::deque<NativeBulletTrail> trails;
    std::deque<NativeWeaponEmitter> emitters;
    std::deque<NativeWeaponCasing> casings;
    std::deque<NativeWeaponSurface> surfaces;
    std::deque<NativeWeaponBeam> beams;
    std::map<uint32_t,HostLoop> active;
    std::map<uint32_t,uint64_t> weapons,cues;
    std::map<uint32_t,RocketSource> sources;
    std::map<uint32_t,QueuedRocket> queued;
};
template<class Q>void effect_write_queue(EffectWriter& w,const Q& q){w.word(uint32_t(q.size()));for(auto item:q)effect_fields(w,item);}
template<class T,class V>bool effect_read_queue(EffectReader& r,std::deque<T>& q,uint64_t tail,uint64_t accepted,V validate,std::string& error){
    const auto count=r.count(32768);uint64_t previous=0;
    for(unsigned i=0;i<count && r.ok;++i){T event;effect_fields(r,event);if(!r.ok || !event.sequence || event.sequence>tail || (i && event.sequence!=previous+1) || (!i && event.sequence>accepted+1) || !validate(event,error))return false;previous=event.sequence;q.push_back(event);}
    return r.ok && accepted<=tail && (tail?(!q.empty() && q.back().sequence==tail):q.empty());
}
bool effect_source_valid(const RocketSource& s,uint64_t tail){return readable(s.attack.actor,0x790) && s.attack.slot<8 && s.attack.weapon==6 && !s.attack.pellet && s.attack.shot && s.attack.shot<=tail && s.identity.entity && s.identity.generation;}
}
NativeEffectCheckpointStats native_effect_checkpoint_stats(){std::lock_guard lock(effect_mutex);return {shot_sequence,hit_sequence,rocket_sources.size(),queued_rockets.size(),host_loops.size()};}
bool native_effect_save_state(std::vector<uint8_t>& out,std::string& error){
    error.clear();std::lock_guard lock(effect_mutex);if(!effect_safe(error))return false;
    EffectWriter w;w.word(0x4E454350);w.word(8);w.word(mem_read32(0x80000000));w.word(guest_ram_size());w.word(authoritative?1:0);
    w.wide(producer_epoch);for(auto v:{hit_sequence,sequence,shot_sequence,loop_sequence,impact_sequence,feedback_sequence,trail_sequence,emitter_sequence,casing_sequence,surface_sequence,beam_sequence})w.wide(v);
    for(auto v:producer_accepted)w.wide(v);w.wide(audio_order);w.wide(automatic_ray_sequence);
    effect_write_queue(w,confirmed_hits);effect_write_queue(w,history);effect_write_queue(w,shots);effect_write_queue(w,loop_history);effect_write_queue(w,impacts);effect_write_queue(w,feedback_events);effect_write_queue(w,trails);effect_write_queue(w,emitters);effect_write_queue(w,casings);effect_write_queue(w,surfaces);effect_write_queue(w,beams);
    const auto now=std::chrono::steady_clock::now();
    w.word(uint32_t(host_loops.size()));for(auto [handle,loop]:host_loops){w.word(handle);effect_fields(w,loop.event);w.wide(uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::microseconds>(now-loop.began).count())));}
    w.word(uint32_t(weapon_shot_ids.size()));for(auto [address,id]:weapon_shot_ids){w.word(address);w.wide(id);}
    w.word(uint32_t(rocket_sources.size()));for(auto [address,source]:rocket_sources){w.word(address);effect_source(w,source);}
    w.word(uint32_t(queued_rockets.size()));for(auto [node,event]:queued_rockets){w.word(node);effect_source(w,event.source);w.word(event.victim);w.wide(event.target);for(auto v:event.words)w.word(v);w.word(readable(node,0x44) && queued_words(node)==event.words?1:0);}
    w.word(uint32_t(host_cue_voices.size()));for(auto [handle,id]:host_cue_voices){w.word(handle);w.wide(id);}
    if(w.bytes.size()>32*1024*1024 || !w.ok){error="Native effect checkpoint exceeds limit";return false;}
    w.wide(effect_hash(w.bytes,w.bytes.size()));out=std::move(w.bytes);return true;
}
bool native_effect_load_state(const std::vector<uint8_t>& bytes,std::string& error){
    error.clear();std::lock_guard lock(effect_mutex);if(!effect_safe(error))return false;
    auto fail=[&](const char* reason){error=reason;return false;};
    if(bytes.size()<168 || bytes.size()>32*1024*1024)return fail("Invalid native effect checkpoint size");
    EffectReader checksum{bytes,bytes.size()-8};uint64_t digest=0;checksum.wide(digest);if(digest!=effect_hash(bytes,bytes.size()-8))return fail("Native effect checkpoint checksum differs");
    EffectReader r{bytes};uint32_t magic=0,version=0,game=0,ram=0,host=0;r.word(magic);r.word(version);r.word(game);r.word(ram);r.word(host);
    if(magic!=0x4E454350 || (version<1 || version>8) || game!=mem_read32(0x80000000) || ram!=guest_ram_size() || host!=unsigned(authoritative.load()))return fail("Native effect checkpoint configuration differs");
    r.version=version;
    const unsigned channels=version>=7?11u:version>=6?9u:version>=4?7u:version>=3?6u:5u;
    EffectState s;r.wide(s.epoch);for(unsigned i=0;i<channels;++i)r.wide(s.tails[i]);for(unsigned i=0;i<channels;++i)r.wide(s.accepted[i]);r.wide(s.order);r.wide(s.ray);
    if(!s.epoch || s.order>INT64_MAX || s.ray>=native_automatic_ray_tag/2)return fail("Invalid native effect counters");
    for(unsigned i=0;i<s.tails.size();++i)if(s.tails[i]>INT64_MAX || s.accepted[i]>s.tails[i])return fail("Invalid native effect publication cursor");
    if(!effect_read_queue(r,s.hits,s.tails[0],s.accepted[0],validate_native_confirmed_hit,error) || !effect_read_queue(r,s.rockets,s.tails[1],s.accepted[1],validate_native_rocket_effect,error) || !effect_read_queue(r,s.shots,s.tails[2],s.accepted[2],validate_native_weapon_shot,error) || !effect_read_queue(r,s.loops,s.tails[3],s.accepted[3],validate_native_weapon_loop,error) || !effect_read_queue(r,s.impacts,s.tails[4],s.accepted[4],validate_native_bullet_impact,error))return fail("Invalid native effect event history");
    if(version>=3 && !effect_read_queue(r,s.feedback,s.tails[5],s.accepted[5],validate_native_feedback,error))return fail("Invalid feedback history");
    if(version>=4 && !effect_read_queue(r,s.trails,s.tails[6],s.accepted[6],validate_native_bullet_trail,error))return fail("Invalid bullet trail history");
    if(version>=6 && (!effect_read_queue(r,s.emitters,s.tails[7],s.accepted[7],validate_native_weapon_emitter,error) || !effect_read_queue(r,s.casings,s.tails[8],s.accepted[8],validate_native_weapon_casing,error)))return fail("Invalid weapon particle history");
    if(version>=7 && (!effect_read_queue(r,s.surfaces,s.tails[9],s.accepted[9],validate_native_weapon_surface,error) || !effect_read_queue(r,s.beams,s.tails[10],s.accepted[10],validate_native_weapon_beam,error)))return fail("Invalid weapon surface/beam history");
    std::set<uint64_t> orders;
    for(const auto& e:s.shots)if(!e.order || e.order>s.order || !orders.insert(e.order).second)return fail("Invalid native shot audio order");
    for(const auto& e:s.loops)if(!e.order || e.order>s.order || !orders.insert(e.order).second)return fail("Invalid native loop audio order");
    const auto now=std::chrono::steady_clock::now();
    auto count=r.count(48);std::set<unsigned> lanes;
    for(unsigned i=0;i<count && r.ok;++i){uint32_t handle=0;r.word(handle);HostLoop loop;effect_fields(r,loop.event);uint64_t elapsed=0;r.wide(elapsed);
        if(int32_t(handle)<0 || (handle&63)>=48 || !loop.event.active || loop.event.sequence>s.tails[3] || !loop.event.order || loop.event.order>s.order || elapsed>7ull*24*60*60*1000000 || !validate_native_weapon_loop(loop.event,error) || !lanes.insert(loop.event.slot*2+loop.event.lane).second)return fail("Invalid active native weapon loop");
        loop.began=now-std::chrono::microseconds(elapsed);if(!s.active.emplace(handle,loop).second)return fail("Duplicate native audio handle");}
    count=r.count(65536);for(unsigned i=0;i<count && r.ok;++i){uint32_t address=0;uint64_t id=0;r.word(address);r.wide(id);if(!readable(address,0x1D8) || !id || id>s.tails[2] || !s.weapons.emplace(address,id).second)return fail("Invalid native weapon-shot mapping");}
    count=r.count(65536);for(unsigned i=0;i<count && r.ok;++i){uint32_t address=0;r.word(address);RocketSource source;effect_source(r,source);if(!effect_source_valid(source,s.tails[2]) || !readable(address,0x164) || mem_read32(address+0x48)!=0x802022E0 || mem_read32(address+0xC8)!=source.attack.actor || !s.sources.emplace(address,source).second)return fail("Saved rocket source does not match restored RAM");}
    count=r.count(65536);for(unsigned i=0;i<count && r.ok;++i){uint32_t node=0,matching=0;r.word(node);QueuedRocket q;effect_source(r,q.source);r.word(q.victim);r.wide(q.target);for(auto& v:q.words)r.word(v);r.word(matching);
        if(!effect_source_valid(q.source,s.tails[2]) || !readable(node,0x44) || !readable(q.victim,0x4C) || !q.target || matching>1 || bool(matching)!=(queued_words(node)==q.words) || !s.queued.emplace(node,q).second)return fail("Saved queued rocket damage does not match restored RAM");}
    if(version>=2){count=r.count(48);for(unsigned i=0;i<count && r.ok;++i){uint32_t handle=0;uint64_t id=0;r.word(handle);r.wide(id);if(int32_t(handle)<0 || (handle&63u)>=48 || !id || id>s.tails[2] || !s.cues.emplace(handle,id).second)return fail("Invalid saved weapon cue lifetime");}}
    if(!r.ok || r.pos!=bytes.size()-8)return fail("Truncated or trailing native effect checkpoint data");
    producer_epoch=s.epoch;producer_accepted=s.accepted;hit_sequence=s.tails[0];sequence=s.tails[1];shot_sequence=s.tails[2];loop_sequence=s.tails[3];impact_sequence=s.tails[4];feedback_sequence=s.tails[5];trail_sequence=s.tails[6];emitter_sequence=s.tails[7];casing_sequence=s.tails[8];surface_sequence=s.tails[9];beam_sequence=s.tails[10];audio_order=s.order;automatic_ray_sequence=s.ray;
    confirmed_hits.swap(s.hits);history.swap(s.rockets);shots.swap(s.shots);loop_history.swap(s.loops);impacts.swap(s.impacts);feedback_events.swap(s.feedback);trails.swap(s.trails);emitters.swap(s.emitters);casings.swap(s.casings);surfaces.swap(s.surfaces);beams.swap(s.beams);host_loops.swap(s.active);host_cue_voices.swap(s.cues);weapon_shot_ids.swap(s.weapons);rocket_sources.swap(s.sources);queued_rockets.swap(s.queued);
    replica_trail_frame.reset();replica_bomb_frame.reset();replica_beam_frames.clear();replica_beams.clear();replica_surfaces.clear();
    return true;
}

bool native_effect_checkpoint_probe(std::string& error){
    if(!getenv("SSNE_TEST_HIDDEN") || !getenv("SSNE_TEST_EFFECT_CHECKPOINT")){error="Effect checkpoint probe is not enabled";return false;}
    std::vector<uint8_t> saved;if(!native_effect_save_state(saved,error))return false;
    auto word=[](const auto& b,size_t at){uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|b.at(at+i);return v;};
    auto canonical=[&](std::vector<uint8_t> b){size_t at=220;for(unsigned stride:{64u,52u,44u,60u,64u,52u,36u,96u,100u,44u,76u}){const auto n=word(b,at);at+=4+n*stride;}const auto count=word(b,at);at+=4;for(unsigned i=0;i<count;++i,at+=72)std::fill_n(b.begin()+at+64,8,0);b.resize(b.size()-8);return b;};
    auto sign=[](auto& b){const auto h=effect_hash(b,b.size()-8);for(unsigned i=0;i<8;++i)b[b.size()-8+i]=uint8_t(h>>(56-i*8));};
    const auto baseline=canonical(saved);unsigned rejected=0;
    auto reject=[&](std::vector<uint8_t> bad){std::string why;if(native_effect_load_state(bad,why) || why.empty())return false;std::vector<uint8_t> after;if(!native_effect_save_state(after,why) || canonical(after)!=baseline)return false;++rejected;return true;};
    auto damaged=saved;damaged.pop_back();if(!reject(damaged)){error="Truncated effect state changed live state";return false;}
    for(unsigned at:{0u,8u,12u,16u,116u,124u,212u,220u}){auto bad=saved;std::fill_n(bad.begin()+at,4,0xFF);sign(bad);if(!reject(bad)){error="Malformed effect state was accepted";return false;}}
    size_t at=220;for(unsigned stride:{64u,52u,44u,60u,64u,52u,36u,96u,100u,44u,76u}){const auto n=word(saved,at);at+=4+n*stride;}const auto loops=word(saved,at);at+=4+loops*72;
    const auto weapons=word(saved,at);at+=4+weapons*12;
    const auto rockets=word(saved,at);at+=4;
    if(rockets){auto bad=saved;std::fill_n(bad.begin()+at,4,0);sign(bad);if(!reject(bad)){error="Invalid restored projectile binding accepted";return false;}}
    // A valid changed counter proves restoration actually replaces external
    // state; restore it before any simulation or event publication resumes.
    auto changed=saved;uint64_t order=(uint64_t(word(saved,204))<<32)|word(saved,208);++order;
    for(unsigned i=0;i<8;++i)changed[204+i]=uint8_t(order>>(56-i*8));sign(changed);
    if(!native_effect_load_state(changed,error))return false;
    std::vector<uint8_t> after;if(!native_effect_save_state(after,error) || canonical(after)==baseline){error="Effect counter positive control did not change";return false;}
    if(!native_effect_load_state(saved,error) || !native_effect_save_state(after,error) || canonical(after)!=baseline){error="Effect checkpoint did not restore original state";return false;}
    const auto stats=native_effect_checkpoint_stats();
    std::ofstream data("native-effect-checkpoint.bin",std::ios::binary);data.write(reinterpret_cast<const char*>(saved.data()),saved.size());
    std::ofstream report("native-effect-checkpoint.json");report<<"{\"rejected\":"<<rejected<<",\"shots\":"<<stats.shots<<",\"hits\":"<<stats.hits<<",\"active_rockets\":"<<stats.rockets<<",\"queued_damage\":"<<stats.queued<<",\"active_loops\":"<<stats.loops<<",\"positive_counter_restore\":true}\n";
    if(!data || !report){error="Cannot write effect checkpoint evidence";return false;}return true;
}
