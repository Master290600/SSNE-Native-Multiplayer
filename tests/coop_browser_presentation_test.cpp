#include "frontend_text.h"
#include "lan_discovery.h"
#include <iostream>
#include <stdexcept>
using namespace port::coop;
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
constexpr uint64_t now=100000;
FrontendRoom room(std::string id="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",std::string name="Sam"){
    FrontendRoom out;out.address="127.0.0.1";out.name=std::move(name);out.port=27341;out.players=3;out.capacity=8;
    out.session_id=std::move(id);out.join_ticket="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";out.join_expires_ms=now+60000;
    out.details_known=true;out.mode=1;out.humans=2;out.bots=1;out.ping_ms=42;out.seen_ms=now;out.probe_ms=now-42;return out;
}
void labels_and_freshness(){
    auto current=room();
    check(lan_room_fresh(current,now+lan_room_fresh_ms),"Freshness boundary expired early");
    check(!lan_room_fresh(current,now+lan_room_fresh_ms+1),"Old availability stayed fresh");
    check(!lan_room_fresh(current,now-1),"Future observation accepted as current");
    for(unsigned language=0;language<3;++language){
        auto label=frontend_room_row(current,std::string(64,'W'),language,now);
        check(label.size()<=36 && label.find("...")!=std::string::npos && label.find("(3/8)")!=std::string::npos && label.find("42ms")!=std::string::npos,"Bounded row lost occupancy or measured RTT");
        check(label.find(frontend_text(FrontText::Lobby,language))!=std::string::npos,"Known lobby status missing");
        current.running=true;label=frontend_room_row(current,current.name,language,now);
        check(label.find(frontend_text(FrontText::InProgress,language))!=std::string::npos,"Running status missing");
        current.details_known=false;
        check(frontend_room_state(current,now)==FrontText::StatusUnknown,"Legacy running bit became a known status");
        current.running=false;
        check(frontend_room_state(current,now)==FrontText::StatusUnknown,"Legacy room was falsely called Lobby");
        label=frontend_room_row(current,current.name,language,now);
        check(label.find(frontend_text(FrontText::StatusUnknown,language))!=std::string::npos,"Localized unknown status missing");
        current.details_known=true;
    }
    current.ping_ms=UINT32_MAX;check(frontend_room_ping(current,now,true)=="?ms","Unknown RTT presented as a measurement");
    current.ping_ms=9000;check(frontend_room_ping(current,now,true)=="999+ms","Large RTT overflowed row budget");
    check(frontend_room_state(current,now+30001)==FrontText::Stale && frontend_room_ping(current,now+30001,true)=="?ms","Stale room retained current status/RTT");
    auto stale=current;stale.seen_ms=now-30001;stale.name="A old";stale.ping_ms=1;
    auto fresh=room();fresh.name="Z live";
    check(sorted_lan_rooms({stale,fresh},LanRoomSort::Ping,{},now).front().name==fresh.name,"Stale low RTT sorted ahead of current measurement");
    check(sorted_lan_rooms({stale,fresh},LanRoomSort::Ping,{0,true,false},now).size()==1,"Free-slot filter advertised stale availability");
}
void identity_and_selection(){
    auto a=room(),b=room("cccccccccccccccccccccccccccccccc","Second");
    auto snapshot=std::vector<FrontendRoom>{a,b};
    a.ping_ms=100;b.ping_ms=2;
    auto fresh=sorted_lan_rooms({a,b},LanRoomSort::Ping,{},now);
    check(fresh.front().session_id==b.session_id,"Explicit ping sort did not reorder");
    auto stable=stable_lan_room_view(snapshot,fresh);
    check(stable.front().session_id==a.session_id,"New RTT reply moved a displayed server");
    check(stable_lan_room_view({},fresh).front().session_id==b.session_id,"Explicit refresh/sort retained old ordering");
    check(!selected_lan_room(a,{b},now),"Removed displayed room selected its replacement row");
    auto moved=a;moved.address="192.168.1.10";moved.join_ticket="dddddddddddddddddddddddddddddddd";
    const auto selected=selected_lan_room(a,{b,moved},now);
    check(selected && selected->session_id==a.session_id && selected->address==moved.address && selected->join_ticket==moved.join_ticket,"Stable session selection did not take the latest endpoint and permit together");
    moved.join_expires_ms=now;check(!selected_lan_room(a,{moved},now),"Expired direct-join credential remained selectable");
    auto protected_room=a;protected_room.join_ticket.clear();protected_room.join_expires_ms=0;
    const auto manual=selected_lan_room(a,{protected_room},now);
    check(manual && manual->session_id==a.session_id && manual->join_ticket.empty(),"Fresh protected room lost its manual PIN route");
    protected_room.join_expires_ms=now+1;check(!selected_lan_room(a,{protected_room},now),"Missing ticket with a fabricated lifetime was accepted");
    protected_room.join_expires_ms=0;protected_room.session_id="invalid";check(!selected_lan_room(protected_room,{protected_room},now),"Protected room accepted an invalid session identity");
    moved=a;moved.seen_ms=now-30001;check(!selected_lan_room(a,{moved},now),"Stale displayed session remained selectable");
    auto legacy=a;legacy.session_id.clear();legacy.join_ticket.clear();legacy.join_expires_ms=0;
    check(bool(selected_lan_room(legacy,{legacy},now)),"Fresh legacy address/PIN route was removed");
    auto replacement=legacy;replacement.address="127.0.0.2";
    check(!selected_lan_room(legacy,{replacement},now),"Legacy row lost endpoint identity");
}
void out_of_order_replies(){
    auto latest=room();latest.probe_ms=now-10;latest.ping_ms=10;
    auto old=latest;old.name="Old";old.probe_ms=now-100;old.ping_ms=100;old.join_ticket="cccccccccccccccccccccccccccccccc";old.join_expires_ms-=100;
    std::vector<FrontendRoom> rooms{latest};merge_lan_room(rooms,old);
    check(rooms.size()==1 && rooms[0].name==latest.name && rooms[0].ping_ms==10 && rooms[0].join_ticket==latest.join_ticket && rooms[0].join_expires_ms==latest.join_expires_ms,"Delayed same-endpoint probe rolled back metadata, RTT or permit");
    latest.address="192.168.1.10";rooms={latest};merge_lan_room(rooms,old);
    check(rooms[0].name==latest.name && rooms[0].address==old.address && rooms[0].join_ticket==old.join_ticket && rooms[0].ping_ms==old.ping_ms,"Interface preference detached endpoint permit/RTT from the newest metadata");
}
}
int main(){try{labels_and_freshness();identity_and_selection();out_of_order_replies();std::cout<<"Browser localized rows, freshness, stable selection and out-of-order probe tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
