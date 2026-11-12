#pragma once
#include "world_state.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>

namespace port::coop {
// Presentation only. None of these values are sent back as gameplay state.
struct PredictedLook { double yaw=0,pitch=0; };
class InputLookHistory {
    struct Entry {uint64_t sequence;int32_t yaw,pitch;double at;};
    std::deque<Entry> entries_;
    uint64_t epoch_=0;
public:
    void reset(uint64_t epoch){entries_.clear();epoch_=epoch;}
    void append(uint64_t epoch,uint64_t sequence,int32_t yaw,int32_t pitch,double at){
        if(epoch!=epoch_)reset(epoch);
        if(yaw || pitch)entries_.push_back({sequence,yaw,pitch,at});
        while(entries_.size()>2048)entries_.pop_front();
    }
    PredictedLook pending(uint64_t epoch,uint64_t ack,double now,uint64_t confirmed_ack=0){
        if(epoch!=epoch_)return {};
        // GPU frames can still display the prior pose while the guest prepares
        // a newer acknowledged pose. Keep that short history for BOTH readers.
        // Confirmation retires prediction for the new pose, but an older GPU
        // camera still needs that delta. Its input can already be older than
        // 250 ms when that camera is produced. Keep confirmed history for the
        // bounded display pipeline; unconfirmed/stalled input still expires.
        while(!entries_.empty()){
            const auto& e=entries_.front();const auto age=now-e.at;
            if(age>1. || (age>.25 && e.sequence>confirmed_ack))entries_.pop_front();else break;
        }
        PredictedLook out;for(const auto& e:entries_)if(e.sequence>ack && (now-e.at<=.25 || e.sequence<=confirmed_ack)){
            out.yaw+=e.yaw/1000000.;out.pitch+=e.pitch/1000000.;
        }
        out.yaw=std::clamp(out.yaw,-2.,2.);out.pitch=std::clamp(out.pitch,-2.,2.);return out;
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
inline bool presentation_continuous(const EntityState& a,const EntityState& b,double seconds){
    if(a.id!=b.id || a.kind!=b.kind || a.definition!=b.definition || a.resource!=b.resource || a.owner!=b.owner || seconds<=0 || seconds>.25)return false;
    for(const char* key:{"camera_mode","camera_frozen","movement_mode","corpse_mode","corpse_active"}){
        auto x=a.properties.find(key),y=b.properties.find(key);if((x==a.properties.end())!=(y==b.properties.end()) || (x!=a.properties.end() && x->second!=y->second))return false;
    }
    auto health=[](const EntityState& e){auto it=e.properties.find("health");return it==e.properties.end()?1.f:std::get<float>(it->second);};
    if((health(a)>0)!=(health(b)>0))return false;
    const auto x=a.properties.find("position"),y=b.properties.find("position");if(x==a.properties.end() || y==b.properties.end())return false;
    const auto* p=std::get_if<std::array<float,3>>(&x->second);const auto* q=std::get_if<std::array<float,3>>(&y->second);if(!p || !q)return false;
    const double limit=b.kind==EntityKind::Projectile || presentation_driving(b)?std::max(3.,160.*seconds):std::max(.75,40.*seconds);
    double d=0;for(unsigned i=0;i<3;++i)d+=double((*p)[i]-(*q)[i])*((*p)[i]-(*q)[i]);return std::isfinite(d) && d<limit*limit;
}
inline bool presentation_field(const std::string& key){
    if(key.starts_with("rotor_row") || key=="pickup_glow_pose")return true;
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
inline void interpolate_property(Property& out,const Property& a,const Property& b,float alpha,const std::string& key){
    if(a.index()!=b.index())return;
    auto blend=[&](float x,float y){return x+(y-x)*alpha;};
    if(const auto* x=std::get_if<std::array<float,3>>(&a)){const auto& y=std::get<std::array<float,3>>(b);auto v=*x;for(unsigned i=0;i<3;++i){const auto delta=key=="enemy_rotation"?std::remainder(y[i]-v[i],6.283185307f):y[i]-v[i];v[i]+=delta*alpha;}out=v;}
    else if(const auto* x=std::get_if<std::array<float,4>>(&a)){const auto& y=std::get<std::array<float,4>>(b);auto v=*x;for(unsigned i=0;i<4;++i)v[i]=blend(v[i],y[i]);out=v;}
    else if(const auto* x=std::get_if<std::vector<float>>(&a)){const auto& y=std::get<std::vector<float>>(b);if(x->size()!=y.size())return;auto v=*x;for(unsigned i=0;i<v.size();++i)v[i]=blend(v[i],y[i]);out=std::move(v);}
}
class ClientPresentation {
    struct Sample {std::shared_ptr<const WorldState> world;double host,received;};
    std::deque<Sample> samples_;
    double cursor_=0,last_render_=0,interval_=1./60,jitter_=0;
    uint64_t epoch_=0;
    PlayerId owner_{};
public:
    void reset(){samples_.clear();epoch_=0;cursor_=last_render_=0;interval_=1./60;jitter_=0;}
    bool push(std::shared_ptr<const WorldState> world,double received,PlayerId owner){
        auto it=world->mission.find("Session.Motion.Time");const auto* t=it==world->mission.end()?nullptr:std::get_if<float>(&it->second);
        if(!t || !std::isfinite(*t))return false;
        bool cut=world->epoch!=epoch_ || owner!=owner_;
        if(!samples_.empty()){
            const auto& old=*samples_.back().world;
            const auto* a=own(old,owner),*b=own(*world,owner);
            cut|=!a || !b || !presentation_continuous(*a,*b,*t-samples_.back().host);
            // Revisions for votes/ready/ack may reuse the same captured pose.
            if(!cut && *t<=samples_.back().host)return false;
            if(*t==samples_.back().host && a && b && a->properties==b->properties)return false;
        }
        if(cut){reset();epoch_=world->epoch;owner_=owner;cursor_=*t;last_render_=received;}
        else if(!samples_.empty()){
            const auto span=double(*t)-samples_.back().host;
            interval_=.8*interval_+.2*std::clamp(span,.008,.10);
            // A delayed ordered packet can release a burst of snapshots. Host
            // cadence alone cannot cover that gap. Retain measured delivery
            // variation, with fast attack and a three-second decay; idle local
            // connections keep the original low delay.
            const auto variation=std::abs((received-samples_.back().received)-span);
            jitter_=std::max(jitter_*std::exp(-span/3.),std::clamp(variation,0.,.12));
        }
        samples_.push_back({std::move(world),*t,received});while(samples_.size()>32)samples_.pop_front();return cut;
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
