#include "local_movement.h"
#include <cstdio>
#include <cstdlib>

using namespace port::coop;

static void require(bool condition,const char* message){
    if(!condition){std::fprintf(stderr,"%s\n",message);std::exit(1);}
}

static void short_step(int fps){
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent input{};
    const MovementTuning tune{240,20,60,2,3,.6f};
    auto ground=[](const MovementPoint&,const MovementPoint& to,bool){
        return std::optional<MovementContact>{{to,true}};
    };
    unsigned frame=0;
    auto next=[&](){
        input.sequence=frame+1;
        return motion.sample(double(frame++)/fps,host,input,tune,ground);
    };
    input.forward=1;
    std::optional<MovementPoint> view;
    for(int i=0;i<fps/8;++i)view=next();
    require(view && (*view)[2]>.1f,"short step did not respond to local input");
    input.forward=0;
    for(int i=0;i<fps;++i)view=next();
    require(view.has_value(),"pending short step dropped the local camera");
    // A late snapshot confirms a slightly shorter voluntary step. Reach that
    // endpoint gradually without leaving the eye detached from the player.
    const auto before=*view;
    host.position=before;
    host.position[2]-=.075f;
    float last=before[2];
    for(int i=0;i<2*fps;++i){
        host.acknowledged=input.sequence;
        view=next();
        require(view.has_value(),"settled local eye handed off to the older interpolation buffer");
        require(std::abs((*view)[2]-last)<.022f,"single-frame camera jump on small correction");
        last=(*view)[2];
    }
    require(std::abs(last-host.position[2])<.001f,"acknowledged stop retained a permanent visual offset");

    auto resumed=motion;
    auto lateral=input;
    lateral.right=1;
    auto eye=*view;
    for(int i=0;i<fps/2;++i){
        lateral.sequence++;
        auto moving=resumed.sample((frame+i)/double(fps),host,lateral,tune,ground);
        require(moving && std::abs((*moving)[2]-eye[2])<.025f,"new movement snapped away the stop offset");
        eye=*moving;
    }
    require(eye[0]>.1f && std::abs(eye[2]-host.position[2])<.001f,"walking again retained the old stop offset");

    auto blocked=motion;
    auto wall_host=host;
    wall_host.position[2]+=.0002f;
    const float wall=wall_host.position[2]+.02f;
    const auto wall_contact=[&](const MovementPoint&,MovementPoint to,bool){
        to[2]=std::min(wall,to[2]);
        return std::optional<MovementContact>{{to,true}};
    };
    const auto blocked_eye=blocked.sample(frame/double(fps),wall_host,input,tune,wall_contact);
    require(blocked_eye && (*blocked_eye)[2]<=wall,"held visual offset crossed a wall");

    host.position[2]-=.08f;
    for(int i=0;i<fps;++i)view=next();
    require(view && std::abs((*view)[2]-host.position[2])<.001f,
            "a real 8cm push after rest was hidden");
    host.position[0]+=128;
    view=next();
    require(!view || *view==host.position,"a teleport retained old prediction");
    require(next()==std::optional<MovementPoint>{host.position},"failed to reacquire a teleported actor");
}

static void closing_door(){
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent walk{};
    walk.forward=1;
    const MovementTuning tune{240,20,60,2,3,.6f};
    for(int i=0;i<30;++i){
        walk.sequence=i+1;
        motion.sample(i/60.,host,walk,tune,[](const MovementPoint&,const MovementPoint& to,bool){
            return std::optional<MovementContact>{{to,true}};
        });
    }
    host.position[2]=.8f;
    auto near_side=[&](const MovementPoint& from,MovementPoint to){
        require(from==host.position,"door sweep did not start on the confirmed side");
        to[2]=std::min(to[2],1.f);
        return to;
    };
    auto eye=motion.follow_brush_push(.5,host.position,near_side);
    require(eye[2]>host.position[2] && eye[2]<=1.f,"closing door left the local eye on the wrong side");
    for(int i=1;i<=60;++i){
        auto next=motion.follow_brush_push(.5+i/60.,host.position,near_side);
        require(next[2]<=eye[2] && next[2]>=host.position[2],"door push replayed an old camera position");
        eye=next;
    }
    require(std::abs(eye[2]-host.position[2])<.001f,"door push did not converge");
}

