#include "movement_command_stream.h"
#include "command_movement.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace port::coop;
using ssne::NativeMovementCommand;

#define CHECK(condition) do { if(!(condition)) { \
    std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#condition);std::exit(1); \
} } while(false)

static constexpr MovementStreamIdentity identity{7,42,123};

static NativeMovementCommand command(uint64_t sequence,int x=0,int y=0,float yaw=0.f){
    NativeMovementCommand result;
    result.sequence=sequence;result.stick_x=int8_t(x);result.stick_y=int8_t(y);result.yaw=yaw;
    return result;
}

static bool same(const NativeMovementCommand& a,const NativeMovementCommand& b){
    return a.sequence==b.sequence && a.stick_x==b.stick_x && a.stick_y==b.stick_y &&
        a.jump_pressed==b.jump_pressed && a.yaw==b.yaw;
}

static void same_commands(const std::vector<NativeMovementCommand>& a,const std::vector<NativeMovementCommand>& b){
    CHECK(a.size()==b.size());
    for(size_t i=0;i<a.size();++i)CHECK(same(a[i],b[i]));
}

static void fifo_completion_is_the_only_ack(){
    MovementCommandFIFO fifo;fifo.bind(identity);
    const std::vector input{command(1,0,127),command(2,0,0),command(3,0,-127)};
    CHECK(fifo.append(input));CHECK(fifo.size()==3);CHECK(fifo.acknowledged()==0);
    CHECK(!fifo.complete(1)); // Receiving is not completion, even for the head.
    auto first=fifo.reserve();CHECK(first && same(*first,input[0]));
    CHECK(fifo.acknowledged()==0 && fifo.size()==3);
    CHECK(!fifo.reserve());CHECK(!fifo.complete(0));CHECK(!fifo.complete(2));
    CHECK(fifo.acknowledged()==0 && fifo.size()==3);
    fifo.bind(identity); // An unchanged binding must preserve the reservation.
    CHECK(!fifo.reserve());CHECK(fifo.complete(1));
    CHECK(fifo.acknowledged()==1 && fifo.size()==2);CHECK(!fifo.complete(1));
    CHECK(fifo.append(input)); // A received duplicate is an idempotent retransmit.
    CHECK(fifo.acknowledged()==1 && fifo.size()==2);
    for(uint64_t sequence=2;sequence<=3;++sequence){
        auto next=fifo.reserve();CHECK(next && next->sequence==sequence);
        CHECK(fifo.acknowledged()==sequence-1);CHECK(fifo.complete(sequence));
    }
    CHECK(fifo.size()==0 && fifo.acknowledged()==3);CHECK(!fifo.reserve());
    CHECK(fifo.append(input));CHECK(fifo.size()==0 && fifo.acknowledged()==3);
}

static void malformed_batches_are_atomic(){
    MovementCommandFIFO fifo;fifo.bind(identity);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    const float inf=std::numeric_limits<float>::infinity();
    const std::vector<std::vector<NativeMovementCommand>> invalid{
        {command(0)}, {command(1),command(3)}, {command(2),command(1)},
        {command(1),command(1)}, {command(1),command(2),command(1)},
        {command(1,0,0,nan)}, {command(1),command(2,0,0,inf)},
        {command(1),command(2,0,0,10001.f)},
    };
    for(const auto& batch:invalid){
        CHECK(!fifo.append(batch));CHECK(fifo.size()==0 && fifo.acknowledged()==0);
        CHECK(!fifo.reserve());
    }
    const std::vector prefix{command(1),command(2)};
    CHECK(fifo.append(prefix));CHECK(fifo.reserve()->sequence==1);
    const std::vector gap{command(1),command(3),command(5)};
    CHECK(!fifo.append(gap));CHECK(fifo.size()==2 && fifo.acknowledged()==0);
    CHECK(!fifo.reserve());CHECK(fifo.complete(1));
    const std::vector next{command(2),command(3)};
    CHECK(fifo.append(next));CHECK(fifo.size()==2); // Rejected batch did not consume seq3.
    const std::vector invalid_old{command(1,0,0,nan)};
    CHECK(!fifo.append(invalid_old));CHECK(fifo.size()==2 && fifo.acknowledged()==1);
    CHECK(fifo.reserve()->sequence==2);CHECK(fifo.complete(2));
    CHECK(fifo.reserve()->sequence==3);CHECK(fifo.complete(3));
}

