// EU8015CC30: direct child lists and compressed octants, with original visit
// order, call-site return addresses and recompiled boundary state retained.
#include "native_collision_batch.inc"
namespace native_collision_traversal {
constexpr uint32_t address=0x8015CC30;
PPCFunc original=nullptr;
template<unsigned... N>constexpr uint32_t mask(){return ((1u<<N)|...);}
double number(uint32_t p){return double(u2f(mem_read32(p)));}
void pair(PPCContext& c,unsigned n,double v){c.ps0[n]=c.ps1[n]=v;}
void put(PPCContext& c,uint32_t p,unsigned n){mem_write32(p,f2u(float(c.ps0[n])));}
void cmp(PPCContext& c,uint32_t a,uint32_t b){auto x=int32_t(a),y=int32_t(b);cr_set_cmp(c,0,x<y,x>y,x==y);}
struct Frame{
 PPCContext& c;
 explicit Frame(PPCContext& x):c(x){auto old=c.r[1];c.r[1]-=0x108;mem_write32(c.r[1],old);c.r[0]=c.lr;
  mem_write64(c.r[1]+0xF8,d2u(c.ps0[30]));mem_write64(c.r[1]+0x100,d2u(c.ps0[31]));
  for(unsigned k=26;k<32;++k)mem_write32(c.r[1]+0xE0+(k-26)*4,c.r[k]);mem_write32(c.r[1]+0x10C,c.r[0]);}
 void finish(){c.r[0]=mem_read32(c.r[1]+0x10C);c.lr=c.r[0];for(unsigned k=26;k<32;++k)c.r[k]=mem_read32(c.r[1]+0xE0+(k-26)*4);
  c.ps0[30]=u2d(mem_read64(c.r[1]+0xF8));c.ps0[31]=u2d(mem_read64(c.r[1]+0x100));c.r[1]+=0x108;}
};
struct Direct{
 explicit Direct(PPCContext&){}
 void writes(uint32_t,uint32_t=0,bool=false,bool=false,bool=false,bool=false){}
 void call(PPCContext& c,uint32_t caller){static const auto fn=ssne_find_func(0x8015DBC0)->func;c.lr=caller;fn(c);}
 void poll(PPCContext& c){if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);}
 void finish(PPCContext&){}
};
template<class Visits>void list(PPCContext& c,Visits& visits,uint32_t caller){
 c.r[0]=mem_read32(c.r[31]+0x14);c.r[9]=mem_read32(c.r[5]+0x28);c.r[0]=uint32_t(int32_t(c.r[0])*24);
 c.r[31]=mem_read16(c.r[31]+2);cmp(c,c.r[31],0);c.r[30]=c.r[9]+c.r[0];visits.writes(mask<0,9,30,31>(),0,true);
 if(cr_bit(c,2))return;
 for(;;){c.r[3]=c.r[30];c.r[4]=c.r[28];c.r[5]=c.r[29];c.r[30]+=24;visits.writes(mask<3,4,5,30>(),0,false,false,false,true);visits.call(c,caller);
  auto count=c.r[31];c.r[31]=count-1;c.xer_ca=c.r[31]<count;cr0_update(c,c.r[31]);visits.writes(mask<31>(),0,true,true);
  if(cr_bit(c,2))return;visits.poll(c);}
}
template<unsigned X,unsigned Y,unsigned Z>void corner(PPCContext& c,unsigned index,unsigned scale){
 pair(c,0,number(c.r[31]+4));c.r[X]=index&1;cr0_update(c,c.r[X]);pair(c,11,number(c.r[31]+8));pair(c,0,round_single(c.ps0[0]*c.ps0[scale]));
 pair(c,11,round_single(cr_bit(c,2)?c.ps0[11]+c.ps0[0]:c.ps0[11]-c.ps0[0]));
 pair(c,12,number(c.r[31]+0xC));c.r[Y]=index&2;cr0_update(c,c.r[Y]);pair(c,12,round_single(cr_bit(c,2)?c.ps0[12]-c.ps0[0]:c.ps0[12]+c.ps0[0]));
 pair(c,13,number(c.r[31]+0x10));c.r[Z]=index&4;cr0_update(c,c.r[Z]);pair(c,13,round_single(cr_bit(c,2)?c.ps0[13]-c.ps0[0]:c.ps0[13]+c.ps0[0]));
}
void descriptor(PPCContext& c,uint32_t packed,uint32_t output,unsigned scale){
 c.r[0]=mem_read16(packed);mem_write16(output,uint16_t(c.r[0]));c.r[0]=mem_read16(packed);c.r[9]=c.r[0]&1;cr0_update(c,c.r[9]);
 c.r[0]=cr_bit(c,2)?0:rotl32(c.r[0],24)&0xFFFFFF;mem_write16(output+2,uint16_t(c.r[0]));
 c.r[0]=mem_read16(packed+4);c.r[9]=mem_read16(packed+2);pair(c,0,number(c.r[31]+4));c.r[0]=rotl32(c.r[0],16)&0xFFFF0000;c.r[9]+=c.r[0];
 put(c,output+8,11);pair(c,0,round_single(c.ps0[0]*c.ps0[scale]));mem_write32(output+0x14,c.r[9]);put(c,output+4,0);
}
template<class Visits>void stream(PPCContext& c,Visits& visits){
 c.r[0]=mem_read32(c.r[31]+0x14);c.r[11]=0x80220000;c.r[10]=mem_read32(c.r[5]+0x44);c.r[9]=0x80220000;c.r[0]=uint32_t(int32_t(c.r[0])*6);
 pair(c,30,number(c.r[11]+0xE3C));pair(c,31,number(c.r[9]+0xE40));c.r[26]=rotl32(c.r[8],24)&255;c.r[27]=0;c.r[30]=c.r[10]+c.r[0];
 visits.writes(mask<0,9,10,11,26,27,30>(),mask<30,31>());
 for(;;){
  c.r[0]=c.r[26]&1;cr0_update(c,c.r[0]);visits.writes(mask<0>(),0,true);
  if(!cr_bit(c,2)){
   corner<9,0,9>(c,c.r[27],30);descriptor(c,c.r[30],c.r[1]+8,31);
   c.r[3]=c.r[1]+8;c.r[4]=c.r[28];c.r[5]=c.r[29];c.r[30]+=6;put(c,c.r[1]+0x14,12);put(c,c.r[1]+0x18,13);
   visits.writes(mask<0,3,4,5,9,30>(),mask<0,11,12,13>(),true,false,false,true);visits.call(c,0x8015CDD8);
  }
  ++c.r[27];const auto s=int32_t(c.r[26]);c.r[26]=uint32_t(s>>1);c.xer_ca=s<0&&(uint32_t(s)<<31)!=0;cmp(c,c.r[27],7);
  visits.writes(mask<26,27>(),0,true,true);if(cr_bit(c,1))return;visits.poll(c);
 }
}
constexpr std::array<uint32_t,8> order_callers={0x8015D194,0x8015D4C8,0x8015D000,0x8015D334,0x8015D0D0,0x8015D404,0x8015CF3C,0x8015D270};
template<class Visits>void octants(PPCContext& c,Visits& visits){
 c.r[9]=8;c.r[5]=mem_read32(c.r[5]+0x44);c.ctr=c.r[9];c.r[0]=mem_read32(c.r[31]+0x14);c.r[10]=c.r[8];c.r[27]=c.r[1]+0x20;
 visits.writes(mask<0,5,9,10,27>(),0,false,false,true);
 do{--c.ctr;visits.writes(0,0,false,false,true);if(c.ctr)visits.poll(c);}while(c.ctr);
 c.r[0]=uint32_t(int32_t(c.r[0])*6);c.r[11]=0x80220000;c.r[9]=0x80220000;pair(c,10,number(c.r[11]+0xE40));c.r[30]=rotl32(c.r[10],24)&255;
 pair(c,9,number(c.r[9]+0xE3C));c.r[11]=c.r[27];c.r[10]=0;c.r[5]+=c.r[0];c.r[0]=8;c.ctr=c.r[0];
 visits.writes(mask<0,5,9,10,11,30>(),mask<9,10>(),false,false,true);
 do{
  c.r[9]=c.r[30]&1;cr0_update(c,c.r[9]);visits.writes(mask<9>(),0,true);
  if(!cr_bit(c,2)){
   corner<0,9,0>(c,c.r[10],9);descriptor(c,c.r[5],c.r[11],10);c.r[5]+=6;put(c,c.r[11]+0xC,12);put(c,c.r[11]+0x10,13);
   visits.writes(mask<0,5,9>(),mask<0,11,12,13>(),true);
  }
  ++c.r[10];const auto s=int32_t(c.r[30]);c.r[30]=uint32_t(s>>1);c.xer_ca=s<0&&(uint32_t(s)<<31)!=0;c.r[11]+=24;--c.ctr;
  visits.writes(mask<10,11,30>(),0,false,true,true);if(c.ctr)visits.poll(c);
 }while(c.ctr);
 c.r[9]=0x80220000;c.r[0]=mem_read16(c.r[31]);pair(c,13,number(c.r[9]+0xE44));pair(c,0,number(c.r[29]+0x18));c.r[30]=rotl32(c.r[0],24)&0xFFFFFF;fcmp(c,0,c.ps0[0],c.ps0[13]);
 const bool positive_x=!cr_bit(c,0);pair(c,0,number(c.r[29]+0x1C));fcmp(c,0,c.ps0[0],c.ps0[13]);const bool positive_y=cr_bit(c,1);
 pair(c,0,number(c.r[29]+0x20));fcmp(c,0,c.ps0[0],c.ps0[13]);const bool positive_z=cr_bit(c,1);
 const unsigned signs=unsigned(positive_x)|(unsigned(positive_y)<<1)|(unsigned(positive_z)<<2);
 const unsigned first=unsigned(positive_x)|(unsigned(!positive_y)<<1)|(unsigned(!positive_z)<<2);
 visits.writes(mask<0,9,30>(),mask<0,13>(),true);
 for(unsigned i=0;i<8;++i){const auto index=i^first;c.r[0]=c.r[30]&(1u<<index);cr0_update(c,c.r[0]);visits.writes(mask<0>(),0,true);
  if(!cr_bit(c,2)){c.r[3]=index?c.r[1]+0x20+index*24:c.r[27];c.r[4]=c.r[28];c.r[5]=c.r[29];visits.writes(mask<3,4,5>(),0,false,false,false,true);visits.call(c,order_callers[signs]+i*24);}
 }
}
template<class Visits>void body(PPCContext& c){
 Frame frame(c);Visits visits(c);c.r[31]=c.r[3];c.r[28]=c.r[4];c.r[8]=mem_read16(c.r[31]);c.r[29]=c.r[6];c.r[0]=c.r[8]&1;cr0_update(c,c.r[0]);
 visits.writes(mask<0,8,28,29,31>(),0,true);
 if(!cr_bit(c,2)){list(c,visits,0x8015CC90);}
 else{c.r[0]=c.r[8]&0xFF00;cmp(c,c.r[0],0);visits.writes(mask<0>(),0,true);
  if(cr_bit(c,2)){list(c,visits,0x8015CCD8);}
  else{c.r[9]=mem_read32(c.r[29]+8);c.r[0]=mem_read32(c.r[9]+0x2C);cmp(c,c.r[0],0);visits.writes(mask<0,9>(),0,true);
   if(cr_bit(c,2))stream(c,visits);else octants(c,visits);}}
 visits.finish(c);frame.finish();
}
void hook(PPCContext& c){
 if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);
 if(c.r[1]<0x80000400u||uint64_t(c.r[1])+0x10>guest_ram_end()){ssne_call_original(c,address,original);return;}
 const auto node=c.r[3],sp=c.r[1]-0x108;
 const auto finite_words=[](uint32_t p,unsigned n){for(unsigned k=0;k<n;++k)if((mem_read32(p+k*4)&0x7F800000u)==0x7F800000u)return false;return true;};
 if(!native_collision_batch::ram(node,24)||native_collision_batch::intersects(node,24,sp,c.r[1]+8)){
  ssne_call_original(c,address,original);return;
 }
 const auto flags=mem_read16(node);
 if(!(flags&1)&&(flags&0xFF00) &&
    (!finite_words(node+4,4)||!finite_words(0x80220E3C,3)||!native_collision_batch::ram(c.r[6]+0x18,12)||!finite_words(c.r[6]+0x18,3))){
  // LLVM may choose different NaN signs when folding conditional +/- in
  // expanded corners. Preserve the original for this non-finite domain.
  ssne_call_original(c,address,original);return;
 }
 static const bool batch=[] {auto value=std::getenv("SSNE_NATIVE_COLLISION_BATCH");return !value||std::strcmp(value,"0");}();
 if(batch)body<native_collision_batch::Batch>(c);else body<Direct>(c);
}
void install(){
 const auto env=std::getenv("SSNE_NATIVE_COLLISION_TRAVERSAL");if(env&&!std::strcmp(env,"0"))return;
 const auto entry=ssne_find_func(address);if(!entry||mem_read32(0x80000000)!=0x47334250u||*ssne_hook_slot(address))return;
 uint64_t h=14695981039346656037ull;for(unsigned i=0;i<2396;++i){h^=mem_read8(address+i);h*=1099511628211ull;}
 if(h!=0x97B2326FDABA6260ull)return;
 auto fingerprint=[](uint32_t p,unsigned n){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<n;++i){h^=mem_read8(p+i);h*=1099511628211ull;}return h;};
 native_collision_batch::ready=fingerprint(0x8015DBC0,916)==0x94F03672B7C1A14Eull&&fingerprint(0x801BA658,84)==0xF0DFCD313EB3BFF9ull&&
  fingerprint(0x801BA9A0,36)==0x4DD0A78B94B91954ull&&fingerprint(0x801BAA24,24)==0x2F03B48C62EEAD37ull&&fingerprint(0x801BAA80,32)==0xD3D5048A15A88E46ull;
 original=entry->func;ssne_install_hook(address,hook);
}
}
