#include "authoritative_session.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace port::coop;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void wait_for(const std::function<bool()>& predicate,const char* message){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(!predicate()){require(std::chrono::steady_clock::now()<end,message);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
void put32(std::vector<uint8_t>& bytes,uint32_t value){for(int i=3;i>=0;--i)bytes.push_back(uint8_t(value>>(i*8)));}
void put64(std::vector<uint8_t>& bytes,uint64_t value){put32(bytes,uint32_t(value>>32));put32(bytes,uint32_t(value));}
void text(std::vector<uint8_t>& bytes,const std::string& value){put32(bytes,uint32_t(value.size()));bytes.insert(bytes.end(),value.begin(),value.end());}
std::vector<uint8_t> frame(wire::Message type,const std::vector<uint8_t>& body){
    std::vector<uint8_t> bytes;put32(bytes,wire::magic);bytes.push_back(uint8_t(wire::version>>8));bytes.push_back(uint8_t(wire::version));
    bytes.push_back(uint8_t(uint16_t(type)>>8));bytes.push_back(uint8_t(type));put32(bytes,uint32_t(body.size()));bytes.insert(bytes.end(),body.begin(),body.end());return bytes;
}
uint32_t read32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
class RawPeer {
    SOCKET socket_=INVALID_SOCKET;
    void transfer(uint8_t* data,size_t size,bool writing){while(size){auto done=writing?send(socket_,reinterpret_cast<const char*>(data),int(size),0):recv(socket_,reinterpret_cast<char*>(data),int(size),0);require(done>0,"Raw chat transfer failed");data+=done;size-=size_t(done);}}
public:
    explicit RawPeer(uint16_t port){
        socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(socket_!=INVALID_SOCKET,"Raw socket creation");
        DWORD timeout=3000;setsockopt(socket_,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(socket_,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
        require(connect(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Raw loopback connect");
        std::vector<uint8_t> hello;put64(hello,1234);text(hello,"123456");text(hello,"Wire Name");hello.push_back(255);send_frame(wire::Message::Hello,hello);
        uint8_t header[wire::header_size];transfer(header,sizeof(header),false);
        require(header[7]==uint8_t(wire::Message::Assigned) && read32(header+8)==16,"Raw peer assignment");
        uint8_t assignment[16];transfer(assignment,sizeof(assignment),false);
    }
    ~RawPeer(){if(socket_!=INVALID_SOCKET){shutdown(socket_,SD_BOTH);closesocket(socket_);}}
    void send_frame(wire::Message type,const std::vector<uint8_t>& payload){auto bytes=frame(type,payload);transfer(bytes.data(),bytes.size(),true);}
    void send_chat(const std::string& value,bool forge_name=false){std::vector<uint8_t> bytes;text(bytes,value);if(forge_name)text(bytes,"Host");send_frame(wire::Message::Chat,bytes);}
};
void model_checks(){
    require(normalize_chat_text("  Hallo Welt  ")==std::optional<std::string>("Hallo Welt"),"Outer spaces were not trimmed");
    require(normalize_chat_text("\xc2\xa0" "Grüße à Sam" "\xc2\xa0")==std::optional<std::string>("Grüße à Sam"),"Unicode chat trim failed");
    for(const std::string invalid:std::vector<std::string>{"", "  \t\r\n", "\xc2\xa0", "bad\nline", "bad\tline", std::string("nul\0inside",10), std::string("\xc0\xaf",2), std::string("\xed\xa0\x80",3), std::string("\xf4\x90\x80\x80",4), std::string("\xf0\x9f",2), std::string(257,'x')})
        require(!normalize_chat_text(invalid),"Invalid chat accepted");
    std::string max_text;for(unsigned i=0;i<128;++i)max_text+="ä";
    require(max_text.size()==chat_text_bytes && normalize_chat_text(max_text)==max_text,"Maximum UTF-8 chat failed");
    require(!normalize_chat_text(max_text.substr(0,max_text.size()-1)),"Truncated UTF-8 accepted");
    require(chat_name_valid("Spieler é") && !chat_name_valid("\xc0\xaf") && !chat_name_valid("  "),"Chat name validation");
    Participant sender;sender.id={0,1};sender.connected=true;sender.name="Trusted name";
    ChatLog log;
    require(!log.send(sender," ",100),"Blank log entry accepted");
    for(unsigned i=0;i<chat_burst_limit;++i)require(log.send(sender,"burst "+std::to_string(i),100),"Allowed burst rejected");
    require(!log.send(sender,"fifth",100) && !log.send(sender,"early",5099),"Rate limit bypassed");
    require(log.send(sender,"next window",5100),"Rate window did not reopen");
    require(log.messages().size()==5 && log.messages().back().sequence==5,"Rejected messages consumed sequence IDs");
    log.clear();require(log.messages().empty(),"Chat clear retained history");
    for(unsigned i=0;i<80;++i)require(log.send(sender,std::to_string(i),uint64_t(i)*5001),"History setup send");
    const auto retained=log.messages();require(retained.size()==chat_history_limit && retained.front().sequence==17 && retained.back().sequence==80,"History was not bounded to64 newest messages");
    auto copy=retained;copy.front().text="changed";require(log.messages().front().text!="changed","Chat history exposed mutable aliases");
    sender.id.generation=2;
    for(unsigned i=0;i<chat_burst_limit;++i)require(log.send(sender,"new generation",400000),"New player inherited old slot rate limit");
    require(!log.send(sender,"fifth new generation",400000),"New player rate limit not enforced");
    sender.connected=false;require(!log.send(sender,"disconnected",500000),"Disconnected participant sent chat");
}
void same_participants(const std::array<Participant,max_players>& a,const std::array<Participant,max_players>& b){
    for(unsigned i=0;i<max_players;++i)require(a[i].id==b[i].id && a[i].connected==b[i].connected && a[i].ready==b[i].ready && a[i].afk==b[i].afk && a[i].character==b[i].character && a[i].name==b[i].name,"Chat changed gameplay participation/readiness/AFK");
}
void network_checks(){
    _putenv_s("SSNE_TEST_HIDDEN","1"); // Loopback only; never opens a LAN listener.
    AuthoritativeServer host(8);AuthoritativeClient a,b;
    require(!host.send_chat("unstarted") && !a.send_chat("unstarted"),"Unstarted chat accepted");
    require(host.start(0,"123456",1234,"Host"),"Host start");const auto port=host.status().port;
    require(a.start("127.0.0.1",port,"123456",1234,"Spieler ä") && b.start("127.0.0.1",port,"123456",1234,"Client B"),"Clients start");
    wait_for([&]{return host.status().joined==3 && a.status().connected && b.status().connected && a.latest_world()->revision && b.latest_world()->revision;},"Chat peers did not connect");
    const auto people=host.participants();const auto vote=host.skip_vote();const auto authority=host.checkpoint_authority();
    auto synchronized=[&](size_t count){wait_for([&]{return host.chat_messages().size()==count && a.chat_messages().size()==count && b.chat_messages().size()==count;},"Chat delivery stalled");require(host.chat_messages()==a.chat_messages() && host.chat_messages()==b.chat_messages(),"Peers disagree on chat order or text");};
    require(host.send_chat("  host hello  "),"Host send");synchronized(1);
    require(a.send_chat("Grüße à tous %s ###literal"),"Unicode client send");synchronized(2);
    require(b.send_chat("Host: this is still Client B"),"Second client send");synchronized(3);
    std::string maximum;for(unsigned i=0;i<128;++i)maximum+="ä";
    require(a.send_chat(maximum),"256-byte client message rejected");synchronized(4);
    const auto messages=host.chat_messages();
    require(messages[0].name=="Host" && messages[0].sender==host.status().player && messages[0].text=="host hello","Host echo identity or trim");
    require(messages[1].name=="Spieler ä" && messages[1].sender==a.status().player && messages[1].text=="Grüße à tous %s ###literal","Client name not derived from authenticated identity");
    require(messages[2].name=="Client B" && messages[2].sender==b.status().player,"Message text forged sender identity");
    for(unsigned i=0;i<messages.size();++i)require(messages[i].sequence==i+1,"Chat sequence not monotonic/exactly once");
    require(!host.send_chat("\n ") && !a.send_chat("\xc0\xaf") && !b.send_chat(std::string(257,'x')),"Invalid API chat accepted");
    same_participants(people,host.participants());require(host.skip_vote().cutscene==vote.cutscene && host.skip_vote().received==vote.received && host.skip_vote().required==vote.required,"Chat changed skip votes");
    require(host.checkpoint_authority()==authority,"Chat entered gameplay checkpoint authority");
    // Larger than a socket read: a pending world must never be overwritten by chat.
    auto world=*host.latest_world();world.tick=2;world.level="Chat transport regression";
    for(unsigned i=0;i<1000;++i){EntityState enemy;enemy.id=100+i;enemy.kind=EntityKind::Enemy;enemy.definition=12;enemy.properties={{"position",std::array<float,3>{float(i),0,20}},{"health",15.f},{"padding",std::string(128,'x')}};world.entities[enemy.id]=std::move(enemy);}
    std::string error;require(host.publish(world,error),error.c_str());
    require(b.send_chat("during world transfer"),"Chat during world send");synchronized(5);
    wait_for([&]{return a.latest_world()->entities.size()==1000 && b.latest_world()->entities.size()==1000;},"Chat corrupted partial world transfer");
    const auto old_id=a.status().player;a.stop();require(a.chat_messages().empty() && !a.send_chat("stopped"),"Stopped client retained session chat");
    wait_for([&]{return host.status().joined==2;},"Disconnected slot not released");
    require(a.start("127.0.0.1",port,"123456",1234,"Client A again"),"Client restart");
    wait_for([&]{return a.status().connected && a.chat_messages().size()==5;},"Late join missed retained history");
    require(a.status().player.slot==old_id.slot && a.status().player.generation>old_id.generation,"Reconnect identity generation not renewed");
    require(a.chat_messages()==host.chat_messages(),"Reconnect history was reordered/duplicated");
    require(a.send_chat("after reconnect"),"Reconnected client chat rejected");synchronized(6);
    require(host.chat_messages().back().name=="Client A again" && host.chat_messages().back().sender==a.status().player,"Reconnected sender used stale identity");
    world=*host.latest_world();++world.epoch;++world.tick;world.level="Next level";world.entities.clear();require(host.publish(world,error,true),error.c_str());
    wait_for([&]{return a.latest_world()->epoch==world.epoch && b.latest_world()->epoch==world.epoch;},"Next world not delivered");
    require(host.chat_messages().size()==6 && a.chat_messages().size()==6 && b.chat_messages().size()==6,"Level change cleared/replayed session chat");
    {
        RawPeer raw(port);wait_for([&]{return host.status().joined==4;},"Raw chat peer not joined");
        raw.send_chat("\xc0\xaf");raw.send_chat("   ");
        for(unsigned i=0;i<20;++i)raw.send_chat("wire burst "+std::to_string(i));
        synchronized(10);require(host.status().joined==4,"Rate-limited chat disconnected player");
        const auto burst=host.chat_messages();
        for(size_t i=6;i<10;++i)require(burst[i].name=="Wire Name" && burst[i].text=="wire burst "+std::to_string(i-6),"Server rate/identity/order enforcement failed");
        raw.send_chat("forged sender",true);
        wait_for([&]{return host.status().joined==3;},"Forged payload was not rejected");
        require(host.chat_messages().size()==10 && a.status().connected && b.status().connected,"Malformed sender payload changed history or other peers");
    }
    AuthoritativeServer rate_host;require(rate_host.start(0,"123456",1234,"Rate host"),"Rate host start");
    for(unsigned i=0;i<4;++i)require(rate_host.send_chat("host burst"),"Host allowed burst rejected");
    require(!rate_host.send_chat("fifth"),"Host API bypassed rate limit");rate_host.stop();require(rate_host.chat_messages().empty(),"Host stop retained chat history");
    a.stop();b.stop();host.stop();require(host.chat_messages().empty() && !host.send_chat("after stop"),"Stopped host retained live chat");
}
}
int main()try{model_checks();network_checks();std::cout<<"Chat passed: authenticated host/two-client exchange, UTF-8 and limits, ordered echoes, partial-world coexistence, reconnect/history, immutable gameplay state, malformed payload rejection and bounded rate/history.\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
