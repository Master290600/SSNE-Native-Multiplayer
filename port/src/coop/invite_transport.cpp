#include "invite_transport.h"
#include "invite_code.h"
#include "invite_upnp.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <juice/juice.h>
extern "C" {
#include "ikcp.h"
}
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>

namespace port::coop::invite {
namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t chunk_size=16*1024, tcp_queue_limit=4*1024*1024;
constexpr size_t datagram_queue_limit=1024*1024, kcp_send_limit=2*1024*1024;
constexpr uint32_t kcp_conv=0x534e4501;
constexpr int datagram_mtu=1200;
bool winsock(){ static const bool ok=[] { WSADATA w{};return WSAStartup(MAKEWORD(2,2),&w)==0; }();return ok; }
void close_socket(SOCKET& s){if(s!=INVALID_SOCKET){closesocket(s);s=INVALID_SOCKET;}}
bool nonblocking(SOCKET s){u_long v=1;return ioctlsocket(s,FIONBIO,&v)==0;}
bool would_block(){const int e=WSAGetLastError();return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS||e==WSAEALREADY;}
uint32_t clock_ms(){return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
template<class T>T socket_export(const char* name){const auto address=GetProcAddress(GetModuleHandleW(L"ws2_32.dll"),name);T fn=nullptr;static_assert(sizeof(fn)==sizeof(address));std::memcpy(&fn,&address,sizeof(fn));return fn;}
bool resolve_numeric(std::string& host,const std::atomic<bool>& cancelled,unsigned timeout_ms,std::string& error){
    IN_ADDR v4{};IN6_ADDR v6{};if(InetPtonA(AF_INET,host.c_str(),&v4)==1||InetPtonA(AF_INET6,host.c_str(),&v6)==1)return true;
    if(host.empty()||host.size()>253||std::any_of(host.begin(),host.end(),[](unsigned char c){return c<33||c>126;})){error="Invalid STUN/TURN host name.";return false;}
    // Async GetAddrInfoEx cancellation requires Windows 8+. Resolve dynamically
    // to preserve the game's Win7 imports; numeric server addresses work there.
    const auto get=socket_export<decltype(&GetAddrInfoExW)>("GetAddrInfoExW");
    const auto free_result=socket_export<decltype(&FreeAddrInfoExW)>("FreeAddrInfoExW");
    const auto cancel=socket_export<int (WSAAPI*)(LPHANDLE)>("GetAddrInfoExCancel");
    const auto result=socket_export<int (WSAAPI*)(LPOVERLAPPED)>("GetAddrInfoExOverlappedResult");
    if(!get||!free_result||!cancel||!result){error="Internet invitations with STUN/TURN host names require Windows 8 or newer. Configure numeric server IPs on Windows 7.";return false;}
    std::wstring name(host.begin(),host.end());ADDRINFOEXW hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_DGRAM;hints.ai_protocol=IPPROTO_UDP;PADDRINFOEXW addresses=nullptr;
    OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!operation.hEvent){error="Cannot create the cancellable DNS event.";return false;}
    HANDLE lookup=nullptr;timeval timeout{};timeout.tv_sec=timeout_ms/1000;timeout.tv_usec=(timeout_ms%1000)*1000;
    int code=get(name.c_str(),nullptr,NS_DNS,nullptr,&hints,&addresses,&timeout,&operation,nullptr,&lookup);bool aborted=false;
    if(code==WSA_IO_PENDING){const auto deadline=Clock::now()+std::chrono::milliseconds(timeout_ms);while(WaitForSingleObject(operation.hEvent,25)==WAIT_TIMEOUT){if(cancelled.load()||Clock::now()>=deadline){cancel(&lookup);aborted=true;break;}}
        // The OVERLAPPED and result pointers must outlive cancellation completion.
        // Windows signals this event after GetAddrInfoExCancel completes the I/O.
        WaitForSingleObject(operation.hEvent,INFINITE);code=result(&operation);}
    std::string numeric;for(auto* p=addresses;p;p=p->ai_next){char text[INET6_ADDRSTRLEN]{};void* bytes=nullptr;if(p->ai_family==AF_INET&&p->ai_addrlen>=sizeof(sockaddr_in))bytes=&reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr;else if(p->ai_family==AF_INET6&&p->ai_addrlen>=sizeof(sockaddr_in6))bytes=&reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr;if(bytes&&InetNtopA(p->ai_family,bytes,text,sizeof(text))){numeric=text;if(p->ai_family==AF_INET)break;}}
    if(addresses)free_result(addresses);CloseHandle(operation.hEvent);
    if(aborted||cancelled.load()){error="STUN/TURN name resolution was cancelled or timed out.";return false;}if(code||numeric.empty()){error="Cannot resolve the configured STUN/TURN server.";return false;}host=std::move(numeric);return true;
}
bool id_valid(const std::string& s){return s.size()==32&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string random_id(){std::array<unsigned char,16> b{};if(BCryptGenRandom(nullptr,b.data(),static_cast<ULONG>(b.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)return {};static const char hex[]="0123456789abcdef";std::string s;for(auto v:b){s+=hex[v>>4];s+=hex[v&15];}return s;}
sockaddr_in loopback(uint16_t port){sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(port);return a;}
bool loopback_listener(SOCKET& s,uint16_t& port){s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==INVALID_SOCKET)return false;BOOL exclusive=TRUE;setsockopt(s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof(exclusive));auto a=loopback(0);int len=sizeof(a);if(bind(s,reinterpret_cast<sockaddr*>(&a),sizeof(a))||listen(s,1)||getsockname(s,reinterpret_cast<sockaddr*>(&a),&len)||!nonblocking(s)){close_socket(s);return false;}port=ntohs(a.sin_port);return true;}
void tcp_options(SOCKET s){BOOL on=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof(on));}
struct Bytes {
    std::deque<std::vector<char>> chunks;size_t offset=0,size=0;
    // Fixed-capacity blocks also bound allocator/deque metadata when a remote
    // peer legitimately (or maliciously) sends many one-byte DATA records.
    void append(const char* p,size_t n){size+=n;while(n){if(chunks.empty()||chunks.back().size()==chunk_size){chunks.emplace_back();chunks.back().reserve(chunk_size);}auto& tail=chunks.back();const size_t count=std::min(n,chunk_size-tail.size());tail.insert(tail.end(),p,p+count);p+=count;n-=count;}}
    void consume(size_t n){size-=n;offset+=n;if(offset==chunks.front().size()){chunks.pop_front();offset=0;}}
};
}

struct InviteTransport::Impl {
    struct Peer {
        Impl* owner=nullptr;std::string remote;juice_agent_t* ice=nullptr;ikcpcb* kcp=nullptr;
        std::atomic<juice_state_t> ice_state{JUICE_STATE_DISCONNECTED};std::atomic<bool> gathered{false},overflow{false};
        std::mutex input_mutex;std::deque<std::vector<char>> input;size_t input_bytes=0;
        SOCKET tcp=INVALID_SOCKET;bool tcp_connecting=false,usable=false,description_sent=false,dead=false;
        bool local_fin=false,remote_fin=false,send_shutdown=false;
        Route local_route=Route::Unknown,remote_route=Route::Unknown,announced_route=Route::Unknown;juice_state_t route_state=JUICE_STATE_DISCONNECTED;
        Clock::time_point born=Clock::now(),last_description{},completed{};std::string local_description,failure_message;
        Bytes to_tcp;std::vector<char> wire;
        std::unique_ptr<PortMapping> mapping;
        ~Peer(){if(ice)juice_destroy(ice);if(kcp)ikcp_release(kcp);close_socket(tcp);}
    };
    mutable std::mutex state_mutex;Status state;std::string pin_value;
    Config config;bool hosting=false;uint16_t host_port=0;uint64_t fingerprint=0;std::string own_id,topic;
    SOCKET listener=INVALID_SOCKET;std::unique_ptr<Signalling> signal;
    std::atomic<bool> stopping{true},network_stopping{true},signal_ready{false},publishing_failed{false};std::thread worker,signal_worker,cleanup_worker;Clock::time_point started{};
    std::mutex wake_mutex;std::condition_variable wake;
    std::mutex signal_mutex;std::deque<Signal> signals;bool signal_overflow=false;
    std::deque<std::pair<std::string,std::string>> rejected;std::string last_peer_error;
    std::mutex outbound_mutex;std::condition_variable outbound_wake,outbound_done;std::deque<Signal> outbound;bool outbound_active=false;
    std::mutex cleanup_mutex;std::condition_variable cleanup_wake;std::deque<std::unique_ptr<PortMapping>> cleanup;bool cleanup_done=false;
    std::vector<std::unique_ptr<Peer>> peers;

