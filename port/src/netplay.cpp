#include "netplay.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
namespace port::netplay {
std::string local_ipv4_addresses() {
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname))) return "see ipconfig";
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result)) return "see ipconfig";
    std::vector<std::string> addresses;
    for (auto* current = result; current; current = current->ai_next) {
        auto* address = reinterpret_cast<sockaddr_in*>(current->ai_addr);
        char ip[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)) && strcmp(ip,"127.0.0.1") &&
            std::find(addresses.begin(), addresses.end(), ip) == addresses.end()) addresses.emplace_back(ip);
    }
    freeaddrinfo(result);
    std::string text;
    for (const auto& address : addresses) { if (!text.empty()) text += " / "; text += address; }
    return text.empty() ? "see ipconfig" : text;
}

namespace {
void put32(uint8_t* p, uint32_t v) { for(int i=3;i>=0;--i) { p[i]=uint8_t(v); v>>=8; } }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
void put64(uint8_t* p,uint64_t v) { put32(p,uint32_t(v>>32)); put32(p+4,uint32_t(v)); }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p))<<32|get32(p+4); }
uint64_t bytes_hash(const std::vector<uint8_t>& bytes) {
    uint64_t h=14695981039346656037ull;
    for(uint8_t b:bytes) { h^=b; h*=1099511628211ull; }
    return h;
}
bool card_size(size_t n) { return !n || (n>=0x80000 && n<=0x1000000 && !(n&(n-1))); }
bool transfer(SOCKET s, uint8_t* p, size_t n, bool writing) {
    while(n) { int k=writing?send(s,reinterpret_cast<char*>(p),int(n),0):recv(s,reinterpret_cast<char*>(p),int(n),0);
        if(k<=0) return false; p+=k; n-=size_t(k); } return true;
}
void pack(uint8_t* p, const Input& v) {
    p[0]=uint8_t(v.pad.buttons>>8); p[1]=uint8_t(v.pad.buttons);
    p[2]=v.pad.stick_x; p[3]=v.pad.stick_y; p[4]=v.pad.cstick_x; p[5]=v.pad.cstick_y;
    p[6]=v.pad.trigger_l; p[7]=v.pad.trigger_r; put32(p+8,uint32_t(v.yaw)); put32(p+12,uint32_t(v.pitch));
    put32(p+16,v.mods.flags); put32(p+20,v.mods.fire_tenths);
    put32(p+24,v.mods.spawn_sequence); put32(p+28,v.mods.spawn_id);
    put32(p+32,v.mods.spawn_count); put32(p+36,v.mods.distance_tenths);
    put32(p+40,v.view_x);put32(p+44,v.view_y);
}
bool unpack(const uint8_t* p, Input& v) {
    v.pad.buttons=uint16_t(uint16_t(p[0])<<8|p[1]); v.pad.stick_x=p[2]; v.pad.stick_y=p[3];
    v.pad.cstick_x=p[4]; v.pad.cstick_y=p[5]; v.pad.trigger_l=p[6]; v.pad.trigger_r=p[7];
    v.yaw=int32_t(get32(p+8)); v.pitch=int32_t(get32(p+12));
    v.mods={get32(p+16),get32(p+20),get32(p+24),get32(p+28),get32(p+32),get32(p+36)};
    v.view_x=get32(p+40);v.view_y=get32(p+44);
    return v.view_x>=1000000 && v.view_x<=5000000 && v.view_y>=1000000 && v.view_y<=3000000 && v.yaw>=-2000000 && v.yaw<=2000000 && v.pitch>=-2000000 && v.pitch<=2000000 &&
        v.mods.flags<=31 && v.mods.fire_tenths>=10 && v.mods.fire_tenths<=100 &&
        v.mods.spawn_count>=1 && v.mods.spawn_count<=10 && v.mods.distance_tenths>=30 && v.mods.distance_tenths<=300;
}
}
struct Session::Impl {
    mutable std::mutex mutex; std::condition_variable changed; std::thread setup;
    std::atomic<bool> stopping{false};
    std::array<SOCKET,max_players> active;
    SOCKET listener=INVALID_SOCKET;
    bool host=false, initialized=false, failed=false; Status state; Input local;
    std::vector<uint8_t> card;
    std::deque<std::array<uint32_t,3>> spawns;
    Impl() { active.fill(INVALID_SOCKET); }
    void fail(const std::string& s) { std::lock_guard lock(mutex); failed=true; state.connected=false; state.message=s; changed.notify_all(); }
    void say(const std::string& s) { std::lock_guard lock(mutex); state.message=s; changed.notify_all(); }
    bool adopt(SOCKET s, unsigned slot) {
        std::lock_guard lock(mutex);
        if(stopping) { closesocket(s); return false; }
        active[slot]=s; return true;
    }
    bool handshake(SOCKET s,const std::string& pin,uint64_t content,unsigned slot,unsigned count) {
        DWORD timeout=120000; BOOL yes=TRUE;
        setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&yes),sizeof(yes));
        std::array<uint8_t,24> hello{}; memcpy(hello.data(),"SRNP",4); put32(hello.data()+4,protocol_version);
        memcpy(hello.data()+8,pin.data(),6); put64(hello.data()+16,content); auto peer=hello;
        if(!transfer(s,hello.data(),hello.size(),true) || !transfer(s,peer.data(),peer.size(),false)) {
            fail("Connection closed during netplay startup."); return false;
        }
        if(memcmp(peer.data(),"SRNP",4)) { fail("Peer is not using native Netplay."); return false; }
        if(get32(peer.data()+4)!=protocol_version) {
            fail("Netplay protocol differs: this PC v"+std::to_string(protocol_version)+", peer v"+
                 std::to_string(get32(peer.data()+4))+". Update both installations."); return false;
        }
        if(memcmp(peer.data()+8,hello.data()+8,6)) { fail("PIN differs. Enter the six-digit host PIN."); return false; }
        if(get64(peer.data()+16)!=content) { fail("Game data differs; both PCs need the same game revision."); return false; }
        if(peer[14] || peer[15]) { fail("Unsupported netplay startup flags."); return false; }
        std::array<uint8_t,8> assignment{};
        if(host) {put32(assignment.data(),slot); put32(assignment.data()+4,count);}
        if(!transfer(s,assignment.data(),assignment.size(),host)) {fail("Player assignment interrupted."); return false;}
        if(!host) {
            slot=get32(assignment.data()); count=get32(assignment.data()+4);
            if(count<2 || count>max_players || !slot || slot>=count) {fail("Invalid player assignment.");return false;}
            std::lock_guard lock(mutex);state.local_player=slot;state.player_count=count;
        }
        std::array<uint8_t,12> header{};
        if(host) {put32(header.data(),uint32_t(card.size()));put64(header.data()+4,bytes_hash(card));}
        if(!transfer(s,header.data(),header.size(),host)) {fail("Save synchronization interrupted.");return false;}
        size_t size=get32(header.data());
        if(!card_size(size)) {fail("Invalid host memory card size.");return false;}
        if(!host) card.resize(size);
        if(!transfer(s,card.data(),size,host) || bytes_hash(card)!=get64(header.data()+4)) {fail("Host save integrity check failed.");return false;}
        std::array<uint8_t,8> ack{};put64(ack.data(),bytes_hash(card));auto expected=ack;
        if(!transfer(s,ack.data(),ack.size(),!host) || ack!=expected) {fail("Host save was not acknowledged.");return false;}
        return true;
    }
    void connect(bool hosting,std::string address,uint16_t port,std::string pin,uint64_t content,unsigned count) {
        if(hosting) {
            SOCKET l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            {std::lock_guard lock(mutex);if(stopping){closesocket(l);return;}listener=l;state.local_player=0;state.player_count=count;state.joined=1;}
            sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_ANY);
            if(l==INVALID_SOCKET || bind(l,reinterpret_cast<sockaddr*>(&a),sizeof(a)) || listen(l,max_players-1)) {fail("Cannot listen on this port.");return;}
            int length=sizeof(a);getsockname(l,reinterpret_cast<sockaddr*>(&a),&length);
            {std::lock_guard lock(mutex);state.port=ntohs(a.sin_port);}
            for(unsigned slot=1;slot<count;++slot) {
                say("Waiting for players: "+std::to_string(slot)+" / "+std::to_string(count));
                SOCKET s=accept(l,nullptr,nullptr);
                if(s==INVALID_SOCKET || !adopt(s,slot)) {fail("Lobby connection closed.");return;}
                if(!handshake(s,pin,content,slot,count)) return;
                {std::lock_guard lock(mutex);state.joined=slot+1;}
            }
            // Release every peer together only once the entire roster is fixed.
            uint8_t ready=1;
            for(unsigned slot=1;slot<count;++slot)
                if(!transfer(active[slot],&ready,1,true)) {fail("Player left before session start.");return;}
        } else {
            SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if(!adopt(s,0)) return;
            sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);
            if(s==INVALID_SOCKET || inet_pton(AF_INET,address.c_str(),&a.sin_addr)!=1 || ::connect(s,reinterpret_cast<sockaddr*>(&a),sizeof(a))) {fail("Connection failed.");return;}
            if(!handshake(s,pin,content,0,0)) return;
            say("Waiting for the remaining players...");
            uint8_t ready=0;
            if(!transfer(s,&ready,1,false) || ready!=1) {fail("Host closed the lobby.");return;}
        }
        {std::lock_guard lock(mutex);if(stopping)return;state.connected=true;state.joined=state.player_count;
         state.message="Connected; local simulation / own camera";changed.notify_all();}
    }
};
Session::Session():impl(std::make_unique<Impl>()) {}
Session::~Session() {stop();}
bool Session::start(bool host,const std::string& address,uint16_t port,const std::string& pin,uint64_t content,
                    const std::vector<uint8_t>& host_card,unsigned players) {
    if(players<2 || players>max_players || !card_size(host_card.size()) || (!host && !host_card.empty())) return false;
    if(impl->setup.joinable() || pin.size()!=6 || !std::all_of(pin.begin(),pin.end(),[](char c){return c>='0'&&c<='9';})) return false;
    WSADATA ws{};if(WSAStartup(MAKEWORD(2,2),&ws))return false;
    impl->initialized=true;impl->host=host;impl->stopping=false;
    {std::lock_guard lock(impl->mutex);impl->failed=false;impl->state={};impl->state.port=port;impl->local={};impl->spawns.clear();impl->card=host_card;}
    impl->say("Connecting...");impl->setup=std::thread([=,this]{impl->connect(host,address,port,pin,content,players);});return true;
}
bool Session::wait_ready() {
    std::unique_lock lock(impl->mutex);impl->changed.wait(lock,[&]{return impl->state.connected||impl->failed||impl->stopping;});
    return impl->state.connected&&!impl->stopping;
}
std::vector<uint8_t> Session::initial_card() const {std::lock_guard lock(impl->mutex);return impl->state.connected?impl->card:std::vector<uint8_t>{};}
void Session::submit_view(uint32_t horizontal,uint32_t vertical) {
    std::lock_guard lock(impl->mutex);
    impl->local.view_x=std::clamp(horizontal,1000000u,5000000u);
    impl->local.view_y=std::clamp(vertical,1000000u,3000000u);
}
void Session::submit_pad(const ssne::PadState& pad) {std::lock_guard lock(impl->mutex);impl->local.pad=pad;}
void Session::submit_mouse(double yaw,double pitch) {
    if(!std::isfinite(yaw)||!std::isfinite(pitch))return;
    std::lock_guard lock(impl->mutex);
    impl->local.yaw=int32_t(std::clamp(double(impl->local.yaw)+yaw*1000000.,-2000000.,2000000.));
    impl->local.pitch=int32_t(std::clamp(double(impl->local.pitch)+pitch*1000000.,-2000000.,2000000.));
}
void Session::submit_mods(uint32_t flags,uint32_t fire_tenths) {
    std::lock_guard lock(impl->mutex);if(!impl->host)return;
    impl->local.mods.flags=flags&31;impl->local.mods.fire_tenths=std::clamp(fire_tenths,10u,100u);
}
void Session::submit_spawn(uint32_t id,uint32_t count,uint32_t distance_tenths) {
    std::lock_guard lock(impl->mutex);if(!impl->host)return;
    if(impl->spawns.size()<8)impl->spawns.push_back({id,std::clamp(count,1u,10u),std::clamp(distance_tenths,30u,300u)});
}
ModControls Session::requested_mods() const {std::lock_guard lock(impl->mutex);return impl->local.mods;}
bool Session::advance(uint64_t tick,uint64_t checksum,std::array<Input,max_players>& inputs,bool world_ready,bool* wait_for_world) {
    std::array<SOCKET,max_players> sockets;Input own;unsigned count,mine;bool host;
    {std::unique_lock lock(impl->mutex);impl->changed.wait(lock,[&]{return impl->state.connected||impl->stopping||impl->failed;});
     if(impl->stopping||impl->failed)return false;
     if(impl->host&&!impl->spawns.empty()) {
         auto spawn=impl->spawns.front();impl->spawns.pop_front();auto& m=impl->local.mods;
         ++m.spawn_sequence;m.spawn_id=spawn[0];m.spawn_count=spawn[1];m.distance_tenths=spawn[2];
     }
     sockets=impl->active;own=impl->local;impl->local.yaw=impl->local.pitch=0;
     host=impl->host;count=impl->state.player_count;mine=impl->state.local_player;}
    inputs={};inputs[mine]=own;
    std::array<uint8_t,24+48*max_players> response{};
    bool ok=true;unsigned ready_mask=world_ready?(1u<<mine):0;std::string reason;
    if(host) {
        for(unsigned slot=1;slot<count;++slot) {
            std::array<uint8_t,68> request{};
            if(!transfer(sockets[slot],request.data(),request.size(),false)) {ok=false;reason="Player "+std::to_string(slot+1)+" disconnected or stalled.";break;}
            bool ready=get32(request.data()+64)==1;
            if(ready)ready_mask|=1u<<slot;
            if(get32(request.data()+64)>1 || get64(request.data())!=tick || !unpack(request.data()+16,inputs[slot])) {
                ok=false;reason="Invalid input/tick from player "+std::to_string(slot+1)+" at tick "+std::to_string(tick);break;
            }
            inputs[slot].mods={}; // shared controls belong exclusively to host
            if(world_ready&&ready&&get64(request.data()+8)!=checksum) {
                ok=false;reason="Simulation mismatch at tick "+std::to_string(tick)+" with player "+std::to_string(slot+1)+
                    " (host hash "+std::to_string(checksum)+", peer hash "+std::to_string(get64(request.data()+8))+"). Session stopped.";
            }
        }
        put64(response.data(),tick);put64(response.data()+8,checksum);put32(response.data()+16,ok?0:1);put32(response.data()+20,ready_mask);
        for(unsigned slot=0;slot<count;++slot)pack(response.data()+24+slot*48,inputs[slot]);
        // A bad tick terminates the whole roster, never just one pair.
        for(unsigned slot=1;slot<count;++slot)
            if(!transfer(sockets[slot],response.data(),response.size(),true)) {ok=false;if(reason.empty())reason="Player "+std::to_string(slot+1)+" disconnected.";}
    } else {
        std::array<uint8_t,68> request{};put64(request.data(),tick);put64(request.data()+8,checksum);pack(request.data()+16,own);put32(request.data()+64,world_ready?1:0);
        ok=transfer(sockets[0],request.data(),request.size(),true)&&transfer(sockets[0],response.data(),response.size(),false);
        if(ok) {
            ready_mask=get32(response.data()+20);
            if(get64(response.data())!=tick || get32(response.data()+16)!=0 || (ready_mask&~((1u<<count)-1))) {
                ok=false;reason="Host stopped the session at tick "+std::to_string(get64(response.data()))+" (input or simulation mismatch).";
            }
            for(unsigned slot=0;slot<count&&ok;++slot)ok=unpack(response.data()+24+slot*48,inputs[slot]);
        }
        if(!ok&&reason.empty())reason="Host disconnected or stalled; simulation stopped.";
    }
    if(wait_for_world)*wait_for_world=ready_mask!=0&&ready_mask!=((1u<<count)-1);
    {std::lock_guard lock(impl->mutex);if(ok)impl->state.tick=tick;else{impl->failed=true;impl->state.connected=false;impl->state.message=reason;impl->changed.notify_all();}}
    return ok;
}
Status Session::status() const {std::lock_guard lock(impl->mutex);return impl->state;}
void Session::stop() {
    impl->stopping=true;impl->changed.notify_all();
    {std::lock_guard lock(impl->mutex);
     for(auto& s:impl->active)if(s!=INVALID_SOCKET){shutdown(s,SD_BOTH);closesocket(s);s=INVALID_SOCKET;}
     if(impl->listener!=INVALID_SOCKET){closesocket(impl->listener);impl->listener=INVALID_SOCKET;}impl->state.connected=false;}
    if(impl->setup.joinable())impl->setup.join();if(impl->initialized){WSACleanup();impl->initialized=false;}
}

uint64_t content_hash(const std::string& path) {
    std::ifstream f(path,std::ios::binary); if(!f) return 0;
    uint64_t h=14695981039346656037ull; char buffer[8192];
    while(f) { f.read(buffer,sizeof(buffer)); for(std::streamsize i=0;i<f.gcount();++i) {h^=uint8_t(buffer[i]);h*=1099511628211ull;} } return h;
}
}
