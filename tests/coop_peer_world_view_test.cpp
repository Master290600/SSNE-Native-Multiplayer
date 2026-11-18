#include "authoritative_session.h"
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>
using namespace port::coop;
namespace {
void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
void wait_for(const std::function<bool()>& done,const char* why){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(!done()){require(std::chrono::steady_clock::now()<deadline,why);std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
std::vector<uint8_t> bytes(const WorldState& w){return encode_delta(difference(nullptr,w));}
const std::vector<int64_t>& rockets(const WorldState& w){return std::get<std::vector<int64_t>>(w.mission.at("Session.RocketEffects.Ids"));}
bool window(const AuthoritativeClient& client,uint64_t tick,int64_t first,int64_t last){const auto w=client.latest_world();if(w->tick!=tick)return false;auto it=w->mission.find("Session.RocketEffects.Ids");if(it==w->mission.end())return false;const auto* ids=std::get_if<std::vector<int64_t>>(&it->second);return ids && (first?(!ids->empty() && ids->front()==first && ids->back()==last):ids->empty());}
void event_window(WorldState& w,unsigned first,unsigned last){std::vector<int64_t> ids,styles;std::vector<float> values;for(unsigned n=first;n<=last;++n){ids.push_back(n);styles.push_back(n%2);for(unsigned k=0;k<10;++k)values.push_back(float(n+k)*.01f);}w.mission["Session.RocketEffects.Ids"]=std::move(ids);w.mission["Session.RocketEffects.Styles"]=std::move(styles);w.mission["Session.RocketEffects.Values"]=std::move(values);}
void player(WorldState& w,PlayerId p){EntityState e;e.id=10+p.slot;e.owner=p;e.kind=EntityKind::Player;e.definition=2;e.properties={{"player_slot",int64_t(p.slot)},{"health",100.f},{"position",std::array<float,3>{float(p.slot),1,2}}};w.entities.emplace(e.id,std::move(e));}
void attach(AuthoritativeServer& host,const WorldState& w){for(const auto& [id,e]:w.entities)if(e.kind==EntityKind::Player)require(host.attach_character(e.owner,id),"Character attach failed");require(host.ready_host(),"Host readiness failed");}
}
int main(){
    AuthoritativeServer host(3);require(host.start(0,"123456",1234,"View host"),"Host start failed");AuthoritativeClient slow,fast;
    require(slow.start("127.0.0.1",host.status().port,"123456",1234,"Slow cursor"),"Slow client start failed");require(fast.start("127.0.0.1",host.status().port,"123456",1234,"Fast cursor"),"Fast client start failed");
    wait_for([&]{return slow.status().connected && fast.status().connected;},"Peer connection timeout");
    WorldState w;w.level="Rlevel1_2";w.tick=1;player(w,host.status().player);player(w,slow.status().player);player(w,fast.status().player);event_window(w,1,0);
    for(unsigned n=0;n<16;++n){EntityState e;e.id=100+n;e.kind=EntityKind::Enemy;e.definition=n%61;e.properties={{"health",100.f},{"position",std::array<float,3>{float(n),0,0}},{"enemy_bone_matrices",std::vector<float>(3072,float(n)*.125f)}};w.entities.emplace(e.id,std::move(e));}
    std::string error;require(host.publish(w,error),error.c_str());attach(host,w);
    wait_for([&]{return slow.latest_world()->entities.contains(10+slow.status().player.slot) && fast.latest_world()->entities.contains(10+fast.status().player.slot);},"Initial graph not delivered");slow.ready(1);fast.ready(1);
    wait_for([&]{return host.participants()[slow.status().player.slot].ready && host.participants()[fast.status().player.slot].ready;},"Peers did not become ready");
    // Both consumers begin at zero, then acknowledge different offered windows.
    auto revision=host.latest_world()->revision;slow.acknowledge_native_events(1,NativeEventCursors{});fast.acknowledge_native_events(1,NativeEventCursors{});
    wait_for([&]{return host.latest_world()->revision>=revision+2;},"Event consumers were not initialized");
    w.tick=2;event_window(w,1,96);w.entities.at(100).properties["health"]=83.f;require(host.publish(w,error),error.c_str());
    wait_for([&]{return window(slow,2,1,96) && window(fast,2,1,96);},"Initial retained event windows differ");
    const auto retained_slow=slow.latest_world(),retained_fast=fast.latest_world();const auto saved_slow=bytes(*retained_slow),saved_fast=bytes(*retained_fast);
    NativeEventCursors consumed{};consumed[1]=96;fast.acknowledge_native_events(1,consumed);
    wait_for([&]{return window(fast,2,0,0);},"Fast consumer acknowledgement did not clear only its own overlay");require(window(slow,2,1,96),"One peer acknowledgement changed another peer overlay");
    w.tick=3;event_window(w,97,192);w.entities.erase(101);auto born=w.entities.at(102);born.id=999;born.resource="New enemy";w.entities.emplace(born.id,std::move(born));w.entities.at(100).properties["enemy_bone_matrices"]=std::vector<float>(4096,.75f);require(host.publish(w,error),error.c_str());
    wait_for([&]{return window(slow,3,1,128) && window(fast,3,97,192);},"Two peer cursor overlays were not retained independently");
    const auto body=host.latest_world();const auto body_bytes=bytes(*body);const auto slow_revision=slow.latest_world()->revision;const auto receive_before=slow.status().bytes_received;
    slow.request_snapshot();wait_for([&]{return slow.status().bytes_received>receive_before+4096;},"ForceFull was suppressed at the same revision");
    require(window(slow,3,1,128) && slow.latest_world()->revision==slow_revision,"ForceFull changed the peer's exact mission baseline");
    consumed[1]=128;slow.acknowledge_native_events(1,consumed);wait_for([&]{return window(slow,3,129,192);},"Remaining retained events did not follow cursor-only rebuild");
    require(slow.latest_world()->entities==body->entities && fast.latest_world()->entities==body->entities,"Cursor-only rebuild changed captured entities/bones");
    consumed[1]=192;fast.acknowledge_native_events(1,consumed);slow.acknowledge_native_events(1,consumed);wait_for([&]{return window(slow,3,0,0) && window(fast,3,0,0);},"Peer event windows did not drain independently");
    require(bytes(*body)==body_bytes && bytes(*retained_slow)==saved_slow && bytes(*retained_fast)==saved_fast,"Published/retained snapshots mutated after cursor updates");
    w.epoch=2;w.tick=0;w.level="Alevel10_4";event_window(w,1,0);require(host.publish(w,error),error.c_str());wait_for([&]{return slow.latest_world()->epoch==2 && fast.latest_world()->epoch==2;},"Epoch invalidation/full baseline failed");
    attach(host,w);slow.ready(2);fast.ready(2);wait_for([&]{return host.participants()[slow.status().player.slot].ready && host.participants()[fast.status().player.slot].ready;},"New epoch readiness failed");
    require(bytes(*retained_slow)==saved_slow && bytes(*retained_fast)==saved_fast,"New epoch released or mutated retained old overlays");
    fast.stop();slow.stop();host.stop();std::cout<<"Real TCP peer world views: two independent retained event cursors, cursor-only revisions, ForceFull, large bone arrays, create/remove, immutable old snapshots and epoch readiness passed.\n";
}
