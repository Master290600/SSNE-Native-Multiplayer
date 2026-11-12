// EU80129154. Native animation key decoding, pose selection and bone blending.
// Derived from animation_pose.cpp research, checked against the EU instructions.
namespace native_animation_pose {
PPCFunc original=nullptr;
bool available=false;
struct Frame {
    PPCContext& c;uint32_t sp;
    explicit Frame(PPCContext& c):c(c),sp(c.r[1]-0xC0){
        mem_write32(sp,c.r[1]);mem_write32(sp+0xC4,c.lr);
        mem_write32(sp+0x7C,cr_get(c));
        for(unsigned k=20;k<32;++k)mem_write32(sp+0x80+(k-20)*4,c.r[k]);
        mem_write64(sp+0xB0,d2u(c.ps0[30]));mem_write64(sp+0xB8,d2u(c.ps0[31]));c.r[1]=sp;
    }
    ~Frame(){
        for(unsigned k=20;k<32;++k)c.r[k]=mem_read32(sp+0x80+(k-20)*4);
        c.ps0[30]=u2d(mem_read64(sp+0xB0));c.ps0[31]=u2d(mem_read64(sp+0xB8));
        cr_put(c,mem_read32(sp+0x7C),0x08);c.lr=mem_read32(sp+0xC4);c.r[1]=sp+0xC0;
    }
};
void body(PPCContext& c){
    using namespace native_render_paths;
    Frame frame(c);const auto object=c.r[3];c.r[31]=object;
    const auto finish=[&]{if(mem_read32(object+0x90))invoke<0x80128CAC>(c,0x80129930,{object});c.r[3]=0;};
    if(const auto shared=mem_read32(object+0x94)){
        const auto result=invoke<0x80129154>(c,0x80129188,{shared});
        if(result){c.r[3]=result;return;}
        auto source=mem_read32(object+0x94);
        if(mem_read32(object+0x80)!=mem_read32(source+0x80)){
            for(int32_t i=0;i<int32_t(mem_read32(object+0x7C));++i){
                const auto mapped=mem_read32(mem_read32(object+0x98)+uint32_t(i)*4);
                if(mapped!=~0u){
                    source=mem_read32(object+0x94);
                    invoke<0x801B9E78>(c,0x801291E8,{mem_read32(source+0x78)+mapped*48,mem_read32(object+0x78)+uint32_t(i)*48});
                }
                source=mem_read32(object+0x94);
                if(i+1<int32_t(mem_read32(object+0x7C)))poll(c);
            }
            mem_write32(object+0x80,mem_read32(source+0x80));
        }
        finish();return;
    }
    const auto packed=mem_read32(mem_read32(mem_read32(object+12)+12)+0x44);
    if(mem_read32(object+0x38)==mem_read32(object+0x40) &&
       mem_read32(object+0x3C)==mem_read32(object+0x44) && number(object+0x4C)==number(object+0x48)){
        c.r[3]=uint32_t(-10);return;
    }
    if(const auto overlay=mem_read32(object+0x90))invoke<0x80129154>(c,0x8012926C,{mem_read32(overlay)});
    mem_write32(object+0x80,mem_read32(c.r[13]-0x6D80));
    const auto decode=[&](uint32_t indices,uint32_t matrices,uint32_t pointers,uint32_t caller){
        const auto basis=number(0x8021D564),cross_scale=number(0x8021D568);
        c.ps0[31]=c.ps1[31]=basis;c.ps0[30]=c.ps1[30]=cross_scale;
        for(int32_t i=0;i<int32_t(mem_read32(object+0x7C));++i){
            const auto index=mem_read16(indices);indices+=2;
            const auto keys=mem_read32(packed);
            invoke<0x801B9E4C>(c,caller,{matrices});
            const auto key=keys+uint32_t(index)*20;
            const auto component=[&](unsigned offset){
                const auto value=mem_read16(key+offset);mem_write16(frame.sp+0x70,value);
                if(c.gqr[5]==0x00070007u)return double(int16_t(value));
                psq_load(c,0,frame.sp+0x70,1,5);return c.ps0[0];
            };
            // Match the original late scale reads and the rounding after each
            // single-precision instruction; packed keys themselves stay uncached.
            double scale=number(packed+8);number(matrices+12,float(component(14)*scale));
            scale=number(packed+12);number(matrices+28,float(component(16)*scale));
            scale=number(packed+16);number(matrices+44,float(component(18)*scale));
            const float x=float(component(0)*basis);number(matrices,x);
            const float y=float(component(2)*basis);number(matrices+16,y);
            const float z=float(component(4)*basis);number(matrices+32,z);
            const float u=float(component(6)*basis);number(matrices+4,u);
            const float yu=float(double(y)*u);
            const float v=float(component(8)*basis);number(matrices+20,v);
            const float zv=float(double(z)*v);
            const float cross_z=float(double(x)*v-yu);
            const float w=float(component(10)*basis);number(matrices+36,w);
            const float xw=float(double(x)*w);
            const float cross_x=float(double(y)*w-zv),cross_y=float(double(z)*u-xw);
            const float factor=float(component(12)*cross_scale);
            number(matrices+40,float(double(cross_z)*factor));
            number(matrices+8,float(double(cross_x)*factor));
            number(matrices+24,float(double(cross_y)*factor));
            mem_write32(pointers,matrices);pointers+=4;matrices+=48;
            if(i+1<int32_t(mem_read32(object+0x7C)))poll(c);
        }
    };
    uint32_t stream_b=0,stream_a=0;
    if(packed){
        if(mem_read32(object+0x40)!=~0u){
            const auto indices=invoke<0x80128C50>(c,0x80129294,{object,mem_read32(object+0x40),mem_read32(object+0x44)});
            if(indices){stream_b=0x802907F0;if(int32_t(mem_read32(object+0x7C))>0)decode(indices,0x8028EFF0,stream_b,0x801292EC);}
        }
        if(mem_read32(object+0x38)!=~0u){
            const auto indices=invoke<0x80128C50>(c,0x80129448,{object,mem_read32(object+0x38),mem_read32(object+0x3C)});
            if(indices){stream_a=0x802908F0;if(int32_t(mem_read32(object+0x7C))>0)decode(indices,0x8028FBF0,stream_a,0x801294A0);}
        }
    }else{
        if(mem_read32(object+0x40)!=~0u)stream_b=invoke<0x80128BF4>(c,0x801295FC,{object,mem_read32(object+0x40),mem_read32(object+0x44)});
        if(mem_read32(object+0x38)!=~0u)stream_a=invoke<0x80128BF4>(c,0x8012961C,{object,mem_read32(object+0x38),mem_read32(object+0x3C)});
    }
    if(!stream_b&&!stream_a){c.r[3]=~0u;return;}
    uint32_t selected=0,caller=0;bool copying=false;
    if(!stream_a){selected=stream_b;caller=0x8012966C;copying=true;}
    else{
        const auto blend=number(object+0x48);
        if(blend>number(0x8021D56C)){selected=stream_b;caller=0x801296C8;copying=true;}
        else if(mem_read32(object+0x40)==mem_read32(object+0x38) && mem_read32(object+0x44)==mem_read32(object+0x3C)){selected=stream_b;caller=0x8012972C;copying=true;}
        else if(blend<number(0x8021D570)){selected=stream_a;caller=0x80129780;copying=true;}
    }
    for(int32_t i=0;i<int32_t(mem_read32(object+0x7C));++i){
        if(copying){
            invoke<0x801B9E78>(c,caller,{mem_read32(selected+uint32_t(i)*4),mem_read32(object+0x78)+uint32_t(i)*48});
        }else{
            const auto b=mem_read32(stream_b+uint32_t(i)*4),a=mem_read32(stream_a+uint32_t(i)*4);
            const auto blend=number(object+0x48);
            for(unsigned k=0;k<12;++k){
                const double vb=number(b+k*4),va=number(a+k*4);
                const float delta=float(vb-va);number(frame.sp+0x38+k*4,float(double(delta)*blend+va));
            }
            for(unsigned block=0;block<2;++block){
                for(unsigned k=0;k<6;++k)mem_write32(frame.sp+8+block*24+k*4,mem_read32(frame.sp+0x38+block*24+k*4));
                if(!block)poll(c);
            }
            invoke<0x801B9E78>(c,0x8012990C,{frame.sp+8,mem_read32(object+0x78)+uint32_t(i)*48});
        }
        if(i+1<int32_t(mem_read32(object+0x7C)))poll(c);
    }
    finish();
}
void draw(PPCContext& c){
    using namespace native_render_paths;
    static const bool enabled=[] {auto s=std::getenv("SSNE_NATIVE_ANIMATION_POSE");return !s || std::strcmp(s,"0");}();
    poll(c);
    if(!enabled || c.r[13]!=0x802E22A0u || !stack_ok(c) || !range(c.r[3],0xA0)){
        ssne_call_original(c,0x80129154,original);return;
    }
    body(c);
}
void install(){
    if(mem_read32(0x80000000)!=0x47334250u || !ssne_find_func(0x80129154))return;
    uint64_t hash=14695981039346656037ull;
    for(unsigned i=0;i<2052;++i){hash^=mem_read8(0x80129154+i);hash*=1099511628211ull;}
    if(hash!=0xC63B80F3F7A871F1ull)return;
    // The existing replica bones hook owns this entry point and must first
    // restore authoritative enemy/weapon poses. Replace only its original-code
    // branch; never overwrite that functional hook with this CPU kernel.
    original=ssne_find_func(0x80129154)->func;available=true;
    log_message("[ssne] native EU animation key decoding/pose blending installed (80129154)");
}
}
bool native_animation_pose_available(){return native_animation_pose::available;}
void native_animation_pose_dispatch(PPCContext& c,PPCFunc translated){
    if(!native_animation_pose::available){ssne_call_original(c,0x80129154,translated);return;}
    native_animation_pose::draw(c);
}
