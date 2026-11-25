#include "local_movement.h"
#include "fixtures/native_jump_reference.h"
#include <cstdio>
#include <cstdlib>

using namespace port::coop;

static unsigned checks=0;
static void require(bool pass,const char* message){
    ++checks;
    if(!pass){std::fprintf(stderr,"%s\n",message);std::exit(1);}
}

static void stick_actions(){
    // Numeric observations from PAL's original pad clamp/input processor,
    // including its asymmetric partial diagonals and signed-byte rounding.
    struct Fixture {int x,y;float right,forward;};
    const Fixture fixtures[]={
        {0,0,0,0},{15,-15,0,0},{40,40,.472222209f,.472222209f},
        {60,20,.652777791f,.0972222239f},{70,0,.763888896f,0},
        {127,127,.707106769f,.707106769f},{-128,-128,-.707106769f,-.707106769f},
        {127,0,1,0},{0,-128,0,-1},{25,25,.152777776f,.152777776f},
        {-50,-45,-.722222209f,-.680555582f},{-40,-40,-.472222209f,-.472222209f},
        {40,-40,.472222209f,-.472222209f},{-40,40,-.472222209f,.472222209f},
        {50,-45,.722222209f,-.680555582f},{-50,45,-.722222209f,.680555582f},
        {-20,60,-.0972222239f,.652777791f},{20,-60,.138888896f,-.652777791f},
        {-70,30,-.866622448f,.498964459f},{70,-30,.916666687f,-.25f}
    };
    for(const auto& expected:fixtures){
        const auto action=native_movement_axes(uint8_t(expected.x+128),uint8_t(expected.y+128));
        require(std::abs(action[0]-expected.right)<1e-7f &&
                std::abs(action[1]-expected.forward)<1e-7f,"stick actions differ from native fixture");
    }
    for(unsigned x=0;x<256;++x)for(unsigned y=0;y<256;++y){
        const auto action=native_movement_axes(uint8_t(x),uint8_t(y));
        require(std::isfinite(action[0]) && std::isfinite(action[1]) &&
                std::hypot(action[0],action[1])<=1.0000002f,"stick actions are nonfinite or exceed native speed");
    }
}

static void jump_curve(){
    // Captured from the original numeric controller at its 10ms clock. The
    // first tick stays at launch height; the last curve displacement seeds
    // falling. A nonzero launch height also checks native float subtraction.
    struct Fixture {
        float start,duration,second,third;
        unsigned fall_tick;
        float last_curve,first_fall;
        unsigned peak_tick,land_tick;
    };
    const Fixture fixtures[]={
        {0,.12f,.828220963f,1.60000002f,7,3.20000005f,3.24471879f,12,39},
        {0,.3f,.334491074f,.665317416f,17,3.18247008f,3.16390514f,16,42},
        {0,.8f,.125631407f,.251069099f,45,3.16060305f,3.14219499f,41,70},
        {0,1,.100514434f,.200929672f,56,3.16060305f,3.14372087f,51,81},
        {0,2,.050263416f,.100514434f,111,3.16060328f,3.14706683f,101,136},
        {8.4f,.12f,9.22822094f,10.f,7,11.5999994f,11.6447182f,12,39},
        {8.4f,.3f,8.73449039f,9.06531715f,17,11.5824699f,11.5639048f,16,42},
        {8.4f,.8f,8.52563095f,8.65106869f,45,11.5606022f,11.5421934f,41,70},
        {8.4f,1,8.50051403f,8.60092926f,56,11.5606022f,11.5437193f,51,81},
        {8.4f,2,8.45026302f,8.50051403f,111,11.5606031f,11.5470667f,101,136}
    };
    for(const auto& expected:fixtures){
        LocalMovement movement;
        const MovementTuning tuning{275,20,20,1.25f,3.2f,expected.duration,.025f};
        const MovementAnchor anchor{{0,expected.start,0},{},0,true};
        const MovementIntent input{1,1,0,0,0};
        auto floor=[&](const MovementPoint&,MovementPoint to,bool,MovementCollisionContext&){
            to[1]=std::max(expected.start,to[1]);
            return std::optional<MovementContact>{{to,to[1]<=expected.start}};
        };
        movement.sample(0.,anchor,input,tuning,floor);
        float peak=expected.start;
        unsigned peak_tick=0,land_tick=0;
        for(unsigned tick=1;tick<160;++tick){
            const auto point=movement.sample(tick*.010000001,anchor,input,tuning,floor);
            require(point.has_value(),"jump prediction unexpectedly abandoned a safe floor");
            const float y=(*point)[1];
            auto near=[&](float native){require(std::abs(y-native)<.000002f,"jump position differs from native fixture");};
            if(tick==1)near(expected.start);
            if(tick==2)near(expected.second);
            if(tick==3)near(expected.third);
            if(tick==expected.fall_tick)near(expected.last_curve);
            if(tick==expected.fall_tick+1)near(expected.first_fall);
            if(y>peak){peak=y;peak_tick=tick;}
            if(tick>peak_tick && peak>expected.start+1 && y<=expected.start && !land_tick)land_tick=tick;
        }
        require(peak_tick==expected.peak_tick,"jump peak occurs on a different native tick");
        require(land_tick==expected.land_tick,"jump landing occurs on a different native tick");
    }
}