static void generation_uses_fixed_time_and_retains_jump_edges(){
    MovementCommandHistory history;history.bind(identity,0,0.);
    auto forward=command(999,0,127,1.25f);
    history.advance(0.,forward,1);CHECK(history.after(0).empty());
    history.advance(.005,forward,1);CHECK(history.after(0).empty());
    history.advance(.010,forward,1);
    auto samples=history.after(0);CHECK(samples.size()==1);
    CHECK(samples[0].sequence==1 && samples[0].jump_pressed && samples[0].stick_y==127);
    history.advance(.030,forward,1);
    samples=history.after(0);CHECK(samples.size()==3);
    CHECK(!samples[1].jump_pressed && !samples[2].jump_pressed);
    history.advance(.030,forward,2);CHECK(history.after(0).size()==3);
    history.advance(.040,forward,2);CHECK(history.after(0).back().jump_pressed);
    history.advance(.050,forward,1);CHECK(!history.after(0).back().jump_pressed);
    const auto before=history.after(0);
    history.advance(.049,forward,9);
    history.advance(std::numeric_limits<double>::quiet_NaN(),forward,9);
    history.advance(std::numeric_limits<double>::infinity(),forward,9);
    same_commands(history.after(0),before);
    history.advance(.060,forward,0);CHECK(history.after(0).size()==6);
    CHECK(!history.after(0).back().jump_pressed);

    MovementCommandHistory unbound;
    unbound.advance(1.,forward,1);CHECK(unbound.after(0).empty());
    unbound.bind({0,42,123},0,0.);unbound.advance(.02,forward,1);
    CHECK(unbound.after(0).empty());

    MovementCommandHistory fractional;fractional.bind(identity,0,0.);
    for(int i=1;i<=20;++i)fractional.advance(i*.0005,forward,0);
    CHECK(fractional.after(0).size()==1);
    fractional.advance(.030,forward,0);CHECK(fractional.after(0).size()==3);
}

static void eighty_ms_hold_is_not_replaced_by_latest_neutral(){
    MovementCommandHistory history;history.bind(identity,0,0.);
    for(int tick=1;tick<=20;++tick)
        history.advance(tick*.01,command(0,0,tick<=8?127:0),0);
    const auto retained=history.after(0);CHECK(retained.size()==20);
    MovementCommandFIFO fifo;fifo.bind(identity);CHECK(fifo.append(retained));
    for(size_t i=0;i<retained.size();++i){
        auto actual=fifo.reserve();CHECK(actual && actual->sequence==i+1);
        CHECK(actual->stick_y==(i<8?127:0));CHECK(fifo.complete(actual->sequence));
    }
    CHECK(fifo.acknowledged()==20 && fifo.size()==0);
    history.acknowledge(8);CHECK(history.after(0).size()==12);
    CHECK(history.after(0).front().sequence==9);
    history.acknowledge(7);CHECK(history.after(0).size()==12); // Older ACK cannot retire more.
    history.acknowledge(19);CHECK(history.after(0).size()==1);
    CHECK(history.after(0).front().sequence==20);
}

