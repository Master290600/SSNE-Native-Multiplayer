#pragma once
#include "world_state.h"
#include "ssne/native_movement_step.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <span>

namespace port::coop {
inline constexpr size_t movement_history_limit=480;
inline constexpr size_t movement_batch_limit=32; // 28 + 32*15 bytes fits the authenticated 512-byte input frame.
inline constexpr double movement_command_tick=.01;
struct MovementStreamIdentity {
    uint64_t epoch=0,entity=0,token=0;
    bool operator==(const MovementStreamIdentity&)const=default;
    explicit operator bool()const{return epoch && entity && token;}
};
// Commands spend real elapsed time only after native completion. A bounded
// credit allows missed native updates to catch up without an unlimited burst.
class MovementStepClock {
    double previous_=0,credit_=0;
    bool initialized_=false;
public:
    void reset(double now=std::numeric_limits<double>::quiet_NaN()){previous_=now;credit_=0;initialized_=std::isfinite(now);}
    void update(double now){
        if(!std::isfinite(now) || (initialized_ && now<previous_))return;
        if(!initialized_){reset(now);return;}
        credit_=std::min(.5,credit_+now-previous_);previous_=now;
    }
    unsigned available()const{return unsigned(std::max(0.,std::floor((credit_+1e-8)/movement_command_tick)));}
    bool complete(){if(!available())return false;credit_=std::max(0.,credit_-movement_command_tick);return true;}
};
inline bool valid_movement_command(const ssne::NativeMovementCommand& command){
    return command.sequence && std::isfinite(command.yaw) && std::abs(command.yaw)<=10000.f;
}
inline std::vector<float> encode_movement_checkpoint(const ssne::NativeMovementState& state){
    std::vector<float> data;data.reserve(23);
    data.insert(data.end(),state.position.begin(),state.position.end());
    data.insert(data.end(),state.velocity.begin(),state.velocity.end());
    data.insert(data.end(),{float(state.phase),float(state.pending_jump),float(state.grounded),float(state.sliding)});
    data.insert(data.end(),state.curve.begin(),state.curve.end());
    data.insert(data.end(),state.ground.begin(),state.ground.end());
    data.push_back(float(state.contact_kind));
    return data;
}
inline std::optional<ssne::NativeMovementState> decode_movement_checkpoint(const Property& property){
    const auto* data=std::get_if<std::vector<float>>(&property);
    if(!data || data->size()!=23 || !std::all_of(data->begin(),data->end(),[](float n){return std::isfinite(n) && std::abs(n)<=1000000.f;}))return {};
    const auto& p=*data;
    if((p[6]!=0 && p[6]!=1 && p[6]!=3) || (p[7]!=0 && p[7]!=1) ||
       (p[8]!=0 && p[8]!=1) || (p[9]!=0 && p[9]!=1) ||
       std::hypot(p[3],p[4],p[5])>=160.f || p[15]<0.f || p[15]>.2f || (p[22]!=0 && p[22]!=1))return {};
    ssne::NativeMovementState result;
    std::copy_n(p.begin(),3,result.position.begin());std::copy_n(p.begin()+3,3,result.velocity.begin());
    result.phase=uint32_t(p[6]);result.pending_jump=uint32_t(p[7]);
    result.grounded=bool(p[8]);result.sliding=bool(p[9]);
    std::copy_n(p.begin()+10,7,result.curve.begin());std::copy_n(p.begin()+17,5,result.ground.begin());
    result.contact_kind=uint32_t(p[22]);
    if(result.phase==1 && (!(result.curve[0]>0.f) || !(result.curve[1]>0.f) || result.curve[1]>2.f ||
       result.curve[2]<0.f || result.curve[2]>result.curve[1]+.011f))return {};
    return result;
}
inline uint64_t movement_unsigned(const Properties& properties,const char* key){
    const auto it=properties.find(key);if(it==properties.end())return 0;
    const auto* value=std::get_if<int64_t>(&it->second);return value && *value>=0?uint64_t(*value):0;
}
inline bool player_movement_metadata(EntityKind kind,std::string_view key){
    return kind==EntityKind::Player && (key=="movement_checkpoint" || key=="movement_spawn" ||
        key=="movement_stream" || key=="movement_ack");
}
// Exactly one entry can be reserved for an original native substep. Receiving
// or reserving never acknowledges it; only successful completion advances ACK.
class MovementCommandFIFO {
    MovementStreamIdentity identity_{};
    std::deque<ssne::NativeMovementCommand> pending_;
    uint64_t received_=0,completed_=0,reserved_=0;
public:
    const MovementStreamIdentity& identity()const{return identity_;}
    void bind(MovementStreamIdentity identity){if(identity_==identity)return;identity_=identity;pending_.clear();received_=completed_=reserved_=0;}
    uint64_t acknowledged()const{return completed_;}
    size_t size()const{return pending_.size();}
    bool append(std::span<const ssne::NativeMovementCommand> commands){
        auto next=received_;size_t additional=0;
        for(const auto& command:commands){
            if(!valid_movement_command(command))return false;
            if(command.sequence<=received_)continue;
            if(command.sequence!=next+1)return false;
            next=command.sequence;++additional;
        }
        if(pending_.size()+additional>movement_history_limit)return false;
        for(const auto& command:commands)if(command.sequence>received_)pending_.push_back(command);
        received_=next;return true;
    }
    std::optional<ssne::NativeMovementCommand> reserve(){
        if(reserved_ || pending_.empty())return {};
        reserved_=pending_.front().sequence;return pending_.front();
    }
    bool complete(uint64_t sequence){
        if(!sequence || reserved_!=sequence || pending_.empty() || pending_.front().sequence!=sequence)return false;
        completed_=sequence;reserved_=0;pending_.pop_front();return true;
    }
};
// Local time creates fixed-duration commands once; rendering and networking
// borrow the same retained history. TCP batching cannot overwrite a short tap.
class MovementCommandHistory {
    MovementStreamIdentity identity_{};
    std::deque<ssne::NativeMovementCommand> commands_;
    uint64_t generated_=0,last_jump_=0;
    double previous_=0,elapsed_=0;
    bool initialized_=false,jump_pending_=false;
public:
    const MovementStreamIdentity& identity()const{return identity_;}
    void reset(){*this={};}
    void bind(MovementStreamIdentity identity,uint64_t ack,double now,uint64_t seen_jump=0){
        if(identity_==identity && initialized_)return;
        reset();identity_=identity;generated_=ack;previous_=now;initialized_=bool(identity);last_jump_=seen_jump;
    }
    void acknowledge(uint64_t ack){
        while(!commands_.empty() && commands_.front().sequence<=ack)commands_.pop_front();
        generated_=std::max(generated_,ack);
    }
    void advance(double now,ssne::NativeMovementCommand input,uint64_t jump_edge){
        if(!initialized_ || !std::isfinite(now) || now<previous_)return;
        const double frame=std::clamp(now-previous_,0.,.05);previous_=now;elapsed_+=frame;
        if(jump_edge && jump_edge>last_jump_){jump_pending_=true;last_jump_=jump_edge;}
        while(elapsed_+1e-9>=movement_command_tick && commands_.size()<movement_history_limit){
            elapsed_-=movement_command_tick;input.sequence=++generated_;
            input.jump_pressed=jump_pending_;jump_pending_=false;
            commands_.push_back(input);
        }
        if(commands_.size()>=movement_history_limit)elapsed_=0;
    }
    std::vector<ssne::NativeMovementCommand> after(uint64_t sequence)const{
        std::vector<ssne::NativeMovementCommand> result;
        for(const auto& command:commands_)if(command.sequence>sequence)result.push_back(command);
        return result;
    }
};
}
