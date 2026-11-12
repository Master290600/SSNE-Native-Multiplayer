// PC adapters for five byte-matched EU decomp.wtf functions. No host pointer
// is inferred from a guest pointer. GPU commands still enter the existing GX
// backend through the same SDK boundaries; native widescreen hooks stay active.
namespace native_render_paths {
bool available=false;
void poll(PPCContext& c){if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);}
bool enabled(){static const bool value=[] {auto s=std::getenv("SSNE_NATIVE_RENDER_PATHS");return !s || std::strcmp(s,"0");}();return value;}
struct Timer {
    unsigned stage=unsigned(perf::names.size());perf::Clock::time_point started{};
    explicit Timer(unsigned index){
        static thread_local std::array<uint64_t,5> sequences{};
        // Resolve by name: appending diagnostic counters must not relabel
        // these sampled native calls as unrelated presentation events.
        static constexpr std::array stages={"native_draw_mode","native_material_setup",
            "native_visibility_setup","native_projection_setup","native_skinning_dispatch"};
        if(perf::enabled.load(std::memory_order_relaxed) && (++sequences[index]&15)==1){
            stage=perf::index(stages[index]);started=perf::Clock::now();
        }
    }
    ~Timer(){if(stage<perf::names.size())perf::record(stage,perf::Clock::now()-started);}
};
bool range(uint32_t p,uint64_t n){return p>=0x80000000u && n<=guest_ram_size() && uint64_t(p)+n<=guest_ram_end();}
bool stack_ok(const PPCContext& c){return range(c.r[1]-0x100,0x200) && !(c.r[1]&7) && range(c.r[13]-0x7000,0x2000);}
float number(uint32_t p){return u2f(mem_read32(p));}
void number(uint32_t p,float v){mem_write32(p,f2u(v));}
struct Frame {
    PPCContext& c;uint32_t sp,size;unsigned first;
    Frame(PPCContext& c,uint32_t size,unsigned first=32):c(c),sp(c.r[1]-size),size(size),first(first){
        mem_write32(sp,c.r[1]);mem_write32(sp+size+4,c.lr);c.r[1]=sp;
        for(unsigned k=first;k<32;++k)mem_write32(sp+size-(32-k)*4,c.r[k]);
    }
    ~Frame(){for(unsigned k=first;k<32;++k)c.r[k]=mem_read32(sp+size-(32-k)*4);c.lr=mem_read32(sp+size+4);c.r[1]=sp+size;}
};
uint32_t invoke(PPCContext& c,uint32_t address,uint32_t caller,std::initializer_list<uint32_t> args={}){
    unsigned reg=3;for(auto v:args)c.r[reg++]=v;c.lr=caller;ssne_call(c,address);return c.r[3];
}
template<uint32_t Address> uint32_t invoke(PPCContext& c,uint32_t caller,std::initializer_list<uint32_t> args={}){
    // Like generated direct calls: resolve once, then enter the function's
    // normal stable hook slot. Avoid ssne_call's global mutex/map lookup for
    // fixed SDK targets in the hot path. Dynamic virtual calls stay dynamic.
    static const auto function=ssne_find_func(Address)->func;
    unsigned reg=3;for(auto v:args)c.r[reg++]=v;c.lr=caller;function(c);return c.r[3];
}
PPCFunc mode_original=nullptr,material_original=nullptr,visibility_original=nullptr,projection_original=nullptr;
void mode_body(PPCContext&);
void material_body(PPCContext&);
void mode(PPCContext& c){Timer timer(0);poll(c);if(!enabled() || !stack_ok(c)){ssne_call_original(c,0x80148048,mode_original);return;}mode_body(c);}
void material(PPCContext& c){Timer timer(1);poll(c);if(!enabled() || !stack_ok(c) || mem_read32(c.r[13]-0x5C84)>15){ssne_call_original(c,0x801233B0,material_original);return;}material_body(c);}
void visibility(PPCContext& c){
    Timer timer(2);poll(c);
    if(!enabled() || !stack_ok(c) || (c.r[3] && !range(c.r[3],16))){ssne_call_original(c,0x801493D0,visibility_original);return;}
    const auto scene=c.r[3];Frame frame(c,0x50,26);
    const auto count=mem_read32(c.r[13]-0x6B74);
    mem_write32(c.r[13]-0x5C18,0);mem_write32(c.r[13]-0x5C1C,0);mem_write32(c.r[13]-0x6B74,count+1);
    if(!scene){mem_write32(c.r[13]-0x6B78,0);mem_write32(c.r[13]-0x6B80,0);c.r[3]=~0u;return;}
    const auto region=invoke<0x8013FC38>(c,0x80149420,{scene,0x80292214});
    if(!region){mem_write32(c.r[13]-0x6B78,0);mem_write32(c.r[13]-0x6B70,~0u);mem_write32(c.r[13]-0x6B80,0);c.r[3]=~0u;return;}
    const auto tag=mem_read32(region+4);if(tag!=mem_read32(c.r[13]-0x6B70))mem_write32(c.r[13]-0x6B70,tag);
    mem_write32(c.r[13]-0x6B80,region);mem_write32(c.r[13]-0x6B7C,scene);
    const auto state=invoke<0x8013FC6C>(c,0x80149464,{scene});mem_write32(c.r[13]-0x6B78,state);
    invoke<0x801DADAC>(c,0x80149474,{0x80291F64});invoke<0x801DB0C0>(c,0x80149480,{0x80291F80});
    // Keep the original reload/poll boundaries: SDK callbacks and interrupts
    // may change the current region or counts. Do not cache object lifetimes.
    uint32_t current=mem_read32(c.r[13]-0x6B78),i=0;
    if(i<mem_read32(current+4))for(;;){mem_write8(mem_read32(current+8)+i++,0);current=mem_read32(c.r[13]-0x6B78);if(i>=mem_read32(current+4))break;poll(c);}
    current=mem_read32(c.r[13]-0x6B78);i=0;
    if(i<mem_read32(current+12))for(;;){mem_write16(mem_read32(current+16)+i*2,0);++i;if(i>=mem_read32(current+12))break;poll(c);}
    // Original conversion bias is data, not a guessed host constant.
    const auto bias=u2d(mem_read64(0x8021F050));std::array<float,4> rect;
    for(unsigned k=0;k<4;++k){const auto n=mem_read32(c.r[13]-0x5C00+k*4)^0x80000000u;rect[k]=float(u2d(0x4330000000000000ull|n)-bias);}
    number(frame.sp+8,rect[0]);number(frame.sp+12,rect[1]);number(frame.sp+16,float(double(rect[2])+rect[0]));number(frame.sp+20,float(double(rect[3])+rect[1]));
    invoke<0x80149120>(c,0x801495DC,{region,frame.sp+8,frame.sp+16,0});c.r[3]=0;
}
void projection(PPCContext& c){
    Timer timer(3);poll(c);const auto object=c.r[3];
    if(!enabled() || !stack_ok(c) || !range(object,4) || !range(mem_read32(object),0x38)){ssne_call_original(c,0x8014CC6C,projection_original);return;}
    Frame frame(c,0x10);const auto table=mem_read32(object),method=mem_read32(table+0x34);const auto adjustment=int32_t(int16_t(mem_read16(table+0x30)));
    invoke(c,method,0x8014CC90,{object+uint32_t(adjustment)});double value=c.ps0[1];
    const auto width=int32_t(mem_read32(c.r[13]-0x5BF8)),height=int32_t(mem_read32(c.r[13]-0x5BF4));
    const auto bias=u2d(mem_read64(0x8021F3C0));
    auto converted=[&](int32_t v){return float(u2d(0x4330000000000000ull|(uint32_t(v)^0x80000000u))-bias);};
    const float fw=converted(width),fh=converted(height);
    if(width>height)value=round_single(value*round_single(double(fh)/fw));
    mem_write32(c.r[13]-0x6B34,1);
    if(value<number(0x8021F3C8))value=number(0x8021F3C8);if(value>number(0x8021F3CC))value=number(0x8021F3CC);
    const double aspect=round_single(double(fw)/fh),near_z=round_single(double(number(c.r[13]-0x6B64))+number(0x8021F3D0));
    const auto far_z=number(c.r[13]-0x6B5C);const auto mode=mem_read32(c.r[13]-0x6B68);
    c.ps0[3]=c.ps1[3]=near_z;c.ps0[4]=c.ps1[4]=far_z;
    if(mode<=2){
        c.ps0[1]=value;c.ps0[2]=aspect;
        const uint32_t callers[]={0x8014CDA8,0x8014CDC8,0x8014CDE8};
        if(mode==1)c.ps0[2]=c.ps1[2]=round_single(aspect*number(0x8021F3D4));
        if(mode==2)c.ps0[1]=c.ps1[1]=round_single(value/number(0x8021F3D8));
        // These precise return addresses identify weapon/gameplay and camera
        // projections in our existing FOV/widescreen implementation.
        invoke<0x801BA814>(c,callers[mode],{0x802922E0});
    }
    invoke<0x801DAC38>(c,0x8014CDF8,{0x802922E0,0});
    for(unsigned k=0;k<4;++k)c.ps0[1+k]=converted(int32_t(mem_read32(c.r[13]-0x5C00+k*4)));
    c.ps0[5]=c.ps1[5]=number(0x8021F3DC);c.ps0[6]=c.ps1[6]=number(0x8021F3E0);
    invoke<0x801DB09C>(c,0x8014CE94);
}
void skin_body(PPCContext& c,PPCFunc translated){
    Timer timer(4);poll(c);const auto object=c.r[3];
    if(!available || !enabled() || !stack_ok(c) || !range(object,0x88) || !range(mem_read32(object+0x14),8)){
        ssne_call_original(c,0x80129958,translated);return;
    }
    Frame frame(c,0x10,30);const auto stamp=mem_read32(c.r[13]-0x6D80);
    mem_write32(object+0x54,mem_read32(object+0x44));mem_write32(object+0x50,mem_read32(object+0x40));
    // lfs/stfs quiets a signaling NaN just as the translated outer function.
    auto blend=mem_read32(object+0x48);if((blend&0x7F800000u)==0x7F800000u && (blend&0x7FFFFFu))blend|=0x400000u;
    mem_write32(object+0x5C,blend);mem_write32(object+0x84,stamp);
    const auto pair=mem_read32(object+0x14);const auto result=invoke<0x8012F968>(c,0x801299A4,{mem_read32(object+12),mem_read32(pair),mem_read32(pair+4),mem_read32(object+0x78)});
    // Re-read descriptors after each callback, retaining original ordering.
    auto descriptor=mem_read32(mem_read32(object+0x14));invoke<0x801B543C>(c,0x801299BC,{mem_read32(descriptor),mem_read32(descriptor+4)});
    descriptor=mem_read32(mem_read32(object+0x14)+4);if(descriptor)invoke<0x801B543C>(c,0x801299D8,{mem_read32(descriptor),mem_read32(descriptor+4)});
    c.r[3]=result;
}
#include "native_render_modes.inc"
void install();
}
void native_skinning_dispatch(PPCContext& c,PPCFunc translated){native_render_paths::skin_body(c,translated);}
