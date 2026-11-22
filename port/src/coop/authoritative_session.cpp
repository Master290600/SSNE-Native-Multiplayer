#include "authoritative_session.h"
#include "local_shot_feedback.h"
#include "ps2_content_identity.h"
#include "custom_map_session.h"
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
#include <bit>
#include <utility>

namespace port::coop {
namespace {
using Clock=std::chrono::steady_clock;
bool reuse_world_copies(){static const bool enabled=[] {const auto value=std::getenv("SSNE_WORLD_COPY_REUSE");return !value||std::strcmp(value,"0");}();return enabled;}
bool reuse_input_ack(){static const bool enabled=[] {const auto value=std::getenv("SSNE_INPUT_ACK_REUSE");return !value||std::strcmp(value,"0");}();return enabled;}
bool reuse_network_snapshot(){static const bool enabled=[] {const auto value=std::getenv("SSNE_NET_SNAPSHOT_REUSE");return !value||std::strcmp(value,"0");}();return enabled;}
bool use_network_world_view(){static const bool enabled=[] {const auto value=std::getenv("SSNE_NET_WORLD_VIEW");return !value||std::strcmp(value,"0");}();return enabled;}

uint64_t milliseconds(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
bool initialize_network(){static const bool ok=[](){WSADATA data{};return WSAStartup(MAKEWORD(2,2),&data)==0;}();return ok;}
bool pin_valid(const std::string& s){return s.size()==6 && std::all_of(s.begin(),s.end(),[](char c){return c>='0' && c<='9';});}
bool nonblocking(SOCKET s){u_long mode=1;return ioctlsocket(s,FIONBIO,&mode)==0;}
bool retry_socket(int error){return error==WSAEWOULDBLOCK || error==WSAEINPROGRESS;}
void close_socket(SOCKET& s){if(s!=INVALID_SOCKET){shutdown(s,SD_BOTH);closesocket(s);s=INVALID_SOCKET;}}
void socket_options(SOCKET s){BOOL yes=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&yes),sizeof(yes));}
void put32(std::vector<uint8_t>& b,uint32_t v){for(int i=3;i>=0;--i)b.push_back(uint8_t(v>>(i*8)));}
void put64(std::vector<uint8_t>& b,uint64_t v){put32(b,uint32_t(v>>32));put32(b,uint32_t(v));}
void put_string(std::vector<uint8_t>& b,const std::string& s){put32(b,uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
void put_point(std::vector<uint8_t>& b,const std::array<float,3>& point){for(float value:point)put32(b,std::bit_cast<uint32_t>(value));}
uint32_t read32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
struct Reader {
    const std::vector<uint8_t>& bytes;size_t offset=0;bool ok=true;
    uint8_t u8(){if(offset>=bytes.size()){ok=false;return 0;}return bytes[offset++];}
    uint32_t u32(){if(bytes.size()-offset<4){ok=false;return 0;}auto value=read32(bytes.data()+offset);offset+=4;return value;}
    uint64_t u64(){auto high=u32(),low=u32();return uint64_t(high)<<32|low;}
    std::string string(unsigned limit){auto n=u32();if(n>limit || n>bytes.size()-offset){ok=false;return {};}std::string s(bytes.begin()+offset,bytes.begin()+offset+n);offset+=n;return s;}
    bool done() const{return ok && offset==bytes.size();}
};
std::array<float,3> read_point(Reader& r){std::array<float,3> p;for(auto& value:p)value=std::bit_cast<float>(r.u32());return p;}
void append_ping_request(std::vector<uint8_t>& b,const TacticalPingRequest& ping){put64(b,ping.epoch);put_point(b,ping.position);put64(b,ping.target);b.push_back(uint8_t(ping.kind));}
TacticalPingRequest read_ping_request(Reader& r){TacticalPingRequest ping;ping.epoch=r.u64();ping.position=read_point(r);ping.target=r.u64();ping.kind=PingKind(r.u8());return ping;}
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
    std::string failure;
    bool fail(std::string reason){failure=std::move(reason);return false;}
    bool within_timeouts() {
        const auto now=milliseconds();
        if(now-last_receive>15000)return fail("No cooperative data received for 15 seconds.");
        if(!transmit.empty() && now-last_progress>10000)return fail("No cooperative send progress for 10 seconds.");
        return true;
    }
    bool read(uint64_t& total) {
        uint8_t buffer[65536];auto n=recv(socket,reinterpret_cast<char*>(buffer),sizeof(buffer),0);
        if(n==SOCKET_ERROR){const auto code=WSAGetLastError();return retry_socket(code) || fail("Cooperative receive socket error "+std::to_string(code)+".");}
        if(!n)return fail("Peer closed the connection.");
        total+=unsigned(n);last_receive=milliseconds();receive.insert(receive.end(),buffer,buffer+n);
        return receive.size()<=incoming_limit+wire::header_size+sizeof(buffer) || fail("Cooperative receive buffer exceeded its limit.");
    }
    template<class F> bool parse(F callback) {
        size_t consumed=0;unsigned frames=0;
        while(receive.size()-consumed>=wire::header_size && frames++<128) {
            const auto* p=receive.data()+consumed;
            if(read32(p)!=wire::magic || (uint16_t(p[4])<<8|p[5])!=wire::version)return fail("Cooperative frame header or protocol differs.");
            const auto type=wire::Message(uint16_t(p[6])<<8|p[7]);const auto size=read32(p+8);
            if(size>incoming_limit)return fail("Cooperative frame exceeded its size limit.");
            if(receive.size()-consumed<wire::header_size+size)break;
            std::vector<uint8_t> body(p+wire::header_size,p+wire::header_size+size);
            if(!callback(type,body)) {
                if(failure.empty())failure="Rejected cooperative message type "+std::to_string(uint16_t(type))+".";
                return false;
            }
            consumed+=wire::header_size+size;
        }
        if(consumed)receive.erase(receive.begin(),receive.begin()+consumed);return true;
    }
    bool write(uint64_t& total) {
        if(transmit.empty())return true;
        auto n=send(socket,reinterpret_cast<const char*>(transmit.data()+sent),int(transmit.size()-sent),0);
        if(n==SOCKET_ERROR){const auto code=WSAGetLastError();return retry_socket(code) || fail("Cooperative send socket error "+std::to_string(code)+".");}
        if(!n)return fail("Peer closed the connection.");
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
bool selected_custom_map(const WorldState& world,const CustomMapRequest& request) {
    if(world.epoch!=request.epoch || selected_custom_map_alias(world)!=request.alias)return false;
    const auto string_is=[&](const char* key,const std::string& value){const auto it=world.mission.find(key);return it!=world.mission.end() && std::get_if<std::string>(&it->second) && std::get<std::string>(it->second)==value;};
    return string_is("Session.Custom.Id",request.id) && string_is("Session.Custom.Alias",request.alias) && string_is("Session.Custom.SHA256",request.map_sha256);
}
bool lobby_setup_changed(const Properties& before,const Properties& after){
    for(const char* key:{"Session.Lobby.Level","Session.Lobby.Mode","Session.Lobby.LevelSet","Session.Lobby.Difficulty","Session.EnemyMultiplier","Session.AmmoCapacity","Session.AmmoPickupMultiplier"}){
        const auto a=before.find(key),b=after.find(key);
        if((a==before.end())!=(b==after.end()) || (a!=before.end() && a->second!=b->second))return true;
    }
    return false;
}
bool transfer_active(CustomMapTransferPhase phase){return phase==CustomMapTransferPhase::Requesting || phase==CustomMapTransferPhase::Receiving || phase==CustomMapTransferPhase::Verifying;}
}

struct AuthoritativeServer::Impl {
    struct InputState { ControlInput input;uint64_t sequence=0,received=0,ack=0,fire_pressed=0; };
    // Every transmitted baseline retains its immutable body and its own event
    // overlay. Acknowledging another revision must not rebind old peer cursors.
    struct PeerSnapshot {
        std::shared_ptr<const WorldState> body;
        std::optional<Properties> mission;
        explicit PeerSnapshot(std::shared_ptr<const WorldState> source):body(std::move(source)){}
        PeerSnapshot(std::shared_ptr<const WorldState> source,Properties overlay):body(std::move(source)),mission(std::move(overlay)){}
        uint64_t revision()const{return body->revision;}
        WorldReadView view()const{return WorldReadView(*body,mission?*mission:body->mission);}
    };
    // The local invitation socket can accept data much faster than the far
    // endpoint. Limit actual unacknowledged work. Small deltas can pipeline
    // across an ordinary RTT, but one large update must drain on its own.
    static constexpr size_t world_window_count=4,world_window_bytes=8*1024;
    struct SentWorld {
        std::shared_ptr<const PeerSnapshot> snapshot;
        size_t bytes=0;
        uint64_t sent_at=0;
    };
    struct Peer {
        Stream stream;std::string address;PlayerId id;bool assigned=false,reject=false,force_full=true,prefer_chat=true,demand_counted=false;
        std::deque<std::vector<uint8_t>> priority;
        std::shared_ptr<const PeerSnapshot> baseline;
        SentWorld in_flight;
        std::map<uint64_t,SentWorld> sent_worlds;
        size_t pending_world_bytes=0;
        uint64_t world_stats_at=0,last_world_ack_ms=0;
        uint64_t world_frames_sent=0,world_bytes_sent=0,world_revisions_acked=0;
        uint64_t awaiting_full_revision=0;
        uint64_t last_state=0,chat_cursor=0,last_telemetry=0,last_probe=0,probe_token=0;
        bool prefer_telemetry=true;
        struct MapTransfer {uint64_t token=0,offset=0,started=0;CustomMapRequest request;std::shared_ptr<const CustomMapPackage> package;bool offered=false,cached=false;};
        MapTransfer map_transfer;uint64_t latest_map_token=0;bool prefer_map=false;
    };
    mutable std::recursive_mutex mutex;
    SessionAuthority authority;NetworkStatus state;ChatLog chat;
    TacticalPingLog tactical_pings;
    std::array<PlayerLatency,max_players> latencies{};
    std::shared_ptr<const WorldState> local_ping_view;
    std::shared_ptr<const WorldState> body=std::make_shared<WorldState>(),published;
    std::shared_ptr<const CustomMapPackage> custom_map_package;uint64_t custom_map_epoch=0;
    uint64_t custom_map_failed_epoch=0;std::string custom_map_failed_alias,custom_map_failure;
    std::array<InputState,max_players> controls{};
    std::array<PlayerId,max_players> reported{};
    std::array<LobbySkinChoice,max_players> skin_choices{};
    struct EventConsumer {PlayerId player{};uint64_t epoch=0;NativeEventCursors consumed{},offered{},initial_floor{};bool initialized=false,offered_initial=false;};
    NativeEventJournal native_events;
    std::array<EventConsumer,max_players> event_consumers{};
    uint64_t revision=0,fingerprint=0;
    unsigned tcp_publication_peers=0;uint64_t publication_serial=1;
    std::array<PlayerId,max_players> bots{};
    std::array<unsigned,max_players> bot_skills{};
    unsigned match_player_limit=8;
    bool lobby_open=true;
    uint8_t lobby_ready_mask=0,spectator_lock_mask=0;
    uint64_t lobby_ready_token=1;
    std::array<PlayerId,max_players> lobby_ready_owners{};
    std::string pin,lan_id;
    bool lan_join_enabled=true;
    struct LanPermit {std::string peer;uint32_t nonce=0;uint64_t expires=0;};
    std::map<std::string,LanPermit> lan_permits;
    SOCKET listener=INVALID_SOCKET;std::thread worker;std::atomic<bool> stopping{false};
    std::vector<Peer> peers;
    std::function<void(const std::string&)> disconnect_log;
    const bool trace_world_queue=[] {
        const char* value=std::getenv("SSNE_COOP_WORLD_STATS");
        return value && std::strcmp(value,"1")==0;
    }();
    Impl(unsigned capacity,std::function<void(const std::string&)> log):authority(capacity),disconnect_log(std::move(log)){state.capacity=capacity;rebuild();}
    std::array<Participant,max_players> effective_participants()const{
        auto result=authority.participants();for(unsigned i=0;i<max_players;++i)if(result[i].connected)result[i].spectator=result[i].spectator || (spectator_lock_mask&(1u<<i));return result;
    }
    uint8_t voluntary_spectators()const{uint8_t mask=0;for(unsigned i=0;i<max_players;++i)if(authority.participants()[i].connected && authority.participants()[i].spectator)mask|=uint8_t(1u<<i);return mask;}
    bool host_has_human_peer()const{
        for(const auto& person:authority.participants())if(person.connected && person.id!=state.player && bots[person.id.slot]!=person.id)return true;
        return false;
    }
    void reset_lobby_votes(){if(!++lobby_ready_token)++lobby_ready_token;lobby_ready_mask=0;}
    void rebuild(std::shared_ptr<WorldState> unpublished={}) {
        // The last human peer can leave while the host is watching. Return that
        // host to gameplay; cancel_spectator_activation preserves forced locks.
        if(!host_has_human_peer())if(const auto* host=authority.participant(state.player);host && host->spectator)
            if(authority.cancel_spectator_activation(state.player)){controls[state.player.slot].input={};++publication_serial;}
        // Only publish() supplies a newly allocated, unescaped world. The body
        // holds its other reference. Published/retained snapshots remain const.
        const bool exclusive=unpublished && unpublished.use_count()==2 && unpublished.get()==body.get() && unpublished.get()!=published.get();
        auto world=exclusive?std::move(unpublished):std::make_shared<WorldState>(*body);world->revision=++revision;
        // A socket can replace an auto-ready bot in the same locked operation.
        // Readiness belongs to the authenticated generation, never just a slot.
        for(unsigned i=0;i<max_players;++i){const auto& person=authority.participants()[i];const auto owner=person.connected?person.id:PlayerId{};if(lobby_ready_owners[i]!=owner){lobby_ready_mask&=uint8_t(~(1u<<i));lobby_ready_owners[i]=owner;}}
        uint8_t bot_mask=0;std::vector<int64_t> skills(max_players,0);
        for(unsigned i=1;i<max_players;++i)if(bots[i].generation && authority.participant(bots[i])){authority.automate(bots[i]);bot_mask|=uint8_t(1u<<i);skills[i]=bot_skills[i];if(lobby_open)lobby_ready_mask|=uint8_t(1u<<i);}
        world->mission["Session.Bots.Mask"]=int64_t(bot_mask);world->mission["Session.Bots.Skills"]=std::move(skills);
        authority.set_spectator_lock(spectator_lock_mask);spectator_lock_mask=authority.spectator_lock_mask();
        world->players=effective_participants();world->vote=authority.skip_vote();
        for(const auto& person:world->players)if(person.connected){
            const auto actor=world->entities.find(person.character);
            if(!person.ready || person.afk || person.spectator || actor==world->entities.end() || !tactical_ping_alive(actor->second))tactical_pings.remove(person.id);
        }
        world->mission["Session.Spectators.VoluntaryMask"]=int64_t(voluntary_spectators());
        world->mission["Session.Spectators.LockMask"]=int64_t(spectator_lock_mask);
        uint8_t connected=0;for(unsigned i=0;i<max_players;++i)if(world->players[i].connected)connected|=uint8_t(1u<<i);
        lobby_ready_mask&=connected;
        world->mission["Session.Lobby.Open"]=int64_t(lobby_open);
        world->mission["Session.Lobby.ReadyMask"]=int64_t(lobby_ready_mask);
        world->mission["Session.Lobby.ReadyToken"]=std::bit_cast<int64_t>(lobby_ready_token);
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
    std::array<PlayerLatency,max_players> measured_latencies()const{
        auto result=latencies;
        for(unsigned slot=0;slot<max_players;++slot){
            const auto& person=authority.participants()[slot];
            if(!person.connected || result[slot].id!=person.id)result[slot]={person.connected?person.id:PlayerId{},0,false};
            if(person.connected && person.id==state.player)result[slot]={person.id,0,true};
            if(person.connected && bots[slot]==person.id)result[slot]={person.id,0,false};
        }
        return result;
    }
    bool accept_ping(PlayerId id,const TacticalPingRequest& request){
        const auto people=effective_participants();
        return state.connected && !stopping && id.slot<people.size() && people[id.slot].id==id &&
            tactical_pings.send(people[id.slot],request,*body,milliseconds());
    }
    std::shared_ptr<const WorldState> small_local_ping_view(const WorldState& view,EntityId target)const{
        if(!state.connected || stopping || tcp_publication_peers || view.epoch!=body->epoch || view.level!=body->level ||
           !tactical_ping_gameplay(view) || !tactical_ping_gameplay(*body))return {};
        const auto* owner=authority.participant(state.player);
        if(!owner || !owner->ready || owner->afk || owner->spectator || (spectator_lock_mask&(1u<<owner->id.slot)) ||
           view.players[owner->id.slot].id!=owner->id || view.players[owner->id.slot].character!=owner->character)return {};
        const auto actor=view.entities.find(owner->character);
        if(actor==view.entities.end() || actor->second.kind!=EntityKind::Player || actor->second.owner!=owner->id)return {};
        auto out=std::make_shared<WorldState>();out->epoch=view.epoch;out->level=view.level;out->mission["Session.NativePhase"]=std::string("gameplay");out->players=effective_participants();
        // Retain no borrowed aliases or bone/mission payload. Only the chosen
        // target and host's tiny eligibility/position records survive this call.
        const auto copy=[&](const EntityState& source){
            EntityState entity;entity.id=source.id;entity.kind=source.kind;entity.owner=source.owner;
            for(const auto* key:{"position","health","pickup_view_mask","pickup_ownership"}){
                const auto it=source.properties.find(key);if(it==source.properties.end())continue;
                if(const auto* array=std::get_if<std::vector<int64_t>>(&it->second);array && array->size()>max_players)return false;
                entity.properties[key]=it->second;
            }
            out->entities.emplace(entity.id,std::move(entity));return true;
        };
        if(!copy(actor->second))return {};
        if(target)if(const auto found=view.entities.find(target);found!=view.entities.end() && target!=owner->character)if(!copy(found->second))return {};
        return out;
    }
    void finish_world_send(Peer& peer) {
        if(!peer.in_flight.snapshot || !peer.stream.transmit.empty())return;
        if(trace_world_queue){
            ++peer.world_frames_sent;
            peer.world_bytes_sent+=peer.in_flight.bytes;
        }
        auto& sent=peer.sent_worlds[peer.in_flight.snapshot->revision()];
        peer.pending_world_bytes+=peer.in_flight.bytes;
        peer.in_flight.bytes+=sent.bytes;
        peer.in_flight.sent_at=sent.snapshot?sent.sent_at:milliseconds();
        sent=std::exchange(peer.in_flight,{});
    }
    void drop(Peer& peer) {
        close_socket(peer.stream.socket);
        std::lock_guard lock(mutex);
        if(peer.demand_counted){if(tcp_publication_peers)--tcp_publication_peers;peer.demand_counted=false;++publication_serial;}
        local_ping_view.reset();tactical_pings.remove(state.player);
        if(peer.assigned){tactical_pings.remove(peer.id);if(latencies[peer.id.slot].id==peer.id)latencies[peer.id.slot]={};if(authority.leave(peer.id)){controls[peer.id.slot]={};rebuild();}peer.assigned=false;}
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
            if(!r.done() || !valid_skin_choice(skin) || content!=fingerprint || (!pin_join && !lan_join))rejection="PIN, game data or cooperative protocol differs.";
            if(rejection.empty() && !chat_name_valid(name))rejection="Invalid player name.";
            if(rejection.empty() && authority.connected_count()>=match_player_limit)for(unsigned i=match_player_limit;i-->1;)if(bots[i].generation && authority.participant(bots[i])){authority.leave(bots[i]);spectator_lock_mask&=uint8_t(~(1u<<i));bots[i]={};controls[i]={};break;}
            if(rejection.empty() && authority.connected_count()>=match_player_limit)rejection="The selected game mode is full (maximum "+std::to_string(match_player_limit)+" players).";
            auto id=rejection.empty()?authority.join(name):std::nullopt;
            if(!id && rejection.empty())rejection="Session is full or player name is invalid.";
            if(!id){std::vector<uint8_t> data;put_string(data,rejection);peer.priority.push_back(packet(wire::Message::Rejected,std::move(data)));peer.reject=true;return true;}
            if(lan_join)lan_permits.erase(permit); // Consume only after a successful player allocation.
            peer.id=*id;peer.assigned=true;++publication_serial;skin_choices[peer.id.slot]={peer.id,uint8_t(skin)};peer.stream.incoming_limit=512;controls[id->slot]={};event_consumers[id->slot]={};event_consumers[id->slot].player=*id;rebuild();
            std::vector<uint8_t> assigned;put32(assigned,id->slot);put64(assigned,id->generation);put32(assigned,state.capacity);
            peer.priority.push_back(packet(wire::Message::Assigned,std::move(assigned)));return true;
        }
        // The connection's identity is authoritative. No gameplay state, target
        // player ID, spawn, damage, mission or transform message is accepted here.
        std::lock_guard lock(mutex);
        if(!authority.participant(peer.id))return false;
        switch(type) {
        case wire::Message::CustomMapRequest: {
            const auto token=r.u64();CustomMapRequest request;request.epoch=r.u64();request.alias=r.string(31);request.id=r.string(48);request.map_sha256=r.string(64);request.known_package_sha256=r.string(64);
            std::string error;if(!r.done() || !token || !custom_map_request_valid(request,error))return false;
            if(token<=peer.latest_map_token)return true;
            peer.latest_map_token=token;
            // An in-flight request may refer to a proposal already replaced
            // by the host. Its next world snapshot drives cancellation; this
            // ordinary race must not display a fatal download error or retire
            // a newer, still-current transfer on the same socket.
            if(request.epoch!=published->epoch || request.alias!=selected_custom_map_alias(*published))return true;
            peer.map_transfer={};peer.map_transfer.token=token;peer.map_transfer.request=std::move(request);peer.map_transfer.started=milliseconds();peer.prefer_map=true;return true;
        }
        case wire::Message::CustomMapCancel: {
            const auto token=r.u64(),epoch=r.u64();if(!r.done() || !token)return false;
            if(token==peer.map_transfer.token && epoch==peer.map_transfer.request.epoch)peer.map_transfer={};return true;
        }
        case wire::Message::TacticalPing: {
            const auto request=read_ping_request(r);if(!r.done())return false;
            // A well-formed rejected marker never disconnects ordinary play.
            accept_ping(peer.id,request);return true;
        }
        case wire::Message::Chat: {
            const auto text=r.string(unsigned(chat_text_bytes));if(!r.done())return false;
            // The socket, never the payload, chooses the displayed player/name.
            // Invalid or rate-limited chat is ignored without affecting play.
            chat.send(*authority.participant(peer.id),text,milliseconds());return true;
        }
        case wire::Message::LobbyReady: {
            const auto token=r.u64();const auto value=r.u8();if(!r.done() || value>1)return false;
            if(token!=lobby_ready_token || !lobby_open)return true;
            const auto mask=uint8_t(1u<<peer.id.slot);
            const auto next=uint8_t(value?lobby_ready_mask|mask:lobby_ready_mask&~mask);
            if(next!=lobby_ready_mask){lobby_ready_mask=next;rebuild();}return true;
        }
        case wire::Message::Input: {
            const auto epoch=r.u64(),sequence=r.u64();auto input=read_control(r);
            const auto fire_pressed=r.done()?0:r.u64();
            if(!r.done() || !sequence || !control_valid(input) || fire_pressed>sequence)return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch)return false;
            auto& old=controls[peer.id.slot];if(sequence<=old.sequence)return true;
            old.sequence=sequence;old.received=milliseconds();old.fire_pressed=fire_pressed;merge_input(old.input,input);return true;
        }
        case wire::Message::Ready: {
            auto epoch=r.u64();if(!r.done())return false;
            auto character=authority.participant(peer.id)->character;
            if(epoch==body->epoch && body->entities.contains(character) && body->entities.at(character).owner==peer.id && authority.finish_join(peer.id)){++publication_serial;rebuild();}return true;
        }
        case wire::Message::Afk: {
            const auto epoch=r.u64(),sequence=r.u64();auto value=r.u8();if(!r.done() || value>1)return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch)return false;
            if(authority.request_afk(peer.id,sequence,value!=0,results_active())==CommandResult::Applied){controls[peer.id.slot].input={};++publication_serial;rebuild();}return true;
        }
        case wire::Message::Spectator: {
            const auto epoch=r.u64(),sequence=r.u64();const auto value=r.u8();if(!r.done() || value>1)return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch)return false;
            if(authority.request_spectator(peer.id,sequence,value!=0,results_active())==CommandResult::Applied){controls[peer.id.slot].input={};++publication_serial;rebuild();}return true;
        }
        case wire::Message::Skip: {
            const auto epoch=r.u64(),sequence=r.u64(),scene=r.u64();if(!r.done())return false;
            if(epoch<body->epoch)return true;if(epoch!=body->epoch)return false;
            if(authority.request_skip(peer.id,sequence,scene)==CommandResult::Applied)rebuild();return true;
        }
        case wire::Message::SnapshotRequest:
            if(!r.done())return false;
            if(peer.awaiting_full_revision){
                // Older clients ask again instead of ACKing a full snapshot
                // for a revision they already acknowledged. Its baseline is
                // usable and we can stop this duplicate request/reply cycle.
                if(peer.baseline && peer.awaiting_full_revision==peer.baseline->revision()){
                    const auto sent=peer.sent_worlds.find(peer.awaiting_full_revision);
                    if(sent!=peer.sent_worlds.end()){
                        peer.pending_world_bytes-=sent->second.bytes;
                        peer.sent_worlds.erase(sent);
                        peer.awaiting_full_revision=0;
                    }
                }
            }else if(!peer.force_full){peer.force_full=true;++publication_serial;}
            return true;
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
            const auto found=peer.sent_worlds.find(revision);
            if(found==peer.sent_worlds.end())return (peer.baseline && revision<=peer.baseline->revision()) ||
                peer.stream.fail("Acknowledgement for an unsent cooperative world revision.");
            if(trace_world_queue && (!peer.baseline || revision>peer.baseline->revision()))++peer.world_revisions_acked;
            peer.baseline=found->second.snapshot;
            peer.last_world_ack_ms=milliseconds()-found->second.sent_at;
            const auto end=std::next(found);
            for(auto it=peer.sent_worlds.begin();it!=end;++it)peer.pending_world_bytes-=it->second.bytes;
            peer.sent_worlds.erase(peer.sent_worlds.begin(),end);
            if(revision>=peer.awaiting_full_revision)peer.awaiting_full_revision=0;
            return true;
        }
        case wire::Message::Ping:{auto time=r.u64();if(!r.done())return false;std::vector<uint8_t> data;put64(data,time);if(peer.priority.size()<32)peer.priority.push_back(packet(wire::Message::Pong,std::move(data)));return true;}
        case wire::Message::Pong:{
            const auto token=r.u64();if(!r.done())return false;const auto now=milliseconds();
            if(peer.probe_token && token==peer.probe_token && now>=peer.last_probe && now-peer.last_probe<=60000){
                latencies[peer.id.slot]={peer.id,unsigned(now-peer.last_probe),true};peer.probe_token=0;
            }
            return true;
        }
        default:return false;
        }
    }
    bool queue_chat(Peer& peer) {
        if(!peer.assigned || peer.reject)return false;
        std::lock_guard lock(mutex);if(!authority.participant(peer.id))return false;
        const auto messages=chat.messages();size_t count=0;
        for(const auto& message:messages)if(message.sequence>peer.chat_cursor)++count;
        if(!count)return false;
        std::vector<uint8_t> bytes;put32(bytes,uint32_t(count));
        for(const auto& message:messages)if(message.sequence>peer.chat_cursor) {
            put64(bytes,message.sequence);put32(bytes,message.sender.slot);put64(bytes,message.sender.generation);
            put_string(bytes,message.name);put_string(bytes,message.text);peer.chat_cursor=message.sequence;
        }
        // A World frame must remain the only frame in its send buffer: its
        // acknowledgement may arrive before a following Chat frame is drained.
        // Only empty streams enter this scheduler, so queue() cannot overwrite
        // any partial transmission or delay the WorldAck completion boundary.
        peer.stream.queue(packet(wire::Message::ChatHistory,std::move(bytes)));return true;
    }
    bool queue_telemetry(Peer& peer){
        const auto now=milliseconds();if(!peer.assigned || peer.reject || !peer.baseline || now-peer.last_telemetry<500)return false;
        std::lock_guard lock(mutex);if(!authority.participant(peer.id))return false;
        std::vector<uint8_t> bytes;put64(bytes,body->epoch);
        const auto people=effective_participants();const auto measured=measured_latencies();
        unsigned count=0;for(const auto& person:people)count+=person.connected;put32(bytes,count);
        for(unsigned slot=0;slot<max_players;++slot)if(people[slot].connected){const auto& value=measured[slot];put32(bytes,slot);put64(bytes,people[slot].id.generation);bytes.push_back(value.known);put32(bytes,value.ping_ms);}
        const auto markers=tactical_pings.messages(*body,people,now);put32(bytes,unsigned(markers.size()));
        for(const auto& ping:markers){
            put64(bytes,ping.sequence);put32(bytes,ping.sender.slot);put64(bytes,ping.sender.generation);put_string(bytes,ping.name);
            put_point(bytes,ping.position);put64(bytes,ping.target);bytes.push_back(uint8_t(ping.kind));put32(bytes,ping.remaining_ms);
        }
        peer.stream.queue(packet(wire::Message::SessionTelemetry,std::move(bytes)));peer.last_telemetry=now;return true;
    }
    bool queue_custom_map(Peer& peer) {
        auto& transfer=peer.map_transfer;if(!peer.assigned || peer.reject || !transfer.token)return false;
        std::string error;
        {std::lock_guard lock(mutex);
            if(transfer.request.epoch!=published->epoch || transfer.request.alias!=selected_custom_map_alias(*published)){transfer={};return false;}
            if(!selected_custom_map(*published,transfer.request))error="The selected custom level identity does not match.";
            else if(custom_map_failed_epoch==transfer.request.epoch && custom_map_failed_alias==transfer.request.alias)error=custom_map_failure;
            else if(!transfer.package && custom_map_package && custom_map_epoch==transfer.request.epoch && custom_map_request_matches(transfer.request,custom_map_package->manifest))transfer.package=custom_map_package;
            else if(!transfer.package && milliseconds()-transfer.started>30000)error="Host custom map package is unavailable.";
        }
        std::vector<uint8_t> bytes;put64(bytes,transfer.token);put64(bytes,transfer.request.epoch);
        if(!error.empty()){put_string(bytes,error);peer.stream.queue(packet(wire::Message::CustomMapError,std::move(bytes)));transfer={};return true;}
        if(!transfer.package)return false;
        const auto& package=*transfer.package;
        if(!transfer.offered){
            const auto manifest=encode_custom_map_manifest(package.manifest);bytes.insert(bytes.end(),manifest.begin(),manifest.end());
            transfer.cached=transfer.request.known_package_sha256==package.manifest.package_sha256;transfer.offered=true;
            peer.stream.queue(packet(wire::Message::CustomMapOffer,std::move(bytes)));return true;
        }
        const auto total=package.manifest.map_bytes+package.manifest.preview_bytes;
        if(transfer.cached || transfer.offset==total){bytes.push_back(transfer.cached);peer.stream.queue(packet(wire::Message::CustomMapComplete,std::move(bytes)));transfer={};return true;}
        put64(bytes,transfer.offset);const auto count=std::min<uint64_t>(custom_map_chunk_bytes,total-transfer.offset);
        const auto map_count=transfer.offset<package.manifest.map_bytes?std::min<uint64_t>(count,package.manifest.map_bytes-transfer.offset):0;
        if(map_count)bytes.insert(bytes.end(),package.map->begin()+size_t(transfer.offset),package.map->begin()+size_t(transfer.offset+map_count));
        if(map_count<count){const auto preview_offset=transfer.offset+map_count-package.manifest.map_bytes;bytes.insert(bytes.end(),package.preview->begin()+size_t(preview_offset),package.preview->begin()+size_t(preview_offset+count-map_count));}
        transfer.offset+=count;peer.stream.queue(packet(wire::Message::CustomMapChunk,std::move(bytes)));return true;
    }
    void run() {
        while(!stopping) {
            fd_set reads,writes;FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(listener,&reads);
            for(auto& peer:peers){FD_SET(peer.stream.socket,&reads);FD_SET(peer.stream.socket,&writes);}
            timeval timeout{0,1000};auto selected=select(0,&reads,&writes,nullptr,&timeout);if(selected==SOCKET_ERROR){std::lock_guard lock(mutex);state.message="Cooperative host socket polling failed: "+std::to_string(WSAGetLastError());break;}
            if(FD_ISSET(listener,&reads))for(unsigned attempts=0;attempts<16;++attempts) {
                sockaddr_in remote{};int length=sizeof(remote);auto socket=accept(listener,reinterpret_cast<sockaddr*>(&remote),&length);if(socket==INVALID_SOCKET)break;
                char address[INET_ADDRSTRLEN]{};
                if(peers.size()>=16 || !nonblocking(socket) || !inet_ntop(AF_INET,&remote.sin_addr,address,sizeof(address))){closesocket(socket);continue;}socket_options(socket);Peer peer;peer.address=address;peer.stream.socket=socket;peer.stream.incoming_limit=256;{std::lock_guard lock(mutex);++tcp_publication_peers;++publication_serial;local_ping_view.reset();tactical_pings.remove(state.player);peer.demand_counted=true;}peers.push_back(std::move(peer));
            }
            for(size_t i=0;i<peers.size();) {
                auto& peer=peers[i];bool ok=true;uint64_t received=0,sent=0;
                if(FD_ISSET(peer.stream.socket,&reads))ok=peer.stream.read(received);
                if(ok)ok=peer.stream.parse([&](auto type,const auto& bytes){return message(peer,type,bytes);});
                // Measure every authenticated remote connection at the host.
                // Echo tokens are bound to that socket and a pending probe.
                if(ok && peer.assigned && !peer.reject && peer.baseline && milliseconds()-peer.last_probe>=1000 &&
                   (!peer.probe_token || milliseconds()-peer.last_probe>=5000) && peer.priority.size()<32){
                    peer.last_probe=milliseconds();peer.probe_token=peer.last_probe;
                    std::vector<uint8_t> data;put64(data,peer.probe_token);peer.priority.push_back(packet(wire::Message::Ping,std::move(data)));
                }
                if(ok && peer.stream.transmit.empty()) {
                    if(!peer.priority.empty()){peer.stream.queue(std::move(peer.priority.front()));peer.priority.pop_front();}
                    else if(peer.reject)ok=false;
                    else if(peer.assigned && peer.prefer_chat && queue_chat(peer))peer.prefer_chat=false;
                    else if(peer.assigned && peer.prefer_telemetry && queue_telemetry(peer))peer.prefer_telemetry=false;
                    else if(peer.assigned && peer.prefer_map && queue_custom_map(peer))peer.prefer_map=false;
                    else if(peer.assigned && !peer.awaiting_full_revision && milliseconds()-peer.last_state>=16) {
                        std::shared_ptr<const PeerSnapshot> current;
                        bool full=false,retry=false;
                        std::optional<EventConsumer> offered_events;
                        {std::lock_guard lock(mutex);
                            // The invitation relay accepts writes on a local socket
                            // before they reach the remote game. Pace snapshots by
                            // application acknowledgements to keep that queue bounded.
                            full=peer.force_full || !peer.baseline || published->epoch!=peer.baseline->body->epoch;
                            const bool pending=peer.sent_worlds.contains(published->revision);
                            // Recover a missing final delta without filling a slow
                            // reliable route with duplicates on every send tick.
                            retry=pending && milliseconds()-peer.last_state>=1000;
                            const bool window_open=(pending || peer.sent_worlds.size()<world_window_count) &&
                                                   peer.pending_world_bytes<world_window_bytes;
                            const bool unchanged=reuse_network_snapshot() && pending && !retry;
                            const bool send_world=full || (window_open && !unchanged && published->revision!=peer.baseline->revision());
                            if(send_world && native_events.active() && native_events.epoch()==published->epoch) {
                                auto consumer=event_consumers[peer.id.slot];
                                if(consumer.epoch!=published->epoch){consumer.epoch=published->epoch;consumer.consumed={};consumer.offered={};consumer.initialized=false;consumer.offered_initial=false;consumer.initial_floor={};}
                                if(consumer.initialized && !native_events.available(consumer.consumed)){ok=peer.stream.fail("Cooperative native event history exhausted.");}
                                else {
                                    if(use_network_world_view()){
                                        // Journal decoration reads only epoch and writes mission.
                                        // Its row pointers stay under mutex; the resulting arrays
                                        // own their bytes. Never copy the entity/bone graph here.
                                        WorldState overlay;overlay.epoch=published->epoch;overlay.mission=published->mission;
                                        native_events.decorate(overlay,consumer.consumed,consumer.initialized);
                                        current=std::make_shared<PeerSnapshot>(published,std::move(overlay.mission));
                                    }else{
                                        auto customized=std::make_shared<WorldState>(*published);
                                        native_events.decorate(*customized,consumer.consumed,consumer.initialized);
                                        current=std::make_shared<PeerSnapshot>(std::move(customized));
                                    }
                                    const auto decorated=current->view();const auto tails=native_events.tails();
                                    if(!consumer.initialized){consumer.offered=tails;if(!consumer.offered_initial){consumer.initial_floor=tails;consumer.offered_initial=true;}}
                                    else {
                                        const char* prefixes[]={"Session.ConfirmedHits.Ids","Session.RocketEffects.Ids","Session.WeaponShots.Ids","Session.WeaponLoops.Ids","Session.BulletImpacts.Ids","Session.Feedback.Ids","Session.BulletTrails.Ids","Session.WeaponEmitters.Ids","Session.WeaponCasings.Ids","Session.ProjectileVisuals.Ids","Session.WeaponSurfaces.Ids","Session.WeaponBeams.Ids"};
                                        for(unsigned n=0;n<native_event_channels;++n){const auto& ids=std::get<std::vector<int64_t>>(decorated.mission.at(prefixes[n]));if(!ids.empty())consumer.offered[n]=std::max(consumer.offered[n],uint64_t(ids[ids.size()-(n==2?2:1)]));}
                                    }
                                    offered_events=std::move(consumer);
                                }
                            }else if(send_world)current=std::make_shared<PeerSnapshot>(published);
                        }
                        if(ok && current && (full || retry || !peer.sent_worlds.contains(current->revision()))) {
                            try{
                                std::optional<WorldReadView> before;
                                if(!full) {
                                    // This connection delivers frames in order. A new revision
                                    // can build on the last complete frame already sent. Using
                                    // only the latest ACK would repeat the same changes during
                                    // every RTT. Resends and resyncs keep the confirmed base.
                                    const auto& base=(!retry && !peer.sent_worlds.empty())
                                        ?peer.sent_worlds.rbegin()->second.snapshot:peer.baseline;
                                    before.emplace(base->view());
                                }
                                auto frame=packet(wire::Message::World,encode_delta(difference(before?&*before:nullptr,current->view())));
                                peer.last_state=milliseconds();
                                if(full || !peer.pending_world_bytes || frame.size()<=world_window_bytes-peer.pending_world_bytes){
                                    if(offered_events){std::lock_guard lock(mutex);event_consumers[peer.id.slot]=std::move(*offered_events);}
                                    if(full)peer.awaiting_full_revision=current->revision();
                                    peer.in_flight={std::move(current),frame.size()};
                                    peer.stream.queue(std::move(frame));
                                    peer.force_full=false;peer.prefer_chat=true;peer.prefer_telemetry=true;peer.prefer_map=true;
                                }
                            }
                            catch(const std::exception&){ok=peer.stream.fail("Could not encode the cooperative world snapshot.");}
                        }
                    }
                    if(ok && peer.stream.transmit.empty() && queue_chat(peer))peer.prefer_chat=false;
                    if(ok && peer.stream.transmit.empty() && queue_telemetry(peer))peer.prefer_telemetry=false;
                    if(ok && peer.stream.transmit.empty() && queue_custom_map(peer))peer.prefer_map=false;
                }
                if(ok && FD_ISSET(peer.stream.socket,&writes)){ok=peer.stream.write(sent);if(ok)finish_world_send(peer);}
                if(trace_world_queue && peer.assigned && disconnect_log) {
                    const auto now=milliseconds();
                    if(now-peer.world_stats_at>=1000) {
                        peer.world_stats_at=now;
                        const auto oldest=peer.sent_worlds.empty()?0:now-peer.sent_worlds.begin()->second.sent_at;
                        disconnect_log("[coop-world-queue] slot="+std::to_string(peer.id.slot)+
                            " pending_worlds="+std::to_string(peer.sent_worlds.size())+" pending_bytes="+std::to_string(peer.pending_world_bytes)+
                            " oldest_ms="+std::to_string(oldest)+" last_world_ack_ms="+std::to_string(peer.last_world_ack_ms)+
                            " sent_frames="+std::to_string(peer.world_frames_sent)+" sent_bytes="+std::to_string(peer.world_bytes_sent)+
                            " acked_revisions="+std::to_string(peer.world_revisions_acked)+" steady_ms="+std::to_string(now));
                    }
                }
                {std::lock_guard lock(mutex);state.bytes_received+=received;state.bytes_sent+=sent;}
                if(ok)ok=peer.stream.within_timeouts();
                if(!ok){
                    if(peer.assigned && disconnect_log)
                        disconnect_log("[native-session-peer-drop] slot="+std::to_string(peer.id.slot)+" reason="+peer.stream.failure);
                    drop(peer);peers.erase(peers.begin()+i);
                }else ++i;
            }
            // select reports a TCP socket writable even when no packet is due.
            // Bound idle polling without coupling it to the simulation thread.
            if(selected>0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for(auto& peer:peers)drop(peer);peers.clear();close_socket(listener);
        std::lock_guard lock(mutex);state.connected=false;lan_permits.clear();lan_id.clear();
    }
};
AuthoritativeServer::AuthoritativeServer(unsigned capacity,std::function<void(const std::string&)> log):impl_(std::make_unique<Impl>(capacity,std::move(log))){}
bool AuthoritativeServer::set_match_player_limit(unsigned limit){
    auto& p=*impl_;std::lock_guard lock(p.mutex);limit=std::min(limit,p.state.capacity);
    if(!limit)return false;
    const auto players=p.authority.participants();for(unsigned slot=limit;slot<max_players;++slot)if(players[slot].connected && players[slot].id!=p.bots[slot])return false;
    for(unsigned slot=limit;slot<max_players;++slot)if(players[slot].connected){p.authority.leave(p.bots[slot]);p.bots[slot]={};p.controls[slot]={};}
    p.match_player_limit=limit;++p.publication_serial;p.rebuild();return true;
}
std::optional<PlayerId> AuthoritativeServer::add_bot(unsigned skill){
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.state.connected || skill<1 || skill>3 || p.authority.connected_count()>=p.match_player_limit)return {};
    unsigned slot=1;const auto participants=p.authority.participants();while(slot<max_players && participants[slot].connected)++slot;
    const auto id=p.authority.join("Bot "+std::to_string(slot),true);if(!id)return {};
    p.bots[id->slot]=*id;p.bot_skills[id->slot]=skill;p.controls[id->slot]={};p.skin_choices[id->slot]={*id,uint8_t(id->slot%11)};++p.publication_serial;p.rebuild();return id;
}
bool AuthoritativeServer::remove_bot(PlayerId id){auto& p=*impl_;std::lock_guard lock(p.mutex);if(id.slot>=max_players || p.bots[id.slot]!=id || !p.authority.leave(id))return false;p.bots[id.slot]={};p.controls[id.slot]={};++p.publication_serial;p.rebuild();return true;}
uint8_t AuthoritativeServer::bot_mask()const{auto& p=*impl_;std::lock_guard lock(p.mutex);uint8_t mask=0;for(unsigned i=1;i<max_players;++i)if(p.bots[i].generation && p.authority.participant(p.bots[i]))mask|=uint8_t(1u<<i);return mask;}
unsigned AuthoritativeServer::bot_skill(unsigned slot)const{auto& p=*impl_;std::lock_guard lock(p.mutex);return slot<max_players?p.bot_skills[slot]:2;}
AuthoritativeServer::~AuthoritativeServer(){stop();}
bool AuthoritativeServer::start(uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& name,unsigned skin) {
    auto& p=*impl_;if(p.worker.joinable() || p.authority.connected_count() || !initialize_network() || !pin_valid(pin) || !chat_name_valid(name) || !valid_skin_choice(skin))return false;
    const auto session_id=new_lan_secret();if(session_id.empty())return false;
    p.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(p.listener==INVALID_SOCKET)return false;
    // Hidden native regression fixtures only use same-PC peers. Avoid opening
    // a LAN listener (and a fresh Windows Firewall prompt) for every test EXE.
    const auto hidden=std::getenv("SSNE_TEST_HIDDEN");
    const bool local_test=hidden && std::strcmp(hidden,"1")==0;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(local_test?INADDR_LOOPBACK:INADDR_ANY);address.sin_port=htons(port);
    if(bind(p.listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(p.listener,8) || !nonblocking(p.listener)){close_socket(p.listener);return false;}
    int length=sizeof(address);getsockname(p.listener,reinterpret_cast<sockaddr*>(&address),&length);
    {std::lock_guard lock(p.mutex);auto id=p.authority.join(name);if(!id){close_socket(p.listener);return false;}p.state.player=*id;p.state.port=ntohs(address.sin_port);p.state.connected=true;p.skin_choices[0]={*id,uint8_t(skin)};p.state.message="Host running; clients may join.";p.pin=pin;p.lan_id=session_id;p.lan_permits.clear();p.lan_join_enabled=true;p.fingerprint=fingerprint;p.stopping=false;p.rebuild();}
    p.worker=std::thread([&p]{p.run();});return true;
}
std::string AuthoritativeServer::lan_session_id()const{std::lock_guard lock(impl_->mutex);return impl_->state.connected?impl_->lan_id:std::string{};}
void AuthoritativeServer::set_lan_join_enabled(bool enabled){
    auto& p=*impl_;std::lock_guard lock(p.mutex);p.lan_join_enabled=enabled;
    if(!enabled)p.lan_permits.clear();
}
LanJoinGrant AuthoritativeServer::issue_lan_join_ticket(const std::string& peer,uint32_t nonce,uint32_t lifetime_ms){
    if(!lifetime_ms || lifetime_ms>60000 || !local_lan_peer(peer))return {};
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.state.connected || p.stopping || !p.lan_join_enabled || !valid_lan_secret(p.lan_id))return {};
    const auto now=milliseconds();for(auto it=p.lan_permits.begin();it!=p.lan_permits.end();)if(it->second.expires<=now)it=p.lan_permits.erase(it);else ++it;
    for(const auto& [ticket,permit]:p.lan_permits)if(permit.peer==peer && permit.nonce==nonce)return {ticket,uint32_t(permit.expires-now)};
    if(p.lan_permits.size()>=64 || std::count_if(p.lan_permits.begin(),p.lan_permits.end(),[&](const auto& item){return item.second.peer==peer;})>=16)return {};
    auto ticket=new_lan_secret();if(ticket.empty() || p.lan_permits.contains(ticket))return {};
    p.lan_permits.emplace(ticket,Impl::LanPermit{peer,nonce,now+lifetime_ms});return {std::move(ticket),lifetime_ms};
}
std::array<LobbySkinChoice,max_players> AuthoritativeServer::requested_skins()const{std::lock_guard lock(impl_->mutex);auto result=impl_->skin_choices;for(unsigned i=0;i<max_players;++i)if(!impl_->authority.participant(result[i].player))result[i]={};return result;}
NetworkStatus AuthoritativeServer::status() const{std::lock_guard lock(impl_->mutex);return impl_->state;}
HostPublicationDemand AuthoritativeServer::publication_demand() const{
    auto& p=*impl_;std::lock_guard lock(p.mutex);HostPublicationDemand out;
    out.tcp_peers=p.tcp_publication_peers;out.serial=p.publication_serial;out.host=p.state.player;
    out.all_ready=p.state.connected && !p.stopping;
    for(unsigned slot=0;slot<max_players;++slot){const auto& person=p.authority.participants()[slot];if(!person.connected)continue;out.owners[slot]=person.id;out.all_ready&=person.ready && person.character!=0;}
    return out;
}
std::shared_ptr<const WorldState> AuthoritativeServer::latest_world() const{std::lock_guard lock(impl_->mutex);return impl_->published;}
bool AuthoritativeServer::set_custom_map_package(uint64_t epoch,std::shared_ptr<const CustomMapPackage> package,std::string& error){
    if(!package || !custom_map_package_valid(*package,error,false))return false;
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    const CustomMapRequest request{epoch,package->manifest.alias,package->manifest.id,package->manifest.map_sha256};
    if(!p.state.connected || !selected_custom_map(*p.published,request)){error="Custom map package is no longer selected by the host.";return false;}
    p.custom_map_epoch=epoch;p.custom_map_package=std::move(package);p.custom_map_failed_epoch=0;p.custom_map_failure.clear();p.custom_map_failed_alias.clear();return true;
}
void AuthoritativeServer::clear_custom_map_package(){auto& p=*impl_;std::lock_guard lock(p.mutex);p.custom_map_package.reset();p.custom_map_epoch=p.custom_map_failed_epoch=0;p.custom_map_failure.clear();p.custom_map_failed_alias.clear();}
void AuthoritativeServer::fail_custom_map_package(uint64_t epoch,const std::string& alias,const std::string& error){auto& p=*impl_;std::lock_guard lock(p.mutex);if(p.published->epoch!=epoch || selected_custom_map_alias(*p.published)!=alias)return;p.custom_map_package.reset();p.custom_map_epoch=0;p.custom_map_failed_epoch=epoch;p.custom_map_failed_alias=alias;p.custom_map_failure=error.empty()?"Host custom map package is unavailable.":error.substr(0,256);}
void AuthoritativeServer::lobby_ready(bool ready){auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.lobby_open || !p.state.connected || p.stopping || !p.authority.participant(p.state.player))return;const auto mask=uint8_t(1u<<p.state.player.slot);const auto next=uint8_t(ready?p.lobby_ready_mask|mask:p.lobby_ready_mask&~mask);if(next!=p.lobby_ready_mask){p.lobby_ready_mask=next;p.rebuild();}}
void AuthoritativeServer::reset_lobby_readiness(){auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.lobby_open || !p.state.connected || p.stopping)return;p.reset_lobby_votes();p.rebuild();}
bool AuthoritativeServer::begin_lobby_game(){auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.lobby_open || !p.state.connected || p.stopping)return false;for(const auto& person:p.authority.participants())if(person.connected && (!(p.lobby_ready_mask&(1u<<person.id.slot)) || p.lobby_ready_owners[person.id.slot]!=person.id))return false;p.lobby_open=false;p.lobby_ready_mask=0;p.rebuild();return true;}
bool AuthoritativeServer::cancel_lobby_game(){auto& p=*impl_;std::lock_guard lock(p.mutex);const auto phase=p.body->mission.find("Session.NativePhase");if(p.lobby_open || !p.state.connected || p.stopping || phase==p.body->mission.end() || phase->second!=Property(std::string("menu")))return false;p.lobby_open=true;p.reset_lobby_votes();p.rebuild();return true;}
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
    const bool reset=loading || world.epoch!=p.body->epoch;
    if(reset){p.spectator_lock_mask=0;p.authority.set_spectator_lock(0);}
    world.players=p.effective_participants();world.vote=p.authority.skip_vote();world.revision=p.revision+1;
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
        if(menu && (previous==p.body->mission.end() || previous->second!=phase->second)){p.lobby_open=true;p.reset_lobby_votes();}
        else if(!menu && phase->second!=Property(std::string("loading"))){p.lobby_open=false;p.lobby_ready_mask=0;}
        // A UI reset can publish the old proposal with a fresh token before
        // this new snapshot is captured. Bind confirmation to the committed
        // setup as well, so that intermediate token cannot confirm a new map.
        else if(menu && (world.epoch!=p.body->epoch || (p.lobby_open && lobby_setup_changed(p.body->mission,world.mission))))p.reset_lobby_votes();
    }
    for(unsigned i=0;i<max_players;++i)world.input_ack[i]=p.controls[i].ack;
    p.local_ping_view.reset();if(reset || world.level!=p.body->level)p.tactical_pings.clear(world.epoch);
    auto unpublished=std::make_shared<WorldState>(std::move(world));p.body=unpublished;p.rebuild(owned&&reuse_world_copies()?std::move(unpublished):nullptr);return true;
}
bool AuthoritativeServer::attach_character(PlayerId id,EntityId entity) {
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    if(!p.body->entities.contains(entity) || p.body->entities.at(entity).kind!=EntityKind::Player || p.body->entities.at(entity).owner!=id)return false;
    if(!p.authority.attach_character(id,entity))return false;if(p.bots[id.slot]==id)p.authority.finish_join(id);++p.publication_serial;p.rebuild();return true;
}
bool AuthoritativeServer::ready_host(){auto& p=*impl_;std::lock_guard lock(p.mutex);auto* player=p.authority.participant(p.state.player);if(!player || !p.body->entities.contains(player->character) || p.body->entities.at(player->character).owner!=player->id || !p.authority.finish_join(p.state.player))return false;++p.publication_serial;p.rebuild();return true;}
std::array<PlayerControl,max_players> AuthoritativeServer::take_controls(ControlInput* host_ui) {
    auto& p=*impl_;std::lock_guard lock(p.mutex);std::array<PlayerControl,max_players> result;bool changed=false;
    if(host_ui)*host_ui={};
    for(unsigned slot=0;slot<max_players;++slot) {
        const auto& player=p.authority.participants()[slot];if(!player.connected)continue;
        auto& input=p.controls[slot];auto& out=result[slot];out.id=player.id;out.ready=player.ready;out.spectator=player.spectator || (p.spectator_lock_mask&(1u<<slot));out.afk=player.afk && !out.spectator;out.sequence=input.sequence;
        if(host_ui && !p.stopping && p.state.connected && player.id==p.state.player && (player.afk || out.spectator) && player.ready && !p.results_active() && milliseconds()-input.received<250){
            // A human can reopen/use Pause while AFK owns the character. Keep
            // only native menu buttons and left-stick navigation, never firing,
            // camera turns, weapon requests or temporary/native preferences.
            constexpr uint16_t menu_buttons=ssne::PAD_START|ssne::PAD_A|ssne::PAD_B|ssne::PAD_X|ssne::PAD_Y|ssne::PAD_Z|ssne::PAD_LEFT|ssne::PAD_RIGHT|ssne::PAD_UP|ssne::PAD_DOWN;
            host_ui->pad.buttons=uint16_t((input.input.pad.buttons|input.input.pressed)&menu_buttons);
            host_ui->pressed=uint16_t(input.input.pressed&menu_buttons);
            host_ui->pad.stick_x=input.input.pad.stick_x;host_ui->pad.stick_y=input.input.pad.stick_y;
        }
        if(player.id==p.state.player && p.results_active() && milliseconds()-input.received<250){
            // AFK controls the character, never the host's shared result menu.
            // Accept only fresh human menu buttons while native actors are gone.
            constexpr uint16_t menu_buttons=ssne::PAD_A|ssne::PAD_B|ssne::PAD_X|ssne::PAD_Y|ssne::PAD_LEFT|ssne::PAD_RIGHT|ssne::PAD_UP|ssne::PAD_DOWN;
            out.input.pad.buttons=uint16_t((input.input.pad.buttons|input.input.pressed)&menu_buttons);
            out.input.pressed=uint16_t(input.input.pressed&menu_buttons);
        }
        else if(out.spectator && milliseconds()-input.received<250){
            // Retain only watch cycling / native menu navigation. The session
            // consumes these under the spectator flag before native PAD input;
            // no trigger, camera turn, weapon or preference can reach gameplay.
            constexpr uint16_t menu=ssne::PAD_START|ssne::PAD_A|ssne::PAD_B|ssne::PAD_X|ssne::PAD_Y|ssne::PAD_Z|ssne::PAD_LEFT|ssne::PAD_RIGHT|ssne::PAD_UP|ssne::PAD_DOWN;
            out.input.pad.buttons=uint16_t((input.input.pad.buttons|input.input.pressed)&menu);out.input.pressed=uint16_t(input.input.pressed&menu);
            out.input.pad.stick_x=input.input.pad.stick_x;out.input.pad.stick_y=input.input.pad.stick_y;
        }
        else if((player.ready || (player.id==p.state.player && !player.character)) && !player.afk && milliseconds()-input.received<250){
            out.input=input.input;out.input.pad.buttons|=out.input.pressed;
            // A press and release may both arrive before one native tick. The
            // edge latch already preserves digital L/R, but original jump/fire
            // actions read the analog triggers. Recreate one consumed pulse.
            if(out.input.pressed&ssne::PAD_L)out.input.pad.trigger_l=255;
            if(out.input.pressed&ssne::PAD_R)out.input.pad.trigger_r=255;
            out.fire_pressed=input.fire_pressed;
        }
        else if(p.lobby_open && !p.results_active() && !player.afk && milliseconds()-input.received<250){
            // Unspawned peers may choose only their own lobby character.
            // Do not forward confirm/back, camera, movement or firing.
            out.input.pad.buttons=input.input.pad.buttons&(ssne::PAD_LEFT|ssne::PAD_RIGHT);
            out.input.pressed=input.input.pressed&(ssne::PAD_LEFT|ssne::PAD_RIGHT);
        }
        if(player.ready && input.ack!=input.sequence){input.ack=input.sequence;changed=true;}
        drain_transients(input.input);
    }
    // Acks describe consumed input, but must be published with the native pose
    // captured after simulation. rebuild() deliberately retains the old pose's
    // input_ack, so an input-only rebuild creates only a redundant revision.
    if(changed && !reuse_input_ack())p.rebuild();return result;
}
uint64_t AuthoritativeServer::begin_cutscene(bool presentation){auto& p=*impl_;std::lock_guard lock(p.mutex);auto id=p.authority.begin_cutscene(presentation);p.rebuild();return id;}
std::array<Participant,max_players> AuthoritativeServer::participants() const{std::lock_guard lock(impl_->mutex);return impl_->effective_participants();}
std::shared_ptr<void> AuthoritativeServer::checkpoint_control_lease(){return std::make_shared<std::unique_lock<std::recursive_mutex>>(impl_->mutex);}
std::vector<uint8_t> AuthoritativeServer::checkpoint_authority() const{std::lock_guard lock(impl_->mutex);return impl_->authority.checkpoint_state();}
bool AuthoritativeServer::restore_checkpoint_authority(const std::vector<uint8_t>& bytes,std::string& error){std::lock_guard lock(impl_->mutex);if(!impl_->authority.restore_checkpoint(bytes,error))return false;impl_->spectator_lock_mask=impl_->authority.spectator_lock_mask();impl_->tactical_pings.clear(impl_->body->epoch);impl_->local_ping_view.reset();return true;}
bool AuthoritativeServer::restore_gameplay_authority(const std::vector<uint8_t>& bytes,std::string& error){std::lock_guard lock(impl_->mutex);if(!impl_->authority.restore_gameplay_checkpoint(bytes,error))return false;impl_->spectator_lock_mask=0;impl_->tactical_pings.clear(impl_->body->epoch);impl_->local_ping_view.reset();return true;}
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
            p.authority=std::move(authority);p.spectator_lock_mask=0;p.body=world;p.published=std::move(world);
            p.revision=p.published->revision;p.state.world_revision=p.revision;
            p.native_events=std::move(journal);
            p.tactical_pings.clear(p.body->epoch);
            p.local_ping_view.reset();
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
CommandResult AuthoritativeServer::host_afk(uint64_t sequence,bool value){auto& p=*impl_;std::lock_guard lock(p.mutex);auto result=p.authority.request_afk(p.state.player,sequence,value,p.results_active());if(result==CommandResult::Applied){p.controls[p.state.player.slot].input={};++p.publication_serial;p.rebuild();}return result;}
bool AuthoritativeServer::host_spectator_available()const{auto& p=*impl_;std::lock_guard lock(p.mutex);const auto* host=p.authority.participant(p.state.player);return host && host->ready && (host->spectator || p.host_has_human_peer());}
CommandResult AuthoritativeServer::host_spectator(uint64_t sequence,bool value){auto& p=*impl_;std::lock_guard lock(p.mutex);if(value && !p.host_has_human_peer())return CommandResult::NotEligible;auto result=p.authority.request_spectator(p.state.player,sequence,value,p.results_active());if(result==CommandResult::Applied){p.controls[p.state.player.slot].input={};++p.publication_serial;p.rebuild();}return result;}
bool AuthoritativeServer::cancel_spectator_activation(PlayerId id){
    auto& p=*impl_;std::lock_guard lock(p.mutex);const auto* before=p.authority.participant(id);const bool changed=before && before->spectator;
    if(!p.authority.cancel_spectator_activation(id))return false;
    if(changed){p.controls[id.slot].input={};++p.publication_serial;p.rebuild();}return true;
}
bool AuthoritativeServer::set_spectator_lock(uint8_t mask){auto& p=*impl_;std::lock_guard lock(p.mutex);if(mask&~((1u<<p.state.capacity)-1u))return false;p.authority.set_spectator_lock(mask);const auto accepted=p.authority.spectator_lock_mask();if(accepted!=p.spectator_lock_mask){const auto changed=accepted^p.spectator_lock_mask;p.spectator_lock_mask=accepted;for(unsigned i=0;i<max_players;++i)if(changed&(1u<<i))p.controls[i].input={};++p.publication_serial;p.rebuild();}return true;}
uint8_t AuthoritativeServer::voluntary_spectator_mask()const{auto& p=*impl_;std::lock_guard lock(p.mutex);return p.voluntary_spectators();}
CommandResult AuthoritativeServer::host_skip(uint64_t sequence,uint64_t scene){auto& p=*impl_;std::lock_guard lock(p.mutex);auto result=p.authority.request_skip(p.state.player,sequence,scene);if(result==CommandResult::Applied)p.rebuild();return result;}
void AuthoritativeServer::submit_host_input(ControlInput input){if(!control_valid(input))return;auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.state.connected)return;auto& state=p.controls[p.state.player.slot];merge_input(state.input,input);++state.sequence;state.received=milliseconds();}
void AuthoritativeServer::disconnect(PlayerId id){auto& p=*impl_;std::lock_guard lock(p.mutex);if(id==p.state.player)return;if(p.authority.leave(id)){p.controls[id.slot]={};++p.publication_serial;p.rebuild();}}
bool AuthoritativeServer::send_chat(const std::string& text){
    auto& p=*impl_;std::lock_guard lock(p.mutex);const auto* sender=p.authority.participant(p.state.player);
    return p.state.connected && !p.stopping && sender && p.chat.send(*sender,text,milliseconds());
}
std::vector<ChatMessage> AuthoritativeServer::chat_messages()const{std::lock_guard lock(impl_->mutex);return impl_->chat.messages();}
bool AuthoritativeServer::send_ping(const TacticalPingRequest& request){auto& p=*impl_;std::lock_guard lock(p.mutex);return p.accept_ping(p.state.player,request);}
bool AuthoritativeServer::send_ping(const TacticalPingRequest& request,const WorldState& current){
    auto& p=*impl_;std::lock_guard lock(p.mutex);const auto view=p.small_local_ping_view(current,request.target);
    if(!view)return false;const auto people=p.effective_participants();
    if(!p.tactical_pings.send(people[p.state.player.slot],request,*view,milliseconds()))return false;
    p.local_ping_view=view;return true;
}
bool AuthoritativeServer::update_local_ping_view(const WorldState& current){
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.local_ping_view || p.tcp_publication_peers)return false;
    const auto people=p.effective_participants();const auto active=p.tactical_pings.messages(*p.local_ping_view,people,milliseconds());
    const auto marker=std::find_if(active.begin(),active.end(),[&](const auto& ping){return ping.sender==p.state.player;});
    if(marker==active.end()){p.local_ping_view.reset();return false;}
    const auto view=p.small_local_ping_view(current,marker->target);
    if(!view || p.tactical_pings.messages(*view,people,milliseconds()).empty()){p.local_ping_view.reset();p.tactical_pings.remove(p.state.player);return false;}
    p.local_ping_view=view;return true;
}
std::vector<TacticalPing> AuthoritativeServer::pings()const{auto& p=*impl_;std::lock_guard lock(p.mutex);return p.state.connected&&!p.stopping?p.tactical_pings.messages(p.local_ping_view&&!p.tcp_publication_peers?*p.local_ping_view:*p.body,p.effective_participants(),milliseconds()):std::vector<TacticalPing>{};}
std::array<PlayerLatency,max_players> AuthoritativeServer::player_latencies()const{auto& p=*impl_;std::lock_guard lock(p.mutex);return p.state.connected&&!p.stopping?p.measured_latencies():std::array<PlayerLatency,max_players>{};}
void AuthoritativeServer::stop(){auto& p=*impl_;p.stopping=true;if(p.worker.joinable())p.worker.join();std::lock_guard lock(p.mutex);p.lan_permits.clear();p.lan_id.clear();p.chat.clear();p.tactical_pings.clear();p.latencies={};p.local_ping_view.reset();p.custom_map_package.reset();p.custom_map_epoch=0;}