static void generated_commands_survive_batching_and_delayed_ack(){
    MovementCommandHistory producer;producer.bind(identity,0,0.);
    MovementCommandFIFO receiver;receiver.bind(identity);
    std::vector<NativeMovementCommand> generated,executed;
    uint64_t delivered_ack=0;
    std::deque<uint64_t> snapshots;
    for(unsigned tick=1;tick<=140;++tick){
        if(tick<=100){
            auto input=command(0,int(tick%7)-3,tick<=8?127:tick<=12?0:tick<=20?-127:0,
                               float(tick%5)*.25f);
            producer.advance(tick*.01,input,tick==4?1:0);
            const auto latest=producer.after(tick-1);
            CHECK(latest.size()==1);generated.push_back(latest.front());
        }
        // Repeated retained histories model reliable batches containing old
        // received commands. Two initial publications are deliberately held.
        if(tick>14 && tick%7==0){
            const auto batch=producer.after(delivered_ack);
            const auto before_ack=receiver.acknowledged();
            CHECK(receiver.append(batch));CHECK(receiver.acknowledged()==before_ack);
        }
        if(auto next=receiver.reserve()){
            CHECK(!receiver.reserve()); // Reserving twice must not execute twice.
            executed.push_back(*next);
            CHECK(receiver.complete(next->sequence));
        }
        if(tick%11==0)snapshots.push_back(receiver.acknowledged());
        if(snapshots.size()>2){
            delivered_ack=snapshots.front();snapshots.pop_front();
            producer.acknowledge(delivered_ack);
        }
    }
    CHECK(receiver.append(producer.after(delivered_ack)));
    while(auto next=receiver.reserve()){
        executed.push_back(*next);CHECK(receiver.complete(next->sequence));
    }
    same_commands(executed,generated);
    CHECK(receiver.acknowledged()==100 && receiver.size()==0);
    producer.acknowledge(receiver.acknowledged());CHECK(producer.after(0).empty());
}

static void binding_changes_clear_pending_reservations_and_history(){
    const std::array changed{
        MovementStreamIdentity{8,42,123},MovementStreamIdentity{7,43,123},
        MovementStreamIdentity{7,42,124},
    };
    for(const auto& next:changed){
        MovementCommandFIFO fifo;fifo.bind(identity);
        const std::vector batch{command(1),command(2)};CHECK(fifo.append(batch));
        CHECK(fifo.reserve());fifo.bind(next);
        CHECK(fifo.identity()==next && fifo.size()==0 && fifo.acknowledged()==0);
        CHECK(!fifo.complete(1) && !fifo.reserve());
        CHECK(fifo.append(batch));CHECK(fifo.reserve()->sequence==1);CHECK(fifo.complete(1));

        MovementCommandHistory history;history.bind(identity,0,0.);
        history.advance(.015,command(0,0,127),1);CHECK(history.after(0).size()==1);
        history.bind(next,10,2.);CHECK(history.identity()==next && history.after(0).empty());
        history.advance(2.005,command(0),0);CHECK(history.after(0).empty());
        history.advance(2.010,command(0),0);
        CHECK(history.after(0).size()==1 && history.after(0).front().sequence==11);
        CHECK(!history.after(0).front().jump_pressed);
        history.reset();CHECK(!history.identity());CHECK(history.after(0).empty());
    }
    MovementCommandHistory history;history.bind(identity,0,0.);
    history.advance(.01,command(0),0);history.bind(identity,900,1.);
    CHECK(history.after(0).size()==1); // Same binding is not an ACK or a time reset.
    history.advance(.02,command(0),0);CHECK(history.after(0).back().sequence==2);
}

static void limits_are_bounded_without_partial_append(){
    MovementCommandHistory history;history.bind(identity,0,0.);
    for(size_t tick=1;tick<=movement_history_limit+100;++tick)
        history.advance(tick*.01,command(0,0,127),0);
    auto samples=history.after(0);CHECK(samples.size()==movement_history_limit);
    CHECK(samples.front().sequence==1 && samples.back().sequence==movement_history_limit);
    history.acknowledge(100);
    history.advance((movement_history_limit+101)*.01,command(0),0);
    samples=history.after(0);CHECK(samples.size()==movement_history_limit-99);
    CHECK(samples.back().sequence==movement_history_limit+1);

    MovementCommandFIFO fifo;fifo.bind(identity);
    std::vector<NativeMovementCommand> full;
    for(uint64_t sequence=1;sequence<=movement_history_limit+1;++sequence)full.push_back(command(sequence));
    CHECK(!fifo.append(full));CHECK(fifo.size()==0 && fifo.acknowledged()==0);
    CHECK(fifo.append(std::span(full).first(movement_history_limit)));
    CHECK(fifo.size()==movement_history_limit && fifo.acknowledged()==0);
    CHECK(!fifo.append(std::span(full).last(1)));CHECK(fifo.size()==movement_history_limit);
    CHECK(fifo.reserve()->sequence==1);CHECK(fifo.complete(1));
    CHECK(fifo.append(std::span(full).last(1)));CHECK(fifo.size()==movement_history_limit);
    CHECK(fifo.acknowledged()==1);
}

