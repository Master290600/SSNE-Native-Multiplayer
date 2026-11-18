#include "ssne/host_frame_dispatch.h"
#include "ssne/ppc_context.h"
#include <cstring>
#include <array>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
namespace {
using ssne::HostFrameDispatchPolicy;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void gates(){
    // Independent eight-row truth table: a ticket exists only when render,
    // pre-flip, and post-SDK facts are all still safe. The two late unsafe
    // cases must cancel, rather than replaying once the next gate becomes true.
    for(unsigned bits=0;bits<8;++bits){
        HostFrameDispatchPolicy policy;
        const bool render=(bits&1)!=0,flip=(bits&2)!=0,after=(bits&4)!=0;
        require(!policy.pending()&&!policy.take(true),"Default policy retained work");
        policy.begin_render(render);
        require(policy.schedule(77,flip)==(render&&flip),"Render/flip gate mismatch");
        require(policy.pending()==(render&&flip),"Scheduled ticket state mismatch");
        const auto token=policy.take(after);
        require(bool(token)==(bits==7),"Three-boundary gate mismatch");
        if(token)require(*token==77,"Original frame token was changed");
        require(!policy.pending()&&!policy.take(true)&&!policy.take(false),"Ticket ran more than once or survived cancellation");
        require(!policy.schedule(78,true),"Schedule reused consumed render permission");
    }
}
void transitions(){
    HostFrameDispatchPolicy policy;
    require(!policy.schedule(1,true),"Flip without a render became eligible");
    policy.begin_render(true);require(policy.schedule(2,true),"Safe frame was not deferred");
    require(!policy.take(false),"Save/epoch/owner transition did not cancel");
    require(!policy.take(true),"Canceled network work was replayed");
    policy.begin_render(false);require(!policy.schedule(3,true),"Legacy render became deferred");
    policy.begin_render(true);require(!policy.schedule(4,false),"Late unsafe flip became deferred");
    require(!policy.take(true),"Unsafe flip left a ticket");
    // Owner removal must revoke both a scheduled ticket and a render which
    // has not reached flip yet. A replacement starts from the legacy path.
    policy.begin_render(true);require(policy.schedule(5,true),"Owner ticket not staged");policy.clear();
    require(!policy.pending()&&!policy.take(true),"Owner removal retained a scheduled ticket");
    policy.begin_render(true);policy.clear();require(!policy.schedule(6,true),"Owner removal retained render permission");
    policy.begin_render(true);require(policy.schedule(7,true),"Old ticket not staged");policy.begin_render(false);
    require(!policy.pending()&&!policy.take(true),"Next render inherited an old ticket");
    for(const uint64_t token:{uint64_t(0),std::numeric_limits<uint64_t>::max()}){
        policy.begin_render(true);require(policy.schedule(token,true),"Boundary token not scheduled");
        const auto result=policy.take(true);require(result&&*result==token,"Boundary frame token truncated");
    }
}
void sequential_frames(){
    HostFrameDispatchPolicy policy;
    std::array<unsigned,64> observed{};
    unsigned native_updates=0,legacy=0,deferred=0,canceled=0;
    for(uint64_t frame=0;frame<observed.size();++frame){
        // Simulation is deliberately outside the dispatch policy. Gate changes
        // must not catch up, replay, or create any additional native update.
        ++native_updates;
        const bool render=frame%4!=0,flip=frame%5!=0,after=frame%7!=0;
        policy.begin_render(render);
        if(!policy.schedule(frame,flip)){++observed[frame];++legacy;}
        else if(const auto ticket=policy.take(after)){require(*ticket==frame,"Callback received another frame's token");++observed[*ticket];++deferred;}
        else ++canceled;
        require(!policy.take(true),"Repeated post-SDK entry ran observer twice");
    }
    require(native_updates==observed.size(),"Policy added a native update");
    require(legacy+deferred+canceled==observed.size(),"Native frame was dispatched or canceled twice");
    for(uint64_t frame=0;frame<observed.size();++frame){
        const bool canceled_frame=frame%4!=0&&frame%5!=0&&frame%7==0;
        require(observed[frame]==unsigned(!canceled_frame),"Observer call count or cancellation differs");
    }
}
}
PPCContext context(uint32_t seed){
    PPCContext c;
    for(unsigned i=0;i<32;++i){c.r[i]=seed+i*23;c.ps0[i]=u2d((uint64_t(seed+i)<<32)|uint32_t(~(seed+i)));c.ps1[i]=u2d((uint64_t(seed+i+32)<<32)|uint32_t(~(seed+i+32)));}
    for(unsigned i=0;i<8;++i){c.cr[i]=uint8_t(seed+i);c.gqr[i]=seed+i*17;}
    c.lr=seed+1;c.ctr=seed+2;c.xer_so=seed+3;c.xer_ov=seed+4;c.xer_ca=seed+5;c.fpscr=seed+6;
    c.hid2=seed+7;c.msr=seed+8;c.srr0=seed+9;c.srr1=seed+10;c.dec=seed+11;
    for(unsigned i=0;i<4;++i)c.sprg[i]=seed+i*7;
    return c;
}
bool same(const PPCContext& a,const PPCContext& b){
    // Compare every architectural field, including floating-point bit patterns;
    // implementation padding is not part of the CPU state contract.
    return !std::memcmp(a.r,b.r,sizeof(a.r))&&!std::memcmp(a.ps0,b.ps0,sizeof(a.ps0))&&
        !std::memcmp(a.ps1,b.ps1,sizeof(a.ps1))&&!std::memcmp(a.cr,b.cr,sizeof(a.cr))&&
        !std::memcmp(a.gqr,b.gqr,sizeof(a.gqr))&&!std::memcmp(a.sprg,b.sprg,sizeof(a.sprg))&&
        a.lr==b.lr&&a.ctr==b.ctr&&a.xer_so==b.xer_so&&a.xer_ov==b.xer_ov&&a.xer_ca==b.xer_ca&&
        a.fpscr==b.fpscr&&a.hid2==b.hid2&&a.msr==b.msr&&a.srr0==b.srr0&&a.srr1==b.srr1&&a.dec==b.dec;
}
void preserved_context(){
    const auto entry=context(0x12340000),sdk=context(0x56780000),mutation=context(0x9abc0000);
    auto current=sdk;auto* const scheduler_object=&current;unsigned called=0;uint32_t guest_effect=0;
    ssne::with_preserved_context(current,entry,[&](PPCContext& observed){
        ++called;require(&observed==scheduler_object,"Observer used another CPU object");
        require(same(observed,entry),"Observer did not receive original flip-entry registers");
        observed=mutation;guest_effect=44;
    });
    require(called==1&&same(current,sdk)&&same(entry,context(0x12340000)),"SDK CPU state or entry copy changed");
    require(guest_effect==44,"Callback side effect was rolled back with CPU state");
    bool caught=false;
    try{ssne::with_preserved_context(current,entry,[&](PPCContext& observed){
        require(&observed==scheduler_object&&same(observed,entry),"Throwing observer context mismatch");
        observed=mutation;guest_effect=55;throw std::runtime_error("expected callback exception");
    });}catch(const std::runtime_error&){caught=true;}
    require(caught&&same(current,sdk)&&guest_effect==55,"Exception lost SDK return state or undid callback effect");
    // Reentrant native observers retain the same scheduler identity at both
    // levels and restore each caller's exact CPU state on return.
    const auto nested=context(0x24680000);
    ssne::with_preserved_context(current,entry,[&](PPCContext& observed){
        observed=mutation;
        ssne::with_preserved_context(observed,nested,[&](PPCContext& inner){
            require(&inner==scheduler_object&&same(inner,nested),"Nested observer CPU identity/registers changed");inner=sdk;
        });
        require(same(observed,mutation),"Nested observer lost outer callback state");
    });
    require(same(current,sdk),"Nested callback changed SDK return state");
    // A changed SDK branch/result must be preserved rather than restoring a
    // previous invocation's output. Entry/current may safely alias as well.
    current=nested;ssne::with_preserved_context(current,entry,[&](PPCContext& observed){observed=mutation;});
    require(same(current,nested),"Changed SDK return branch restored stale CPU registers");
    ssne::with_preserved_context(current,current,[&](PPCContext& observed){require(same(observed,nested),"Aliased entry changed before dispatch");observed=mutation;});
    require(same(current,nested),"Aliased entry lost SDK CPU state");
}
int main(){try{gates();transitions();sequential_frames();preserved_context();
    std::cout<<"Host frame dispatch: fresh three-boundary gates, exact tokens, one-shot cancellation, owner clear and same-object PPC context restoration passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
