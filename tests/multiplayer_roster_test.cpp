#include "../port/src/multiplayer_roster.h"
#include <iostream>
#include <limits>
#include <stdexcept>
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    using namespace port;using namespace port::coop;
    std::array<Participant,max_players> people{};std::array<PlayerLatency,max_players> latency{};std::array<RosterHealth,max_players> health{};
    people[0]={{0,3},true,true,false,10,"Host"};people[1]={{1,5},true,true,true,11,"AFK"};
    people[2]={{2,7},true,true,false,12,"Bot"};people[3]={{3,9},true,true,false,13,"Dead"};
    people[4]={{4,2},true,true,false,14,"Watching",true};people[5]={{5,6},true,false,false,0,"Loading"};
    for(unsigned i=0;i<6;++i){health[i]={people[i].id,100,true};latency[i]={people[i].id,i*10,true};}
    health[3].health=0;
    auto rows=multiplayer_roster(people,latency,health,4);
    check(rows.size()==6 && rows[0].host && rows[0].ping_known && rows[0].ping_ms==0,"Host zero RTT should be known");
    check(rows[1].status==RosterStatus::Afk && rows[1].health==100,"AFK status lost");
    check(rows[2].bot && !rows[2].ping_known,"Local bot must not pretend to have network latency");
    check(rows[3].status==RosterStatus::Dead && rows[3].health_known,"Death should retain known zero health");
    check(rows[4].status==RosterStatus::Spectator && !rows[4].health_known,"Spectator inherits stale health");
    check(rows[5].status==RosterStatus::Loading && !rows[5].health_known,"Loading player inherits stale health");
    latency[1].id.generation--;health[1].player.generation--;health[2].health=std::numeric_limits<float>::quiet_NaN();
    rows=multiplayer_roster(people,latency,health,4);
    check(!rows[1].ping_known && !rows[1].health_known && !rows[2].health_known,"Reused identities or NaN health leaked into roster");
    people[1].connected=false;people[2].id.slot=6;
    rows=multiplayer_roster(people,latency,health,4);check(rows.size()==4,"Disconnected or malformed owner appeared");
    std::cout<<"multiplayer roster tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
