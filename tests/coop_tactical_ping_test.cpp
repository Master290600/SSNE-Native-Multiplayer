#include "authoritative_session.h"
#include "wire_protocol.h"
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace port::coop;
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void wait_for(const std::function<bool()>& condition,const char* message){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(!condition()){require(std::chrono::steady_clock::now()<end,message);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
EntityState entity(EntityId id,EntityKind kind,std::array<float,3> position,PlayerId owner={}){
    EntityState e;e.id=id;e.kind=kind;e.owner=owner;e.properties["position"]=position;
    if(kind==EntityKind::Player || kind==EntityKind::Enemy)e.properties["health"]=100.f;
    return e;
}
void model_checks(){
    WorldState world;world.level="Ping model";world.mission["Session.NativePhase"]=std::string("gameplay");
    Participant person;person.id={0,1};person.connected=person.ready=true;person.character=1;person.name="Trusted host";
    world.players[0]=person;world.entities[1]=entity(1,EntityKind::Player,{0,0,0},person.id);
    world.entities[2]=entity(2,EntityKind::Enemy,{20,0,0});world.entities[3]=entity(3,EntityKind::Pickup,{10,0,0});
    TacticalPingLog log;TacticalPingRequest location{1,{5,0,0},0,PingKind::Location};
    require(log.send(person,location,world,0),"Initial location ping failed");
    require(!log.send(person,location,world,749),"750ms rate limit bypassed");
    require(log.send(person,{1,{999,999,999},2,PingKind::Enemy},world,750),"Target ping did not snap to authoritative position");
    auto shown=log.messages(world,world.players,750);
    require(shown.size()==1 && shown[0].sequence==2 && shown[0].position==std::array<float,3>{20,0,0} && shown[0].name==person.name && shown[0].sender==person.id,"Identity/position/replacement incorrect");
    require(shown[0].remaining_ms==8000,"Marker lifetime incorrect");
    require(log.messages(world,world.players,8749).size()==1 && log.messages(world,world.players,8750).empty(),"Marker expiry incorrect");
    for(auto invalid:std::vector<TacticalPingRequest>{{0,{5,0,0},0,PingKind::Location},{2,{5,0,0},0,PingKind::Location},{1,{251,0,0},0,PingKind::Location},{1,{std::numeric_limits<float>::quiet_NaN(),0,0},0,PingKind::Location},{1,{0,0,0},0,PingKind::Enemy},{1,{0,0,0},2,PingKind::Pickup},{1,{0,0,0},2,PingKind::Location},{1,{0,0,0},2,PingKind(3)}})
        require(!log.send(person,invalid,world,10000),"Invalid epoch/range/position/kind accepted");
    person.afk=true;require(!log.send(person,location,world,10000),"AFK sent a marker");person.afk=false;
    person.spectator=true;require(!log.send(person,location,world,10000),"Spectator sent a marker");person.spectator=false;
    person.ready=false;require(!log.send(person,location,world,10000),"Loading player sent a marker");person.ready=true;
    world.entities[1].properties["health"]=0.f;require(!log.send(person,location,world,10000),"Dead player sent a marker");world.entities[1].properties["health"]=100.f;
    world.entities[3].properties["pickup_view_mask"]=int64_t(2);require(!log.send(person,{1,{10,0,0},3,PingKind::Pickup},world,10000),"Invisible pickup accepted");
    world.entities[3].properties["pickup_view_mask"]=int64_t(1);world.entities[3].properties["pickup_ownership"]=std::vector<int64_t>{1};
    require(!log.send(person,{1,{10,0,0},3,PingKind::Pickup},world,10000),"Consumed pickup accepted");world.entities[3].properties["pickup_ownership"]=std::vector<int64_t>{0};
    require(log.send(person,{1,{10,0,0},3,PingKind::Pickup},world,10000),"Pickup marker failed");
    world.entities.erase(3);require(log.messages(world,world.players,10001).empty(),"Collected pickup marker persisted");
    require(log.send(person,location,world,10750),"Replacement after rate window failed");
    auto new_people=world.players;new_people[0].id.generation=2;require(log.messages(world,new_people,10751).empty(),"Reused slot inherited marker");
    auto next=world;++next.epoch;require(log.messages(next,world.players,10751).empty(),"Old epoch marker leaked");
    require(log.send(person,{2,{5,0,0},0,PingKind::Location},next,10751),"New epoch retained old rate limit");
    log.remove(person.id);require(log.messages(next,world.players,10751).empty(),"Disconnect retained marker");
    next.mission["Session.NativePhase"]=std::string("loading");require(!log.send(person,{2,{5,0,0},0,PingKind::Location},next,12000),"Loading phase accepted marker");
}
void idle_host_checks(){
    _putenv_s("SSNE_TEST_HIDDEN","1");AuthoritativeServer host;
    require(host.start(0,"123456",1234,"Idle host"),"Idle host start failed");
    auto world=*host.latest_world();world.level="Idle host ping";world.tick=1;world.mission["Session.NativePhase"]=std::string("gameplay");
    const auto id=host.status().player;world.entities[1]=entity(1,EntityKind::Player,{0,0,0},id);
    std::string error;require(host.publish(world,error) && host.attach_character(id,1) && host.ready_host(),"Idle world setup failed");
    const auto frozen=host.latest_world();const auto revision=host.status().world_revision;
    WorldState local;local.epoch=frozen->epoch;local.level=frozen->level;local.players=host.participants();local.mission["Session.NativePhase"]=std::string("gameplay");
    local.entities[1]=entity(1,EntityKind::Player,{500,0,0},id);local.entities[7]=entity(7,EntityKind::Enemy,{520,0,0});
    const TacticalPingRequest request{local.epoch,{520,0,0},7,PingKind::Enemy};
    require(!host.send_ping(request),"Frozen-world path invented a target");
    auto invalid=local;invalid.players[0].id.generation++;
    require(!host.send_ping(request,invalid),"Trusted view accepted wrong host identity");
    require(host.send_ping(request,local),"Idle host could not use tiny current view");
    auto markers=host.pings();require(markers.size()==1 && markers[0].position==std::array<float,3>{520,0,0},"Idle host marker used stale origin/target");
    const auto remaining=markers[0].remaining_ms;
    local.entities[7].properties["position"]=std::array<float,3>{530,0,0};
    require(host.update_local_ping_view(local),"Small target refresh failed");markers=host.pings();
    require(markers.size()==1 && markers[0].position==std::array<float,3>{530,0,0} && markers[0].remaining_ms<=remaining,"Target refresh extended lifetime or kept stale position");
    require(host.latest_world().get()==frozen.get() && host.status().world_revision==revision,"Idle marker captured/published an entire world");
    local.entities[7].properties["health"]=0.f;
    require(!host.update_local_ping_view(local) && host.pings().empty(),"Dead target retained cached marker");
    require(host.latest_world().get()==frozen.get(),"Dead target refresh rebuilt world");host.stop();
}
void network_checks(){
    _putenv_s("SSNE_TEST_HIDDEN","1");
    AuthoritativeServer host(8);AuthoritativeClient client;
    require(host.pings().empty() && client.pings().empty(),"Unstarted marker state");
    require(!host.send_ping({1,{0,0,0},0,PingKind::Location}) && !client.send_ping({1,{0,0,0},0,PingKind::Location}),"Unstarted ping accepted");
    require(host.start(0,"123456",1234,"Host %s"),"Host start failed");
    require(client.start("127.0.0.1",host.status().port,"123456",1234,"Client ä"),"Client start failed");
    wait_for([&]{return client.status().connected && host.status().joined==2 && client.latest_world()->revision;},"Connection failed");
    const auto host_id=host.status().player,client_id=client.status().player;
    auto world=*host.latest_world();world.level="Ping network";world.tick=1;world.mission["Session.NativePhase"]=std::string("gameplay");
    world.entities[1]=entity(1,EntityKind::Player,{0,0,0},host_id);world.entities[2]=entity(2,EntityKind::Player,{5,0,0},client_id);
    world.entities[100]=entity(100,EntityKind::Enemy,{20,0,0});world.entities[200]=entity(200,EntityKind::Pickup,{15,0,0});
    std::string error;require(host.publish(world,error),error.c_str());
    require(host.attach_character(host_id,1) && host.attach_character(client_id,2) && host.ready_host(),"Character setup failed");
    wait_for([&]{return client.latest_world()->entities.size()==4;},"Gameplay not replicated");client.ready(world.epoch);
    wait_for([&]{return host.participants()[client_id.slot].ready && client.latest_world()->players[client_id.slot].ready;},"Ready not replicated");
    const auto authority=host.checkpoint_authority();const auto revision=host.status().world_revision;
    require(host.send_ping({world.epoch,{7,0,0},0,PingKind::Location}),"Host location send failed");
    require(!host.send_ping({world.epoch,{8,0,0},0,PingKind::Location}),"Host API bypassed cooldown");
    wait_for([&]{return client.pings().size()==1;},"Host location not delivered");
    auto markers=client.pings();require(markers[0].name=="Host %s" && markers[0].sender==host_id && markers[0].target==0 && markers[0].kind==PingKind::Location,"Host identity/kind not preserved");
    require(client.send_ping({world.epoch,{1000,0,0},100,PingKind::Enemy}),"Client enemy send failed");
    wait_for([&]{return host.pings().size()==2 && client.pings().size()==2;},"Client marker round trip failed");
    markers=host.pings();const auto found=std::find_if(markers.begin(),markers.end(),[&](const auto& p){return p.sender==client_id;});
    require(found!=markers.end() && found->name=="Client ä" && found->position==std::array<float,3>{20,0,0} && found->target==100 && found->kind==PingKind::Enemy,"Client spoofed marker identity/position");
    wait_for([&]{auto a=host.player_latencies(),b=client.player_latencies();return a[0].known && a[0].ping_ms==0 && a[client_id.slot].id==client_id && a[client_id.slot].known && b[client_id.slot].id==client_id && b[client_id.slot].known && b[0].known;},"Measured roster RTT not delivered");
    require(host.checkpoint_authority()==authority && host.status().world_revision==revision,"Ping/RTT polluted gameplay authority or rebuilt a world");
    client.set_afk(true);wait_for([&]{return host.participants()[client_id.slot].afk && client.latest_world()->players[client_id.slot].afk;},"AFK state not replicated");
    require(!client.send_ping({world.epoch,{1,0,0},0,PingKind::Location}),"AFK client sent marker");
    require(host.pings().size()==1 && client.pings().size()==1,"AFK marker not hidden");
    client.set_afk(false);wait_for([&]{return !client.latest_world()->players[client_id.slot].afk;},"AFK off not replicated");
    // A well-formed invalid request is rejected without disconnecting peers.
    std::this_thread::sleep_for(std::chrono::milliseconds(760));
    require(client.send_ping({world.epoch,{1000,0,0},0,PingKind::Location}),"Well-formed request not queued");
    std::this_thread::sleep_for(std::chrono::milliseconds(550));require(client.status().connected && host.pings().size()==1,"Invalid range accepted or disconnected play");
    world=*host.latest_world();++world.epoch;++world.tick;world.level="Next ping level";world.entities.clear();world.mission["Session.NativePhase"]=std::string("loading");
    require(host.publish(world,error,true),error.c_str());
    wait_for([&]{return client.latest_world()->epoch==world.epoch;},"Epoch not replicated");
    require(host.pings().empty() && client.pings().empty() && !client.send_ping({world.epoch-1,{1,0,0},0,PingKind::Location}),"Level change replayed markers");
    client.stop();wait_for([&]{return host.status().joined==1;},"Disconnect not processed");
    require(client.pings().empty() && !client.player_latencies()[client_id.slot].known,"Stopped client retained transient state");
    const auto metrics=host.player_latencies();require(!metrics[client_id.slot].known && !metrics[client_id.slot].id.generation,"Disconnected slot retained RTT");
    host.stop();require(host.pings().empty() && !host.player_latencies()[0].known,"Stopped host retained transient state");
}
}
int main()try{model_checks();idle_host_checks();network_checks();std::cout<<"Tactical pings and roster RTT: validation, lifetime, identities, authoritative target positions, idle tiny-view refresh, loopback delivery, no world rebuilds, AFK, epochs and disconnects passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