struct AuthoritativeClient::Impl {
    uint8_t skin=255;
    mutable std::mutex mutex;NetworkStatus state;
    std::shared_ptr<const WorldState> world=std::make_shared<WorldState>();
    double world_received=0;
    InputLookHistory look_history;
    LocalControlSample local_control;
    std::deque<AuthoritativeClient::WorldSample> motion_history;
    std::map<uint64_t,std::shared_ptr<const WorldState>> history;
    std::thread worker;std::atomic<bool> stopping{false};
    ControlInput pending;uint64_t input_sequence=0,command_sequence=0,sent_input=0;
    std::deque<std::vector<uint8_t>> commands,chat_outgoing;
    std::deque<ChatMessage> chat;
    ChatRateLimit chat_rate;
    uint64_t chat_sequence=0;
    struct TimedPing {TacticalPing ping;uint64_t expires=0;};
    std::vector<TimedPing> tactical_pings;
    std::array<PlayerLatency,max_players> latencies{};
    uint64_t last_tactical_ping=0,ping_token=0;bool tactical_ping_sent=false;
    CustomMapReceiver custom_map_receiver;uint64_t custom_map_serial=0,custom_map_progress=0;
    std::string address,pin,name;uint16_t port=0;uint64_t fingerprint=0;
    void fail(const std::string& message){std::lock_guard lock(mutex);state.connecting=false;state.connected=false;state.message=message;if(transfer_active(custom_map_receiver.status().phase))custom_map_receiver.cancel(message);}
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
            if(delta.epoch==world->epoch && delta.revision==world->revision){std::vector<uint8_t> ack;put64(ack,delta.revision);enqueue(wire::Message::WorldAck,std::move(ack));return true;}
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
            const auto& transfer=custom_map_receiver.status();
            if(transfer.phase!=CustomMapTransferPhase::Idle && transfer.phase!=CustomMapTransferPhase::Cancelled && !selected_custom_map(*next,transfer.request)){
                std::vector<uint8_t> cancel;put64(cancel,custom_map_receiver.token());put64(cancel,transfer.request.epoch);enqueue(wire::Message::CustomMapCancel,std::move(cancel));custom_map_receiver.cancel("The selected level changed.");
            }
            // Never relabel movement/fire accumulated during decode in the old
            // world as new input. Framed old-epoch commands remain rejectable.
            if(next->epoch!=world->epoch){pending={};sent_input=input_sequence;retired_history.swap(history);retired_motion.swap(motion_history);look_history.reset(next->epoch);local_control={};tactical_pings.clear();last_tactical_ping=0;tactical_ping_sent=false;}
            else if(world->revision)history[world->revision]=world;
            if(next->level!=world->level || !tactical_ping_gameplay(*next))tactical_pings.clear();
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
        if(type==wire::Message::CustomMapOffer){
            if(bytes.size()>1040)return false;const auto token=r.u64(),epoch=r.u64();if(!r.ok)return false;
            if(token!=custom_map_receiver.token() || epoch!=custom_map_receiver.status().request.epoch)return true;
            CustomMapManifest manifest;std::string error;
            if(!decode_custom_map_manifest(std::span<const uint8_t>(bytes).subspan(r.offset),manifest,error)){custom_map_receiver.fail(error);return false;}
            custom_map_progress=milliseconds();return custom_map_receiver.offer(token,epoch,manifest,error);
        }
        if(type==wire::Message::CustomMapChunk){
            if(bytes.size()<25 || bytes.size()>custom_map_chunk_bytes+24)return false;const auto token=r.u64(),epoch=r.u64(),offset=r.u64();if(!r.ok)return false;
            std::string error;const auto ok=custom_map_receiver.chunk(token,epoch,offset,std::span<const uint8_t>(bytes).subspan(r.offset),error);custom_map_progress=milliseconds();return ok;
        }
        if(type==wire::Message::CustomMapComplete){
            const auto token=r.u64(),epoch=r.u64();const auto cached=r.u8();if(!r.done() || cached>1)return false;
            std::string error;custom_map_progress=milliseconds();return custom_map_receiver.finish(token,epoch,error,cached!=0);
        }
        if(type==wire::Message::CustomMapError){
            const auto token=r.u64(),epoch=r.u64();const auto error=r.string(256);if(!r.done())return false;
            if(token==custom_map_receiver.token() && epoch==custom_map_receiver.status().request.epoch && transfer_active(custom_map_receiver.status().phase))custom_map_receiver.fail(error);return true;
        }
        if(type==wire::Message::SessionTelemetry){
            if(bytes.size()>4096)return false;
            const auto epoch=r.u64();const auto count=r.u32();if(!epoch || count>max_players)return false;
            std::array<PlayerLatency,max_players> measured{};uint8_t slots=0;
            for(unsigned i=0;i<count;++i){
                PlayerLatency value;value.id.slot=r.u32();value.id.generation=r.u64();const auto known=r.u8();value.ping_ms=r.u32();value.known=known!=0;
                if(!r.ok || value.id.slot>=max_players || !value.id.generation || known>1 || value.ping_ms>60000 || (slots&(1u<<value.id.slot)))return false;
                slots|=uint8_t(1u<<value.id.slot);measured[value.id.slot]=value;
            }
            const auto ping_count=r.u32();if(ping_count>max_players)return false;
            std::vector<TimedPing> incoming;incoming.reserve(ping_count);slots=0;const auto now=milliseconds();
            for(unsigned i=0;i<ping_count;++i){
                TacticalPing ping;ping.sequence=r.u64();ping.epoch=epoch;ping.sender.slot=r.u32();ping.sender.generation=r.u64();ping.name=r.string(64);
                ping.position=read_point(r);ping.target=r.u64();ping.kind=PingKind(r.u8());ping.remaining_ms=r.u32();
                const TacticalPingRequest request{ping.epoch,ping.position,ping.target,ping.kind};
                if(!r.ok || !ping.sequence || ping.sender.slot>=max_players || !ping.sender.generation || !chat_name_valid(ping.name) ||
                   !tactical_ping_request_valid(request) || !ping.remaining_ms || ping.remaining_ms>tactical_ping_lifetime_ms || (slots&(1u<<ping.sender.slot)))return false;
                slots|=uint8_t(1u<<ping.sender.slot);incoming.push_back({std::move(ping),now+ping.remaining_ms});
            }
            if(!r.done())return false;
            latencies=measured;
            // World and telemetry are separate snapshots. Ignore a marker
            // snapshot whose epoch is not yet installed; the next refresh
            // carries the remaining lifetime without replaying old worlds.
            if(epoch==world->epoch)tactical_pings=std::move(incoming);
            return true;
        }
        if(type==wire::Message::ChatHistory) {
            if(bytes.size()>24*1024)return false;
            const auto count=r.u32();if(!count || count>chat_history_limit)return false;
            std::vector<ChatMessage> incoming;incoming.reserve(count);uint64_t previous=0;
            for(unsigned i=0;i<count;++i){
                ChatMessage message;message.sequence=r.u64();message.sender.slot=r.u32();message.sender.generation=r.u64();
                message.name=r.string(64);message.text=r.string(unsigned(chat_text_bytes));
                const auto normalized=normalize_chat_text(message.text);
                if(!r.ok || !message.sequence || message.sequence<=previous || message.sender.slot>=max_players ||
                   !message.sender.generation || !chat_name_valid(message.name) || !normalized || *normalized!=message.text)return false;
                previous=message.sequence;incoming.push_back(std::move(message));
            }
            if(!r.done())return false;
            for(auto& message:incoming)if(message.sequence>chat_sequence){chat_sequence=message.sequence;chat.push_back(std::move(message));}
            while(chat.size()>chat_history_limit)chat.pop_front();return true;
        }
        if(type==wire::Message::Ping){const auto token=r.u64();if(!r.done())return false;std::vector<uint8_t> data;put64(data,token);enqueue(wire::Message::Pong,std::move(data));return true;}
        if(type==wire::Message::Pong){const auto sent=r.u64();if(!r.done())return false;const auto now=milliseconds();if(ping_token && sent==ping_token && now>=sent){state.ping_ms=unsigned(std::min<uint64_t>(60000,now-sent));ping_token=0;}return true;}
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
                    // Hosts before local effects still receive the original protocol-71 packet.
                    if(world->mission.contains(local_weapon_key))put64(data,local_control.fire_pressed);
                    const auto input_frame=packet(wire::Message::Input,std::move(data));
                    stream.transmit.insert(stream.transmit.end(),input_frame.begin(),input_frame.end());
                    stream.last_progress=milliseconds();
                    sent_input=input_sequence;drain_transients(pending);
                }
                if(state.connected && !chat_outgoing.empty()){
                    const auto& chat_frame=chat_outgoing.front();
                    stream.transmit.insert(stream.transmit.end(),chat_frame.begin(),chat_frame.end());chat_outgoing.pop_front();
                    stream.last_progress=milliseconds();
                }
                if(stream.transmit.empty() && state.connected) {
                    const auto now=milliseconds();
                    // Keep an outstanding probe until it is answered or times out.
                    if(now-ping>=1000 && (!ping_token || now-ping>=5000)) {
                        ping=now;
                        ping_token=now;
                        std::vector<uint8_t> data;
                        put64(data,ping_token);
                        stream.queue(packet(wire::Message::Ping,std::move(data)));
                    }
                }
            }
            fd_set reads,writes;FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(stream.socket,&reads);
            if(!stream.transmit.empty())FD_SET(stream.socket,&writes);
            // Local submissions do not make the network socket readable. A
            // 10ms idle poll therefore adds input delay even at a 1ms RTT.
            timeval wait{0,1000};auto selected=select(0,&reads,&writes,nullptr,&wait);if(selected==SOCKET_ERROR){stream.fail("Cooperative poll socket error "+std::to_string(WSAGetLastError())+".");break;}
            uint64_t received=0,sent=0;if(FD_ISSET(stream.socket,&reads))ok=stream.read(received);
            if(ok)ok=stream.parse([&](auto type,const auto& bytes){return message(type,bytes);});
            if(ok && FD_ISSET(stream.socket,&writes))ok=stream.write(sent);
            {std::lock_guard lock(mutex);state.bytes_received+=received;state.bytes_sent+=sent;
                if(transfer_active(custom_map_receiver.status().phase) && milliseconds()-custom_map_progress>30000){std::vector<uint8_t> cancel;put64(cancel,custom_map_receiver.token());put64(cancel,custom_map_receiver.status().request.epoch);enqueue(wire::Message::CustomMapCancel,std::move(cancel));custom_map_receiver.fail("Custom map download stalled.");}
            }
            if(ok)ok=stream.within_timeouts();
        }
        close_socket(stream.socket);std::lock_guard lock(mutex);state.connecting=false;state.connected=false;
        if(transfer_active(custom_map_receiver.status().phase))custom_map_receiver.cancel("Disconnected during custom map transfer.");
        if(stopping)state.message="Disconnected.";
        else if(state.message=="Connected to authoritative host." || state.message=="Connected; receiving authoritative world." ||
                state.message=="Connecting to authoritative host.")state.message=std::move(stream.failure);
    }
};
AuthoritativeClient::AuthoritativeClient():impl_(std::make_unique<Impl>()){}
AuthoritativeClient::~AuthoritativeClient(){stop();}
bool AuthoritativeClient::request_custom_map(const CustomMapRequest& request,std::string& error){
    if(!custom_map_request_valid(request,error))return false;
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    if(!p.state.connected || p.stopping || p.commands.size()>=126 || !selected_custom_map(*p.world,request)){error="Custom map request does not match the connected host world.";return false;}
    if(transfer_active(p.custom_map_receiver.status().phase)){std::vector<uint8_t> cancel;put64(cancel,p.custom_map_receiver.token());put64(cancel,p.custom_map_receiver.status().request.epoch);p.enqueue(wire::Message::CustomMapCancel,std::move(cancel));}
    if(!p.custom_map_receiver.request(++p.custom_map_serial,request,error))return false;
    std::vector<uint8_t> bytes;put64(bytes,p.custom_map_serial);put64(bytes,request.epoch);put_string(bytes,request.alias);put_string(bytes,request.id);put_string(bytes,request.map_sha256);put_string(bytes,request.known_package_sha256);p.enqueue(wire::Message::CustomMapRequest,std::move(bytes));p.custom_map_progress=milliseconds();return true;
}
CustomMapTransferStatus AuthoritativeClient::custom_map_transfer_status()const{std::lock_guard lock(impl_->mutex);return impl_->custom_map_receiver.status();}
std::optional<CustomMapDownload> AuthoritativeClient::take_custom_map_package(){std::lock_guard lock(impl_->mutex);return impl_->custom_map_receiver.take();}
void AuthoritativeClient::cancel_custom_map(const std::string& reason){auto& p=*impl_;std::lock_guard lock(p.mutex);if(transfer_active(p.custom_map_receiver.status().phase) && p.state.connected){std::vector<uint8_t> bytes;put64(bytes,p.custom_map_receiver.token());put64(bytes,p.custom_map_receiver.status().request.epoch);p.enqueue(wire::Message::CustomMapCancel,std::move(bytes));}p.custom_map_receiver.cancel(reason);}
bool AuthoritativeClient::start(const std::string& address,uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& name,unsigned skin){return start_connection(address,port,pin,fingerprint,name,skin,false);}
bool AuthoritativeClient::start_lan(const std::string& address,uint16_t port,const std::string& ticket,uint64_t fingerprint,const std::string& name,unsigned skin){return start_connection(address,port,ticket,fingerprint,name,skin,true);}
bool AuthoritativeClient::start_connection(const std::string& address,uint16_t port,const std::string& pin,uint64_t fingerprint,const std::string& name,unsigned skin,bool lan) {
    auto& p=*impl_;if(p.worker.joinable() || !initialize_network() || !port || !(lan?valid_lan_secret(pin):pin_valid(pin)) || !chat_name_valid(name) || !valid_skin_choice(skin))return false;
    p.skin=uint8_t(skin);p.address=address;p.port=port;p.pin=pin;p.fingerprint=fingerprint;p.name=name;p.state={};p.world=std::make_shared<WorldState>();p.history.clear();p.motion_history.clear();p.look_history.reset(1);p.local_control={};p.world_received=0;p.commands.clear();p.chat_outgoing.clear();p.chat.clear();p.chat_rate.clear();p.chat_sequence=0;p.tactical_pings.clear();p.latencies={};p.last_tactical_ping=p.ping_token=0;p.tactical_ping_sent=false;p.pending={};p.input_sequence=p.command_sequence=p.sent_input=0;p.state.port=port;p.state.connecting=true;p.state.message="Connecting to authoritative host.";p.stopping=false;p.custom_map_receiver=CustomMapReceiver{};p.custom_map_serial=p.custom_map_progress=0;
    p.worker=std::thread([&p]{p.run();});return true;
}
NetworkStatus AuthoritativeClient::status()const{std::lock_guard lock(impl_->mutex);return impl_->state;}
std::shared_ptr<const WorldState> AuthoritativeClient::latest_world()const{std::lock_guard lock(impl_->mutex);return impl_->world;}
AuthoritativeClient::WorldSample AuthoritativeClient::world_sample()const{std::lock_guard lock(impl_->mutex);return {impl_->world,impl_->world_received};}
std::vector<AuthoritativeClient::WorldSample> AuthoritativeClient::motion_samples(uint64_t after,uint64_t through)const{std::lock_guard lock(impl_->mutex);std::vector<WorldSample> out;for(const auto& s:impl_->motion_history)if(s.world->revision>after && s.world->revision<=through)out.push_back(s);return out;}
PredictedLook AuthoritativeClient::predicted_look(uint64_t epoch,uint64_t ack,double now,
                                                  const std::array<float,3>& host_angles,float pitch_low,float pitch_high,double scale){
    std::lock_guard lock(impl_->mutex);
    return impl_->look_history.sample(epoch,ack,now,host_angles,pitch_low,pitch_high,scale);
}
void AuthoritativeClient::clear_predicted_look(){std::lock_guard lock(impl_->mutex);impl_->look_history.reset(impl_->world->epoch);}
void AuthoritativeClient::ready(uint64_t epoch){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,epoch);impl_->enqueue(wire::Message::Ready,std::move(bytes));}
void AuthoritativeClient::lobby_ready(bool ready){std::lock_guard lock(impl_->mutex);auto token=impl_->world->epoch;if(const auto it=impl_->world->mission.find("Session.Lobby.ReadyToken");it!=impl_->world->mission.end())if(const auto value=std::get_if<int64_t>(&it->second))token=std::bit_cast<uint64_t>(*value);std::vector<uint8_t> bytes;put64(bytes,token);bytes.push_back(ready);impl_->enqueue(wire::Message::LobbyReady,std::move(bytes));}
void AuthoritativeClient::submit_input(ControlInput input){
    if(!control_valid(input))return;
    auto& p=*impl_;
    std::lock_guard lock(p.mutex);
    merge_input(p.pending,input);
    const auto sequence=++p.input_sequence;
    const auto epoch=p.world->epoch;
    p.look_history.append(epoch,sequence,input.yaw,input.pitch);

    auto& local=p.local_control;
    const auto fire_down=[](const ssne::PadState& pad){return (pad.buttons&ssne::PAD_R) || pad.trigger_r>=128;};
    const bool previous_fire=fire_down(local.input.pad),fire=fire_down(input.pad);
    if(!local.fire_released && !fire)local.fire_released=sequence;
    if((fire && !previous_fire) || (input.pressed&ssne::PAD_R))local.fire_pressed=sequence;
    if(previous_fire && !fire)local.fire_released=sequence;
    if(((input.pad.buttons&ssne::PAD_L) && !(local.input.pad.buttons&ssne::PAD_L)) ||
       (input.pressed&ssne::PAD_L))local.jump_pressed=sequence;
    local.epoch=epoch;
    local.sequence=sequence;
    local.input=input;
    local.sampled_at=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
LocalControlSample AuthoritativeClient::local_control() const{
    std::lock_guard lock(impl_->mutex);
    return impl_->local_control;
}
void AuthoritativeClient::set_afk(bool value){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,impl_->world->epoch);put64(bytes,++impl_->command_sequence);bytes.push_back(value);impl_->enqueue(wire::Message::Afk,std::move(bytes));}
void AuthoritativeClient::set_spectator(bool value){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,impl_->world->epoch);put64(bytes,++impl_->command_sequence);bytes.push_back(value);impl_->enqueue(wire::Message::Spectator,std::move(bytes));}
void AuthoritativeClient::vote_skip(uint64_t scene){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,impl_->world->epoch);put64(bytes,++impl_->command_sequence);put64(bytes,scene);impl_->enqueue(wire::Message::Skip,std::move(bytes));}
void AuthoritativeClient::request_snapshot(){std::lock_guard lock(impl_->mutex);impl_->enqueue(wire::Message::SnapshotRequest);}
void AuthoritativeClient::acknowledge_native_events(uint64_t epoch,const NativeEventCursors& consumed){std::lock_guard lock(impl_->mutex);std::vector<uint8_t> bytes;put64(bytes,epoch);for(auto cursor:consumed)put64(bytes,cursor);impl_->enqueue(wire::Message::NativeEventsAck,std::move(bytes));}
bool AuthoritativeClient::send_chat(const std::string& text){
    const auto normalized=normalize_chat_text(text);if(!normalized)return false;
    auto& p=*impl_;std::lock_guard lock(p.mutex);
    if(!p.state.connected || p.stopping || p.chat_outgoing.size()>=chat_burst_limit || !p.chat_rate.accept(milliseconds()))return false;
    std::vector<uint8_t> bytes;put_string(bytes,*normalized);p.chat_outgoing.push_back(packet(wire::Message::Chat,std::move(bytes)));return true;
}
std::vector<ChatMessage> AuthoritativeClient::chat_messages()const{std::lock_guard lock(impl_->mutex);return {impl_->chat.begin(),impl_->chat.end()};}
bool AuthoritativeClient::send_ping(const TacticalPingRequest& request){
    if(!tactical_ping_request_valid(request))return false;
    auto& p=*impl_;std::lock_guard lock(p.mutex);const auto now=milliseconds();
    if(!p.state.connected || p.stopping || p.commands.size()>=128 || request.epoch!=p.world->epoch || !tactical_ping_gameplay(*p.world) ||
       p.state.player.slot>=max_players)return false;
    const auto& own=p.world->players[p.state.player.slot];
    if(own.id!=p.state.player || !own.connected || !own.ready || own.afk || own.spectator ||
       (p.tactical_ping_sent && (now<p.last_tactical_ping || now-p.last_tactical_ping<tactical_ping_cooldown_ms)))return false;
    std::vector<uint8_t> bytes;append_ping_request(bytes,request);p.enqueue(wire::Message::TacticalPing,std::move(bytes));
    p.last_tactical_ping=now;p.tactical_ping_sent=true;return true;
}
std::vector<TacticalPing> AuthoritativeClient::pings()const{
    auto& p=*impl_;std::lock_guard lock(p.mutex);std::vector<TacticalPing> out;
    if(!p.state.connected || p.stopping || !tactical_ping_gameplay(*p.world))return out;
    const auto now=milliseconds();
    for(const auto& entry:p.tactical_pings){
        const auto& ping=entry.ping;if(entry.expires<=now || ping.epoch!=p.world->epoch || ping.sender.slot>=max_players)continue;
        const auto& person=p.world->players[ping.sender.slot];
        if(!person.connected || person.id!=ping.sender || !person.ready || person.afk || person.spectator)continue;
        const auto actor=p.world->entities.find(person.character);
        if(actor==p.world->entities.end() || actor->second.owner!=person.id || !tactical_ping_alive(actor->second))continue;
        auto shown=ping;
        if(ping.target){
            const auto target=p.world->entities.find(ping.target);
            if(target==p.world->entities.end() || target->second.kind!=(ping.kind==PingKind::Enemy?EntityKind::Enemy:EntityKind::Pickup) || !tactical_ping_alive(target->second))continue;
            if(ping.kind==PingKind::Pickup && !tactical_ping_pickup_available(target->second,ping.sender.slot))continue;
            if(const auto* point=tactical_ping_position(target->second);point && tactical_ping_position_valid(*point))shown.position=*point;else continue;
        }
        shown.remaining_ms=uint32_t(std::min<uint64_t>(tactical_ping_lifetime_ms,entry.expires-now));out.push_back(std::move(shown));
    }
    return out;
}
std::array<PlayerLatency,max_players> AuthoritativeClient::player_latencies()const{
    auto& p=*impl_;std::lock_guard lock(p.mutex);std::array<PlayerLatency,max_players> out{};
    if(!p.state.connected || p.stopping)return out;
    for(unsigned slot=0;slot<max_players;++slot){const auto& person=p.world->players[slot];if(person.connected){out[slot]={person.id,0,false};if(p.latencies[slot].id==person.id)out[slot]=p.latencies[slot];}}
    return out;
}
void AuthoritativeClient::stop(){impl_->stopping=true;if(impl_->worker.joinable())impl_->worker.join();std::lock_guard lock(impl_->mutex);impl_->state.connecting=false;impl_->state.connected=false;impl_->chat.clear();impl_->chat_outgoing.clear();impl_->chat_rate.clear();impl_->chat_sequence=0;impl_->tactical_pings.clear();impl_->latencies={};impl_->last_tactical_ping=impl_->ping_token=0;impl_->tactical_ping_sent=false;impl_->custom_map_receiver.cancel("Disconnected.");}
}