    void set_state(Phase p,const std::string& message){std::lock_guard lock(state_mutex);state.phase=p;state.message=message;}
    void fail(const std::string& message){if(!stopping.load()){std::lock_guard lock(state_mutex);state.phase=Phase::Failed;state.message=message;state.network_ready=false;}}
    static void ice_state_cb(juice_agent_t*,juice_state_t s,void* u){auto& p=*static_cast<Peer*>(u);p.ice_state.store(s);p.owner->wake.notify_one();}
    static void ice_gather_cb(juice_agent_t*,void* u){auto& p=*static_cast<Peer*>(u);p.gathered.store(true);p.owner->wake.notify_one();}
    static void ice_receive_cb(juice_agent_t*,const char* data,size_t size,void* u){
        auto& p=*static_cast<Peer*>(u);if(p.owner->network_stopping.load())return;
        // Untrusted UDP never grows an unbounded queue. ICE authenticated the
        // selected candidate; KCP validates its own packet/sequence fields.
        if(size<24||size>datagram_mtu){p.overflow.store(true);p.owner->wake.notify_one();return;}
        {std::lock_guard lock(p.input_mutex);if(p.input_bytes+size>datagram_queue_limit||p.input.size()>=2048)p.overflow.store(true);else{p.input.emplace_back(data,data+size);p.input_bytes+=size;}}
        p.owner->wake.notify_one();
    }
    static int kcp_output(const char* data,int len,ikcpcb*,void* u){auto& p=*static_cast<Peer*>(u);if(!p.ice||p.dead)return -1;const int r=juice_send(p.ice,data,static_cast<size_t>(len));return r==JUICE_ERR_SUCCESS||r==JUICE_ERR_AGAIN?0:-1;}
    void enqueue(Signal s){if(network_stopping.load()||!id_valid(s.sender)||s.sender==own_id||s.fingerprint!=fingerprint||s.description.size()>8192)return;{std::lock_guard lock(signal_mutex);auto same=std::find_if(signals.begin(),signals.end(),[&](const auto& p){return p.sender==s.sender&&p.kind==s.kind&&p.target==s.target;});if(same!=signals.end())*same=std::move(s);else if(signals.size()>=64)signal_overflow=true;else signals.push_back(std::move(s));}wake.notify_one();}
    bool publish(Signal s){if(stopping.load())return false;s.sender=own_id;s.fingerprint=fingerprint;{std::lock_guard lock(outbound_mutex);auto same=std::find_if(outbound.begin(),outbound.end(),[&](const auto& p){return p.kind==s.kind&&p.target==s.target;});if(same!=outbound.end())*same=std::move(s);else if(outbound.size()>=64){last_peer_error="Invitation signalling is busy; the new connection was deferred.";return false;}else outbound.push_back(std::move(s));}outbound_wake.notify_one();return true;}
    void signal_run(){
        std::string error;if(!signal->start(config.signal_url,topic,[this](Signal s){enqueue(std::move(s));},error)){fail("Invitation signalling: "+error);return;}
        signal_ready.store(true);wake.notify_one();
        while(!stopping.load()){Signal next;{std::unique_lock lock(outbound_mutex);outbound_wake.wait(lock,[&]{return stopping.load()||!outbound.empty();});if(stopping.load())break;next=std::move(outbound.front());outbound.pop_front();outbound_active=true;}
            // Failed HTTPS signalling must not interrupt established P2P data.
            // The receive service reconnects itself; SDP retries cover the gap.
            if(signal->connected()){const bool sent=signal->publish(next,error);publishing_failed.store(!sent);if(!sent&&(error.rfind("signal:oversize:",0)==0||error.rfind("signal:invalid:",0)==0)){std::lock_guard lock(signal_mutex);if(rejected.size()<64)rejected.emplace_back(next.target,"Invitation signalling: "+error.substr(error.find(':',7)+1));else signal_overflow=true;wake.notify_one();}}
            {std::lock_guard lock(outbound_mutex);outbound_active=false;}outbound_done.notify_all();}
    }
    void cleanup_run(){for(;;){std::unique_ptr<PortMapping> next;{std::unique_lock lock(cleanup_mutex);cleanup_wake.wait(lock,[&]{return cleanup_done||!cleanup.empty();});if(cleanup.empty()&&cleanup_done)return;next=std::move(cleanup.front());cleanup.pop_front();}next->stop();}}
    void retire_mapping(Peer& p){if(!p.mapping)return;p.mapping->request_stop();{std::lock_guard lock(cleanup_mutex);cleanup.push_back(std::move(p.mapping));}cleanup_wake.notify_one();}
    Peer* make_peer(const std::string& remote,const std::string& description){
        auto peer=std::make_unique<Peer>();auto* p=peer.get();p->owner=this;p->remote=remote;
        juice_config_t c{};c.concurrency_mode=JUICE_CONCURRENCY_MODE_POLL;
        c.stun_server_host=config.testing_loopback_candidates?nullptr:(config.stun_host.empty()?nullptr:config.stun_host.c_str());c.stun_server_port=config.stun_port;
        std::vector<juice_turn_server_t> turns;for(const auto& r:config.relays)turns.push_back({r.host.c_str(),r.username.c_str(),r.password.c_str(),r.port});
        c.turn_servers=turns.data();c.turn_servers_count=static_cast<int>(turns.size());
        c.bind_address=config.testing_loopback_candidates?"127.0.0.1":(config.bind_address.empty()?nullptr:config.bind_address.c_str());
        c.cb_state_changed=ice_state_cb;c.cb_gathering_done=ice_gather_cb;c.cb_recv=ice_receive_cb;c.user_ptr=p;
        p->ice=juice_create(&c);if(!p->ice)return nullptr;
        p->kcp=ikcp_create(kcp_conv,p);if(!p->kcp)return nullptr;
        p->kcp->output=kcp_output;p->kcp->stream=1;ikcp_setmtu(p->kcp,datagram_mtu);ikcp_wndsize(p->kcp,256,256);ikcp_nodelay(p->kcp,1,10,2,0);
        if(!description.empty()&&(juice_set_remote_description(p->ice,description.c_str())!=0||juice_set_remote_gathering_done(p->ice)!=0))return nullptr;
        if(juice_gather_candidates(p->ice)!=0)return nullptr;
        peers.push_back(std::move(peer));return p;
    }
    void maybe_mapping(Peer& p){
        if(!config.automatic_mapping||config.testing_loopback_candidates||p.mapping)return;
        {std::lock_guard lock(cleanup_mutex);if(cleanup.size()>=16)return;}
        // libjuice owns and binds the UDP socket. Use its actual host candidate,
        // never guess a game/listener port. Mapping remains an optional aid to
        // STUN/ICE: only libjuice advertises and authenticates candidates.
        char sdp[JUICE_MAX_SDP_STRING_LEN]{};if(juice_get_local_description(p.ice,sdp,sizeof(sdp)))return;
        std::istringstream lines(sdp);std::string line;
        while(std::getline(lines,line)){if(line.find("candidate:")==std::string::npos)continue;std::istringstream in(line);std::string foundation,transport,ip,typ,type;unsigned component=0,priority=0,port=0;
            if(!(in>>foundation>>component>>transport>>priority>>ip>>port>>typ>>type)||type!="host"||transport!="UDP"||port==0||port>65535)continue;
            IN_ADDR addr{};if(InetPtonA(AF_INET,ip.c_str(),&addr)!=1||ip=="127.0.0.1"||ip=="0.0.0.0")continue;
            p.mapping=std::make_unique<PortMapping>();p.mapping->start(ip,static_cast<uint16_t>(port));break;
        }
    }
    void receive_signals(){
        std::deque<Signal> work;std::deque<std::pair<std::string,std::string>> failures;{std::lock_guard lock(signal_mutex);work.swap(signals);failures.swap(rejected);if(signal_overflow){last_peer_error="Excess invitation signalling was dropped; existing peers remain connected.";signal_overflow=false;}}
        for(auto& failure:failures)for(auto& p:peers)if(!p->usable&&((hosting&&p->remote==failure.first)||!hosting)){p->dead=true;p->failure_message=failure.second;last_peer_error=failure.second;}
        for(auto& s:work){
            if(!id_valid(s.sender)||s.sender==own_id||s.fingerprint!=fingerprint||s.description.size()>8192)continue;
            if(s.kind==Signal::Cancel){if(!s.description.empty())continue;for(auto& p:peers)if(p->remote==s.sender&&(s.target==own_id||(hosting&&s.target.empty()&&!p->usable)))p->dead=true;continue;}
            if(hosting){
                if(s.kind!=Signal::Offer||!s.target.empty()||s.description.empty())continue;
                auto existing=std::find_if(peers.begin(),peers.end(),[&](const auto& p){return p->remote==s.sender;});
                if(existing!=peers.end()){auto& p=**existing;if(p.description_sent&&Clock::now()-p.last_description>std::chrono::seconds(1)){Signal answer;answer.kind=Signal::Answer;answer.target=p.remote;answer.description=p.local_description;publish(std::move(answer));p.last_description=Clock::now();}continue;}
                if(peers.size()>=7)continue;
                if(!make_peer(s.sender,s.description))continue;
            }else{
                if(s.kind!=Signal::Answer||s.target!=own_id||s.description.empty()||peers.empty())continue;
                auto& p=*peers.front();if(!p.remote.empty()&&p.remote!=s.sender)continue;
                if(p.remote.empty()){if(juice_set_remote_description(p.ice,s.description.c_str())!=0||juice_set_remote_gathering_done(p.ice)!=0){p.dead=true;continue;}p.remote=s.sender;}
            }
        }
    }
    bool connect_host_tcp(Peer& p){
        p.tcp=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(p.tcp==INVALID_SOCKET||!nonblocking(p.tcp))return false;tcp_options(p.tcp);auto a=loopback(host_port);
        const int r=connect(p.tcp,reinterpret_cast<sockaddr*>(&a),sizeof(a));if(r==0){p.tcp_connecting=false;return true;}if(would_block()){p.tcp_connecting=true;return true;}return false;
    }
    bool send_frame(Peer& p,unsigned char type,const char* data,size_t size){std::array<char,chunk_size+5> frame{};frame[0]=static_cast<char>(type);for(unsigned i=0;i<4;++i)frame[i+1]=static_cast<char>((static_cast<uint32_t>(size)>>(24-i*8))&255);if(size)std::memcpy(frame.data()+5,data,size);return ikcp_send(p.kcp,frame.data(),static_cast<int>(size+5))>=0;}
    bool drain_kcp(Peer& p){
        std::array<char,chunk_size> data{};
        while(p.to_tcp.size<=tcp_queue_limit-chunk_size*2){const int n=ikcp_recv(p.kcp,data.data(),static_cast<int>(data.size()));if(n<0){if(n!=-1&&n!=-2)return false;break;}p.wire.insert(p.wire.end(),data.data(),data.data()+n);
            size_t consumed=0;while(p.wire.size()-consumed>=5){const auto* h=reinterpret_cast<const unsigned char*>(p.wire.data()+consumed);uint32_t size=0;for(unsigned i=1;i<=4;++i)size=(size<<8)|h[i];if(size>chunk_size||(h[0]<1||h[0]>4)||((h[0]==2||h[0]==3)&&size)||(h[0]==4&&size!=1))return false;if(p.wire.size()-consumed<5+size)break;if(h[0]==3)return false;
                if(p.remote_fin)return false;if(h[0]==1)p.to_tcp.append(reinterpret_cast<const char*>(h+5),size);else if(h[0]==2)p.remote_fin=true;else {if(h[5]!=1&&h[5]!=2)return false;p.remote_route=h[5]==2?Route::Relay:Route::Direct;}consumed+=5+size;}
            if(consumed)p.wire.erase(p.wire.begin(),p.wire.begin()+static_cast<ptrdiff_t>(consumed));if(p.wire.size()>chunk_size+5)return false;
        }return true;
    }
    bool pump_tcp(Peer& p){
        if(p.tcp==INVALID_SOCKET)return true;
        fd_set reads,writes,errors;FD_ZERO(&reads);FD_ZERO(&writes);FD_ZERO(&errors);
        if(!p.local_fin&&!p.tcp_connecting&&static_cast<size_t>(ikcp_waitsnd(p.kcp))*datagram_mtu<kcp_send_limit)FD_SET(p.tcp,&reads);
        if(p.tcp_connecting||p.to_tcp.size)FD_SET(p.tcp,&writes);FD_SET(p.tcp,&errors);timeval zero{};if(select(0,&reads,&writes,&errors,&zero)==SOCKET_ERROR||FD_ISSET(p.tcp,&errors))return false;
        if(p.tcp_connecting&&FD_ISSET(p.tcp,&writes)){int err=0,len=sizeof(err);if(getsockopt(p.tcp,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&err),&len)||err)return false;p.tcp_connecting=false;}
        if(!p.tcp_connecting&&FD_ISSET(p.tcp,&writes)&&p.to_tcp.size){auto& first=p.to_tcp.chunks.front();const int n=send(p.tcp,first.data()+p.to_tcp.offset,static_cast<int>(first.size()-p.to_tcp.offset),0);if(n>0)p.to_tcp.consume(static_cast<size_t>(n));else if(n==0||!would_block())return false;}
        if(!p.tcp_connecting&&FD_ISSET(p.tcp,&reads)){std::array<char,chunk_size> bytes{};const int n=recv(p.tcp,bytes.data(),static_cast<int>(bytes.size()),0);if(n>0){if(!send_frame(p,1,bytes.data(),static_cast<size_t>(n)))return false;}else if(n==0){p.local_fin=true;if(!send_frame(p,2,nullptr,0))return false;}else if(!would_block())return false;}
        if(p.remote_fin&&!p.to_tcp.size&&!p.send_shutdown&&!p.tcp_connecting){shutdown(p.tcp,SD_SEND);p.send_shutdown=true;}
        // Allow the final received FIN's ACK to leave, and retain the agent for
        // retransmitted FINs. Destroying it before ikcp_update loses that ACK.
        if(p.local_fin&&p.remote_fin&&!p.to_tcp.size&&ikcp_waitsnd(p.kcp)==0){if(p.completed==Clock::time_point{})p.completed=Clock::now();else if(Clock::now()-p.completed>std::chrono::milliseconds(500))return false;}
        return true;
    }
    void tick_peer(Peer& p){
        if(p.dead||p.overflow.load()){p.dead=true;return;}
        const auto ice=p.ice_state.load();if(ice==JUICE_STATE_FAILED){p.dead=true;return;}
        if(!p.usable&&Clock::now()-p.born>std::chrono::milliseconds(config.connect_timeout_ms)){p.dead=true;return;}
        if(p.gathered.load()&&(!p.description_sent||(!hosting&&p.remote.empty()&&Clock::now()-p.last_description>std::chrono::seconds(2)))){char sdp[JUICE_MAX_SDP_STRING_LEN]{};if(juice_get_local_description(p.ice,sdp,sizeof(sdp))){p.dead=true;return;}p.local_description=sdp;Signal s;s.kind=hosting?Signal::Answer:Signal::Offer;s.target=hosting?p.remote:std::string{};s.description=p.local_description;if(!publish(std::move(s))){p.dead=true;return;}p.description_sent=true;p.last_description=Clock::now();}
        maybe_mapping(p);
        if(!p.usable&&(ice==JUICE_STATE_CONNECTED||ice==JUICE_STATE_COMPLETED)){p.usable=true;if(hosting&&!connect_host_tcp(p)){p.dead=true;return;}}
        if(p.usable&&(p.local_route==Route::Unknown||p.route_state!=ice)){char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN]{},remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN]{};
            if(juice_get_selected_candidates(p.ice,local,sizeof(local),remote,sizeof(remote))==0){p.local_route=std::string(local).find("typ relay")!=std::string::npos||std::string(remote).find("typ relay")!=std::string::npos?Route::Relay:Route::Direct;p.route_state=ice;}}
        // ICE can select an asymmetric path: only the other endpoint may send
        // through TURN. Exchange the actual local selection as bounded KCP
        // control metadata, so the connection's route accounts for both ways.
        if(p.usable&&!p.local_fin&&p.local_route!=Route::Unknown&&p.announced_route!=p.local_route){const char route=p.local_route==Route::Relay?2:1;if(!send_frame(p,4,&route,1)){p.dead=true;return;}p.announced_route=p.local_route;}
        std::deque<std::vector<char>> incoming;{std::lock_guard lock(p.input_mutex);incoming.swap(p.input);}
        for(auto& d:incoming){const int result=ikcp_input(p.kcp,d.data(),static_cast<long>(d.size()));{std::lock_guard lock(p.input_mutex);p.input_bytes-=d.size();}if(result<0){p.dead=true;return;}}
        if(p.usable){if(!drain_kcp(p)||!pump_tcp(p)){p.dead=true;return;}ikcp_update(p.kcp,clock_ms());if(p.kcp->state==static_cast<IUINT32>(-1))p.dead=true;}
    }
    void update_status(){
        unsigned count=0;Route route=Route::Unknown;for(const auto& p:peers)if(p->usable&&!p->dead){++count;const auto combined=p->local_route==Route::Relay||p->remote_route==Route::Relay?Route::Relay:p->local_route;if(route==Route::Unknown||combined==Route::Relay)route=combined;}
        const bool healthy=signal->connected()&&!publishing_failed.load();std::lock_guard lock(state_mutex);state.peers=count;state.network_ready=count!=0;state.route=route;if(state.phase!=Phase::Failed){state.phase=count?Phase::Connected:(hosting?(healthy?Phase::Ready:Phase::Preparing):Phase::Connecting);state.message=!last_peer_error.empty()?last_peer_error:(!healthy?"Invitation signalling is reconnecting; existing peers remain connected.":(count?"Invitation transport connected.":(hosting?"Invitation is ready.":"Connecting invitation...")));}
    }
    void accept_local(){
        if(hosting||listener==INVALID_SOCKET||peers.empty()||!peers.front()->usable)return;auto& p=*peers.front();if(p.tcp!=INVALID_SOCKET)return;
        auto socket=accept(listener,nullptr,nullptr);if(socket==INVALID_SOCKET)return;if(!nonblocking(socket)){closesocket(socket);return;}tcp_options(socket);p.tcp=socket;close_socket(listener);
    }
    void run(){
        while(!network_stopping.load()&&!signal_ready.load()){ {std::lock_guard lock(state_mutex);if(state.phase==Phase::Failed)return;}if(!hosting&&Clock::now()-started>std::chrono::milliseconds(config.connect_timeout_ms)){fail("Invitation signalling timed out.");return;}std::unique_lock lock(wake_mutex);wake.wait_for(lock,std::chrono::milliseconds(10));}
        if(network_stopping.load())return;
        std::string dns_error;const bool had_servers=(!config.testing_loopback_candidates&&!config.stun_host.empty())||!config.relays.empty();
        if(!config.testing_loopback_candidates&&!config.stun_host.empty()&&!resolve_numeric(config.stun_host,network_stopping,5000,dns_error)){if(network_stopping.load())return;config.stun_host.clear();}
        std::vector<Relay> resolved_relays;for(auto relay:config.relays){std::string error;if(resolve_numeric(relay.host,network_stopping,5000,error))resolved_relays.push_back(std::move(relay));else {if(network_stopping.load())return;dns_error=std::move(error);}}config.relays=std::move(resolved_relays);
        if(had_servers&&config.stun_host.empty()&&config.relays.empty()&&!dns_error.empty()){fail(dns_error);return;}
        if(!dns_error.empty())last_peer_error="An unavailable STUN/TURN server was skipped; remaining servers are in use.";
        if(network_stopping.load())return;
        if(!hosting){auto* p=make_peer({},{});if(!p){fail("Unable to create the ICE connection.");return;}p->born=started;}
        set_state(hosting?Phase::Ready:Phase::Connecting,hosting?"Invitation is ready.":"Connecting invitation...");
        while(!network_stopping.load()){
            receive_signals();{std::lock_guard lock(state_mutex);if(state.phase==Phase::Failed)break;}
            for(auto& p:peers)tick_peer(*p);
            accept_local();
            if(!hosting&&!peers.empty()&&peers.front()->dead){fail(peers.front()->failure_message.empty()?"Invitation connection closed or timed out.":peers.front()->failure_message);break;}
            for(auto& p:peers)if(p->dead)retire_mapping(*p);peers.erase(std::remove_if(peers.begin(),peers.end(),[](const auto& p){return p->dead;}),peers.end());
            update_status();std::unique_lock lock(wake_mutex);wake.wait_for(lock,std::chrono::milliseconds(10),[this]{return network_stopping.load();});
        }
    }
    bool start(bool host,uint16_t port,const std::string& code_or_pin,uint64_t fp,const Config& cfg,std::string& error){
        error.clear();if(!winsock()){error="Windows sockets are unavailable.";return false;}
        if(!cfg.connect_timeout_ms||cfg.connect_timeout_ms>300000||cfg.relays.size()>8){error="Invalid invitation transport configuration.";return false;}
        if(!cfg.bind_address.empty()){IN_ADDR v4{};IN6_ADDR v6{};if(InetPtonA(AF_INET,cfg.bind_address.c_str(),&v4)!=1&&InetPtonA(AF_INET6,cfg.bind_address.c_str(),&v6)!=1){error="The ICE bind address must be a numeric local IP address.";return false;}}
        auto invitation=host?make_compact_invitation(code_or_pin,error):decode_invitation(code_or_pin,error);if(!invitation)return false;
        if(host&&!port){error="The game TCP listener port is missing.";return false;}
        own_id=random_id();if(own_id.empty()){error="Secure invitation identity generation failed.";return false;}
        config=cfg;hosting=host;host_port=port;fingerprint=fp;topic=invitation_topic(*invitation);pin_value=invitation->pin;
        uint16_t local=port;if(!host&&!loopback_listener(listener,local)){error="Unable to open the private loopback listener.";return false;}
        signal=config.signalling_factory?config.signalling_factory():make_signalling();if(!signal){close_socket(listener);error="The invitation signalling factory returned no service.";return false;}
        {std::lock_guard lock(state_mutex);state={};state.phase=Phase::Preparing;state.code=encode_invitation(*invitation);state.local_port=local;state.message="Preparing invitation...";}
        started=Clock::now();stopping.store(false);network_stopping.store(false);signal_ready.store(false);publishing_failed.store(false);cleanup_done=false;try{cleanup_worker=std::thread([this]{cleanup_run();});signal_worker=std::thread([this]{try{signal_run();}catch(...){fail("Invitation signalling worker failed.");}});worker=std::thread([this]{try{run();}catch(const std::exception& e){fail(std::string("Invitation transport: ")+e.what());}catch(...){fail("Invitation transport failed.");}});}catch(...){stop();error="Unable to start the invitation worker.";return false;}return true;
    }
    void stop(){
        network_stopping.store(true);wake.notify_all();if(worker.joinable())worker.join();
        // Withdraw pending offers on the existing signalling owner. A stopped
        // receive service or slow/offline POST never extends this 120ms budget;
        // the host's finite pending-ICE timeout is the fallback in that case.
        if(!stopping.load()&&signal&&signal_ready.load()&&signal->connected()){
            {std::lock_guard lock(outbound_mutex);outbound.clear();auto cancel=[&](const std::string& remote){Signal s;s.kind=Signal::Cancel;s.sender=own_id;s.target=remote;s.fingerprint=fingerprint;outbound.push_back(std::move(s));};if(hosting){for(auto& p:peers)cancel(p->remote);}else cancel(peers.empty()?std::string{}:peers.front()->remote);}
            outbound_wake.notify_one();std::unique_lock lock(outbound_mutex);outbound_done.wait_for(lock,std::chrono::milliseconds(120),[&]{return outbound.empty()&&!outbound_active;});
        }
        stopping.store(true);outbound_wake.notify_all();if(signal)signal->stop();if(signal_worker.joinable())signal_worker.join();
        // Best-effort ordered close uses the same KCP stream, not a second
        // reliability protocol. Cancellation remains bounded even if offline.
        for(auto& p:peers){if(p->usable&&!p->dead){send_frame(*p,3,nullptr,0);ikcp_flush(p->kcp);}retire_mapping(*p);}peers.clear();close_socket(listener);signal.reset();
        {std::lock_guard lock(cleanup_mutex);cleanup_done=true;}cleanup_wake.notify_all();if(cleanup_worker.joinable())cleanup_worker.join();
        {std::lock_guard lock(signal_mutex);signals.clear();rejected.clear();signal_overflow=false;}last_peer_error.clear();
        {std::lock_guard lock(outbound_mutex);outbound.clear();outbound_active=false;}
        std::lock_guard lock(state_mutex);state={};pin_value.clear();
    }
    ~Impl(){stop();}
};

InviteTransport::InviteTransport():impl_(std::make_unique<Impl>()){}
InviteTransport::~InviteTransport()=default;
bool InviteTransport::host(uint16_t port,const std::string& pin,uint64_t fingerprint,const Config& config,std::string& error){impl_->stop();return impl_->start(true,port,pin,fingerprint,config,error);}
bool InviteTransport::join(const std::string& code,uint64_t fingerprint,const Config& config,std::string& error){impl_->stop();return impl_->start(false,0,code,fingerprint,config,error);}
Status InviteTransport::status()const{std::lock_guard lock(impl_->state_mutex);return impl_->state;}
std::string InviteTransport::invitation()const{return status().code;}
std::string InviteTransport::pin()const{std::lock_guard lock(impl_->state_mutex);return impl_->pin_value;}
void InviteTransport::stop(){impl_->stop();}
}
