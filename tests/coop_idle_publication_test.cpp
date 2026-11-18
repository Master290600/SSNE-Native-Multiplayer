#include "authoritative_session.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>
using namespace port::coop;
namespace {
void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
void wait_for(const std::function<bool()>& done,const char* why){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(!done()){require(std::chrono::steady_clock::now()<end,why);std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
SOCKET pending(uint16_t port){auto s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(s!=INVALID_SOCKET,"Pending socket creation failed");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);require(connect(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Pending TCP connect failed");return s;}
void close_pending(SOCKET s){shutdown(s,SD_BOTH);closesocket(s);}
void player(WorldState& w,PlayerId owner){EntityState e;e.id=100+owner.slot;e.kind=EntityKind::Player;e.definition=2;e.owner=owner;e.properties={{"health",100.f},{"player_slot",int64_t(owner.slot)},{"position",std::array<float,3>{float(owner.slot),1,2}}};w.entities[e.id]=std::move(e);}
void attach(AuthoritativeServer& server,const WorldState& w){for(const auto& [id,e]:w.entities)if(e.kind==EntityKind::Player)require(server.attach_character(e.owner,id),"Attach failed");require(server.ready_host(),"Host ready failed");}
}
int main(){
    AuthoritativeServer host(4);require(host.start(0,"123456",1234,"Idle host"),"Host start failed");const auto first=host.publication_demand();require(!first.tcp_peers&&!first.all_ready&&first.owners[0]==host.status().player,"Host is not a TCP peer / must initially be unready");
    WorldState w;w.level="Rlevel1_2";w.tick=1;player(w,first.host);std::string error;require(host.publish(w,error),error.c_str());attach(host,w);
    auto ready=host.publication_demand();require(!ready.tcp_peers&&ready.all_ready&&ready.serial>first.serial,"Ready host did not wake publication");
    // Inputs and normal publications must not keep resetting the idle deadline.
    const auto quiet_serial=ready.serial;for(unsigned n=0;n<24;++n){ControlInput input;input.yaw=7;host.submit_host_input(input);require(host.take_controls()[0].sequence>0,"Host input delayed by publication policy");++w.tick;require(host.publish(w,error),error.c_str());require(host.publication_demand().serial==quiet_serial,"Input or captured world spuriously wakes idle cadence");}
    const auto bot=host.add_bot(2);require(bool(bot),"Bot admission failed");auto demand=host.publication_demand();require(!demand.tcp_peers&&!demand.all_ready&&demand.serial>quiet_serial,"Unready bot was hidden by no-TCP policy");player(w,*bot);++w.tick;require(host.publish(w,error),error.c_str());require(host.attach_character(*bot,100+bot->slot),"Bot attach failed");require(host.publication_demand().all_ready&&!host.publication_demand().tcp_peers,"Ready in-process bot incorrectly demands network capture");
    auto before=host.publication_demand();require(host.host_afk(1,true)==CommandResult::Applied,"Host AFK failed");demand=host.publication_demand();require(demand.all_ready&&!demand.tcp_peers&&demand.serial>before.serial,"AFK host+bot must stay no-TCP while waking status publication");require(host.host_afk(2,false)==CommandResult::Applied,"Host AFK restore failed");
    // Actual accepted sockets demand fresh capture before sending a Hello.
    auto serial=host.publication_demand().serial;const auto a=pending(host.status().port);wait_for([&]{return host.publication_demand().tcp_peers==1;},"Unassigned socket not counted");require(host.publication_demand().serial>serial,"Accept did not wake publication");const auto b=pending(host.status().port);wait_for([&]{return host.publication_demand().tcp_peers==2;},"Second pending peer not counted");
    serial=host.publication_demand().serial;close_pending(a);wait_for([&]{return host.publication_demand().tcp_peers==1;},"Unassigned close not counted");require(host.publication_demand().serial>serial,"Unassigned close did not wake publication");close_pending(b);wait_for([&]{return !host.publication_demand().tcp_peers;},"Pending sockets did not release demand");
    AuthoritativeClient one,two;require(one.start("127.0.0.1",host.status().port,"123456",1234,"Idle peer one"),"First peer start failed");require(two.start("127.0.0.1",host.status().port,"123456",1234,"Idle peer two"),"Second peer start failed");wait_for([&]{return one.status().connected&&two.status().connected;},"Two-peer Hello timeout");
    demand=host.publication_demand();require(demand.tcp_peers==2&&!demand.all_ready&&demand.owners[one.status().player.slot]==one.status().player&&demand.owners[two.status().player.slot]==two.status().player,"Coherent remote ownership/readiness demand differs");
    player(w,one.status().player);player(w,two.status().player);++w.tick;require(host.publish(w,error),error.c_str());attach(host,w);wait_for([&]{return one.latest_world()->entities.contains(100+one.status().player.slot)&&two.latest_world()->entities.contains(100+two.status().player.slot);},"Remote characters not published");one.ready(1);two.ready(1);wait_for([&]{return host.publication_demand().all_ready;},"Remote readiness stalled behind idle policy");
    serial=host.publication_demand().serial;const auto revision=host.latest_world()->revision;one.request_snapshot();wait_for([&]{return host.publication_demand().serial>serial;},"Forced snapshot request did not wake native publication");require(host.latest_world()->revision==revision,"Demand wake alone changed world revision");
    serial=host.publication_demand().serial;one.set_afk(true);wait_for([&]{return host.publication_demand().serial>serial;},"Remote AFK status did not wake publication");
    one.stop();wait_for([&]{return host.publication_demand().tcp_peers==1;},"First client close demand stale");two.stop();wait_for([&]{return !host.publication_demand().tcp_peers;},"Last client close demand stale");
    // A new epoch clears readiness even with zero TCP sockets. This status
    // disables idle capture until the normal character/ready handoff finishes.
    w.entities.erase(100+one.status().player.slot);w.entities.erase(100+two.status().player.slot);w.epoch=2;w.tick=0;w.level="Alevel10_4";require(host.publish(w,error),error.c_str());require(!host.publication_demand().all_ready,"Epoch reset retained readiness");attach(host,w);require(host.publication_demand().all_ready,"Host/bot epoch reattachment failed");
    require(host.remove_bot(*bot),"Bot removal failed");require(!host.publication_demand().tcp_peers&&host.publication_demand().all_ready,"Bot removal created phantom TCP demand");host.stop();require(!host.publication_demand().all_ready&&!host.publication_demand().tcp_peers,"Stopped server retained demand");
    std::cout<<"TCP publication demand: local host/bots excluded, unready/AFK/epoch transitions, two unassigned/assigned sockets, accept/Hello/drop/forced-request serial wakes, immediate controls and quiet publication serial passed.\n";
}
