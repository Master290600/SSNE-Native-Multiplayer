#include "authoritative_session.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <cstring>
#include <limits>
#include <cstdlib>

namespace port::coop {
namespace {
using Clock=std::chrono::steady_clock;
bool reuse_world_copies(){static const bool enabled=[] {const auto value=std::getenv("SSNE_WORLD_COPY_REUSE");return !value||std::strcmp(value,"0");}();return enabled;}

uint64_t milliseconds(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
bool initialize_network(){static const bool ok=[](){WSADATA data{};return WSAStartup(MAKEWORD(2,2),&data)==0;}();return ok;}
bool pin_valid(const std::string& s){return s.size()==6 && std::all_of(s.begin(),s.end(),[](char c){return c>='0' && c<='9';});}
bool nonblocking(SOCKET s){u_long mode=1;return ioctlsocket(s,FIONBIO,&mode)==0;}
bool retry_socket(){auto error=WSAGetLastError();return error==WSAEWOULDBLOCK || error==WSAEINPROGRESS;}
void close_socket(SOCKET& s){if(s!=INVALID_SOCKET){shutdown(s,SD_BOTH);closesocket(s);s=INVALID_SOCKET;}}
void socket_options(SOCKET s){BOOL yes=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&yes),sizeof(yes));}
void put32(std::vector<uint8_t>& b,uint32_t v){for(int i=3;i>=0;--i)b.push_back(uint8_t(v>>(i*8)));}
void put64(std::vector<uint8_t>& b,uint64_t v){put32(b,uint32_t(v>>32));put32(b,uint32_t(v));}
void put_string(std::vector<uint8_t>& b,const std::string& s){put32(b,uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
uint32_t read32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
struct Reader {
    const std::vector<uint8_t>& bytes;size_t offset=0;bool ok=true;
    uint8_t u8(){if(offset>=bytes.size()){ok=false;return 0;}return bytes[offset++];}
    uint32_t u32(){if(bytes.size()-offset<4){ok=false;return 0;}auto value=read32(bytes.data()+offset);offset+=4;return value;}
    uint64_t u64(){auto high=u32(),low=u32();return uint64_t(high)<<32|low;}
    std::string string(unsigned limit){auto n=u32();if(n>limit || n>bytes.size()-offset){ok=false;return {};}std::string s(bytes.begin()+offset,bytes.begin()+offset+n);offset+=n;return s;}
    bool done() const{return ok && offset==bytes.size();}
};
std::vector<uint8_t> packet(wire::Message type,std::vector<uint8_t> body={}) {
    std::vector<uint8_t> frame;frame.reserve(body.size()+wire::header_size);put32(frame,wire::magic);
    frame.push_back(uint8_t(wire::version>>8));frame.push_back(uint8_t(wire::version));
    frame.push_back(uint8_t(uint16_t(type)>>8));frame.push_back(uint8_t(type));put32(frame,uint32_t(body.size()));
    frame.insert(frame.end(),body.begin(),body.end());return frame;
}
struct Stream {
    SOCKET socket=INVALID_SOCKET;
    std::vector<uint8_t> receive,transmit;
    size_t sent=0;
    size_t incoming_limit=max_frame_bytes;
    uint64_t last_receive=milliseconds(),last_progress=milliseconds();
    bool read(uint64_t& total) {
        uint8_t buffer[65536];auto n=recv(socket,reinterpret_cast<char*>(buffer),sizeof(buffer),0);
        if(n==SOCKET_ERROR)return retry_socket();if(!n)return false;
        total+=unsigned(n);last_receive=milliseconds();receive.insert(receive.end(),buffer,buffer+n);
        return receive.size()<=incoming_limit+wire::header_size+sizeof(buffer);
    }
    template<class F> bool parse(F callback) {
        size_t consumed=0;unsigned frames=0;
        while(receive.size()-consumed>=wire::header_size && frames++<128) {
            const auto* p=receive.data()+consumed;
            if(read32(p)!=wire::magic || (uint16_t(p[4])<<8|p[5])!=wire::version)return false;
            const auto type=wire::Message(uint16_t(p[6])<<8|p[7]);const auto size=read32(p+8);
            if(size>incoming_limit)return false;
            if(receive.size()-consumed<wire::header_size+size)break;
            std::vector<uint8_t> body(p+wire::header_size,p+wire::header_size+size);
            if(!callback(type,body))return false;consumed+=wire::header_size+size;
        }
        if(consumed)receive.erase(receive.begin(),receive.begin()+consumed);return true;
    }
    bool write(uint64_t& total) {
        if(transmit.empty())return true;
        auto n=send(socket,reinterpret_cast<const char*>(transmit.data()+sent),int(transmit.size()-sent),0);
        if(n==SOCKET_ERROR)return retry_socket();if(!n)return false;
        total+=unsigned(n);sent+=unsigned(n);last_progress=milliseconds();
        if(sent==transmit.size()){transmit.clear();sent=0;}return true;
    }
    void queue(std::vector<uint8_t> frame){transmit=std::move(frame);sent=0;last_progress=milliseconds();}
};
bool control_valid(const ControlInput& c) {
    return !(c.pad.buttons&~uint16_t(0x1F7F)) && !(c.pressed&~uint16_t(0x1F7F)) &&
        c.yaw>=-2000000 && c.yaw<=2000000 && c.pitch>=-2000000 && c.pitch<=2000000 && (c.weapon==255 || c.weapon<32) && std::all_of(c.preferences.begin(),c.preferences.end(),[](auto n){return n==255 || n<=3;});
}
void append_control(std::vector<uint8_t>& b,const ControlInput& c) {
    b.push_back(uint8_t(c.pad.buttons>>8));b.push_back(uint8_t(c.pad.buttons));
    b.push_back(c.pad.stick_x);b.push_back(c.pad.stick_y);b.push_back(c.pad.cstick_x);b.push_back(c.pad.cstick_y);
    b.push_back(c.pad.trigger_l);b.push_back(c.pad.trigger_r);
    b.push_back(uint8_t(c.pressed>>8));b.push_back(uint8_t(c.pressed));put32(b,uint32_t(c.yaw));put32(b,uint32_t(c.pitch));b.push_back(c.weapon);
    b.insert(b.end(),c.preferences.begin(),c.preferences.end());
}
ControlInput read_control(Reader& r) {
    ControlInput c;c.pad.buttons=uint16_t(r.u8())<<8;c.pad.buttons|=r.u8();
    c.pad.stick_x=r.u8();c.pad.stick_y=r.u8();c.pad.cstick_x=r.u8();c.pad.cstick_y=r.u8();
    c.pad.trigger_l=r.u8();c.pad.trigger_r=r.u8();c.pressed=uint16_t(r.u8())<<8;c.pressed|=r.u8();
    c.yaw=int32_t(r.u32());c.pitch=int32_t(r.u32());c.weapon=r.u8();for(auto& n:c.preferences)n=r.u8();return c;
}
void merge_input(ControlInput& stored,const ControlInput& next) {
    auto yaw=std::clamp(int64_t(stored.yaw)+next.yaw,int64_t(-2000000),int64_t(2000000));
    auto pitch=std::clamp(int64_t(stored.pitch)+next.pitch,int64_t(-2000000),int64_t(2000000));
    auto edges=uint16_t(stored.pressed|next.pressed|(next.pad.buttons&~stored.pad.buttons));
    auto weapon=next.weapon==255?stored.weapon:next.weapon;
    stored=next;stored.yaw=int32_t(yaw);stored.pitch=int32_t(pitch);stored.pressed=edges;stored.weapon=weapon;
}
void drain_transients(ControlInput& c){c.yaw=c.pitch=0;c.pressed=0;c.weapon=255;}
}

struct AuthoritativeServer::Impl {
    struct InputState { ControlInput input;uint64_t sequence=0,received=0,ack=0; };
    struct Peer {
        Stream stream;std::string address;PlayerId id;bool assigned=false,reject=false,force_full=true;
        std::deque<std::vector<uint8_t>> priority;
        std::shared_ptr<const WorldState> baseline,in_flight;
        std::map<uint64_t,std::shared_ptr<const WorldState>> sent_worlds;
        uint64_t last_state=0;
    };
    mutable std::recursive_mutex mutex;
    SessionAuthority authority;NetworkStatus state;
    std::shared_ptr<const WorldState> body=std::make_shared<WorldState>(),published;
    std::array<InputState,max_players> controls{};
    std::array<PlayerId,max_players> reported{};
    std::array<LobbySkinChoice,max_players> skin_choices{};
    struct EventConsumer {PlayerId player{};uint64_t epoch=0;NativeEventCursors consumed{},offered{},initial_floor{};bool initialized=false,offered_initial=false;};
    NativeEventJournal native_events;
    std::array<EventConsumer,max_players> event_consumers{};
    uint64_t revision=0,fingerprint=0;
    std::array<PlayerId,max_players> bots{};
    std::array<unsigned,max_players> bot_skills{};
    unsigned match_player_limit=8;
    bool lobby_open=true;
    uint8_t lobby_ready_mask=0;
    std::string pin,lan_id;
    struct LanPermit {std::string peer;uint32_t nonce=0;uint64_t expires=0;};
    std::map<std::string,LanPermit> lan_permits;
    SOCKET listener=INVALID_SOCKET;std::thread worker;std::atomic<bool> stopping{false};
    std::vector<Peer> peers;
    explicit Impl(unsigned capacity):authority(capacity){state.capacity=capacity;rebuild();}
    void rebuild(std::shared_ptr<WorldState> unpublished={}) {
        // Only publish() supplies a newly allocated, unescaped world. The body
        // holds its other reference. Published/retained snapshots remain const.
        const bool exclusive=unpublished && unpublished.use_count()==2 && unpublished.get()==body.get() && unpublished.get()!=published.get();
        auto world=exclusive?std::move(unpublished):std::make_shared<WorldState>(*body);world->revision=++revision;
        uint8_t bot_mask=0;std::vector<int64_t> skills(max_players,0);
        for(unsigned i=1;i<max_players;++i)if(bots[i].generation && authority.participant(bots[i])){authority.automate(bots[i]);bot_mask|=uint8_t(1u<<i);skills[i]=bot_skills[i];if(lobby_open)lobby_ready_mask|=uint8_t(1u<<i);}
        world->mission["Session.Bots.Mask"]=int64_t(bot_mask);world->mission["Session.Bots.Skills"]=std::move(skills);
        world->players=authority.participants();world->vote=authority.skip_vote();
        uint8_t connected=0;for(unsigned i=0;i<max_players;++i)if(world->players[i].connected)connected|=uint8_t(1u<<i);
        lobby_ready_mask&=connected;
        world->mission["Session.Lobby.Open"]=int64_t(lobby_open);
        world->mission["Session.Lobby.ReadyMask"]=int64_t(lobby_ready_mask);
        // Input acknowledgements must describe this captured pose. Taking
        // controls happens BEFORE native simulation; votes/joins must not pair
        // those newer acknowledgements with the previous frame's camera.
        published=world;state.world_revision=world->revision;state.joined=authority.connected_count();
    }
    bool results_active()const{
        const auto it=body->mission.find("Session.Results.Active");
        const auto active=it==body->mission.end()?nullptr:std::get_if<int64_t>(&it->second);
        return active && *active==1;
    }
    void drop(Peer& peer) {
        close_socket(peer.stream.socket);
        if(peer.assigned){std::lock_guard lock(mutex);if(authority.leave(peer.id)){controls[peer.id.slot]={};rebuild();}peer.assigned=false;}
    }
    bool message(Peer& peer,wire::Message type,const std::vector<uint8_t>& bytes) {
        Reader r{bytes};
        if(!peer.assigned) {
            if(type!=wire::Message::Hello || bytes.size()>128 || peer.reject)return false;
            auto content=r.u64();auto entered=r.string(32);auto name=r.string(64);const auto skin=r.done()?255:r.u8();
            std::lock_guard lock(mutex);
            std::string rejection;
            const auto permit=lan_permits.find(entered);
            const bool lan_join=permit!=lan_permits.end() && permit->second.peer==peer.address && permit->second.expires>milliseconds();
            const bool pin_join=pin_valid(entered) && entered==pin;
            if(!r.done() || (skin!=255 && skin>10) || content!=fingerprint || (!pin_join && !lan_join))rejection="PIN, game data or cooperative protocol differs.";
            if(rejection.empty() && (name.empty() || name.find_first_not_of(' ')==std::string::npos || std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32 || c==127;})))rejection="Invalid player name.";
            if(rejection.empty() && authority.connected_count()>=match_player_limit)for(unsigned i=match_player_limit;i-->1;)if(bots[i].generation && authority.participant(bots[i])){authority.leave(bots[i]);bots[i]={};controls[i]={};break;}
            if(rejection.empty() && authority.connected_count()>=match_player_limit)rejection="The selected game mode is full (maximum "+std::to_string(match_player_limit)+" players).";
            auto id=rejection.empty()?authority.join(name):std::nullopt;
            if(!id && rejection.empty())rejection="Session is full or player name is invalid.";
            if(!id){std::vector<uint8_t> data;put_string(data,rejection);peer.priority.push_back(packet(wire::Message::Rejected,std::move(data)));peer.reject=true;return true;}
            if(lan_join)lan_permits.erase(permit); // Consume only after a successful player allocation.
            peer.id=*id;peer.assigned=true;skin_choices[peer.id.slot]={peer.id,uint8_t(skin)};peer.stream.incoming_limit=128;controls[id->slot]={};event_consumers[id->slot]={};event_consumers[id->slot].player=*id;rebuild();
            std::vector<uint8_t> assigned;put32(assigned,id->slot);put64(assigned,id->generation);put32(assigned,state.capacity);
            peer.priority.push_back(packet(wire::Message::Assigned,std::move(assigned)));return true;
        }
        // The connection's identity is authoritative. No gameplay state, target
        // player ID, spawn, damage, mission or transform message is accepted here.
        std::lock_guard lock(mutex);
        if(!authority.participant(peer.id))return false;
        switch(type) {
        case wire::Message::LobbyReady: {
            const auto epoch=r.u64();const auto value=r.u8();if(!r.done() || value>1)return false;
            if(epoch!=body->epoch || !lobby_open)return true;
            const auto mask=uint8_t(1u<<peer.id.slot);
            const auto next=uint8_t(value?lobby_ready_mask|mask:lobby_ready_mask&~mask);
            if(next!=lobby_ready_mask){lobby_ready_mask=next;rebuild();}return true;
        }
        case wire::Message::Input: {
            const auto epoch=r.u64(),sequence=r.u64();auto input=read_control(r);if(!r.done() || !sequence || !control_valid(input))return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch)return false;
            auto& old=controls[peer.id.slot];if(sequence<=old.sequence)return true;
            old.sequence=sequence;old.received=milliseconds();merge_input(old.input,input);return true;
        }
        case wire::Message::Ready: {
            auto epoch=r.u64();if(!r.done())return false;
            auto character=authority.participant(peer.id)->character;
            if(epoch==body->epoch && body->entities.contains(character) && body->entities.at(character).owner==peer.id && authority.finish_join(peer.id))rebuild();return true;
        }
        case wire::Message::Afk: {
            const auto epoch=r.u64(),sequence=r.u64();auto value=r.u8();if(!r.done() || value>1)return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch)return false;
            if(authority.request_afk(peer.id,sequence,value!=0,results_active())==CommandResult::Applied){controls[peer.id.slot].input={};rebuild();}return true;
        }
        case wire::Message::Skip: {
            const auto epoch=r.u64(),sequence=r.u64(),scene=r.u64();if(!r.done())return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch)return false;
            if(authority.request_skip(peer.id,sequence,scene)==CommandResult::Applied)rebuild();return true;
        }
        case wire::Message::SnapshotRequest:if(!r.done())return false;peer.force_full=true;return true;
        case wire::Message::NativeEventsAck: {
            const auto epoch=r.u64();NativeEventCursors consumed{};for(auto& cursor:consumed)cursor=r.u64();
            if(!r.done())return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch || !native_events.active())return false;
            auto& consumer=event_consumers[peer.id.slot];
            if(consumer.player!=peer.id)return false;
            if(consumer.epoch!=epoch){consumer.epoch=epoch;consumer.consumed={};consumer.initialized=false;}
            bool changed=!consumer.initialized;
            for(unsigned i=0;i<native_event_channels;++i){if(consumed[i]>consumer.offered[i] || (consumer.initialized && consumed[i]<consumer.consumed[i]))return false;changed|=consumed[i]!=consumer.consumed[i];}
            if(changed){consumer.consumed=consumed;consumer.initialized=true;rebuild();}
            return true;
        }
        case wire::Message::WorldAck: {
            auto revision=r.u64();if(!r.done() || !revision)return false;
            if(peer.baseline && revision<=peer.baseline->revision)return true;
            if(auto found=peer.sent_worlds.find(revision);found!=peer.sent_worlds.end())peer.baseline=found->second;
            else if(peer.in_flight && peer.stream.transmit.empty() && peer.in_flight->revision==revision)peer.baseline=peer.in_flight;
            else return false;
            return true;
        }
        case wire::Message::Ping:{auto time=r.u64();if(!r.done())return false;std::vector<uint8_t> data;put64(data,time);if(peer.priority.size()<32)peer.priority.push_back(packet(wire::Message::Pong,std::move(data)));return true;}
        default:return false;
        }
    }
    void run() {
        while(!stopping) {
            fd_set reads,writes;FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(listener,&reads);
            for(auto& peer:peers){FD_SET(peer.stream.socket,&reads);FD_SET(peer.stream.socket,&writes);}
            timeval timeout{0,1000};auto selected=select(0,&reads,&writes,nullptr,&timeout);if(selected==SOCKET_ERROR){std::lock_guard lock(mutex);state.message="Cooperative host socket polling failed: "+std::to_string(WSAGetLastError());break;}
            if(FD_ISSET(listener,&reads))for(unsigned attempts=0;attempts<16;++attempts) {
                sockaddr_in remote{};int length=sizeof(remote);auto socket=accept(listener,reinterpret_cast<sockaddr*>(&remote),&length);if(socket==INVALID_SOCKET)break;
                char address[INET_ADDRSTRLEN]{};
                if(peers.size()>=16 || !nonblocking(socket) || !inet_ntop(AF_INET,&remote.sin_addr,address,sizeof(address))){closesocket(socket);continue;}socket_options(socket);Peer peer;peer.address=address;peer.stream.socket=socket;peer.stream.incoming_limit=256;peers.push_back(std::move(peer));
            }
            for(size_t i=0;i<peers.size();) {
                auto& peer=peers[i];bool ok=true;uint64_t received=0,sent=0;
                if(FD_ISSET(peer.stream.socket,&reads))ok=peer.stream.read(received);
                if(ok)ok=peer.stream.parse([&](auto type,const auto& bytes){return message(peer,type,bytes);});
                if(ok && peer.stream.transmit.empty()) {
                    if(peer.in_flight){peer.sent_worlds[peer.in_flight->revision]=std::move(peer.in_flight);while(peer.sent_worlds.size()>64)peer.sent_worlds.erase(peer.sent_worlds.begin());}
                    if(!peer.priority.empty()){peer.stream.queue(std::move(peer.priority.front()));peer.priority.pop_front();}
                    else if(peer.reject)ok=false;
                    else if(peer.assigned && milliseconds()-peer.last_state>=16) {
                        std::shared_ptr<const WorldState> current;
                        {std::lock_guard lock(mutex);current=published;
                            if(native_events.active() && native_events.epoch()==published->epoch) {
                                auto& consumer=event_consumers[peer.id.slot];
                                if(consumer.epoch!=published->epoch){consumer.epoch=published->epoch;consumer.consumed={};consumer.offered={};consumer.initialized=false;consumer.offered_initial=false;consumer.initial_floor={};}
                                if(consumer.initialized && !native_events.available(consumer.consumed)){ok=false;}
                                else {auto customized=std::make_shared<WorldState>(*published);native_events.decorate(*customized,consumer.consumed,consumer.initialized);
                                    const auto tails=native_events.tails();
                                    if(!consumer.initialized){consumer.offered=tails;if(!consumer.offered_initial){consumer.initial_floor=tails;consumer.offered_initial=true;}}
                                    else {
                                        const char* prefixes[]={"Session.ConfirmedHits.Ids","Session.RocketEffects.Ids","Session.WeaponShots.Ids","Session.WeaponLoops.Ids","Session.BulletImpacts.Ids","Session.Feedback.Ids","Session.BulletTrails.Ids","Session.WeaponEmitters.Ids","Session.WeaponCasings.Ids","Session.ProjectileVisuals.Ids","Session.WeaponSurfaces.Ids","Session.WeaponBeams.Ids"};
                                        for(unsigned n=0;n<native_event_channels;++n){const auto& ids=std::get<std::vector<int64_t>>(customized->mission.at(prefixes[n]));if(!ids.empty())consumer.offered[n]=std::max(consumer.offered[n],uint64_t(ids[ids.size()-(n==2?2:1)]));}
                                    }
                                    current=std::move(customized);
                                }
                            }
                        }
                        if(ok && (peer.force_full || !peer.baseline || current->revision!=peer.baseline->revision)) {
                            try{peer.stream.queue(packet(wire::Message::World,encode_delta(difference(peer.force_full?nullptr:peer.baseline.get(),*current))));peer.in_flight=current;peer.last_state=milliseconds();peer.force_full=false;}
                            catch(const std::exception&){ok=false;}
                        }
                    }
                }
                if(ok && FD_ISSET(peer.stream.socket,&writes))ok=peer.stream.write(sent);
                {std::lock_guard lock(mutex);state.bytes_received+=received;state.bytes_sent+=sent;}
                if(milliseconds()-peer.stream.last_receive>15000 || (!peer.stream.transmit.empty() && milliseconds()-peer.stream.last_progress>10000))ok=false;
                if(!ok){drop(peer);peers.erase(peers.begin()+i);}else ++i;
            }
            // select reports a TCP socket writable even when no packet is due.
            // Bound idle polling without coupling it to the simulation thread.
            if(selected>0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for(auto& peer:peers)drop(peer);peers.clear();close_socket(listener);
        std::lock_guard lock(mutex);state.connected=false;lan_permits.clear();lan_id.clear();
    }
};
AuthoritativeServer::AuthoritativeServer(unsigned capacity):impl_(std::make_unique<Impl>(capacity)){}
bool AuthoritativeServer::set_match_player_limit(unsigned limit){
    auto& p=*impl_;std::lock_guard lock(p.mutex);limit=std::min(limit,p.state.capacity);
    if(!limit)return false;
    const auto players=p.authority.participants();for(unsigned slot=limit;slot<max_players;++slot)if(players[slot].connected && players[slot].id!=p.bots[slot])return false;
    for(unsigned slot=limit;slot<max_players;++slot)if(players[slot].connected){p.authority.leave(p.bots[slot]);p.bots[slot]={};p.controls[slot]={};}
    p.match_player_limit=limit;p.rebuild();return true;
}
std::optional<PlayerId> AuthoritativeServer::add_bot(unsigned skill){
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.state.connected || skill<1 || skill>3 || p.authority.connected_count()>=p.match_player_limit)return {};
    unsigned slot=1;const auto participants=p.authority.participants();while(slot<max_players && participants[slot].connected)++slot;
    const auto id=p.authority.join("Bot "+std::to_string(slot),true);if(!id)return {};
    p.bots[id->slot]=*id;p.bot_skills[id->slot]=skill;p.controls[id->slot]={};p.skin_choices[id->slot]={*id,uint8_t(id->slot%11)};p.rebuild();return id;
}
bool AuthoritativeServer::remove_bot(PlayerId id){auto& p=*impl_;std::lock_guard lock(p.mutex);if(id.slot>=max_players || p.bots[id.slot]!=id || !p.authority.leave(id))return false;p.bots[id.slot]={};p.controls[id.slot]={};p.rebuild();return true;}
uint8_t AuthoritativeServer::bot_mask()const{auto& p=*impl_;std::lock_guard lock(p.mutex);uint8_t mask=0;for(unsigned i=1;i<max_players;++i)if(p.bots[i].generation && p.authority.participant(p.bots[i]))mask|=uint8_t(1u<<i);return mask;}
unsigned AuthoritativeServer::bot_skill(unsigned slot)const{auto& p=*impl_;std::lock_guard lock(p.mutex);return slot<max_players?p.bot_skills[slot]:2;}
AuthoritativeServer::~AuthoritativeServer(){stop();}
bool AuthoritativeServer::start(uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& name,unsigned skin) {
    auto& p=*impl_;if(p.worker.joinable() || p.authority.connected_count() || !initialize_network() || !pin_valid(pin) || (skin!=255 && skin>10))return false;
    const auto session_id=new_lan_secret();if(session_id.empty())return false;
    p.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(p.listener==INVALID_SOCKET)return false;
    // Hidden native regression fixtures only use same-PC peers. Avoid opening
    // a LAN listener (and a fresh Windows Firewall prompt) for every test EXE.
    const auto hidden=std::getenv("SSNE_TEST_HIDDEN");
    const bool local_test=hidden && std::strcmp(hidden,"1")==0;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(local_test?INADDR_LOOPBACK:INADDR_ANY);address.sin_port=htons(port);
    if(bind(p.listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(p.listener,8) || !nonblocking(p.listener)){close_socket(p.listener);return false;}
    int length=sizeof(address);getsockname(p.listener,reinterpret_cast<sockaddr*>(&address),&length);
    {std::lock_guard lock(p.mutex);auto id=p.authority.join(name);if(!id){close_socket(p.listener);return false;}p.state.player=*id;p.state.port=ntohs(address.sin_port);p.state.connected=true;p.skin_choices[0]={*id,uint8_t(skin)};p.state.message="Host running; clients may join.";p.pin=pin;p.lan_id=session_id;p.lan_permits.clear();p.fingerprint=fingerprint;p.stopping=false;p.rebuild();}
    p.worker=std::thread([&p]{p.run();});return true;
}
std::string AuthoritativeServer::lan_session_id()const{std::lock_guard lock(impl_->mutex);return impl_->state.connected?impl_->lan_id:std::string{};}
LanJoinGrant AuthoritativeServer::issue_lan_join_ticket(const std::string& peer,uint32_t nonce,uint32_t lifetime_ms){
    if(!lifetime_ms || lifetime_ms>60000 || !local_lan_peer(peer))return {};
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.state.connected || p.stopping || !valid_lan_secret(p.lan_id))return {};
    const auto now=milliseconds();for(auto it=p.lan_permits.begin();it!=p.lan_permits.end();)if(it->second.expires<=now)it=p.lan_permits.erase(it);else ++it;
    for(const auto& [ticket,permit]:p.lan_permits)if(permit.peer==peer && permit.nonce==nonce)return {ticket,uint32_t(permit.expires-now)};
    if(p.lan_permits.size()>=64 || std::count_if(p.lan_permits.begin(),p.lan_permits.end(),[&](const auto& item){return item.second.peer==peer;})>=16)return {};
    auto ticket=new_lan_secret();if(ticket.empty() || p.lan_permits.contains(ticket))return {};
    p.lan_permits.emplace(ticket,Impl::LanPermit{peer,nonce,now+lifetime_ms});return {std::move(ticket),lifetime_ms};
}
std::array<LobbySkinChoice,max_players> AuthoritativeServer::requested_skins()const{std::lock_guard lock(impl_->mutex);auto result=impl_->skin_choices;for(unsigned i=0;i<max_players;++i)if(!impl_->authority.participant(result[i].player))result[i]={};return result;}
NetworkStatus AuthoritativeServer::status() const{std::lock_guard lock(impl_->mutex);return impl_->state;}
std::shared_ptr<const WorldState> AuthoritativeServer::latest_world() const{std::lock_guard lock(impl_->mutex);return impl_->published;}
void AuthoritativeServer::lobby_ready(bool ready){auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.lobby_open)return;const auto mask=uint8_t(1u<<p.state.player.slot);p.lobby_ready_mask=uint8_t(ready?p.lobby_ready_mask|mask:p.lobby_ready_mask&~mask);p.rebuild();}
bool AuthoritativeServer::begin_lobby_game(){auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.lobby_open || !p.state.connected)return false;for(const auto& person:p.authority.participants())if(person.connected && !(p.lobby_ready_mask&(1u<<person.id.slot)))return false;p.lobby_open=false;p.lobby_ready_mask=0;p.rebuild();return true;}
std::vector<ConnectionEvent> AuthoritativeServer::take_events() {
    auto& p=*impl_;std::lock_guard lock(p.mutex);std::vector<ConnectionEvent> events;
    for(unsigned slot=0;slot<max_players;++slot) {
        const auto& now=p.authority.participants()[slot];auto before=p.reported[slot];
        if(before.slot<max_players && (!now.connected || before!=now.id))events.push_back({ConnectionEvent::Kind::Left,before});
        if(now.connected && before!=now.id)events.push_back({ConnectionEvent::Kind::Joined,now.id});
        p.reported[slot]=now.connected?now.id:PlayerId{};
    }
    return events;
}
bool AuthoritativeServer::publish(WorldState world,std::string& error,bool loading,std::span<const WorldState> native_batches) {
    return publish_impl(std::move(world),error,loading,native_batches,false);
}
bool AuthoritativeServer::publish_owned(WorldState world,std::string& error,bool loading,std::span<const WorldState> native_batches) {
    return publish_impl(std::move(world),error,loading,native_batches,true);
}
bool AuthoritativeServer::publish_impl(WorldState world,std::string& error,bool loading,std::span<const WorldState> native_batches,bool owned) {
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    if(world.epoch<p.body->epoch || (world.epoch==p.body->epoch && world.tick<p.body->tick)){error="Host world went backwards";return false;}
    world.players=p.authority.participants();world.vote=p.authority.skip_vote();world.revision=p.revision+1;
    const bool reset=loading || world.epoch!=p.body->epoch;
    const auto phase=world.mission.find("Session.NativePhase");
    const bool credits=phase!=world.mission.end() && phase->second==Property(std::string("credits"));
    const auto movie=world.mission.find("Session.Movie.Active");
    const bool presentation=credits || (movie!=world.mission.end() && movie->second==Property(int64_t(1)));
    if(reset){for(auto& player:world.players){player.ready=false;player.character=0;}if(!presentation)world.vote={};}
    if(!validate_world(world,error))return false;
    try{p.native_events.ingest(world,native_batches);}catch(const std::exception& e){error=e.what();return false;}
    auto minimum=p.native_events.tails();
    for(unsigned i=0;i<max_players;++i){const auto* person=p.authority.participant(p.event_consumers[i].player);const auto& consumer=p.event_consumers[i];if(person && consumer.epoch==world.epoch && (consumer.initialized || consumer.offered_initial))for(unsigned n=0;n<native_event_channels;++n)minimum[n]=std::min(minimum[n],consumer.initialized?consumer.consumed[n]:consumer.initial_floor[n]);}
    p.native_events.prune(minimum);
    if(reset){
        p.authority.begin_loading(presentation);
        // A menu snapshot is not a new load every frame. Clearing here used
        // to drop key edges between network receipt and the next guest tick.
        const bool menu=phase!=world.mission.end() && phase->second==Property(std::string("menu"));
        const auto result_flag=world.mission.find("Session.Results.Active");
        const auto active_results=result_flag==world.mission.end()?nullptr:std::get_if<int64_t>(&result_flag->second);
        const bool results=active_results && *active_results==1;
        if((!menu && !results) || world.epoch!=p.body->epoch)for(auto& input:p.controls){input.input={};input.received=0;}
    }
    if(auto phase=world.mission.find("Session.NativePhase");phase!=world.mission.end()) {
        const bool menu=phase->second==Property(std::string("menu"));
        // Loading during boot is not a new lobby. Re-enter only on a real menu edge.
        const auto previous=p.body->mission.find("Session.NativePhase");
        if(menu && (previous==p.body->mission.end() || previous->second!=phase->second)){p.lobby_open=true;p.lobby_ready_mask=0;}
        else if(!menu && phase->second!=Property(std::string("loading"))){p.lobby_open=false;p.lobby_ready_mask=0;}
    }
    for(unsigned i=0;i<max_players;++i)world.input_ack[i]=p.controls[i].ack;
    auto unpublished=std::make_shared<WorldState>(std::move(world));p.body=unpublished;p.rebuild(owned&&reuse_world_copies()?std::move(unpublished):nullptr);return true;
}
bool AuthoritativeServer::attach_character(PlayerId id,EntityId entity) {
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    if(!p.body->entities.contains(entity) || p.body->entities.at(entity).kind!=EntityKind::Player || p.body->entities.at(entity).owner!=id)return false;
    if(!p.authority.attach_character(id,entity))return false;if(p.bots[id.slot]==id)p.authority.finish_join(id);p.rebuild();return true;
}
bool AuthoritativeServer::ready_host(){auto& p=*impl_;std::lock_guard lock(p.mutex);auto* player=p.authority.participant(p.state.player);if(!player || !p.body->entities.contains(player->character) || p.body->entities.at(player->character).owner!=player->id || !p.authority.finish_join(p.state.player))return false;p.rebuild();return true;}
std::array<PlayerControl,max_players> AuthoritativeServer::take_controls() {
    auto& p=*impl_;std::lock_guard lock(p.mutex);std::array<PlayerControl,max_players> result;bool changed=false;
    for(unsigned slot=0;slot<max_players;++slot) {
        const auto& player=p.authority.participants()[slot];if(!player.connected)continue;
        auto& input=p.controls[slot];auto& out=result[slot];out.id=player.id;out.ready=player.ready;out.afk=player.afk;out.sequence=input.sequence;
        if(player.id==p.state.player && p.results_active() && milliseconds()-input.received<250){
            // AFK controls the character, never the host's shared result menu.
            // Accept only fresh human menu buttons while native actors are gone.
            constexpr uint16_t menu_buttons=ssne::PAD_A|ssne::PAD_B|ssne::PAD_X|ssne::PAD_Y|ssne::PAD_LEFT|ssne::PAD_RIGHT|ssne::PAD_UP|ssne::PAD_DOWN;
            out.input.pad.buttons=uint16_t((input.input.pad.buttons|input.input.pressed)&menu_buttons);
            out.input.pressed=uint16_t(input.input.pressed&menu_buttons);
        }
        else if((player.ready || (player.id==p.state.player && !player.character)) && !player.afk && milliseconds()-input.received<250){out.input=input.input;out.input.pad.buttons|=out.input.pressed;}
        else if(p.lobby_open && !p.results_active() && !player.afk && milliseconds()-input.received<250){
            // Unspawned peers may choose only their own lobby character.
            // Do not forward confirm/back, camera, movement or firing.
            out.input.pad.buttons=input.input.pad.buttons&(ssne::PAD_LEFT|ssne::PAD_RIGHT);
            out.input.pressed=input.input.pressed&(ssne::PAD_LEFT|ssne::PAD_RIGHT);
        }
        if(player.ready && input.ack!=input.sequence){input.ack=input.sequence;changed=true;}
        drain_transients(input.input);
    }
    if(changed)p.rebuild();return result;
}
uint64_t AuthoritativeServer::begin_cutscene(bool presentation){auto& p=*impl_;std::lock_guard lock(p.mutex);auto id=p.authority.begin_cutscene(presentation);p.rebuild();return id;}
std::array<Participant,max_players> AuthoritativeServer::participants() const{std::lock_guard lock(impl_->mutex);return impl_->authority.participants();}
std::shared_ptr<void> AuthoritativeServer::checkpoint_control_lease(){return std::make_shared<std::unique_lock<std::recursive_mutex>>(impl_->mutex);}
std::vector<uint8_t> AuthoritativeServer::checkpoint_authority() const{std::lock_guard lock(impl_->mutex);return impl_->authority.checkpoint_state();}
bool AuthoritativeServer::restore_checkpoint_authority(const std::vector<uint8_t>& bytes,std::string& error){std::lock_guard lock(impl_->mutex);return impl_->authority.restore_checkpoint(bytes,error);}
bool AuthoritativeServer::restore_gameplay_authority(const std::vector<uint8_t>& bytes,std::string& error){std::lock_guard lock(impl_->mutex);return impl_->authority.restore_gameplay_checkpoint(bytes,error);}
std::unique_ptr<AuthoritativeServer::PreparedResume> AuthoritativeServer::prepare_checkpoint_resume(
        const std::vector<uint8_t>& live,uint64_t epoch,const std::string& level,const Properties& setup,
        const NativeEventCursors& tails,std::string& error) {
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    try {
        if(epoch<=p.body->epoch){error="Checkpoint resume requires a fresh world epoch";return {};}
        auto authority=p.authority;if(!authority.retain_live_history(live,error))return {};
        authority.begin_loading();
        auto world=std::make_shared<WorldState>();world->epoch=epoch;world->revision=p.revision+1;
        world->level=level;world->mission=setup;world->mission["Session.NativePhase"]=std::string("loading");
        world->players=authority.participants();
        for(unsigned i=0;i<max_players;++i)world->input_ack[i]=p.controls[i].ack;
        if(!validate_world(*world,error))return {};
        NativeEventJournal journal;journal.resume(epoch,tails);
        auto result=std::make_unique<PreparedResume>();
        result->commit_=[&p,authority=std::move(authority),world=std::move(world),journal=std::move(journal)]()mutable{
            // The caller retains checkpoint_control_lease through this commit.
            // All validation/allocation precedes the durable card commit.
            p.authority=std::move(authority);p.body=world;p.published=std::move(world);
            p.revision=p.published->revision;p.state.world_revision=p.revision;
            p.native_events=std::move(journal);
            for(auto& input:p.controls){input.input={};input.received=0;}
        };
        return result;
    }catch(const std::exception& e){error=e.what();return {};}
}
bool AuthoritativeServer::end_cutscene(uint64_t scene){
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.authority.end_cutscene(scene))return false;
    // Camera identity and vote end are one atomic publication. A transport
    // snapshot between engine frames must not retain the previous camera ID.
    auto body=std::make_shared<WorldState>(*p.body);
    for(auto it=body->mission.begin();it!=body->mission.end();)
        if(it->first.starts_with("Session.Cutscene."))it=body->mission.erase(it);else ++it;
    p.body=std::move(body);p.rebuild();return true;
}
SkipVote AuthoritativeServer::skip_vote() const{std::lock_guard lock(impl_->mutex);return impl_->authority.skip_vote();}
std::optional<SkipDecision> AuthoritativeServer::skip_decision() const{std::lock_guard lock(impl_->mutex);return impl_->authority.skip_decision();}
CommandResult AuthoritativeServer::host_afk(uint64_t sequence,bool value){auto& p=*impl_;std::lock_guard lock(p.mutex);auto result=p.authority.request_afk(p.state.player,sequence,value,p.results_active());if(result==CommandResult::Applied){p.controls[p.state.player.slot].input={};p.rebuild();}return result;}
CommandResult AuthoritativeServer::host_skip(uint64_t sequence,uint64_t scene){auto& p=*impl_;std::lock_guard lock(p.mutex);auto result=p.authority.request_skip(p.state.player,sequence,scene);if(result==CommandResult::Applied)p.rebuild();return result;}
void AuthoritativeServer::submit_host_input(ControlInput input){if(!control_valid(input))return;auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.state.connected)return;auto& state=p.controls[p.state.player.slot];merge_input(state.input,input);++state.sequence;state.received=milliseconds();}
void AuthoritativeServer::disconnect(PlayerId id){auto& p=*impl_;std::lock_guard lock(p.mutex);if(id==p.state.player)return;if(p.authority.leave(id)){p.controls[id.slot]={};p.rebuild();}}
void AuthoritativeServer::stop(){auto& p=*impl_;p.stopping=true;if(p.worker.joinable())p.worker.join();std::lock_guard lock(p.mutex);p.lan_permits.clear();p.lan_id.clear();}

