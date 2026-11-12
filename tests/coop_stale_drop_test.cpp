#include "authoritative_session.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <thread>
#include <functional>
#include <iostream>
#include <cstdlib>
using namespace port::coop;
void require(bool value,const char* text){if(!value){std::cerr<<text<<'\n';std::exit(1);}}
void wait_for(const std::function<bool()>& ready){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!ready()){require(std::chrono::steady_clock::now()<end,"TCP readiness timeout");std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
void u32(std::vector<uint8_t>& b,uint32_t n){for(int i=3;i>=0;--i)b.push_back(uint8_t(n>>(i*8)));}
void u64(std::vector<uint8_t>& b,uint64_t n){u32(b,uint32_t(n>>32));u32(b,uint32_t(n));}
void string(std::vector<uint8_t>& b,const std::string& s){u32(b,uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
void send_frame(SOCKET socket,wire::Message type,const std::vector<uint8_t>& body){std::vector<uint8_t> b;u32(b,wire::magic);b.push_back(0);b.push_back(wire::version);b.push_back(0);b.push_back(uint8_t(type));u32(b,uint32_t(body.size()));b.insert(b.end(),body.begin(),body.end());size_t sent=0;while(sent<b.size()){const auto n=send(socket,reinterpret_cast<const char*>(b.data()+sent),int(b.size()-sent),0);require(n>0,"Raw TCP send failed");sent+=size_t(n);}}
int main(){
 AuthoritativeServer host(2);require(host.start(0,"123456",1234),"Host start failed");
 SOCKET old=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(old!=INVALID_SOCKET,"Socket failed");DWORD timeout=5000;setsockopt(old,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(host.status().port);require(connect(old,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Old peer connect failed");
 std::vector<uint8_t> hello;u64(hello,1234);string(hello,"123456");string(hello,"Old peer");send_frame(old,wire::Message::Hello,hello);wait_for([&]{return host.status().joined==2;});
 const auto old_id=host.participants()[1].id;host.disconnect(old_id);
 AuthoritativeClient replacement;require(replacement.start("127.0.0.1",host.status().port,"123456",1234,"Replacement"),"Replacement start failed");wait_for([&]{return replacement.status().connected;});const auto id=replacement.status().player;
 require(id.slot==old_id.slot && id.generation>old_id.generation,"Slot generation was reused");
 WorldState world;world.epoch=1;world.tick=1;world.level="Levels/Rome01";
 for(auto who:{host.status().player,id}){EntityState e;e.id=100+who.slot;e.kind=EntityKind::Player;e.definition=2;e.owner=who;e.properties={{"position",std::array<float,3>{0,0,0}},{"health",100.f},{"player_slot",int64_t(who.slot)}};world.entities[e.id]=e;}
 std::string error;require(host.publish(world,error),error.c_str());for(const auto& [entity,e]:world.entities)require(host.attach_character(e.owner,entity),"Character attach failed");require(host.ready_host(),"Host ready failed");replacement.ready(1);wait_for([&]{return host.take_controls()[id.slot].ready;});
 ControlInput input;input.pad.stick_x=222;replacement.submit_input(input);wait_for([&]{const auto c=host.take_controls()[id.slot];return c.sequence && c.input.pad.stick_x==222;});
 const auto before=host.take_controls()[id.slot].sequence;
 // The old socket stays open across reassignment, then sends a stale request.
 std::vector<uint8_t> stale;u64(stale,1);send_frame(old,wire::Message::Ready,stale);
 char data[4096];int n=0;do{n=recv(old,data,sizeof(data),0);}while(n>0);require(n==0,"Old peer was not closed after stale identity request");closesocket(old);
 std::this_thread::sleep_for(std::chrono::milliseconds(20));const auto after=host.take_controls()[id.slot];
 require(after.id==id && after.ready && after.sequence==before && after.input.pad.stick_x==222,"Late old-peer drop erased replacement controls");
 replacement.stop();host.stop();std::cout<<"Real TCP stale old-peer close preserves replacement identity, readiness and controls.\n";
}
