#include "native_replica.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
using namespace port::coop::presentation_detail;
namespace {
constexpr uint32_t base=0x80001000u,ram_end=base+1024;
using Ram=std::array<uint8_t,1024>;
struct Pose {std::vector<float> visual;};
void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
void store(Ram& ram,uint32_t address,uint32_t value){require(address>=base&&uint64_t(address)+4<=ram_end,"Oracle write out of range");for(unsigned k=0;k<4;++k)ram[address-base+k]=uint8_t(value>>(24-k*8));}
std::vector<VisualWrite> oracle(const std::vector<VisualWrite>& input){std::map<uint32_t,VisualWrite> map;for(const auto& w:input){auto [it,fresh]=map.emplace(w.address,w);if(!fresh&&w.replace)it->second=w;}std::vector<VisualWrite> out;for(const auto& [a,w]:map)out.push_back(w);return out;}
Ram commit(Ram ram,const std::vector<VisualWrite>& words){for(const auto& w:words)store(ram,w.address,w.value);return ram;}
struct Plan {
 std::vector<VisualWrite> full,words;std::vector<NativeBoneSpan> spans;std::vector<Pose> poses;
 void word(uint32_t address,uint32_t value,bool replace=true){VisualWrite w{address,value,uint32_t(full.size()),replace};full.push_back(w);words.push_back(w);}
 void bone(uint32_t address,std::vector<float> values){spans.push_back({address,uint32_t(values.size()),uint32_t(full.size()),poses.size()});for(unsigned i=0;i<values.size();++i)full.push_back({address+i*4,std::bit_cast<uint32_t>(values[i]),uint32_t(full.size()),true});poses.push_back({std::move(values)});}
};
Ram initial(){Ram ram;for(unsigned i=0;i<ram.size();++i)ram[i]=uint8_t(i*73+19);return ram;}
void check(Plan plan,bool fast){
 std::vector<BoneWriteRange> ranges;for(const auto& w:plan.words)ranges.push_back({w.address,4});
 require(disjoint_bone_spans(plan.spans,ranges,ram_end)==fast,"Wrong span alias/range decision");
 const auto before=initial(),expected=commit(before,oracle(plan.full));
 for(bool reuse:{false,true}){
  auto words=plan.words;require(expand_bone_words(words,plan.spans,plan.poses),"Fallback expansion failed");normalize_visual_plan(words,reuse);require(commit(before,words)==expected,"Alias fallback differs from independent old map/byte RAM oracle");
  auto restored=commit(before,words);for(const auto& w:words)for(unsigned k=0;k<4;++k)restored[w.address-base+k]=before[w.address-base+k];require(restored==before,"Word fallback did not restore exact overlapped bytes");
 }
 if(fast){
  auto words=plan.words;normalize_visual_plan(words,true);auto actual=commit(before,words);
  std::vector<SavedBoneSpan> saved;
  for(const auto& span:plan.spans){SavedBoneSpan raw{span.address,{before.begin()+span.address-base,before.begin()+span.address-base+span.count*4}};std::vector<float> original;std::vector<uint8_t> packed;
   require(prepare_bone_bytes(raw.original,plan.poses[span.pose_index].visual,original,packed),"Valid packing rejected");
   for(unsigned i=0;i<original.size();++i){uint32_t expected_bits=0;for(unsigned k=0;k<4;++k)expected_bits=(expected_bits<<8)|raw.original[i*4+k];require(std::bit_cast<uint32_t>(original[i])==expected_bits,"Original registry pose lost raw float bits");}
   std::copy(packed.begin(),packed.end(),actual.begin()+span.address-base);saved.push_back(std::move(raw));
  }
  require(actual==expected,"Span fastpath differs from byte RAM oracle");
  for(const auto& w:words)for(unsigned k=0;k<4;++k)actual[w.address-base+k]=before[w.address-base+k];
  for(const auto& span:saved)std::copy(span.original.begin(),span.original.end(),actual.begin()+span.address-base);
  require(actual==before,"Raw span plus scalar backup failed exact restoration");
 }
}
}
int main(){
 Plan empty;check(empty,true);
 Plan separate;separate.word(base,0x11223344);separate.bone(base+64,{1.f,-0.f,-2.5f});separate.word(base+120,0x55667788,false);separate.bone(base+76,{.125f,10001.f});check(separate,true); // adjacent spans, post-write registry rejection threshold retained
 for(unsigned offset=0;offset<4;++offset){Plan p;p.word(base+64+offset,0xaabbccdd);p.bone(base+64,{1.f,2.f,3.f});p.word(base+64+offset,0x10203040);check(p,false);}
 Plan stamps;stamps.word(base+60,0xdeadbeef);stamps.bone(base+64,{1.f,2.f,3.f});stamps.word(base+68,0xdead0000,false);stamps.bone(base+68,{-0.f,5.f});stamps.word(base+72,0x12345678,false);check(stamps,false);
 for(unsigned offset:{0u,1u,2u,3u,4u,7u,8u,11u}){Plan p;p.bone(base+80,{1.f,2.f,3.f});p.bone(base+80+offset,{4.f,5.f});check(p,false);}
 std::array<NativeBoneSpan,1> one{{{base+64,3,0,0}}};
 for(unsigned width:{1u,2u,4u})for(uint32_t address:{base+64-width+1,base+64,base+75,base+76-width}){std::array<BoneWriteRange,1> r{{{address,width}}};require(!disjoint_bone_spans(one,r,ram_end),"Partial1/2/4-byte overlap missed");}
 for(unsigned width:{1u,2u,4u})for(uint32_t address:{base+64-width,base+76}){std::array<BoneWriteRange,1> r{{{address,width}}};require(disjoint_bone_spans(one,r,ram_end),"Adjacent scalar bytes treated as overlap");}
 require(!disjoint_bone_spans(std::array<NativeBoneSpan,1>{{{0xfffffffcu,2,0,0}}},{},ram_end),"Wrapped span accepted");
 require(!disjoint_bone_spans(std::array<NativeBoneSpan,1>{{{ram_end-4,2,0,0}}},{},ram_end),"RAM-end crossing accepted");
 require(disjoint_bone_spans(std::array<NativeBoneSpan,1>{{{ram_end-4,1,0,0}}},{},ram_end),"Exact RAM-end span rejected");
 uint64_t ordinal=0;require(reserve_visual_ordinals(ordinal,3)&&ordinal==3,"Logical span ordinal reservation failed");ordinal=UINT32_MAX;require(!reserve_visual_ordinals(ordinal,1)&&ordinal==UINT32_MAX,"Ordinal overflow accepted");ordinal=uint64_t(UINT32_MAX)+1;require(!reserve_visual_ordinals(ordinal,0),"Invalid initial ordinal underflow accepted");
 std::vector<float> values{0.f,-0.f,std::bit_cast<float>(1u),10001.f,1000000.f};require(valid_bone_values(values),"Old finite render thresholds changed");
 for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),1000000.125f}){auto bad=values;bad.back()=invalid;require(!valid_bone_values(bad),"Late invalid coefficient accepted");}
 std::mt19937 random(0x424f4e45u);for(unsigned trial=0;trial<300;++trial){Plan p;for(unsigned i=0;i<24;++i){const auto a=base+32+unsigned(random()%120);if(random()%3)p.word(a,uint32_t(random()),random()%3!=0);else{std::vector<float> f(1+random()%5);for(auto& value:f)value=float(int(random()%200)-100)/8;p.bone(a,std::move(f));}}std::vector<BoneWriteRange> ranges;for(const auto& w:p.words)ranges.push_back({w.address,4});const bool disjoint=disjoint_bone_spans(p.spans,ranges,ram_end);check(std::move(p),disjoint);}
 std::cout<<"PASS render bone spans: independent old map/byte oracle, partial/scalar/stamp/span aliases, original ordinals, adjacent/end ranges, exact raw restoration, float bits and late-invalid validation\n";
}