struct AuthoritativeClient::Impl {
    uint8_t skin=255;
    mutable std::mutex mutex;NetworkStatus state;
    std::shared_ptr<const WorldState> world=std::make_shared<WorldState>();
    double world_received=0;
    InputLookHistory look_history;
    std::deque<AuthoritativeClient::WorldSample> motion_history;
    std::map<uint64_t,std::shared_ptr<const WorldState>> history;
    std::thread worker;std::atomic<bool> stopping{false};
    ControlInput pending;uint64_t input_sequence=0,command_sequence=0,sent_input=0;
    std::deque<std::vector<uint8_t>> commands;
    std::string address,pin,name;uint16_t port=0;uint64_t fingerprint=0;
    void fail(const std::string& message){std::lock_guard lock(mutex);state.connecting=false;state.connected=false;state.message=message;}
    void enqueue(wire::Message type,std::vector<uint8_t> bytes={}) {
        if(commands.size()<128)commands.push_back(packet(type,std::move(bytes)));
        else state.message="Too many pending cooperative commands.";
    }
    bool world_message(const std::vector<uint8_t>& bytes) {
        {std::lock_guard lock(mutex);if(!state.connected)return false;}
        // Only this socket worker publishes worlds. Readers retain immutable
        // shared snapshots, so decoding/copying/validation needs no input or
        // render lock. Commit readiness, epoch and input reset together below.
        WorldDelta delta;std::string error;
        if(!decode_delta(bytes,delta,error)){std::lock_guard lock(mutex);state.message=error;return false;}
        std::shared_ptr<const WorldState> baseline,previous;
        {
            std::lock_guard lock(mutex);previous=world;
            if(delta.epoch<world->epoch || (delta.epoch==world->epoch && delta.revision<world->revision))return true;
            if(delta.epoch==world->epoch && delta.revision==world->revision && !delta.full){std::vector<uint8_t> ack;put64(ack,delta.revision);enqueue(wire::Message::WorldAck,std::move(ack));return true;}
            if(!delta.full){
                if(delta.base_revision==world->revision)baseline=world;
                else if(auto base=history.find(delta.base_revision);base!=history.end())baseline=base->second;
                else {enqueue(wire::Message::SnapshotRequest);return true;}
            }
        }
        std::shared_ptr<WorldState> next;
        bool applied=false;
        if(reuse_world_copies()){
            WorldState empty;if(delta.full)empty.epoch=delta.epoch;
            auto prepared=apply_delta_copy(baseline?*baseline:empty,delta,error);
            if(prepared){next=std::make_shared<WorldState>(std::move(*prepared));applied=true;}
        }else{
            next=baseline?std::make_shared<WorldState>(*baseline):std::make_shared<WorldState>();
            if(delta.full)next->epoch=delta.epoch;
            applied=apply_delta(*next,delta,error);
        }
        if(!applied){
            std::lock_guard lock(mutex);
            if(error=="Stale or missing world baseline"){enqueue(wire::Message::SnapshotRequest);return true;}
            state.message=error;return false;
        }
        // Old snapshot destruction can also be expensive. Release its last
        // references after leaving the short publication/input lock.
        decltype(history) retired_history;decltype(motion_history) retired_motion;
        std::vector<std::shared_ptr<const WorldState>> retired;
        {
            std::lock_guard lock(mutex);
            // Never relabel movement/fire accumulated during decode in the old
            // world as new input. Framed old-epoch commands remain rejectable.
            if(next->epoch!=world->epoch){pending={};sent_input=input_sequence;retired_history.swap(history);retired_motion.swap(motion_history);look_history.reset(next->epoch);}
            else if(world->revision)history[world->revision]=world;
            while(history.size()>64){retired.push_back(std::move(history.begin()->second));history.erase(history.begin());}
            world=next;world_received=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();state.world_revision=next->revision;state.joined=0;
            motion_history.push_back({world,world_received});while(motion_history.size()>32){retired.push_back(std::move(motion_history.front().world));motion_history.pop_front();}
            for(const auto& player:next->players)state.joined+=player.connected;
            std::vector<uint8_t> ack;put64(ack,next->revision);enqueue(wire::Message::WorldAck,std::move(ack));
            state.message="Connected to authoritative host.";
        }
        return true;
    }
    bool message(wire::Message type,const std::vector<uint8_t>& bytes) {
        if(type==wire::Message::World)return world_message(bytes);
        Reader r{bytes};std::lock_guard lock(mutex);
        if(type==wire::Message::Rejected){state.connecting=false;state.connected=false;state.message=r.string(256);return false;}
        if(type==wire::Message::Assigned) {
            auto slot=r.u32();auto generation=r.u64();auto capacity=r.u32();
            if(!r.done() || state.connected || !generation || slot>=capacity || capacity>8 || capacity<1)return false;
            state.player={slot,generation};state.capacity=capacity;state.connecting=false;state.connected=true;state.message="Connected; receiving authoritative world.";return true;
        }
        if(!state.connected)return false;
        if(type==wire::Message::Pong){auto sent=r.u64();if(!r.done())return false;state.ping_ms=unsigned(std::min<uint64_t>(60000,milliseconds()-sent));return true;}
        return false;
    }
    void run() {
        Stream stream;addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* addresses=nullptr;
        if(getaddrinfo(address.c_str(),std::to_string(port).c_str(),&hints,&addresses)){fail("Host address cannot be resolved.");return;}
        auto start=milliseconds();bool connected=false;int connect_error=0;
        for(auto* target=addresses;target && !stopping && !connected;target=target->ai_next) {
            stream.socket=socket(target->ai_family,target->ai_socktype,target->ai_protocol);
            if(stream.socket==INVALID_SOCKET || !nonblocking(stream.socket)){close_socket(stream.socket);continue;}
            const auto connection=connect(stream.socket,target->ai_addr,int(target->ai_addrlen));
            connect_error=connection==SOCKET_ERROR?WSAGetLastError():0;
            if(!connection)connected=true;
            else if(connect_error==WSAEWOULDBLOCK || connect_error==WSAEINPROGRESS)while(!stopping && milliseconds()-start<10000) {
                // Winsock reports failed nonblocking connects in exceptfds,
                // while successful connects become writable. Inspect SO_ERROR
                // for either outcome instead of waiting for the connect timeout.
                fd_set writes,errors;FD_ZERO(&writes);FD_ZERO(&errors);FD_SET(stream.socket,&writes);FD_SET(stream.socket,&errors);timeval wait{0,10000};
                const auto selected=select(0,nullptr,&writes,&errors,&wait);
                if(selected==SOCKET_ERROR){connect_error=WSAGetLastError();break;}
                if(selected>0){int error=0,size=sizeof(error);if(getsockopt(stream.socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)==SOCKET_ERROR)error=WSAGetLastError();connected=!error;connect_error=error;break;}
            }
            if(!connected){if(!connect_error)connect_error=WSAGetLastError();close_socket(stream.socket);}
        }
        freeaddrinfo(addresses);if(!connected){fail("Cannot connect to cooperative host: socket error "+std::to_string(connect_error));return;}
        socket_options(stream.socket);std::vector<uint8_t> hello;put64(hello,fingerprint);put_string(hello,pin);put_string(hello,name);hello.push_back(skin);stream.queue(packet(wire::Message::Hello,std::move(hello)));
        uint64_t ping=milliseconds();bool ok=true;
        while(!stopping && ok) {
            if(stream.transmit.empty()) {
                std::lock_guard lock(mutex);
                // Preserve control-command order, but do not make current
                // movement wait another socket iteration behind a WorldAck.
                if(!commands.empty()){stream.queue(std::move(commands.front()));commands.pop_front();}
                if(state.connected && sent_input!=input_sequence) {
                    std::vector<uint8_t> data;put64(data,world->epoch);put64(data,input_sequence);append_control(data,pending);
                    const auto input_frame=packet(wire::Message::Input,std::move(data));
                    stream.transmit.insert(stream.transmit.end(),input_frame.begin(),input_frame.end());
                    stream.last_progress=milliseconds();
                    sent_input=input_sequence;drain_transients(pending);
                } else if(stream.transmit.empty() && state.connected && milliseconds()-ping>=1000) {
                    ping=milliseconds();std::vector<uint8_t> data;put64(data,ping);stream.queue(packet(wire::Message::Ping,std::move(data)));
                }
            }
            fd_set reads,writes;FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(stream.socket,&reads);
            if(!stream.transmit.empty())FD_SET(stream.socket,&writes);
            // Local submissions do not make the network socket readable. A
            // 10ms idle poll therefore adds input delay even at a 1ms RTT.
            timeval wait{0,1000};auto selected=select(0,&reads,&writes,nullptr,&wait);if(selected==SOCKET_ERROR){ok=false;break;}
            uint64_t received=0,sent=0;if(FD_ISSET(stream.socket,&reads))ok=stream.read(received);
            if(ok)ok=stream.parse([&](auto type,const auto& bytes){return message(type,bytes);});
            if(ok && FD_ISSET(stream.socket,&writes))ok=stream.write(sent);
            {std::lock_guard lock(mutex);state.bytes_received+=received;state.bytes_sent+=sent;}
            if(milliseconds()-stream.last_receive>15000 || (!stream.transmit.empty() && milliseconds()-stream.last_progress>10000))ok=false;
        }
        close_socket(stream.socket);std::lock_guard lock(mutex);state.connecting=false;state.connected=false;
        if(stopping)state.message="Disconnected.";else if(state.message=="Connected to authoritative host." || state.message=="Connected; receiving authoritative world.")state.message="Host disconnected or stalled.";
        else if(state.message=="Connecting to authoritative host.")state.message="Connection closed before player assignment; check protocol and game data.";
    }
};
AuthoritativeClient::AuthoritativeClient():impl_(std::make_unique<Impl>()){}
AuthoritativeClient::~AuthoritativeClient(){stop();}
bool AuthoritativeClient::start(const std::string& address,uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& name,unsigned skin){return start_connection(address,port,pin,fingerprint,name,skin,false);}
bool AuthoritativeClient::start_lan(const std::string& address,uint16_t port,const std::string& ticket,uint64_t fingerprint,const std::string& name,unsigned skin){return start_connection(address,port,ticket,fingerprint,name,skin,true);}
bool AuthoritativeClient::start_connection(const std::string& address,uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& name,unsigned skin,bool lan) {
    auto& p=*impl_;if(p.worker.joinable() || !initialize_network() || !port || !(lan?valid_lan_secret(pin):pin_valid(pin)) || name.empty() || name.size()>64 || (skin!=255 && skin>10))return false;
    p.skin=uint8_t(skin);p.address=address;p.port=port;p.pin=pin;p.fingerprint=fingerprint;p.name=name;p.state={};p.world=std::make_shared<WorldState>();p.history.clear();p.motion_history.clear();p.look_history.reset(1);p.world_received=0;p.commands.clear();p.pending={};p.input_sequence=p.command_sequence=p.sent_input=0;p.state.port=port;p.state.connecting=true;p.state.message="Connecting to authoritative host.";p.stopping=false;
    p.worker=std::thread([&p]{p.run();});return true;
}
NetworkStatus AuthoritativeClient::status()const{std::lock_guard lock(impl_->mutex);return impl_->state;}
std::shared_ptr<const WorldState> AuthoritativeClient::latest_world()const{std::lock_guard lock(impl_->mutex);return impl_->world;}
AuthoritativeClient::WorldSample AuthoritativeClient::world_sample()const{std::lock_guard lock(impl_->mutex);return {impl_->world,impl_->world_received};}
std::vector<AuthoritativeClient::WorldSample> AuthoritativeClient::motion_samples(uint64_t after,uint64_t through)const{std::lock_guard lock(impl_->mutex);std::vector<WorldSample> out;for(const auto& s:impl_->motion_history)if(s.world->revision>after && s.world->revision<=through)out.push_back(s);return out;}
PredictedLook AuthoritativeClient::predicted_look(uint64_t epoch,uint64_t ack){
    std::lock_guard lock(impl_->mutex);
    const auto slot=impl_->state.player.slot;
    const auto confirmed=slot<max_players && impl_->world->epoch==epoch?impl_->world->input_ack[slot]:0;
    return impl_->look_history.pending(epoch,ack,std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),confirmed);
}
void AuthoritativeClient::clear_predicted_look(){std::lock_guard lock(impl_->mutex);impl_->look_history.reset(impl_->world->epoch);}
void AuthoritativeClient::ready(uint64_t epoch){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,epoch);impl_->enqueue(wire::Message::Ready,std::move(bytes));}
void AuthoritativeClient::lobby_ready(bool ready){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,impl_->world->epoch);bytes.push_back(ready);impl_->enqueue(wire::Message::LobbyReady,std::move(bytes));}
void AuthoritativeClient::submit_input(ControlInput input){if(!control_valid(input))return;std::lock_guard lock(impl_->mutex);merge_input(impl_->pending,input);++impl_->input_sequence;impl_->look_history.append(impl_->world->epoch,impl_->input_sequence,input.yaw,input.pitch,std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void AuthoritativeClient::set_afk(bool value){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,impl_->world->epoch);put64(bytes,++impl_->command_sequence);bytes.push_back(value);impl_->enqueue(wire::Message::Afk,std::move(bytes));}
void AuthoritativeClient::vote_skip(uint64_t scene){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,impl_->world->epoch);put64(bytes,++impl_->command_sequence);put64(bytes,scene);impl_->enqueue(wire::Message::Skip,std::move(bytes));}
void AuthoritativeClient::request_snapshot(){std::lock_guard lock(impl_->mutex);impl_->enqueue(wire::Message::SnapshotRequest);}
void AuthoritativeClient::acknowledge_native_events(uint64_t epoch,const NativeEventCursors& consumed){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,epoch);for(auto cursor:consumed)put64(bytes,cursor);impl_->enqueue(wire::Message::NativeEventsAck,std::move(bytes));}
void AuthoritativeClient::stop(){impl_->stopping=true;if(impl_->worker.joinable())impl_->worker.join();std::lock_guard lock(impl_->mutex);impl_->state.connecting=false;impl_->state.connected=false;}
}
