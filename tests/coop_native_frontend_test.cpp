#include "frontend_text.h"
#include "native_lobby.h"
#include "native_frontend.h"
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
 LanAdvertisement advertisement;LanBrowser browser;unsigned discovery=0;
 for(unsigned port=28400;port<28460;++port)if(advertisement.start(0x12345678,[&](){const auto st=host.status();return FrontendRoom{"","Host",st.port,st.joined,st.capacity,host.lan_session_id()};},uint16_t(port),true,[&](const auto& peer,uint32_t nonce){return host.issue_lan_join_ticket(peer,nonce);})){discovery=port;break;}
 check(discovery && browser.search(0x12345678,uint16_t(discovery),true),"Authenticated LAN discovery failed");
 while(browser.searching())std::this_thread::sleep_for(std::chrono::milliseconds(20));
 const auto rooms=browser.rooms();check(rooms.size()==1 && rooms[0].session_id==session && browser.can_join(rooms[0]),"Discovery room lacks a current scoped credential");
 auto forged=rooms[0];forged.address="127.0.0.2";check(!browser.can_join(forged),"Room credential accepted with a different endpoint");
 connect(rooms[0].join_ticket,true);advertisement.stop();browser.stop();check(browser.rooms().empty() && !browser.can_join(rooms[0]),"Stopped browser retains a joinable room");
 auto old=host.issue_lan_join_ticket("127.0.0.1",5);check(!old.ticket.empty(),"Session-lifetime test ticket missing");host.stop();check(host.lan_session_id().empty() && host.issue_lan_join_ticket("127.0.0.1",6).ticket.empty(),"Stopped host retains LAN credentials");
 AuthoritativeServer fresh;check(fresh.start(0,"963852",0x12345678,"Host") && fresh.lan_session_id()!=session,"New room reused session identity");
 AuthoritativeClient stale;check(stale.start_lan("127.0.0.1",fresh.status().port,old.ticket,0x12345678,"Stale"),"Stale credential syntax rejected");finish(stale);check(!stale.status().connected,"Prior room credential accepted by a new host");stale.stop();
 unsigned issued=0;for(unsigned address=1;address<=5;++address)for(unsigned nonce=0;nonce<17;++nonce)issued+=!fresh.issue_lan_join_ticket("127.0.0."+std::to_string(address),100+nonce).ticket.empty();
 check(issued==64,"Pending LAN permits lack global/per-peer bounds");fresh.stop();
}
int main(){try{
 for(unsigned e=1;e<=5;++e)for(unsigned cap=0;cap<=5;++cap)for(unsigned pickup=1;pickup<=5;++pickup)
  check(valid_multiplayer_options({"Sam",e,cap,pickup}),"Valid extended host rule rejected");
 for(auto bad:{MultiplayerOptions{"Sam",0,0,1},MultiplayerOptions{"Sam",6,0,1},MultiplayerOptions{"Sam",1,6,1},MultiplayerOptions{"Sam",1,0,0},MultiplayerOptions{"Sam",1,0,6},MultiplayerOptions{"   ",1,0,1},MultiplayerOptions{"bad\n",1,0,1},MultiplayerOptions{std::string(25,'a'),1,0,1}})
  check(!valid_multiplayer_options(bad),"Invalid name or rule accepted");
 unsigned cap=0;for(unsigned expected:{1u,3u,4u,5u,2u,0u}){cap=next_ammo_capacity(cap);check(cap==expected,"Ammo menu order lost native maximum");}
 const std::string pal_name="J\366rg \351";
 check(native_name_input(native_name_utf8(pal_name))==pal_name,"PAL accented name roundtrip failed");
 check(native_name_input("Sam\n\t")=="Sam","Name controls accepted");
 check(valid_multiplayer_options({}),"Profile name default rejected");

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
 lan_join_checks();
 std::cout<<"Native translations, endpoint validation, LAN discovery, deduplication, scoped direct join, expiry and shutdown passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
