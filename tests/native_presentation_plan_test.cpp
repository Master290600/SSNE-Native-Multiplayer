#include "native_replica.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <type_traits>
static_assert(!std::is_copy_constructible_v<port::coop::NativeReplica> && !std::is_copy_assignable_v<port::coop::NativeReplica>);
static_assert(!std::is_move_constructible_v<port::coop::NativeReplica> && !std::is_move_assignable_v<port::coop::NativeReplica>);
using port::coop::presentation_detail::VisualWrite;
using port::coop::presentation_detail::normalize_visual_plan;
static void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
static bool same(const VisualWrite& a,const VisualWrite& b){return a.address==b.address&&a.value==b.value&&a.ordinal==b.ordinal&&a.replace==b.replace;}
static void check(const std::vector<VisualWrite>& input){
    // Independent semantic oracle: the first write creates an address and only
    // replace=true may change it. This is the original map/try_emplace contract.
    std::map<uint32_t,VisualWrite> oracle;
    for(const auto& write:input){auto [entry,inserted]=oracle.emplace(write.address,write);if(!inserted&&write.replace)entry->second=write;}
    auto legacy=input,optimized=input;normalize_visual_plan(legacy,false);normalize_visual_plan(optimized,true);
    require(legacy.size()==oracle.size()&&optimized.size()==oracle.size(),"Alias normalization changed unique address count");
    size_t i=0;for(const auto& [address,expected]:oracle){require(same(legacy[i],expected),"Stable reference differs from map/try_emplace contract");require(same(optimized[i],expected),"Ordinal plan differs from alias last-write contract");if(i)require(optimized[i-1].address<optimized[i].address,"Plan contains duplicate or unordered addresses");++i;}
}
int main(){
    check({});
    check({{0x80000010,0x7fc01234,0,false},{0x80000010,0x3f800000,1,false},{0x80000010,0x80000000,2,true},{0x80000010,0xdeadbeef,3,false},{0x80000004,0x00000000,4,true},{0x80000004,0x3f000000,5,true}});
    check({{4,11,0,false},{4,12,1,false},{8,13,2,true},{8,14,3,false}});
    std::mt19937 random(0x53414d35u);
    for(unsigned trial=0;trial<400;++trial){
        std::vector<VisualWrite> writes;const unsigned count=trial%7==0?0:unsigned(random()%4096);
        writes.reserve(count);for(unsigned i=0;i<count;++i)writes.push_back({uint32_t(0x80001000u+4u*uint32_t(random()%192u)),uint32_t(random()),i,(random()%4u)!=0});check(writes);
    }
    // Reuse a retained allocation across shrinking/growing plans; stale tail
    // entries must never become writes after a failed or empty lease.
    std::vector<VisualWrite> retained;retained.reserve(1024);
    for(unsigned pass=0;pass<64;++pass){retained.clear();for(unsigned i=0;i<pass%17;++i)retained.push_back({0x80002000u+4u*(i%3),pass*100+i,i,i%2!=0});check(retained);normalize_visual_plan(retained,true);}
    std::cout<<"PASS presentation plan: stable reference, last-write/try-emplace alias order, exact uint32 payloads, empty/shrinking plans and400 deterministic adversarial cases\n";
}
