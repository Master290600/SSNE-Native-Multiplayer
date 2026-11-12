#include "world_state.h"
#include <random>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <limits>
#include <memory>
using namespace port::coop;
namespace port::coop {bool reference_apply_delta(WorldState&,const WorldDelta&,std::string&);}
void require(bool ok,const char* why){if(!ok){std::cerr<<why<<std::endl;std::_Exit(1);}}
std::vector<uint8_t> bytes(const WorldState& w){return encode_delta(difference(nullptr,w));}
WorldState world(unsigned count){WorldState w;w.epoch=3;w.revision=10;w.tick=90;w.level="Rlevel1_2";w.mission["checkpoint"]=int64_t(4);
 for(unsigned i=0;i<count;++i){EntityState e;e.id=i+1;e.kind=EntityKind::Enemy;e.definition=i%20;e.resource="actors/Enemy";e.properties={{"health",100.f},{"position",std::array<float,3>{float(i),2,3}},{"rotation",std::array<float,4>{0,0,0,1}},{"target",EntityReference{1}},{"name",std::string("enemy")},{"ammo",std::vector<int64_t>{1,2,3}},{"weights",std::vector<float>{.1f,.2f}},{"mode",int64_t(2)}};w.entities.emplace(e.id,std::move(e));}return w;}
int main(){
 unsigned good=0,bad=0;
 for(unsigned id=1;id<=2400;++id){
  auto baseline=world(id%31+2);auto next=baseline;++next.revision;next.tick+=id%7;next.entities.at(1).properties["health"]=float(id);next.entities.at(2).properties["position"]=std::array<float,3>{float(id),0,5};
  if(id%3==0)next.entities.erase(2);if(id%5==0){auto e=next.entities.at(1);e.id=100;next.entities.emplace(100,std::move(e));}
  if(id%7==0){++next.epoch;next.revision=1;next.tick=0;next.level="Rlevel1_4a";}
  auto delta=difference(id%7?&baseline:nullptr,next);
  switch(id%12){case 0:delta.epoch=0;break;case 1:delta.revision=0;break;case 2:delta.base_revision=999;delta.full=false;break;case 3:delta.tick=0;delta.epoch=baseline.epoch;break;case 4:delta.mission.set["illegal\nkey"]=int64_t(1);break;case 5:delta.mission.set["badfloat"]=std::numeric_limits<float>::infinity();break;case 6:delta.remove.push_back(99999);break;case 7:delta.level=std::string(257,'X');break;default:break;}
  const auto old_bytes=bytes(baseline);std::string a="previous",b="previous",c="previous";
  auto expected=baseline;const bool accepted=reference_apply_delta(expected,delta,a);const auto expected_bytes=bytes(expected);
  auto prepared=apply_delta_copy(baseline,delta,b);auto actual=baseline;const bool committed=apply_delta(actual,delta,c);
  if(bool(prepared)!=accepted||committed!=accepted||a!=b||a!=c||bytes(baseline)!=old_bytes||bytes(actual)!=expected_bytes||(prepared&&bytes(*prepared)!=expected_bytes)){
   std::cerr<<"FAIL id="<<id<<" accepted="<<accepted<<" prepared="<<bool(prepared)<<" errors="<<a<<'/'<<b<<'/'<<c<<std::endl;std::_Exit(1);
  }
  if(accepted)++good;else{++bad;require(expected_bytes==old_bytes,"invalid original delta mutated state");}
 }
 require(good&&bad,"both valid/invalid coverage");
 // The old client clones the immutable baseline before apply_delta clones it
 // again. The prepared path returns its one validated copy directly.
 auto baseline=world(500),next=baseline;++next.revision;++next.tick;next.entities.at(1).properties["health"]=42.f;auto delta=difference(&baseline,next);std::array<std::vector<double>,2> times;
 for(unsigned pass=0;pass<22;++pass)for(unsigned step=0;step<2;++step){const unsigned mode=(pass+step)%2;std::string error;const auto start=std::chrono::steady_clock::now();
  if(mode){auto prepared=apply_delta_copy(baseline,delta,error);require(bool(prepared),"prepared benchmark");auto snapshot=std::make_shared<WorldState>(std::move(*prepared));require(snapshot->revision==next.revision,"prepared revision");}
  else{auto snapshot=std::make_shared<WorldState>(baseline);require(reference_apply_delta(*snapshot,delta,error),"reference benchmark");require(snapshot->revision==next.revision,"reference revision");}
  const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();if(pass>=2)times[mode].push_back(us);
 }
 for(auto& t:times)std::sort(t.begin(),t.end());std::cout<<"PASS 2400 delta-copy comparisons;valid="<<good<<" rejected="<<bad<<" exact world/wire/error strings,immutable baselines and atomic failure"<<std::endl;
 std::cout<<"COPY_BENCH entities=500 old_client_median_us="<<(times[0][9]+times[0][10])/2<<" prepared_median_us="<<(times[1][9]+times[1][10])/2<<" scope=component_only"<<std::endl;
}
