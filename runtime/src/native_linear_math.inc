// Verified EU leaf math. The return values, instruction rounding, paired lanes
// and overlap order follow the original functions, not the generated labels.
// In particular801BAA80 computes a three-component vector dot product.
namespace native_linear_math {
constexpr std::array<uint32_t,5> addresses={0x801B9E4C,0x801B9E78,0x801B9EAC,0x801BAA80,0x801BAAA0};
constexpr std::array<unsigned,5> sizes={44,52,204,32,60};
constexpr std::array<uint64_t,5> fingerprints={0x5B76D5845EE9197Cull,0x077D9E23E845819Bull,0x9CD7260176B1D543ull,0xD3D5048A15A88E46ull,0xBCA37A343B54F3A7ull};
std::array<PPCFunc,5> originals{};
bool ram(uint32_t p,unsigned bytes){return uint32_t(p-0x80000000u)<=kRamSize-bytes;}
uint8_t* pointer(uint32_t p){return g_ram+uint32_t(p-0x80000000u);}
bool finite(uint32_t p,unsigned components){
    for(unsigned k=0;k<components;++k){uint32_t bits;std::memcpy(&bits,pointer(p)+k*4,4);if((SSNE_BSWAP32(bits)&0x7F800000u)==0x7F800000u)return false;}
    return true;
}
double load(uint32_t p){uint32_t bits;std::memcpy(&bits,pointer(p),4);return double(u2f(SSNE_BSWAP32(bits)));}
void store(uint32_t p,double value){
    auto bits=f2u(float(value));
    // Prevent folding an input float->double->float back to an unquieted sNaN.
    if((bits&0x7F800000u)==0x7F800000u&&(bits&0x7FFFFFu))bits|=0x400000u;
    bits=SSNE_BSWAP32(bits);std::memcpy(pointer(p),&bits,4);
}
void pair(PPCContext& c,unsigned r,double x,double y){c.ps0[r]=x;c.ps1[r]=y;}
void identity(PPCContext& c){
    const auto dst=c.r[3];const double zero=load(c.r[13]-0x451C),one=load(c.r[13]-0x4520);
    pair(c,0,zero,zero);pair(c,1,one,one);
    store(dst+8,zero);store(dst+12,zero);
    pair(c,2,round_single(zero),round_single(one));
    store(dst+24,zero);store(dst+28,zero);
    pair(c,1,round_single(one),round_single(zero));
    store(dst+32,zero);store(dst+36,zero);
    store(dst+16,c.ps0[2]);store(dst+20,c.ps1[2]);
    store(dst,c.ps0[1]);store(dst+4,c.ps1[1]);
    store(dst+40,c.ps0[1]);store(dst+44,c.ps1[1]);
}
void copy(PPCContext& c){
    // Each pair is loaded before that pair is stored. A partially overlapping
    // destination must see the same forward propagation as the original.
    for(unsigned r=0;r<6;++r){const auto offset=r*8;const double a=load(c.r[3]+offset),b=load(c.r[3]+offset+4);pair(c,r,a,b);store(c.r[4]+offset,a);store(c.r[4]+offset+4,b);}
}
void dot3(PPCContext& c){
    const double ay=load(c.r[3]+4),az=load(c.r[3]+8),by=load(c.r[4]+4),bz=load(c.r[4]+8);
    const double yy=round_single(ay*by),zz=round_single(az*bz);pair(c,2,yy,zz);
    const double ax=load(c.r[3]),ay2=load(c.r[3]+4),bx=load(c.r[4]),by2=load(c.r[4]+4);
    pair(c,5,ax,ay2);pair(c,4,bx,by2);
    const double xy=round_single(ax*bx+yy),yz=round_single(ay2*by2+zz);pair(c,3,xy,yz);
    pair(c,1,round_single(xy+zz),round_single(zz));
}
void cross3(PPCContext& c){
    const double bx=load(c.r[4]),by=load(c.r[4]+4),az=load(c.r[3]+8),ax=load(c.r[3]),ay=load(c.r[3]+4),bz=load(c.r[4]+8);
    pair(c,1,bx,by);pair(c,2,az,az);pair(c,0,ax,ay);pair(c,6,round_single(by),round_single(bx));pair(c,3,bz,bz);
    const double xz=round_single(bx*az),yz=round_single(by*az);pair(c,4,xz,yz);
    const double xx=round_single(bx*ax),yx=round_single(by*ax);pair(c,7,xx,yx);
    const double first=round_single(ax*bz-xz),second=round_single(ay*bz-yz);pair(c,5,first,second);
    const double third=round_single(ax*c.ps0[6]-xx),fourth=round_single(ay*c.ps1[6]-yx);pair(c,8,third,fourth);
    pair(c,9,round_single(second),round_single(second));pair(c,10,round_single(first),round_single(fourth));
    store(c.r[5],c.ps0[9]);pair(c,10,round_single(-c.ps0[10]),round_single(-c.ps1[10]));
    store(c.r[5]+4,c.ps0[10]);store(c.r[5]+8,c.ps1[10]);
}
void concatenate(PPCContext& c){
    const auto sp=c.r[1]-64,dst=c.r[5];
    mem_write32(sp,c.r[1]);mem_write64(sp+8,d2u(c.ps0[14]));mem_write64(sp+16,d2u(c.ps0[15]));mem_write64(sp+40,d2u(c.ps0[31]));
    std::array<double,12> a,b;std::array<double,2> tail;
    for(unsigned i=0;i<12;++i){a[i]=load(c.r[3]+i*4);b[i]=load(c.r[4]+i*4);}
    tail[0]=load(0x802DBF10);tail[1]=load(0x802DBF14);std::array<double,12> result;
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col){
        double value=round_single(b[col]*a[row*4]);
        value=round_single(b[4+col]*a[row*4+1]+value);
        value=round_single(b[8+col]*a[row*4+2]+value);
        if(col>=2)value=round_single(tail[col-2]*a[row*4+3]+value);
        result[row*4+col]=value;
    }
    for(unsigned offset:{0u,16u,8u,24u,32u,40u}){store(dst+offset,result[offset/4]);store(dst+offset+4,result[offset/4+1]);}
    // Preserve even the original paired-register byproducts; the scalar halves
    // of14,15,31 remain unchanged, matching their guest stack save/restore.
    pair(c,0,result[10],result[11]);pair(c,2,result[8],result[9]);
    pair(c,1,a[2],a[3]);pair(c,3,a[6],a[7]);pair(c,4,a[8],a[9]);pair(c,5,a[10],a[11]);
    for(unsigned r=6;r<=11;++r)pair(c,r,b[(r-6)*2],b[(r-6)*2+1]);
    pair(c,12,result[0],result[1]);pair(c,13,result[2],result[3]);
    c.ps1[14]=result[5];c.ps1[15]=result[7];c.ps1[31]=tail[1];c.r[6]=0x802DBF10;
}
bool arguments(unsigned index,const PPCContext& c){
    if(c.gqr[0]!=0)return false;
    if(index==0)return ram(c.r[3],48)&&ram(c.r[13]-0x4520,8);
    if(index==1)return ram(c.r[3],48)&&ram(c.r[4],48);
    // Multiple NaN operands can select different payloads after host compiler
    // instruction scheduling. Keep all non-finite arithmetic in the original.
    if(index==3)return ram(c.r[3],12)&&ram(c.r[4],12)&&finite(c.r[3],3)&&finite(c.r[4],3);
    if(index==4)return ram(c.r[3],12)&&ram(c.r[4],12)&&ram(c.r[5],12)&&finite(c.r[3],3)&&finite(c.r[4],3);
    const auto sp=c.r[1]-64;
    if(!ram(sp,64)||!ram(c.r[3],48)||!ram(c.r[4],48)||!ram(c.r[5],48))return false;
    // Original saves can alias matrix inputs/outputs inside its own ABI frame.
    // Keep those unusual cases in the original routine rather than reorder.
    for(unsigned i=3;i<=5;++i)if(uint64_t(c.r[i])<uint64_t(sp)+64&&uint64_t(sp)<uint64_t(c.r[i])+48)return false;
    return finite(c.r[3],12)&&finite(c.r[4],12)&&finite(0x802DBF10,2);
}
template<unsigned I>void hook(PPCContext& c){
    static const bool enabled=[] {auto s=std::getenv("SSNE_NATIVE_LINEAR_MATH");return !s||std::strcmp(s,"0");}();
    native_render_paths::poll(c);
    if(!enabled||!arguments(I,c)){ssne_call_original(c,addresses[I],originals[I]);return;}
    if constexpr(I==0)identity(c);else if constexpr(I==1)copy(c);else if constexpr(I==2)concatenate(c);else if constexpr(I==3)dot3(c);else cross3(c);
}
template<size_t...I>unsigned install_entries(std::index_sequence<I...>){
    unsigned count=0;const auto one=[&]<unsigned N>{
        auto entry=ssne_find_func(addresses[N]);if(!entry||*ssne_hook_slot(addresses[N]))return;
        uint64_t h=14695981039346656037ull;for(unsigned k=0;k<sizes[N];++k){h^=mem_read8(addresses[N]+k);h*=1099511628211ull;}
        if(h!=fingerprints[N])return;originals[N]=entry->func;ssne_install_hook(addresses[N],hook<N>);++count;
    };(one.template operator()<I>(),...);return count;
}
void install(){
    if(mem_read32(0x80000000)!=0x47334250u)return;
    const auto count=install_entries(std::make_index_sequence<5>{});log_message("[ssne] native EU vector/matrix math entries installed="+std::to_string(count)+"; existing hooks preserved");
}
}
