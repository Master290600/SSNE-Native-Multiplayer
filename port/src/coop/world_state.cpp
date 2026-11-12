#include "world_state.h"
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <algorithm>

namespace port::coop {
namespace {
constexpr size_t max_entities=32768,max_properties=256,max_mission_properties=4096;
// A bounded native-event batch contains up to 128 rows with several integer
// tags per row (flame particles use five). Keep encode/decode limits identical.
constexpr size_t max_integer_values=1024;
bool text_ok(const std::string& s,size_t limit,bool empty=true) {
    return (empty || !s.empty()) && s.size()<=limit &&
        std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32 || c==127;});
}
bool notice_text_ok(const std::string& text) {
    // Authored arena instructions share the pickup popup and contain line
    // breaks. They are display text, never identifiers or resource paths.
    return text.size()<=127 && std::none_of(text.begin(),text.end(),[](unsigned char c){return (c<32 && c!='\n' && c!='\r' && c!='\t') || c==127;});
}
bool properties_ok(const Properties& fields,size_t limit) {
    if(fields.size()>limit)return false;
    for(const auto& [name,p]:fields) {
        if(!text_ok(name,128,false))return false;
        bool ok=std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,float>)return std::isfinite(value);
            else if constexpr(std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<float,4>>)
                return std::all_of(value.begin(),value.end(),[](float f){return std::isfinite(f);});
            else if constexpr(std::is_same_v<T,std::string>)return name=="pickup_notice_text"?notice_text_ok(value):text_ok(value,512);
            else if constexpr(std::is_same_v<T,std::vector<int64_t>>)return value.size()<=(name=="netricsa_pages"?16384:max_integer_values);
            else if constexpr(std::is_same_v<T,std::vector<float>>)return value.size()<=4096 && std::all_of(value.begin(),value.end(),[](float f){return std::isfinite(f);});
            else return true;
        },p);
        if(!ok)return false;
    }
    return true;
}
PropertyPatch diff_properties(const Properties* baseline,const Properties& current) {
    PropertyPatch patch;
    for(const auto& [key,value]:current)
        if(!baseline || !baseline->contains(key) || baseline->at(key)!=value)patch.set.emplace(key,value);
    if(baseline)for(const auto& [key,value]:*baseline)if(!current.contains(key))patch.remove.push_back(key);
    return patch;
}
bool apply_properties(Properties& fields,const PropertyPatch& patch,size_t limit) {
    if(!properties_ok(patch.set,limit) || patch.remove.size()>limit)return false;
    std::set<std::string> removed;
    for(const auto& key:patch.remove) {
        if(!text_ok(key,128,false) || patch.set.contains(key) || !removed.insert(key).second)return false;
        fields.erase(key);
    }
    for(const auto& [key,value]:patch.set)fields[key]=value;
    return properties_ok(fields,limit);
}
struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t value){bytes.push_back(value);if(bytes.size()>max_frame_bytes)throw std::length_error("Coop frame too large");}
    void u32(uint32_t value){for(int i=3;i>=0;--i)u8(uint8_t(value>>(8*i)));}
    void u64(uint64_t value){u32(uint32_t(value>>32));u32(uint32_t(value));}
    void text(const std::string& value,size_t limit=512,bool notice=false){if(!(notice?notice_text_ok(value):text_ok(value,limit)))throw std::invalid_argument("Invalid coop string");u32(uint32_t(value.size()));for(auto c:value)u8(uint8_t(c));}
    void id(PlayerId value){u32(value.slot);u64(value.generation);}
    void property(const std::string& key,const Property& p) {
        u8(uint8_t(p.index()));
        std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,int64_t>)u64(std::bit_cast<uint64_t>(value));
            else if constexpr(std::is_same_v<T,float>)u32(std::bit_cast<uint32_t>(value));
            else if constexpr(std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<float,4>>)
                for(float f:value)u32(std::bit_cast<uint32_t>(f));
            else if constexpr(std::is_same_v<T,EntityReference>)u64(value.id);
            else if constexpr(std::is_same_v<T,std::string>)text(value,key=="pickup_notice_text"?127:512,key=="pickup_notice_text");
            else if constexpr(std::is_same_v<T,std::vector<float>>){u32(uint32_t(value.size()));for(auto f:value)u32(std::bit_cast<uint32_t>(f));}
            else {u32(uint32_t(value.size()));for(auto n:value)u64(std::bit_cast<uint64_t>(n));}
        },p);
    }
    void patch(const PropertyPatch& patch,size_t limit) {
        if(!properties_ok(patch.set,limit) || patch.remove.size()>limit)throw std::invalid_argument("Invalid coop properties");
        u32(uint32_t(patch.set.size()));for(const auto& [key,value]:patch.set){text(key,128);property(key,value);}
        u32(uint32_t(patch.remove.size()));for(const auto& key:patch.remove)text(key,128);
    }
};
struct Reader {
    const std::vector<uint8_t>& bytes;size_t position=0;bool ok=true;
    uint8_t u8(){if(position==bytes.size()){ok=false;return 0;}return bytes[position++];}
    uint32_t u32(){uint32_t n=0;for(unsigned i=0;i<4;++i)n=n*256+u8();return n;}
    uint64_t u64(){const auto high=u32(),low=u32();return uint64_t(high)<<32|low;}
    unsigned count(size_t limit){const auto n=u32();if(n>limit){ok=false;return 0;}return n;}
    std::string text(size_t limit=512,bool notice=false){auto n=count(limit);if(n>bytes.size()-position){ok=false;return {};}std::string s(bytes.begin()+position,bytes.begin()+position+n);position+=n;ok&=notice?notice_text_ok(s):text_ok(s,limit);return s;}
    PlayerId id(){auto slot=u32();auto generation=u64();return {slot,generation};}
    Property property(const std::string& key) {
        switch(u8()) {
        case 0:return std::bit_cast<int64_t>(u64());
        case 1:return std::bit_cast<float>(u32());
        case 2:{std::array<float,3> v;for(auto& f:v)f=std::bit_cast<float>(u32());return v;}
        case 3:{std::array<float,4> v;for(auto& f:v)f=std::bit_cast<float>(u32());return v;}
        case 4:return EntityReference{u64()};
        case 5:return text(key=="pickup_notice_text"?127:512,key=="pickup_notice_text");
        case 6:{std::vector<int64_t> v;auto n=count(key=="netricsa_pages"?16384:max_integer_values);for(unsigned i=0;i<n && ok;++i)v.push_back(std::bit_cast<int64_t>(u64()));return v;}
        case 7:{std::vector<float> v;auto n=count(4096);for(unsigned i=0;i<n && ok;++i)v.push_back(std::bit_cast<float>(u32()));return v;}
        default:ok=false;return int64_t(0);
        }
    }
    bool boolean(){const auto n=u8();if(n>1)ok=false;return n!=0;}
    PropertyPatch patch(size_t limit) {
        PropertyPatch p;auto n=count(limit);
        for(unsigned i=0;i<n && ok;++i){auto key=text(128);auto value=property(key);if(!p.set.emplace(key,std::move(value)).second)ok=false;}
        n=count(limit);for(unsigned i=0;i<n && ok;++i)p.remove.push_back(text(128));return p;
    }
};
}
bool validate_world(const WorldState& w,std::string& error) {
    auto fail=[&](const char* message){error=message;return false;};
    if(!w.epoch || !text_ok(w.level,256) || !properties_ok(w.mission,max_mission_properties))return fail("Invalid world metadata");
    for(const char* key:{"Session.Lobby.Open","Session.Lobby.ReadyMask"})if(auto field=w.mission.find(key);field!=w.mission.end()) {
        const auto value=std::get_if<int64_t>(&field->second);const auto limit=std::string_view(key)=="Session.Lobby.Open"?1:255;
        if(!value || *value<0 || *value>limit)return fail("Invalid lobby metadata");
    }
    if(auto scene=w.mission.find("Session.Presentation.Scene");scene!=w.mission.end()) {
        const auto value=std::get_if<int64_t>(&scene->second);auto phase=w.mission.find("Session.NativePhase");
        if(!value || (*value!=3 && *value!=5) || !w.entities.empty() || phase==w.mission.end() ||
           !std::holds_alternative<std::string>(phase->second) || std::get<std::string>(phase->second)!=(*value==5?"credits":"menu"))
            return fail("Invalid authoritative presentation scene");
    }
    bool has_results=false;for(const auto& [key,value]:w.mission)has_results|=key.starts_with("Session.Results.");
    if(has_results) {
        auto integer=[&](const char* key)->const int64_t*{auto found=w.mission.find(key);return found==w.mission.end()?nullptr:std::get_if<int64_t>(&found->second);};
        const auto active=integer("Session.Results.Active"),sequence=integer("Session.Results.Sequence"),score=integer("Session.Results.Score");
        auto level=w.mission.find("Session.Results.Level"),values=w.mission.find("Session.Results.Values");
        if(!active || *active!=1 || !sequence || *sequence<=0 || !score || *score<0 || uint64_t(*score)>UINT32_MAX || level==w.mission.end() || !std::holds_alternative<std::string>(level->second) || !text_ok(std::get<std::string>(level->second),64,false) || values==w.mission.end() || !std::holds_alternative<std::vector<float>>(values->second) || std::get<std::vector<float>>(values->second).size()!=13)return fail("Invalid authoritative results metadata");
    }
    if(w.entities.size()>max_entities)return fail("Too many entities");
    bool has_music=false;for(const auto& [key,value]:w.mission)has_music|=key.starts_with("Session.Music.");
    if(auto it=w.mission.find("Session.EnemyMultiplier");it!=w.mission.end()) {
        const auto value=std::get_if<int64_t>(&it->second);
        if(!value || (*value<1 || *value>5))return fail("Invalid enemy multiplier");
    }
    for(const char* key:{"Session.AmmoCapacity","Session.AmmoPickupMultiplier"})if(auto it=w.mission.find(key);it!=w.mission.end()) {
        const auto value=std::get_if<int64_t>(&it->second);
        if(!value || *value<(std::string_view(key)=="Session.AmmoCapacity"?0:1) || *value>5)return fail("Invalid ammunition rule");
    }
    bool has_speech=false;for(const auto& [key,value]:w.mission)has_speech|=key.starts_with("Session.Speech.");
    if(has_speech){
        auto integer=[&](const char* key)->const int64_t*{auto found=w.mission.find(key);return found==w.mission.end()?nullptr:std::get_if<int64_t>(&found->second);};
        const auto sequence=integer("Session.Speech.Sequence"),track=integer("Session.Speech.Track"),reference=integer("Session.Speech.Reference"),position=integer("Session.Speech.OutputFrames"),rate=integer("Session.Speech.OutputRate"),phase=integer("Session.Speech.Phase");
        if(!sequence || *sequence<=0 || !track || *track<0 || uint64_t(*track)>=UINT32_MAX || !reference || *reference<=0 || *reference>32767 || !position || *position<0 || *position>32000ll*3600 || !rate || *rate!=32000 || !phase || (*phase!=0 && *phase!=1) || (*phase==0 && *position))return fail("Invalid authoritative speech event");
    }
    if(has_music){
        auto integer=[&](const char* key)->const int64_t*{auto found=w.mission.find(key);return found==w.mission.end()?nullptr:std::get_if<int64_t>(&found->second);};
        const auto sequence=integer("Session.Music.Sequence"),track=integer("Session.Music.Track"),force=integer("Session.Music.Force");auto fade=w.mission.find("Session.Music.Fade");
        if(!sequence || *sequence<=0 || !track || *track<0 || uint64_t(*track)>=UINT32_MAX || !force || (*force!=0 && *force!=1) || fade==w.mission.end() || !std::holds_alternative<float>(fade->second) || std::get<float>(fade->second)<0 || std::get<float>(fade->second)>600)return fail("Invalid authoritative music request");
    }
    bool has_cutscene=false;for(const auto& [key,value]:w.mission)has_cutscene|=key.starts_with("Session.Cutscene.");
    if(has_cutscene){
        auto id=w.mission.find("Session.Cutscene.Id"),reference=w.mission.find("Session.Cutscene.Reference"),camera=w.mission.find("Session.Cutscene.Camera");
        if(id==w.mission.end() || reference==w.mission.end() || camera==w.mission.end())return fail("Incomplete native cutscene metadata");
        auto i=std::get_if<int64_t>(&id->second),r=std::get_if<int64_t>(&reference->second);auto c=std::get_if<std::vector<float>>(&camera->second);
        if(!i || *i<=0 || !r || *r<=0 || *r>32767 || !c || c->size()!=14 || (*c)[12]<=0 || (*c)[12]>=180 || (*c)[13]<0 || (*c)[13]>1024)return fail("Invalid native cutscene camera");
        if(w.vote.cutscene!=uint64_t(*i))return fail("Native cutscene vote identity differs");
    }
    for(const auto& [id,e]:w.entities) {
        if(!id || e.id!=id || unsigned(e.kind)>unsigned(EntityKind::Mission) || !text_ok(e.resource,256) ||
           !properties_ok(e.properties,max_properties)){
            error="Invalid entity state id="+std::to_string(id)+" kind="+std::to_string(unsigned(e.kind))+" definition="+std::to_string(e.definition)+" properties="+std::to_string(e.properties.size());
            for(const auto& [key,value]:e.properties)if(!properties_ok(Properties{{key,value}},1)){error+=" field="+key;break;}
            return false;
        }
        if(e.owner.slot>max_players || (e.owner.slot<max_players && !e.owner.generation))return fail("Invalid entity owner");
    }
    for(unsigned slot=0;slot<max_players;++slot) {
        const auto& p=w.players[slot];
        if(!p.connected){if(p.ready || p.afk || p.character)return fail("Disconnected participant has active state");continue;}
        if(p.id.slot!=slot || !p.id.generation || !text_ok(p.name,64,false) || (p.ready && !p.character))return fail("Invalid participant");
        if(p.ready && (!w.entities.contains(p.character) || w.entities.at(p.character).kind!=EntityKind::Player || w.entities.at(p.character).owner!=p.id))return fail("Ready participant has no matching character");
    }
    if(w.vote.received>w.vote.required || w.vote.required>max_players || (!w.vote.cutscene && (w.vote.received || w.vote.required || w.vote.skip_requested)))return fail("Invalid cutscene vote");
    return true;
}
WorldDelta difference(const WorldState* base,const WorldState& current) {
    std::string error;if(!validate_world(current,error))throw std::invalid_argument(error);
    WorldDelta d;d.epoch=current.epoch;d.revision=current.revision;d.tick=current.tick;
    d.full=!base || base->epoch!=current.epoch;
    if(d.full)base=nullptr;
    d.base_revision=base?base->revision:0;d.level=current.level;
    d.players=current.players;d.input_ack=current.input_ack;d.vote=current.vote;
    d.mission=diff_properties(base?&base->mission:nullptr,current.mission);
    for(const auto& [id,e]:current.entities) {
        const EntityState* before=base && base->entities.contains(id)?&base->entities.at(id):nullptr;
        if(before && *before==e)continue;
        EntityPatch patch;patch.id=id;patch.create=!before;
        patch.kind=e.kind;patch.definition=e.definition;patch.resource=e.resource;patch.owner=e.owner;
        patch.properties=diff_properties(before?&before->properties:nullptr,e.properties);
        d.entities.push_back(std::move(patch));
    }
    if(base)for(const auto& [id,e]:base->entities)if(!current.entities.contains(id))d.remove.push_back(id);
    return d;
}
std::optional<WorldState> apply_delta_copy(const WorldState& w,const WorldDelta& d,std::string& error) {
    auto fail=[&](const char* message)->std::optional<WorldState>{error=message;return std::nullopt;};
    if(!d.epoch || !d.revision || (d.full && d.base_revision) ||
       (!d.full && (d.epoch!=w.epoch || d.base_revision!=w.revision)) ||
       (d.epoch==w.epoch && d.revision<=w.revision) || d.epoch<w.epoch)return fail("Stale or missing world baseline");
    if(d.epoch==w.epoch && d.tick<w.tick)return fail("Host tick went backwards");
    if(d.entities.size()>max_entities || d.remove.size()>max_entities || (d.full && !d.remove.empty()))return fail("Invalid entity patch size");
    WorldState next=d.full?WorldState{}:w;
    next.epoch=d.epoch;next.revision=d.revision;next.tick=d.tick;next.level=d.level;
    next.players=d.players;next.input_ack=d.input_ack;next.vote=d.vote;
    if(!apply_properties(next.mission,d.mission,max_mission_properties))return fail("Invalid mission patch");
    std::set<EntityId> touched;
    for(auto id:d.remove){if(!id || !touched.insert(id).second || !next.entities.erase(id))return fail("Invalid entity removal");}
    for(const auto& patch:d.entities) {
        if(!patch.id || !touched.insert(patch.id).second || patch.create==next.entities.contains(patch.id))return fail("Invalid entity identity transition");
        auto& e=next.entities[patch.id];e.id=patch.id;e.kind=patch.kind;e.definition=patch.definition;e.resource=patch.resource;e.owner=patch.owner;
        if(!apply_properties(e.properties,patch.properties,max_properties))return fail("Invalid entity property patch");
    }
    if(!validate_world(next,error))return std::nullopt;
    return next;
}
bool apply_delta(WorldState& w,const WorldDelta& d,std::string& error) {
    auto next=apply_delta_copy(w,d,error);if(!next)return false;
    w=std::move(*next);return true;
}
std::vector<uint8_t> encode_delta(const WorldDelta& d) {
    Writer w;w.u64(d.epoch);w.u64(d.base_revision);w.u64(d.revision);w.u64(d.tick);w.u8(d.full);w.text(d.level,256);w.patch(d.mission,max_mission_properties);
    if(d.entities.size()>max_entities || d.remove.size()>max_entities)throw std::invalid_argument("Too many entity patches");
    w.u32(uint32_t(d.entities.size()));
    for(const auto& e:d.entities){w.u64(e.id);w.u8(e.create);w.u8(uint8_t(e.kind));w.u32(e.definition);w.text(e.resource,256);w.id(e.owner);w.patch(e.properties,max_properties);}
    w.u32(uint32_t(d.remove.size()));for(auto id:d.remove)w.u64(id);
    for(const auto& p:d.players){w.id(p.id);w.u8(p.connected);w.u8(p.ready);w.u8(p.afk);w.u64(p.character);w.text(p.name,64);}
    for(auto ack:d.input_ack)w.u64(ack);
    w.u64(d.vote.cutscene);w.u32(d.vote.received);w.u32(d.vote.required);w.u8(d.vote.skip_requested);return std::move(w.bytes);
}
bool decode_delta(const std::vector<uint8_t>& bytes,WorldDelta& output,std::string& error) {
    if(bytes.size()>max_frame_bytes){error="Coop frame too large";return false;}
    Reader r{bytes};WorldDelta d;d.epoch=r.u64();d.base_revision=r.u64();d.revision=r.u64();d.tick=r.u64();d.full=r.boolean();d.level=r.text(256);d.mission=r.patch(max_mission_properties);
    auto n=r.count(max_entities);for(unsigned i=0;i<n && r.ok;++i){EntityPatch p;p.id=r.u64();p.create=r.boolean();p.kind=EntityKind(r.u8());p.definition=r.u32();p.resource=r.text(256);p.owner=r.id();p.properties=r.patch(max_properties);d.entities.push_back(std::move(p));}
    n=r.count(max_entities);for(unsigned i=0;i<n && r.ok;++i)d.remove.push_back(r.u64());
    for(auto& p:d.players){p.id=r.id();p.connected=r.boolean();p.ready=r.boolean();p.afk=r.boolean();p.character=r.u64();p.name=r.text(64);}
    for(auto& ack:d.input_ack)ack=r.u64();
    d.vote.cutscene=r.u64();d.vote.received=r.u32();d.vote.required=r.u32();d.vote.skip_requested=r.boolean();
    if(!r.ok || r.position!=bytes.size()){error="Malformed coop world frame";return false;}
    output=std::move(d);return true;
}
}