static void checkpoint_validation_preserves_signed_physical_state(){
    ssne::NativeMovementState original;
    original.position={1.25f,-2.f,3.5f};original.velocity={-12.5f,6.f,-3.f};
    original.phase=1;original.pending_jump=1;original.grounded=true;
    original.contact_kind=1;
    original.curve={2.f,.7f,.3f,1.f,2.f,.1f,-1.f};
    original.ground={-.25f,-1.f,2.f,3.f,.5f};
    auto encoded=encode_movement_checkpoint(original);CHECK(encoded.size()==23);
    const auto decoded=decode_movement_checkpoint(Property{encoded});CHECK(decoded);
    CHECK(decoded->position==original.position && decoded->velocity==original.velocity);
    CHECK(decoded->phase==original.phase && decoded->pending_jump==original.pending_jump);
    CHECK(decoded->grounded==original.grounded && decoded->sliding==original.sliding);
    CHECK(decoded->curve==original.curve && decoded->ground==original.ground);
    CHECK(decoded->contact_kind==original.contact_kind);
    for(float sign:{-1.f,1.f}){
        auto pushed=original;pushed.velocity={sign*25.f,sign*10.f,sign*4.f};
        auto result=decode_movement_checkpoint(Property{encode_movement_checkpoint(pushed)});
        CHECK(result && result->velocity==pushed.velocity);
    }
    CHECK(!decode_movement_checkpoint(Property{int64_t(0)}));
    auto invalid=encoded;invalid.pop_back();CHECK(!decode_movement_checkpoint(Property{invalid}));
    invalid=encoded;invalid.push_back(0);CHECK(!decode_movement_checkpoint(Property{invalid}));
    for(size_t index=0;index<encoded.size();++index){
        invalid=encoded;invalid[index]=std::numeric_limits<float>::quiet_NaN();
        CHECK(!decode_movement_checkpoint(Property{invalid}));
        invalid=encoded;invalid[index]=std::numeric_limits<float>::infinity();
        CHECK(!decode_movement_checkpoint(Property{invalid}));
    }
    for(const auto& [index,value]:std::vector<std::pair<size_t,float>>{
        {0,1000001.f},{6,2.f},{7,2.f},{8,-1.f},{9,.5f},{3,160.f},
        {10,0.f},{11,0.f},{11,2.01f},{12,-.001f},{12,.712f},{15,-.001f},{15,.201f},{22,2.f},{22,-1.f},{22,.5f},
    }){
        invalid=encoded;invalid[index]=value;CHECK(!decode_movement_checkpoint(Property{invalid}));
    }
}

static void metadata_types_are_exact(){
    Properties properties;
    CHECK(movement_unsigned(properties,"movement_ack")==0);
    properties["movement_ack"]=int64_t(17);CHECK(movement_unsigned(properties,"movement_ack")==17);
    properties["movement_ack"]=int64_t(-1);CHECK(movement_unsigned(properties,"movement_ack")==0);
    properties["movement_ack"]=17.f;CHECK(movement_unsigned(properties,"movement_ack")==0);
    for(const auto* key:{"movement_checkpoint","movement_spawn","movement_stream","movement_ack"}){
        CHECK(player_movement_metadata(EntityKind::Player,key));
        CHECK(!player_movement_metadata(EntityKind::Enemy,key));
    }
    CHECK(!player_movement_metadata(EntityKind::Player,"movement_ack_extra"));
}