static void native_coasting(){
    // Independent numeric observations from PAL 800A8BAC's stock walking
    // controller, including the first neutral tick and its displacement
    // cutoff. Delayed snapshots must not replace this coast with a separate
    // estimated stop target or replay it after the acknowledgement arrives.
    struct Fixture {unsigned held;float released_velocity;std::array<float,9> positions;};
    const Fixture fixtures[]={
        {8,11.4431324f,{.642274678f,.733819723f,.807055771f,.865644634f,.9125157f,
                       .950012565f,.980010033f,.980010033f,.980010033f}},
        {10,12.2736044f,{.884055793f,.982244611f,1.06079566f,1.12363648f,1.17390919f,
                        1.2141273f,1.24630177f,1.27204144f,1.27204144f}},
        {80,13.7499981f,{10.4499969f,10.5599966f,10.6479969f,10.7183971f,10.7747173f,
                        10.8197737f,10.8558187f,10.884655f,10.884655f}}
    };
    const MovementTuning tuning{275,20,20,1.25f,3.2f,.8f,.025f};
    for(const auto& expected:fixtures)for(unsigned lag:{0u,3u,10u,24u}){
        LocalMovement movement;
        const MovementAnchor initial{{},{},0,true};
        MovementIntent input{1,0,0,1,0};
        auto ground=[](const MovementPoint& from,const MovementPoint& to,bool,MovementCollisionContext& query){
            const auto at=query.purpose==MovementResolvePurpose::Motion &&
                movement_length(movement_difference(to,from))<.025f ? from : to;
            return std::optional<MovementContact>{{at,true}};
        };
        // Publish input at the beginning of the first native tick, with no
        // elapsed physics. This aligns the oracle's and client's input edge.
        movement.sample(0.,initial,input,tuning,ground);
        for(unsigned tick=1;tick<=expected.held;++tick){
            input.sequence=tick;
            const auto point=movement.sample(tick*.010000001,initial,input,tuning,ground);
            require(point.has_value(),"pending walking input lost prediction");
            if(tick==expected.held)
                require(std::abs((*point)[2]-expected.positions[0])<.000002f,
                        "walking endpoint differs from original numeric fixture");
        }
        input.forward=0;
        for(unsigned coast=1;coast<=100;++coast){
            MovementAnchor anchor=initial;
            const int reference=int(coast)-int(lag)-1;
            if(reference>=0){
                anchor.position[2]=expected.positions[std::min(unsigned(reference),8u)];
                anchor.velocity[2]=expected.released_velocity*std::pow(.8f,float(reference));
                anchor.acknowledged=expected.held+unsigned(reference);
            }
            input.sequence=expected.held+coast;
            const auto point=movement.sample(input.sequence*.010000001,anchor,input,tuning,ground);
            require(point.has_value(),"delayed coast acknowledgement lost prediction");
            require(std::abs((*point)[2]-expected.positions[std::min(coast,8u)])<.000002f,
                    "neutral input coast differs from original numeric fixture");
        }
    }
}

