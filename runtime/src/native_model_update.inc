#include "native_animation_lifetime.inc"
// PAL801286C0: derive the PC adapter from the decomp.wtf research draft and
// verify every branch against the original instructions. The draft still has
// one compiler register-allocation difference (score5), not a byte-matched C
// claim. Preserve its model-cache search, frame throttling and SDK boundaries.
namespace native_model_update {
bool available=false;
void install(){
    native_animation_lifetime::install();
    if(mem_read32(0x80000000)!=0x47334250u || !ssne_find_func(0x801286C0))return;
    uint64_t h=14695981039346656037ull;
    for(unsigned i=0;i<372;++i){h^=mem_read8(0x801286C0+i);h*=1099511628211ull;}
    if(h!=0x4ACD7FF3F282C009ull)return;
    available=true;log_message("[ssne] native PAL model cache/update adapter installed (801286C0)");
}
void body(PPCContext& c,PPCFunc original){
    using namespace native_render_paths;
    static const bool enabled=[] {const auto* s=std::getenv("SSNE_NATIVE_MODEL_UPDATE");return !s || std::strcmp(s,"0");}();
    poll(c);const auto model=c.r[3],argument=c.r[4];
    // Read the current RAM limit once, including after state restoration.
    // Only the verified PAL SDA layout is adapted; unusual callers fall back.
    const auto end=guest_ram_end();
    if(!available || !enabled || c.r[13]!=0x802E22A0u || (c.r[1]&7) || c.r[1]<0x80000100u ||
       uint64_t(c.r[1])+0x100>end || model<0x80000000u || uint64_t(model)+0xB0>end){
        ssne_call_original(c,0x801286C0,original);return;
    }
    Frame frame(c,0x10,31);c.r[31]=model;
    const auto view=mem_read32(c.r[13]-0x6D94);
    if(view<=3){const auto p=model+0x9C+view*4;
        number(p,float(double(float(double(number(0x8021D550))-number(p)))*number(0x8021D554)));
    }
    if(c.ps0[1]<double(number(model+0x2C)))number(model+0x2C,float(c.ps0[1]));
    mem_write32(model+0x34,0);
    if(!mem_read32(model+0x14)){
        invoke<0x801288D0>(c,0x8012873C,{model,1,argument});
        if(!mem_read32(model+0x14)){
            const auto resource_list=mem_read32(model+8);
            auto node=resource_list?mem_read32(resource_list+0x14):0;
            const auto key=node?mem_read32(model+12):0;
            while(node){
                if(mem_read32(node+12)==key){
                    const auto pair=mem_read32(node+0x14);
                    if(pair && mem_read32(node+0x40)==mem_read32(model+0x40)){c.r[3]=pair;return;}
                }
                node=mem_read32(node+4);if(node)poll(c);
            }
            c.r[3]=0;return;
        }
    }
    auto stamp=mem_read32(c.r[13]-0x6D80);
    const auto delta=int32_t(stamp-mem_read32(model+0x84));
    bool update=delta>0;
    if(update && delta<=4){
        const auto sum=mem_read32(model+0x58)+stamp;
        const auto a=int32_t(sum),b=int32_t(mem_read32(model+0x60));
        const auto q=(b==0 || (a==INT32_MIN && b==-1))?(a<0?~0u:0u):uint32_t(a/b);
        update=sum==q*uint32_t(b);
    }
    if(update){
        if(mem_read32(model+0x80)!=stamp){
            const auto result=invoke<0x80129154>(c,0x801287EC,{model});
            if(int32_t(result)<0){c.r[3]=mem_read32(model+0x14);return;}
            stamp=mem_read32(c.r[13]-0x6D80);
            if(mem_read32(model+0x80)!=stamp){c.r[3]=mem_read32(model+0x14);return;}
        }
        if(mem_read32(model+0x84)!=stamp)invoke<0x80129958>(c,0x8012881C,{model});
    }
    c.r[3]=mem_read32(model+0x14);
}
}
void native_model_update_dispatch(PPCContext& c,PPCFunc original){native_model_update::body(c,original);}