static constexpr MovementTuning tuning{275.f,20.f,60.f,2.f,3.f,.8f};

struct OpenContact {
    unsigned calls=0;
    bool grounded=true;
    std::optional<MovementContact> operator()(const MovementPoint&,const MovementPoint& to,
                                              bool,MovementCollisionContext& context){
        CHECK(context.purpose==MovementResolvePurpose::Motion);
        CHECK(std::abs(context.dt-.01f)<1e-7f);
        ++calls;
        return MovementContact{to,grounded};
    }
};

static void completed_ack_replays_exactly_the_remaining_production_commands(){
    MovementCommandHistory history;history.bind(identity,0,0.);
    for(int tick=1;tick<=20;++tick)
        history.advance(tick*.01,command(0,0,tick<=8?127:0),0);
    const auto commands=history.after(0);
    const auto immutable=commands;
    ssne::NativeMovementState initial;initial.position={0,1,0};initial.grounded=true;
    CommandMovement predictor;OpenContact contact;
    CHECK(predictor.sample(0,initial,commands,tuning,contact));
    const auto original_end=predictor.state();CHECK(original_end);
    CHECK(contact.calls==20 && predictor.completed()==20);

    // This authoritative checkpoint is produced by the actual production class,
    // not a second physics formula. A delayed receipt of ACK5 must replay15 steps.
    CommandMovement prefix;OpenContact prefix_contact;
    CHECK(prefix.sample(0,initial,std::span(commands).first(5),tuning,prefix_contact));
    CHECK(prefix_contact.calls==5);
    MovementCommandFIFO native;native.bind(identity);CHECK(native.append(commands));
    for(int i=0;i<5;++i){auto reserved=native.reserve();CHECK(reserved);CHECK(native.complete(reserved->sequence));}
    CHECK(native.acknowledged()==5);
    history.acknowledge(native.acknowledged());
    const auto remainder=history.after(0);CHECK(remainder.size()==15 && remainder.front().sequence==6);
    contact.calls=0;
    CHECK(predictor.sample(native.acknowledged(),*prefix.state(),remainder,tuning,contact));
    CHECK(contact.calls==15 && predictor.completed()==20);
    CHECK(predictor.state()==original_end);
    contact.calls=0;
    CHECK(predictor.sample(native.acknowledged(),*prefix.state(),remainder,tuning,contact));
    CHECK(contact.calls==0 && predictor.state()==original_end); // Same snapshot cannot double-replay.
    same_commands(commands,immutable); // Source commands remain immutable.

    auto modified=remainder;modified[0].stick_x=1;
    CHECK(!predictor.sample(5,*prefix.state(),modified,tuning,contact));
    CHECK(!predictor.state()); // Rewriting already predicted history is rejected, not silently reused.
}

static void same_ack_signed_push_restores_and_replays_actual_integration(){
    ssne::NativeMovementState initial;initial.position={0,1,0};initial.grounded=true;
    std::vector<NativeMovementCommand> commands;
    for(uint64_t sequence=1;sequence<=12;++sequence)commands.push_back(command(sequence,0,sequence<=8?127:0));
    const auto immutable=commands;
    CommandMovement cached;OpenContact contact;
    CHECK(cached.sample(0,initial,commands,tuning,contact));
    const auto unpushed=*cached.state();
    for(float sign:{1.f,-1.f}){
        auto pushed=initial;pushed.position[0]=sign*.08f;pushed.velocity[0]=sign*12.f;
        CommandMovement fresh;OpenContact fresh_contact;
        const auto expected=fresh.sample(0,pushed,commands,tuning,fresh_contact);CHECK(expected);
        contact.calls=0;
        const auto actual=cached.sample(0,pushed,commands,tuning,contact);CHECK(actual);
        CHECK(contact.calls==commands.size()); // ACK unchanged, physical checkpoint changed.
        CHECK(cached.state()==fresh.state());
        CHECK(sign*((*actual)[0]-initial.position[0])>.1f);
        CHECK((*actual)[0]!=unpushed.position[0]); // A blanket no-backwards/stop clamp fails.
        same_commands(commands,immutable);
    }
    auto translated=initial;translated.position[0]=-.08f;
    CommandMovement fresh;OpenContact fresh_contact;
    CHECK(fresh.sample(0,translated,commands,tuning,fresh_contact));
    CHECK(cached.sample(0,translated,commands,tuning,contact));
    CHECK(cached.state()==fresh.state()); // Position-only external push, still the same ACK.
}

