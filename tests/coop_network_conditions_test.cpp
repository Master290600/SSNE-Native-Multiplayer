#include "authoritative_session.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <deque>
#include <iostream>
#include <cstdlib>
#include <functional>
using namespace port::coop;
using Clock=std::chrono::steady_clock;
uint64_t now_ms(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
void wait_for(const std::function<bool()>& check,const char* message){auto end=now_ms()+10000;while(!check()){require(now_ms()<end,message);std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
bool nonblock(SOCKET socket){u_long mode=1;return ioctlsocket(socket,FIONBIO,&mode)==0;}
uint32_t read32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
// A real TCP relay. It delays/jitters whole application frames, drops selected
// world/input frames, and briefly stalls forwarding. Frame loss is deliberately
// stronger than TCP's normal reliable delivery; this is not a kernel packet-loss benchmark.
class Link {
    struct Pending {std::vector<uint8_t> bytes;size_t sent=0;uint64_t due=0;};
    struct Flow {SOCKET source=INVALID_SOCKET,destination=INVALID_SOCKET;std::vector<uint8_t> receive;std::deque<Pending> pending;};
    SOCKET listener=INVALID_SOCKET;std::thread thread;std::atomic<bool> stop_{false};
    unsigned delay_,jitter_,counter_=0;uint32_t random_=42;
    uint16_t upstream_;uint64_t began_=0;
    std::atomic<unsigned> dropped_{0};
    unsigned random(){random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;return random_;}
    bool receive(Flow& flow) {
        uint8_t buffer[65536];auto n=recv(flow.source,reinterpret_cast<char*>(buffer),sizeof(buffer),0);
        if(n==SOCKET_ERROR)return WSAGetLastError()==WSAEWOULDBLOCK;if(!n)return false;
        flow.receive.insert(flow.receive.end(),buffer,buffer+n);size_t consumed=0;
        while(flow.receive.size()-consumed>=12) {
            const auto* p=flow.receive.data()+consumed;auto size=read32(p+8);if(size>max_frame_bytes)return false;
            if(flow.receive.size()-consumed<size+12)break;auto type=wire::Message(uint16_t(p[6])<<8|p[7]);
            bool drop=(type==wire::Message::World && ++counter_%5==0) || (type==wire::Message::Input && ++counter_%10==0);
            if(drop)++dropped_;
            else {
                auto jitter=int(random()%(2*jitter_+1))-int(jitter_);
                auto due=now_ms()+unsigned(std::max(0,int(delay_)+jitter));
                flow.pending.push_back({std::vector<uint8_t>(p,p+size+12),0,due});
            }
            consumed+=size+12;
        }
        if(consumed)flow.receive.erase(flow.receive.begin(),flow.receive.begin()+consumed);
        return flow.receive.size()<max_frame_bytes+65536 && flow.pending.size()<256;
    }
    bool send(Flow& flow,bool uplink) {
        if(uplink && now_ms()-began_>=450 && now_ms()-began_<700)return true;
        if(flow.pending.empty() || flow.pending.front().due>now_ms())return true;
        auto& packet=flow.pending.front();auto n=::send(flow.destination,reinterpret_cast<const char*>(packet.bytes.data()+packet.sent),int(packet.bytes.size()-packet.sent),0);
        if(n==SOCKET_ERROR)return WSAGetLastError()==WSAEWOULDBLOCK;if(!n)return false;
        packet.sent+=unsigned(n);if(packet.sent==packet.bytes.size())flow.pending.pop_front();return true;
    }
    void run() {
        SOCKET down=INVALID_SOCKET,up=INVALID_SOCKET;
        while(!stop_ && down==INVALID_SOCKET){down=accept(listener,nullptr,nullptr);if(down==INVALID_SOCKET)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        if(stop_)return;up=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(upstream_);
        if(connect(up,reinterpret_cast<sockaddr*>(&target),sizeof(target))){closesocket(down);closesocket(up);return;}
        nonblock(up);nonblock(down);Flow uplink{down,up,{},{}},downlink{up,down,{},{}};began_=now_ms();
        while(!stop_) {
            fd_set reads;FD_ZERO(&reads);FD_SET(up,&reads);FD_SET(down,&reads);timeval wait{0,1000};
            if(select(0,&reads,nullptr,nullptr,&wait)==SOCKET_ERROR)break;
            if(FD_ISSET(down,&reads) && !receive(uplink))break;
            if(FD_ISSET(up,&reads) && !receive(downlink))break;
            if(!send(uplink,true) || !send(downlink,false))break;
        }
        closesocket(up);closesocket(down);
    }
public:
    uint16_t port=0;
    Link(uint16_t upstream,unsigned round_trip,unsigned jitter):delay_(round_trip/2),jitter_(jitter),upstream_(upstream) {
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(listener,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0 && listen(listener,1)==0 && nonblock(listener),"Relay listen");
        int size=sizeof(local);getsockname(listener,reinterpret_cast<sockaddr*>(&local),&size);port=ntohs(local.sin_port);thread=std::thread([this]{run();});
    }
    ~Link(){stop_=true;if(thread.joinable())thread.join();closesocket(listener);}
    unsigned dropped()const{return dropped_;}
};
int main() {
    for(unsigned ping:{0u,30u,60u,100u,150u}) {
        AuthoritativeServer host(2);require(host.start(0,"123456",99),"Host start");
        Link link(host.status().port,ping,10);AuthoritativeClient client;
        require(client.start("127.0.0.1",link.port,"123456",99,"Delayed client"),"Client start");
        wait_for([&]{return client.status().connected;},"Delayed handshake");
        WorldState world;world.level="Network fixture";world.tick=1;
        EntityState a;a.id=1;a.kind=EntityKind::Player;a.owner=host.status().player;a.properties={{"position",std::array<float,3>{0,0,0}}};world.entities[1]=a;
        EntityState b=a;b.id=2;b.owner=client.status().player;world.entities[2]=b;
        std::string error;require(host.publish(world,error),error.c_str());require(host.attach_character(a.owner,1) && host.attach_character(b.owner,2) && host.ready_host(),"Character setup");
        wait_for([&]{return client.latest_world()->players[b.owner.slot].character==2;},"Initial delayed snapshot");client.ready(1);
        wait_for([&]{return host.take_controls()[b.owner.slot].ready;},"Delayed readiness");
        uint64_t maximum_host_call=0;auto begin=Clock::now();
        for(unsigned tick=2;tick<=101;++tick) {
            auto deadline=begin+std::chrono::milliseconds((tick-2)*16);
            std::this_thread::sleep_until(deadline);ControlInput input;input.pad.stick_y=220;input.yaw=1000;client.submit_input(input);
            auto before=now_ms();host.take_controls();world.tick=tick;world.entities[2].properties["position"]=std::array<float,3>{float(tick),0,0};require(host.publish(world,error),error.c_str());maximum_host_call=std::max(maximum_host_call,now_ms()-before);
        }
        wait_for([&]{return client.latest_world()->tick==101;},"Lost/delayed delta failed to recover latest world");
        require(std::get<std::array<float,3>>(client.latest_world()->entities.at(2).properties.at("position"))[0]==101.f,"Recovered state is incorrect");
        require(host.status().connected && maximum_host_call<50,"Network latency held host simulation");
        require(link.dropped()>0,"Loss path not exercised");
        client.set_afk(true);wait_for([&]{return client.latest_world()->players[b.owner.slot].afk;},"Delayed AFK command");
        auto scene=host.begin_cutscene();host.host_skip(1,scene);wait_for([&]{return client.latest_world()->vote.skip_requested;},"Vote result lost under delay");
        std::cout<<"RTT "<<ping<<" ms + jitter/frame loss/250-ms interruption: latest authority state recovered; max host call "<<maximum_host_call<<" ms; "<<link.dropped()<<" dropped application frames.\n";
        client.stop();host.stop();
    }
}
