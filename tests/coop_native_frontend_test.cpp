#include <winsock2.h>
#include <ws2tcpip.h>
#include <set>
#include <mutex>
#include <array>
#include <functional>
#include "frontend_text.h"
#include "native_lobby.h"
#include "native_frontend.h"
#include "frontend_startup.h"
#include "lan_discovery.h"
#include "authoritative_session.h"
#include <chrono>
#include <thread>
#include <stdexcept>
#include <atomic>
#include <iostream>
using namespace port::coop;
void require(bool value){if(!value)throw std::runtime_error("Native frontend check failed");}

void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
unsigned frontend_startup_checks(){
 unsigned checks=0;auto test=[&](bool v,const char* why){++checks;check(v,why);};
 FrontendStartupReadiness phase;constexpr uint32_t main=0x123400,other=0x567800;
 test(!phase.ready() && !phase.owner(),"Unconstructed frontend accepted startup");
 {
  FrontendStartupReadiness::Open asset(phase,other,false);
  asset.completed();test(!phase.ready(),"Unrelated resource Open completed initial frontend");
 }
 test(!phase.ready(),"Noninitial Open invented startup readiness");
 {
  FrontendStartupReadiness::Open initial(phase,main,true);
  test(phase.owner()==main && !phase.ready(),"Assigned root accepted a reentrant loading frame");
  {
   FrontendStartupReadiness::Open child(phase,main,false);
   child.completed();test(!phase.ready(),"Nested Open accepted startup before its return");
  }
  test(!phase.ready(),"Nested completion bypassed pending initial Open");
  initial.completed();test(!phase.ready(),"Completion marker accepted startup before Open returned");
 }
 test(phase.ready(),"Completed initial frontend did not permit startup");
 {
  FrontendStartupReadiness::Open profile(phase,other,false);
  test(!phase.ready(),"Open after initial completion accepted a reentrant frame");
  profile.completed();
 }
 test(phase.ready(),"Successful later Open lost completed initial frontend");
 phase.destroyed(other);test(phase.ready(),"Unrelated GUI destruction reset frontend lifetime");
 phase.destroyed(main);test(!phase.ready() && !phase.owner(),"Destroyed initial GUI retained readiness");
 try{
  FrontendStartupReadiness::Open initial(phase,main,true);
  throw std::runtime_error("test original Open failure");
 }catch(const std::runtime_error&){}
 test(!phase.ready(),"Aborted initial Open accepted startup");
 {
  FrontendStartupReadiness::Open retry(phase,main,true);retry.completed();
 }
 test(phase.ready(),"Clean initial Open could not recover after an exception");
 {
  FrontendStartupReadiness::Open initial(phase,main,true);
  try{FrontendStartupReadiness::Open child(phase,main,false);throw std::runtime_error("nested failure");}
  catch(const std::runtime_error&){}
  initial.completed();
 }
 test(!phase.ready(),"Caught nested failure allowed outer completion to mark readiness");
 {
  FrontendStartupReadiness::Open retry(phase,main,true);retry.completed();
 }
 test(phase.ready(),"Nested failure prevented a later clean lifetime Open");
 {
  FrontendStartupReadiness::Open old(phase,main,true);
  phase.reset();test(!phase.ready() && !phase.owner(),"Reset retained completed state");
  old.completed();
 }
 test(!phase.ready(),"Stale completion token revived a reset lifetime");
 {
  FrontendStartupReadiness::Open old(phase,main,true);
  phase.reset();
  {FrontendStartupReadiness::Open replacement(phase,other,true);replacement.completed();}
  test(!phase.ready(),"Replacement lifetime ignored an in-flight older Open");
  old.completed();
 }
 test(phase.ready() && phase.owner()==other,"Stale outer completion corrupted replacement lifetime");
 {
  FrontendStartupReadiness::Open replacement(phase,main,true);
  test(!phase.ready() && phase.owner()==main,"New GUI owner inherited old ready state");
  {FrontendStartupReadiness::Open nested(phase,main,true);nested.completed();}
  test(!phase.ready(),"Nested initial completion bypassed outer Open");
  replacement.completed();
 }
 test(phase.ready(),"Nested successful original Opens did not settle");
 // Readiness is independent of profile confirmation. The existing initialize
 // profile-ID check must still return before any native level queue.
 bool profile_id_present=false;unsigned profile_starts=0,level_queues=0;
 auto attempt=[&]{if(!phase.ready())return;if(!profile_id_present){++profile_starts;return;}++level_queues;};
 attempt();test(profile_starts==1 && level_queues==0,"Missing profile escaped the existing profile gate");
 {FrontendStartupReadiness::Open profile(phase,main,false);attempt();profile.completed();}
 test(profile_starts==1 && level_queues==0,"Reentrant frame opened a profile or queued a level");
 profile_id_present=true;attempt();test(level_queues==1,"Confirmed profile remained blocked after Open completion");
 phase.reset();attempt();test(level_queues==1,"Lifetime reset allowed an explicit startup action");
 return checks;
}
void lobby_level_proposal_checks(){
 LobbyLevelSelection choice;constexpr uint32_t parent=0x123400,other=0x567800;
 auto first=[&](){return choice.selected() && choice.selected()->level=="Rlevel1_2" && choice.selected()->level_set==0 && choice.selected()->difficulty==2 && choice.selected()->mode==1;};
 check(!choice.selected() && !choice.take() && !choice.begin(0),"Empty lobby has a startable proposal");
 check(choice.begin(parent) && !choice.begin(parent),"Selector ownership can be entered twice");
 check(!choice.confirm(other,{"Rlevel1_2",0,1,1}) && !choice.confirm(parent,{"",0,1,1}),"Foreign or empty map selection changed proposal");
 choice.closed(other);check(choice.phase()==LobbyLevelSelection::Phase::Selecting,"Foreign selector close changed ownership");
 choice.closed(parent);check(choice.owns(parent) && choice.confirm(parent,{"Rlevel1_2",0,1,1}),"Native close-before-confirm lost selector ownership");
 check(!choice.selected() && !choice.take(),"Map confirmation started before difficulty confirmation");
 check(!choice.confirm_difficulty(other,2) && !choice.confirm_difficulty(parent,-1) && !choice.confirm_difficulty(parent,4),"Invalid or foreign difficulty accepted");
 check(choice.confirm_difficulty(parent,2) && first(),"Complete map/difficulty proposal not retained for lobby");
 check(!choice.confirm_difficulty(parent,1) && first(),"Repeated native confirmation changed retained proposal");
 check(!choice.begin(0) && first(),"Invalid reselection discarded confirmed proposal");

 check(choice.begin(parent) && first(),"Opening replacement discarded confirmed proposal");
 check(choice.confirm(parent,{"Rlevel1_4a",0,1,1}) && first() && !choice.take(),"Incomplete replacement escaped into lobby start");
 check(!choice.back_from_difficulty(other) && choice.back_from_difficulty(parent) && first(),"Difficulty Back discarded previous complete proposal");
 choice.closed(parent);choice.cancel();
 check(choice.phase()==LobbyLevelSelection::Phase::Selected && first(),"Cancelled reselection did not return to previous proposal");
 check(choice.begin(other),"Complete proposal prevented reopening selector");
 choice.closed(other);choice.cancel();check(first(),"Cancelling map carousel discarded previous proposal");
 check(!choice.confirm(parent,{"Rlevel1_4a",0,1,1}),"Old selector callback replaced proposal after reopening");

 check(choice.begin(parent) && choice.confirm(parent,{"Rlevel1_4a",2,1,1}) && choice.confirm_difficulty(parent,3),"Replacement map/difficulty confirmation failed");
 check(choice.selected() && choice.selected()->level=="Rlevel1_4a" && choice.selected()->level_set==2 && choice.selected()->difficulty==3,"Replacement did not atomically replace complete proposal");
 const auto started=choice.take();
 check(started && started->level=="Rlevel1_4a" && started->difficulty==3 && !choice.selected() && !choice.take(),"Explicit start did not consume selected proposal exactly once");
 choice.cancel();check(choice.phase()==LobbyLevelSelection::Phase::Queued && !choice.begin(parent) && !choice.take(),"Cancellation allowed an already queued start to run again");

 choice.reset();check(choice.begin(parent) && choice.confirm(parent,{"DMlevel1_1",0,1,5}) && choice.confirm_difficulty(parent,1),"Survival proposal fixture failed");
 choice.sync_mode(5);check(choice.selected() && choice.selected()->mode==5,"Matching mode discarded valid proposal");
 choice.sync_mode(1);check(!choice.selected() && !choice.take() && choice.phase()==LobbyLevelSelection::Phase::Idle,"Mode change retained stale Survival proposal");
 check(!choice.confirm_difficulty(parent,1) && !choice.owns(parent),"Late difficulty callback revived proposal after mode change");
 check(choice.begin(parent) && choice.confirm(parent,{"DMlevel1_2",0,1,2}),"Pending versus fixture failed");
 choice.sync_mode(1);check(choice.phase()==LobbyLevelSelection::Phase::Idle && !choice.selected(),"Mode change retained unfinished incompatible draft");

 check(choice.begin(parent) && choice.confirm(parent,{"Rlevel1_2",0,1,1}) && choice.confirm_difficulty(parent,2) && choice.begin(parent),"Cross-mode replacement fixture failed");
 check(choice.confirm(parent,{"DMlevel1_1",0,1,2}) && !choice.selected(),"Cross-mode map confirmation exposed old mode proposal");
 choice.cancel();check(!choice.selected() && choice.phase()==LobbyLevelSelection::Phase::Idle,"Cancelling other mode restored incompatible proposal");
 check(choice.begin(parent),"Cancelled first selector could not reopen");choice.cancel();
 check(!choice.selected() && !choice.take() && !choice.owns(parent),"Cancelling first selection invented a startable proposal");
}
void finish(AuthoritativeClient& client){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
 while(client.status().connecting && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
 check(!client.status().connecting,"LAN handshake failed to reach a terminal state");
}
void lan_join_checks(){
 const auto id=new_lan_secret(),other=new_lan_secret(),ticket=new_lan_secret();
 check(valid_lan_secret(id) && id!=other && valid_lan_secret(ticket),"LAN OS random identifiers invalid");
 std::vector<FrontendRoom> merged;
 merge_lan_room(merged,{"192.168.1.7","Sam",27341,1,8,id,ticket,100});
 merge_lan_room(merged,{"127.0.0.1","Sam",27341,2,8,id,other,200});
 merge_lan_room(merged,{"192.168.1.7","Sam",27341,3,8,id,ticket,300});
 check(merged.size()==1 && merged[0].players==3 && merged[0].address=="127.0.0.1" && merged[0].join_ticket==other && merged[0].join_expires_ms==200,"Duplicate endpoint merge detached credential or lost loopback preference");
 merge_lan_room(merged,{"192.168.1.8","Sam",27341,1,8,other,ticket,100});
 merge_lan_room(merged,{"127.0.0.1","Sam",27343,1,8,id,ticket,100});
 check(merged.size()==3,"Independent hosts or ports incorrectly deduplicated");
 AuthoritativeServer host;check(host.start(0,"963852",0x12345678,"Host"),"LAN test host failed");
 const auto gameport=host.status().port;const auto session=host.lan_session_id();check(valid_lan_secret(session),"Missing session identifier");
 check(host.issue_lan_join_ticket("224.0.0.1",1).ticket.empty(),"Non-LAN multicast peer obtained ticket");
 check(host.issue_lan_join_ticket("127.0.0.1",1,60001).ticket.empty(),"Unbounded lifetime accepted");
 auto manual=[&](const char* pin,bool expected){AuthoritativeClient c;check(c.start("127.0.0.1",gameport,pin,0x12345678,"Manual"),"Manual start failed");finish(c);check(c.status().connected==expected,"Manual PIN behavior changed");c.stop();};
 manual("000000",false);manual("963852",true);
 auto connect=[&](const std::string& credential,bool expected,uint64_t fingerprint=0x12345678){AuthoritativeClient c;check(c.start_lan("127.0.0.1",gameport,credential,fingerprint,"LAN"),"LAN credential syntax rejected");finish(c);check(c.status().connected==expected,"IP binding, expiry, fingerprint or one-use ticket check failed");c.stop();};
 auto wrong_peer=host.issue_lan_join_ticket("127.0.0.2",2);check(!wrong_peer.ticket.empty(),"Loopback test ticket not issued");connect(wrong_peer.ticket,false);
 auto expired=host.issue_lan_join_ticket("127.0.0.1",3,5);check(!expired.ticket.empty(),"Short test ticket not issued");std::this_thread::sleep_for(std::chrono::milliseconds(25));connect(expired.ticket,false);
 auto once=host.issue_lan_join_ticket("127.0.0.1",4);check(!once.ticket.empty(),"Join ticket not issued");
 check(host.issue_lan_join_ticket("127.0.0.1",4).ticket==once.ticket,"Repeated discovery query changed active ticket");
 AuthoritativeClient strict;check(!strict.start("127.0.0.1",gameport,once.ticket,0x12345678,"Manual"),"Manual entry accepted a LAN credential");
 connect(once.ticket,false,0x87654321);connect(once.ticket,true);connect(once.ticket,false);
 auto revoked=host.issue_lan_join_ticket("127.0.0.1",7);check(!revoked.ticket.empty(),"Password revocation test lacks an initial permit");
 host.set_lan_join_enabled(false);
 check(host.issue_lan_join_ticket("127.0.0.1",7).ticket.empty() && host.issue_lan_join_ticket("127.0.0.1",8).ticket.empty(),"Protected host still issues LAN permits");
 connect(revoked.ticket,false);manual("000000",false);manual("963852",true);
 host.set_lan_join_enabled(true);
 auto enabled=host.issue_lan_join_ticket("127.0.0.1",8);check(!enabled.ticket.empty(),"Disabling password protection did not restore LAN access");connect(enabled.ticket,true);
 LanAdvertisement advertisement;LanBrowser browser;unsigned discovery=0;
 std::set<uint32_t> issued_scan_nonces;
 for(unsigned port=28400;port<28460;++port)if(advertisement.start(0x12345678,[&](){const auto st=host.status();FrontendRoom room{"","Host",st.port,st.joined,st.capacity,host.lan_session_id()};room.details_known=true;room.mode=5;room.difficulty=3;room.humans=st.joined;room.map="DMlevel1_1";room.map_name="Arena";room.running=true;room.enemies=3;room.ammo_capacity=2;room.pickups=5;return room;},uint16_t(port),true,[&](const auto& peer,uint32_t nonce){issued_scan_nonces.insert(nonce);return host.issue_lan_join_ticket(peer,nonce);})){discovery=port;break;}
 check(discovery && browser.search(0x12345678,uint16_t(discovery),true),"Authenticated LAN discovery failed");
 while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));
 const auto rooms=browser.rooms();check(rooms.size()==1 && rooms[0].session_id==session && browser.can_join(rooms[0]),"Discovery room lacks a current scoped credential");
 check(rooms[0].details_known && rooms[0].mode==5 && rooms[0].difficulty==3 && rooms[0].humans==rooms[0].players && !rooms[0].bots && rooms[0].map=="DMlevel1_1" && rooms[0].map_name=="Arena" && rooms[0].running && rooms[0].enemies==3 && rooms[0].ammo_capacity==2 && rooms[0].pickups==5 && rooms[0].ping_ms<1000 && rooms[0].seen_ms>=rooms[0].probe_ms,"Live metadata or measured RTT roundtrip failed");
 auto forged=rooms[0];forged.address="127.0.0.2";check(!browser.can_join(forged),"Room credential accepted with a different endpoint");
 connect(rooms[0].join_ticket,true);advertisement.stop();check(issued_scan_nonces.size()==1,"RTT probes consumed separate LAN permits within one scan");browser.stop();check(browser.rooms().empty() && !browser.can_join(rooms[0]),"Stopped browser retains a joinable room");
 host.set_lan_join_enabled(false);
 const std::string game_name="A separately named game";
 check(advertisement.start(0x12345678,[&,game_name](){const auto st=host.status();return FrontendRoom{"",game_name,st.port,st.joined,st.capacity,host.lan_session_id()};},uint16_t(discovery),true,[&](const auto& peer,uint32_t nonce){return host.issue_lan_join_ticket(peer,nonce);}),"Protected game advertisement failed");
 check(browser.search(0x12345678,uint16_t(discovery),true),"Protected game search failed");
 while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));
 const auto protected_rooms=browser.rooms();
 check(protected_rooms.size()==1 && protected_rooms[0].name==game_name && protected_rooms[0].session_id==session && protected_rooms[0].join_ticket.empty() && !protected_rooms[0].join_expires_ms,"Protected game name or absent-permit discovery roundtrip failed");
 check(!browser.can_join(protected_rooms[0]) && bool(selected_lan_room(protected_rooms[0],protected_rooms,lan_clock_ms())),"Protected game either auto-joined or lost manual PIN selection");
 manual("000000",false);manual("963852",true);advertisement.stop();browser.stop();host.set_lan_join_enabled(true);
 auto old=host.issue_lan_join_ticket("127.0.0.1",5);check(!old.ticket.empty(),"Session-lifetime test ticket missing");host.stop();check(host.lan_session_id().empty() && host.issue_lan_join_ticket("127.0.0.1",6).ticket.empty(),"Stopped host retains LAN credentials");
 AuthoritativeServer fresh;check(fresh.start(0,"963852",0x12345678,"Host") && fresh.lan_session_id()!=session,"New room reused session identity");
 AuthoritativeClient stale;check(stale.start_lan("127.0.0.1",fresh.status().port,old.ticket,0x12345678,"Stale"),"Stale credential syntax rejected");finish(stale);check(!stale.status().connected,"Prior room credential accepted by a new host");stale.stop();
 unsigned issued=0;for(unsigned address=1;address<=5;++address)for(unsigned nonce=0;nonce<17;++nonce)issued+=!fresh.issue_lan_join_ticket("127.0.0."+std::to_string(address),100+nonce).ticket.empty();
 check(issued==64,"Pending LAN permits lack global/per-peer bounds");fresh.stop();
}

