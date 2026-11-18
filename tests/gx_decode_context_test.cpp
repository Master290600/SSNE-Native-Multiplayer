// Exercise the actual FIFO/parser TU; the archive supplies all other modules.
#define SSNE_GX_FIFO_CONTEXT_TEST 1
#include "../runtime/src/gx/gx_fifo.cpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <array>
namespace {
using namespace ssne;
using ssne::detail::GxDecodeChoice;
struct Event { uint8_t opcode;int depth;GxDecodeChoice choice; };
std::vector<Event> events;
bool collect=true;
uint64_t observed=0;
const GxDecodeContext a{true,false,false,true,false},b{false,true,true,false,true};
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool observer(uint8_t opcode,uint64_t,int depth){
    const auto* override=ssne::gx_decode_context_override();
    if(collect)events.push_back({opcode,depth,override?GxDecodeChoice{true,*override}:GxDecodeChoice{}});
    ++observed;
    return opcode!=0x40; // Let actual recursive-DL dispatch run; bypass raster side effects.
}
void write(const std::vector<uint8_t>& bytes,size_t begin,size_t end){
    for(size_t i=begin;i<end;){const size_t n=std::min<size_t>(4,end-i);uint32_t value=0;for(size_t k=0;k<n;++k)value=(value<<8)|bytes[i+k];ssne::gx_fifo_write(value,int(n));i+=n;}
}
void write(const std::vector<uint8_t>& bytes){write(bytes,0,bytes.size());}
void mark(const GxDecodeContext& context){ssne::gx_mark_decode_context(context);require(!ssne::gx_decode_context_override(),"TLS override leaked to producer");}
void reset_events(){ssne::gx_sync();events.clear();observed=0;}
GxDecodeChoice expected(const GxDecodeContext& context){return ssne::gx_scoped_context_enabled()?GxDecodeChoice{true,context}:GxDecodeChoice{};}
std::vector<uint8_t> save(){ssne::StateWriter writer;ssne::gx_state_save(writer);return writer.buf;}
bool load(const std::vector<uint8_t>& bytes,bool apply=true){ssne::StateReader reader{bytes.data(),bytes.size()};return ssne::gx_state_load(reader,apply)&&reader.ok&&reader.pos==reader.n;}
void assert_prefix(const std::vector<uint8_t>& bytes,const char* tag){require(bytes.size()>=4&&!std::memcmp(bytes.data(),tag,4),"Wrong GX state tag");}
void live_state_roundtrip(){
    // Unmarked/SP-style streams retain the exact legacy state format even
    // when the context-support process default is enabled.
    ssne::gx_clear_decode_context();write({0});ssne::gx_sync();
    const auto empty=save();assert_prefix(empty,"GX01");require(load(empty,false)&&load(empty),"Live-only empty GX01 roundtrip failed");
    const std::vector<uint8_t> command{0x10,0,1,0,0,0,0,0,0,0,0,0,0};
    for(size_t split=1;split<command.size();++split){
        ssne::gx_clear_decode_context();write(command,0,split);ssne::gx_sync();const auto state=save();assert_prefix(state,"GX01");
        require(load(state,false)&&load(state),"Live-only pending GX01 roundtrip failed");reset_events();write(command,split,command.size());write({0});ssne::gx_sync();
        require(events.size()==2&&!events[0].choice.marked&&!events[1].choice.marked,"Live-only GX01 acquired a scoped classification");
    }
    // Both directions across a partial opcode need GX02: a known prefix must
    // survive a later clear, and a future known context must follow a live prefix.
    for(bool known_prefix:{false,true}){
        reset_events();if(known_prefix)mark(a);else ssne::gx_clear_decode_context();write(command,0,3);
        if(known_prefix)ssne::gx_clear_decode_context();else mark(b);ssne::gx_sync();const auto state=save();
        assert_prefix(state,ssne::gx_scoped_context_enabled()?"GX02":"GX01");require(load(state),"Mixed live/known pending state rejected");
        reset_events();write(command,3,command.size());write({0});ssne::gx_sync();
        require(events.size()==2&&events[0].choice==(known_prefix?expected(a):GxDecodeChoice{})&&events[1].choice==(known_prefix?GxDecodeChoice{}:expected(b)),"Mixed live/known state lost opcode-start or next classification");
    }
    // A byte-less known choice also governs future bytes after restore.
    mark(a);ssne::gx_sync();const auto known_empty=save();assert_prefix(known_empty,ssne::gx_scoped_context_enabled()?"GX02":"GX01");
    require(load(known_empty),"Known next-opcode state rejected");reset_events();write({0});ssne::gx_sync();require(events.size()==1&&events[0].choice==expected(a),"Known byte-less choice lost after restore");
    ssne::gx_clear_decode_context();write({0});ssne::gx_sync();assert_prefix(save(),"GX01");
}
void split_commands(){
    ssne::gx::g.cp[0x50]=1u<<9;ssne::gx::g.cp[0x70]=1u|(4u<<1); // XYZ float, 12 bytes/vertex.
    std::vector<std::vector<uint8_t>> commands={
        {0x08,0x50,0,0,2,0},
        {0x10,0,0,0,0,0,0,0,0},
        {0x10,0,1,0,0,0,0,0,0,0,0,0,0},
        {0x61,0,0,0,0},
        {ssne::gx::kCrosshairOpcode,ssne::gx::kCrosshairMagic,0,0,0,0,0,0,0,0},
        {0x90,0,1,0,0,0,0,0,0,0,0,0,0,0,0},
        {0x20,0,0,0,0},{0x44},{0x00},{0x7f}
    };
    for(const auto& command:commands)for(size_t split=1;split<=command.size();++split){
        reset_events();mark(a);write(command,0,split);mark(b);write(command,split,command.size());write({0});ssne::gx_sync();
        require(events.size()==2,"Split command emitted wrong event count");
        require(events[0].opcode==command[0]&&events[0].choice==expected(a),"Split header/payload changed opcode-start context");
        require(events[1].opcode==0&&events[1].choice==expected(b),"Next opcode did not inherit latest context");
        require(!ssne::gx_decode_context_override(),"TLS override leaked after parser return");
    }
    reset_events();mark(a);mark(b);write({0});ssne::gx_sync();
    require(events.size()==1&&events[0].choice==expected(b),"Same-offset replacement changed byte classification");
    reset_events();mark(a);ssne::gx_clear_decode_context();write({0});ssne::gx_sync();
    require(events.size()==1&&!events[0].choice.marked,"Clear did not restore legacy/live fallback");
}
void nested_lists(){
    // The top-level DL opcode's START determines every recursive inner opcode.
    std::array<uint8_t,9> inner{0x40,0x80,0x00,0x20,0x00,0,0,0,1};
    std::memcpy(ssne::host_ptr(0x80001000),inner.data(),inner.size());*ssne::host_ptr(0x80002000)=0;
    const std::vector<uint8_t> command{0x40,0x80,0,0x10,0,0,0,0,9};
    for(size_t split=1;split<command.size();++split){
        reset_events();mark(a);write(command,0,split);mark(b);write(command,split,command.size());write({0});ssne::gx_sync();
        require(events.size()==4,"Nested display-list event count incorrect");
        require(events[0].depth==0&&events[1].depth==1&&events[2].depth==2,"Display-list recursion did not run production parser");
        for(unsigned i=0;i<3;++i)require(events[i].choice==expected(a),"Nested display-list context did not inherit parent");
        require(events[3].depth==0&&events[3].choice==expected(b),"Nested context leaked into next root opcode");
    }
}
void state_roundtrip(){
    const std::vector<uint8_t> command{0x10,0,1,0,0,0,0,0,0,0,0,0,0};
    for(size_t split=1;split<command.size();++split){
        mark(a);write(command,0,split);mark(b);ssne::gx_sync();const auto state=save();
        assert_prefix(state,ssne::gx_scoped_context_enabled()?"GX02":"GX01");
        const auto pending=ssne::g_pending;const auto cp=ssne::gx::g.cp[0x50];const auto head=ssne::g_head.load();
        require(load(state,false),"Validation-only state rejected");
        require(ssne::g_pending==pending&&ssne::gx::g.cp[0x50]==cp&&ssne::g_head.load()==head,"Validation-only load mutated live state");
        require(load(state),"Pending state restore failed");reset_events();write(command,split,command.size());write({0});ssne::gx_sync();
        require(events.size()==2&&events[0].choice==expected(a)&&events[1].choice==expected(b),"Pending prefix restore lost opcode context");
        require(ssne::g_head.load()>=head,"State restore rewound raw ring counter");
        if(ssne::gx_scoped_context_enabled()){
            // Strip only the explicit extension to form a genuine legacy GX01 chunk.
            // Locate the extension from its fixed two-context pending-state representation.
            auto legacy=state;const size_t metadata=8+24;require(legacy.size()>metadata,"GX02 fixture too short");legacy.resize(legacy.size()-metadata);legacy[3]='1';
            require(load(legacy),"GX01 legacy prefix rejected under opt-in");reset_events();mark(b);write(command,split,command.size());write({0});ssne::gx_sync();
            require(events.size()==2&&!events[0].choice.marked&&events[1].choice==expected(b),"GX01 unknown prefix was silently reclassified");
            auto invalid=state;invalid.back()=0x80;require(!load(invalid,false),"Invalid context mask accepted");
            invalid=state;invalid.pop_back();require(!load(invalid,false),"Truncated context extension accepted");
        }
    }
    ssne::gx_clear_decode_context();write({0});ssne::gx_sync();
}
void stress_threaded(){
    // Byte-less scope churn stays a single fixed queue entry even before startup.
    for(unsigned i=0;i<100000;++i){mark(i&1?a:b);ssne::gx_clear_decode_context();}
    if(ssne::gx_scoped_context_enabled()){std::lock_guard lock(ssne::g_context_mutex);require(ssne::g_context_marks.size()<=1,"Zero-byte marker churn grew metadata queue");}
    collect=false;observed=0;
    // Cross raw-ring wrap with more than one full ring of bytes and distinct markers.
    const uint64_t bytes=ssne::kRingSize+128;
    for(uint64_t i=0;i<bytes;i+=4){mark(i&4?a:b);ssne::gx_fifo_write(0,4);}
    ssne::gx_sync();require(observed==bytes,"Threaded wrap lost or repeated commands");
    if(ssne::gx_scoped_context_enabled()){std::lock_guard lock(ssne::g_context_mutex);require(ssne::g_context_marks.high_water()<=ssne::kContextMarkerLimit,"Metadata queue exceeded bound");}
    // A long, incomplete XF payload cannot block marker consumption/backpressure.
    collect=true;reset_events();mark(a);write({0x10,0xff,0xff,0,0});
    for(unsigned i=0;i<65536;++i){mark(i&1?a:b);ssne::gx_fifo_write(0,4);}
    mark(b);write({0});ssne::gx_sync();
    require(events.size()==2&&events[0].opcode==0x10&&events[0].choice==expected(a)&&events[1].choice==expected(b),"Partial-command marker stress changed classification");
    require(ssne::g_decode_timeline.size()<=2,"Consumed timeline retained unbounded history");
    ssne::gx_clear_decode_context();write({0});ssne::gx_sync();
    require(!ssne::gx_decode_context_override(),"Worker TLS leaked to producer");
}
}
int main(int argc,char** argv){
    const bool threaded=argc>1&&std::string(argv[1])=="--threaded";int result=0;
    try{
        if(argc>1&&std::string(argv[1])=="--expect-default")require(ssne::gx_scoped_context_enabled(),"Scoped context process default is disabled");
        std::vector<uint8_t> ram(ssne::kRamSize);ssne::g_ram=ram.data();
        ssne::gx_set_threaded(threaded);ssne::g_decode_test_observer=observer;
        if(threaded)stress_threaded();else{live_state_roundtrip();split_commands();nested_lists();state_roundtrip();}
        require(!ssne::gx_decode_context_override(),"Decoder override escaped opcode lifetime");
        std::cout<<"GX actual FIFO/parser: "<<(ssne::gx_scoped_context_enabled()?"scoped":"legacy")<<' '<<(threaded?"threaded wrap/backpressure":"all splits/DL/state")<<" passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}
    if(ssne::g_gx_thread){ssne::stop_gx_thread();delete ssne::g_gx_thread;ssne::g_gx_thread=nullptr;}
    delete[] ssne::g_ring;ssne::g_ring=nullptr;ssne::g_ram=nullptr;return result;
}
