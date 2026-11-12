// Native geometry suffix and exact uncommon SDK-interrupt continuation.
// Guest register/scratch effects remain explicit at the recompiled ABI boundary.
namespace native_collision_pair {
#define SSNE_PAIR_INLINE __attribute__((always_inline)) inline
struct Replay;
thread_local Replay* replay=nullptr;
struct Replay{
 PPCContext& c;unsigned remaining;uint32_t enabled;Replay* previous;
 Replay(PPCContext& c,unsigned count):c(c),remaining(count),enabled(c.msr&0x8000u),previous(replay){replay=this;c.msr&=~0x8000u;}
 void checkpoint(){if(!--remaining){c.msr|=enabled;replay=previous;}}
 ~Replay(){if(remaining)c.msr=(c.msr&~0x8000u)|enabled;if(replay==this)replay=previous;}
};
void poll(PPCContext& c){if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);}
template<uint32_t A>void call(PPCContext& c,uint32_t lr){
 static const auto fn=ssne_find_func(A)->func;c.lr=lr;
 if constexpr(A==0x801BA658||A==0x801BA9A0||A==0x801BAA80||A==0x801BAA24){
  // Match SSNE_HOOK: select the live override BEFORE the entry poll. An IRQ
  // replacement affects subsequent calls, not this already dispatched call.
  const auto selected=*ssne_hook_slot(A);
  if(replay)replay->checkpoint();
  if(selected)selected(c);else ssne_call_original(c,A,fn);
 }else fn(c);
}

