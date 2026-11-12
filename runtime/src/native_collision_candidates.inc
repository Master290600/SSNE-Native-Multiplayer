// EU8015EB6C: gather broad-phase candidates, filter masks, prepare each sweep.
// Preserve guest state at callbacks and interrupt boundaries. Candidate contents
// are read afresh; callbacks may change later objects and even the list itself.
namespace native_collision_candidates {
constexpr uint32_t address=0x8015EB6C;
PPCFunc original=nullptr;
bool native_child=true;
void cmp(PPCContext& c,uint32_t a,uint32_t b){const auto x=int32_t(a),y=int32_t(b);cr_set_cmp(c,0,x<y,x>y,x==y);}
void pair(PPCContext& c,unsigned n,double value){c.ps0[n]=c.ps1[n]=value;}
double number(uint32_t p){return double(u2f(mem_read32(p)));}
void put(PPCContext& c,uint32_t p,unsigned n){mem_write32(p,f2u(float(c.ps0[n])));}
void poll(PPCContext& c){if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);}
template<uint32_t A>void call(PPCContext& c,uint32_t lr){static const auto f=ssne_find_func(A)->func;c.lr=lr;f(c);}
// A standalone root query has no later parent writes to merge. Emit its exact
// completed rejection directly, rather than constructing and flushing a Batch
// (which snapshots the entire CPU for deferred state merging).
void root(PPCContext& c,uint32_t caller){
 using namespace native_collision_batch;
 c.lr=caller;if(statistics)++totals.parents;
 if((c.msr&0x8000u)&&g_irq_pending.load(std::memory_order_relaxed)){
  if(statistics){++totals.executed;++totals.blocked_irq;}
  call<0x8015DBC0>(c,caller);return;
 }
 Inputs input;const auto decision=input.predict(c);
 if(statistics){++totals.predictions;if(decision==Decision::MaskReject)++totals.mask;else if(decision==Decision::BoundsReject)++totals.bounds;else if(decision==Decision::Visit)++totals.visits;else if(decision==Decision::Interrupted)++totals.interrupted;else++totals.unknown;}
 if(decision==Decision::Unknown){if(statistics)++totals.executed;call<0x8015DBC0>(c,caller);return;}
 if(decision==Decision::Interrupted){native_collision_pair::resume_at_sdk(c,input.interrupted_at);return;}
 if(decision==Decision::Visit){if(statistics)++totals.native_visits;visit(c,input);return;}
 const auto node=c.r[3],work=c.r[5],sp=c.r[1]-0x90;
 mem_write32(sp,c.r[1]);
 for(unsigned k=23;k<32;++k)mem_write32(sp+0x4C+(k-23)*4,c.r[k]);
 for(unsigned k=28;k<32;++k)mem_write64(sp+0x70+(k-28)*8,d2u(c.ps0[k]));
 mem_write32(sp+0x94,caller);
 if(decision==Decision::BoundsReject)emit_math(c,sp,input.result,node,work);else c.r[9]=0;
 cr0_update(c,0);c.r[0]=caller;
 if(statistics){++totals.materialized;++totals.flushed;}
}
struct Frame{
 PPCContext& c;
 explicit Frame(PPCContext& c):c(c){const auto sp=c.r[1];c.r[1]-=0x520;mem_write32(c.r[1],sp);c.r[0]=c.lr;
  for(unsigned k=29;k<32;++k)mem_write64(c.r[1]+0x508+(k-29)*8,d2u(c.ps0[k]));
  for(unsigned k=24;k<32;++k)mem_write32(c.r[1]+0x4E8+(k-24)*4,c.r[k]);mem_write32(c.r[1]+0x524,c.r[0]);}
 void finish(){c.r[0]=mem_read32(c.r[1]+0x524);c.lr=c.r[0];for(unsigned k=24;k<32;++k)c.r[k]=mem_read32(c.r[1]+0x4E8+(k-24)*4);
  for(unsigned k=29;k<32;++k)c.ps0[k]=u2d(mem_read64(c.r[1]+0x508+(k-29)*8));c.r[1]+=0x520;}
};
void prepare_sweep(PPCContext& c){
 c.r[27]=c.r[10]+4;c.r[9]=mem_read32(c.r[13]-0x5B9C);c.r[10]=0x40;c.r[0]=0;c.ctr=c.r[10];c.r[26]=c.r[11]+1;
 mem_write32(c.r[9]+0x108,c.r[0]);c.r[11]=c.r[30]+0x9C;c.r[9]+=0xFC;c.r[28]=0x80220000;c.r[29]=c.r[1]+0x4B8;
 do{mem_write32(c.r[9],c.r[0]);c.r[9]-=4;--c.ctr;if(c.ctr)poll(c);}while(c.ctr);
 pair(c,0,number(c.r[30]+0x9C));c.r[3]=c.r[1]+0x4C4;pair(c,12,number(c.r[11]+8));pair(c,13,number(c.r[11]+4));
 put(c,c.r[1]+0x4C4,0);put(c,c.r[1]+0x4C8,13);put(c,c.r[1]+0x4CC,12);mem_write32(c.r[1]+0x4BC,c.r[31]);mem_write32(c.r[1]+0x4C0,c.r[30]);
 call<0x801BAA3C>(c,0x8015ECD4);pair(c,0,number(c.r[28]+0xE6C));fcmp(c,0,c.ps0[1],c.ps0[0]);
 if(cr_bit(c,1)){
  pair(c,31,round_single(c.ps0[30]/c.ps0[1]));c.r[3]=c.r[1]+0x4C4;c.r[4]=c.r[1]+0x4D0;c.ps0[1]=c.ps0[31];
  call<0x801BA9C4>(c,0x8015ECF4);put(c,c.r[1]+0x4DC,31);
 }else{put(c,c.r[1]+0x4D0,29);put(c,c.r[25]+4,30);put(c,c.r[25]+8,29);put(c,c.r[1]+0x4DC,30);}
 mem_write32(c.r[1]+0x4B8,c.r[29]);c.r[5]=c.r[29];c.r[11]=mem_read32(c.r[30]+0xC);c.r[9]=mem_read32(c.r[31]+0xC);
 c.r[4]=mem_read32(c.r[11]+0x18);c.r[3]=mem_read32(c.r[9]+0x18);c.r[4]=uint32_t(int32_t(c.r[4])*24);
 c.r[10]=mem_read32(c.r[9]+0x28);c.r[0]=mem_read32(c.r[11]+0x28);c.r[3]=uint32_t(int32_t(c.r[3])*24);
 c.r[4]=c.r[0]+c.r[4];c.r[3]=c.r[10]+c.r[3];
 if(native_child)root(c,0x8015ED40);
 else call<0x8015DBC0>(c,0x8015ED40);
}
bool eligible(PPCContext& c){
 cmp(c,c.r[30],c.r[31]);if(cr_bit(c,2))return false;c.r[0]=mem_read32(c.r[31]+0x30);cmp(c,c.r[0],0);if(cr_bit(c,2))return false;
 c.r[9]=mem_read16(c.r[31]+0xA);c.r[0]=mem_read16(c.r[30]+8);c.r[8]=c.r[0]&c.r[9];cr0_update(c,c.r[8]);if(!cr_bit(c,2))return true;
 c.r[9]=mem_read16(c.r[30]+0xA);c.r[0]=mem_read16(c.r[31]+8);c.r[8]=c.r[0]&c.r[9];cr0_update(c,c.r[8]);return !cr_bit(c,2);
}
void body(PPCContext& c){
 Frame frame(c);c.r[30]=c.r[4];c.r[31]=c.r[3];c.r[9]=mem_read32(c.r[30]+0xC);c.r[0]=mem_read32(c.r[9]+0xC);cmp(c,c.r[0],1);
 if(cr_bit(c,2)){c.r[0]=0;mem_write32(c.r[9]+0xC,c.r[0]);}
 c.r[0]=mem_read32(c.r[31]+0x1590);c.r[9]=0x80220000;mem_write32(c.r[13]-0x5BA0,c.r[5]);c.r[11]=0;
 mem_write32(c.r[13]-0x5B9C,c.r[0]);c.r[3]=c.r[30];mem_write32(c.r[13]-0x5BA4,c.r[31]);pair(c,31,number(c.r[9]+0xE68));
 mem_write32(c.r[31]+0x1594,c.r[6]);mem_write32(c.r[5]+0x10,c.r[11]);put(c,c.r[5]+0x20,31);
 c.r[0]=mem_read32(c.r[13]-0x6A48);c.r[9]=mem_read32(c.r[31]+0x1588);c.r[0]+=c.r[9];mem_write32(c.r[13]-0x6A48,c.r[0]);
 call<0x8015B29C>(c,0x8015EBE8);
 pair(c,1,number(c.r[30]+0x10));c.r[3]=c.r[31];pair(c,2,number(c.r[30]+0x18));c.r[4]=c.r[1]+8;pair(c,3,number(c.r[30]+0x1C));c.r[5]=0x12C;
 call<0x8015B520>(c,0x8015EC04);
 c.r[0]=mem_read32(c.r[13]-0x6A44);c.r[24]=c.r[3];c.r[11]=0;c.r[10]=c.r[1]+8;c.r[0]+=c.r[24];cmp(c,c.r[11],c.r[24]);mem_write32(c.r[13]-0x6A44,c.r[0]);
 if(cr_bit(c,0)){
  c.r[9]=0x80220000;c.ps0[30]=c.ps0[31];pair(c,29,number(c.r[9]+0xE70));c.r[25]=c.r[1]+0x4D0;
  for(;;){
   c.r[31]=mem_read32(c.r[10]);c.r[26]=c.r[11]+1;c.r[27]=c.r[10]+4;
   if(eligible(c))prepare_sweep(c);
   c.r[11]=c.r[26];c.r[10]=c.r[27];cmp(c,c.r[11],c.r[24]);if(!cr_bit(c,0))break;poll(c);
  }
 }
 c.r[9]=mem_read32(c.r[13]-0x5BA0);c.r[3]=mem_read32(c.r[9]+0x10);frame.finish();
}
void hook(PPCContext& c){
 poll(c);
 if(c.r[1]<0x80000900u||uint64_t(c.r[1])+8>guest_ram_end()){ssne_call_original(c,address,original);return;}
 body(c);
}
void install(){
 const auto env=std::getenv("SSNE_NATIVE_COLLISION_CANDIDATES");if(env&&!std::strcmp(env,"0"))return;
 const auto entry=ssne_find_func(address);if(!entry||mem_read32(0x80000000)!=0x47334250u||*ssne_hook_slot(address))return;
 uint64_t h=14695981039346656037ull;for(unsigned k=0;k<524;++k){h^=mem_read8(address+k);h*=1099511628211ull;}
 if(h!=0xE5A9BC9DB347AD5Dull)return;
 original=entry->func;ssne_install_hook(address,hook);
}
}