static void delayed_jump_clock(){
    const MovementTuning tuning{275,20,20,1.25f,3.2f,.8f,.025f};
    auto floor=[](const MovementPoint&,MovementPoint to,bool,MovementCollisionContext&){
        to[1]=std::max(0.f,to[1]);
        return std::optional<MovementContact>{{to,to[1]<=0.f}};
    };
    for(float lag:{0.f,.014f,.240f}){
        LocalMovement movement;
        MovementIntent input{1,1,0,0,0};
        MovementAnchor anchor{{},{},0,true};
        require(!movement.head_compression(),"uninitialized compression was exposed as local");
        movement.sample(0.,anchor,input,tuning,floor);
        float peak=0;
        for(unsigned tick=1;tick<=80;++tick){
            // An ACK identifies the consumed controls. The native host can
            // complete another physics tick before publishing that sample;
            // fractional snapshot age must not add that phase lead to Y.
            const float at=std::max(0.f,float(tick)-lag*100.f);
            const unsigned before=std::min(unsigned(at),80u),after=std::min(before+1,80u);
            const float alpha=at-before;
            const auto& a=native_jump_reference[before];
            const auto& b=native_jump_reference[after];
            anchor.position[1]=a.height+(b.height-a.height)*alpha;
            anchor.velocity[1]=a.velocity+(b.velocity-a.velocity)*alpha;
            anchor.grounded=a.grounded && b.grounded;
            anchor.acknowledged=before>0?before-1:0;
            input.sequence=tick+1;
            const auto point=movement.sample(tick*.010000001,anchor,input,tuning,floor);
            require(point.has_value(),"delayed jump abandoned a safe local prediction");
            peak=std::max(peak,(*point)[1]);
            if(tick<=45){
                require(std::abs((*point)[1]-native_jump_reference[tick].height)<.000002f,
                        "snapshot phase lead changed the original local jump curve");
                const auto head=movement.head_compression();
                require(head && std::abs(*head-native_jump_reference[tick].head)<.0000001f,
                        "head compression differs from original preincrement jump clock");
            }
            if(tick>45 && tick<=56){
                const auto head=movement.head_compression();
                require(head && std::abs(*head-native_jump_reference[tick].head)<.0000001f,
                        "head compression did not decay on the native physics clock");
            }
        }
        require(peak<=3.200002f,"delayed host pose raised the local jump above native height");
        movement.reset();
        require(!movement.head_compression(),"reset retained local head compression");
    }
}

static void jump_authority_and_contacts(){
    const MovementTuning tuning{275,20,20,1.25f,3.2f,.8f,.025f};
    auto floor=[](const MovementPoint&,MovementPoint to,bool,MovementCollisionContext&){
        to[1]=std::max(0.f,to[1]);
        return std::optional<MovementContact>{{to,to[1]<=0.f}};
    };
    MovementIntent input{1,1,0,0,0};
    MovementAnchor ground{{},{},0,true};
    LocalMovement denied;
    denied.sample(0.,ground,input,tuning,floor);
    float peak=0;
    std::optional<MovementPoint> point;
    for(unsigned tick=1;tick<=160;++tick){
        input.sequence=tick+1;
        if(tick>=5)ground.acknowledged=tick-1;
        point=denied.sample(tick*.010000001,ground,input,tuning,floor);
        require(point.has_value(),"denied jump abandoned safe ground authority");
        peak=std::max(peak,(*point)[1]);
    }
    require(peak<3.1f,"acknowledged ground authority could not correct a denied jump");
    require(point && (*point)[1]<.002f,"denied jump did not return to authoritative ground");

    LocalMovement ceiling;
    ground.acknowledged=0;
    input={1,1,0,0,0};
    auto low_ceiling=[&](const MovementPoint& from,MovementPoint to,bool walking,MovementCollisionContext& query){
        to[1]=std::min(.5f,to[1]);
        return floor(from,to,walking,query);
    };
    ceiling.sample(0.,ground,input,tuning,low_ceiling);
    for(unsigned tick=1;tick<=100;++tick){
        input.sequence=tick+1;
        point=ceiling.sample(tick*.010000001,ground,input,tuning,low_ceiling);
        require(point && (*point)[1]<=.500001f,"local jump phase bypassed a ceiling collision");
    }
    require(point && (*point)[1]<.002f,"ceiling contact did not fall back to the floor");

    LocalMovement falling;
    input={1,1,0,0,0};
    falling.sample(0.,ground,input,tuning,floor);
    for(unsigned tick=1;tick<=45;++tick){
        input.sequence=tick+1;
        falling.sample(tick*.010000001,ground,input,tuning,floor);
    }
    auto pushed=falling;
    const auto ordinary=falling.sample(46*.010000001,ground,input,tuning,floor);
    auto elevated=ground;elevated.position[0]=1;elevated.position[1]=1;elevated.grounded=false;
    const auto corrected=pushed.sample(46*.010000001,elevated,input,tuning,floor);
    require(ordinary && corrected && (*corrected)[1]>(*ordinary)[1]+.06f && (*corrected)[0]>.06f,
            "jump phase guard suppressed fall or horizontal push reconciliation");
    const auto brush=pushed.follow_brush_push(.5,elevated.position,
        [](const MovementPoint&,const MovementPoint& to){return to;});
    (void)brush;
    require(!pushed.head_compression(),"brush fallback exposed an untracked local head phase");
    LocalMovement swimming;
    const SwimInput water{{},1.f,false,1.f};
    swimming.sample(0.,ground,input,tuning,floor,&water);
    swimming.sample(.010000001,ground,input,tuning,floor,&water);
    require(!swimming.head_compression(),"swimming exposed walking head compression");
}