static void same_ack_airborne_push_keeps_sign_and_is_not_hidden_by_rest(){
    const std::vector commands{command(1)};
    ssne::NativeMovementState initial;initial.position={0,10,0};initial.phase=3;
    CommandMovement cached;OpenContact contact;contact.grounded=false;
    for(float sign:{1.f,-1.f}){
        auto pushed=initial;pushed.velocity[1]=sign*5.f;
        auto result=cached.sample(0,pushed,commands,tuning,contact);CHECK(result);
        CHECK(sign*((*result)[1]-pushed.position[1])>0.f);
        CHECK(cached.state()->phase==3 && !cached.state()->grounded);
        CHECK(cached.falling()==(sign<0.f));
    }
}

static void step_clock_charges_only_completed_time(){
    MovementStepClock clock;CHECK(clock.available()==0);CHECK(!clock.complete());
    clock.reset(0.);clock.update(0.);CHECK(clock.available()==0);
    clock.update(.005);CHECK(clock.available()==0);
    clock.update(.010);CHECK(clock.available()==1);
    clock.update(.010);CHECK(clock.available()==1);
    CHECK(clock.complete());CHECK(clock.available()==0);CHECK(!clock.complete());
    clock.reset(0.);clock.update(.2);CHECK(clock.available()==20);
    for(int i=0;i<7;++i)CHECK(clock.complete());
    CHECK(clock.available()==13);
    clock.update(.2);clock.update(.1);
    clock.update(std::numeric_limits<double>::quiet_NaN());
    clock.update(std::numeric_limits<double>::infinity());
    clock.update(-std::numeric_limits<double>::infinity());
    CHECK(clock.available()==13);
    clock.update(.205);CHECK(clock.available()==13);
    clock.update(.210);CHECK(clock.available()==14);
    for(int i=0;i<14;++i)CHECK(clock.complete());
    for(int i=0;i<100;++i)CHECK(!clock.complete());
    CHECK(clock.available()==0);
    clock.update(.220);CHECK(clock.available()==1);

    clock.reset();CHECK(clock.available()==0);
    clock.update(std::numeric_limits<double>::quiet_NaN());CHECK(clock.available()==0);
    clock.update(10.);CHECK(clock.available()==0); // First real timestamp establishes the epoch.
    clock.update(10.01);CHECK(clock.available()==1);
}

static void step_clock_long_stall_has_bounded_credit(){
    MovementStepClock clock;clock.reset(0.);
    clock.update(10.);CHECK(clock.available()==50);
    for(int i=0;i<50;++i)CHECK(clock.complete());
    CHECK(clock.available()==0 && !clock.complete());
    clock.update(10.);CHECK(clock.available()==0); // Discarded stall time cannot return.
    clock.update(10.005);CHECK(clock.available()==0);
    clock.update(10.01);CHECK(clock.available()==1);
    CHECK(clock.complete());CHECK(!clock.complete());
    clock.update(10.51);CHECK(clock.available()==50);
    clock.update(1000.);CHECK(clock.available()==50); // Credit itself also remains capped.
}