namespace {
using DiscoveryBytes=std::vector<uint8_t>;
void test_put(DiscoveryBytes& bytes,uint64_t value,unsigned count){for(unsigned i=count;i-->0;)bytes.push_back(uint8_t(value>>(i*8)));}
uint32_t test_get32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
DiscoveryBytes test_details(unsigned mode=5,unsigned difficulty=2,unsigned humans=2,unsigned bots=1,unsigned spectators=1,std::string map="DMlevel1_1",std::string title="Courtyard"){
 DiscoveryBytes p{1,uint8_t(mode),uint8_t(difficulty),1,uint8_t(humans),uint8_t(bots),uint8_t(spectators),2,3,4,uint8_t(map.size())};p.insert(p.end(),map.begin(),map.end());p.push_back(uint8_t(title.size()));p.insert(p.end(),title.begin(),title.end());return p;
}
DiscoveryBytes test_reply(const DiscoveryBytes& request,unsigned port,DiscoveryBytes metadata,bool with_details=true,unsigned players=3,unsigned capacity=4){
 DiscoveryBytes out(request.begin(),request.begin()+22);test_put(out,port,2);out.push_back(uint8_t(players));out.push_back(uint8_t(capacity));out.push_back(4);out.insert(out.end(),{'T','e','s','t'});out.insert(out.end(),{'L','A','N','2'});out.insert(out.end(),32,'a');out.insert(out.end(),32,'b');test_put(out,60000,4);
 if(with_details){out.insert(out.end(),{'R','O','O','M'});test_put(out,metadata.size(),2);out.insert(out.end(),metadata.begin(),metadata.end());}return out;
}
struct DiscoveryWirePeer {
 SOCKET socket=INVALID_SOCKET;std::atomic<bool> run{true};std::thread worker;unsigned port=0;std::set<uint32_t> probes,scans;
 explicit DiscoveryWirePeer(std::function<void(DiscoveryWirePeer&,const DiscoveryBytes&,const sockaddr_in&,int,unsigned)> send){
  socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);check(socket!=INVALID_SOCKET,"Raw discovery socket failed");sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);check(!bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local)),"Raw discovery bind failed");int size=sizeof(local);check(!getsockname(socket,reinterpret_cast<sockaddr*>(&local),&size),"Raw discovery port unavailable");port=ntohs(local.sin_port);
  worker=std::thread([this,send=std::move(send)]{unsigned count=0;while(run){fd_set reads;FD_ZERO(&reads);FD_SET(socket,&reads);timeval wait{0,50000};if(select(0,&reads,nullptr,nullptr,&wait)<=0)continue;std::array<uint8_t,512> data{};sockaddr_in remote{};int size=sizeof(remote);const auto n=recvfrom(socket,reinterpret_cast<char*>(data.data()),int(data.size()),0,reinterpret_cast<sockaddr*>(&remote),&size);if(n<22)continue;probes.insert(test_get32(data.data()+18));if(n==30)scans.insert(test_get32(data.data()+26));send(*this,DiscoveryBytes(data.begin(),data.begin()+n),remote,size,++count);}});
 }
 void reply(const DiscoveryBytes& bytes,const sockaddr_in& remote,int size){sendto(socket,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&remote),size);}
 void close(){run=false;if(worker.joinable())worker.join();if(socket!=INVALID_SOCKET){closesocket(socket);socket=INVALID_SOCKET;}}
 ~DiscoveryWirePeer(){close();}
};
}
void browser_details_checks(){
 FrontendRoom room{"127.0.0.1","zeta",29001,3,4,new_lan_secret(),new_lan_secret(),lan_clock_ms()+60000};room.details_known=true;room.mode=5;room.difficulty=2;room.humans=2;room.bots=1;room.spectators=1;room.map="DMlevel1_1";room.map_name="Courtyard";room.running=true;room.enemies=2;room.ammo_capacity=3;room.pickups=4;room.ping_ms=40;room.seen_ms=200;room.probe_ms=180;
 check(valid_lan_room_details(room) && lan_room_has_human_slot(room),"Valid detailed room rejected");
 for(unsigned mode=1;mode<=5;++mode)for(unsigned difficulty=0;difficulty<=3;++difficulty){auto value=room;value.mode=mode;value.difficulty=difficulty;check(valid_lan_room_details(value),"Supported mode/difficulty rejected");}
 for(unsigned kind=0;kind<12;++kind){auto bad=room;
  switch(kind){case 0:bad.mode=0;break;case 1:bad.mode=6;break;case 2:bad.difficulty=4;break;case 3:bad.humans=4;break;case 4:bad.bots=4;break;case 5:bad.spectators=3;break;case 6:bad.enemies=0;break;case 7:bad.ammo_capacity=6;break;case 8:bad.pickups=6;break;case 9:bad.map="../escape";break;case 10:bad.map_name="bad\nname";break;case 11:bad.map=std::string(49,'a');break;}
  check(!valid_lan_room_details(bad),"Malformed server details accepted");
 }
 auto full=room;full.players=full.capacity=4;full.humans=4;full.bots=0;check(!lan_room_has_human_slot(full),"Full human room shown as joinable");full.humans=3;full.bots=1;check(lan_room_has_human_slot(full),"Full bot room lost human replacement");
 auto alpha=room;alpha.name="Alpha";alpha.ping_ms=5;alpha.mode=1;alpha.running=false;auto unknown=room;unknown.name="unknown";unknown.details_known=false;unknown.mode=0;unknown.ping_ms=UINT32_MAX;
 room.seen_ms=alpha.seen_ms=unknown.seen_ms=lan_clock_ms();
 auto order=sorted_lan_rooms({unknown,room,alpha});check(order[0].name=="Alpha" && order[2].name=="unknown","Ping sorting or unknown order failed");
 check(sorted_lan_rooms({unknown,room,alpha},LanRoomSort::Name)[0].name=="Alpha","Name sorting failed");
 check(sorted_lan_rooms({unknown,room,alpha},LanRoomSort::Mode)[2].name=="unknown","Unknown mode not sorted last");
 auto stale_unknown=unknown;stale_unknown.mode=5;stale_unknown.name="Aardvark";check(sorted_lan_rooms({stale_unknown,room},LanRoomSort::Mode)[1].name=="Aardvark","Stale mode field changed unknown-metadata ordering");
 check(sorted_lan_rooms({unknown,room,alpha},LanRoomSort::Ping,{5,false,false}).size()==1,"Mode filter includes unknown/other modes");
 check(sorted_lan_rooms({room,alpha},LanRoomSort::Ping,{0,false,true}).size()==1,"Lobby filter failed");
 full.humans=4;full.bots=0;check(sorted_lan_rooms({full,room},LanRoomSort::Players,{0,true,false}).size()==1,"Full room filter failed");
 auto newest=room;newest.name="Latest";newest.probe_ms=200;newest.seen_ms=220;newest.address="192.168.1.10";newest.mode=2;auto old=room;old.name="Stale";old.probe_ms=180;old.address="127.0.0.1";old.ping_ms=7;old.join_ticket=new_lan_secret();old.join_expires_ms+=100;
 std::vector<FrontendRoom> merged;merge_lan_room(merged,newest);merge_lan_room(merged,old);check(merged.size()==1 && merged[0].name=="Latest" && merged[0].mode==2 && merged[0].address=="127.0.0.1" && merged[0].join_ticket==old.join_ticket && merged[0].ping_ms==7,"Out-of-order metadata rolled back or detached endpoint permit/RTT");
 DiscoveryWirePeer peer([](auto& self,const auto& query,const auto& remote,int size,unsigned count){
  if(count==1)return;std::this_thread::sleep_for(std::chrono::milliseconds(80));
  self.reply(test_reply(query,29001,test_details()),remote,size);
  self.reply(test_reply(query,29002,{9}),remote,size); // Unknown metadata version retains scoped legacy join.
  self.reply(test_reply(query,29003,test_details(2,1,4,0,0),true,4,4),remote,size);
  self.reply(test_reply(query,29004,test_details(2,1,3,1,0),true,4,4),remote,size);
  self.reply(test_reply(query,29005,{},false),remote,size);
  std::vector<DiscoveryBytes> bad;
  bad.push_back(test_reply(query,29100,test_details(6)));bad.push_back(test_reply(query,29101,test_details(5,4)));bad.push_back(test_reply(query,29102,test_details(5,2,4,0)));bad.push_back(test_reply(query,29103,test_details(5,2,2,1,3)));bad.push_back(test_reply(query,29104,test_details(5,2,2,1,1,"../escape")));bad.push_back(test_reply(query,29105,test_details(5,2,2,1,1,std::string(49,'a'))));bad.push_back(test_reply(query,29106,test_details(5,2,2,1,1,"DMlevel1_1","bad\nname")));
  auto flags=test_details();flags[3]=2;bad.push_back(test_reply(query,29107,flags));auto ammo=test_details();ammo[8]=6;bad.push_back(test_reply(query,29108,ammo));
  auto truncated=test_reply(query,29109,test_details());truncated.pop_back();bad.push_back(truncated);auto extra=test_reply(query,29110,test_details());extra.push_back(0);bad.push_back(extra);
  auto ticket=test_reply(query,29111,test_details());ticket[27+4+4+32]='G';bad.push_back(ticket);
  auto fingerprint=test_reply(query,29112,test_details());fingerprint[10]^=1;bad.push_back(fingerprint);auto nonce=test_reply(query,29113,test_details());nonce[18]^=0x80;bad.push_back(nonce);
  auto oversized=test_reply(query,29114,test_details());oversized.resize(600,0);bad.push_back(oversized);
  for(const auto& packet:bad)self.reply(packet,remote,size);
 });
 LanBrowser browser;check(browser.search(0x12345678,uint16_t(peer.port),true),"Wire-matrix browser start failed");while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));peer.close();
 const auto rooms=browser.rooms();check(rooms.size()==5,"Malformed/oversized/foreign-nonce metadata escaped bounds, or compatible room lost");std::set<unsigned> ports;for(const auto& value:rooms){ports.insert(value.port);check(value.ping_ms>=40 && value.ping_ms<400 && value.seen_ms>=value.probe_ms,"Ping measures elapsed scan rather than matched probe RTT");}
 check(ports==std::set<unsigned>({29001,29002,29003,29004,29005}),"Wrong wire-matrix room set");
 for(const auto& value:rooms){check(value.details_known==(value.port!=29002 && value.port!=29005),"Legacy/future metadata handling failed");check(browser.can_join(value)==(value.port!=29003),"Full-room or bot replacement join availability failed");}
 check(peer.scans.size()==1 && peer.probes.size()>=3,"Probe nonces did not rotate independently of scan permit nonce");browser.stop();
}