double number(uint32_t p){return double(u2f(mem_read32(p)));}
void pair(PPCContext& c,unsigned n,double v){c.ps0[n]=c.ps1[n]=v;}
void put(PPCContext& c,uint32_t p,unsigned n){mem_write32(p,f2u(float(c.ps0[n])));}
void compare(PPCContext& c,uint32_t a,uint32_t b){const auto x=int32_t(a),y=int32_t(b);cr_set_cmp(c,0,x<y,x>y,x==y);}
struct Frame {
 PPCContext& c;
 explicit Frame(PPCContext& c):c(c){
  const auto sp=c.r[1];c.r[1]-=0x90;mem_write32(c.r[1],sp);c.r[0]=c.lr;
  for(unsigned n=28;n<32;++n)mem_write64(c.r[1]+0x70+(n-28)*8,d2u(c.ps0[n]));
  for(unsigned n=23;n<32;++n)mem_write32(c.r[1]+0x4C+(n-23)*4,c.r[n]);mem_write32(c.r[1]+0x94,c.r[0]);
 }
 void finish(){
  c.r[0]=mem_read32(c.r[1]+0x94);c.lr=c.r[0];
  for(unsigned n=23;n<32;++n)c.r[n]=mem_read32(c.r[1]+0x4C+(n-23)*4);
  for(unsigned n=28;n<32;++n)c.ps0[n]=u2d(mem_read64(c.r[1]+0x70+(n-28)*8));c.r[1]+=0x90;
 }
};
SSNE_PAIR_INLINE void subtract(PPCContext& c){call<0x801BA9A0>(c,0x8015DD14);}
SSNE_PAIR_INLINE void length_squared(PPCContext& c){call<0x801BAA24>(c,0x8015DD2C);}
SSNE_PAIR_INLINE void centers(PPCContext& c){
 c.r[26]=mem_read32(c.r[28]+4);c.r[9]=mem_read32(c.r[13]-0x5BA0);c.r[0]=mem_read32(c.r[26]+0x2C);
 pair(c,28,number(c.r[9]+0x20));compare(c,c.r[0],0);c.r[27]=mem_read32(c.r[28]+8);
 if(!cr_bit(c,2)){
  pair(c,0,number(c.r[29]+8));c.r[11]=c.r[29]+8;c.r[9]=c.r[1]+8;put(c,c.r[1]+8,0);c.r[23]=c.r[9];
  pair(c,0,number(c.r[11]+4));put(c,c.r[9]+4,0);pair(c,13,number(c.r[11]+8));put(c,c.r[9]+8,13);
 }else{
  c.r[30]=c.r[1]+8;c.r[3]=c.r[26]+0x34;c.r[4]=c.r[29]+8;c.r[5]=c.r[1]+0x38;call<0x801BA658>(c,0x8015DC58);c.r[23]=c.r[30];
  pair(c,13,number(c.r[1]+0x38));pair(c,0,number(c.r[1]+0x40));pair(c,12,number(c.r[1]+0x3C));
  put(c,c.r[1]+0x30,0);put(c,c.r[1]+8,13);put(c,c.r[1]+0x28,13);put(c,c.r[1]+0x2C,12);put(c,c.r[30]+4,12);
  pair(c,0,number(c.r[1]+0x30));put(c,c.r[30]+8,0);
 }
 c.r[0]=mem_read32(c.r[27]+0x2C);compare(c,c.r[0],0);
 if(!cr_bit(c,2)){
  pair(c,13,number(c.r[31]+8));c.r[9]=c.r[31]+8;put(c,c.r[1]+0x18,13);
  pair(c,0,number(c.r[9]+4));put(c,c.r[1]+0x1C,0);pair(c,13,number(c.r[9]+8));put(c,c.r[1]+0x20,13);
 }else{
  c.r[3]=c.r[27]+0x34;c.r[4]=c.r[31]+8;c.r[5]=c.r[1]+0x38;call<0x801BA658>(c,0x8015DCC0);
  pair(c,0,number(c.r[1]+0x38));pair(c,13,number(c.r[1]+0x3C));pair(c,12,number(c.r[1]+0x40));
  put(c,c.r[1]+0x18,0);put(c,c.r[1]+0x1C,13);put(c,c.r[1]+0x20,12);put(c,c.r[1]+0x28,0);put(c,c.r[1]+0x2C,13);put(c,c.r[1]+0x30,12);
 }
}
SSNE_PAIR_INLINE bool swept_bounds(PPCContext& c){
 pair(c,12,number(c.r[27]+0x94));c.r[4]=c.r[1]+0x18;pair(c,0,number(c.r[31]+4));c.r[5]=c.r[1]+0x28;
 pair(c,13,number(c.r[26]+0x94));c.r[3]=c.r[23];pair(c,31,number(c.r[29]+4));pair(c,0,round_single(c.ps0[0]*c.ps0[12]));
 pair(c,29,number(c.r[28]+0x24));c.r[30]=c.r[28]+0x18;pair(c,31,round_single(c.ps0[31]*c.ps0[13]+c.ps0[0]));
 subtract(c);c.r[3]=c.r[1]+0x28;c.r[4]=c.r[30];call<0x801BAA80>(c,0x8015DD20);
 c.ps0[30]=c.ps0[1];c.r[3]=c.r[1]+0x28;length_squared(c);
 c.r[9]=0x80220000;pair(c,31,round_single(c.ps0[31]*c.ps0[31]));pair(c,0,number(c.r[9]+0xE4C));fcmp(c,0,c.ps0[30],c.ps0[0]);
 bool reject=false;
 if(cr_bit(c,0)){fcmp(c,0,c.ps0[1],c.ps0[31]);reject=cr_bit(c,1);}
 if(!reject){
  c.r[9]=0x80220000;pair(c,0,round_single(c.ps0[31]-c.ps0[1]));pair(c,13,number(c.r[9]+0xE4C));
  pair(c,1,round_single(c.ps0[30]*c.ps0[30]+c.ps0[0]));fcmp(c,0,c.ps0[1],c.ps0[13]);reject=cr_bit(c,0);
  if(!reject){
   fcmp(c,0,c.ps0[30],c.ps0[13]);
   if(cr_bit(c,0))c.r[0]=1;
   else{
    pair(c,0,round_single(c.ps0[28]/c.ps0[29]));c.r[0]=1;pair(c,0,round_single(c.ps0[30]-c.ps0[0]));fcmp(c,0,c.ps0[0],c.ps0[13]);
    if(cr_bit(c,1)){pair(c,0,round_single(c.ps0[0]*c.ps0[0]));fcmp(c,0,c.ps0[0],c.ps0[1]);reject=cr_bit(c,1);}
   }
  }
 }
 if(reject)c.r[0]=0;compare(c,c.r[0],0);return !cr_bit(c,2);
}
SSNE_PAIR_INLINE void remember_pair(PPCContext& c){
 c.r[8]=mem_read32(c.r[13]-0x5B9C);c.r[6]=c.r[3];c.r[7]=c.r[31];compare(c,c.r[3],c.r[31]);
 if(cr_bit(c,1)){c.r[6]=c.r[31];c.r[7]=c.r[3];}
 const auto s=int32_t(c.r[7]);c.r[9]=uint32_t(s>>1);c.xer_ca=s<0&&(uint32_t(s)<<31)!=0;
 c.r[9]=c.r[6]+c.r[9];c.r[9]=rotl32(c.r[9],31)&0xFC;c.r[10]=mem_read32(c.r[8]+c.r[9]);c.r[5]=c.r[8]+c.r[9];compare(c,c.r[10],0);
 while(!cr_bit(c,2)){
  c.r[0]=mem_read32(c.r[10]);compare(c,c.r[0],c.r[6]);
  if(cr_bit(c,2)){c.r[0]=mem_read32(c.r[10]+4);compare(c,c.r[0],c.r[7]);if(cr_bit(c,2)){c.r[0]=0;compare(c,c.r[0],0);return;}}
  c.r[5]=c.r[10]+8;c.r[10]=mem_read32(c.r[10]+8);compare(c,c.r[10],0);if(!cr_bit(c,2))poll(c);
 }
 c.r[10]=mem_read32(c.r[8]+0x108);c.r[0]=mem_read32(c.r[8]+0x104);compare(c,c.r[10],c.r[0]);
 if(cr_bit(c,0)){
  c.r[11]=uint32_t(int32_t(c.r[10])*12);c.r[9]=mem_read32(c.r[8]+0x100);c.r[0]=c.r[10]+1;mem_write32(c.r[8]+0x108,c.r[0]);c.r[10]=c.r[9]+c.r[11];
  mem_write32(c.r[5],c.r[10]);mem_write32(c.r[9]+c.r[11],c.r[6]);mem_write32(c.r[10]+4,c.r[7]);mem_write32(c.r[10]+8,c.r[26]);
 }
 c.r[0]=1;compare(c,c.r[0],0);
}
SSNE_PAIR_INLINE void leaves(PPCContext& c){
 c.r[9]=mem_read32(c.r[28]+4);c.r[0]=mem_read32(c.r[29]+0x14);c.r[11]=mem_read32(c.r[9]+0xC);c.r[3]=mem_read16(c.r[29]+2);c.r[0]+=c.r[0];
 c.r[9]=mem_read32(c.r[11]+0x3C);c.r[29]=mem_read32(c.r[11]+0x34);compare(c,c.r[3],0);c.r[9]+=c.r[0];if(cr_bit(c,2))return;c.r[26]=0;
 for(;;){
  c.r[0]=mem_read16(c.r[9]);c.r[27]=c.r[9]+2;c.r[11]=mem_read16(c.r[31]);c.r[30]=c.r[3]-1;c.r[0]=uint32_t(int32_t(c.r[0])*10);
  c.r[9]=mem_read16(c.r[29]+c.r[0]);c.r[3]=c.r[29]+c.r[0];c.r[9]&=c.r[11];c.r[0]=c.r[9]&0xFC;cr0_update(c,c.r[0]);
  if(!cr_bit(c,2)){remember_pair(c);if(!cr_bit(c,2)){c.r[4]=c.r[31];c.r[5]=c.r[28];call<0x8015C404>(c,0x8015DEF4);}}
  c.r[9]=c.r[27];c.r[3]=c.r[30];cr0_update(c,c.r[3]);if(cr_bit(c,2))return;poll(c);
 }
}
SSNE_PAIR_INLINE void after_bounds(PPCContext& c){
 c.r[0]=c.r[25]^1;c.r[9]=c.r[0]&1;cr0_update(c,c.r[9]);
 if(!cr_bit(c,2)){
  c.r[0]=c.r[24]&1;cr0_update(c,c.r[0]);const bool finish=!cr_bit(c,2);
  c.r[9]=mem_read32(c.r[28]+4);c.r[3]=c.r[29];c.r[4]=c.r[31];c.r[6]=c.r[28];c.r[5]=mem_read32(c.r[9]+0xC);
  if(finish){call<0x8015CC30>(c,0x8015DDC8);return;}call<0x8015CC30>(c,0x8015DDE4);
 }
 c.r[0]=c.r[25]&2;cr0_update(c,c.r[0]);if(!cr_bit(c,2)){leaves(c);return;}
 c.r[9]=mem_read32(c.r[13]-0x5BA0);c.r[0]=mem_read32(c.r[9]+0x1C);compare(c,c.r[0],0);if(!cr_bit(c,2))return;
 c.r[3]=c.r[29];c.r[4]=c.r[31];c.r[7]=c.r[28];c.ps0[1]=c.ps0[28];c.r[5]=c.r[1]+8;c.r[6]=c.r[1]+0x18;call<0x8015C790>(c,0x8015DF30);
}
SSNE_PAIR_INLINE void query(PPCContext& c){
 c.r[29]=c.r[3];c.r[31]=c.r[4];c.r[25]=mem_read16(c.r[29]);c.r[28]=c.r[5];c.r[24]=mem_read16(c.r[31]);c.r[0]=c.r[25]&c.r[24];c.r[9]=c.r[0]&0xFC;cr0_update(c,c.r[9]);if(cr_bit(c,2))return;
 centers(c);if(!swept_bounds(c))return;
 after_bounds(c);
}
void body(PPCContext& c){Frame frame(c);query(c);frame.finish();}
void resume_at_sdk(PPCContext& c,unsigned ordinal){
 Replay guard(c,ordinal);body(c);
 if(guard.remaining)throw std::runtime_error("native collision SDK replay boundary not reached");
}
#undef SSNE_PAIR_INLINE
}
