// EU8009B808 replacement: point/radius query against an entity's
// broad sphere, then its individual collision spheres. Descendant behavior,
// virtual adjustments, callback order and guest scratch memory are retained.
namespace native_sphere_query {
constexpr uint32_t address=0x8009B808;
PPCFunc original=nullptr;
bool range(uint32_t p,uint32_t bytes){return p>=0x80000000u && uint64_t(p)+bytes<=guest_ram_end();}
template<unsigned Site> void indirect(PPCContext& c,uint32_t target){
    static const bool trace=std::getenv("SSNE_COOP_TRACE_LIFETIMES")!=nullptr;
    if(trace){ssne_call(c,target);return;}
    if(target<kRamSize)target|=0x80000000u;
    struct Cached {uint32_t address=0;PPCFunc* slot=nullptr;PPCFunc function=nullptr;};
    static thread_local std::array<Cached,4> cached{};
    auto& hit=cached[(target>>2)&3];
    if(hit.address!=target || !hit.slot){
        const auto entry=ssne_find_func(target);
        if(!entry){ssne_call(c,target);return;}
        hit={target,ssne_hook_slot(target),entry->func};
    }
    if(*hit.slot)(*hit.slot)(c);else hit.function(c);
}
void distance(PPCContext& c,uint32_t caller){
    static const auto function=ssne_find_func(0x801BABB0)->func;
    c.lr=caller;function(c);
}
void body(PPCContext& c){
    const auto old_sp=c.r[1];c.r[1]-=0x48;mem_write32(c.r[1],old_sp);
    c.r[0]=c.lr;
    mem_write64(c.r[1]+0x38,d2u(c.ps0[30]));mem_write64(c.r[1]+0x40,d2u(c.ps0[31]));
    for(unsigned k=29;k<32;++k)mem_write32(c.r[1]+0x2C+(k-29)*4,c.r[k]);
    mem_write32(c.r[1]+0x4C,c.r[0]);
    c.r[31]=c.r[3];c.r[29]=c.r[4];c.r[9]=mem_read32(c.r[31]+0x48);c.ps0[30]=c.ps0[1];
    c.r[3]=uint32_t(int32_t(int16_t(mem_read16(c.r[9]+0x190))));c.r[0]=mem_read32(c.r[9]+0x194);
    c.r[3]+=c.r[31];c.lr=0x8009B844;indirect<0>(c,c.r[0]);
    c.ps0[1]=c.ps1[1]=round_single(c.ps0[30]+c.ps0[1]);c.r[3]=c.r[31]+0x20;
    c.ps0[31]=c.ps1[31]=round_single(c.ps0[1]*c.ps0[1]);c.r[4]=c.r[29];
    distance(c,0x8009B858);fcmp(c,0,c.ps0[1],c.ps0[31]);
    if(!cr_bit(c,0))c.r[3]=~0u;
    else {
        c.r[30]=0;
        for(;;){
            c.r[9]=mem_read32(c.r[31]+0x48);
            c.r[3]=uint32_t(int32_t(int16_t(mem_read16(c.r[9]+0x198))));c.r[0]=mem_read32(c.r[9]+0x19C);
            c.r[3]+=c.r[31];c.lr=0x8009B8DC;indirect<1>(c,c.r[0]);
            const auto a=int32_t(c.r[30]),b=int32_t(c.r[3]);cr_set_cmp(c,0,a<b,a>b,a==b);
            if(!cr_bit(c,0)){
                c.ps0[13]=c.ps1[13]=double(u2f(mem_read32(c.r[31]+0x20)));c.r[9]=c.r[31]+0x20;c.r[3]=0;
                mem_write32(c.r[29],f2u(float(c.ps0[13])));
                c.ps0[0]=c.ps1[0]=double(u2f(mem_read32(c.r[9]+4)));mem_write32(c.r[29]+4,f2u(float(c.ps0[0])));
                c.ps0[13]=c.ps1[13]=double(u2f(mem_read32(c.r[9]+8)));mem_write32(c.r[29]+8,f2u(float(c.ps0[13])));
                break;
            }
            if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);
            c.r[9]=mem_read32(c.r[31]+0x48);c.r[4]=c.r[30];c.r[0]=mem_read32(c.r[9]+0x1A4);
            c.r[3]=uint32_t(int32_t(int16_t(mem_read16(c.r[9]+0x1A0))));c.r[3]+=c.r[31];
            c.lr=0x8009B884;indirect<2>(c,c.r[0]);
            c.r[9]=mem_read32(c.r[31]+0x48);c.r[3]=c.r[1]+8;c.r[5]=c.r[30];
            c.ps0[1]=c.ps1[1]=round_single(c.ps0[30]+c.ps0[1]);
            c.r[4]=uint32_t(int32_t(int16_t(mem_read16(c.r[9]+0x1A8))));
            c.ps0[31]=c.ps1[31]=round_single(c.ps0[1]*c.ps0[1]);c.r[0]=mem_read32(c.r[9]+0x1AC);
            c.r[4]+=c.r[31];c.lr=0x8009B8AC;indirect<3>(c,c.r[0]);
            c.r[3]=c.r[29];c.r[4]=c.r[1]+8;distance(c,0x8009B8B8);fcmp(c,0,c.ps0[1],c.ps0[31]);
            if(cr_bit(c,0)){
                c.r[9]=mem_read32(c.r[31]+0x48);c.r[3]=c.r[1]+0x18;c.r[5]=c.r[30];
                c.r[4]=uint32_t(int32_t(int16_t(mem_read16(c.r[9]+0x1A8))));c.r[0]=mem_read32(c.r[9]+0x1AC);
                c.r[4]+=c.r[31];c.lr=0x8009B928;indirect<4>(c,c.r[0]);
                c.ps0[13]=c.ps1[13]=double(u2f(mem_read32(c.r[1]+0x18)));c.r[3]=c.r[30];
                c.ps0[12]=c.ps1[12]=double(u2f(mem_read32(c.r[1]+0x1C)));c.ps0[0]=c.ps1[0]=double(u2f(mem_read32(c.r[1]+0x20)));
                mem_write32(c.r[29],f2u(float(c.ps0[13])));mem_write32(c.r[29]+8,f2u(float(c.ps0[0])));mem_write32(c.r[29]+4,f2u(float(c.ps0[12])));
                break;
            }
            ++c.r[30];
        }
    }
    c.r[0]=mem_read32(c.r[1]+0x4C);c.lr=c.r[0];
    for(unsigned k=29;k<32;++k)c.r[k]=mem_read32(c.r[1]+0x2C+(k-29)*4);
    c.ps0[30]=u2d(mem_read64(c.r[1]+0x38));c.ps0[31]=u2d(mem_read64(c.r[1]+0x40));c.r[1]+=0x48;
}
void hook(PPCContext& c){
    if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);
    if(!range(c.r[1]-0x48,0x98)){ssne_call_original(c,address,original);return;}
    body(c);
}
void install(){
    static const bool enabled=[] {auto value=std::getenv("SSNE_NATIVE_SPHERE_QUERY");return !value || std::strcmp(value,"0");}();
    if(!enabled)return;
    const auto entry=ssne_find_func(address);if(!entry || mem_read32(0x80000000)!=0x47334250u || *ssne_hook_slot(address))return;
    uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<352;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}
    if(hash!=0x0449930CD1BD079Cull)return;
    original=entry->func;ssne_install_hook(address,hook);
}
}
