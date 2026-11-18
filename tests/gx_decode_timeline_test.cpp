#include "../runtime/src/gx/gx_decode_timeline.h"
#include <iostream>
#include <stdexcept>
using namespace ssne;
using namespace ssne::detail;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
GxDecodeChoice choice(unsigned bits){GxDecodeChoice result;require(gx_decode_choice_from_bits(bits,result),"Invalid fixture");return result;}
}
int main(){try{
    const auto a=choice(33),b=choice(50);const GxDecodeChoice live{};
    GxDecodeMarkerQueue<4> queue;
    for(unsigned i=0;i<100000;++i)require(queue.push({0,i&1?a:b}),"Same-offset replacement exhausted queue");
    require(queue.size()==1&&queue.high_water()==1,"Byte-less scope changes grew queue");
    require(queue.push({1,a})&&queue.push({2,b})&&queue.push({3,live}),"Bounded queue append failed");
    require(!queue.push({4,a})&&queue.size()==4,"Full queue did not apply backpressure");
    require(!queue.push({2,a}),"Out-of-order queue append accepted");
    queue.pop();queue.pop();require(queue.push({4,a})&&queue.push({5,b}),"Wrapped queue append failed");
    for(unsigned expected=2;expected<6;++expected){require(queue.front().offset==expected,"Wrapped queue order changed");queue.pop();}
    require(queue.size()==0&&queue.high_water()==4,"Queue bound not preserved");
    GxDecodeTimeline timeline;require(timeline.append({10,a})&&timeline.append({12,b})&&timeline.append({13,live}),"Timeline append failed");
    require(timeline.at(9)==live&&timeline.at(10)==a&&timeline.at(12)==b&&timeline.at(13)==live,"Opcode-start lookup incorrect");
    timeline.compact_pending(10,14);
    require(timeline.size()==2&&timeline.at(10)==a&&timeline.at(14)==live,"Partial-command context was reclassified");
    for(unsigned i=0;i<10000;++i){require(timeline.append({14+i,i&1?a:b}),"Partial history append failed");timeline.compact_pending(10,15+i);require(timeline.size()<=2&&timeline.at(10)==a,"Partial history grew or changed start");}
    const auto saved=timeline.pending_state(10,10014);
    require(valid_gx_decode_pending_state(saved,10004),"Canonical pending state rejected");
    GxDecodeTimeline restored;restored.restore_pending(saved);
    require(restored.at(0)==timeline.at(10)&&restored.at(10004)==timeline.at(10014),"Pending context roundtrip changed bits");
    GxDecodeTimeline tagged;GxDecodeMarkerDebug tag1,tag2;tag1.serial=1;tag2.serial=2;
    require(tagged.append({10,a,tag1,10})&&tagged.append({12,a,tag2,12}),"Same-flags diagnostic marker rejected");
    tagged.compact_pending(10,14);
    require(tagged.find(10)->debug.serial==1&&tagged.find(10)->marker_offset==10&&tagged.find(14)->debug.serial==2&&tagged.find(14)->marker_offset==12,"Compaction lost original marker source");
    require(tagged.pending_state(10,14).size()==1,"Diagnostic-only tag changed GX02 semantic masks");
    tagged.compact_pending(11,15);
    require(tagged.find(11)->debug.serial==1&&tagged.find(11)->marker_offset==10&&tagged.find(15)->debug.serial==2&&tagged.find(15)->marker_offset==12,"Repeated compaction relabeled marker origin");
    GxDecodeOffsetClock clock;clock.restore(0x100000005ull,7);
    require(clock.logical(0x100000005ull)==7&&clock.logical(0x100000009ull)==11,"Loaded prefix offset clock incorrect");
    for(unsigned bits=0;bits<128;++bits){GxDecodeChoice value;const bool valid=bits==0||(bits>=32&&bits<64);require(gx_decode_choice_from_bits(bits,value)==valid,"Context bit validation incorrect");if(valid)require(gx_decode_choice_bits(value)==bits,"Context bit roundtrip changed flags");}
    require(!valid_gx_decode_pending_state({},5),"Empty state accepted");
    require(!valid_gx_decode_pending_state({{1,a}},5),"Nonzero prefix accepted");
    require(!valid_gx_decode_pending_state({{0,a},{4,b}},5),"Interior marker accepted");
    require(!valid_gx_decode_pending_state({{0,a},{5,a}},5),"Redundant state accepted");
    require(!valid_gx_decode_pending_state({{0,a},{0,b}},0),"Empty prefix with two contexts accepted");
    std::cout<<"GX context timeline: queue bound, zero-byte coalescing, wrap, partial prefixes and serialized masks passed\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
