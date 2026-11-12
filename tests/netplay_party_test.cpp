#include "netplay.h"
#include <thread>
#include <vector>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
using namespace port::netplay;
static void check(bool ok,const char* why){if(!ok){fprintf(stderr,"%s\n",why);std::abort();}}
int main() {
    Session host;
    std::vector<uint8_t> card(0x80000);for(unsigned n=0;n<card.size();++n)card[n]=uint8_t(n*13);
    check(!host.start(true,"",0,"123456",99,card,9),"Invalid roster accepted");
    check(host.start(true,"",0,"123456",99,card,8),"Host startup failed");
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!host.status().port){check(std::chrono::steady_clock::now()<deadline,"Host port timeout");std::this_thread::yield();}
    std::vector<std::unique_ptr<Session>> clients;
    for(unsigned i=1;i<8;++i){clients.push_back(std::make_unique<Session>());check(clients.back()->start(false,"127.0.0.1",host.status().port,"123456",99),"Client startup failed");}
    check(host.wait_ready(),"Host lobby failed");
    unsigned ids=1;
    for(auto& c:clients){check(c->wait_ready(),"Client lobby failed");auto st=c->status();check(st.player_count==8&&st.local_player>0&&st.local_player<8,"Invalid assignment");check(!(ids&(1u<<st.local_player)),"Duplicate slot");ids|=1u<<st.local_player;check(c->initial_card()==card,"Card differs");}
    check(ids==255,"Incomplete roster");
    auto run=[](Session& session) {
        unsigned me=session.status().local_player;
        for(uint64_t tick=1;tick<=600;++tick) {
            ssne::PadState pad;pad.buttons=uint16_t(1u<<me);pad.stick_x=uint8_t(120+me);
            session.submit_pad(pad);session.submit_mouse(double(me)*.01,0);
            session.submit_mods(me==0?3:7,30); session.submit_view(1000000+me*100000,1000000+me*50000);
            std::array<Input,max_players> inputs;bool wait=false;
            bool ready=tick>2||me!=7;
            uint64_t hash=tick==600&&me==7?789:123;
            bool ok=session.advance(tick,hash,inputs,ready,&wait);
            if(tick==600){check(!ok,"Mismatch not stopped for whole roster");break;}
            check(ok,"Input advance failed");check(wait==(tick<=2),"Readiness barrier wrong");
            for(unsigned slot=0;slot<8;++slot){check(inputs[slot].view_x==1000000+slot*100000 && inputs[slot].view_y==1000000+slot*50000,"Visibility metadata changed across peers");check(inputs[slot].pad.buttons==(1u<<slot),"Input routed to wrong player");check(inputs[slot].pad.stick_x==120+slot,"Stick routed incorrectly");check(inputs[slot].yaw==int(slot)*10000,"Mouse routed incorrectly");check(inputs[slot].mods.flags==(slot?0:3),"Client changed shared mods");}
        }
    };
    std::vector<std::thread> threads;threads.emplace_back(run,std::ref(host));
    for(auto& c:clients)threads.emplace_back(run,std::ref(*c));
    for(auto& t:threads)t.join();
    check(host.status().message.find("player 8")!=std::string::npos,"Mismatch source missing");
    for(auto& c:clients)c->stop();host.stop();
    std::puts("Eight-peer lobby, save snapshot, unique slots, 599 input ticks, readiness and mismatch broadcast passed");
}
