#include "authoritative_session.h"
#include "custom_map_session.h"
#include "wire_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>
using namespace port::coop;
namespace {
void require(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
void wait_for(const std::function<bool()>& predicate,const char* message){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);while(!predicate()){require(std::chrono::steady_clock::now()<deadline,message);std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
std::string sha(std::span<const uint8_t> bytes){gc::Sha256 hash;hash.update(bytes.data(),bytes.size());return gc::Sha256::hex(hash.finish());}
std::shared_ptr<const CustomMapPackage> package(size_t count=4*1024*1024,bool preview=true){
    auto p=std::make_shared<CustomMapPackage>();auto map=std::make_shared<std::vector<uint8_t>>(count);for(size_t i=0;i<count;++i)(*map)[i]=uint8_t((i*37)^(i>>8));p->map=map;
    p->manifest.id="transfer_test";p->manifest.title="Test map \xc3\xa4";p->manifest.base_level="Rlevel1_1";p->manifest.modes=3;p->manifest.map_bytes=count;p->manifest.map_sha256=sha(*map);
    const auto identity=p->manifest.id+'\n'+p->manifest.map_sha256;p->manifest.alias="Custom_"+sha({reinterpret_cast<const uint8_t*>(identity.data()),identity.size()}).substr(0,24);
    if(preview){auto png=std::make_shared<std::vector<uint8_t>>(1111,133);p->preview=png;p->manifest.preview_bytes=png->size();p->manifest.preview_sha256=sha(*png);}
    p->manifest.package_sha256=custom_map_package_fingerprint(p->manifest);return p;
}
CustomMapRequest request(const CustomMapPackage& p,uint64_t epoch=1){return {epoch,p.manifest.alias,p.manifest.id,p.manifest.map_sha256};}
WorldState custom_world(const CustomMapPackage& p,uint64_t epoch=1){WorldState world;world.epoch=epoch;world.tick=1;world.level=p.manifest.alias;world.mission["Session.Custom.Id"]=p.manifest.id;world.mission["Session.Custom.Alias"]=p.manifest.alias;world.mission["Session.Custom.SHA256"]=p.manifest.map_sha256;return world;}
void full_receive(CustomMapReceiver& receiver,uint64_t token,const CustomMapPackage& p){
    std::string error;require(receiver.offer(token,1,p.manifest,error),error.c_str());
    std::vector<uint8_t> bytes=*p.map;if(p.preview)bytes.insert(bytes.end(),p.preview->begin(),p.preview->end());
    for(size_t offset=0;offset<bytes.size();){const auto count=std::min<size_t>(custom_map_chunk_bytes,bytes.size()-offset);require(receiver.chunk(token,1,offset,std::span<const uint8_t>(bytes).subspan(offset,count),error),error.c_str());offset+=count;}
    require(receiver.finish(token,1,error),error.c_str());
}
void receiver_tests(){
    std::string error;const auto p=package(custom_map_chunk_bytes+17);const auto r=request(*p);require(custom_map_package_valid(*p,error),error.c_str());
    auto encoded=encode_custom_map_manifest(p->manifest);CustomMapManifest decoded;require(decode_custom_map_manifest(encoded,decoded,error)&&decoded==p->manifest,"Manifest roundtrip");
    auto bad=p->manifest;bad.id="../escape";bad.package_sha256=custom_map_package_fingerprint(bad);require(!custom_map_manifest_valid(bad,error),"Traversal id accepted");
    bad=p->manifest;bad.base_level="C:\\outside";bad.package_sha256=custom_map_package_fingerprint(bad);require(!custom_map_manifest_valid(bad,error),"Absolute base path accepted");
    bad=p->manifest;bad.map_bytes=custom_map_max_bytes+1;bad.package_sha256=custom_map_package_fingerprint(bad);require(!custom_map_manifest_valid(bad,error),"Oversized map accepted");
    bad=p->manifest;bad.preview_bytes=custom_preview_max_bytes+1;bad.package_sha256=custom_map_package_fingerprint(bad);require(!custom_map_manifest_valid(bad,error),"Oversized preview accepted");
    bad=p->manifest;bad.title="bad\nname";bad.package_sha256=custom_map_package_fingerprint(bad);require(!custom_map_manifest_valid(bad,error),"Control title accepted");
    bad=p->manifest;bad.title="\xed\xa0\x80";bad.package_sha256=custom_map_package_fingerprint(bad);require(!custom_map_manifest_valid(bad,error),"Invalid UTF8 title accepted");
    bad=p->manifest;bad.alias="Custom_aaaaaaaaaaaaaaaaaaaaaaaa";bad.package_sha256=custom_map_package_fingerprint(bad);require(!custom_map_manifest_valid(bad,error),"Noncanonical alias accepted");
    bad=p->manifest;bad.title="Changed";require(!custom_map_manifest_valid(bad,error),"Stale package fingerprint accepted");
    encoded.push_back(0);require(!decode_custom_map_manifest(encoded,decoded,error),"Trailing manifest data accepted");
    CustomMapReceiver receiver;require(receiver.request(1,r,error),error.c_str());full_receive(receiver,1,*p);auto result=receiver.take();require(result&&!result->cache_hit&&*result->package->map==*p->map&&*result->package->preview==*p->preview,"Completed payload differs");require(!receiver.take(),"Completed payload taken twice");
    require(receiver.request(2,r,error),error.c_str());require(receiver.offer(2,1,p->manifest,error),error.c_str());require(!receiver.chunk(2,1,1,std::span<const uint8_t>(*p->map).first(1),error),"Out-of-order chunk accepted");require(receiver.status().phase==CustomMapTransferPhase::Failed,"Bad offset did not fail receiver");
    require(receiver.request(3,r,error),error.c_str());require(receiver.offer(3,1,p->manifest,error),error.c_str());require(receiver.chunk(3,1,0,std::span<const uint8_t>(*p->map).first(10),error),error.c_str());require(!receiver.chunk(3,1,0,std::span<const uint8_t>(*p->map).first(10),error),"Replayed chunk accepted");
    require(receiver.request(4,r,error),error.c_str());require(receiver.offer(4,1,p->manifest,error),error.c_str());require(!receiver.chunk(4,1,0,std::span<const uint8_t>(*p->map).first(custom_map_chunk_bytes+1),error),"Oversized chunk accepted");
    require(receiver.request(5,r,error),error.c_str());require(receiver.offer(5,1,p->manifest,error),error.c_str());receiver.cancel("User cancelled");require(receiver.chunk(5,1,0,std::span<const uint8_t>(*p->map).first(1),error)&&receiver.finish(5,1,error),"Late cancelled payload was not ignored");require(!receiver.take()&&receiver.status().phase==CustomMapTransferPhase::Cancelled,"Cancelled transfer resurrected");
    require(receiver.request(6,r,error),error.c_str());require(receiver.chunk(5,1,0,std::span<const uint8_t>(*p->map).first(1),error),"Retired token disconnected receiver");require(receiver.offer(6,2,p->manifest,error)&&receiver.status().phase==CustomMapTransferPhase::Requesting,"Wrong epoch mutated receiver");full_receive(receiver,6,*p);require(receiver.take().has_value(),"Replacement transfer failed");
    const auto small=package(123,false);auto corrupt=std::make_shared<CustomMapPackage>(*small);auto bytes=std::make_shared<std::vector<uint8_t>>(*small->map);(*bytes)[0]^=1;corrupt->map=bytes;
    require(!custom_map_package_valid(*corrupt,error),"Corrupt host payload accepted");require(receiver.request(7,request(*small),error),error.c_str());require(receiver.offer(7,1,small->manifest,error),error.c_str());require(receiver.chunk(7,1,0,*bytes,error),error.c_str());require(!receiver.finish(7,1,error)&&!receiver.take(),"Corrupt client payload completed");
    require(receiver.request(8,request(*small),error),error.c_str());full_receive(receiver,8,*small);require(receiver.take().has_value(),"Preview-less transfer failed");
    auto cached=r;cached.known_package_sha256=p->manifest.package_sha256;require(receiver.request(9,cached,error),error.c_str());require(receiver.offer(9,1,p->manifest,error),error.c_str());require(receiver.finish(9,1,error,true),error.c_str());result=receiver.take();require(result&&result->cache_hit&&!result->package->map&&result->package->manifest==p->manifest,"Cache reuse copied or lost manifest");
    require(receiver.request(10,r,error),error.c_str());require(receiver.offer(10,1,p->manifest,error),error.c_str());require(!receiver.finish(10,1,error,true),"Unverified cached completion accepted");
}
void put32(std::vector<uint8_t>& b,uint32_t n){for(int i=3;i>=0;--i)b.push_back(uint8_t(n>>(i*8)));}
void put64(std::vector<uint8_t>& b,uint64_t n){put32(b,uint32_t(n>>32));put32(b,uint32_t(n));}
void text(std::vector<uint8_t>& b,const std::string& s){put32(b,uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
void send_frame(SOCKET socket,wire::Message type,std::vector<uint8_t> body){std::vector<uint8_t> frame;put32(frame,wire::magic);frame.push_back(0);frame.push_back(wire::version);frame.push_back(0);frame.push_back(uint8_t(type));put32(frame,uint32_t(body.size()));frame.insert(frame.end(),body.begin(),body.end());size_t offset=0;while(offset<frame.size()){const auto n=send(socket,reinterpret_cast<const char*>(frame.data()+offset),int(frame.size()-offset),0);require(n>0,"Raw frame send");offset+=size_t(n);}}
SOCKET raw_peer(uint16_t port){auto socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(socket!=INVALID_SOCKET,"Raw socket");sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(port);require(connect(socket,reinterpret_cast<sockaddr*>(&target),sizeof(target))==0,"Raw connect");std::vector<uint8_t> hello;put64(hello,1234);text(hello,"123456");text(hello,"Raw client");send_frame(socket,wire::Message::Hello,std::move(hello));return socket;}
void network_tests(){
    const auto p=package();auto world=custom_world(*p);AuthoritativeServer host;require(host.start(0,"123456",1234,"Transfer host"),"Host start");std::string error;require(host.publish(world,error),error.c_str());require(host.set_custom_map_package(1,p,error),error.c_str());
    AuthoritativeClient client,observer;require(client.start("127.0.0.1",host.status().port,"123456",1234,"Downloader"),"Client start");require(observer.start("127.0.0.1",host.status().port,"123456",1234,"Observer"),"Observer start");wait_for([&]{const auto a=client.status(),b=observer.status();require(a.connecting||a.connected,a.message.c_str());require(b.connecting||b.connected,b.message.c_str());return client.latest_world()->level==world.level&&observer.latest_world()->level==world.level;},"Initial custom world");
    const auto r=request(*p);require(client.request_custom_map(r,error),error.c_str());unsigned publications=0;uint64_t observer_updates=0,last_observed=observer.latest_world()->revision;
    wait_for([&]{require(client.status().connected,"Client disconnected while transferring");const auto progress=client.custom_map_transfer_status();require(progress.phase!=CustomMapTransferPhase::Failed,progress.error.c_str());require(progress.received<=progress.total&&progress.total<=custom_map_max_bytes+custom_preview_max_bytes,"Unbounded receive progress");if(progress.phase==CustomMapTransferPhase::Complete)return true;++world.tick;require(host.publish(world,error),error.c_str());++publications;const auto revision=observer.latest_world()->revision;if(revision!=last_observed){++observer_updates;last_observed=revision;}return false;},"Full custom map transfer");
    auto download=client.take_custom_map_package();require(download&&!download->cache_hit&&download->package->manifest==p->manifest&&*download->package->map==*p->map&&*download->package->preview==*p->preview,"TCP payload differs");require(publications>3&&observer_updates>0,"Transfer blocked other world publications");
    const auto before=client.status().bytes_received;auto cached=r;cached.known_package_sha256=p->manifest.package_sha256;require(client.request_custom_map(cached,error),error.c_str());wait_for([&]{return client.custom_map_transfer_status().phase==CustomMapTransferPhase::Complete;},"Cached transfer");download=client.take_custom_map_package();require(download&&download->cache_hit&&!download->package->map,"TCP cached result missing");require(client.status().bytes_received-before<96*1024,"Cache reuse transferred map chunks");
    // Identical native map bytes still require refreshed host title/preview.
    auto updated=std::make_shared<CustomMapPackage>(*p);updated->manifest.title="Host renamed this map";auto preview=std::make_shared<std::vector<uint8_t>>(2222,77);updated->preview=preview;updated->manifest.preview_bytes=preview->size();updated->manifest.preview_sha256=sha(*preview);updated->manifest.package_sha256=custom_map_package_fingerprint(updated->manifest);require(host.set_custom_map_package(1,updated,error),error.c_str());require(client.request_custom_map(cached,error),error.c_str());wait_for([&]{return client.custom_map_transfer_status().phase==CustomMapTransferPhase::Complete;},"Changed metadata transfer");download=client.take_custom_map_package();require(download&&!download->cache_hit&&download->package->manifest.title==updated->manifest.title&&*download->package->preview==*preview,"Changed host metadata reused stale cache");
    require(client.request_custom_map(r,error),error.c_str());client.cancel_custom_map("Cancelled by test");std::this_thread::sleep_for(std::chrono::milliseconds(60));require(client.status().connected&&!client.take_custom_map_package()&&client.custom_map_transfer_status().phase==CustomMapTransferPhase::Cancelled,"TCP cancellation resurrected package");
    host.clear_custom_map_package();require(client.request_custom_map(r,error),error.c_str());host.fail_custom_map_package(1,p->manifest.alias,"Test preparation failure");wait_for([&]{return client.custom_map_transfer_status().phase==CustomMapTransferPhase::Failed;},"Host preparation error missing");require(client.custom_map_transfer_status().error=="Test preparation failure"&&client.status().connected&&observer.status().connected,"Package failure disconnected ordinary gameplay");
    host.clear_custom_map_package();require(client.request_custom_map(r,error),error.c_str());world=WorldState{};world.epoch=2;world.level="Rlevel1_2";world.tick=1;require(host.publish(world,error),error.c_str());wait_for([&]{return client.latest_world()->epoch==2;},"New level publication");require(client.custom_map_transfer_status().phase==CustomMapTransferPhase::Cancelled&&!client.take_custom_map_package(),"New level did not retire download");require(!client.request_custom_map(r,error),"Old epoch requested after level change");require(!host.set_custom_map_package(1,p,error),"Old package published into new epoch");
    // An authenticated peer cannot request an arbitrary/non-selected package.
    auto raw=raw_peer(host.status().port);wait_for([&]{return host.status().joined==4;},"Raw assignment");std::vector<uint8_t> bad;put64(bad,1);put64(bad,2);text(bad,"../../outside");text(bad,p->manifest.id);text(bad,p->manifest.map_sha256);text(bad,"");send_frame(raw,wire::Message::CustomMapRequest,std::move(bad));wait_for([&]{return host.status().joined==3;},"Malformed path request accepted");closesocket(raw);require(client.status().connected&&observer.status().connected,"Adversarial request affected other peers");
    client.stop();observer.stop();host.stop();
}

uint64_t raw_u64(const std::vector<uint8_t>& bytes,size_t offset=0){require(offset<=bytes.size() && bytes.size()-offset>=8,"Short raw token");uint64_t value=0;for(unsigned i=0;i<8;++i)value=(value<<8)|bytes[offset+i];return value;}
std::pair<wire::Message,std::vector<uint8_t>> receive_raw_frame(SOCKET socket){
    auto receive=[&](uint8_t* bytes,size_t size){while(size){const auto n=recv(socket,reinterpret_cast<char*>(bytes),int(size),0);require(n>0,"Raw TCP response timed out or disconnected");bytes+=n;size-=size_t(n);}};
    uint8_t h[12];receive(h,sizeof(h));const auto size=uint32_t(h[8])<<24|uint32_t(h[9])<<16|uint32_t(h[10])<<8|h[11];require(size<=max_frame_bytes,"Oversized raw TCP response");std::vector<uint8_t> body(size);receive(body.data(),body.size());return {wire::Message(uint16_t(h[6])<<8|h[7]),std::move(body)};
}
void raw_map_request(SOCKET socket,uint64_t token,const CustomMapRequest& request){std::vector<uint8_t> bytes;put64(bytes,token);put64(bytes,request.epoch);text(bytes,request.alias);text(bytes,request.id);text(bytes,request.map_sha256);text(bytes,request.known_package_sha256);send_frame(socket,wire::Message::CustomMapRequest,std::move(bytes));}
void acknowledge_raw_frame(SOCKET socket,wire::Message type,const std::vector<uint8_t>& bytes){
    if(type==wire::Message::World){WorldDelta delta;std::string error;require(decode_delta(bytes,delta,error),error.c_str());std::vector<uint8_t> ack;put64(ack,delta.revision);send_frame(socket,wire::Message::WorldAck,std::move(ack));}
    else if(type==wire::Message::Ping)send_frame(socket,wire::Message::Pong,bytes);
}
void ordered_lobby_transfer_retirement_test(){
    const auto a=package(4*1024*1024,false),b=package(custom_map_chunk_bytes+11,false);auto world=custom_world(*a,2);world.level="Rlevel1_1";world.mission["Session.NativePhase"]=std::string("menu");world.mission["Session.Lobby.Level"]=a->manifest.alias;
    AuthoritativeServer host(2);std::string error;require(host.start(0,"123456",1234,"Ordered transfer host") && host.publish(world,error) && host.set_custom_map_package(2,a,error),"Ordered transfer host setup");auto raw=raw_peer(host.status().port);DWORD timeout=5000;setsockopt(raw,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));wait_for([&]{return host.status().joined==2;},"Ordered transfer peer assignment");raw_map_request(raw,1,request(*a,2));const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);auto receive=[&]{require(std::chrono::steady_clock::now()<deadline,"Ordered transfer response deadline");return receive_raw_frame(raw);};
    // Observe a real chunk before changing the selection. The client has not
    // consumed the new authoritative metadata when these later requests send.
    for(;;){auto [type,bytes]=receive();require(type!=wire::Message::CustomMapError,"Initial ordered transfer failed");acknowledge_raw_frame(raw,type,bytes);if(type==wire::Message::CustomMapChunk && raw_u64(bytes)==1)break;}
    world.mission["Session.Lobby.Level"]=b->manifest.alias;world.mission["Session.Custom.Id"]=b->manifest.id;world.mission["Session.Custom.Alias"]=b->manifest.alias;world.mission["Session.Custom.SHA256"]=b->manifest.map_sha256;++world.tick;require(host.publish(world,error) && host.set_custom_map_package(2,b,error),"Ordered replacement publication");
    raw_map_request(raw,2,request(*a,2));std::vector<uint8_t> fence;put64(fence,101);send_frame(raw,wire::Message::Ping,fence);
    for(;;){auto [type,bytes]=receive();require(type!=wire::Message::CustomMapError,"Normal selection change or late old request emitted fatal transfer error");acknowledge_raw_frame(raw,type,bytes);if(type==wire::Message::Pong && raw_u64(bytes)==101)break;}
    CustomMapReceiver receiver;require(receiver.request(3,request(*b,2),error),error.c_str());raw_map_request(raw,3,request(*b,2));std::vector<uint8_t> cancel;put64(cancel,1);put64(cancel,2);send_frame(raw,wire::Message::CustomMapCancel,std::move(cancel));raw_map_request(raw,4,request(*a,2));raw_map_request(raw,5,request(*b,1));
    // A late higher-token old-alias request consumes its replay floor but may
    // not replace or cancel the valid transfer already using token3.
    while(receiver.status().phase!=CustomMapTransferPhase::Complete){
        auto [type,bytes]=receive();require(type!=wire::Message::CustomMapError,"Late request/cancel interrupted the replacement transfer");acknowledge_raw_frame(raw,type,bytes);
        if(type==wire::Message::CustomMapOffer && raw_u64(bytes)==3){CustomMapManifest manifest;require(decode_custom_map_manifest(std::span<const uint8_t>(bytes).subspan(16),manifest,error) && receiver.offer(3,raw_u64(bytes,8),manifest,error),error.c_str());}
        else if(type==wire::Message::CustomMapChunk && raw_u64(bytes)==3)require(receiver.chunk(3,raw_u64(bytes,8),raw_u64(bytes,16),std::span<const uint8_t>(bytes).subspan(24),error),error.c_str());
        else if(type==wire::Message::CustomMapComplete && raw_u64(bytes)==3){require(bytes.size()==17 && bytes[16]<=1,"Invalid ordered completion");require(receiver.finish(3,raw_u64(bytes,8),error,bytes[16]!=0),error.c_str());}
    }
    const auto received=receiver.take();require(received && *received->package->map==*b->map && received->package->manifest==b->manifest && host.status().joined==2,"Replacement transfer mixed bytes or lost authenticated peer");
    world.mission["Session.Custom.Id"]=std::string("wrong_identity");++world.tick;require(host.publish(world,error),error.c_str());raw_map_request(raw,6,request(*b,2));
    for(;;){auto [type,bytes]=receive();acknowledge_raw_frame(raw,type,bytes);if(type==wire::Message::CustomMapError){require(raw_u64(bytes)==6 && host.status().joined==2,"Current-alias identity error was ignored or disconnected peer");break;}}
    // Identity-format corruption is still rejected; silent retirement applies
    // only to well-formed requests whose proposal context was superseded.
    auto malformed=request(*b,2);malformed.id="wrong_identity";raw_map_request(raw,7,malformed);wait_for([&]{return host.status().joined==1;},"Current-alias malformed identity was not rejected");closesocket(raw);host.stop();
}
void lobby_selection_tests(){
    WorldState world;world.level="Rlevel1_1";world.mission["Session.NativePhase"]=std::string("menu");world.mission["Session.Lobby.Level"]=std::string("Custom_proposal");
    require(selected_custom_map_alias(world)=="Custom_proposal" && world.level=="Rlevel1_1" && world.epoch==1,"Menu proposal mutated or failed to select alias");
    for(const auto phase:{"loading","gameplay"}){world.mission["Session.NativePhase"]=std::string(phase);require(selected_custom_map_alias(world)==world.level,"Non-menu phase used lobby proposal");}
    world.mission["Session.NativePhase"]=std::string("menu");world.mission["Session.Lobby.Level"]=std::string();require(selected_custom_map_alias(world).empty(),"Empty menu proposal fell back to an old active map");
    world.mission["Session.Lobby.Level"]=int64_t(1);require(selected_custom_map_alias(world).empty(),"Invalid menu proposal fell back to an old active map");
    world.mission.erase("Session.Lobby.Level");require(selected_custom_map_alias(world)==world.level,"Legacy menu without proposal lost active level");
}
void lobby_network_tests(){
    const auto p=package(4*1024*1024,false),other=package(4*1024*1024+1,false);auto world=custom_world(*p);world.level="Rlevel1_1";world.mission["Session.NativePhase"]=std::string("menu");world.mission["Session.Lobby.Level"]=p->manifest.alias;
    AuthoritativeServer host(2);require(host.start(0,"123456",1234,"Lobby transfer host"),"Lobby transfer host start");std::string error;require(host.publish(world,error),error.c_str());require(host.set_custom_map_package(1,p,error),error.c_str());require(!host.set_custom_map_package(1,other,error),"Host offered an unselected lobby package");
    AuthoritativeClient client;require(client.start("127.0.0.1",host.status().port,"123456",1234,"Lobby downloader"),"Lobby downloader start");wait_for([&]{return client.status().connected && selected_custom_map_alias(*client.latest_world())==p->manifest.alias;},"Lobby proposal publication");
    require(!client.request_custom_map(request(*other),error),"Client requested a different lobby proposal");require(!client.request_custom_map(request(*p,2),error),"Client requested wrong lobby epoch");
    host.clear_custom_map_package();require(client.request_custom_map(request(*p),error),error.c_str());host.fail_custom_map_package(1,p->manifest.alias,"Lobby package preparation failed");wait_for([&]{return client.custom_map_transfer_status().phase==CustomMapTransferPhase::Failed;},"Menu package failure was not delivered");require(client.custom_map_transfer_status().error=="Lobby package preparation failed" && client.status().connected,"Menu package failure affected connection");
    require(host.set_custom_map_package(1,p,error),error.c_str());host.fail_custom_map_package(1,other->manifest.alias,"Wrong proposal failure");require(client.request_custom_map(request(*p),error),error.c_str());
    bool reset_while_receiving=false;int64_t token_before=0;
    wait_for([&]{
        const auto status=client.custom_map_transfer_status();require(client.status().connected && status.phase!=CustomMapTransferPhase::Failed && status.phase!=CustomMapTransferPhase::Cancelled,"Readiness update interrupted lobby download");
        if(status.phase==CustomMapTransferPhase::Complete)return true;
        if(status.phase==CustomMapTransferPhase::Receiving && !reset_while_receiving){token_before=std::get<int64_t>(host.latest_world()->mission.at("Session.Lobby.ReadyToken"));host.reset_lobby_readiness();host.lobby_ready(true);reset_while_receiving=true;}
        ++world.tick;require(host.publish(world,error),error.c_str());return false;
    },"Lobby download with readiness reset");
    require(reset_while_receiving && std::get<int64_t>(host.latest_world()->mission.at("Session.Lobby.ReadyToken"))!=token_before,"Test never reset readiness during active chunks");
    auto received=client.take_custom_map_package();require(received && *received->package->map==*p->map,"Lobby download payload differs");require(host.latest_world()->level=="Rlevel1_1" && host.latest_world()->epoch==1 && selected_custom_map_alias(*host.latest_world())==p->manifest.alias,"Lobby pre-download changed active level/epoch");
    require(client.request_custom_map(request(*p),error),error.c_str());wait_for([&]{return client.custom_map_transfer_status().phase==CustomMapTransferPhase::Receiving;},"Second lobby transfer did not receive chunks");
    world.mission["Session.Lobby.Level"]=std::string("Rlevel1_2");for(auto it=world.mission.begin();it!=world.mission.end();)if(it->first.starts_with("Session.Custom."))it=world.mission.erase(it);else ++it;++world.tick;require(host.publish(world,error),error.c_str());
    wait_for([&]{return selected_custom_map_alias(*client.latest_world())=="Rlevel1_2" && client.custom_map_transfer_status().phase==CustomMapTransferPhase::Cancelled;},"Same-epoch menu selection did not cancel old download");require(!client.take_custom_map_package() && !client.request_custom_map(request(*p),error),"Retired lobby package remained requestable");
    world.mission["Session.Lobby.Level"]=other->manifest.alias;world.mission["Session.Custom.Id"]=other->manifest.id;world.mission["Session.Custom.Alias"]=other->manifest.alias;world.mission["Session.Custom.SHA256"]=other->manifest.map_sha256;++world.tick;require(host.publish(world,error),error.c_str());require(host.set_custom_map_package(1,other,error),error.c_str());
    wait_for([&]{return selected_custom_map_alias(*client.latest_world())==other->manifest.alias;},"Replacement lobby proposal missing");require(client.request_custom_map(request(*other),error),error.c_str());wait_for([&]{const auto status=client.custom_map_transfer_status();require(status.phase!=CustomMapTransferPhase::Failed,status.error.c_str());return status.phase==CustomMapTransferPhase::Complete;},"Same-epoch replacement lobby download");received=client.take_custom_map_package();require(received && *received->package->map==*other->map && received->package->manifest==other->manifest,"Replacement lobby package mixed old transfer data");
    world.mission["Session.NativePhase"]=std::string("loading");++world.tick;require(host.publish(world,error),error.c_str());wait_for([&]{return client.latest_world()->tick==world.tick;},"Loading phase publication");require(!client.request_custom_map(request(*other),error) && !host.set_custom_map_package(1,other,error),"Non-menu old active level accepted proposal package");
    client.stop();host.stop();
}

}
int main(){_putenv_s("SSNE_TEST_HIDDEN","1");receiver_tests();network_tests();lobby_selection_tests();lobby_network_tests();ordered_lobby_transfer_retirement_test();std::cout<<"Custom map transfer validation, TCP, cancellation, cache and concurrency passed.\n";}
