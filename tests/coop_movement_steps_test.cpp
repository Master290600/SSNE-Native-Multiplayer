#include "authoritative_session.h"
#include "movement_command_stream.h"
#include "command_movement.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace port::coop;
using Clock=std::chrono::steady_clock;
using Action=ssne::NativeMovementStepAction;
static unsigned checks=0;
static void require(bool pass,const char* message){++checks;if(!pass){std::cerr<<message<<'\n';std::exit(1);}}
template<class Predicate> static void wait_for(Predicate&& predicate,const char* message){
    const auto end=Clock::now()+std::chrono::seconds(8);
    while(!predicate()){require(Clock::now()<end,message);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
static double now(){return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();}
static ssne::NativeMovementState checkpoint(){ssne::NativeMovementState s;s.grounded=true;s.contact_kind=1;return s;}
static EntityState actor(PlayerId owner){
    EntityState e;e.id=200+owner.slot;e.kind=EntityKind::Player;e.definition=2;e.owner=owner;
    e.properties={{"position",std::array<float,3>{}},{"health",100.f},
        {"movement_spawn",int64_t(1)},{"movement_checkpoint",encode_movement_checkpoint(checkpoint())}};
    return e;
}
struct Fixture {
    AuthoritativeServer host{3};AuthoritativeClient client;WorldState world;PlayerId local{},remote{};
    Fixture(){
        require(host.start(0,"123456",1234,"Movement host"),"Host could not start loopback service");
        require(client.start("127.0.0.1",host.status().port,"123456",1234,"Movement client"),"Client could not start");
        wait_for([&]{const auto state=client.status();if(!state.connecting && !state.connected){std::cerr<<state.message<<'\n';require(false,"Client transport failed during assignment");}return state.connected;},"Client was not assigned");
        local=host.status().player;remote=client.status().player;
        world.level="Rlevel1_1";world.mission["Session.NativePhase"]=std::string("gameplay");
        world.entities[200+local.slot]=actor(local);world.entities[200+remote.slot]=actor(remote);
        publish();bind_both();
    }
    ~Fixture(){client.stop();host.stop();}
    void publish(){std::string error;++world.tick;require(host.publish(world,error),error.c_str());}
    void ready_remote(){
        wait_for([&]{return client.latest_world()->players[remote.slot].character==200+remote.slot;},"Client did not receive character binding");
        client.ready(world.epoch);
        wait_for([&]{return host.participants()[remote.slot].ready;},"Client Ready did not reach authority");
        publish();
    }
    void bind_both(){
        require(host.attach_character(local,200+local.slot) && host.attach_character(remote,200+remote.slot),"Actor attachment failed");
        require(host.ready_host(),"Host could not become ready");ready_remote();
        wait_client_stream(stream());
    }
    uint64_t stream()const{return movement_unsigned(host.latest_world()->entities.at(200+remote.slot).properties,"movement_stream");}
    uint64_t ack()const{return movement_unsigned(host.latest_world()->entities.at(200+remote.slot).properties,"movement_ack");}
    MovementStreamIdentity identity()const{return {world.epoch,200+remote.slot,stream()};}
    void wait_client_stream(uint64_t token){
        wait_for([&]{const auto w=client.latest_world();const auto it=w->entities.find(200+remote.slot);return w->epoch==world.epoch && it!=w->entities.end() && movement_unsigned(it->second.properties,"movement_stream")==token;},"Client did not receive movement stream change");
    }
    ssne::NativeMovementStepDecision reserve(uint64_t spawn=1){
        ssne::NativeMovementStepDecision result;
        wait_for([&]{result=host.take_movement_step(remote.slot,spawn);return result.action==Action::Execute;},"Movement command did not cross the actual socket");return result;
    }
};

static void network_commands_and_ack(Fixture& f){
    const auto identity=f.identity();require(bool(identity),"Host did not publish an eligible command stream");
    require(f.host.take_movement_step(f.local.slot,1).action==Action::Legacy,"Local host was put under remote command scheduling");
    require(f.host.take_movement_step(f.remote.slot,1).action==Action::Wait && f.ack()==0,"Empty movement stream advanced physics or ACK");
    double clock=now();f.client.submit_input({});
    require(f.client.movement_commands(identity,0,clock,0).empty(),"Binding invented an elapsed movement command");
    ControlInput input;input.pad.stick_y=255;input.pad.buttons=ssne::PAD_L;input.pressed=ssne::PAD_L;
    f.client.submit_input(input);
    clock+=.050000001;auto commands=f.client.movement_commands(identity,0,clock,.25f);
    require(commands.size()==5,"Five native quanta were not generated as five distinct commands");
    input.pressed=0;input.pad.buttons=0;f.client.submit_input(input);
    clock+=.030000001;commands=f.client.movement_commands(identity,0,clock,.25f);
    require(commands.size()==8,"Short forward tap lost its duration before transport");
    input.pad.stick_y=0;f.client.submit_input(input);
    clock+=.050000001;f.client.movement_commands(identity,0,clock,-.75f);
    clock+=.050000001;f.client.movement_commands(identity,0,clock,-.75f);
    input.pad.stick_y=128;f.client.submit_input(input);
    clock+=.040000001;commands=f.client.movement_commands(identity,0,clock,0.f);
    require(commands.size()==22,"Batched forward/reversal/release command count changed");
    // Deliberately accumulate enough real credit and queued commands for the
    // same bounded catch-up decision used by the original native step hook.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bool catch_up=false;
    for(unsigned n=1;n<=22;++n){
        const auto reserved=f.reserve();const auto& command=reserved.command;
        catch_up=catch_up || reserved.catch_up;
        require(command.sequence==n,"Socket transport reordered or coalesced movement sequences");
        require(command.stick_x==0 && command.stick_y==(n<=8?127:n<=18?-128:0),"Socket transport lost a short tap or reversal state");
        require(command.jump_pressed==(n==1),"Queued jump edge was dropped or repeated");
        require(command.yaw==(n<=8?.25f:n<=18?-.75f:0.f),"Socket transport rebound historical movement heading");
        require(f.host.take_movement_step(f.remote.slot,1).action==Action::Wait,"Reserving a native step consumed the next queued command");
        require(f.ack()==n-1,"Receive/reserve published an unexecuted movement ACK");
        f.host.complete_movement_step(f.remote.slot,n);
        require(f.ack()==n-1,"Completion ACK escaped without its matching pose publication");
        auto state=checkpoint();state.position[0]=float(n)+.25f;
        f.world.entities.at(identity.entity).properties["position"]=state.position;
        f.world.entities.at(identity.entity).properties["movement_checkpoint"]=encode_movement_checkpoint(state);
        f.publish();require(f.ack()==n,"Completed native command was not acknowledged with publication");
        if(n==1){
            wait_for([&]{const auto w=f.client.latest_world();const auto& e=w->entities.at(identity.entity);return movement_unsigned(e.properties,"movement_ack")==1;},"Completed ACK did not cross back to client");
            const auto w=f.client.latest_world();const auto decoded=decode_movement_checkpoint(w->entities.at(identity.entity).properties.at("movement_checkpoint"));
            require(decoded && decoded->position[0]==1.25f,"ACK and authoritative pose arrived from different publications");
            const auto outstanding=f.client.movement_commands(identity,1,clock,0);
            require(outstanding.size()==21 && outstanding.front().sequence==2,"Client retired more than the completed command quantum");
        }
    }
    require(catch_up,"A queued burst with earned real-time credit never enabled bounded catch-up");
    require(f.host.take_movement_step(f.remote.slot,1).action==Action::Wait,"Queue underflow replayed the latest held pad");
    f.host.complete_movement_step(f.remote.slot,999);f.publish();require(f.ack()==22,"Unreserved completion invented a native ACK");
    wait_for([&]{const auto w=f.client.latest_world();return movement_unsigned(w->entities.at(identity.entity).properties,"movement_ack")==22;},"Client did not receive the final completed ACK");
    require(f.client.status().connected,"Valid command batching disconnected the client");
}

static void stream_lifecycle(Fixture& f){
    auto previous=f.stream();
    f.world.entities.at(200+f.remote.slot).properties.erase("movement_checkpoint");f.publish();f.wait_client_stream(0);
    require(f.host.take_movement_step(f.remote.slot,1).action==Action::Legacy,"Unsupported movement mode retained a walking queue");
    f.world.entities.at(200+f.remote.slot).properties["movement_checkpoint"]=encode_movement_checkpoint(checkpoint());f.publish();f.wait_client_stream(f.stream());
    require(f.stream() && f.stream()!=previous && f.ack()==0,"Returning to walking reused its retired command stream");
    previous=f.stream();f.client.set_afk(true);
    wait_for([&]{return f.host.participants()[f.remote.slot].afk;},"AFK command did not arrive");
    f.publish();f.wait_client_stream(0);
    require(f.host.take_movement_step(f.remote.slot,1).action==Action::Legacy,"AFK controller was blocked by remote queue underflow");
    f.client.set_afk(false);wait_for([&]{return !f.host.participants()[f.remote.slot].afk;},"Return from AFK did not arrive");
    f.publish();f.wait_client_stream(f.stream());require(f.stream()!=previous,"AFK return reused historical human commands");
    previous=f.stream();f.world.entities.at(200+f.remote.slot).properties["movement_spawn"]=int64_t(2);
    f.publish();f.wait_client_stream(f.stream());
    require(f.stream()!=previous && f.ack()==0,"Respawn reused a completed command floor");
    require(f.host.take_movement_step(f.remote.slot,1).action==Action::Legacy && f.host.take_movement_step(f.remote.slot,2).action==Action::Wait,"Spawn identity did not guard its native queue");
    const auto old_identity=MovementStreamIdentity{f.world.epoch,200+f.remote.slot,previous};
    require(f.client.movement_commands(old_identity,0,now(),0).empty(),"Client generated movement for a retired spawn token");
    auto identity=f.identity();double clock=now();f.client.submit_input({});f.client.movement_commands(identity,0,clock,0);
    clock+=.010000001;f.client.movement_commands(identity,0,clock,0);
    require(f.reserve(2).command.sequence==1,"Respawn did not restart at the first command quantum");
    // A level change abandons that reservation and all old pending commands.
    previous=f.stream();++f.world.epoch;f.world.tick=0;f.publish();
    wait_for([&]{return f.client.latest_world()->epoch==f.world.epoch;},"New level epoch did not arrive");
    require(f.host.take_movement_step(f.remote.slot,2).action==Action::Legacy,"Old-level reservation survived loading");
    f.bind_both();require(f.stream()!=previous && f.ack()==0,"New level reused old movement commands");
    const auto old_owner=f.remote;previous=f.stream();f.client.stop();
    wait_for([&]{return !f.host.participants()[old_owner.slot].connected;},"Old owner did not leave authority");
    f.world.entities.erase(200+old_owner.slot);f.publish();
    require(f.client.start("127.0.0.1",f.host.status().port,"123456",1234,"Reconnected movement client"),"Client could not reconnect");
    wait_for([&]{return f.client.status().connected;},"Replacement owner was not assigned");
    f.remote=f.client.status().player;
    require(f.remote.slot==old_owner.slot && f.remote.generation!=old_owner.generation,"Reconnection did not replace the logical owner generation");
    f.world.entities[200+f.remote.slot]=actor(f.remote);f.publish();
    require(f.host.attach_character(f.remote,200+f.remote.slot),"Replacement owner actor attachment failed");
    f.ready_remote();f.wait_client_stream(f.stream());
    require(f.stream()!=previous && f.ack()==0 && f.host.take_movement_step(f.remote.slot,1).action==Action::Wait,"Old owner's commands entered the replacement character");
}

static void put32(std::vector<uint8_t>& b,uint32_t n){for(int i=3;i>=0;--i)b.push_back(uint8_t(n>>(8*i)));}
static void put64(std::vector<uint8_t>& b,uint64_t n){put32(b,uint32_t(n>>32));put32(b,uint32_t(n));}
static void put_text(std::vector<uint8_t>& b,const std::string& s){put32(b,uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
static uint32_t get32(const uint8_t* b){return uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];}
struct RawPeer {
    SOCKET socket=INVALID_SOCKET;PlayerId player{};
    static bool transfer(SOCKET s,uint8_t* p,size_t size,bool writing){while(size){const int n=writing?send(s,reinterpret_cast<const char*>(p),int(size),0):recv(s,reinterpret_cast<char*>(p),int(size),0);if(n<=0)return false;p+=n;size-=unsigned(n);}return true;}
    void send_frame(wire::Message type,std::vector<uint8_t> body){
        std::vector<uint8_t> frame;put32(frame,wire::magic);frame.push_back(uint8_t(wire::version>>8));frame.push_back(uint8_t(wire::version));frame.push_back(uint8_t(uint16_t(type)>>8));frame.push_back(uint8_t(type));put32(frame,uint32_t(body.size()));frame.insert(frame.end(),body.begin(),body.end());
        require(transfer(socket,frame.data(),frame.size(),true),"Raw authenticated frame could not be sent");
    }
    std::pair<wire::Message,std::vector<uint8_t>> receive(){
        uint8_t h[12];require(transfer(socket,h,sizeof(h),false),"Raw authenticated frame header failed");
        require(get32(h)==wire::magic && get32(h+8)<=max_frame_bytes,"Invalid raw server frame");
        std::vector<uint8_t> body(get32(h+8));require(transfer(socket,body.data(),body.size(),false),"Raw server payload failed");return {wire::Message(uint16_t(h[6])<<8|h[7]),std::move(body)};
    }
    explicit RawPeer(uint16_t port){
        socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(socket!=INVALID_SOCKET,"Raw peer socket failed");
        DWORD timeout=5000;setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
        require(connect(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Raw peer connection failed");
        std::vector<uint8_t> hello;put64(hello,1234);put_text(hello,"123456");put_text(hello,"Delayed old stream");send_frame(wire::Message::Hello,std::move(hello));
        const auto [type,body]=receive();require(type==wire::Message::Assigned && body.size()==16,"Raw peer was not authenticated/assigned");
        player.slot=get32(body.data());player.generation=uint64_t(get32(body.data()+4))<<32|get32(body.data()+8);
    }
    ~RawPeer(){if(socket!=INVALID_SOCKET)closesocket(socket);}
    void fence(uint64_t number){
        std::vector<uint8_t> body;put64(body,number);send_frame(wire::Message::Ping,std::move(body));
        for(;;){const auto [type,data]=receive();if(type==wire::Message::Pong && data.size()==8 && (uint64_t(get32(data.data()))<<32|get32(data.data()+4))==number)return;}
    }
    void movement(MovementStreamIdentity identity){
        std::vector<uint8_t> body;put64(body,identity.epoch);put64(body,identity.entity);put64(body,identity.token);put32(body,1);put64(body,1);
        body.push_back(0);body.push_back(127);body.push_back(0);put32(body,std::bit_cast<uint32_t>(.125f));send_frame(wire::Message::MovementSteps,std::move(body));
    }
};

static void old_stream_in_flight(Fixture& f){
    RawPeer peer(f.host.status().port);const auto id=peer.player;const EntityId entity=200+id.slot;
    f.world.entities[entity]=actor(id);f.publish();require(f.host.attach_character(id,entity),"Raw peer actor attachment failed");
    std::vector<uint8_t> ready;put64(ready,f.world.epoch);peer.send_frame(wire::Message::Ready,std::move(ready));peer.fence(1001);
    require(f.host.participants()[id.slot].ready,"Raw peer did not become ready");f.publish();
    const MovementStreamIdentity old{f.world.epoch,entity,movement_unsigned(f.host.latest_world()->entities.at(entity).properties,"movement_stream")};
    require(bool(old),"Raw peer movement stream was absent");
    f.world.entities.at(entity).properties["movement_spawn"]=int64_t(2);f.publish();
    const MovementStreamIdentity fresh{f.world.epoch,entity,movement_unsigned(f.host.latest_world()->entities.at(entity).properties,"movement_stream")};
    require(fresh.token && fresh.token!=old.token,"Raw peer spawn did not change stream identity");
    // Deliberately send OLD bytes after the new stream is already committed.
    // Pong fences actual handling on that same authenticated TCP channel.
    peer.movement(old);peer.fence(1002);
    require(f.host.participants()[id.slot].connected && f.host.take_movement_step(id.slot,2).action==Action::Wait,
            "In-flight old stream disconnected its peer or entered the new character");
    peer.movement(fresh);peer.fence(1003);
    ssne::NativeMovementStepDecision step;
    wait_for([&]{step=f.host.take_movement_step(id.slot,2);return step.action==Action::Execute;},
             "Current raw stream never earned a native movement quantum");
    require(step.action==Action::Execute && step.command.sequence==1 && step.command.stick_y==127,"Current stream was rejected after safely ignoring old bytes");
    f.host.complete_movement_step(id.slot,1);f.publish();
    require(movement_unsigned(f.host.latest_world()->entities.at(entity).properties,"movement_ack")==1,"Current raw stream completion ACK missing");
    require(f.client.status().connected,"Additional authenticated movement peer disrupted the real client");
}

static void received_ack_is_not_applied_ack(){
    Fixture f;const auto identity=f.identity();const auto initial=checkpoint();
    const MovementTuning tune{275,20,20,1.25f,3.2f,.8f,.025f};
    auto floor=[](const MovementPoint& from,MovementPoint to,bool,MovementCollisionContext&){
        if(movement_length(movement_difference(to,from))<.025f)to=from;
        return std::optional<MovementContact>{{to,true,false,1,false}};
    };
    ControlInput forward;forward.pad.stick_y=255;f.client.submit_input(forward);
    const double bound_at=now();
    require(f.client.movement_commands(identity,0,bound_at,0).empty(),"ACK-race stream failed to bind");
    auto original=f.client.movement_commands(identity,0,bound_at+.040000001,0);
    require(original.size()==4,"ACK-race did not generate its four original command quanta");
    CommandMovement source;std::array<ssne::NativeMovementState,5> states;states[0]=initial;
    for(unsigned n=1;n<=4;++n){
        require(source.sample(0,initial,std::span(original).first(n),tune,floor).has_value(),
                "Actual predictor could not produce the ACK-race fixture");
        states[n]=*source.state();
    }
    auto publish_completed=[&](unsigned first,unsigned last){
        for(unsigned n=first;n<=last;++n){
            require(f.reserve().command.sequence==n,"ACK-race socket reordered native commands");
            f.host.complete_movement_step(f.remote.slot,n);
        }
        f.world.entities.at(identity.entity).properties["position"]=states[last].position;
        f.world.entities.at(identity.entity).properties["movement_checkpoint"]=encode_movement_checkpoint(states[last]);
        f.publish();
        wait_for([&]{return movement_unsigned(f.client.latest_world()->entities.at(identity.entity).properties,"movement_ack")==last;},
                 "ACK-race authoritative checkpoint was not received");
    };
    publish_completed(1,2);
    f.client.acknowledge_movement_checkpoint(identity,2); // Native application committed checkpoint2.
    publish_completed(3,4); // Socket has checkpoint4, presentation still needs commands3/4.
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    f.client.submit_input({});
    f.client.acknowledge_movement_checkpoint(identity,999); // Unreceived future ACK must be rejected.
    auto wrong=identity;++wrong.token;f.client.acknowledge_movement_checkpoint(wrong,4);
    const auto outstanding=f.client.movement_commands(identity,2,now(),0);
    require(outstanding.size()>=2 && outstanding[0].sequence==3 && outstanding[1].sequence==4,
            "A received-only ACK or UI heartbeat erased commands needed by the applied checkpoint");
    CommandMovement replay;
    const auto replayed=replay.sample(2,states[2],outstanding,tune,floor);
    require(replayed.has_value(),"Applied checkpoint could not replay across a newer received ACK");
    std::vector<ssne::NativeMovementCommand> all(original.begin(),original.begin()+2);
    all.insert(all.end(),outstanding.begin(),outstanding.end());
    CommandMovement independent;
    require(independent.sample(0,initial,all,tune,floor).has_value() && independent.state()==replay.state(),
            "Retained TCP commands changed actual prediction during the received/applied ACK race");
    f.client.acknowledge_movement_checkpoint(identity,4); // Native application now commits checkpoint4.
    // A diagnostic query from the old floor must show that explicit native
    // application, rather than mere receipt, retired commands3/4.
    const auto retired=f.client.movement_commands(identity,2,now(),0);
    require(retired.empty() || retired.front().sequence>4,
            "Explicit native-applied checkpoint did not retire its completed movement commands");
}

static void input_only_overlay_heartbeat(){
    // Use an independent real-clock stream: the ordering test above advances
    // its synthetic presentation clock and must not influence this heartbeat.
    Fixture f;const auto identity=f.identity();ControlInput neutral;
    f.client.submit_input(neutral);
    const double bound_at=now();
    require(f.client.movement_commands(identity,0,bound_at,.625f).empty(),
            "Fresh overlay stream invented a command at bind");
    uint64_t completed=0,acknowledged=0,applied=0;unsigned executions=0,publications=0;
    const auto started=Clock::now();
    while(Clock::now()-started<std::chrono::milliseconds(6200)){
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        // Information overlays still submit UI controls, but do not sample
        // the player's eye or call movement_commands.
        f.client.submit_input(neutral);
        const auto previous_ack=f.ack();
        for(unsigned step=0;step<4;++step){
            const auto decision=f.host.take_movement_step(f.remote.slot,1);
            if(decision.action==Action::Wait)break;
            require(decision.action==Action::Execute && decision.command.sequence==completed+1,
                    "Input-only heartbeat lost contiguous completed movement time");
            require(decision.command.stick_x==0 && decision.command.stick_y==0 &&
                    !decision.command.jump_pressed && decision.command.yaw==.625f,
                    "Overlay heartbeat invented motion, a jump edge, or a different facing");
            completed=decision.command.sequence;++executions;
            f.host.complete_movement_step(f.remote.slot,completed);
            require(f.ack()==previous_ack,"Heartbeat completion published ACK before its pose");
        }
        if(completed>previous_ack){
            auto state=checkpoint();state.position[0]=float(completed)*.25f;
            f.world.entities.at(identity.entity).properties["position"]=state.position;
            f.world.entities.at(identity.entity).properties["movement_checkpoint"]=encode_movement_checkpoint(state);
            f.publish();++publications;
            require(f.ack()==completed,"Heartbeat command ACK was not published with its pose");
        }
        const auto world=f.client.latest_world();
        const auto& entity=world->entities.at(identity.entity);
        acknowledged=std::max(acknowledged,movement_unsigned(entity.properties,"movement_ack"));
        if(acknowledged>applied){
            const auto decoded=decode_movement_checkpoint(entity.properties.at("movement_checkpoint"));
            require(decoded && decoded->position[0]==float(acknowledged)*.25f,
                    "Overlay received an ACK without its matching native checkpoint");
            applied=acknowledged; // Simulate successful native application without rendering its eye.
            f.client.acknowledge_movement_checkpoint(identity,applied);
        }
    }
    require(Clock::now()-started>=std::chrono::seconds(6),
            "Heartbeat scenario did not span an actual overlay rendering gap");
    require(executions>movement_history_limit && publications>=100 && applied>movement_history_limit,
            "Movement stopped during the UI-only overlay or failed to retire completed ACKs");
    const double elapsed=now()-bound_at;
    require(double(completed)*movement_command_tick<=elapsed+.011,
            "Heartbeat completed more movement time than actually elapsed");
    wait_for([&]{const auto world=f.client.latest_world();return movement_unsigned(world->entities.at(identity.entity).properties,"movement_ack")==completed;},
             "Input-only heartbeat's completed ACK did not reach the client");
    require(f.client.status().connected,"UI-only heartbeat disconnected its eligible movement stream");
    std::cout<<"Input-only real-clock heartbeat: "<<executions<<" completed commands, "
             <<publications<<" pose/ACK publications in "<<elapsed<<"s without a render call\n";
}

int main(){
    Fixture fixture;network_commands_and_ack(fixture);stream_lifecycle(fixture);old_stream_in_flight(fixture);
    received_ack_is_not_applied_ack();input_only_overlay_heartbeat();
    std::cout<<"Real TCP movement integration: "<<checks<<" checks passed (wire "<<wire::version<<")\n";
}
