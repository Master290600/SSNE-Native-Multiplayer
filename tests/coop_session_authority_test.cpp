#include "session_authority.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <array>
using namespace port::coop;
void require(bool result,const char* message){if(!result){std::cerr<<message<<'\n';std::exit(1);}}
int main() {
    for(unsigned capacity=1;capacity<=8;++capacity) {
        SessionAuthority server(capacity);std::array<PlayerId,8> ids;
        for(unsigned slot=0;slot<capacity;++slot) {
            ids[slot]=*server.join("Player "+std::to_string(slot));
            require(ids[slot].slot==slot,"Slot assignment");
            require(!server.finish_join(ids[slot]),"Join became ready without a character");
            require(server.attach_character(ids[slot],100+slot),"Character attachment");
            require(server.finish_join(ids[slot]),"Join readiness");
        }
        require(!server.join("Overflow"),"Roster overflow");
        auto scene=server.begin_cutscene();
        for(unsigned slot=0;slot<capacity;++slot) {
            require(server.request_skip(ids[slot],1,scene)==CommandResult::Applied,"Valid vote rejected");
            require(server.request_skip(ids[slot],1,scene)==CommandResult::StaleSequence,"Replay accepted");
            require(server.request_skip(ids[slot],2,scene)==CommandResult::Unchanged,"Repeated vote counted twice");
            require(server.skip_vote().received==slot+1,"Duplicate vote changed tally");
            require(server.skip_vote().skip_requested==(slot+1==capacity),"Cutscene ended before unanimity");
        }
        require(server.end_cutscene(scene),"End valid cutscene");
        scene=server.begin_cutscene();
        require(server.skip_vote().received==0,"Votes leaked to next cutscene");
        require(server.request_skip(ids[0],3,scene-1)==CommandResult::WrongCutscene,"Late previous-cutscene vote");
        if(capacity>1) {
            auto last=ids[capacity-1];
            for(unsigned i=0;i+1<capacity;++i)server.request_skip(ids[i],4,scene);
            require(!server.skip_vote().skip_requested,"Missing player vote ignored");
            require(server.leave(last),"Leave failed");
            require(server.skip_vote().skip_requested,"Disconnected player blocks completed vote");
            auto replacement=*server.join("Replacement");
            require(replacement.slot==last.slot && replacement.generation!=last.generation,"Reused player identity");
            require(server.request_afk(last,999,true)==CommandResult::StalePlayer,"Old connection changed replacement");
            require(server.attach_character(replacement,100+last.slot),"Replacement character");
            require(server.finish_join(replacement),"Replacement ready");
            require(server.skip_vote().skip_requested,"Committed skip undone by joining player");
        }
    }
    for(unsigned afk_count=1;afk_count<=7;++afk_count) {
        SessionAuthority server;std::array<PlayerId,8> ids;
        for(unsigned i=0;i<8;++i){ids[i]=*server.join("Player");server.attach_character(ids[i],i+1);server.finish_join(ids[i]);}
        auto scene=server.begin_cutscene();
        for(unsigned i=8-afk_count;i<8;++i) {
            require(server.request_afk(ids[i],1,true)==CommandResult::Applied,"AFK transition");
            require(server.participant(ids[i])->character==i+1,"AFK replaced character");
            require(server.request_skip(ids[i],2,scene)==CommandResult::NotEligible,"AFK vote accepted");
        }
        require(server.skip_vote().required==8-afk_count,"AFK electorate");
        for(unsigned i=0;i<8-afk_count;++i)server.request_skip(ids[i],1,scene);
        require(server.skip_vote().skip_requested,"AFK players blocked unanimous active players");
    }
    SessionAuthority server;auto a=*server.join("A"),b=*server.join("B"),c=*server.join("C");
    for(auto p:{a,b,c}){server.attach_character(p,p.slot+1);server.finish_join(p);}
    auto scene=server.begin_cutscene();server.request_skip(a,1,scene);
    server.request_afk(b,1,true);server.request_afk(b,2,false);
    require(server.skip_vote().required==3 && server.skip_vote().received==1,"AFK return electorate");
    server.request_skip(c,1,scene);require(!server.skip_vote().skip_requested,"Returning player ignored");
    server.request_afk(b,3,true);require(server.skip_vote().skip_requested,"AFK change did not reconsider vote");
    auto decision=server.skip_decision();require(decision && decision->vote.required==2 && decision->vote.received==2 && decision->connected_mask==7 && decision->eligible_mask==5 && decision->afk_mask==2,"Unanimous decision electorate was not captured");
    server.request_afk(b,4,false);decision=server.skip_decision();
    require(server.skip_vote().required==3 && decision && decision->vote.required==2 && decision->eligible_mask==5 && decision->afk_mask==2,"AFK return rewrote an already committed decision");
    require(server.end_cutscene(scene) && !server.skip_decision(),"Completed-scene decision was not cleared");
    require(server.connected_count()==3 && server.participant(b)->character==2,"AFK lost slot or inventory entity");
    SessionAuthority empty;empty.begin_cutscene();require(!empty.skip_vote().skip_requested,"Empty electorate auto-skipped");
    SessionAuthority joining;auto first=*joining.join("First");joining.attach_character(first,1);joining.finish_join(first);
    auto second=*joining.join("Second");joining.attach_character(second,2);joining.finish_join(second);
    auto current=joining.begin_cutscene();joining.request_skip(first,1,current);
    auto late=*joining.join("Late");joining.attach_character(late,3);
    require(joining.skip_vote().required==2,"Loading join blocks cutscene vote");
    joining.finish_join(late);require(joining.skip_vote().required==3,"Ready join omitted from open vote");
    joining.request_skip(second,1,current);require(!joining.skip_vote().skip_requested,"Joined human omitted");
    joining.request_skip(late,1,current);require(joining.skip_vote().skip_requested,"Ready join cannot vote");
    require(!joining.attach_character(first,2) && !joining.attach_character(first,4),"AFK character identity replaced");
    joining.begin_loading();
    require(joining.connected_count()==3 && !joining.participant(first)->ready && !joining.participant(first)->character,"Loading retained old character or lost connection");
    require(!joining.finish_join(first),"Old readiness accepted without new character");
    require(joining.request_afk(first,2,true)==CommandResult::NotEligible,"Ordinary loading accepts an AFK command");
    require(joining.request_afk(first,3,true,true)==CommandResult::Applied && joining.request_afk(first,4,false,true)==CommandResult::Applied,"Explicit result-screen AFK return rejected");
    require(!joining.participant(first)->ready && !joining.participant(first)->character && !joining.skip_vote().cutscene,"Result AFK permission changed readiness, character or cutscene state");
    require(joining.request_afk(first,5,true)==CommandResult::NotEligible,"Result AFK permission leaked into later loading");
    require(joining.attach_character(first,104) && joining.finish_join(first),"New-level character cannot replace old binding");
    bool rejected=false;try{SessionAuthority invalid(9);}catch(...){rejected=true;}require(rejected,"Capacity above eight");
    rejected=false;try{SessionAuthority invalid(0);}catch(...){rejected=true;}require(rejected,"Zero capacity");
    std::cout<<"Server session policy: 1..8 slots, stale identities, AFK status and unanimous cutscene voting passed.\n";
}