static void deferred_ground_step(){
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent input{1,0,0,1,0};
    const MovementTuning tune{240,20,60,2,3,.6f};
    unsigned moves=0,corrections=0,supports=0;
    bool check_reset=false;
    auto ground=[&](const MovementPoint&,MovementPoint to,bool walking,MovementCollisionContext& query){
        if(query.purpose!=MovementResolvePurpose::Motion){
            require(query.dt==0.f,"a ground probe advanced controller time");
            for(float field:query.ground_state)require(field==0.f,"a probe consumed a pending lift");
            query.ground_state[0]=999.f; // Probe output must never reach the walking controller.
            if(query.purpose==MovementResolvePurpose::Correction)++corrections;else ++supports;
        }else if(walking){
            require(query.dt==.01f,"walking used a different clock from the native controller");
            if(check_reset){
                for(float field:query.ground_state)require(field==0.f,"reset retained a pending ground step");
                check_reset=false;
            }
            if(++moves==1)query.ground_state[0]=.4f;
            else if(moves==2){
                require(query.ground_state[0]==.4f,"a correction lost the next-tick lift");
                to[1]+=query.ground_state[0];
                query.ground_state={};
            }
        }else{
            for(float field:query.ground_state)require(field==0.f,"an airborne move retained ground state");
        }
        return std::optional<MovementContact>{{to,true}};
    };
    motion.sample(0.,host,input,tune,ground);
    const auto first=motion.sample(.01,host,input,tune,ground);
    require(first && (*first)[1]==1.f,"a step was lifted on the contact tick");
    host.position[0]=.2f;
    input.sequence=2;
    const auto second=motion.sample(.020001,host,input,tune,ground);
    require(second && std::abs((*second)[1]-1.4f)<.001f,"next walking tick did not consume the lift once");
    require(corrections>0,"the deferred-step case did not exercise reconciliation");
    input.sequence=input.jump=3;
    motion.sample(.030002,host,input,tune,ground);
    require(supports>0,"the deferred-step case did not exercise support probes");
    motion.reset();
    check_reset=true;
    host.position={0,1,0};
    input={4,0,0,1,0};
    motion.sample(1.,host,input,tune,ground);
    motion.sample(1.02,host,input,tune,ground);
    require(!check_reset,"reset case did not simulate movement");
}

static void temporary_contact(){
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent input{1,0,0,1,0};
    const MovementTuning tune{240,20,60,2,3,.6f};
    const auto ground=[](const MovementPoint&,const MovementPoint& to,bool){
        return std::optional<MovementContact>{{to,true}};
    };
    for(unsigned i=0;i<=30;++i){
        input.sequence=i+1;
        motion.sample(i/60.,host,input,tune,ground);
    }
    host.position[2]=.8f;
    auto boundary=[&](const MovementPoint& from,MovementPoint to){
        require(from==host.position,"temporary contact did not clip from the confirmed actor");
        to[2]=std::min(to[2],1.f);
        return to;
    };
    const auto held=motion.follow_brush_push(.52,host.position,boundary,true);
    require(held[2]>=host.position[2] && held[2]<=1.f,"temporary contact crossed its confirmed boundary");
    host.velocity={0,0,12};
    const auto resumed=motion.sample(.54,host,input,tune,ground);
    require(resumed && (*resumed)[2]-held[2]>.1f,"resuming after a contact restarted walking from rest");
}

static void unsupported_contact_acknowledgement(){
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent input{1,0,0,1,0};
    const MovementTuning tune{240,20,60,2,3,.6f};
    const auto ground=[](const MovementPoint&,const MovementPoint& to,bool){
        return std::optional<MovementContact>{{to,true}};
    };
    for(unsigned i=0;i<=30;++i){
        input.sequence=i+1;
        motion.sample(i/60.,host,input,tune,ground);
    }
    const auto clear=[](const MovementPoint&,const MovementPoint& to){return to;};
    host.position[2]=4.4f;
    const auto first=motion.follow_brush_push(.52,host.position,clear,true);
    host.position[2]+=.2f;
    const auto caught_up=motion.follow_brush_push(.56,host.position,clear,true);
    require(caught_up==first,"a host advancing toward its local eye pulled the eye backwards");

    auto pushed=motion;
    host.position[2]-=.1f;
    const auto back=pushed.follow_brush_push(.57,host.position,clear,true);
    require(back[2]<caught_up[2],"a real confirmed backward push was hidden");
    host.position[2]+=.1f;
    const float wall=host.position[2]+.01f;
    auto clipped=motion;
    const auto at_wall=clipped.follow_brush_push(.57,host.position,[&](const MovementPoint&,MovementPoint to){
        to[2]=std::min(to[2],wall);return to;
    },true);
    require(at_wall[2]<=wall,"temporary eye hold passed through a confirmed collision boundary");

    const auto expired=motion.follow_brush_push(.68,host.position,clear,true);
    require(expired[2]<caught_up[2],"unsupported contact held a stale eye past 150ms");
    MovementPoint eye=expired;
    for(unsigned i=1;i<=100;++i)eye=motion.follow_brush_push(.68+i*.01,host.position,clear,true);
    require(movement_length(movement_difference(eye,host.position))<.001f,
            "expired unsupported contact did not converge to the confirmed actor");
}