int main(){try{
 const auto startup_checks=frontend_startup_checks();
 lobby_level_proposal_checks();
 for(unsigned e=1;e<=5;++e)for(unsigned cap=0;cap<=5;++cap)for(unsigned pickup=1;pickup<=5;++pickup)
  check(valid_multiplayer_options({"Sam",e,cap,pickup}),"Valid extended host rule rejected");
 for(auto bad:{MultiplayerOptions{"Sam",0,0,1},MultiplayerOptions{"Sam",6,0,1},MultiplayerOptions{"Sam",1,6,1},MultiplayerOptions{"Sam",1,0,0},MultiplayerOptions{"Sam",1,0,6},MultiplayerOptions{"   ",1,0,1},MultiplayerOptions{"bad\n",1,0,1},MultiplayerOptions{std::string(25,'a'),1,0,1}})
  check(!valid_multiplayer_options(bad),"Invalid name or rule accepted");
 unsigned cap=0;for(unsigned expected:{1u,3u,4u,5u,2u,0u}){cap=next_ammo_capacity(cap);check(cap==expected,"Ammo menu order lost native maximum");}
 const std::string pal_name="J\366rg \351";
 check(native_name_input(native_name_utf8(pal_name))==pal_name,"PAL accented name roundtrip failed");
 check(native_name_input("Sam\n\t")=="Sam","Name controls accepted");
 check(valid_multiplayer_options({}),"Profile name default rejected");
 MultiplayerOptions online;online.host_name=std::string(32,'\366');online.pin="963852";online.use_password=true;
 check(valid_multiplayer_options(online) && native_name_utf8(online.host_name).size()==64,"Maximum PAL host game name or protected PIN was rejected");
 for(const auto& invalid:{std::string(33,'a'),std::string("   "),std::string("bad\nname"),std::string("bad\177name"),std::string("bad\200name")}){auto bad=online;bad.host_name=invalid;check(!valid_multiplayer_options(bad),"Invalid host game name accepted");}
 for(const auto* invalid:{"","12345","1234567","12345a","12 456"}){auto bad=online;bad.pin=invalid;check(!valid_multiplayer_options(bad),"Invalid Online Setup PIN accepted");}
 online.skin=0;check(valid_multiplayer_options(online),"Saved first skin rejected");online.skin=16;check(valid_multiplayer_options(online),"Saved PS2 skin rejected");online.skin=255;check(valid_multiplayer_options(online),"Profile-derived skin default rejected");online.skin=256;check(!valid_multiplayer_options(online),"Unbounded saved skin accepted");

 for(uint16_t buttons:{uint16_t(0),uint16_t(ssne::PAD_START),uint16_t(ssne::PAD_A|ssne::PAD_START),uint16_t(ssne::PAD_START|ssne::PAD_X|ssne::PAD_LEFT),uint16_t(ssne::PAD_B|ssne::PAD_R)}){
  ssne::PadState source;source.buttons=buttons;source.stick_x=43;source.stick_y=211;source.cstick_x=19;source.cstick_y=233;source.trigger_l=77;source.trigger_r=188;
  for(bool results:{false,true}){
   const auto normalized=native_results_input(source,results);
   const auto expected=results && (buttons&ssne::PAD_START)?uint16_t((buttons&~ssne::PAD_START)|ssne::PAD_A):buttons;
   check(normalized.buttons==expected,"Result Start must confirm without Pause; other scenes/buttons must be preserved");
   check(normalized.stick_x==source.stick_x && normalized.stick_y==source.stick_y && normalized.cstick_x==source.cstick_x && normalized.cstick_y==source.cstick_y && normalized.trigger_l==source.trigger_l && normalized.trigger_r==source.trigger_r,"Result button normalization changed analog input");
  }
 }
 ssne::PadState pad;pad.buttons=ssne::PAD_B|ssne::PAD_A|ssne::PAD_START|ssne::PAD_LEFT;
 require(native_lobby_choice(pad).pad.buttons==(ssne::PAD_B|ssne::PAD_LEFT));
 LobbyConfirmRelease back_release;back_release.arm();const auto mask=ssne::PAD_A|ssne::PAD_START|ssne::PAD_B;
 require(back_release.filter(ssne::PAD_B,mask)==0);require(back_release.filter(0,mask)==0);require(back_release.filter(ssne::PAD_B,mask)==ssne::PAD_B);
 for(unsigned i=0;i<unsigned(FrontText::Count);++i)for(unsigned language=0;language<3;++language)require(*frontend_text(FrontText(i),language));
 FrontendEndpoint e;require(valid_frontend_endpoint(e));for(auto text:{"","a b","a/b","a\n"}){auto bad=e;bad.address=text;require(!valid_frontend_endpoint(bad));}
 for(auto pin:{"12345","1234567","12345a"}){auto bad=e;bad.pin=pin;require(!valid_frontend_endpoint(bad));}
 e.port=65536;require(!valid_frontend_endpoint(e));e.port=0;require(!valid_frontend_endpoint(e));
 e={};e.pin="12345";require(frontend_endpoint_error(e)==FrontendEndpointError::Pin);e={};e.address="bad address";require(frontend_endpoint_error(e)==FrontendEndpointError::Address);e={};e.port=65536;require(frontend_endpoint_error(e)==FrontendEndpointError::Port);e={};require(frontend_endpoint_error(e)==FrontendEndpointError::None);
 LanAdvertisement server;LanBrowser browser;std::atomic<unsigned> players{2};unsigned port=0;
 for(unsigned p=28340;p<28400;++p)if(server.start(0x12345678,[&](){return FrontendRoom{"","Sam",27341,players.load(),8};},uint16_t(p),true)){port=p;break;}
 require(port);require(browser.search(0x87654321,uint16_t(port),true));while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));require(browser.rooms().empty());
 require(browser.search(0x12345678,uint16_t(port),true));while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));auto rooms=browser.rooms();require(rooms.size()==1 && rooms[0].address=="127.0.0.1" && rooms[0].port==27341 && rooms[0].players==2);
 players=9;require(browser.search(0x12345678,uint16_t(port),true));while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));require(browser.rooms().empty());
 players=3;require(browser.search(0x12345678,uint16_t(port),true));while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));require(browser.rooms().size()==1 && browser.rooms()[0].players==3);
 server.stop();require(browser.search(0x12345678,uint16_t(port),true));while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));require(browser.rooms().empty());
 lan_join_checks();browser_details_checks();
 std::cout<<"Frontend startup lifecycle checks="<<startup_checks<<"; native translations, endpoint validation, LAN discovery, deduplication, scoped direct join, metadata bounds, per-probe RTT, sorting/filtering, expiry and shutdown passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
