// Typed bound calculations for a whole traversal. Rejected children still
// receive their mathematical test; redundant internal guest ABI work can be
// materialized at the last observable boundary instead of for every child.
#include "native_collision_pair.inc"
namespace native_linear_math {template<unsigned>void hook(PPCContext&);}
namespace native_collision_batch {
enum class Decision {Unknown,MaskReject,BoundsReject,Visit,Interrupted};
bool ready=false,statistics=false;
struct Statistics{uint64_t predictions=0,mask=0,bounds=0,visits=0,unknown=0,materialized=0,flushed=0,executed=0,blocked_irq=0,parents=0,native_visits=0,interrupted=0;};
Statistics totals;
void enable_statistics(){statistics=true;}
void read_statistics(uint64_t* out){const uint64_t values[]={totals.predictions,totals.mask,totals.bounds,totals.visits,totals.unknown,totals.materialized,totals.flushed,totals.executed,totals.blocked_irq,totals.parents};std::memcpy(out,values,sizeof(values));}
bool ram(uint32_t p,unsigned bytes){return uint32_t(p-0x80000000u)<=kRamSize-bytes;}
bool intersects(uint32_t p,unsigned bytes,uint32_t lo,uint32_t hi){return uint64_t(p)<hi&&uint64_t(lo)<uint64_t(p)+bytes;}
double number(uint32_t p){return double(u2f(mem_read32(p)));}
using Vector=std::array<double,3>;
// A rejection has no geometry callouts. Retain its already calculated values
// so the final ABI boundary can be emitted without running the math a second
// time. All stored values have already undergone the original single rounding.
struct BoundResult{
 std::array<float,3> a,b,d,ray,scratch;
 std::array<float,8> matrix;
 float f0=0,f1=0,f1high=0,yy=0,zz=0,xy=0,yz=0,scale_b=0,f13=0;
 float tolerance=0,speed=0,radius2=0,projection=0;
 bool transformed=false,direct_a=false;
};
Vector center(uint32_t node,uint32_t model){
 Vector p={number(node+8),number(node+12),number(node+16)};
 if(mem_read32(model+0x2C))return p;
 Vector result;
 for(unsigned row=0;row<3;++row){auto m=model+0x34+row*16;
  const auto x=round_single(number(m)*p[0]),y=round_single(number(m+4)*p[1]);
  const auto zx=round_single(number(m+8)*p[2]+x),wy=round_single(number(m+12)*1.+y);
  result[row]=round_single(zx+wy);
 }return result;
}
#ifdef SSNE_COLLISION_TEST_PROBES
void(*checkpoint_probe)(unsigned)=nullptr;
#endif
struct Inputs{
 bool valid=false,transform_a=false;unsigned interrupted_at=0;uint32_t model_a=0,model_b=0;uint16_t source_flags=0;uint32_t target=0,work=0,stack=0,sda=0;uint16_t target_flags=0;
 std::array<double,12> matrix_a;Vector center_b,ray;
 double scale_a=0,radius_b=0,tolerance=0,speed=0,zero=0;
 double scale_b=0;
 bool transform_b=false;
 std::array<float,8> matrix_b;
 BoundResult result;
 bool initialize(const PPCContext& c){
 valid=false;if(!ready||c.gqr[0]||c.r[1]<0x80000400u||uint64_t(c.r[1])+0x110>guest_ram_end())return false;
 static const auto root=ssne_hook_slot(0x8015DBC0),mtx=ssne_hook_slot(0x801BA658),sub=ssne_hook_slot(0x801BA9A0),length=ssne_hook_slot(0x801BAA24),dot=ssne_hook_slot(0x801BAA80);
 if(*root||*mtx||*sub||*length||(*dot&&*dot!=native_linear_math::hook<3>))return false;
 const auto b=c.r[4],w=c.r[5],lo=c.r[1]-0x90,hi=c.r[1]+0x110;
 if(!ram(b,24)||intersects(b,24,lo,hi))return false;
 if(!ram(w,0x28)||intersects(w,0x28,lo,hi)||!ram(c.r[13]-0x5BA0,4)||intersects(c.r[13]-0x5BA0,4,lo,hi)||intersects(0x80220E4C,4,lo,hi))return false;
 const auto ma=mem_read32(w+4),mb=mem_read32(w+8),world=mem_read32(c.r[13]-0x5BA0);
 if(!ram(ma,0x98)||!ram(mb,0x98)||!ram(world,0x24)||intersects(ma,0x98,lo,hi)||intersects(mb,0x98,lo,hi)||intersects(world,0x24,lo,hi))return false;
 model_a=ma;model_b=mb;target=b;work=w;stack=c.r[1];sda=c.r[13];target_flags=mem_read16(b);transform_a=mem_read32(ma+0x2C)==0;
 if(transform_a)for(unsigned k=0;k<12;++k)matrix_a[k]=number(ma+0x34+k*4);
 transform_b=mem_read32(mb+0x2C)==0;
 center_b=center(b,mb);scale_a=number(ma+0x94);scale_b=number(mb+0x94);radius_b=round_single(number(b+4)*scale_b);
 if(transform_b){const auto x=number(b+8),y=number(b+12),z=number(b+16);const auto m=mb+0x44;
  const auto ax=round_single(number(m)*x),ay=round_single(number(m+4)*y);
  matrix_b={float(number(m)),float(number(m+4)),float(number(m+8)),float(number(m+12)),float(ax),float(ay),float(round_single(number(m+8)*z+ax)),float(round_single(number(m+12)*1.+ay))};
 }
 for(unsigned k=0;k<3;++k)ray[k]=number(w+0x18+k*4);
 tolerance=number(world+0x20);speed=number(w+0x24);zero=number(0x80220E4C);valid=true;return true;
 }
 bool checkpoint(const PPCContext& c,unsigned ordinal){
#ifdef SSNE_COLLISION_TEST_PROBES
  if(checkpoint_probe)checkpoint_probe(ordinal);
#endif
  if((c.msr&0x8000u)&&g_irq_pending.load(std::memory_order_relaxed)){interrupted_at=ordinal;return false;}
  return true;
 }
 Decision predict(const PPCContext& c){
 interrupted_at=0;
 if(!valid||c.r[4]!=target||c.r[5]!=work||c.r[1]!=stack||c.r[13]!=sda||c.gqr[0])if(!initialize(c))return Decision::Unknown;
 const auto a=c.r[3];if(!ram(a,24)||intersects(a,24,stack-0x90,stack+8))return Decision::Unknown;
 source_flags=mem_read16(a);if(!(source_flags&target_flags&0xFC))return Decision::MaskReject;
 Vector ca={number(a+8),number(a+12),number(a+16)};
 if(transform_a&&!checkpoint(c,1))return Decision::Interrupted;
 if(transform_a){Vector transformed;for(unsigned row=0;row<3;++row){const auto* m=matrix_a.data()+row*4;const auto x=round_single(m[0]*ca[0]),y=round_single(m[1]*ca[1]);const auto lo=round_single(m[2]*ca[2]+x),hi=round_single(m[3]*1.+y);transformed[row]=round_single(lo+hi);if(row==1)result.matrix={float(m[0]),float(m[1]),float(m[2]),float(m[3]),float(x),float(y),float(lo),float(hi)};}ca=transformed;}
 if(transform_b&&!checkpoint(c,1+unsigned(transform_a)))return Decision::Interrupted;
 if(transform_b)result.matrix=matrix_b;
 if(!checkpoint(c,1+unsigned(transform_a)+unsigned(transform_b)))return Decision::Interrupted;
 Vector d;for(unsigned k=0;k<3;++k)d[k]=round_single(ca[k]-center_b[k]);
 const auto radius=round_single(number(a+4)*scale_a+radius_b),radius2=round_single(radius*radius);
 if(!checkpoint(c,2+unsigned(transform_a)+unsigned(transform_b)))return Decision::Interrupted;
 const auto yy=round_single(d[1]*ray[1]),zz=round_single(d[2]*ray[2]);
 const auto projection=round_single(round_single(d[0]*ray[0]+yy)+zz);
 if(!checkpoint(c,3+unsigned(transform_a)+unsigned(transform_b)))return Decision::Interrupted;
 const auto x2=round_single(d[0]*d[0]),y2=round_single(d[1]*d[1]);
 const auto distance2=round_single(round_single(d[2]*d[2]+x2)+y2);
 if(!std::isfinite(projection)||!std::isfinite(distance2)||!std::isfinite(radius2)||!std::isfinite(zero))return Decision::Unknown;
 auto reject=[&](double f0,double f1,double high,double f13){
  result.direct_a=!transform_a;result.transformed=transform_a||transform_b;
  result.scale_b=float(scale_b);result.tolerance=float(tolerance);result.speed=float(speed);
  for(unsigned k=0;k<3;++k){result.a[k]=float(ca[k]);result.b[k]=float(center_b[k]);result.d[k]=float(d[k]);result.ray[k]=float(ray[k]);result.scratch[k]=float(transform_b?center_b[k]:ca[k]);}
  result.radius2=float(radius2);result.projection=float(projection);result.yy=float(yy);result.zz=float(zz);
  result.xy=float(round_single(d[0]*ray[0]+yy));result.yz=float(round_single(d[1]*ray[1]+zz));
  result.f0=float(f0);result.f1=float(f1);result.f1high=float(high);result.f13=float(f13);
  return Decision::BoundsReject;
 };
 if(projection<zero&&distance2>radius2)return reject(zero,distance2,y2,scale_a);
 const auto discriminant=round_single(projection*projection+round_single(radius2-distance2));
 if(!std::isfinite(discriminant))return Decision::Unknown;
 if(discriminant<zero)return reject(round_single(radius2-distance2),discriminant,discriminant,zero);
 if(projection<zero){reject(round_single(radius2-distance2),discriminant,discriminant,zero);return Decision::Visit;}
 const auto time=round_single(tolerance/speed),delta=round_single(projection-time);
 if(!std::isfinite(time)||!std::isfinite(delta))return Decision::Unknown;
 if(delta>zero){const auto cutoff=round_single(delta*delta);if(!std::isfinite(cutoff))return Decision::Unknown;if(cutoff>discriminant)return reject(cutoff,discriminant,discriminant,zero);}
 reject(delta>zero?round_single(delta*delta):delta,discriminant,discriminant,zero);return Decision::Visit;
 }
};
void emit_math(PPCContext& c,uint32_t sp,const BoundResult& r,uint32_t node,uint32_t work){
   auto pair=[&](unsigned k,double lo,double hi){c.ps0[k]=lo;c.ps1[k]=hi;};
   auto both=[&](unsigned k,double v){pair(k,v,v);};
   for(unsigned k=0;k<3;++k){
    mem_write32(sp+8+k*4,f2u(r.a[k]));mem_write32(sp+0x18+k*4,f2u(r.b[k]));mem_write32(sp+0x28+k*4,f2u(r.d[k]));
    if(r.transformed)mem_write32(sp+0x38+k*4,f2u(r.scratch[k]));
   }
   both(0,r.f0);pair(1,r.f1,r.f1high);pair(2,r.yy,r.zz);pair(3,r.xy,r.yz);
   pair(4,r.ray[0],r.ray[1]);pair(5,r.d[0],r.d[1]);pair(6,r.d[0],r.d[1]);pair(7,r.d[2],0.);
   if(r.transformed){pair(8,r.matrix[0],r.matrix[1]);pair(9,r.matrix[2],r.matrix[3]);pair(10,r.matrix[4],r.matrix[5]);pair(11,r.matrix[6],r.matrix[7]);}
   both(12,r.scale_b);both(13,r.f13);
   c.ps1[28]=r.tolerance;c.ps1[29]=r.speed;c.ps1[31]=r.radius2;
   c.r[3]=sp+0x28;c.r[4]=work+0x18;c.r[5]=sp+0x28;c.r[9]=0x80220000;
   if(r.direct_a)c.r[11]=node+8;
}
void visit(PPCContext& c,const Inputs& input){
 const auto node=c.r[3],target=c.r[4],work=c.r[5];
 native_collision_pair::Frame frame(c);
 const auto& r=input.result;emit_math(c,c.r[1],r,node,work);
 c.r[23]=c.r[1]+8;c.r[24]=input.target_flags;c.r[25]=input.source_flags;
 c.r[26]=input.model_a;c.r[27]=input.model_b;c.r[28]=work;c.r[29]=node;c.r[30]=work+0x18;c.r[31]=target;
 c.ps0[28]=r.tolerance;c.ps0[29]=r.speed;c.ps0[30]=r.projection;c.ps0[31]=r.radius2;
 c.r[0]=1;cr0_update(c,1);c.lr=0x8015DD2C;
 native_collision_pair::after_bounds(c);frame.finish();
}
struct Writes{
 uint32_t registers=0,floats=0;bool cr0=false,link=false;
 void add(uint32_t r,uint32_t f,bool cr,bool lr){registers|=r;floats|=f;cr0|=cr;link|=lr;}
 void clear(){registers=floats=0;cr0=link=false;}
};
struct Pending{
 uint32_t args[3],callee_saved[6],caller;
 explicit Pending(const PPCContext& c):caller(c.lr){
  for(unsigned k=0;k<3;++k)args[k]=c.r[k+3];for(unsigned k=0;k<6;++k)callee_saved[k]=c.r[k+26];
 }
};
struct Batch{
 std::optional<Pending> math,any;
 Inputs inputs;
 BoundResult pending_result;
 Writes after_any;uint32_t floats_after_math=0;
 explicit Batch(PPCContext&){if(statistics)++totals.parents;}
 void writes(uint32_t r,uint32_t f=0,bool cr=false,bool=false,bool=false,bool lr=false){
  if(math||any){after_any.add(r,f,cr,lr);floats_after_math|=f;}
 }
 void materialize(PPCContext& c,const Pending& p,bool bounds){
  for(unsigned k=0;k<3;++k)c.r[k+3]=p.args[k];for(unsigned k=0;k<6;++k)c.r[k+26]=p.callee_saved[k];c.lr=p.caller;
  // Emit the exact completed root frame from typed values. This performs no
  // guest calls, input rereads or interrupt delivery; the parent retains its
  // original legal poll. Unlike v141, no original rejection is replayed.
  const auto sp=c.r[1]-0x90;
  mem_write32(sp,c.r[1]);
  for(unsigned k=23;k<32;++k)mem_write32(sp+0x4C+(k-23)*4,c.r[k]);
  for(unsigned k=28;k<32;++k)mem_write64(sp+0x70+(k-28)*8,d2u(c.ps0[k]));
  mem_write32(sp+0x94,p.caller);
  if(bounds){const auto& r=pending_result;
   emit_math(c,sp,r,p.args[0],p.args[2]);
  }else c.r[9]=0;
  cr0_update(c,0);c.r[0]=p.caller;c.lr=p.caller;
  if(statistics)++totals.materialized;
 }
 void flush(PPCContext& c){
  if(!math&&!any)return;const auto parent=c;
  if(math)materialize(c,*math,true);if(any)materialize(c,*any,false);
  // Parent nonvolatile registers are maintained throughout the native loop.
  for(unsigned k=26;k<32;++k)c.r[k]=parent.r[k];
  for(unsigned k=0;k<26;++k)if(after_any.registers&(1u<<k))c.r[k]=parent.r[k];
  const auto floats=math?floats_after_math:after_any.floats;
  for(unsigned k=0;k<32;++k)if(floats&(1u<<k)){c.ps0[k]=parent.ps0[k];c.ps1[k]=parent.ps1[k];}
  if(after_any.cr0)c.cr[0]=parent.cr[0];
  // A pending call sets LR itself; every later caller assignment is tracked.
  if(after_any.link)c.lr=parent.lr;
  math.reset();any.reset();after_any.clear();floats_after_math=0;if(statistics)++totals.flushed;
 }
 void call(PPCContext& c,uint32_t caller){
  c.lr=caller;after_any.link=true;
  if((c.msr&0x8000u)&&g_irq_pending.load(std::memory_order_relaxed)){flush(c);inputs.valid=false;static const auto fn=ssne_find_func(0x8015DBC0)->func;if(statistics){++totals.executed;++totals.blocked_irq;}fn(c);return;}
  const auto decision=inputs.predict(c);if(statistics){++totals.predictions;if(decision==Decision::MaskReject)++totals.mask;else if(decision==Decision::BoundsReject)++totals.bounds;else if(decision==Decision::Visit)++totals.visits;else if(decision==Decision::Interrupted)++totals.interrupted;else++totals.unknown;}
  if(decision==Decision::Interrupted){const auto stage=inputs.interrupted_at;flush(c);inputs.valid=false;c.lr=caller;native_collision_pair::resume_at_sdk(c,stage);return;}
  if(decision==Decision::Unknown){flush(c);inputs.valid=false;static const auto fn=ssne_find_func(0x8015DBC0)->func;c.lr=caller;if(statistics)++totals.executed;fn(c);return;}
  if(decision==Decision::Visit){flush(c);inputs.valid=false;c.lr=caller;if(statistics)++totals.native_visits;visit(c,inputs);return;}
  if(decision==Decision::BoundsReject){math.emplace(c);pending_result=inputs.result;any.reset();floats_after_math=0;}else any.emplace(c);
  after_any.clear();
 }
 void poll(PPCContext& c){if((c.msr&0x8000u)&&g_irq_pending.load(std::memory_order_relaxed)){flush(c);inputs.valid=false;safepoint(c);}}
 void finish(PPCContext& c){flush(c);}
};
}