static void native_cutoff_rest(){
    const MovementTuning tune{275,20,20,1.25f,3.2f,.8f,.025f};
    for(unsigned lag:{0u,3u,10u,24u})for(unsigned held:{4u,8u,10u,80u}){
        struct State {MovementPoint position{},velocity{};};
        std::array<State,500> history{};
        LocalMovement movement;
        MovementPoint eye{};
        unsigned correction_queries=0;
        auto ground=[&](const MovementPoint& from,const MovementPoint& to,bool,MovementCollisionContext& query){
            auto result=to;
            if(query.purpose==MovementResolvePurpose::Motion &&
               movement_length(movement_difference(to,from))<tune.minimum_ground_motion)result=from;
            if(query.purpose==MovementResolvePurpose::Correction)++correction_queries;
            return std::optional<MovementContact>{{result,true}};
        };
        for(unsigned tick=1;tick<history.size();++tick){
            const bool pressed=tick>=10 && tick<10+held;
            auto host=history[tick-1];
            host.velocity[2]+=(tune.ground_acceleration*pressed-tune.ground_decay*host.velocity[2])*.01f;
            if(std::abs(host.velocity[2]*.01f)>=tune.minimum_ground_motion)
                host.position[2]+=host.velocity[2]*.01f;
            history[tick]=host;
            const auto ack=tick>lag?tick-lag:0;
            const auto& remote=history[ack];
            const MovementAnchor anchor{remote.position,remote.velocity,ack,true};
            const MovementIntent input{tick,0,0,float(pressed),0};
            const auto previous=eye;
            if(auto view=movement.sample(tick*.010000001,anchor,input,tune,ground))eye=*view;
            if(tick>200){
                require(movement_length(movement_difference(eye,remote.position))<.002f,
                        "PAL walking cutoff left a permanent resting camera error");
                require(movement_length(movement_difference(eye,previous))<.002f,
                        "a settled camera continued correcting or snapped");
            }
        }
        require(correction_queries>0,"cutoff regression did not exercise collision-safe corrections");
    }
}

static void native_visible_rest(){
    const MovementTuning tune{275,20,20,1.25f,3.2f,.8f,.025f};
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent input{1,0,0,1,0};
    auto ground=[&](const MovementPoint& from,const MovementPoint& to,bool,MovementCollisionContext& query){
        const auto at=query.purpose==MovementResolvePurpose::Motion &&
            movement_length(movement_difference(to,from))<tune.minimum_ground_motion ? from : to;
        return std::optional<MovementContact>{{at,true}};
    };
    unsigned tick=0;
    const auto sample=[&](){
        input.sequence=++tick;
        return motion.sample(tick*.010000001,host,input,tune,ground).value();
    };
    motion.sample(0.,host,input,tune,ground);
    for(unsigned i=0;i<10;++i)sample();
    input.forward=0;
    auto eye=sample();
    for(unsigned i=0;i<8;++i)eye=sample();
    // The full local tap is still unacknowledged. A stale quiet host must
    // not reset that prediction just because its visible body is stationary.
    require(eye[2]>1.f,"quiet old authority discarded an unacknowledged tap");
    host.position=eye;
    host.position[2]-=.075f;
    host.velocity[2]=2.226f; // Measured PAL residual velocity after visible walking has stopped.
    for(unsigned i=0;i<12;++i){
        host.acknowledged=input.sequence;
        eye=sample();
        host.velocity[2]*=.8f;
    }
    require(host.velocity[2]>.06f,"visible-rest fixture waited until velocity was nearly zero");
    require(std::abs(eye[2]-host.position[2])<.025f,
            "visible stop waited for near-zero velocity before retiring its camera offset");
    for(unsigned i=0;i<70;++i){host.acknowledged=input.sequence;eye=sample();}
    require(std::abs(eye[2]-host.position[2])<.001f,"visible-rest correction failed to converge");

    // A later real push must still move the eye even though the old walking
    // velocity remains below the capsule's displacement cutoff.
    const auto before=eye;
    host.position[2]-=.08f;
    for(unsigned i=0;i<70;++i){host.acknowledged=input.sequence;eye=sample();}
    require(eye[2]<before[2]-.07f && std::abs(eye[2]-host.position[2])<.001f,
            "native visible-rest criterion hid a confirmed push");
}

