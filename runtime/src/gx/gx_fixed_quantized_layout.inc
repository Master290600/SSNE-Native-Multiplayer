// The dominant captured quantized layout: f32 position/normal, two RGBA8
// colors, three f32 UV pairs and one u16 UV pair. Dispatch once per primitive.
namespace fixed_layout173 {
inline bool overlap(const void* a,size_t an,const void* b,size_t bn){
 const auto x=reinterpret_cast<uintptr_t>(a),y=reinterpret_cast<uintptr_t>(b);
 return x<=y?y-x<an:x-y<bn;
}
inline uint32_t index16(const uint8_t* p){return uint32_t(p[0])<<8|p[1];}
inline uint32_t word(const uint8_t* p){uint32_t x;std::memcpy(&x,p,4);return SSNE_BSWAP32(x);}
struct Source {const uint8_t* base;uint32_t step,offset;};
inline const uint8_t* indexed16(const Source& s,const uint8_t* p){return s.base+index16(p+s.offset)*s.step;}
bool decode(const PackedVertexPlan& plan,const uint8_t* data,uint32_t count,uint8_t* dest,size_t size){
 if(plan.common_format||plan.inputs!=4||plan.stride!=100||plan.position_matrix_stream||count<3||size_t(count)*100!=size)return false;
 if(overlap(dest,size,&plan,sizeof(plan))||overlap(dest,size,ssne::g_ram,ssne::kRamSize)||overlap(dest,size,data,size_t(count)*plan.input_stride))return false;
 const auto& a=plan.attributes;
 if(a[0].type!=3||a[0].fmt!=4||a[0].n!=3||a[1].type!=3||a[1].fmt!=4)return false;
 for(unsigned k=2;k<4;++k)if(a[k].type!=2||a[k].fmt!=5)return false;
 for(unsigned k=4;k<7;++k)if(a[k].type!=3||a[k].fmt!=4||a[k].n!=2)return false;
 if(a[7].type!=3||a[7].fmt!=2||a[7].n!=2)return false;
 for(unsigned k=0;k<4;++k)if(plan.tex_matrix_offset[k]>=0)return false;
 Source sources[8];
 for(unsigned k=0;k<8;++k){
  if(a[k].array!=k||a[k].offset>=plan.input_stride||((k<2||k>=4)&&plan.input_stride-a[k].offset<2))return false;
  // The prepared plan already proves every indexed input is cached RAM.
  sources[k]={ssne::g_ram+plan.base[k],plan.step[k],a[k].offset};
 }
 const auto input_stride=plan.input_stride;const float matrix=float(plan.position_matrix),scale=a[7].scale;
 uint32_t tex_matrix[4];std::memcpy(tex_matrix,plan.tex_matrix,16);
 ssne::perf::detail::Scope timer(ssne::perf::detail::VertexDecode);
 ssne::perf::detail::count(ssne::perf::detail::Vertices,count);
 ssne::perf::detail::Scope family(ssne::perf::detail::VertexFixed);
 ssne::perf::detail::count(ssne::perf::detail::FixedVertices,count);
 for(unsigned v=0;v<count;++v,data+=input_stride,dest+=100){
  const auto* p=indexed16(sources[0],data);uint32_t pos[4]={word(p),word(p+4),word(p+8),0x3F800000u};
  std::memcpy(dest,pos,16);std::memcpy(dest+84,pos,12);std::memcpy(dest+96,&matrix,4);
  p=indexed16(sources[1],data);const uint32_t normal[3]={word(p),word(p+4),word(p+8)};std::memcpy(dest+72,normal,12);
  for(unsigned k=2;k<4;++k){const auto& s=sources[k];std::memcpy(dest+16+(k-2)*4,s.base+data[s.offset]*s.step,4);}
  for(unsigned k=0;k<3;++k){p=indexed16(sources[4+k],data);const uint32_t uv[3]={word(p),word(p+4),tex_matrix[k]};std::memcpy(dest+24+k*12,uv,12);}
  p=indexed16(sources[7],data);float uv[3]={float(index16(p))*scale,float(index16(p+2))*scale,0};
  std::memcpy(uv+2,tex_matrix+3,4);std::memcpy(dest+60,uv,12);
 }
 return true;
}
}
