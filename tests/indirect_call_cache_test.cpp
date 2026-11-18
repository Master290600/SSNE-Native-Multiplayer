#include "ssne/recomp_hooks.h"
#include "ssne/runtime.h"
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
constexpr uint32_t a=0x80001000u,b=0x80001804u,unknown=0x80002A00u;
static_assert((((a>>2)^(a>>11))&511)==(((b>>2)^(b>>11))&511));
std::atomic<unsigned> bodies{0},hooks{0};
uint64_t digest=14695981039346656037ull;
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
PPCContext seed(){PPCContext c;std::memset(&c,0,sizeof(c));c.r[3]=7;c.r[4]=2;c.ps0[1]=8.;c.ps1[1]=4.;c.lr=0x80003000;return c;}
void pure_a(PPCContext& c){c.r[3]=c.r[3]*3+c.r[4];c.ps0[1]=c.ps0[1]*.5+3.;c.ps1[1]=c.ps1[1]*2.-1.;c.cr[2]=4;}
void pure_b(PPCContext& c){c.r[3]^=0x1234;c.ps0[2]=c.ps0[1]+.25;c.cr[3]=2;}
void original_a(PPCContext& ctx){
    SSNE_HOOK(a);++bodies;pure_a(ctx);
    if(ctx.r[9])throw std::runtime_error("original failure");
    if(ctx.r[8]){--ctx.r[8];ssne_call(ctx,b);}
    if(ctx.r[6]){--ctx.r[6];ssne_call(ctx,a);}
}
void original_b(PPCContext& ctx){SSNE_HOOK(b);++bodies;pure_b(ctx);if(ctx.r[7]){--ctx.r[7];ssne_call(ctx,a);}}
void intercept_a(PPCContext& ctx){++hooks;ctx.r[5]+=5;ssne_call_original(ctx,a,original_a);}
void replacement_a(PPCContext& ctx){++hooks;ctx.r[5]+=13;ssne_call_original(ctx,a,original_a);}
void intercept_b(PPCContext& ctx){++hooks;ctx.r[5]+=9;ssne_call_original(ctx,b,original_b);}
void unknown_hook(PPCContext& ctx){ctx.r[3]=999;ctx.ps0[1]=-7.5;}
void fail_before_entry(PPCContext&){throw std::runtime_error("before entry");}
void fatal_as_exception(const char* message){throw std::runtime_error(message);}
void same(const PPCContext& got,const PPCContext& expected,const char* message){require(!std::memcmp(&got,&expected,sizeof(got)),message);}
void note(const PPCContext& c){for(const auto value:{uint64_t(c.r[3]),uint64_t(c.r[5]),std::bit_cast<uint64_t>(c.ps0[1]),std::bit_cast<uint64_t>(c.ps1[1]),std::bit_cast<uint64_t>(c.ps0[2])})digest=(digest^value)*1099511628211ull;}
}
// The actual immutable generated-table contract, using synthetic functions only.
const SSNEFuncEntry g_ssne_funcs[]={{a,original_a,"synthetic_a"},{b,original_b,"synthetic_b"}};
const size_t g_ssne_func_count=2;
const uint32_t g_ssne_entry=a;
const bool g_ssne_has_game_code=false;
int main(){
    // Keep the intentional invalid-call test from writing diagnostic RAM dumps
    // when invoked from a developer shell which is profiling another process.
#ifdef _WIN32
    _putenv_s("SSNE_PERF_LOG","");
#else
    unsetenv("SSNE_PERF_LOG");
#endif
    ssne::set_fatal_handler(fatal_as_exception);
    auto* slot=ssne_hook_slot(a);require(slot && !*slot,"Initial hook slot is not empty");
    for(unsigned i=0;i<20;++i){auto got=seed(),expected=got;pure_a(expected);ssne_call(got,a);same(got,expected,"Unhooked typed output changed");note(got);}
    // A physical target must share the already warmed virtual target/slot.
    {auto got=seed(),expected=got;pure_a(expected);ssne_call(got,a&0x7FFFFFFFu);same(got,expected,"Physical alias changed typed output");note(got);}
    ssne_install_hook(a,intercept_a);
    {auto got=seed(),expected=got;expected.r[5]+=5;pure_a(expected);ssne_call(got,a);same(got,expected,"Installed hook was hidden by warmed cache");note(got);}
    ssne_install_hook(a,replacement_a);
    {auto got=seed(),expected=got;expected.r[5]+=13;pure_a(expected);ssne_call(got,a&0x7FFFFFFFu);same(got,expected,"Replacement hook or physical alias was stale");note(got);}
    // Direct writes through the public stable slot must also be observed.
    *slot=nullptr;
    {auto got=seed(),expected=got;pure_a(expected);ssne_call(got,a);same(got,expected,"Uninstalled hook remained cached");note(got);}
    ssne_install_hook(a,intercept_a);ssne_install_hook(b,intercept_b);
    for(unsigned i=0;i<40;++i){
        auto got=seed(),expected=got;
        expected.r[5]+=5;pure_a(expected);expected.r[5]+=9;pure_b(expected);expected.r[5]+=5;pure_a(expected);
        ssne_call(got,a);ssne_call(got,b);ssne_call(got,a);
        same(got,expected,"Direct-mapped collision changed dispatch output");note(got);
    }
    // Original bypass applies to one entry only; recursive calls remain hooked.
    {auto got=seed(),expected=got;got.r[6]=expected.r[6]=3;const auto before=hooks.load();
        for(unsigned i=0;i<4;++i){expected.r[5]+=5;pure_a(expected);}expected.r[6]=0;
        ssne_call(got,a);same(got,expected,"Nested original scope suppressed a recursive hook");require(hooks==before+4,"Recursive hook count changed");note(got);}
    // Reentrant B -> A replaces the outer A cache entry while its call is live.
    {auto got=seed(),expected=got;got.r[8]=expected.r[8]=1;got.r[7]=expected.r[7]=1;
        expected.r[5]+=5;pure_a(expected);expected.r[8]=0;
        expected.r[5]+=9;pure_b(expected);expected.r[7]=0;expected.r[5]+=5;pure_a(expected);
        ssne_call(got,a);same(got,expected,"Reentrant colliding call used an invalid cache entry");note(got);}
    {auto got=seed();got.r[9]=1;bool failed=false;try{ssne_call(got,a);}catch(const std::runtime_error&){failed=true;}require(failed,"Original exception was swallowed");
        failed=false;try{ssne_call_original(got,a,fail_before_entry);}catch(const std::runtime_error&){failed=true;}require(failed,"Pre-entry exception was swallowed");
        got=seed();auto expected=got;expected.r[5]+=5;pure_a(expected);ssne_call(got,a);same(got,expected,"Exception leaked an original bypass");note(got);}
    // Force map rehashes; cached pointers must keep pointing at the same node.
    for(uint32_t i=0;i<8192;++i)ssne_install_hook(0x81000000u+i*4,nullptr);
    require(ssne_hook_slot(a)==slot,"Map rehash invalidated stable hook slot");
    {auto got=seed(),expected=got;expected.r[5]+=5;pure_a(expected);ssne_call(got,a);same(got,expected,"Rehash invalidated cached slot");note(got);}
    // Unknown original targets retain the fatal path, but can acquire a hook
    // immediately after a cached miss, then return to fatal after uninstall.
    for(unsigned attempt=0;attempt<2;++attempt){
        auto got=seed();bool failed=false;try{ssne_call(got,unknown);}catch(const std::runtime_error& e){failed=std::strstr(e.what(),"80002A00")!=nullptr;}
        require(failed,"Unknown indirect target did not preserve its fatal diagnostic");
        ssne_install_hook(unknown,unknown_hook);auto expected=seed();expected.r[3]=999;expected.ps0[1]=-7.5;
        got=seed();ssne_call(got,unknown&0x7FFFFFFFu);same(got,expected,"Unknown target hook was hidden by cached miss");note(got);ssne_install_hook(unknown,nullptr);
    }
    // Each host thread gets an independent lookup cache; installed slot state
    // remains shared and all calls preserve the same typed register results.
    auto worker=[] {for(unsigned i=0;i<200;++i){auto got=seed(),expected=got;expected.r[5]+=5;pure_a(expected);ssne_call(got,a);same(got,expected,"Other-thread dispatch changed typed output");}};
    std::thread first(worker),second(worker);first.join();second.join();
    ssne_install_hook(a,nullptr);ssne_install_hook(b,nullptr);ssne::set_fatal_handler(nullptr);
    std::cout<<"Indirect call cache: hooks/original/reentry/collisions/aliases/misses/thread-local parity passed; digest="<<digest<<".\n";
}