static void queue_clock_and_snapshot_ack_share_actual_completion(){
    MovementCommandFIFO fifo;fifo.bind(identity);
    std::vector<NativeMovementCommand> commands;
    for(uint64_t sequence=1;sequence<=300;++sequence)commands.push_back(command(sequence));
    CHECK(fifo.append(commands));
    MovementStepClock clock;clock.reset(0.);clock.update(.02);
    CHECK(clock.available()==2 && fifo.acknowledged()==0);
    const auto reserved=fifo.reserve();CHECK(reserved && reserved->sequence==1);
    CHECK(clock.available()==2 && fifo.acknowledged()==0); // Reserving neither spends nor ACKs.
    CHECK(!fifo.reserve());CHECK(!fifo.complete(2));
    CHECK(clock.available()==2 && fifo.acknowledged()==0);
    clock.update(.03);CHECK(clock.available()==3);
    CHECK(fifo.complete(reserved->sequence));CHECK(clock.complete());
    CHECK(clock.available()==2 && fifo.acknowledged()==1);

    uint64_t completed=1;
    const std::array times{.03,.033,.078,.095,.105,.31,.31,.31,.35,.5,.5,1.4,1.4,1.4,1.41};
    for(const double now:times){
        clock.update(now);CHECK(clock.available()<=50);
        // This models the queue/clock caller boundary. The separate actual
        // native-wrapper tests own the production catch-up iteration limit.
        for(unsigned attempt=0;attempt<4 && clock.available();++attempt){
            const auto next=fifo.reserve();CHECK(next && next->sequence==completed+1);
            const auto before=clock.available();
            CHECK(fifo.acknowledged()==completed && before>0);
            CHECK(fifo.complete(next->sequence));CHECK(clock.complete());
            ++completed;
            CHECK(clock.available()+1==before && fifo.acknowledged()==completed);
            CHECK(completed<=uint64_t(std::floor((now+1e-8)/movement_command_tick)));
        }
        const auto snapshot_ack=fifo.acknowledged();
        CHECK(snapshot_ack==completed); // Publication cannot claim reserved/received work.
    }

    // A new stream/epoch cannot inherit old queued commands, a reservation or
    // wall-time credit. The first new command needs a new real 10ms interval.
    if(!fifo.reserve())CHECK(fifo.size()==0);
    fifo.bind({identity.epoch+1,identity.entity,identity.token+1});clock.reset(2.);
    CHECK(fifo.acknowledged()==0 && fifo.size()==0 && clock.available()==0);
    CHECK(!fifo.complete(completed+1) && !clock.complete());
    const std::vector fresh{command(1)};CHECK(fifo.append(fresh));
    clock.update(2.005);CHECK(clock.available()==0 && fifo.acknowledged()==0);
    clock.update(2.010);CHECK(clock.available()==1);
    CHECK(fifo.reserve()->sequence==1);CHECK(fifo.acknowledged()==0 && clock.available()==1);
    CHECK(fifo.complete(1));CHECK(clock.complete());
    CHECK(fifo.acknowledged()==1 && clock.available()==0);

    clock.update(2.2);const auto credit=clock.available();CHECK(credit>0);
    CHECK(!fifo.reserve());CHECK(clock.available()==credit); // Underflow spends no physics time.
}

int main(){
    fifo_completion_is_the_only_ack();
    malformed_batches_are_atomic();
    generation_uses_fixed_time_and_retains_jump_edges();
    eighty_ms_hold_is_not_replaced_by_latest_neutral();
    generated_commands_survive_batching_and_delayed_ack();
    binding_changes_clear_pending_reservations_and_history();
    limits_are_bounded_without_partial_append();
    checkpoint_validation_preserves_signed_physical_state();
    metadata_types_are_exact();
    completed_ack_replays_exactly_the_remaining_production_commands();
    same_ack_signed_push_restores_and_replays_actual_integration();
    same_ack_airborne_push_keeps_sign_and_is_not_hidden_by_rest();
    step_clock_charges_only_completed_time();
    step_clock_long_stall_has_bounded_credit();
    queue_clock_and_snapshot_ack_share_actual_completion();
    std::puts("movement_command_stream: 15 production-API test groups passed");
}
