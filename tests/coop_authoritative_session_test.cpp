#include "authoritative_session.h"
#include "authority_checkpoint_cases.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <functional>
#include <algorithm>
#include <bit>
using namespace port::coop;
using Clock=std::chrono::steady_clock;
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
void wait_for(const std::function<bool()>& predicate,const char* message) {
    auto end=Clock::now()+std::chrono::seconds(8);
    while(!predicate()){require(Clock::now()<end,message);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
void put32(std::vector<uint8_t>& b,uint32_t n){for(int i=3;i>=0;--i)b.push_back(uint8_t(n>>(8*i)));}
void put64(std::vector<uint8_t>& b,uint64_t n){put32(b,uint32_t(n>>32));put32(b,uint32_t(n));}
void text(std::vector<uint8_t>& b,const std::string& s){put32(b,uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
std::vector<uint8_t> frame(wire::Message type,const std::vector<uint8_t>& body) {
    std::vector<uint8_t> b;put32(b,wire::magic);b.push_back(0);b.push_back(wire::version);
    b.push_back(0);b.push_back(uint8_t(type));put32(b,uint32_t(body.size()));b.insert(b.end(),body.begin(),body.end());return b;
}
bool transfer(SOCKET socket,uint8_t* data,size_t size,bool writing) {
    while(size){auto n=writing?send(socket,reinterpret_cast<const char*>(data),int(size),0):recv(socket,reinterpret_cast<char*>(data),int(size),0);if(n<=0)return false;data+=n;size-=size_t(n);}return true;
}
uint64_t get64(const std::vector<uint8_t>& bytes){require(bytes.size()>=8,"Short opaque token");uint64_t value=0;for(unsigned i=0;i<8;++i)value=(value<<8)|bytes[i];return value;}
std::pair<wire::Message,std::vector<uint8_t>> receive_frame(SOCKET socket){
    uint8_t header[12];require(transfer(socket,header,sizeof(header),false),"Read framed response header");
    auto size=uint32_t(header[8])<<24|uint32_t(header[9])<<16|uint32_t(header[10])<<8|header[11];require(size<=max_frame_bytes,"Oversized framed response");
    std::vector<uint8_t> bytes(size);require(transfer(socket,bytes.data(),bytes.size(),false),"Read framed response body");
    return {wire::Message(uint16_t(header[6])<<8|header[7]),std::move(bytes)};
}
void raw_lobby_ready(SOCKET socket,uint64_t token,bool value){
    std::vector<uint8_t> body;put64(body,token);body.push_back(value);auto packet=frame(wire::Message::LobbyReady,body);
    require(transfer(socket,packet.data(),packet.size(),true),"Lobby-ready send");
    // A following Ping on the same TCP stream fences processing of the ready
    // request, so a rejected replay is proved without a scheduling sleep.
    static uint64_t serial=0;body.clear();put64(body,++serial);packet=frame(wire::Message::Ping,body);
    require(transfer(socket,packet.data(),packet.size(),true),"Lobby-ready processing fence send");
    const auto end=Clock::now()+std::chrono::seconds(8);
    for(;;){auto response=receive_frame(socket);if(response.first==wire::Message::Pong && get64(response.second)==serial)break;require(Clock::now()<end,"Lobby-ready processing fence timeout");}
}
int64_t lobby_integer(const AuthoritativeServer& host,const char* key){const auto world=host.latest_world();const auto it=world->mission.find(key);require(it!=world->mission.end() && std::holds_alternative<int64_t>(it->second),"Missing integer lobby property");return std::get<int64_t>(it->second);}
uint64_t lobby_token(const AuthoritativeServer& host){return std::bit_cast<uint64_t>(lobby_integer(host,"Session.Lobby.ReadyToken"));}
SOCKET raw_peer(uint16_t port) {
    auto socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(socket!=INVALID_SOCKET,"Raw socket creation");
    DWORD timeout=5000;setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(port);
    require(connect(socket,reinterpret_cast<sockaddr*>(&target),sizeof(target))==0,"Raw socket connection");
    std::vector<uint8_t> hello;put64(hello,1234);text(hello,"123456");text(hello,"Adversarial");auto packet=frame(wire::Message::Hello,hello);
    require(transfer(socket,packet.data(),packet.size(),true),"Raw hello send");
    uint8_t header[12];require(transfer(socket,header,sizeof(header),false),"Raw assignment header");
    require(header[7]==uint8_t(wire::Message::Assigned),"Raw peer not assigned");
    auto size=uint32_t(header[8])<<24|uint32_t(header[9])<<16|uint32_t(header[10])<<8|header[11];require(size==16,"Assignment payload size");
    std::vector<uint8_t> assigned(size);require(transfer(socket,assigned.data(),assigned.size(),false),"Raw assignment payload");return socket;
}
// Withhold assignment on a loopback socket so Pending is observable without
// depending on thread scheduling or a slow external network.
SOCKET loopback_endpoint(uint16_t& port,bool listening) {
    auto socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(socket!=INVALID_SOCKET,"Lifecycle socket creation");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Lifecycle socket bind");
    int size=sizeof(address);require(getsockname(socket,reinterpret_cast<sockaddr*>(&address),&size)==0,"Lifecycle socket port");port=ntohs(address.sin_port);
    if(listening){require(listen(socket,1)==0,"Lifecycle socket listen");u_long mode=1;require(ioctlsocket(socket,FIONBIO,&mode)==0,"Lifecycle listener nonblocking");}
    return socket;
}
SOCKET accept_pending(SOCKET listener) {
    SOCKET peer=INVALID_SOCKET;wait_for([&]{peer=accept(listener,nullptr,nullptr);return peer!=INVALID_SOCKET;},"Lifecycle connection not accepted");
    u_long mode=0;require(ioctlsocket(peer,FIONBIO,&mode)==0,"Lifecycle accepted socket blocking");
    DWORD timeout=5000;setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    return peer;
}
void check_connecting_lifecycle() {
    uint16_t port=0;auto listener=loopback_endpoint(port,true);AuthoritativeClient client;
    require(!client.status().connecting && !client.status().connected,"Unstarted client has pending connection");
    require(!client.start("127.0.0.1",0,"123456",1234,"Invalid lifecycle"),"Invalid endpoint accepted");
    require(!client.status().connecting && !client.status().connected,"Failed start retained Pending");
    require(client.start("127.0.0.1",port,"123456",1234,"Pending lifecycle"),"Pending lifecycle start");
    auto peer=accept_pending(listener);
    uint8_t header[12];require(transfer(peer,header,sizeof(header),false),"Lifecycle hello header");
    require(header[7]==uint8_t(wire::Message::Hello),"Lifecycle client did not send Hello");
    auto size=uint32_t(header[8])<<24|uint32_t(header[9])<<16|uint32_t(header[10])<<8|header[11];require(size<=256,"Lifecycle Hello payload size");
    std::vector<uint8_t> hello(size);require(transfer(peer,hello.data(),hello.size(),false),"Lifecycle Hello payload");
    auto status=client.status();require(status.connecting && !status.connected,"TCP connection cleared Pending before assignment");
    std::vector<uint8_t> assignment;put32(assignment,1);put64(assignment,1);put32(assignment,8);auto assigned=frame(wire::Message::Assigned,assignment);
    require(transfer(peer,assigned.data(),assigned.size(),true),"Lifecycle assignment send");
    wait_for([&]{return client.status().connected;},"Lifecycle assignment not accepted");
    require(!client.status().connecting,"Assigned client still reports Pending");
    closesocket(peer);wait_for([&]{auto s=client.status();return !s.connecting && !s.connected;},"Disconnected client retained a live lifecycle state");client.stop();
    require(client.start("127.0.0.1",port,"123456",1234,"Cancelled lifecycle"),"Cancelled lifecycle start");peer=accept_pending(listener);
    require(client.status().connecting && !client.status().connected,"Unassigned retry lost Pending");
    client.stop();require(!client.status().connecting && !client.status().connected,"Cancelled client retained Pending");closesocket(peer);closesocket(listener);
    auto reserved=loopback_endpoint(port,false);AuthoritativeClient refused;
    require(refused.start("127.0.0.1",port,"123456",1234,"Refused lifecycle"),"Refused lifecycle start");
    wait_for([&]{return !refused.status().connecting;},"Refused connection retained Pending");
    status=refused.status();require(!status.connected && status.message.find("socket error "+std::to_string(WSAECONNREFUSED))!=std::string::npos,"Refused connection lost Winsock refusal reason");refused.stop();closesocket(reserved);
}
void check_lobby_readiness(){
    AuthoritativeServer host(3);auto unchanged=host.latest_world();host.lobby_ready(true);host.reset_lobby_readiness();require(!host.begin_lobby_game() && host.latest_world()==unchanged,"Unstarted lobby API mutated a snapshot");
    require(host.start(0,"123456",1234,"Ready host"),"Ready host start");const auto bot=*host.add_bot();require(bot.slot==1 && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Bot did not auto-ready");
    auto peer=raw_peer(host.status().port);wait_for([&]{return host.status().joined==3;},"Ready peer join");const auto original=host.participants()[2].id;const auto initial=lobby_token(host);
    raw_lobby_ready(peer,initial,true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==6,"Peer ready modified another player's bit");
    unchanged=host.latest_world();require(!host.begin_lobby_game() && host.latest_world()==unchanged,"Failed Start mutated readiness/world revision");
    host.lobby_ready(true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==7,"Host ready missing");
    host.reset_lobby_readiness();const auto changed=lobby_token(host);require(changed!=initial && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Setup reset retained human ready or lost bot ready");
    raw_lobby_ready(peer,initial,true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Stale ready re-armed after setup reset");
    raw_lobby_ready(peer,changed,true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==6,"Current ready token rejected");
    raw_lobby_ready(peer,initial,false);require(lobby_integer(host,"Session.Lobby.ReadyMask")==6,"Stale unready cleared current confirmation");
    host.lobby_ready(true);require(host.begin_lobby_game() && !lobby_integer(host,"Session.Lobby.Open") && !lobby_integer(host,"Session.Lobby.ReadyMask"),"Unanimous Start did not consume lobby readiness");
    raw_lobby_ready(peer,changed,true);unchanged=host.latest_world();host.reset_lobby_readiness();host.lobby_ready(true);require(!host.begin_lobby_game() && host.latest_world()==unchanged,"Closed lobby accepted readiness/reset/Start");
    WorldState menu;menu.tick=1;menu.mission["Session.NativePhase"]=std::string("menu");std::string error;require(host.publish(menu,error),error.c_str());const auto reopened=lobby_token(host);
    require(reopened!=changed && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Reopened lobby retained previous readiness/token");raw_lobby_ready(peer,changed,true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Previous lobby request accepted after re-entry");
    raw_lobby_ready(peer,reopened,true);menu.tick=2;require(host.publish(menu,error),error.c_str());require(lobby_token(host)==reopened && lobby_integer(host,"Session.Lobby.ReadyMask")==6,"Ordinary menu publication reset readiness");
    menu.epoch=2;menu.tick=0;require(host.publish(menu,error),error.c_str());require(lobby_token(host)!=reopened && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"New menu epoch retained a previous confirmation");
    raw_lobby_ready(peer,lobby_token(host),true);closesocket(peer);wait_for([&]{return host.status().joined==2;},"Ready peer disconnect");require(lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Disconnected slot retained ready");
    peer=raw_peer(host.status().port);wait_for([&]{return host.status().joined==3;},"Ready replacement join");const auto replacement=host.participants()[2].id;require(replacement.slot==original.slot && replacement.generation!=original.generation && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Replacement inherited previous generation's readiness");
    closesocket(peer);host.stop();

    AuthoritativeServer bots(2);require(bots.start(0,"123456",1234,"Bot replacement host"),"Bot replacement host start");require(bots.set_match_player_limit(2),"Bot replacement admission limit");require(bool(bots.add_bot()),"Replacement bot add");bots.lobby_ready(true);require(lobby_integer(bots,"Session.Lobby.ReadyMask")==3,"Ready replacement setup");
    bots.reset_lobby_readiness();bots.lobby_ready(true);const auto token=lobby_token(bots);require(token!=bots.latest_world()->epoch,"Ready token test did not differ from world epoch");
    AuthoritativeClient human;require(human.start("127.0.0.1",bots.status().port,"123456",1234,"Human replacement"),"Human replacement start");wait_for([&]{return human.status().connected && !bots.bot_mask();},"Human did not replace bot");const auto id=human.status().player;
    require(id.slot==1 && lobby_integer(bots,"Session.Lobby.ReadyMask")==1 && !bots.begin_lobby_game(),"Human inherited evicted bot ready");
    require(bots.set_spectator_lock(2) && bots.participants()[1].spectator && !bots.begin_lobby_game(),"Unready spectator bypassed connected-player gate");
    wait_for([&]{const auto w=human.latest_world();const auto it=w->mission.find("Session.Lobby.ReadyToken");return it!=w->mission.end() && std::bit_cast<uint64_t>(std::get<int64_t>(it->second))==token;},"Human missing ready token");human.lobby_ready(true);wait_for([&]{return lobby_integer(bots,"Session.Lobby.ReadyMask")==3;},"Client did not send advertised token");
    human.lobby_ready(false);wait_for([&]{return lobby_integer(bots,"Session.Lobby.ReadyMask")==1;},"Client unready rejected");require(!bots.begin_lobby_game(),"Unready human spectator ignored");
    human.stop();wait_for([&]{return bots.status().joined==1;},"Human replacement stop");require(bots.begin_lobby_game(),"Disconnected human blocked remaining ready host");bots.stop();
}
void check_lobby_legacy_epoch_fallback(){
    uint16_t port=0;auto listener=loopback_endpoint(port,true);AuthoritativeClient client;require(client.start("127.0.0.1",port,"123456",1234,"Epoch fallback"),"Legacy lobby client start");auto peer=accept_pending(listener);require(receive_frame(peer).first==wire::Message::Hello,"Legacy lobby Hello");
    std::vector<uint8_t> bytes;put32(bytes,1);put64(bytes,1);put32(bytes,2);auto packet=frame(wire::Message::Assigned,bytes);require(transfer(peer,packet.data(),packet.size(),true),"Legacy assignment send");
    WorldState world;world.epoch=37;world.revision=1;world.mission["Session.Lobby.Open"]=int64_t(1);world.mission["Session.Lobby.ReadyMask"]=int64_t(0);world.players[1]={{1,1},true,false,false,0,"Epoch fallback"};
    packet=frame(wire::Message::World,encode_delta(difference(static_cast<const WorldState*>(nullptr),world)));require(transfer(peer,packet.data(),packet.size(),true),"Legacy lobby world send");wait_for([&]{return client.latest_world()->epoch==37;},"Legacy lobby world receive");client.lobby_ready(true);
    const auto end=Clock::now()+std::chrono::seconds(8);for(;;){const auto response=receive_frame(peer);if(response.first==wire::Message::LobbyReady){require(response.second.size()==9 && get64(response.second)==37 && response.second[8]==1,"Client changed LobbyReady shape or lost legacy epoch fallback");break;}require(Clock::now()<end,"Legacy lobby ready receive timeout");}
    client.stop();closesocket(peer);closesocket(listener);
}
void check_lobby_setup_atomic_readiness(){
    AuthoritativeServer host(3);require(host.start(0,"123456",1234,"Atomic setup host"),"Atomic setup host start");require(bool(host.add_bot()),"Atomic setup bot");auto peer=raw_peer(host.status().port);wait_for([&]{return host.status().joined==3;},"Atomic setup peer join");
    WorldState world;world.tick=1;world.mission={{"Session.NativePhase",std::string("menu")},{"Session.Lobby.Level",std::string("Rlevel1_1")},{"Session.Lobby.Mode",int64_t(1)},{"Session.Lobby.LevelSet",int64_t(0)},{"Session.Lobby.Difficulty",int64_t(1)},{"Session.EnemyMultiplier",int64_t(1)},{"Session.AmmoCapacity",int64_t(1)},{"Session.AmmoPickupMultiplier",int64_t(1)}};std::string error;require(host.publish(world,error),error.c_str());
    raw_lobby_ready(peer,lobby_token(host),true);host.lobby_ready(true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==7,"Atomic setup confirmations missing");
    host.reset_lobby_readiness();const auto intermediate=lobby_token(host);require(host.latest_world()->mission.at("Session.Lobby.Level")==Property(std::string("Rlevel1_1")),"Fixture did not expose intermediate old-proposal snapshot");
    world.mission["Session.Lobby.Level"]=std::string("Rlevel1_2");++world.tick;require(host.publish(world,error),error.c_str());require(lobby_token(host)!=intermediate && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"New proposal did not atomically renew token/clear humans");
    raw_lobby_ready(peer,intermediate,true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Old proposal with intermediate new token confirmed changed map");
    raw_lobby_ready(peer,lobby_token(host),true);host.lobby_ready(true);const auto stable=lobby_token(host);++world.tick;require(host.publish(world,error),error.c_str());require(lobby_token(host)==stable && lobby_integer(host,"Session.Lobby.ReadyMask")==7,"Unchanged setup tick invalidated confirmations");
    for(const char* key:{"Session.Lobby.Mode","Session.Lobby.LevelSet","Session.Lobby.Difficulty","Session.EnemyMultiplier","Session.AmmoCapacity","Session.AmmoPickupMultiplier"}){
        const auto before=lobby_token(host);world.mission[key]=std::get<int64_t>(world.mission.at(key))+1;++world.tick;require(host.publish(world,error),error.c_str());require(lobby_token(host)!=before && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Changed lobby rule retained human confirmation");
        raw_lobby_ready(peer,before,true);require(lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Old rule token re-armed confirmation");raw_lobby_ready(peer,lobby_token(host),true);host.lobby_ready(true);
    }
    const auto before_removal=lobby_token(host);world.mission.erase("Session.AmmoPickupMultiplier");++world.tick;require(host.publish(world,error),error.c_str());require(lobby_token(host)!=before_removal && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Removed lobby rule retained confirmation");
    auto invalid=world;invalid.epoch=0;invalid.mission["Session.Lobby.Level"]=std::string("Rlevel1_3");const auto unchanged=host.latest_world();require(!host.publish(invalid,error) && host.latest_world()==unchanged,"Rejected setup publication mutated token or readiness");
    closesocket(peer);host.stop();
}
void check_lobby_failed_start_recovery(){
    AuthoritativeServer host(2);require(host.start(0,"123456",1234,"Failed map host"),"Failed map host start");require(bool(host.add_bot()),"Failed map bot add");
    WorldState world;world.tick=1;world.mission["Session.NativePhase"]=std::string("menu");std::string error;require(host.publish(world,error),error.c_str());
    auto unchanged=host.latest_world();require(!host.cancel_lobby_game() && host.latest_world()==unchanged,"Already-open lobby cancellation mutated state");
    host.lobby_ready(true);const auto token=lobby_token(host);require(host.begin_lobby_game(),"Failed-map Start setup");
    require(host.cancel_lobby_game() && lobby_integer(host,"Session.Lobby.Open")==1 && lobby_token(host)!=token && lobby_integer(host,"Session.Lobby.ReadyMask")==2,"Rejected native map did not reopen and invalidate human ready");
    unchanged=host.latest_world();require(!host.cancel_lobby_game() && !host.begin_lobby_game() && host.latest_world()==unchanged,"Recovery allowed duplicate cancellation or Start without fresh ready");
    host.lobby_ready(true);require(host.begin_lobby_game(),"Fresh confirmation could not retry rejected map");
    world.tick=2;world.mission["Session.NativePhase"]=std::string("loading");require(host.publish(world,error),error.c_str());unchanged=host.latest_world();require(!host.cancel_lobby_game() && host.latest_world()==unchanged && !lobby_integer(host,"Session.Lobby.Open"),"Cancellation reopened a loading session");
    world.tick=3;world.mission["Session.NativePhase"]=std::string("gameplay");require(host.publish(world,error),error.c_str());unchanged=host.latest_world();require(!host.cancel_lobby_game() && host.latest_world()==unchanged && !lobby_integer(host,"Session.Lobby.Open"),"Cancellation reopened gameplay");
    host.stop();unchanged=host.latest_world();require(!host.cancel_lobby_game() && host.latest_world()==unchanged,"Stopped session cancellation mutated state");
}
void check_afk_host_ui_input(){
    AuthoritativeServer host(2);require(host.start(0,"123456",1234,"AFK UI host"),"AFK UI host start");
    AuthoritativeClient client;require(client.start("127.0.0.1",host.status().port,"123456",1234,"AFK UI peer"),"AFK UI peer start");
    wait_for([&]{return client.status().connected;},"AFK UI peer assignment");
    const auto local=host.status().player,remote=client.status().player;WorldState world;world.tick=1;world.level="Rlevel1_1";
    for(const auto id:{local,remote}){EntityState actor;actor.id=100+id.slot;actor.kind=EntityKind::Player;actor.owner=id;actor.definition=2;actor.properties={{"position",std::array<float,3>{float(id.slot),0,10}},{"health",100.f}};world.entities.emplace(actor.id,std::move(actor));}
    std::string error;require(host.publish(world,error),error.c_str());require(host.attach_character(local,100+local.slot)&&host.attach_character(remote,100+remote.slot)&&host.ready_host(),"AFK UI actor readiness");
    wait_for([&]{return client.latest_world()->players[remote.slot].character!=0;},"AFK UI peer character");client.ready(world.epoch);
    wait_for([&]{return host.take_controls()[remote.slot].ready;},"AFK UI peer readiness");
    require(host.host_afk(1,true)==CommandResult::Applied,"AFK UI host takeover");client.set_afk(true);
    wait_for([&]{return host.participants()[remote.slot].afk;},"AFK UI remote takeover");
    auto neutral=[](const ControlInput& input){return !input.pad.buttons&&!input.pressed&&input.pad.stick_x==128&&input.pad.stick_y==128&&input.pad.cstick_x==128&&input.pad.cstick_y==128&&!input.pad.trigger_l&&!input.pad.trigger_r&&!input.yaw&&!input.pitch&&input.weapon==255&&std::all_of(input.preferences.begin(),input.preferences.end(),[](auto v){return v==255;});};
    auto read_ui=[&]{ControlInput ui;host.take_controls(&ui);return ui;};
    ControlInput human;human.pad.buttons=ssne::PAD_START|ssne::PAD_A|ssne::PAD_R;human.pressed=ssne::PAD_Z;human.pad.stick_x=210;human.pad.stick_y=64;human.pad.cstick_x=255;human.pad.cstick_y=0;human.pad.trigger_l=human.pad.trigger_r=255;human.yaw=1234;human.pitch=-1234;human.weapon=3;human.preferences.fill(2);
    host.submit_host_input(human);auto ui=read_ui();
    require(ui.pad.buttons==(ssne::PAD_START|ssne::PAD_A|ssne::PAD_Z)&&ui.pad.stick_x==210&&ui.pad.stick_y==64,"AFK UI host Start/menu navigation lost");
    require(ui.pad.cstick_x==128&&ui.pad.cstick_y==128&&!ui.pad.trigger_l&&!ui.pad.trigger_r&&!ui.yaw&&!ui.pitch&&ui.weapon==255&&std::all_of(ui.preferences.begin(),ui.preferences.end(),[](auto v){return v==255;}),"AFK UI leaked fire/look/weapon/preferences");
    auto gameplay=host.take_controls();require(gameplay[local.slot].afk&&neutral(gameplay[local.slot].input),"AFK UI bypassed host gameplay sanitation");
    host.submit_host_input({});require(neutral(read_ui()),"AFK UI retained a released menu button");host.take_controls();
    ControlInput tap;tap.pressed=ssne::PAD_START;host.submit_host_input(tap);
    require(read_ui().pad.buttons==ssne::PAD_START,"AFK UI lost a pressed-only Start tap");host.take_controls();require(neutral(read_ui()),"AFK UI replayed consumed Start tap");
    const auto remote_sequence=host.take_controls()[remote.slot].sequence;client.submit_input(human);
    wait_for([&]{auto current=host.take_controls()[remote.slot];require(neutral(current.input),"AFK UI bypassed remote gameplay sanitation");return current.sequence>remote_sequence;},"AFK UI remote input receipt");require(neutral(read_ui()),"Remote AFK input reached host UI");
    host.submit_host_input(human);std::this_thread::sleep_for(std::chrono::milliseconds(280));require(neutral(read_ui()),"AFK UI kept stale host input");
    host.submit_host_input(tap);require(host.host_afk(2,false)==CommandResult::Applied&&neutral(read_ui()),"AFK UI survived human return");
    require(host.host_afk(3,true)==CommandResult::Applied&&neutral(read_ui()),"AFK UI replayed an earlier takeover's tap");
    client.stop();host.stop();require(neutral(read_ui()),"AFK UI survived server stop");
}
int main() {
    authority_checkpoint_test::checkpoint_regression();
    std::cout<<"Authority checkpoint header: "<<authority_checkpoint_test::checkpoint_checks<<" checks passed.\n";
    check_afk_host_ui_input();
    check_lobby_readiness();check_lobby_legacy_epoch_fallback();check_lobby_setup_atomic_readiness();check_lobby_failed_start_recovery();
    AuthoritativeServer host(8);host.submit_host_input({});
    require(host.start(0,"123456",1234),"Host start");auto host_id=host.status().player;auto port=host.status().port;
    check_connecting_lifecycle();
    AuthoritativeClient bad;require(bad.start("127.0.0.1",port,"123456",4321,"Wrong data"),"Wrong-data client start");
    wait_for([&]{return !bad.status().connecting;},"Wrong fingerprint retained Pending");require(!bad.status().connected && bad.status().message.find("differs")!=std::string::npos,"Wrong fingerprint not rejected");bad.stop();
    require(bad.start("127.0.0.1",port,"654321",1234,"Wrong PIN"),"Wrong-PIN client start");
    wait_for([&]{return !bad.status().connecting;},"Wrong PIN retained Pending");
    require(!bad.status().connected && bad.status().message.find("differs")!=std::string::npos,"Wrong PIN lost rejection reason");bad.stop();
    require(host.status().joined==1,"Rejected peer took a slot");
    require(host.set_match_player_limit(1),"Mode admission limit not set");
    AuthoritativeClient limited;require(limited.start("127.0.0.1",port,"123456",1234,"Mode full"),"Limited client start");
    wait_for([&]{return !limited.status().connecting;},"Full mode retained Pending");require(!limited.status().connected && limited.status().message.find("selected game mode is full")!=std::string::npos,"Versus admission limit not enforced");limited.stop();
    require(host.status().joined==1,"Mode-rejected peer took a slot");
    require(host.set_match_player_limit(8),"Coop admission limit not restored");
    std::array<std::unique_ptr<AuthoritativeClient>,7> clients;
    for(unsigned i=0;i<7;++i){clients[i]=std::make_unique<AuthoritativeClient>();require(clients[i]->start("127.0.0.1",port,"123456",1234,"Player "+std::to_string(i+1)),"Client start");}
    wait_for([&]{return host.status().joined==8 && std::all_of(clients.begin(),clients.end(),[](auto& c){return c->status().connected;});},"Eight connections not assigned");
    require(!host.set_match_player_limit(4),"Versus limit discarded occupied upper slots");
    std::array<PlayerId,8> ids;ids[0]=host_id;
    for(auto& client:clients){require(!client->status().connecting,"Assigned player still Pending");auto id=client->status().player;require(id.slot>0 && id.slot<8 && !ids[id.slot].generation,"Duplicate slot");ids[id.slot]=id;}
    auto events=host.take_events();require(events.size()==8,"Join events");
    WorldState world;world.tick=1;world.level="Levels/Rome02";world.mission={{"door.open",int64_t(1)},{"boss.phase",int64_t(2)},{"secret.found",int64_t(1)}};
    for(auto id:ids) {
        EntityState character;character.id=100+id.slot;character.kind=EntityKind::Player;character.owner=id;character.definition=2;
        character.properties={{"position",std::array<float,3>{float(id.slot),0,10}},{"health",87.f},{"ammo",std::vector<int64_t>{12,48}}};world.entities[character.id]=character;
    }
    for(unsigned n=0;n<1000;++n){EntityState enemy;enemy.id=1000+n;enemy.kind=EntityKind::Enemy;enemy.definition=12;enemy.properties={{"position",std::array<float,3>{float(n),0,50}},{"health",n<100?0.f:15.f}};world.entities[enemy.id]=enemy;}
    std::string error;require(host.publish(world,error),error.c_str());
    for(auto id:ids)require(host.attach_character(id,100+id.slot),"Host character binding");require(host.ready_host(),"Host readiness");
    for(auto& client:clients) {
        wait_for([&]{return client->latest_world()->players[client->status().player.slot].character!=0;},"Client missing character snapshot");
        require(client->latest_world()->entities.size()==world.entities.size(),"Incomplete join world");
        client->ready(world.epoch);
    }
    wait_for([&]{auto c=host.take_controls();return std::all_of(c.begin(),c.end(),[](auto& input){return input.ready;});},"Ready acknowledgements");
    std::array<bool,8> seen{};ControlInput local;local.pad.stick_x=132;local.yaw=9999;host.submit_host_input(local);
    for(auto& client:clients){auto slot=client->status().player.slot;ControlInput input;input.pad.stick_x=uint8_t(132+slot);input.pad.buttons=ssne::PAD_A;input.yaw=int32_t(slot*10000);client->submit_input(input);}
    wait_for([&]{
        auto controls=host.take_controls();
        for(unsigned slot=0;slot<8;++slot) {
            if(controls[slot].sequence && !seen[slot]) {
                require(controls[slot].id==ids[slot],"Input changed player identity");
                require(controls[slot].input.pad.stick_x==132+slot,"Input routed to wrong slot");
                require(controls[slot].input.yaw==int32_t(slot?slot*10000:9999),"Mouse input lost or duplicated");
                seen[slot]=true;
            }
        }
        return std::all_of(seen.begin(),seen.end(),[](bool b){return b;});
    },"Missing native control request");
    auto start=Clock::now();
    for(unsigned tick=2;tick<=401;++tick) {
        world.tick=tick;world.entities.at(1000).properties["health"]=float(tick);world.entities.at(1000).properties["position"]=std::array<float,3>{float(tick),0,50};
        require(host.publish(world,error),error.c_str());host.take_controls();
        // No client game/render loop consumes state during this interval.
    }
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
    require(elapsed<3000,"Slow client render loop blocks host publication");
    for(auto& client:clients)wait_for([&]{return client->latest_world()->tick==401;},"Coalesced authoritative state not delivered");
    auto bytes=clients[0]->status().bytes_received;clients[0]->request_snapshot();
    wait_for([&]{return clients[0]->status().bytes_received>bytes+1000;},"Explicit resnapshot not sent");
    require(clients[0]->latest_world()->tick==401,"Resnapshot lost current state");
    clients[6]->set_afk(true);auto afk_id=clients[6]->status().player;
    wait_for([&]{return clients[6]->latest_world()->players[afk_id.slot].afk;},"AFK command not authoritative");
    auto preserved=clients[6]->latest_world()->entities.at(100+afk_id.slot);
    ControlInput illegal;illegal.pad.stick_x=255;illegal.pad.buttons=ssne::PAD_R;clients[6]->submit_input(illegal);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));auto controls=host.take_controls();
    require(controls[afk_id.slot].afk && controls[afk_id.slot].input.pad.buttons==0 && controls[afk_id.slot].input.pad.stick_x==128,"AFK human retains control");
    auto scene=host.begin_cutscene();require(host.skip_vote().required==7,"AFK vote electorate");
    world.mission["Session.Cutscene.Id"]=int64_t(scene);world.mission["Session.Cutscene.Reference"]=int64_t(1953);
    world.mission["Session.Cutscene.Camera"]=std::vector<float>{1,0,0,4,0,1,0,5,0,0,1,6,90,0};
    ++world.tick;require(host.publish(world,error),error.c_str());
    wait_for([&]{return clients[0]->latest_world()->mission.contains("Session.Cutscene.Camera");},"Native camera metadata not delivered");
    host.host_skip(1,scene);for(unsigned i=0;i<6;++i)clients[i]->vote_skip(scene);
    wait_for([&]{return host.skip_vote().skip_requested;},"Active unanimous vote blocked by AFK");require(host.end_cutscene(scene),"Cutscene end");
    wait_for([&]{return !clients[0]->latest_world()->vote.cutscene && !clients[0]->latest_world()->mission.contains("Session.Cutscene.Camera");},"Native camera and vote not cleared atomically");
    for(auto it=world.mission.begin();it!=world.mission.end();)
        if(it->first.starts_with("Session.Cutscene."))it=world.mission.erase(it);else ++it;
    clients[6]->set_afk(false);wait_for([&]{return !clients[6]->latest_world()->players[afk_id.slot].afk;},"AFK return");
    require(clients[6]->latest_world()->entities.at(100+afk_id.slot)==preserved,"AFK return respawned or changed inventory");
    scene=host.begin_cutscene();host.host_skip(2,scene);for(unsigned i=0;i<6;++i)clients[i]->vote_skip(scene);
    wait_for([&]{return host.skip_vote().received==7;},"Seven votes not counted");require(!host.skip_vote().skip_requested,"Unanimity bypassed");
    auto old_id=clients[6]->status().player;clients[6]->stop();
    wait_for([&]{return host.status().joined==7 && host.skip_vote().skip_requested;},"Disconnect stopped host or blocked vote");require(host.end_cutscene(scene),"End disconnected vote");
    require(host.status().connected,"Client disconnect ended host");
    clients[6]=std::make_unique<AuthoritativeClient>();require(clients[6]->start("127.0.0.1",port,"123456",1234,"Late join"),"Late join startup");
    wait_for([&]{return clients[6]->status().connected && host.status().joined==8;},"Late join not assigned");auto new_id=clients[6]->status().player;
    require(new_id.slot==old_id.slot && new_id.generation!=old_id.generation,"Rejoined slot has stale identity");
    events=host.take_events();require(events.size()==2 && events[0].kind==ConnectionEvent::Kind::Left && events[1].kind==ConnectionEvent::Kind::Joined,"Leave/rejoin events");
    world.entities.erase(100+old_id.slot);EntityState replacement=preserved;replacement.id=500+new_id.slot;replacement.owner=new_id;world.entities[replacement.id]=replacement;world.tick=402;
    require(host.publish(world,error),error.c_str());require(host.attach_character(new_id,replacement.id),"Late character attachment");
    wait_for([&]{return clients[6]->latest_world()->tick==402 && clients[6]->latest_world()->players[new_id.slot].character==replacement.id;},"Late join snapshot incomplete");
    auto late=clients[6]->latest_world();
    auto expected_mission=world.mission;
    expected_mission["Session.Lobby.Open"]=int64_t(1);
    expected_mission["Session.Lobby.ReadyMask"]=int64_t(0);
    expected_mission["Session.Lobby.ReadyToken"]=std::bit_cast<int64_t>(lobby_token(host));
    expected_mission["Session.Bots.Mask"]=int64_t(0);
    expected_mission["Session.Spectators.VoluntaryMask"]=int64_t(0);
    expected_mission["Session.Spectators.LockMask"]=int64_t(0);
    expected_mission["Session.Bots.Skills"]=std::vector<int64_t>(max_players,0);
    require(late->mission==expected_mission && late->entities.at(1000).properties==world.entities.at(1000).properties,"Late join reset door/boss/mission/enemy state");
    clients[6]->ready(world.epoch);wait_for([&]{return host.take_controls()[new_id.slot].ready;},"Late join readiness");
    auto attacker_slot=clients[5]->status().player;clients[5]->stop();wait_for([&]{return host.status().joined==7;},"Slot not released");
    auto attacker=raw_peer(port);auto malicious=frame(wire::Message::World,{});require(transfer(attacker,malicious.data(),malicious.size(),true),"Forged world send");
    wait_for([&]{return host.status().joined==7;},"Client world authority not rejected");closesocket(attacker);
    require(host.status().connected,"Forged client state stopped server");
    world.tick=403;require(host.publish(world,error),error.c_str());
    wait_for([&]{return clients[0]->latest_world()->tick==403;},"Host did not continue after malicious peer");
    require(clients[0]->latest_world()->mission==expected_mission,"Client forged mission state");
    // Original mission results remove the native characters before LevelExit.
    // Invalid loading publications must leave live authority untouched.
    WorldState loading=world;loading.tick=404;loading.entities.clear();
    auto invalid=loading;invalid.level=std::string(257,'X');
    require(!host.publish(invalid,error,true) && host.take_controls()[0].ready,"Invalid loading state mutated authority");
    require(host.publish(loading,error,true),error.c_str());
    wait_for([&]{return clients[0]->latest_world()->tick==404;},"Results state not delivered");
    require(!host.ready_host(),"Results retained a live host character");
    for(unsigned i=0;i<7;++i)if(i!=5)clients[i]->ready(world.epoch);
    local={};local.pad.buttons=ssne::PAD_A;host.submit_host_input(local);
    require(host.take_controls()[0].input.pad.buttons==ssne::PAD_A,"Host cannot dismiss native results while characters load");
    require(!clients[0]->latest_world()->players[0].character && clients[0]->latest_world()->entities.empty(),"Results retained old entities/bindings");
    // Only a validated host result snapshot permits AFK changes without a
    // character. The socket client sends the usual authenticated Afk message.
    require(host.host_afk(3,true)==CommandResult::NotEligible,"Ordinary loading allowed host AFK change");
    auto malformed_results=loading;malformed_results.mission["Session.Results.Active"]=std::string("1");
    require(!host.publish(malformed_results,error,true) && host.host_afk(4,true)==CommandResult::NotEligible,"Malformed result metadata granted AFK permission");
    loading.tick=405;loading.mission["Session.Results.Active"]=int64_t(1);loading.mission["Session.Results.Sequence"]=int64_t(1);
    loading.mission["Session.Results.Score"]=int64_t(0);loading.mission["Session.Results.Level"]=world.level;loading.mission["Session.Results.Values"]=std::vector<float>(13,0.f);
    require(host.publish(loading,error,true),error.c_str());
    wait_for([&]{return clients[0]->latest_world()->tick==405;},"Result permission snapshot not delivered");
    require(host.host_afk(5,true)==CommandResult::Applied,"Unready host cannot enter AFK on results");
    ControlInput manual_result;manual_result.pad.buttons=ssne::PAD_A|ssne::PAD_X|ssne::PAD_LEFT|ssne::PAD_R|ssne::PAD_START;
    manual_result.pad.stick_x=255;manual_result.pad.cstick_y=0;manual_result.pad.trigger_r=255;manual_result.yaw=1234;manual_result.pitch=-1234;manual_result.weapon=3;manual_result.preferences.fill(2);
    host.submit_host_input(manual_result);auto menu_control=host.take_controls()[0];
    auto only_menu=[](const PlayerControl& control){return control.afk && !control.ready && control.input.pad.buttons==(ssne::PAD_A|ssne::PAD_X|ssne::PAD_LEFT) && control.input.pad.stick_x==128 && control.input.pad.stick_y==128 && control.input.pad.cstick_x==128 && control.input.pad.cstick_y==128 && !control.input.pad.trigger_l && !control.input.pad.trigger_r && !control.input.yaw && !control.input.pitch && control.input.weapon==255 && std::all_of(control.input.preferences.begin(),control.input.preferences.end(),[](auto value){return value==255;});};
    require(only_menu(menu_control),"AFK host result confirmation was dropped or leaked character controls");
    ++loading.tick;require(host.publish(loading,error,true),error.c_str());require(only_menu(host.take_controls()[0]),"Repeated result loading publication erased host menu input");
    const auto result_client=clients[0]->status().player;const auto before_remote_input=host.take_controls()[result_client.slot].sequence;
    clients[0]->submit_input(manual_result);
    PlayerControl received_remote;
    wait_for([&]{received_remote=host.take_controls()[result_client.slot];return received_remote.sequence>before_remote_input;},"Remote result input was not received");
    require(!received_remote.input.pad.buttons && !received_remote.input.pressed && !received_remote.input.yaw && !received_remote.input.pitch && received_remote.input.pad.stick_x==128 && received_remote.input.pad.stick_y==128 && received_remote.input.pad.cstick_x==128 && received_remote.input.pad.cstick_y==128 && !received_remote.input.pad.trigger_l && !received_remote.input.pad.trigger_r && received_remote.input.weapon==255,"Fresh remote client result input reached shared menu controls");
    std::this_thread::sleep_for(std::chrono::milliseconds(280));require(!host.take_controls()[0].input.pad.buttons,"Stale AFK host result input remained active");
    host.submit_host_input({});require(!host.take_controls()[0].input.pad.buttons,"Host result input release was lost");
    require(host.host_afk(6,false)==CommandResult::Applied,"Unready host cannot return from AFK on results");
    clients[0]->set_afk(true);wait_for([&]{return host.participants()[result_client.slot].afk;},"Unready client result AFK request rejected");
    clients[0]->set_afk(false);wait_for([&]{return !host.participants()[result_client.slot].afk;},"Unready client cannot return from AFK on results");
    require(!host.participants()[0].ready && !host.participants()[result_client.slot].ready && !host.skip_vote().cutscene,"Results AFK change forged readiness or a cutscene");
    for(auto it=loading.mission.begin();it!=loading.mission.end();)if(it->first.starts_with("Session.Results."))it=loading.mission.erase(it);else ++it;
    loading.tick=407;require(host.publish(loading,error,true),error.c_str());
    local={};local.pad.buttons=ssne::PAD_A;host.submit_host_input(local);++loading.tick;require(host.publish(loading,error,true),error.c_str());require(!host.take_controls()[0].input.pad.buttons,"Ordinary loading retained result menu input");
    require(host.host_afk(7,true)==CommandResult::NotEligible,"Results AFK exception survived ordinary loading");
    // Level epoch resets readiness and removes old-level entities atomically.
    world.epoch=2;world.tick=0;world.level="Levels/Rome03";
    for(auto it=world.entities.begin();it!=world.entities.end();)if(it->second.kind!=EntityKind::Player)it=world.entities.erase(it);else ++it;
    require(host.publish(world,error),error.c_str());
    wait_for([&]{return clients[0]->latest_world()->epoch==2;},"Level epoch not delivered");
    require(!clients[0]->latest_world()->players[0].ready && !clients[0]->latest_world()->entities.contains(1000),"Old-level state retained");
    for(const auto& [id,e]:world.entities)if(e.kind==EntityKind::Player && e.owner.slot!=attacker_slot.slot)require(host.attach_character(e.owner,id),"New-level character attachment failed");
    require(host.ready_host(),"Host did not resume new level");
    for(unsigned i=0;i<7;++i)if(i!=5)clients[i]->ready(2);
    wait_for([&]{auto controls=host.take_controls();for(unsigned i=0;i<8;++i)if(i!=attacker_slot.slot && !controls[i].ready)return false;return true;},"New level readiness");
    for(auto& client:clients)client->stop();host.stop();
    std::cout<<"Real TCP authority test: connection lifecycle, 8 clients/host slots, late joins, independent host clock, AFK identity, unanimous votes, disconnects, forged state rejection and level epochs passed; 400 host publications in "<<elapsed<<" ms.\n";
}