static void resting_pending_lift(){
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent input{1,0,0,1,0};
    const MovementTuning tune{240,20,60,2,3,.6f,.025f};
    unsigned motions=0;
    auto ground=[&](const MovementPoint&,MovementPoint to,bool walking,MovementCollisionContext& query){
        if(query.purpose==MovementResolvePurpose::Motion && walking){
            if(++motions==1)query.ground_state[0]=.4f;
            else if(motions==2){
                require(query.ground_state[0]==.4f,"resting discarded the pending native lift");
                to[1]+=query.ground_state[0];
                query.ground_state={};
            }
        }
        return std::optional<MovementContact>{{to,true}};
    };
    motion.sample(0.,host,input,tune,ground);
    const auto first=motion.sample(.010000001,host,input,tune,ground);
    require(first && (*first)[1]==1.f,"pending lift advanced on the contact tick");
    host.position=*first;host.acknowledged=1;
    input={2,0,0,0,0};
    const auto second=motion.sample(.020000002,host,input,tune,ground);
    require(motions==2 && second && std::abs((*second)[1]-1.4f)<.001f,
            "quiet grounded rest skipped the next-tick native lift");

    // A vertical support change is a collision-resolved correction, even if
    // its horizontal residual speed is already under the native cutoff.
    unsigned corrections=0;
    auto support=[&](const MovementPoint&,const MovementPoint& to,bool,MovementCollisionContext& query){
        if(query.purpose==MovementResolvePurpose::Correction)++corrections;
        return std::optional<MovementContact>{{to,true}};
    };
    host.position=*second;host.acknowledged=2;host.velocity={0,.2f,0};
    input.sequence=3;
    motion.sample(.030000003,host,input,tune,support);
    host.position[1]+=.02f;
    motion.sample(.040000004,host,input,tune,support);
    require(corrections>0,"visible-rest shortcut bypassed vertical support correction");

    // Without a native displacement cutoff, retain the tiny-speed fallback
    // and allow real sub-centimetre walking motion to continue.
    LocalMovement no_cutoff;
    const MovementTuning continuous{275,20,20,1.25f,3.2f,.8f,0.f};
    host={{0,1,0},{0,0,.1f},0,true};input={1,0,0,0,0};
    no_cutoff.sample(0.,host,input,continuous,support);
    const auto moving=no_cutoff.sample(.010000001,host,input,continuous,support);
    require(moving && (*moving)[2]>.0007f,"zero-cutoff walking was declared stopped prematurely");
}

int main(){
    native_visible_rest();
    resting_pending_lift();
    native_cutoff_rest();
    unsupported_contact_acknowledgement();
    temporary_contact();
    deferred_ground_step();
    closing_door();
    for(int fps:{60,144,240})short_step(fps);
    // On a delayed connection the last snapshot at key-up can still show
    // the untouched starting position. Continue a local coast while it is
    // in flight. A later confirmed relocation still owns the resting eye.
    LocalMovement motion;
    MovementAnchor host{{0,1,0},{},0,true};
    MovementIntent input{};
    const MovementTuning tune{240,20,60,2,3,.6f};
    const auto flat=[](const MovementPoint& from,const MovementPoint& to,bool,MovementCollisionContext& query){
        const auto resolved=query.purpose==MovementResolvePurpose::Motion &&
            movement_length(movement_difference(to,from))<.025f ? from : to;
        return std::optional<MovementContact>{{resolved,true}};
    };
    int tick=0;
    const auto sample=[&](){
        input.sequence=uint64_t(tick+1);
        return motion.sample(++tick/60.,host,input,tune,flat).value();
    };
    input.forward=1;
    for(int i=0;i<10;++i)sample();
    const float release=sample()[2];
    input.forward=0;
    auto previous=release;
    for(int i=0;i<15;++i){
        const float now=sample()[2];
        require(now>=previous-.001f,"stale host rewound an unconfirmed step");
        previous=now;
    }
    require(previous>release+.03f,"neutral input froze the eye while the step was in flight");
    host.position[2]=previous+.1f;
    for(int i=0;i<120;++i){
        host.acknowledged=input.sequence;
        sample();
    }
    require(sample()[2]==host.position[2],"late authoritative rest was not reached");

    // Measured on PAL flat ground. The velocity keeps falling after the
    // walking controller has dropped its last sub-2.5cm displacement.
    motion.reset();
    host.position={0,1,0};
    for(float speed:{2.226f,1.424f,.373f,.009f}){
        host.velocity={0,0,speed};
        for(int i=0;i<4;++i){
            host.acknowledged=input.sequence;
            require(sample()==host.position,"reported braking velocity moved a stationary eye");
        }
    }
}
