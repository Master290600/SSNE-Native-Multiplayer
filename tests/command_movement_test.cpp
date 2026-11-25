#include "command_movement.h"
#include "fixtures/native_jump_reference.h"
#include <cstdio>
#include <cstdlib>

using namespace port::coop;
using Command=ssne::NativeMovementCommand;
using State=ssne::NativeMovementState;
static unsigned checks=0;
static void require(bool pass,const char* message){++checks;if(!pass){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
static const MovementTuning tuning{275,20,20,1.25f,3.2f,.8f,.025f};
static State origin(){State s;s.grounded=true;s.contact_kind=1;return s;}
static std::optional<MovementContact> floor_contact(const MovementPoint& from,MovementPoint to,bool walking,MovementCollisionContext& query){
    require(query.dt==.01f && query.purpose==MovementResolvePurpose::Motion,"command did not execute exactly one native motion quantum");
    if(walking && movement_length(movement_difference(to,from))<tuning.minimum_ground_motion)to=from;
    const bool contact=walking || to[1]<0.f;
    to[1]=std::max(0.f,to[1]);
    return MovementContact{to,contact,false,unsigned(contact),false};
}

static void original_ground_fixtures(){
    struct Fixture{unsigned held;float released,final;};
    for(const auto test:{Fixture{8,.642274678f,.980010033f},Fixture{10,.884055793f,1.27204144f},Fixture{80,10.4499969f,10.884655f}}){
        CommandMovement movement;std::vector<Command> commands;
        for(unsigned tick=1;tick<=test.held+30;++tick){
            commands.push_back({tick,0,int8_t(tick<=test.held?127:0),false,0});
            const auto point=movement.sample(0,origin(),commands,tuning,floor_contact);
            require(point.has_value(),"valid ground command lost prediction");
            if(tick==test.held)require(std::abs((*point)[2]-test.released)<.000002f,"command movement differs from native pressed-distance fixture");
            if(tick>=test.held+8)require(std::abs((*point)[2]-test.final)<.000002f,"command movement differs from native coast fixture");
        }
    }
}

static void replay_and_ack_batches(){
    // The separate numeric fixtures above check physics. This exercises the
    // real predictor's different execution modes: one-step accumulation,
    // completed-ACK replay, unchanged checkpoints, and coalesced publication.
    std::vector<Command> all;
    for(unsigned tick=1;tick<=160;++tick)all.push_back({tick,0,int8_t(tick<=55?-128:tick<=110?127:0),false,0});
    CommandMovement reference;
    std::vector<State> states{origin()};
    for(unsigned count=1;count<=all.size();++count){
        require(reference.sample(0,states[0],std::span(all).first(count),tuning,floor_contact).has_value(),"reference command trace failed");
        states.push_back(*reference.state());
    }
    for(unsigned age:{0u,1u,24u}){
        CommandMovement remote;float previous=0;
        for(unsigned count=1;count<=all.size();++count){
            // Hold each completed native checkpoint for multiple UI frames.
            // Its sequence remains the exact number of simulated10ms quanta.
            const unsigned ack=count>age ? ((count-age)/7)*7 : 0;
            const auto pending=std::span(all).subspan(ack,count-ack);
            const auto point=remote.sample(ack,states[ack],pending,tuning,floor_contact);
            require(point && movement_length(movement_difference(*point,states[count].position))<.000002f,
                    "completed-command replay changed the native direction-reversal trajectory");
            if(count>110)require((*point)[2]>=previous-.000002f,"stale ACK pulled a released reversal backwards");
            previous=(*point)[2];
            const auto held=remote.sample(ack,states[ack],pending,tuning,floor_contact);
            require(held==point && remote.completed()==count,"unchanged snapshot advanced command physics a second time");
        }
    }
    CommandMovement batch;
    const auto point=batch.sample(0,states[0],all,tuning,floor_contact);
    require(point && movement_length(movement_difference(*point,states.back().position))<.000002f,
            "coalesced transmission changed total commanded motion");
}

static void jump_pending_and_checkpoint(){
    std::vector<Command> commands;
    CommandMovement motion;
    State first;
    for(unsigned tick=1;tick<=81;++tick){
        commands.push_back({tick,0,0,tick==1,0});
        const auto point=motion.sample(0,origin(),commands,tuning,floor_contact);
        require(point.has_value(),"native jump command failed");
        const auto state=*motion.state();
        if(tick==1){
            first=state;
            require(state.pending_jump==1 && state.phase==0 && state.position[1]==0.f,"fresh L edge skipped original pending-jump quantum");
        }else{
            require(std::abs((*point)[1]-native_jump_reference[tick-1].height)<.000003f,
                    "command jump does not match original pending/curve timing");
            require(motion.head_compression() && std::abs(*motion.head_compression()-native_jump_reference[tick-1].head)<.0000001f,
                    "command head compression lost original jump timing");
        }
    }
    CommandMovement restored;
    const auto replayed=restored.sample(1,first,std::span(commands).subspan(1),tuning,floor_contact);
    require(replayed && *replayed==motion.state()->position,"checkpoint lost a queued jump before its native execution");

    // The authority denied the queued edge. Retire that completed command,
    // restore its true pending flag, and never replay the acknowledged edge.
    State denied=origin();
    const auto denied_point=restored.sample(1,denied,std::span(commands).subspan(1),tuning,floor_contact);
    require(denied_point && (*denied_point)[1]==0.f && restored.state()->pending_jump==0,
            "authoritative jump denial was hidden by a retained local curve");
}

static void peer_motion_replays_unacknowledged_commands(){
    CommandMovement movement;
    CommandMovement::PeerPositions peers;
    const std::array<Command,1> commands{{{1,0,127,false,0}}};
    unsigned queries=0;
    auto resolve=[&](const MovementPoint& from,MovementPoint to,bool walking,MovementCollisionContext& query){
        ++queries;
        if(peers[1])to=from;
        return floor_contact(from,to,walking,query);
    };
    peers[1]=MovementPoint{0.f,0.f,2.f};
    const auto stopped=movement.sample(0,origin(),commands,tuning,resolve,peers);
    require(stopped==origin().position,"initial replay missed the occupied passage");
    require(movement.sample(0,origin(),commands,tuning,resolve,peers)==stopped && queries==1,
            "an unchanged player pose rebuilt the same replay");
    peers[1].reset();
    const auto clear=movement.sample(0,origin(),commands,tuning,resolve,peers);
    require(clear==MovementPoint{0.f,0.f,.0275f} && queries==2 && movement.completed()==1,
            "a cleared passage was ignored until the next movement ACK");
    peers[1]=MovementPoint{0.f,0.f,2.f};
    require(movement.sample(0,origin(),commands,tuning,resolve,peers)==stopped && queries==3,
            "a player entering the passage did not invalidate the cached replay");
}

static void camera_reconciles_a_cleared_passage(){
    CommandMovement movement;
    const std::array<Command,1> commands{{{1,0,127,false,0}}};
    const auto block=[](const MovementPoint& from,const MovementPoint&,bool,MovementCollisionContext&){
        MovementContact contact{from,true,false,1,false};
        contact.player_contact=true;
        return std::optional<MovementContact>{contact};
    };
    const auto previous=movement.sample(0,origin(),commands,tuning,block);
    const auto sweep=[](const MovementPoint&,const MovementPoint& to){return std::optional<MovementPoint>{to};};
    require(previous==origin().position && movement.camera_position(1.,tuning,sweep)==*previous,
            "touching a player moved the view away from the predicted collision");

    auto confirmed=origin();
    confirmed.position[2]=4.f;
    const std::array<Command,1> next{{{2,0,127,false,0}}};
    const auto rebased=movement.sample(1,confirmed,next,tuning,block);
    const auto display=movement.camera_position(1.02,tuning,sweep);
    require(rebased==confirmed.position && display[2]>0.f && display[2]<.4f,
            "a teammate moving during contact jumped directly to the rebased body");
    movement.sample(2,confirmed,{},tuning,floor_contact);
    for(unsigned frame=2;frame<=16;++frame)
        movement.camera_position(1.+.02*frame,tuning,sweep);
    require(movement.camera_position(1.34,tuning,sweep)==confirmed.position &&
            movement.state()->position==confirmed.position,
            "the camera did not settle at the unchanged physical position");
}

static void pending_lift_and_fallback(){
    CommandMovement motion;State anchor=origin();anchor.position[1]=1.f;anchor.ground[0]=.4f;
    std::vector<Command> commands{{11,0,0,false,0}};
    unsigned queries=0;
    auto lift=[&](const MovementPoint&,MovementPoint to,bool walking,MovementCollisionContext& query){
        ++queries;require(walking && query.dt==.01f,"pending step used a nonnative clock");
        to[1]+=query.ground_state[0];query.ground_state={};
        return std::optional<MovementContact>{{to,true,false,1,false}};
    };
    auto point=motion.sample(10,anchor,commands,tuning,lift);
    require(point && std::abs((*point)[1]-1.4f)<.000001f,"resting command discarded a pending lift");
    motion.sample(10,anchor,commands,tuning,lift);
    require(queries==1,"reused checkpoint consumed a native lift twice");
    commands.push_back({12,0,0,false,0});motion.sample(10,anchor,commands,tuning,lift);
    require(queries==2 && std::abs(motion.state()->position[1]-1.4f)<.000001f,"next command replayed an already consumed lift");
    auto unavailable=[](const MovementPoint&,const MovementPoint&,bool,MovementCollisionContext&)->std::optional<MovementContact>{return {};};
    anchor.position[0]=1.f;
    require(!motion.sample(10,anchor,commands,tuning,unavailable) && !motion.state(),"unsafe dynamic collision retained a prediction lease");
    require(motion.sample(10,anchor,commands,tuning,lift) && commands.size()==2,"collision fallback discarded transport command history");
}

static void command_validation(){
    CommandMovement motion;std::vector<Command> commands{{1,0,127,false,0}};
    require(motion.sample(0,origin(),commands,tuning,floor_contact).has_value(),"valid initial command rejected");
    commands[0].stick_y=-128;
    require(!motion.sample(0,origin(),commands,tuning,floor_contact),"mutated historical command was silently reused");
    commands[0].sequence=2;
    require(!motion.sample(0,origin(),commands,tuning,floor_contact),"missing movement command was invented");
    commands[0].sequence=1;commands[0].yaw=std::numeric_limits<float>::quiet_NaN();
    require(!motion.sample(0,origin(),commands,tuning,floor_contact),"nonfinite movement heading accepted");
    auto unsupported=origin();unsupported.phase=2;
    require(!motion.sample(0,unsupported,{},tuning,floor_contact),"jump-pad phase was approximated as a normal jump");
    commands.assign(513,{});
    require(!motion.sample(0,origin(),commands,tuning,floor_contact),"unbounded command replay accepted");
}

static void airborne_ground_state(){
    // Actor708..718 survives the switch to air. The original collision
    // controller still receives these fields and advances its cooldown even
    // when the phase0 ground-support cast is disabled.
    State airborne=origin();airborne.phase=3;airborne.grounded=false;airborne.contact_kind=0;
    airborne.position[1]=5.f;airborne.ground={.4f,3.f,4.f,5.f,.2f};
    CommandMovement motion;
    std::vector<Command> commands{{11,0,0,false,0},{12,0,0,false,0}};
    unsigned queries=0;
    auto air=[&](const MovementPoint&,const MovementPoint& to,bool walking,MovementCollisionContext& query){
        require(!walking && query.dt==.01f,"air checkpoint invoked a walking support cast");
        require(query.ground_state[0]==.4f && query.ground_state[1]==3.f &&
                query.ground_state[2]==4.f && query.ground_state[3]==5.f,
                "air prediction discarded pending lift or its original contact point");
        const float expected=queries?float(.2f-.01f):.2f;
        require(std::abs(query.ground_state[4]-expected)<.0000001f,
                "air prediction lost the previous collision cooldown output");
        query.ground_state[4]=std::max(0.f,query.ground_state[4]-.01f);++queries;
        return std::optional<MovementContact>{{to,false,false,0,false}};
    };
    const auto point=motion.sample(10,airborne,commands,tuning,air);
    require(point && (*point)[1]<5.f && queries==2,"air commands did not run the retained collision state");
    const auto state=motion.state();
    require(state && state->ground[0]==.4f && std::abs(state->ground[4]-.18f)<.0000001f,
            "completed air commands did not retain the native cooldown/pending state");
    motion.sample(10,airborne,commands,tuning,air);
    require(queries==2,"unchanged air checkpoint decremented cooldown twice");
}

static void original_sliding_fixtures(){
    // Numeric outputs measured from PAL800A8AB4 and its original paired-single
    // vector integration. The first fixture is the actual curb regression.
    struct Fixture{MovementPoint before,after,displacement;};
    const std::array fixtures{
        Fixture{{0.f,0.f,13.3629713f},{0.f,-.48999998f,13.4016743f},{0.f,-.00489999959f,.134016737f}},
        Fixture{{0.f,0.f,-13.7498741f},{0.f,-.48999998f,-10.9998865f},{0.f,-.00489999959f,-.109998859f}},
        Fixture{{2.70000005f,-.49000001f,0.f},{2.43000007f,-.980000019f,1.375f},{.0242999997f,-.00980000012f,.0137499999f}},
        Fixture{{0.f,-147.f,0.f},{0.f,-147.490005f,1.375f},{0.f,-1.47490001f,.0137499999f}},
        Fixture{{0.f,-147.490005f,0.f},{0.f,-147.f,1.375f},{0.f,-1.46999991f,.0137499999f}}
    };
    for(const bool boosted:{false,true})for(const bool grounded:{false,true})for(const auto& test:fixtures){
        auto tune=tuning;
        if(boosted){tune.ground_acceleration=1000.f;tune.ground_decay=6.f;}
        tune.default_ground_acceleration=275.f;tune.default_ground_decay=20.f;
        State anchor=origin();anchor.grounded=grounded;anchor.sliding=true;anchor.velocity=test.before;
        anchor.ground={.3125f,3.f,4.f,5.f,.8f};
        const std::array<Command,1>commands{{{1,0,127,false,0}}};
        CommandMovement motion;unsigned queries=0;
        auto contact=[&](const MovementPoint&,const MovementPoint& to,bool walking,MovementCollisionContext& query){
            ++queries;
            require(walking,"phase0 sliding changed the original ground-support query");
            require(to==test.displacement,"sliding requested displacement differs from original PAL numeric output");
            require(query.ground_state==anchor.ground,"sliding dropped its pending step/contact/cooldown");
            return std::optional<MovementContact>{{to,grounded,false,1,true}};
        };
        const auto point=motion.sample(0,anchor,commands,tune,contact);
        require(point && motion.state()->velocity==test.after,
                "phase0 sliding force or gravity differs from original PAL numeric output");
        require(motion.state()->phase==0 && motion.state()->sliding && motion.state()->ground==anchor.ground,
                "sliding contact lost native phase or pending collision state");
        motion.sample(0,anchor,commands,tune,contact);
        require(queries==1,"unchanged sliding checkpoint advanced the native command twice");
    }
    // The slope flag, not merely an absent support flag, selects half force.
    State unsupported=origin();unsupported.grounded=false;
    const std::array<Command,1>commands{{{1,0,127,false,0}}};
    CommandMovement normal;
    const auto no_slide=normal.sample(0,unsupported,commands,tuning,
        [](const MovementPoint&,const MovementPoint& to,bool walking,MovementCollisionContext&){
            require(walking && to==MovementPoint{0,0,.0275f},"unsupported non-sliding phase0 used half force or gravity");
            return std::optional<MovementContact>{{to,false,false,1,false}};
        });
    require(no_slide && normal.state()->velocity==MovementPoint{0,0,2.75f},
            "missing support alone selected the sliding force");
    State airborne=origin();airborne.phase=3;airborne.grounded=false;airborne.sliding=true;airborne.contact_kind=0;
    CommandMovement air;
    const auto in_air=air.sample(0,airborne,commands,tuning,
        [](const MovementPoint&,const MovementPoint& to,bool walking,MovementCollisionContext&){
            require(!walking,"airborne slope flag enabled a ground-support cast");
            return std::optional<MovementContact>{{to,false,false,0,true}};
        });
    require(in_air && air.state()->velocity==MovementPoint{0,-.97999996f,.199999988f},
            "airborne slope flag replaced original full gravity/air acceleration");
}

static void original_air_fall_fixtures(){
    // PAL800A8A50 applies full gravity in the air without the terminal-velocity
    // branch used only by phase0 sliding. Values are original numeric outputs.
    struct Fixture{float before,after,displacement;};
    for(const bool slope_flag:{false,true})for(const auto test:{
            Fixture{-146.899994f,-147.87999f,-1.47879982f},
            Fixture{-147.f,-147.979996f,-1.47979987f},
            Fixture{-147.490005f,-148.470001f,-1.48469996f}}){
        State anchor=origin();anchor.phase=3;anchor.grounded=false;anchor.contact_kind=0;
        anchor.sliding=slope_flag;anchor.velocity[1]=test.before;
        const std::array<Command,1>commands{{{1,0,0,false,0}}};
        CommandMovement motion;
        const auto point=motion.sample(0,anchor,commands,tuning,
            [&](const MovementPoint&,const MovementPoint& to,bool walking,MovementCollisionContext&){
                require(!walking && to==MovementPoint{0,test.displacement,0},
                        "Deep air fall used a sliding terminal cap or changed original displacement");
                return std::optional<MovementContact>{{to,false,false,0,slope_flag}};
            });
        require(point && motion.state()->velocity[1]==test.after && motion.state()->phase==3,
                "Deep air fall differs from original native gravity integration");
    }
    // Leaving the explicitly supported state range still yields authority
    // fallback; removing the artificial cap must not widen the safety bounds.
    State unsupported=origin();unsupported.phase=3;unsupported.grounded=false;unsupported.contact_kind=0;
    unsupported.velocity[1]=-159.5f;
    const std::array<Command,1>commands{{{1,0,0,false,0}}};
    CommandMovement motion;
    require(!motion.sample(0,unsupported,commands,tuning,
        [](const MovementPoint&,const MovementPoint& to,bool,MovementCollisionContext&){
            return std::optional<MovementContact>{{to,false,false,0,false}};
        }),"Air fall beyond the supported160m/s range did not return authority fallback");
}

int main(){
    original_ground_fixtures();replay_and_ack_batches();jump_pending_and_checkpoint();pending_lift_and_fallback();command_validation();
    airborne_ground_state();original_sliding_fixtures();original_air_fall_fixtures();
    peer_motion_replays_unacknowledged_commands();camera_reconciles_a_cleared_passage();
    std::printf("Command movement: %u checks passed\n",checks);
}
