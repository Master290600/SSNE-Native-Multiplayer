#pragma once
#include "world_state.h"
#include <deque>
#include <span>
#include <stdexcept>
#include <cmath>
#include <algorithm>

namespace port::coop {
inline constexpr unsigned native_event_channels=12;
using NativeEventCursors=std::array<uint64_t,native_event_channels>;
// Compact immutable rows; native producers publish their recent window, while
// transport retains rows until each actual game consumer acknowledges them.
class NativeEventJournal {
    struct Field {const char* name;unsigned width;bool floating=false;};
    struct Schema {const char* prefix;std::vector<Field> fields;};
    struct Row {uint64_t id;std::vector<int64_t> integers;std::vector<float> floats;};
    std::array<std::deque<Row>,native_event_channels> rows_;
    NativeEventCursors tails_{};
    uint64_t epoch_=0;
    bool active_=false;
    static const std::array<Schema,native_event_channels>& schemas() {
        static const std::array<Schema,native_event_channels> value={{
            {"Session.ConfirmedHits.",{{"Ids",1},{"Shots",1},{"Sources",1},{"Targets",1},{"Generations",1},{"Codes",1},{"Pellets",1},{"Values",3,true}}},
            {"Session.RocketEffects.",{{"Ids",1},{"Styles",1},{"Values",10,true}}},
            {"Session.WeaponShots.",{{"Ids",2},{"Weapons",1},{"Cues",1},{"Orders",1},{"Flags",1},{"Positions",3,true}}},
            {"Session.WeaponLoops.",{{"Ids",1},{"Codes",1},{"Durations",1},{"Origins",1},{"Orders",1},{"Elapsed",1},{"Positions",3,true}}},
            {"Session.BulletImpacts.",{{"Ids",1},{"Tags",1},{"Vectors",9,true}}},
            {"Session.Feedback.",{{"Ids",1},{"Codes",3},{"Values",12,true}}},
            {"Session.BulletTrails.",{{"Ids",1},{"Masks",1},{"Vectors",19,true}}},
            {"Session.WeaponEmitters.",{{"Ids",1},{"Tags",5},{"Values",17,true}}},
            {"Session.WeaponCasings.",{{"Ids",1},{"Tags",4},{"Values",19,true}}},
            {"Session.ProjectileVisuals.",{{"Ids",1},{"Tags",4},{"Values",30,true}}},
            {"Session.WeaponSurfaces.",{{"Ids",1},{"Tags",2},{"Values",7,true}}},
            {"Session.WeaponBeams.",{{"Ids",1},{"Tags",4},{"Values",13,true}}}
        }};return value;
    }
public:
    static constexpr size_t batch_size=128,max_retained=8192;
    bool active()const{return active_;}
    uint64_t epoch()const{return epoch_;}
    NativeEventCursors tails()const{return tails_;}
    // Saved transient events precede the new session baseline. Persistent
    // objects/active loops are included in the world snapshot independently.
    void resume(uint64_t epoch,const NativeEventCursors& tails) {
        if(!epoch || std::any_of(tails.begin(),tails.end(),[](auto n){return n>uint64_t(INT64_MAX);}))throw std::runtime_error("Invalid native event resume baseline");
        epoch_=epoch;tails_=tails;rows_={};active_=true;
    }
    void ingest(const WorldState& world,std::span<const WorldState> earlier={}) {
        const bool reset=epoch_!=world.epoch;auto new_tails=reset?NativeEventCursors{}:tails_;std::array<std::deque<Row>,native_event_channels> additions;bool present=false;
        auto stage=[&](const WorldState& packet){
        if(packet.epoch!=world.epoch)throw std::runtime_error("Native producer batch crossed a level epoch");
        for(unsigned channel=0;channel<native_event_channels;++channel) {
            const auto& schema=schemas()[channel];
            auto ids=packet.mission.find(std::string(schema.prefix)+"Ids");
            if(ids==packet.mission.end())continue;
            present=true;
            const auto* values=std::get_if<std::vector<int64_t>>(&ids->second);
            if(!values || values->size()%schema.fields.front().width)throw std::runtime_error("Invalid native event identity shape");
            const auto count=values->size()/schema.fields.front().width;
            if(count>32768)throw std::runtime_error("Native producer batch is unbounded");
            for(const auto& field:schema.fields) {
                auto found=packet.mission.find(std::string(schema.prefix)+field.name);
                if(found==packet.mission.end())throw std::runtime_error("Incomplete native event producer");
                if(field.floating){const auto* data=std::get_if<std::vector<float>>(&found->second);if(!data || data->size()!=count*field.width)throw std::runtime_error("Invalid native event float shape");for(auto value:*data)if(!std::isfinite(value) || std::abs(value)>1000000.f)throw std::runtime_error("Invalid native event coefficient");}
                else {const auto* data=std::get_if<std::vector<int64_t>>(&found->second);if(!data || data->size()!=count*field.width)throw std::runtime_error("Invalid native event integer shape");}
            }
            uint64_t previous=0;
            for(size_t i=0;i<count;++i) {
                const auto signed_id=(*values)[i*schema.fields.front().width];
                if(signed_id<=0 || (previous && uint64_t(signed_id)!=previous+1))throw std::runtime_error("Invalid native event producer order");
                const auto id=uint64_t(signed_id);previous=id;
                if(id<=new_tails[channel])continue;
                if(id!=new_tails[channel]+1)throw std::runtime_error("Native event producer lost unjournaled events");
                Row row{id,{},{}};
                for(const auto& field:schema.fields) {
                    const auto& data=packet.mission.at(std::string(schema.prefix)+field.name);
                    if(field.floating){const auto& v=std::get<std::vector<float>>(data);row.floats.insert(row.floats.end(),v.begin()+i*field.width,v.begin()+(i+1)*field.width);}
                    else {const auto& v=std::get<std::vector<int64_t>>(data);row.integers.insert(row.integers.end(),v.begin()+i*field.width,v.begin()+(i+1)*field.width);}
                }
                additions[channel].push_back(std::move(row));new_tails[channel]=id;
            }
        }
        };
        for(const auto& packet:earlier)stage(packet);
        stage(world);
        if(reset){epoch_=world.epoch;tails_={};rows_={};active_=false;}
        active_|=present;tails_=new_tails;
        for(unsigned i=0;i<native_event_channels;++i)for(auto& row:additions[i])rows_[i].push_back(std::move(row));
    }
    // Internal producer bursts are ingested atomically before publishing one
    // bounded world. This does not increase world publication frequency.
    static std::vector<WorldState> producer_batches(WorldState& world) {
        std::array<size_t,native_event_channels> counts{};size_t maximum=0;
        for(unsigned channel=0;channel<native_event_channels;++channel){const auto& schema=schemas()[channel];auto ids=world.mission.find(std::string(schema.prefix)+"Ids");if(ids==world.mission.end())continue;
            const auto* v=std::get_if<std::vector<int64_t>>(&ids->second);if(!v || v->size()%schema.fields.front().width)throw std::runtime_error("Invalid native producer shape");
            const auto count=counts[channel]=v->size()/schema.fields.front().width;if(count>32768)throw std::runtime_error("Native producer exceeded retained capacity");maximum=std::max(maximum,count);
            for(const auto& field:schema.fields){auto found=world.mission.find(std::string(schema.prefix)+field.name);if(found==world.mission.end())throw std::runtime_error("Incomplete native producer");
                if(field.floating){auto data=std::get_if<std::vector<float>>(&found->second);if(!data || data->size()!=count*field.width)throw std::runtime_error("Invalid native producer float shape");}
                else {auto data=std::get_if<std::vector<int64_t>>(&found->second);if(!data || data->size()!=count*field.width)throw std::runtime_error("Invalid native producer integer shape");}
            }
        }
        if(maximum<=batch_size)return {};
        std::vector<WorldState> batches((maximum+batch_size-1)/batch_size);
        for(size_t batch=0;batch<batches.size();++batch){auto& packet=batches[batch];packet.epoch=world.epoch;packet.tick=world.tick;
            for(unsigned channel=0;channel<native_event_channels;++channel){const auto& schema=schemas()[channel];if(!world.mission.contains(std::string(schema.prefix)+"Ids"))continue;
                const auto begin=std::min(batch*batch_size,counts[channel]),end=std::min(begin+batch_size,counts[channel]);
                for(const auto& field:schema.fields){const auto key=std::string(schema.prefix)+field.name;
                    if(field.floating){const auto& data=std::get<std::vector<float>>(world.mission.at(key));packet.mission[key]=std::vector<float>(data.begin()+begin*field.width,data.begin()+end*field.width);}
                    else {const auto& data=std::get<std::vector<int64_t>>(world.mission.at(key));packet.mission[key]=std::vector<int64_t>(data.begin()+begin*field.width,data.begin()+end*field.width);}
                }
            }
        }
        for(unsigned channel=0;channel<native_event_channels;++channel)if(counts[channel]>batch_size){const auto& schema=schemas()[channel];
            for(const auto& field:schema.fields){auto& value=world.mission.at(std::string(schema.prefix)+field.name);const auto first=(counts[channel]-batch_size)*field.width;
                if(field.floating){auto& data=std::get<std::vector<float>>(value);data.erase(data.begin(),data.begin()+first);}
                else {auto& data=std::get<std::vector<int64_t>>(value);data.erase(data.begin(),data.begin()+first);}
            }
        }
        return batches;
    }
    bool available(const NativeEventCursors& cursor)const {
        for(unsigned i=0;i<native_event_channels;++i)
            if(!rows_[i].empty() && cursor[i]<tails_[i] && cursor[i]+1<rows_[i].front().id)return false;
        return true;
    }
    void prune(const NativeEventCursors& consumed) {
        for(unsigned i=0;i<native_event_channels;++i)
            while(rows_[i].size()>batch_size && (rows_[i].front().id<=consumed[i] || rows_[i].size()>max_retained))rows_[i].pop_front();
    }
    void decorate(WorldState& world,const NativeEventCursors& consumed,bool initialized)const {
        if(!active_ || world.epoch!=epoch_)return;
        world.mission["Session.NativeEventTails"]=std::vector<int64_t>(tails_.begin(),tails_.end());
        std::array<std::vector<const Row*>,native_event_channels> batches;
        for(unsigned i=0;i<native_event_channels;++i) {
            const auto& rows=rows_[i];
            if(!initialized){for(size_t j=rows.size()>batch_size?rows.size()-batch_size:0;j<rows.size();++j)batches[i].push_back(&rows[j]);}
            else for(const auto& row:rows)if(row.id>consumed[i]){if(batches[i].size()==batch_size)break;batches[i].push_back(&row);}
        }
        // Shots and loops share audio order. A partial batch must not deliver a
        // later loop before an omitted earlier shot (or the converse).
        if(initialized) {
            uint64_t cutoff=UINT64_MAX;
            for(unsigned i:{2u,3u}) {
                size_t n=0;for(const auto& row:rows_[i])if(row.id>consumed[i]) {
                    if(n++==batch_size){cutoff=std::min(cutoff,uint64_t(row.integers[4]));break;}
                }
            }
            for(unsigned i:{2u,3u})while(!batches[i].empty() && uint64_t(batches[i].back()->integers[4])>=cutoff)batches[i].pop_back();
        }
        for(unsigned i=0;i<native_event_channels;++i) {
            const auto& schema=schemas()[i];size_t integer_offset=0,float_offset=0;
            for(const auto& field:schema.fields) {
                const auto key=std::string(schema.prefix)+field.name;
                if(field.floating){std::vector<float> v;for(auto* row:batches[i])v.insert(v.end(),row->floats.begin()+float_offset,row->floats.begin()+float_offset+field.width);world.mission[key]=std::move(v);float_offset+=field.width;}
                else {std::vector<int64_t> v;for(auto* row:batches[i])v.insert(v.end(),row->integers.begin()+integer_offset,row->integers.begin()+integer_offset+field.width);world.mission[key]=std::move(v);integer_offset+=field.width;}
            }
        }
    }
};
}
