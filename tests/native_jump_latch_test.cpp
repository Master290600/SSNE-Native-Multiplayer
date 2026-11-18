#include "../runtime/src/native_jump_latch.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <map>

namespace {
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
ssne::detail::NativeJumpBinding owner(unsigned slot=0){return {0x80400000u+slot*0x1000,0x80300000u+slot*0x70C,0x80500000u+slot*0x100,1};}
using Latch=ssne::detail::NativeJumpLatch;
void pulse_lifecycle(){
    Latch latch;auto a=owner();
    require(latch.prepare(0,a,true,true,1,100).pressed,"Original press missing");
    latch.finish(0,false);
    const auto replay=latch.prepare(0,a,true,false,2,104);
    require(replay.pressed && replay.retained && !replay.queued,"Edge lost before120Hz step");
    latch.finish(0,true);
    require(!latch.prepare(0,a,true,false,3,108).pressed,"Consumed edge replayed twice");
    latch.queue(0,109);
    auto queued=latch.prepare(0,a,true,false,3,110);
    require(queued.pressed && queued.queued,"Complete between-sample tap lost");
    latch.finish(0,false);
    require(latch.prepare(0,a,true,false,4,159).pressed,"50ms edge expired early");
    require(!latch.prepare(0,a,true,false,5,160).pressed,"Expired edge survived50ms limit");
    latch.queue(0,161);latch.cancel(0);
    require(!latch.prepare(0,a,true,false,6,162).pressed,"Cancelled edge escaped to gameplay");
    latch.queue(0,163);auto b=a;b.life++;
    require(!latch.prepare(0,b,true,false,7,164).pressed,"Old life edge escaped to respawn");
    latch.queue(0,165);b.actor+=0x10000;
    require(!latch.prepare(0,b,true,false,8,166).pressed,"Old actor edge escaped to new slot owner");
    latch.queue(0,167);b.mode+=0x100;
    require(!latch.prepare(0,b,true,false,9,168).pressed,"Vehicle/movement transition kept stale edge");
    latch.queue(0,169);b.bank+=0x10000;
    require(!latch.prepare(0,b,true,false,10,170).pressed,"Controller bank transition kept stale edge");
    latch.queue(0,171);
    require(!latch.prepare(0,b,false,false,11,172).pressed,"Menu/death/watch/pending context accepted edge");
    require(!latch.prepare(0,b,true,false,12,173).pressed,"Blocked context rearmed old edge");
    // Timestamps are intentionally32-bit to pack an actor generation atomically.
    latch.queue(0,0xFFFFFFFEu);
    require(latch.prepare(0,b,true,false,13,2).pressed,"Clock wrap lost fresh edge");
    latch.finish(0,true);
    require(!latch.prepare(0,b,true,false,13,3).pressed,"Same pad processing pass rearmed consumption");
}
void eight_slots(){
    Latch latch;
    for(unsigned i=0;i<8;++i)require(!latch.prepare(i,owner(i),true,false,1,0).pressed,"New owner starts with a jump");
    for(unsigned i=0;i<8;++i)latch.queue(i,10+i);
    for(unsigned i=0;i<8;++i){
        require(latch.prepare(i,owner(i),true,false,2,20).pressed,"Logical slot missed own mailbox");
        latch.finish(i,i%2==0);
    }
    for(unsigned i=0;i<8;++i)require(latch.prepare(i,owner(i),true,false,3,24).pressed==(i%2!=0),"Logical slot retention aliases another player");
    latch.cancel_all();for(unsigned i=0;i<8;++i)require(!latch.prepare(i,owner(i),true,false,4,25).pressed,"World reset retained a player edge");
    latch.queue(8,30);latch.cancel(8);require(!latch.prepare(8,owner(),true,true,1,30).pressed,"Invalid slot aliases slot0");
}
void actual_binding_resolver(){
    using namespace ssne::detail;
    NativeJumpWorld world{0x81800000u,0x802121E8u,0x80300000u,0x80310000u,0x80320000u,8,true,42};
    std::map<uint32_t,uint32_t> words;
    auto read=[&](uint32_t p){return words[p];};
    for(unsigned i=0;i<8;++i){
        const auto actor=0x80400000u+i*0x1000;
        words[actor+0x48]=world.actor_type;words[actor+0x178]=i;words[actor+0x17C]=i;words[actor+0x344]=0x80500000u+i*0x100;
        words[world.registry+4*i]=actor;
        unsigned slot=8;NativeJumpBinding binding;
        require(resolve_native_jump_binding(actor,world,read,slot,binding),"Real native actor/controller binding rejected");
        require(slot==i && binding.actor==actor && binding.life==42,"Real native membership binding changed");
        require(binding.bank==(i<4?world.first_bank+i*0x70C:world.second_bank+(i-4)*0x70C),"Logical slots4..7 do not read their native second bank");
    }
    unsigned slot=8;NativeJumpBinding binding;const auto actor=0x80407000u;
    words[actor+0x178]=3;
    require(resolve_native_jump_binding(actor,world,read,slot,binding) && slot==7 && binding.bank==world.first_bank+3*0x70C,"Online mailbox aliases input index instead of membership");
    auto offline=world;offline.network=false;offline.count=4;words[actor+0x17C]=0;
    require(resolve_native_jump_binding(actor,offline,read,slot,binding) && slot==3,"Offline keyboard on controller3 routed to actor index0");
    words[actor+0x178]=7;offline.second_bank=0;
    require(!resolve_native_jump_binding(actor,offline,read,slot,binding),"Offline controller aliases an extended bank");
    words[actor+0x17C]=7;world.second_bank=0;
    require(!resolve_native_jump_binding(actor,world,read,slot,binding),"Missing second native bank accepted");
    world.second_bank=0x80310000u;words[world.registry+7*4]=0;
    require(!resolve_native_jump_binding(actor,world,read,slot,binding),"Wrong native registry ownership accepted");
}
// Independent original action/physics oracle. A press is a transition from
// zero analog value; Physics samples only each1/120s, not each rendered frame.
struct Action {
    bool pressed=false,held=false;
    void process(bool value){
        if(value){pressed=!held;if(pressed)held=true;}
        else if(held){pressed=false;held=false;}
    }
};
struct Counts {unsigned kept=0,legacy=0;};
Counts high_fps(unsigned fps){
    Latch latch;auto a=owner(7);Action input;
    Counts count;double acc=0;uint64_t frame=0;
    constexpr unsigned presses=24;
    // Leave1.1seconds between pulses, as the unmodified ground movement has
    // no midair repeat. Force every pulse to start on a rendered frame that
    // precedes rather than contains a120Hz physics step whenever fps>120.
    for(unsigned n=0;n<presses;++n){
        bool armed=false,seen=false;
        for(unsigned local=0;local<fps*2;++local){
            ++frame;const auto now=uint32_t((double(frame)*1000)/fps);
            const auto prospective=acc+1.0/double(fps);
            bool down=false;
            if(!armed && (fps<=120 || prospective<1.0/120)){down=true;armed=true;}
            input.process(down);
            const auto decision=latch.prepare(7,a,true,input.pressed,frame,now);
            acc=prospective;bool stepped=false;
            while(acc+1e-12>=1.0/120){
                acc-=1.0/120;stepped=true;
                if(input.pressed && !seen)++count.legacy;
                if(decision.pressed && !seen){++count.kept;seen=true;}
            }
            latch.finish(7,stepped);
            if(armed && local>fps)break;
        }
    }
    require(count.kept==presses,"A valid ground tap disappeared between input and fixed physics");
    if(fps<=120)require(count.legacy==presses,"Original120Hz-or-slower oracle changed");
    else require(count.legacy<presses,"Negative control does not expose the original no-step-frame loss");
    return count;
}
void held_and_scoped_action(){
    Latch latch;auto a=owner();Action input;unsigned edges=0;bool injected=false;
    for(uint64_t frame=1;frame<=1000;++frame){
        // Constant held Space is one edge, even when the native physics
        // accumulator consumes only every fourth rendered frame.
        input.process(true);const auto original=input.pressed;
        const auto decision=latch.prepare(0,a,true,original,frame,uint32_t(frame));
        if(decision.pressed)injected=true;
        const bool step=frame%4==0;
        if(step && decision.pressed)++edges;
        latch.finish(0,step);
        // The real integration restores the borrowed pressed field after the
        // original call; it never changes held/analog state.
        require(input.pressed==original && input.held,"Scoped replay mutated the raw held state");
    }
    require(injected && edges==1,"Holding jump created automatic repeated jumps");
    input.process(false);require(!input.pressed && !input.held,"Original release oracle broken");
    latch.queue(0,1001);auto decision=latch.prepare(0,a,true,false,1001,1001);
    bool borrowed=input.pressed;if(decision.pressed)input.pressed=true;
    require(input.pressed,"Queued tap never reached original jump input");
    input.pressed=borrowed;latch.finish(0,true);input.process(false);
    require(!input.pressed,"Synthetic pressed stuck in original value0/held0 branch");
}
void cross_thread_mailbox(){
    Latch latch;auto a=owner(5);latch.prepare(5,a,true,false,1,100);
    std::thread producer([&]{latch.queue(5,101);});producer.join();
    require(latch.prepare(5,a,true,false,2,102).pressed,"UI-thread press did not cross atomic mailbox");
    std::thread cancel([&]{latch.cancel(5);});cancel.join();
    require(!latch.prepare(5,a,true,false,3,103).pressed,"UI-thread cancellation kept guest pending state");
}
}
int main(){try{
    pulse_lifecycle();eight_slots();actual_binding_resolver();held_and_scoped_action();cross_thread_mailbox();
    for(unsigned fps:{60u,120u,144u,240u,300u,600u,1000u}){
        const auto counts=high_fps(fps);std::cout<<fps<<" fps: kept="<<counts.kept<<" legacy="<<counts.legacy<<'\n';
    }
    std::cout<<checks<<" assertions passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
