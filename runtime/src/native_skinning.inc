// Direct PC implementation of the PAL CPU skinning kernel (8012F968).
// The animator, host poses, mesh ownership, cache publication and renderer stay
// on their existing paths. Arithmetic follows decompiled/animation_skin.cpp;
// do not reassociate its single-precision rounding boundaries.
namespace native_skinning {
constexpr uint32_t address=0x8012F968u;
PPCFunc original=nullptr;
using Vec=std::array<float,3>;
using Matrix=std::array<float,12>;
struct Range {uint32_t begin=0,bytes=0;};
bool ram(uint32_t p,uint64_t bytes) {
    return p>=0x80000000u && !(p&3u) && bytes<=guest_ram_size() && uint64_t(p)+bytes<=guest_ram_end();
}
bool overlap(Range a,Range b) {return a.bytes && b.bytes && uint64_t(a.begin)<uint64_t(b.begin)+b.bytes && uint64_t(b.begin)<uint64_t(a.begin)+a.bytes;}
Range cleared(uint32_t p,uint32_t bytes){return {p&~31u,bytes?uint32_t((uint64_t(bytes)+(p&31u)+31u)&~31ull):0};}
float read(uint32_t p){return u2f(mem_read32(p));}
void write(uint32_t p,float v){mem_write32(p,f2u(v));}
Vec vector(uint32_t p){return {read(p),read(p+4),read(p+8)};}
void vector(uint32_t p,const Vec& v){for(unsigned k=0;k<3;++k)write(p+k*4,v[k]);}
float mul(float a,float b){return float(double(a)*b);}
float madd(float a,float b,float c){return float(double(a)*b+c);}
float add(float a,float b){return float(double(a)+b);}
void poll(PPCContext& ctx){SSNE_POLL();}

// Validate before the first write. Exotic overlapping buffers, non-F32 GQRs,
// zero-count groups and out-of-range indices retain the translated path.
bool supported(PPCContext& c) {
    if(c.gqr[0]!=0 || !ram(c.r[13]-0x4464,8) || read(c.r[13]-0x4464)!=.5f || read(c.r[13]-0x4460)!=3.f ||
       !ram(c.r[3],16) || !ram(c.r[4],8) || (c.r[5] && !ram(c.r[5],8)))return false;
    const auto data=mem_read32(c.r[3]+12);
    if(!ram(data,0x34))return false;
    const auto pos=mem_read32(c.r[4]),pbytes=mem_read32(c.r[4]+4);
    const auto norm=c.r[5]?mem_read32(c.r[5]):0,nbytes=c.r[5]?mem_read32(c.r[5]+4):0;
    if(pbytes>4*1024*1024 || nbytes>4*1024*1024 || !ram(pos,pbytes) || (c.r[5] && !ram(norm,nbytes)))return false;
    const auto scratch=mem_read32(0x802ABAD4u);
    uint32_t max_count=0,groups=0;
    for(auto bone=mem_read32(data+0x30);bone;bone=mem_read32(bone)) {
        if(++groups>256 || !ram(bone,0x34))return false;
        const auto n=mem_read32(bone+12),nn=c.r[5]?mem_read32(bone+40):0;
        if(!n || n>8192 || nn>8192)return false;
        max_count=std::max(max_count,std::max(n,nn));
        if(!ram(mem_read32(bone+16),uint64_t(n)*12) || !ram(mem_read32(bone+20),uint64_t(n)*4) || !ram(mem_read32(bone+24),uint64_t(n)*4))return false;
        if(nn && (!ram(mem_read32(bone+44),uint64_t(nn)*12) || !ram(mem_read32(bone+48),uint64_t(nn)*4)))return false;
        for(unsigned i=0;i<n;++i)if(uint64_t(mem_read32(mem_read32(bone+20)+i*4))*12+12>pbytes)return false;
        for(unsigned i=0;i<nn;++i)if(uint64_t(mem_read32(mem_read32(bone+48)+i*4))*12+12>nbytes)return false;
    }
    if(!ram(c.r[6],uint64_t(groups)*48) || (max_count && !ram(scratch,uint64_t(max_count)*12)))return false;
    const std::array<Range,3> outputs={cleared(pos,pbytes),cleared(norm,nbytes),Range{scratch,max_count*12}};
    for(unsigned i=0;i<3;++i){if(outputs[i].bytes && !ram(outputs[i].begin,outputs[i].bytes))return false;for(unsigned j=0;j<i;++j)if(overlap(outputs[i],outputs[j]))return false;}
    auto disjoint=[&](uint32_t p,uint32_t n){for(auto out:outputs)if(overlap(out,{p,n}))return false;return true;};
    if(!disjoint(c.r[3],16) || !disjoint(data,0x34) || !disjoint(c.r[4],8) || (c.r[5] && !disjoint(c.r[5],8)) || !disjoint(c.r[6],groups*48) || !disjoint(0x802ABAD4,4))return false;
    for(auto bone=mem_read32(data+0x30);bone;bone=mem_read32(bone)) {
        const auto n=mem_read32(bone+12),nn=c.r[5]?mem_read32(bone+40):0;
        if(!disjoint(bone,0x34) || !disjoint(mem_read32(bone+16),n*12) || !disjoint(mem_read32(bone+20),n*4) || !disjoint(mem_read32(bone+24),n*4) ||
           (nn && (!disjoint(mem_read32(bone+44),nn*12) || !disjoint(mem_read32(bone+48),nn*4))))return false;
    }
    return true;
}
Vec point(const Matrix& m,const Vec& v,bool single,bool fast) {
    Vec out;
    for(unsigned row=0;row<3;++row){const auto k=row*4;
        if(!single && (row<2 || fast))out[row]=madd(m[k+2],v[2],madd(m[k+1],v[1],madd(m[k],v[0],m[k+3])));
        else out[row]=add(madd(m[k+2],v[2],mul(m[k],v[0])),madd(m[k+3],1.f,mul(m[k+1],v[1])));
    }return out;
}
Vec normal(const Matrix& m,const Vec& v,bool single) {
    if(single){Vec out;for(unsigned row=0;row<3;++row){const auto k=row*4;out[row]=madd(m[k+2],v[2],add(mul(m[k],v[0]),mul(m[k+1],v[1])));}return out;}
    return {madd(m[2],v[2],madd(m[1],v[1],mul(m[0],v[0]))),madd(m[6],v[2],madd(m[5],v[1],mul(m[4],v[0]))),add(madd(m[10],v[2],mul(m[8],v[0])),mul(m[9],v[1]))};
}
void transform(PPCContext& c,uint32_t matrix,uint32_t input,uint32_t output,uint32_t count,bool normals) {
    Matrix m;for(unsigned k=0;k<12;++k)m[k]=read(matrix+k*4);
    const bool fast=!normals && count>15;const uint32_t step=fast?2:1,groups=(count+step-1)/step;
    std::array<Vec,2> current{},next{};
    auto calculate=[&](unsigned base,auto& target){for(unsigned k=0;k<step && base+k<count;++k){auto v=vector(input+(base+k)*12);target[k]=normals?normal(m,v,count==1):point(m,v,count==1,fast);}};
    calculate(0,current);
    for(unsigned g=0;g<groups;++g){
        if(g+1<groups)calculate((g+1)*step,next);
        for(unsigned k=0;k<step && g*step+k<count;++k)vector(output+(g*step+k)*12,current[k]);
        if(g+2<groups)poll(c);current=next;
    }
}
void normalize(Vec& v) {
    const float squared=add(madd(v[2],v[2],mul(v[0],v[0])),mul(v[1],v[1]));
    const double estimate=1.0/std::sqrt(double(squared));
    const float estimate_squared=float(estimate*estimate),half=float(estimate*.5);
    const float inverse=mul(float(-(double(estimate_squared)*squared-3.0)),half);
    for(auto& f:v)f=mul(f,inverse);
}
void kernel(PPCContext& c) {
    // Preserve original cache-line clearing and its interrupt safe points.
    // No persistent decoded pointers or geometry cache can outlive resources.
    const auto resource=c.r[3],pd=c.r[4],nd=c.r[5],matrices=c.r[6];
    const auto pos=mem_read32(pd),norm=nd?mem_read32(nd):0;
    auto clear=[&](uint32_t p,uint32_t bytes){auto call=c;call.r[3]=p;call.r[4]=bytes;ssne_call(call,0x801B54C8);};
    clear(pos,mem_read32(pd+4));if(nd)clear(norm,mem_read32(nd+4));
    const auto scratch=mem_read32(0x802ABAD4u);auto matrix=matrices;
    for(auto bone=mem_read32(mem_read32(resource+12)+0x30);bone;matrix+=48){
        const auto count=mem_read32(bone+12),indices=mem_read32(bone+20),weights=mem_read32(bone+24);
        transform(c,matrix,mem_read32(bone+16),scratch,count,false);
        for(unsigned i=0;i<count;++i){
            const auto target=pos+mem_read32(indices+i*4)*12;const auto in=vector(scratch+i*12);auto out=vector(target);const auto weight=read(weights+i*4);
            for(unsigned k=0;k<3;++k)out[k]=add(out[k],mul(in[k],weight));vector(target,out);if(i+1<count)poll(c);
        }
        const auto n=nd?mem_read32(bone+40):0;
        if(n){const auto ni=mem_read32(bone+48);transform(c,matrix,mem_read32(bone+44),scratch,n,true);
            for(unsigned i=0;i<n;++i){auto out=vector(scratch+i*12);normalize(out);vector(norm+mem_read32(ni+i*4)*12,out);if(i+1<n)poll(c);}
        }
        bone=mem_read32(bone);if(bone)poll(c);
    }
    c.r[3]=0;
}
void hook(PPCContext& c){
    perf::Scope timer("cpu_skinning");
    // This setting is for reproducible developer A/B runs, not a save option.
    static const bool enabled=[] {auto v=std::getenv("SSNE_NATIVE_SKINNING");return !v || std::strcmp(v,"0");}();
    poll(c);
    if(!enabled || !supported(c)){ssne_call_original(c,address,original);return;}
    kernel(c);
}
void install(){
    const auto* entry=ssne_find_func(address);
    if(mem_read32(0x80000000)!=0x47334250u || !entry)return;
    constexpr std::array<uint32_t,6> signature={0x9421FF50u,0x7C0802A6u,0x7D800026u,0xBE61007Cu,0x900100B4u,0x91810078u};
    for(unsigned i=0;i<signature.size();++i)if(mem_read32(address+i*4)!=signature[i])return;
    original=entry->func;ssne_install_hook(address,hook);
    log_message("[ssne] native PC CPU skinning installed (PAL 8012F968)");
}
}
