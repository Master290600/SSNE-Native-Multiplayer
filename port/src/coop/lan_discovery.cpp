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
#include <deque>
#include <tuple>
#include <utility>
#include <map>
namespace port::coop {namespace {
using Bytes=std::vector<uint8_t>;using Clock=std::chrono::steady_clock;
constexpr std::array<uint8_t,8> magic{'S','S','N','E','D','I','S','C'};
constexpr std::array<uint8_t,4> lan_extension{'L','A','N','2'};
constexpr std::array<uint8_t,4> room_extension{'R','O','O','M'},ping_extension{'P','N','G','1'};
constexpr size_t packet_limit=512,details_limit=124;
bool display_text(const std::string& text,size_t limit){return text.size()<=limit && std::none_of(text.begin(),text.end(),[](unsigned char c){return c<32 || c==127;});}
bool map_identifier(const std::string& text){return text.size()<=48 && (text.empty() || (text.front()!='/' && text.find("//")==std::string::npos && std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='-' || c=='/';})));}
std::string folded(std::string text){for(auto& c:text)if(c>='A'&&c<='Z')c=char(c+'a'-'A');return text;}
bool network(){static bool ok=[](){WSADATA d{};return WSAStartup(MAKEWORD(2,2),&d)==0;}();return ok;}
void put(Bytes& out,uint64_t value,unsigned bytes){for(unsigned i=bytes;i-->0;)out.push_back(uint8_t(value>>(i*8)));}
uint64_t get(const uint8_t* p,unsigned bytes){uint64_t n=0;while(bytes--)n=n<<8|*p++;return n;}
Bytes request(uint64_t fingerprint,uint32_t nonce,uint32_t scan_nonce){
    Bytes out(magic.begin(),magic.end());put(out,wire::version,2);put(out,fingerprint,8);put(out,nonce,4);
    // Independent RTT probes share one IP-bound permit within this scan.
    out.insert(out.end(),ping_extension.begin(),ping_extension.end());put(out,scan_nonce,4);return out;
}
bool prefix(const uint8_t* bytes,int n,uint64_t fingerprint){return n>=22 && std::equal(magic.begin(),magic.end(),bytes) && get(bytes+8,2)==wire::version && get(bytes+10,8)==fingerprint;}
bool readable(SOCKET socket){fd_set f;FD_ZERO(&f);FD_SET(socket,&f);timeval wait{0,50000};return select(0,&f,nullptr,nullptr,&wait)>0;}
bool room_valid(const FrontendRoom& room){return room.port && room.port<=65535 && room.capacity && room.capacity<=8 && room.players && room.players<=room.capacity && !room.name.empty() && room.name.size()<=64 && std::none_of(room.name.begin(),room.name.end(),[](unsigned char c){return c<32 || c==127;});}
bool loopback_address(const std::string& address){in_addr value{};return inet_pton(AF_INET,address.c_str(),&value)==1 && (ntohl(value.s_addr)>>24)==127;}
void append_details(Bytes& answer,const FrontendRoom& room){
    if(!room.details_known)return;
    Bytes value{1,uint8_t(room.mode),uint8_t(room.difficulty),uint8_t(room.running),uint8_t(room.humans),uint8_t(room.bots),uint8_t(room.spectators),uint8_t(room.enemies),uint8_t(room.ammo_capacity),uint8_t(room.pickups),uint8_t(room.map.size())};
    value.insert(value.end(),room.map.begin(),room.map.end());value.push_back(uint8_t(room.map_name.size()));value.insert(value.end(),room.map_name.begin(),room.map_name.end());
    answer.insert(answer.end(),room_extension.begin(),room_extension.end());put(answer,value.size(),2);answer.insert(answer.end(),value.begin(),value.end());
}
bool parse_details(const uint8_t* bytes,size_t size,FrontendRoom& room){
    if(size<7 || !std::equal(room_extension.begin(),room_extension.end(),bytes))return false;
    const auto length=size_t(get(bytes+4,2));if(length<1 || length>details_limit || length+6!=size)return false;
    const auto* value=bytes+6;if(value[0]!=1)return true; // Future versions keep a usable legacy room.
    if(length<12 || (value[3]&~1u))return false;
    room.details_known=true;room.mode=value[1];room.difficulty=value[2];room.running=value[3]!=0;room.humans=value[4];room.bots=value[5];room.spectators=value[6];room.enemies=value[7];room.ammo_capacity=value[8];room.pickups=value[9];
    const size_t map_size=value[10],name_at=11+map_size;if(map_size>48 || name_at>=length)return false;
    const size_t name_size=value[name_at];if(name_size>64 || name_at+1+name_size!=length)return false;
    room.map.assign(reinterpret_cast<const char*>(value+11),map_size);room.map_name.assign(reinterpret_cast<const char*>(value+name_at+1),name_size);
    return valid_lan_room_details(room);
}
bool parse_room(const uint8_t* bytes,size_t size,FrontendRoom& room,uint64_t now){
    if(size<27)return false;const size_t base=27+bytes[26];if(base>size)return false;
    room.port=unsigned(get(bytes+22,2));room.players=bytes[24];room.capacity=bytes[25];room.name.assign(reinterpret_cast<const char*>(bytes+27),bytes[26]);if(!room_valid(room))return false;
    size_t at=base;
    if(size-at>=4 && std::equal(lan_extension.begin(),lan_extension.end(),bytes+at)){
        if(size-at<72)return false;
        room.session_id.assign(reinterpret_cast<const char*>(bytes+at+4),32);if(!valid_lan_secret(room.session_id))return false;
        const auto remaining=uint32_t(get(bytes+at+68,4));if(remaining>60000)return false;
        if(remaining){room.join_ticket.assign(reinterpret_cast<const char*>(bytes+at+36),32);if(!valid_lan_secret(room.join_ticket))return false;room.join_expires_ms=now+remaining;}
        at+=72;
    }
    return at==size || parse_details(bytes+at,size-at,room);
}
}
bool valid_lan_room_details(const FrontendRoom& room){
    return !room.details_known || (room_valid(room) && room.mode>=1 && room.mode<=5 && room.difficulty<=3 && room.humans<=room.players && room.bots<=room.players && room.humans+room.bots==room.players && room.spectators<=room.humans && room.enemies>=1 && room.enemies<=5 && room.ammo_capacity<=5 && room.pickups>=1 && room.pickups<=5 && map_identifier(room.map) && display_text(room.map_name,64));
}
bool lan_room_has_human_slot(const FrontendRoom& room){return room_valid(room) && (room.players<room.capacity || (room.details_known && valid_lan_room_details(room) && room.bots>0));}
bool lan_room_fresh(const FrontendRoom& room,uint64_t now){return room.seen_ms && room.seen_ms<=now && now-room.seen_ms<=lan_room_fresh_ms;}
LanRoomState lan_room_state(const FrontendRoom& room,uint64_t now){
    if(room.seen_ms && !lan_room_fresh(room,now))return LanRoomState::Stale;
    if(!room.details_known || !lan_room_fresh(room,now))return LanRoomState::Unknown;
    return room.running?LanRoomState::Running:LanRoomState::Lobby;
}
std::string lan_room_key(const FrontendRoom& room){return (room.session_id.empty()?room.address:room.session_id)+":"+std::to_string(room.port);}
std::optional<FrontendRoom> selected_lan_room(const FrontendRoom& displayed,const std::vector<FrontendRoom>& rooms,uint64_t now){
    const auto key=lan_room_key(displayed);
    const auto found=std::find_if(rooms.begin(),rooms.end(),[&](const auto& room){return lan_room_key(room)==key;});
    if(found==rooms.end() || !lan_room_fresh(*found,now))return {};
    if(!found->session_id.empty()){
        if(!valid_lan_secret(found->session_id))return {};
        // An explicitly absent automatic permit leaves the fresh room usable
        // through manual PIN entry. An issued but expired token stays invalid.
        if(found->join_ticket.empty()){if(found->join_expires_ms)return {};}
        else if(!valid_lan_secret(found->join_ticket) || found->join_expires_ms<=now)return {};
    }
    return *found;
}
std::vector<FrontendRoom> stable_lan_room_view(const std::vector<FrontendRoom>& previous,std::vector<FrontendRoom> fresh){
    std::map<std::string,FrontendRoom> available;for(auto& room:fresh)available.emplace(lan_room_key(room),room);
    std::vector<FrontendRoom> next;next.reserve(fresh.size());
    for(const auto& room:previous)if(auto it=available.find(lan_room_key(room));it!=available.end()){next.push_back(std::move(it->second));available.erase(it);}
    for(const auto& room:fresh)if(auto it=available.find(lan_room_key(room));it!=available.end()){next.push_back(std::move(it->second));available.erase(it);}
    return next;
}
std::vector<FrontendRoom> sorted_lan_rooms(std::vector<FrontendRoom> rooms,LanRoomSort sort,LanRoomFilter filter,uint64_t now){
    std::erase_if(rooms,[&](const auto& room){return (filter.mode && (!room.details_known || room.mode!=filter.mode)) || (filter.hide_full && (!lan_room_fresh(room,now) || !lan_room_has_human_slot(room))) || (filter.hide_running && room.details_known && room.running);});
    std::stable_sort(rooms.begin(),rooms.end(),[&](const auto& a,const auto& b){
        if(sort==LanRoomSort::Ping){const auto pa=lan_room_fresh(a,now)?a.ping_ms:UINT32_MAX,pb=lan_room_fresh(b,now)?b.ping_ms:UINT32_MAX;if(pa!=pb)return pa<pb;}
        if(sort==LanRoomSort::Players && a.players!=b.players)return a.players>b.players;
        if(sort==LanRoomSort::Mode){const auto ka=a.details_known?a.mode:6,kb=b.details_known?b.mode:6;if(ka!=kb)return ka<kb;}
        if(sort==LanRoomSort::Map && folded(a.map_name.empty()?a.map:a.map_name)!=folded(b.map_name.empty()?b.map:b.map_name))return folded(a.map_name.empty()?a.map:a.map_name)<folded(b.map_name.empty()?b.map:b.map_name);
        return std::tuple(folded(a.name),a.session_id,a.port,a.address)<std::tuple(folded(b.name),b.session_id,b.port,b.address);
    });return rooms;
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
    // Prefer a loopback endpoint on the same PC, while taking metadata from
    // the newest probe. Its source-bound permit and RTT always stay paired.
    const bool older=room.probe_ms && existing->probe_ms>room.probe_ms;
    // A delayed reply from the same endpoint cannot roll its permit or RTT
    // back after a newer probe. Across interfaces, keep loopback preference.
    const auto endpoint=existing->address==room.address?(older?*existing:room):
        loopback_address(existing->address) && !loopback_address(room.address)?*existing:room;
    auto snapshot=older?*existing:std::move(room);
    snapshot.address=endpoint.address;snapshot.port=endpoint.port;snapshot.session_id=endpoint.session_id;snapshot.join_ticket=endpoint.join_ticket;snapshot.join_expires_ms=endpoint.join_expires_ms;snapshot.ping_ms=endpoint.ping_ms;
    snapshot.seen_ms=std::max(snapshot.seen_ms,existing->seen_ms);*existing=std::move(snapshot);
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
        std::array<uint8_t,packet_limit> packet{};
        while(impl_->running){
            if(!readable(s))continue;sockaddr_in sender{};int size=sizeof(sender);const auto n=recvfrom(s,reinterpret_cast<char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&sender),&size);
            if((n!=22 && n!=30) || !prefix(packet.data(),n,fingerprint) || (n==30 && !std::equal(ping_extension.begin(),ping_extension.end(),packet.begin()+22)))continue;
            const auto room=provider();if(!room_valid(room) || !valid_lan_room_details(room))continue;
            Bytes answer(packet.begin(),packet.begin()+22);put(answer,room.port,2);answer.push_back(uint8_t(room.players));answer.push_back(uint8_t(room.capacity));answer.push_back(uint8_t(room.name.size()));answer.insert(answer.end(),room.name.begin(),room.name.end());
            if(valid_lan_secret(room.session_id)){
                char source[INET_ADDRSTRLEN]{};if(!inet_ntop(AF_INET,&sender.sin_addr,source,sizeof(source)))continue;
                const auto grant=issuer?issuer(source,uint32_t(get(packet.data()+(n==30?26:18),4))):LanJoinGrant{};
                // Discovery replies are unicast. Never reveal the six-digit PIN.
                answer.insert(answer.end(),lan_extension.begin(),lan_extension.end());answer.insert(answer.end(),room.session_id.begin(),room.session_id.end());
                const bool valid=valid_lan_secret(grant.ticket) && grant.valid_ms && grant.valid_ms<=60000;
                const std::string ticket=valid?grant.ticket:std::string(32,'0');answer.insert(answer.end(),ticket.begin(),ticket.end());put(answer,valid?grant.valid_ms:0,4);
            }
            append_details(answer,room);if(answer.size()>packet_limit)continue;
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
    const auto now=lan_clock_ms();
    if(!lan_room_fresh(room,now) || !valid_lan_secret(room.session_id) || !valid_lan_secret(room.join_ticket) || room.join_expires_ms<=now)return false;
    std::lock_guard lock(impl_->mutex);return std::any_of(impl_->rooms.begin(),impl_->rooms.end(),[&](const auto& current){return lan_room_fresh(current,now) && current.session_id==room.session_id && current.address==room.address && current.port==room.port && current.join_ticket==room.join_ticket && current.join_expires_ms>=room.join_expires_ms && lan_room_has_human_slot(current);});
}
bool LanBrowser::search(uint64_t fingerprint,uint16_t port,bool loopback){
    stop();{std::lock_guard lock(impl_->mutex);impl_->rooms.clear();}if(!network() || !port)return false;
    auto s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET)return false;
    BOOL yes=TRUE;setsockopt(s,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes));
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(loopback?INADDR_LOOPBACK:INADDR_ANY);
    if(bind(s,reinterpret_cast<sockaddr*>(&local),sizeof(local))){closesocket(s);return false;}
    impl_->running=true;impl_->searching=true;
    impl_->worker=std::thread([this,s,fingerprint,port,loopback](){
        const auto scan_nonce=uint32_t(Clock::now().time_since_epoch().count());uint32_t nonce=scan_nonce;
        struct Probe {uint32_t nonce;uint64_t sent;};std::deque<Probe> probes;
        const auto begin=Clock::now();auto next=begin;
        while(impl_->running && Clock::now()-begin<std::chrono::seconds(2)){
            if(Clock::now()>=next){
                const auto query=request(fingerprint,++nonce,scan_nonce);probes.push_back({nonce,lan_clock_ms()});if(probes.size()>4)probes.pop_front();
                for(uint32_t address:std::array<uint32_t,2>{uint32_t(INADDR_LOOPBACK),uint32_t(INADDR_BROADCAST)}){if(loopback && address!=INADDR_LOOPBACK)continue;sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(address);target.sin_port=htons(port);sendto(s,reinterpret_cast<const char*>(query.data()),int(query.size()),0,reinterpret_cast<sockaddr*>(&target),sizeof(target));}next=Clock::now()+std::chrono::milliseconds(500);
            }
            if(!readable(s))continue;std::array<uint8_t,packet_limit> packet{};sockaddr_in sender{};int size=sizeof(sender);const auto n=recvfrom(s,reinterpret_cast<char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&sender),&size);
            if(n<27 || !prefix(packet.data(),n,fingerprint))continue;
            const auto echoed=uint32_t(get(packet.data()+18,4));const auto probe=std::find_if(probes.begin(),probes.end(),[&](const auto& p){return p.nonce==echoed;});if(probe==probes.end())continue;
            const auto now=lan_clock_ms();FrontendRoom room;if(!parse_room(packet.data(),size_t(n),room,now))continue;
            room.seen_ms=now;room.probe_ms=probe->sent;room.ping_ms=uint32_t(std::min<uint64_t>(now-probe->sent,60000));
            char text[INET_ADDRSTRLEN]{};if(!inet_ntop(AF_INET,&sender.sin_addr,text,sizeof(text)))continue;room.address=text;
            std::lock_guard lock(impl_->mutex);merge_lan_room(impl_->rooms,std::move(room));
        }
        closesocket(s);impl_->searching=false;
    });return true;
}
}