void surface_jump_uses_local_edge(){
    const MovementTuning tuning{200.f,12.f,80.f,4.f,3.f,.65f};
    MovementAnchor host{{0.f,0.f,0.f},{3.f,0.f,0.f},1,false};
    auto floor=[](const MovementPoint&,MovementPoint to,bool,MovementCollisionContext&){
        const bool landed=to[1]<=0.f;
        if(landed)to[1]=0.f;
        return std::optional<MovementContact>{{to,landed}};
    };
    SwimInput water{{},1.f,true,1.f,20.f};
    LocalMovement client;
    client.sample(0.,host,{1,0,0,0,0},tuning,floor,&water);
    const MovementIntent held{2,2,0,0,0};
    client.sample(.010000001,host,held,tuning,floor,&water);
    const auto launch=client.sample(.020000002,host,held,tuning,floor,&water);
    require(launch && client.leaving_water() && (*launch)[1]>.18f && (*launch)[0]==0.f,
            "surface jump did not immediately use the native upward impulse");

    host.acknowledged=2;
    for(unsigned i=3;i<50;++i)client.sample(i*.010000001,host,held,tuning,floor,&water);
    host.grounded=true;
    const auto landed=client.sample(.50000005,host,held,tuning,floor);
    require(landed && (*landed)[1]==0.f && !client.leaving_water(),
            "surface jump did not hand off on confirmed land");

    LocalMovement submerged;
    host.acknowledged=1;
    host.grounded=false;
    water.surface=false;
    submerged.sample(0.,host,{1,0,0,0,0},tuning,floor,&water);
    submerged.sample(.010000001,host,held,tuning,floor,&water);
    water.surface=true;
    const auto surfaced=submerged.sample(.020000002,host,held,tuning,floor,&water);
    require(surfaced && (*surfaced)[1]==0.f && !submerged.leaving_water(),
            "a submerged press was replayed when the same held edge reached the surface");
}

void surface_jump_keeps_local_reentry(){
    const MovementTuning tuning{200.f,12.f,80.f,4.f,3.f,.65f};
    MovementAnchor host{{0.f,0.f,0.f},{},1,false};
    const SwimInput surface{{},1.f,true,1.f,20.f};
    const SwimInput submerged{{},1.f,false,1.f,20.f};
    const MovementIntent held{2,2,0,0,0};
    auto water=[](const MovementPoint& from,const MovementPoint& to,bool,MovementCollisionContext&){
        return std::optional<MovementContact>{{to,false,from[1]>0.f && to[1]<=0.f}};
    };
    LocalMovement client;
    client.sample(0.,host,{1,0,0,0,0},tuning,water,&surface,&submerged);
    for(unsigned i=1;i<=45;++i)
        client.sample(i*.010000001,host,held,tuning,water,&surface,&submerged);
    require(client.entering_water() && client.surface_jump_active() && !client.leaving_water(),
            "local reentry was retired by the pre-launch swim sample");
    host={{0.f,1.9f,0.f},{},2,false};
    const auto delayed_airborne=client.sample(.46000005,host,held,tuning,water,nullptr,&submerged);
    require(delayed_airborne && (*delayed_airborne)[1]<0.f && client.entering_water(),
            "delayed airborne authority replayed the surface jump after reentry");
    host={{0.f,-.3f,0.f},{0.f,-5.f,0.f},2,false};
    const auto confirmed=client.sample(.47000005,host,held,tuning,water,&submerged,&submerged);
    require(confirmed && (*confirmed)[1]<0.f && !client.surface_jump_active(),
            "confirmed swim phase did not retire the surface jump");
}

int main(){
    stick_actions();
    jump_curve();
    native_coasting();
    delayed_jump_clock();
    jump_authority_and_contacts();
    surface_jump_uses_local_edge();
    surface_jump_keeps_local_reentry();
    std::printf("Native movement parity: %u checks passed\n",checks);
}
