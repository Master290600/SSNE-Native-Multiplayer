#pragma once
#include "world_state.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace port::coop {
// Presentation only. None of these values are sent back as gameplay state.
struct PredictedLook { double yaw=0,pitch=0; };
class InputLookHistory {
    static constexpr double turn=6.283185307179586;
    struct Entry {uint64_t sequence;int32_t yaw,pitch;};
    // Inputs arriving before the first usable camera pose are rebased at its acknowledgement.
    std::deque<Entry> initial_inputs_;
    uint64_t epoch_=0,last_local_input_=0;
    double yaw_=0,pitch_=0,pitch_low_=0,pitch_high_=0,scale_=1,last_sample_=0;
    bool initialized_=false,moved_=false;

    void move(const Entry& input){
        yaw_=std::remainder(yaw_+input.yaw*scale_/1000000.,turn);
        pitch_=std::clamp(pitch_-input.pitch*scale_/1000000.,pitch_low_,pitch_high_);
        last_local_input_=input.sequence;
        moved_=true;
    }
public:
    void reset(uint64_t epoch){
        initial_inputs_.clear();
        epoch_=epoch;
        last_local_input_=0;
        initialized_=moved_=false;
    }
    void append(uint64_t epoch,uint64_t sequence,int32_t yaw,int32_t pitch){
        if(epoch!=epoch_)reset(epoch);
        if(!yaw && !pitch)return;
        if(initialized_)move({sequence,yaw,pitch});
        else {
            if(initial_inputs_.size()==2048)initial_inputs_.pop_front();
            initial_inputs_.push_back({sequence,yaw,pitch});
        }
    }
    PredictedLook sample(uint64_t epoch,uint64_t ack,double now,const std::array<float,3>& host_angles,
                         float pitch_low,float pitch_high,double scale=1.){
        if(epoch!=epoch_)return {};
        pitch_low_=pitch_low;
        pitch_high_=pitch_high;
        scale_=scale;
        const double target_yaw=host_angles[0],target_pitch=std::clamp(double(host_angles[1]),pitch_low_,pitch_high_);
        if(!initialized_){
            yaw_=target_yaw;
            pitch_=target_pitch;
            last_sample_=now;
            initialized_=true;
            for(const auto& input:initial_inputs_)if(input.sequence>ack)move(input);
            initial_inputs_.clear();
        }
        const auto elapsed=std::clamp(now-last_sample_,0.,.05);
        last_sample_=now;
        pitch_=std::clamp(pitch_,pitch_low_,pitch_high_);
        if(!last_local_input_){
            yaw_=target_yaw;
            pitch_=target_pitch;
        }else if(!moved_ && ack>=last_local_input_){
            // Acknowledged input may have been clamped or merged by the host.
            // Correct the remaining visual difference without stepping the camera.
            const auto yaw_error=std::remainder(target_yaw-yaw_,turn);
            const auto pitch_error=target_pitch-pitch_;
            if(std::abs(yaw_error)<.00001 && std::abs(pitch_error)<.00001){
                yaw_=target_yaw;
                pitch_=target_pitch;
                last_local_input_=0;
            }else {
                const auto blend=-std::expm1(-elapsed/.14);
                yaw_=std::remainder(yaw_+yaw_error*blend,turn);
                pitch_+=pitch_error*blend;
            }
        }
        moved_=false;
        if(!last_local_input_)return {};
        return {std::remainder(yaw_-target_yaw,turn),double(host_angles[1])-pitch_};
    }
};
using PresentationMatrix=std::array<float,12>;
inline PresentationMatrix matrix_identity(){return {1,0,0,0,0,1,0,0,0,0,1,0};}
inline PresentationMatrix matrix_product(const PresentationMatrix& a,const PresentationMatrix& b){
    PresentationMatrix out{};for(unsigned r=0;r<3;++r){for(unsigned c=0;c<3;++c)for(unsigned k=0;k<3;++k)out[r*4+c]+=a[r*4+k]*b[k*4+c];out[r*4+3]=a[r*4+3];for(unsigned k=0;k<3;++k)out[r*4+3]+=a[r*4+k]*b[k*4+3];}return out;
}
inline PresentationMatrix matrix_rigid_inverse(const PresentationMatrix& m){
    auto out=matrix_identity();for(unsigned r=0;r<3;++r){for(unsigned c=0;c<3;++c)out[r*4+c]=m[c*4+r];out[r*4+3]=0;for(unsigned k=0;k<3;++k)out[r*4+3]-=out[r*4+k]*m[k*4+3];}return out;
}
inline bool presentation_matrix(const Properties& p,const std::string& prefix,PresentationMatrix& out){
    for(unsigned r=0;r<3;++r){const auto it=p.find(prefix+std::to_string(r));if(it==p.end())return false;const auto* row=std::get_if<std::array<float,4>>(&it->second);if(!row)return false;for(unsigned c=0;c<4;++c){if(!std::isfinite((*row)[c]))return false;out[r*4+c]=(*row)[c];}}return true;
}
inline void put_presentation_matrix(Properties& p,const std::string& prefix,const PresentationMatrix& m){
    for(unsigned r=0;r<3;++r)p[prefix+std::to_string(r)]=std::array<float,4>{m[r*4],m[r*4+1],m[r*4+2],m[r*4+3]};
}
// The final world camera from PAL 800A5EDC is Ry(yaw)*Rx(pitch)*Rz(roll).
// Its transposed intermediate is the view matrix, not camera+24. Preserve bob/shake.
inline PresentationMatrix native_camera_rotation(const std::array<float,3>& angles){
    auto x=matrix_identity(),y=x,z=x;
    const float sx=std::sin(angles[1]),cx=std::cos(angles[1]),sy=std::sin(angles[0]),cy=std::cos(angles[0]),sz=std::sin(angles[2]),cz=std::cos(angles[2]);
    x[5]=cx;x[6]=-sx;x[9]=sx;x[10]=cx;y[0]=cy;y[2]=sy;y[8]=-sy;y[10]=cy;z[0]=cz;z[1]=-sz;z[4]=sz;z[5]=cz;
    return matrix_product(y,matrix_product(x,z));
}
// No pending look must preserve native bob/shake and its exact float basis.
// Multiplying a rotation by its transpose needlessly perturbs that basis.
inline PresentationMatrix presentation_look_rotation(const std::array<float,3>& frame_angles,
        const std::array<float,3>& angles,const PresentationMatrix& camera){
    if(angles==frame_angles)return camera;
    return matrix_product(matrix_product(native_camera_rotation(angles),
        matrix_rigid_inverse(native_camera_rotation(frame_angles))),camera);
}
inline bool presentation_driving(const EntityState& e){auto it=e.properties.find("movement_mode");const auto* mode=it==e.properties.end()?nullptr:std::get_if<int64_t>(&it->second);return e.kind==EntityKind::Player && mode && *mode==3;}
enum class PresentationTransition { Continuous, SampleGap, Discontinuity };
inline PresentationTransition presentation_transition(const EntityState& a,const EntityState& b,double seconds){
    if(a.id!=b.id || a.kind!=b.kind || a.definition!=b.definition || a.resource!=b.resource || a.owner!=b.owner || seconds<=0)
        return PresentationTransition::Discontinuity;
    auto water_view=[](const EntityState& player,int64_t movement,int64_t camera){
        const auto m=player.properties.find("movement_mode"),c=player.properties.find("camera_mode");
        return player.kind==EntityKind::Player && m!=player.properties.end() && c!=player.properties.end() &&
               std::get_if<int64_t>(&m->second) && std::get<int64_t>(m->second)==movement &&
               std::get_if<int64_t>(&c->second) && std::get<int64_t>(c->second)==camera;
    };
    bool shore=false;
    for(const char* key:{"camera_mode","camera_frozen","movement_mode","corpse_mode","corpse_active"}){
        const auto x=a.properties.find(key),y=b.properties.find(key);
        if((x==a.properties.end())!=(y==b.properties.end()))return PresentationTransition::Discontinuity;
        if(x==a.properties.end() || x->second==y->second)continue;
        if(std::string_view(key)=="camera_mode" || std::string_view(key)=="movement_mode"){
            shore=shore || (water_view(a,1,0) && water_view(b,2,1)) || (water_view(a,2,1) && water_view(b,1,0));
            if(shore)continue;
        }
        return PresentationTransition::Discontinuity;
    }
    auto health=[](const EntityState& e){auto it=e.properties.find("health");return it==e.properties.end()?1.f:std::get<float>(it->second);};
    if((health(a)>0)!=(health(b)>0))return PresentationTransition::Discontinuity;
    const auto x=a.properties.find("position"),y=b.properties.find("position");if(x==a.properties.end() || y==b.properties.end())return PresentationTransition::Discontinuity;
    const auto* p=std::get_if<std::array<float,3>>(&x->second);const auto* q=std::get_if<std::array<float,3>>(&y->second);if(!p || !q)return PresentationTransition::Discontinuity;
    double limit=std::max(.75,40.*seconds);
    if(b.kind==EntityKind::Projectile || presentation_driving(b))limit=std::max(3.,160.*seconds);
    else if(b.kind==EntityKind::Enemy && b.definition==5){
        // The original adult Werebull charges at 40 units/s. Network timestamps
        // can straddle one extra native movement tick (50 Hz PAL or 60 Hz).
        limit=40.*(seconds+1./50.);
    }
    double d=0;for(unsigned i=0;i<3;++i)d+=double((*p)[i]-(*q)[i])*((*p)[i]-(*q)[i]);
    if(!std::isfinite(d) || d>=limit*limit)return PresentationTransition::Discontinuity;
    // Switch the original camera pose immediately at the shoreline, keeping
    // local input history across a continuous, living player's movement.
    return seconds>.25 || shore ? PresentationTransition::SampleGap : PresentationTransition::Continuous;
}
inline bool presentation_continuous(const EntityState& a,const EntityState& b,double seconds){
    return presentation_transition(a,b,seconds)==PresentationTransition::Continuous;
}
inline bool presentation_field(const std::string& key){
    if(key.starts_with("rotor_row") || key=="pickup_glow_pose")return true;
    if(key=="camera_mode" || key=="projectile_source" || key=="projectile_energy_phase" || key=="projectile_energy_visual")return true;
    return key=="position" || key=="enemy_rotation" || key=="enemy_scale" || key.starts_with("camera_row") || key=="render_visible" || key.starts_with("render_row") || key.starts_with("body_render") ||
        ((key.starts_with("body") || key.starts_with("enemy_")) && key.find("_bone_")!=std::string::npos) ||
        (key.starts_with("weapon_model") && (key.find("_row")!=std::string::npos || key.find("_bone_")!=std::string::npos || key=="weapon_model_slot" || key=="weapon_model_visibility"));
}
inline EntityState presentation_fields(const EntityState& e){
    EntityState out{e.id,e.kind,e.definition,e.resource,e.owner,{}};
    // Network-only inventories, HUD/configuration and script payloads can be
    // large. Copy only the values used by this render frame, in map order.
    for(const auto& item:e.properties)if(presentation_field(item.first))out.properties.emplace_hint(out.properties.end(),item);
    return out;
}
// Reproducible developer A/B only; no saved preference or network state.
inline bool presentation_vector_reuse_enabled(){
    static const bool enabled=[] {
        const auto* mode=std::getenv("SSNE_PRESENTATION_VECTOR_REUSE");
        return !mode || std::strcmp(mode,"0")!=0;
    }();
    return enabled;
}
inline void interpolate_property(Property& out,const Property& a,const Property& b,float alpha,const std::string& key){
    if(a.index()!=b.index())return;
    auto blend=[&](float x,float y){return x+(y-x)*alpha;};
    if(const auto* x=std::get_if<std::array<float,3>>(&a)){const auto& y=std::get<std::array<float,3>>(b);auto v=*x;for(unsigned i=0;i<3;++i){const auto delta=key=="enemy_rotation"?std::remainder(y[i]-v[i],6.283185307f):y[i]-v[i];v[i]+=delta*alpha;}out=v;}
    else if(const auto* x=std::get_if<std::array<float,4>>(&a)){const auto& y=std::get<std::array<float,4>>(b);auto v=*x;for(unsigned i=0;i<4;++i)v[i]=blend(v[i],y[i]);out=v;}
    else if(const auto* x=std::get_if<std::vector<float>>(&a)){
        const auto& y=std::get<std::vector<float>>(b);if(x->size()!=y.size())return;
        // presentation_fields already owns a vector for this render frame.
        // Reuse that storage; when out aliases a/b their equal sizes also mean
        // resize cannot invalidate either input, and each coefficient is independent.
        if(presentation_vector_reuse_enabled())if(auto* v=std::get_if<std::vector<float>>(&out)){
            v->resize(x->size());
            for(unsigned i=0;i<v->size();++i)(*v)[i]=blend((*x)[i],y[i]);
            return;
        }
        // Exact old path for A/B and a target with another property type.
        auto v=*x;for(unsigned i=0;i<v.size();++i)v[i]=blend(v[i],y[i]);out=std::move(v);
    }
}
inline bool presentation_cache_enabled(){
    static const bool enabled=[] {
        const auto* mode=std::getenv("SSNE_PRESENTATION_CACHE");
        return !mode || std::strcmp(mode,"0")!=0;
    }();
    return enabled;
}
class ClientPresentation {
    struct Sample {std::shared_ptr<const WorldState> world;double host,received;};
    std::deque<Sample> samples_;
    double cursor_=0,last_render_=0,interval_=1./60,jitter_=0;
    uint64_t epoch_=0;
    PlayerId owner_{};
    struct CachedField {
        const std::string* key=nullptr;
        const Property* latest=nullptr;
        const Property* before=nullptr;
        const Property* after=nullptr;
        bool blend=false;
    };
    struct CachedEntity {
        EntityId id=0;
        const EntityState* current=nullptr;
        const EntityState* before=nullptr;
        const EntityState* after=nullptr;
        bool continuous=false;
        std::vector<CachedField> fields;
    };
    // The cached descriptions refer only to immutable owned snapshots. There
    // are no guest-RAM pointers or borrowed output-node pointers in this cache.
    std::vector<CachedEntity> cached_entities_;
    std::shared_ptr<const WorldState> cached_before_,cached_after_,cached_latest_;
    uint64_t history_generation_=0,cached_generation_=0;
    PlayerId cached_owner_{};
    bool cache_valid_=false;
    void clear_cache(){
        cache_valid_=false;cached_entities_.clear();
        cached_before_.reset();cached_after_.reset();cached_latest_.reset();
    }
    static bool render_entity(const EntityState& e){
        return e.kind==EntityKind::Player || e.kind==EntityKind::Enemy || e.kind==EntityKind::Pickup || e.kind==EntityKind::Projectile ||
            (e.kind==EntityKind::Physics && (e.definition==3 || e.definition==4));
    }
    static bool blends_value(const Property& a,const Property& b){
        if(a.index()!=b.index())return false;
        if(std::holds_alternative<std::array<float,3>>(a) || std::holds_alternative<std::array<float,4>>(a))return true;
        const auto* x=std::get_if<std::vector<float>>(&a);
        return x && x->size()==std::get<std::vector<float>>(b).size();
    }
    void prepare_cached_entities(const Sample& before,const Sample& after,const Sample& latest,double span){
        cache_valid_=false;
        cached_before_=before.world;cached_after_=after.world;cached_latest_=latest.world;
        size_t used=0;
        for(const auto& [id,current]:latest.world->entities){
            if(!render_entity(current))continue;
            if(used==cached_entities_.size())cached_entities_.emplace_back();
            auto& plan=cached_entities_[used++];plan.id=id;plan.current=&current;plan.fields.clear();
            auto a=before.world->entities.find(id),b=after.world->entities.find(id);
            plan.before=a==before.world->entities.end()?nullptr:&a->second;
            plan.after=b==after.world->entities.end()?nullptr:&b->second;
            plan.continuous=plan.before && plan.after && span>0 && presentation_continuous(*plan.before,*plan.after,span) &&
                presentation_continuous(*plan.after,current,std::max(.001,latest.host-after.host));
            for(const auto& [key,value]:current.properties){
                if(!presentation_field(key))continue;
                CachedField field{&key,&value};
                if(plan.continuous){
                    bool eligible=key!="projectile_energy_visual";
                    if(current.owner==owner_ && !presentation_driving(current) && key.starts_with("camera_row"))eligible=false;
                    if(current.owner==owner_ && key.starts_with("weapon_model") && key.find("_row")!=std::string::npos)eligible=false;
                    if(eligible && key.starts_with("weapon_model")){
                        const auto slot=current.properties.find("weapon_model_slot");
                        if(slot==current.properties.end() || !a->second.properties.contains("weapon_model_slot") || !b->second.properties.contains("weapon_model_slot") || a->second.properties.at("weapon_model_slot")!=slot->second || b->second.properties.at("weapon_model_slot")!=slot->second || a->second.properties.at("weapon_model_visibility")!=current.properties.at("weapon_model_visibility") || b->second.properties.at("weapon_model_visibility")!=current.properties.at("weapon_model_visibility"))eligible=false;
                    }
                    if(eligible){
                        const auto x=a->second.properties.find(key),y=b->second.properties.find(key);
                        if(x!=a->second.properties.end() && y!=b->second.properties.end()){
                            if(key=="pickup_glow_pose"){
                                const auto size=std::get<std::vector<float>>(value).size();
                                const bool lifetime_edge=std::any_of(samples_.begin(),samples_.end(),[&](const Sample& sample){if(sample.host<before.host)return false;auto found=sample.world->entities.find(id);if(found==sample.world->entities.end())return true;auto pose=found->second.properties.find(key);const auto* v=pose==found->second.properties.end()?nullptr:std::get_if<std::vector<float>>(&pose->second);return !v || v->size()!=size;});
                                if(lifetime_edge)eligible=false;
                            }
                            if(eligible && key.find("_bone_matrices")!=std::string::npos){auto count=key.substr(0,key.size()-8)+"count",layout=key.substr(0,key.size()-8)+"layout";if(!a->second.properties.contains(count) || !b->second.properties.contains(count) || !a->second.properties.contains(layout) || !b->second.properties.contains(layout) || a->second.properties.at(count)!=current.properties.at(count) || b->second.properties.at(count)!=current.properties.at(count) || a->second.properties.at(layout)!=current.properties.at(layout) || b->second.properties.at(layout)!=current.properties.at(layout))eligible=false;}
                            if(eligible && blends_value(x->second,y->second)){
                                field.before=&x->second;field.after=&y->second;field.blend=true;
                            }
                        }
                    }
                }
                plan.fields.push_back(field);
            }
        }
        cached_entities_.resize(used);cached_generation_=history_generation_;cached_owner_=owner_;cache_valid_=true;
    }
public:
    void reset(){samples_.clear();clear_cache();++history_generation_;epoch_=0;cursor_=last_render_=0;interval_=1./60;jitter_=0;}
    PresentationTransition push(std::shared_ptr<const WorldState> world,double received,PlayerId owner){
        auto it=world->mission.find("Session.Motion.Time");const auto* t=it==world->mission.end()?nullptr:std::get_if<float>(&it->second);
        if(!t || !std::isfinite(*t))return PresentationTransition::Continuous;
        auto transition=PresentationTransition::Discontinuity;
        if(world->epoch==epoch_ && owner==owner_ && !samples_.empty()){
            const auto& old=*samples_.back().world;
            const auto* a=own(old,owner),*b=own(*world,owner);
            // Revisions for votes/ready/ack may reuse the same captured pose.
            if(a && b){
                if(*t==samples_.back().host && *a==*b)return PresentationTransition::Continuous;
                transition=presentation_transition(*a,*b,*t-samples_.back().host);
            }
        }
        if(transition!=PresentationTransition::Continuous){reset();epoch_=world->epoch;owner_=owner;cursor_=*t;last_render_=received;}
        else {
            const auto span=double(*t)-samples_.back().host;
            interval_=.8*interval_+.2*std::clamp(span,.008,.10);
            // A delayed ordered packet can release a burst of snapshots. Host
            // cadence alone cannot cover that gap. Retain measured delivery
            // variation, with fast attack and a three-second decay; idle local
            // connections keep the original low delay.
            const auto variation=std::abs((received-samples_.back().received)-span);
            jitter_=std::max(jitter_*std::exp(-span/3.),std::clamp(variation,0.,.12));
        }
        samples_.push_back({std::move(world),*t,received});while(samples_.size()>32)samples_.pop_front();
        ++history_generation_;cache_valid_=false;return transition;
    }
    static const EntityState* own(const WorldState& world,PlayerId player){for(const auto& [id,e]:world.entities)if(e.kind==EntityKind::Player && e.owner==player)return &e;return nullptr;}
    std::map<EntityId,EntityState> sample(double now,Properties* vehicles=nullptr){
        std::map<EntityId,EntityState> out;if(samples_.empty())return out;
        const auto& latest=samples_.back();
        const auto delay=delay_ms()/1000.;
        const double target=latest.host+std::clamp(now-latest.received,0.,.25)-delay;
        const double dt=std::clamp(now-last_render_,0.,.1);last_render_=now;
        if(std::abs(target-cursor_)>.25)cursor_=target;
        else cursor_+=dt*std::clamp(1.+(target-cursor_)*4.,.9,1.1);
        cursor_=std::clamp(cursor_,samples_.front().host,latest.host);
        while(samples_.size()>2 && samples_[1].host<cursor_)samples_.pop_front();
        const Sample* before=&samples_.front(),*after=before;
        for(const auto& s:samples_){if(s.host<=cursor_)before=&s;if(s.host>=cursor_){after=&s;break;}after=&s;}
        const double span=after->host-before->host;const float alpha=span>0?float(std::clamp((cursor_-before->host)/span,0.,1.)):1;
        if(vehicles){
            vehicles->clear();
            const auto& current=latest.world->mission;
            for(const char* key:{"Session.Vehicles.Tags","Session.Vehicles.Values"})if(auto it=current.find(key);it!=current.end())(*vehicles)[key]=it->second;
            auto tags=[](const Properties& p){auto i=p.find("Session.Vehicles.Tags");return i==p.end()?nullptr:std::get_if<std::vector<int64_t>>(&i->second);};
            auto values=[](const Properties& p){auto i=p.find("Session.Vehicles.Values");return i==p.end()?nullptr:std::get_if<std::vector<float>>(&i->second);};
            const auto* tc=tags(current),*ta=tags(before->world->mission),*tb=tags(after->world->mission);
            const auto* va=values(before->world->mission),*vb=values(after->world->mission);
            if(tc && ta && tb && va && vb && tc->size()%20==0 && ta->size()==tc->size() && tb->size()==tc->size() && span>0){
                auto out=vehicles->find("Session.Vehicles.Values");auto* vc=out==vehicles->end()?nullptr:std::get_if<std::vector<float>>(&out->second);size_t offset=0;
                for(size_t n=0;vc && n<tc->size();n+=20){
                    const auto count=(*tc)[n+6];if(count<0 || count>128)break;
                    const size_t length=(*tc)[n+1]?104+size_t(count)*12:0;
                    // Match complete records, not array positions from a different
                    // vehicle lifetime. Entry/exit and teleports snap immediately.
                    if(!std::equal(tc->begin()+n,tc->begin()+n+8,ta->begin()+n) || !std::equal(tc->begin()+n,tc->begin()+n+8,tb->begin()+n))break;
                    if(offset+length>vc->size() || offset+length>va->size() || offset+length>vb->size())break;
                    double distance=0,tail=0;for(unsigned i=0;length && i<3;++i){distance+=std::pow((*vb)[offset+i]-(*va)[offset+i],2);tail+=std::pow((*vc)[offset+i]-(*vb)[offset+i],2);}
                    const auto limit=std::max(3.,160.*span),tail_limit=std::max(3.,160.*std::max(.001,latest.host-after->host));
                    if(length && distance<limit*limit && tail<tail_limit*tail_limit)for(size_t i=0;i<length;++i){
                        if(i==6 || i==7)continue; // Health is authoritative, never a render field.
                        const auto delta=i>=3 && i<6?std::remainder((*vb)[offset+i]-(*va)[offset+i],6.283185307f):(*vb)[offset+i]-(*va)[offset+i];
                        (*vc)[offset+i]=(*va)[offset+i]+delta*alpha;
                    }
                    offset+=length;
                }
            }
        }
        for(const auto& [id,current]:latest.world->entities){
            if(current.kind!=EntityKind::Player && current.kind!=EntityKind::Enemy && current.kind!=EntityKind::Pickup && current.kind!=EntityKind::Projectile && !(current.kind==EntityKind::Physics && (current.definition==3 || current.definition==4)))continue;
            auto e=presentation_fields(current);auto a=before->world->entities.find(id),b=after->world->entities.find(id);
            if(a!=before->world->entities.end() && b!=after->world->entities.end() && span>0 && presentation_continuous(a->second,b->second,span) && presentation_continuous(b->second,current,std::max(.001,latest.host-after->host))){
                for(auto& [key,value]:e.properties){
                    if(key=="projectile_energy_visual")continue;
                    // The local camera uses the latest orientation, so replaying
                    // pending mouse deltas never fights an older buffered angle.
                    if(current.owner==owner_ && !presentation_driving(current) && key.starts_with("camera_row"))continue;
                    if(current.owner==owner_ && key.starts_with("weapon_model") && key.find("_row")!=std::string::npos)continue;
                    if(key.starts_with("weapon_model")){
                        const auto slot=current.properties.find("weapon_model_slot");
                        if(slot==current.properties.end() || !a->second.properties.contains("weapon_model_slot") || !b->second.properties.contains("weapon_model_slot") || a->second.properties.at("weapon_model_slot")!=slot->second || b->second.properties.at("weapon_model_slot")!=slot->second || a->second.properties.at("weapon_model_visibility")!=current.properties.at("weapon_model_visibility") || b->second.properties.at("weapon_model_visibility")!=current.properties.at("weapon_model_visibility"))continue;
                    }
                    const auto x=a->second.properties.find(key),y=b->second.properties.find(key);
                    if(x!=a->second.properties.end() && y!=b->second.properties.end()){
                        // Creation/retirement is a discrete edge. Buffered old
                        // poses must not resurrect a glow absent in the latest
                        // snapshot, or erase a newly restored glow pose.
                        if(key=="pickup_glow_pose"){
                            const auto size=std::get<std::vector<float>>(value).size();
                            const bool lifetime_edge=std::any_of(samples_.begin(),samples_.end(),[&](const Sample& s){if(s.host<before->host)return false;auto found=s.world->entities.find(id);if(found==s.world->entities.end())return true;auto pose=found->second.properties.find(key);const auto* v=pose==found->second.properties.end()?nullptr:std::get_if<std::vector<float>>(&pose->second);return !v || v->size()!=size;});
                            if(lifetime_edge)continue;
                        }
                        if(key.find("_bone_matrices")!=std::string::npos){auto count=key.substr(0,key.size()-8)+"count",layout=key.substr(0,key.size()-8)+"layout";if(!a->second.properties.contains(count) || !b->second.properties.contains(count) || !a->second.properties.contains(layout) || !b->second.properties.contains(layout) || a->second.properties.at(count)!=current.properties.at(count) || b->second.properties.at(count)!=current.properties.at(count) || a->second.properties.at(layout)!=current.properties.at(layout) || b->second.properties.at(layout)!=current.properties.at(layout))continue;}
                        interpolate_property(value,x->second,y->second,alpha,key);
                    }
                }
                if(current.owner==owner_ && !presentation_driving(current)){
                    PresentationMatrix camera,ca,cb;
                    if(presentation_matrix(current.properties,"camera_row",camera) && presentation_matrix(a->second.properties,"camera_row",ca) && presentation_matrix(b->second.properties,"camera_row",cb)){
                        const auto slot=current.properties.find("weapon_model_slot");
                        if(slot!=current.properties.end() && a->second.properties.contains("weapon_model_slot") && b->second.properties.contains("weapon_model_slot") && a->second.properties.at("weapon_model_slot")==slot->second && b->second.properties.at("weapon_model_slot")==slot->second){
                            for(unsigned hand=0;hand<2;++hand){
                                const auto prefix="weapon_model"+std::to_string(hand)+"_row";PresentationMatrix ga,gb,gc;
                                if(presentation_matrix(a->second.properties,prefix,ga) && presentation_matrix(b->second.properties,prefix,gb) && presentation_matrix(current.properties,prefix,gc) && a->second.properties.at("weapon_model_visibility")==current.properties.at("weapon_model_visibility") && b->second.properties.at("weapon_model_visibility")==current.properties.at("weapon_model_visibility")){
                                    auto ra=matrix_product(matrix_rigid_inverse(ca),ga),rb=matrix_product(matrix_rigid_inverse(cb),gb);
                                    for(unsigned i=0;i<12;++i)ra[i]+=(rb[i]-ra[i])*alpha;
                                    put_presentation_matrix(e.properties,prefix,matrix_product(camera,ra));
                                }
                            }
                        }
                        const auto previous_camera=camera;
                        for(unsigned r=0;r<3;++r)camera[r*4+3]=ca[r*4+3]+(cb[r*4+3]-ca[r*4+3])*alpha;
                        const auto translation=matrix_product(camera,matrix_rigid_inverse(previous_camera));
                        for(unsigned hand=0;hand<2;++hand){const auto prefix="weapon_model"+std::to_string(hand)+"_row";PresentationMatrix gun;if(presentation_matrix(e.properties,prefix,gun))put_presentation_matrix(e.properties,prefix,matrix_product(translation,gun));}
                        put_presentation_matrix(e.properties,"camera_row",camera);
                    }
                }
            }
            out.emplace(id,std::move(e));
        }return out;
    }
    // Caller-owned scratch stays mutable only for its current render lease.
    // Every call restores all discrete/latest values before local camera work;
    // interpolation overwrites its complete result, never accumulating deltas.
    void sample_into(double now,std::map<EntityId,EntityState>& out,Properties* vehicles=nullptr){
        // Guard the public scratch API against accidental aliasing of a retained
        // snapshot. Normal NativeSession output is a separate private member.
        for(const auto& sample:samples_)
            if(&out==&sample.world->entities || (vehicles && vehicles==&sample.world->mission))
                throw std::invalid_argument("Presentation output aliases an immutable snapshot");
        if(!presentation_cache_enabled()){out=sample(now,vehicles);return;}
        if(samples_.empty()){out.clear();return;}
        const auto& latest=samples_.back();
        const auto delay=delay_ms()/1000.;
        const double target=latest.host+std::clamp(now-latest.received,0.,.25)-delay;
        const double dt=std::clamp(now-last_render_,0.,.1);last_render_=now;
        if(std::abs(target-cursor_)>.25)cursor_=target;
        else cursor_+=dt*std::clamp(1.+(target-cursor_)*4.,.9,1.1);
        cursor_=std::clamp(cursor_,samples_.front().host,latest.host);
        while(samples_.size()>2 && samples_[1].host<cursor_)samples_.pop_front();
        const Sample* before=&samples_.front(),*after=before;
        for(const auto& s:samples_){if(s.host<=cursor_)before=&s;if(s.host>=cursor_){after=&s;break;}after=&s;}
        const double span=after->host-before->host;const float alpha=span>0?float(std::clamp((cursor_-before->host)/span,0.,1.)):1;
        if(vehicles){
            const auto& current=latest.world->mission;
            // Retain output nodes/vector capacity while restoring all latest values.
            for(auto it=vehicles->begin();it!=vehicles->end();){
                if((it->first=="Session.Vehicles.Tags" || it->first=="Session.Vehicles.Values") && current.contains(it->first))++it;
                else it=vehicles->erase(it);
            }
            for(const char* key:{"Session.Vehicles.Tags","Session.Vehicles.Values"})if(auto it=current.find(key);it!=current.end())(*vehicles)[key]=it->second;
            auto tags=[](const Properties& p){auto i=p.find("Session.Vehicles.Tags");return i==p.end()?nullptr:std::get_if<std::vector<int64_t>>(&i->second);};
            auto values=[](const Properties& p){auto i=p.find("Session.Vehicles.Values");return i==p.end()?nullptr:std::get_if<std::vector<float>>(&i->second);};
            const auto* tc=tags(current),*ta=tags(before->world->mission),*tb=tags(after->world->mission);
            const auto* va=values(before->world->mission),*vb=values(after->world->mission);
            if(tc && ta && tb && va && vb && tc->size()%20==0 && ta->size()==tc->size() && tb->size()==tc->size() && span>0){
                auto out=vehicles->find("Session.Vehicles.Values");auto* vc=out==vehicles->end()?nullptr:std::get_if<std::vector<float>>(&out->second);size_t offset=0;
                for(size_t n=0;vc && n<tc->size();n+=20){
                    const auto count=(*tc)[n+6];if(count<0 || count>128)break;
                    const size_t length=(*tc)[n+1]?104+size_t(count)*12:0;
                    // Match complete records, not array positions from a different
                    // vehicle lifetime. Entry/exit and teleports snap immediately.
                    if(!std::equal(tc->begin()+n,tc->begin()+n+8,ta->begin()+n) || !std::equal(tc->begin()+n,tc->begin()+n+8,tb->begin()+n))break;
                    if(offset+length>vc->size() || offset+length>va->size() || offset+length>vb->size())break;
                    double distance=0,tail=0;for(unsigned i=0;length && i<3;++i){distance+=std::pow((*vb)[offset+i]-(*va)[offset+i],2);tail+=std::pow((*vc)[offset+i]-(*vb)[offset+i],2);}
                    const auto limit=std::max(3.,160.*span),tail_limit=std::max(3.,160.*std::max(.001,latest.host-after->host));
                    if(length && distance<limit*limit && tail<tail_limit*tail_limit)for(size_t i=0;i<length;++i){
                        if(i==6 || i==7)continue; // Health is authoritative, never a render field.
                        const auto delta=i>=3 && i<6?std::remainder((*vb)[offset+i]-(*va)[offset+i],6.283185307f):(*vb)[offset+i]-(*va)[offset+i];
                        (*vc)[offset+i]=(*va)[offset+i]+delta*alpha;
                    }
                    offset+=length;
                }
            }
        }
        if(!cache_valid_ || cached_generation_!=history_generation_ || cached_owner_!=owner_ ||
           cached_before_.get()!=before->world.get() || cached_after_.get()!=after->world.get() || cached_latest_.get()!=latest.world.get())
            prepare_cached_entities(*before,*after,latest,span);
        auto output=out.begin();
        for(const auto& plan:cached_entities_){
            const auto& current=*plan.current;
            while(output!=out.end() && output->first<plan.id)output=out.erase(output);
            if(output==out.end() || output->first!=plan.id)
                output=out.emplace_hint(output,plan.id,EntityState{current.id,current.kind,current.definition,current.resource,current.owner,{}});
            auto& e=output->second;
            e.id=current.id;e.kind=current.kind;e.definition=current.definition;e.resource=current.resource;e.owner=current.owner;
            auto field_output=e.properties.begin();
            for(const auto& field:plan.fields){
                const auto& key=*field.key;
                if(field_output==e.properties.end() || field_output->first!=key){
                    while(field_output!=e.properties.end() && field_output->first<key)field_output=e.properties.erase(field_output);
                    if(field_output==e.properties.end() || field_output->first!=key)
                        field_output=e.properties.emplace_hint(field_output,key,*field.latest);
                }
                auto& value=field_output->second;
                if(field.blend)interpolate_property(value,*field.before,*field.after,alpha,key);
                else value=*field.latest;
                ++field_output;
            }
            e.properties.erase(field_output,e.properties.end());
            if(plan.continuous){
                const auto* a=plan.before;const auto* b=plan.after;
                if(current.owner==owner_ && !presentation_driving(current)){
                    PresentationMatrix camera,ca,cb;
                    if(presentation_matrix(current.properties,"camera_row",camera) && presentation_matrix(a->properties,"camera_row",ca) && presentation_matrix(b->properties,"camera_row",cb)){
                        const auto slot=current.properties.find("weapon_model_slot");
                        if(slot!=current.properties.end() && a->properties.contains("weapon_model_slot") && b->properties.contains("weapon_model_slot") && a->properties.at("weapon_model_slot")==slot->second && b->properties.at("weapon_model_slot")==slot->second){
                            for(unsigned hand=0;hand<2;++hand){
                                const auto prefix="weapon_model"+std::to_string(hand)+"_row";PresentationMatrix ga,gb,gc;
                                if(presentation_matrix(a->properties,prefix,ga) && presentation_matrix(b->properties,prefix,gb) && presentation_matrix(current.properties,prefix,gc) && a->properties.at("weapon_model_visibility")==current.properties.at("weapon_model_visibility") && b->properties.at("weapon_model_visibility")==current.properties.at("weapon_model_visibility")){
                                    auto ra=matrix_product(matrix_rigid_inverse(ca),ga),rb=matrix_product(matrix_rigid_inverse(cb),gb);
                                    for(unsigned i=0;i<12;++i)ra[i]+=(rb[i]-ra[i])*alpha;
                                    put_presentation_matrix(e.properties,prefix,matrix_product(camera,ra));
                                }
                            }
                        }
                        const auto previous_camera=camera;
                        for(unsigned r=0;r<3;++r)camera[r*4+3]=ca[r*4+3]+(cb[r*4+3]-ca[r*4+3])*alpha;
                        const auto translation=matrix_product(camera,matrix_rigid_inverse(previous_camera));
                        for(unsigned hand=0;hand<2;++hand){const auto prefix="weapon_model"+std::to_string(hand)+"_row";PresentationMatrix gun;if(presentation_matrix(e.properties,prefix,gun))put_presentation_matrix(e.properties,prefix,matrix_product(translation,gun));}
                        put_presentation_matrix(e.properties,"camera_row",camera);
                    }
                }
            }
            ++output;
        }
        out.erase(output,out.end());
    }
    double delay_ms()const{return std::clamp(interval_+.008+jitter_,.025,.15)*1000;}
};
// Keep first-person gun roots in the same camera space while local look and
// buffered translation change. Bone animation stays authoritative/interpolated.
inline void follow_local_camera(EntityState& e,const EntityState& authoritative,PredictedLook look,float pitch_low,float pitch_high){
    PresentationMatrix from,to;if(!presentation_matrix(e.properties,"camera_row",from) || !presentation_matrix(authoritative.properties,"camera_row",to))return;
    // The sampled pose can lag the most recently APPLIED body revision. Rebase
    // from the camera actually owning this visual gun, then adopt the latest
    // authoritative orientation. Using the newer body's camera as the old
    // gun reference applies that revision difference a second time.
    for(unsigned r=0;r<3;++r)to[r*4+3]=from[r*4+3];
    const auto angles=authoritative.properties.find("view_angles");const auto* v=angles==authoritative.properties.end()?nullptr:std::get_if<std::array<float,3>>(&angles->second);
    if(v && (look.yaw || look.pitch)){
        auto next=*v;next[0]+=float(look.yaw);next[1]=std::clamp(next[1]-float(look.pitch),pitch_low,pitch_high);
        const auto correction=matrix_product(native_camera_rotation(next),matrix_rigid_inverse(native_camera_rotation(*v)));
        auto rotated=matrix_product(correction,to);for(unsigned r=0;r<3;++r)rotated[r*4+3]=to[r*4+3];to=rotated;
    }
    put_presentation_matrix(e.properties,"camera_row",to);
    const auto adjustment=matrix_product(to,matrix_rigid_inverse(from));
    for(unsigned hand=0;hand<2;++hand){const auto prefix="weapon_model"+std::to_string(hand)+"_row";PresentationMatrix gun;if(presentation_matrix(e.properties,prefix,gun))put_presentation_matrix(e.properties,prefix,matrix_product(adjustment,gun));}
}
}
