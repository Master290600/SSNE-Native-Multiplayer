// EU801BABB0: squared distance between two three-component vectors.
// Return value and all paired-lane byproducts retain the original rounding.
namespace native_distance_squared {
constexpr uint32_t address=0x801BABB0;
PPCFunc original=nullptr;
bool ram(uint32_t p){return uint32_t(p-0x80000000u)<=kRamSize-12;}
bool inputs(const PPCContext& c,std::array<double,6>& v){
 if(c.gqr[0]||!ram(c.r[3])||!ram(c.r[4]))return false;
 for(unsigned k=0;k<6;++k){
  const auto p=c.r[k<3?3:4]+(k%3)*4;uint32_t bits;std::memcpy(&bits,g_ram+uint32_t(p-0x80000000u),4);bits=SSNE_BSWAP32(bits);
  if((bits&0x7F800000u)==0x7F800000u)return false;v[k]=double(u2f(bits));
 }return true;
}
void kernel(PPCContext& c,const std::array<double,6>& v){
 const double dx=round_single(v[0]-v[3]),dy=round_single(v[1]-v[4]),dz=round_single(v[2]-v[5]);
 const double yy=round_single(dy*dy),zz=round_single(dz*dz);
 const double xy=round_single(dx*dx+yy);
 c.ps0[0]=dx;c.ps1[0]=dy;c.ps0[2]=yy;c.ps1[2]=zz;
 c.ps0[1]=round_single(xy+zz);c.ps1[1]=round_single(zz);
}
void hook(PPCContext& c){
 if(g_irq_pending.load(std::memory_order_relaxed))safepoint(c);
 std::array<double,6> v;if(!inputs(c,v)){ssne_call_original(c,address,original);return;}kernel(c,v);
}
void install(){
 const auto env=std::getenv("SSNE_NATIVE_DISTANCE_SQUARED");if(env&&!std::strcmp(env,"0"))return;
 const auto entry=ssne_find_func(address);if(!entry||mem_read32(0x80000000)!=0x47334250u||*ssne_hook_slot(address))return;
 uint64_t h=14695981039346656037ull;for(unsigned k=0;k<40;++k){h^=mem_read8(address+k);h*=1099511628211ull;}
 if(h!=0xD7565A651AB8688Eull)return;
 original=entry->func;ssne_install_hook(address,hook);
}
}
