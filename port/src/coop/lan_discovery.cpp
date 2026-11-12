#include "lan_discovery.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>
#include <algorithm>
#include <chrono>
namespace port::coop {namespace {
using Bytes=std::vector<uint8_t>;using Clock=std::chrono::steady_clock;
constexpr std::array<uint8_t,8> magic{'S','S','N','E','D','I','S','C'};
constexpr std::array<uint8_t,4> lan_extension{'L','A','N','2'};
bool network(){static bool ok=[](){WSADATA d{};return WSAStartup(MAKEWORD(2,2),&d)==0;}();return ok;}
void put(Bytes& out,uint64_t value,unsigned bytes){for(unsigned i=bytes;i-->0;)out.push_back(uint8_t(value>>(i*8)));}
uint64_t get(const uint8_t* p,unsigned bytes){uint64_t n=0;while(bytes--)n=n<<8|*p++;return n;}
Bytes request(uint64_t fingerprint,uint32_t nonce){Bytes out(magic.begin(),magic.end());put(out,wire::version,2);put(out,fingerprint,8);put(out,nonce,4);return out;}
bool prefix(const uint8_t* bytes,int n,uint64_t fingerprint){return n>=22 && std::equal(magic.begin(),magic.end(),bytes) && get(bytes+8,2)==wire::version && get(bytes+10,8)==fingerprint;}
bool readable(SOCKET socket){fd_set f;FD_ZERO(&f);FD_SET(socket,&f);timeval wait{0,50000};return select(0,&f,nullptr,nullptr,&wait)>0;}
bool room_valid(const FrontendRoom& room){return room.port && room.port<=65535 && room.capacity && room.capacity<=8 && room.players && room.players<=room.capacity && !room.name.empty() && room.name.size()<=64 && std::none_of(room.name.begin(),room.name.end(),[](unsigned char c){return c<32 || c==127;});}
bool loopback_address(const std::string& address){in_addr value{};return inet_pton(AF_INET,address.c_str(),&value)==1 && (ntohl(value.s_addr)>>24)==127;}
}
uint64_t lan_clock_ms(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
std::string new_lan_secret(){std::array<uint8_t,16> bytes{};if(BCryptGenRandom(nullptr,bytes.data(),ULONG(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return {};static constexpr char hex[]="0123456789abcdef";std::string value;value.reserve(32);for(auto b:bytes){value+=hex[b>>4];value+=hex[b&15];}return value;}
bool valid_lan_secret(const std::string& value){return value.size()==32 && value.find_first_not_of("0123456789abcdef")==std::string::npos;}
bool local_lan_peer(const std::string& address){
    if(!network())return false;in_addr peer{};if(inet_pton(AF_INET,address.c_str(),&peer)!=1)return false;
    const auto remote=ntohl(peer.s_addr);if((remote>>24)==127)return true;
    if(!remote || remote==0xFFFFFFFFu || (remote>>28)==14)return false;
    ULONG size=16384;Bytes storage(size);auto* adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    auto result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,adapters,&size);
    if(result==ERROR_BUFFER_OVERFLOW){storage.resize(size);adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,adapters,&size);}
    if(result!=NO_ERROR)return false;
    for(auto* adapter=adapters;adapter;adapter=adapter->Next)if(adapter->OperStatus==IfOperStatusUp)for(auto* local=adapter->FirstUnicastAddress;local;local=local->Next){
        if(!local->Address.lpSockaddr || local->Address.lpSockaddr->sa_family!=AF_INET || !local->OnLinkPrefixLength || local->OnLinkPrefixLength>32)continue;
        const auto here=ntohl(reinterpret_cast<sockaddr_in*>(local->Address.lpSockaddr)->sin_addr.s_addr);if((here>>24)==127)continue;
        const auto mask=UINT32_MAX<<(32-local->OnLinkPrefixLength);if((remote&mask)==(here&mask))return true;
    }
    return false;
}
void merge_lan_room(std::vector<FrontendRoom>& rooms,FrontendRoom room){
    const auto existing=std::find_if(rooms.begin(),rooms.end(),[&](const auto& r){return r.port==room.port && (valid_lan_secret(r.session_id) && valid_lan_secret(room.session_id)?r.session_id==room.session_id:r.address==room.address);});
    if(existing==rooms.end()){if(rooms.size()<16)rooms.push_back(std::move(room));return;}
    if(loopback_address(existing->address) && !loopback_address(room.address)){existing->name=std::move(room.name);existing->players=room.players;existing->capacity=room.capacity;return;}
    *existing=std::move(room);
}
struct LanAdvertisement::Impl {std::atomic<bool> running{false};std::thread worker;};
LanAdvertisement::LanAdvertisement():impl_(std::make_unique<Impl>()){}LanAdvertisement::~LanAdvertisement(){stop();}
void LanAdvertisement::stop(){impl_->running=false;if(impl_->worker.joinable())impl_->worker.join();}
bool LanAdvertisement::start(uint64_t fingerprint,std::function<FrontendRoom()> provider,uint16_t port,bool loopback,LanJoinIssuer issuer){
    stop();if(!network() || !provider || !port)return false;
    auto s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET)return false;
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(port);local.sin_addr.s_addr=htonl(loopback?INADDR_LOOPBACK:INADDR_ANY);
    if(bind(s,reinterpret_cast<sockaddr*>(&local),sizeof(local))){closesocket(s);return false;}
    impl_->running=true;impl_->worker=std::thread([this,s,fingerprint,provider=std::move(provider),issuer=std::move(issuer)](){
        std::array<uint8_t,256> packet{};
        while(impl_->running){
            if(!readable(s))continue;sockaddr_in sender{};int size=sizeof(sender);const auto n=recvfrom(s,reinterpret_cast<char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&sender),&size);
            if(n!=22 || !prefix(packet.data(),n,fingerprint))continue;
            const auto room=provider();if(!room_valid(room))continue;
            Bytes answer(packet.begin(),packet.begin()+22);put(answer,room.port,2);answer.push_back(uint8_t(room.players));answer.push_back(uint8_t(room.capacity));answer.push_back(uint8_t(room.name.size()));answer.insert(answer.end(),room.name.begin(),room.name.end());
            if(valid_lan_secret(room.session_id)){
                char source[INET_ADDRSTRLEN]{};if(!inet_ntop(AF_INET,&sender.sin_addr,source,sizeof(source)))continue;
                const auto grant=issuer?issuer(source,uint32_t(get(packet.data()+18,4))):LanJoinGrant{};
                // Discovery replies are unicast. Never reveal the six-digit PIN.
                answer.insert(answer.end(),lan_extension.begin(),lan_extension.end());answer.insert(answer.end(),room.session_id.begin(),room.session_id.end());
                const bool valid=valid_lan_secret(grant.ticket) && grant.valid_ms && grant.valid_ms<=60000;
                const std::string ticket=valid?grant.ticket:std::string(32,'0');answer.insert(answer.end(),ticket.begin(),ticket.end());put(answer,valid?grant.valid_ms:0,4);
            }
            sendto(s,reinterpret_cast<const char*>(answer.data()),int(answer.size()),0,reinterpret_cast<sockaddr*>(&sender),size);
        }
        closesocket(s);
    });return true;
}
struct LanBrowser::Impl {std::atomic<bool> running{false},searching{false};std::thread worker;mutable std::mutex mutex;std::vector<FrontendRoom> rooms;};
LanBrowser::LanBrowser():impl_(std::make_unique<Impl>()){}LanBrowser::~LanBrowser(){stop();}
void LanBrowser::stop(){impl_->running=false;if(impl_->worker.joinable())impl_->worker.join();impl_->searching=false;std::lock_guard lock(impl_->mutex);impl_->rooms.clear();}
bool LanBrowser::searching()const{return impl_->searching;}
std::vector<FrontendRoom> LanBrowser::rooms()const{std::lock_guard lock(impl_->mutex);return impl_->rooms;}
bool LanBrowser::can_join(const FrontendRoom& room)const{
    if(!valid_lan_secret(room.session_id) || !valid_lan_secret(room.join_ticket) || room.join_expires_ms<=lan_clock_ms())return false;
    std::lock_guard lock(impl_->mutex);return std::any_of(impl_->rooms.begin(),impl_->rooms.end(),[&](const auto& current){return current.session_id==room.session_id && current.address==room.address && current.port==room.port && current.join_ticket==room.join_ticket && current.join_expires_ms>=room.join_expires_ms;});
}
bool LanBrowser::search(uint64_t fingerprint,uint16_t port,bool loopback){
    stop();{std::lock_guard lock(impl_->mutex);impl_->rooms.clear();}if(!network() || !port)return false;
    auto s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET)return false;
    BOOL yes=TRUE;setsockopt(s,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes));
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(loopback?INADDR_LOOPBACK:INADDR_ANY);
    if(bind(s,reinterpret_cast<sockaddr*>(&local),sizeof(local))){closesocket(s);return false;}
    impl_->running=true;impl_->searching=true;
    impl_->worker=std::thread([this,s,fingerprint,port,loopback](){
        const auto nonce=uint32_t(Clock::now().time_since_epoch().count());const auto query=request(fingerprint,nonce);const auto begin=Clock::now();auto next=begin;
        while(impl_->running && Clock::now()-begin<std::chrono::seconds(2)){
            if(Clock::now()>=next){
                for(uint32_t address:std::array<uint32_t,2>{uint32_t(INADDR_LOOPBACK),uint32_t(INADDR_BROADCAST)}){if(loopback && address!=INADDR_LOOPBACK)continue;sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(address);target.sin_port=htons(port);sendto(s,reinterpret_cast<const char*>(query.data()),int(query.size()),0,reinterpret_cast<sockaddr*>(&target),sizeof(target));}next=Clock::now()+std::chrono::milliseconds(500);
            }
            if(!readable(s))continue;std::array<uint8_t,256> packet{};sockaddr_in sender{};int size=sizeof(sender);const auto n=recvfrom(s,reinterpret_cast<char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&sender),&size);
            if(n<27 || !prefix(packet.data(),n,fingerprint) || get(packet.data()+18,4)!=nonce)continue;
            const auto base=27+packet[26];if(n!=base && n!=base+72)continue;
            FrontendRoom room;room.port=unsigned(get(packet.data()+22,2));room.players=packet[24];room.capacity=packet[25];room.name.assign(reinterpret_cast<const char*>(packet.data()+27),packet[26]);if(!room_valid(room))continue;
            if(n==base+72){
                if(!std::equal(lan_extension.begin(),lan_extension.end(),packet.begin()+base))continue;
                room.session_id.assign(reinterpret_cast<const char*>(packet.data()+base+4),32);if(!valid_lan_secret(room.session_id))continue;
                const auto remaining=uint32_t(get(packet.data()+base+68,4));if(remaining>60000)continue;
                if(remaining){room.join_ticket.assign(reinterpret_cast<const char*>(packet.data()+base+36),32);if(!valid_lan_secret(room.join_ticket))continue;room.join_expires_ms=lan_clock_ms()+remaining;}
            }
            char text[INET_ADDRSTRLEN]{};if(!inet_ntop(AF_INET,&sender.sin_addr,text,sizeof(text)))continue;room.address=text;
            std::lock_guard lock(impl_->mutex);merge_lan_room(impl_->rooms,std::move(room));
        }
        closesocket(s);impl_->searching=false;
    });return true;
}
}
