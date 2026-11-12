#include "authoritative_session.h"
#include <iostream>
using namespace port::coop;
int main(){AuthoritativeServer host(1);if(!host.start(0,"123456",1234))return 2;WorldState w;w.level="Rlevel1_2";w.tick=1;EntityState e;e.id=1;e.kind=EntityKind::Enemy;e.properties["health"]=100.f;w.entities.emplace(1,std::move(e));auto& alias=w.entities.at(1).properties.at("health");std::string error;if(!host.publish(std::move(w),error))return 3;auto retained=host.latest_world();const auto before=encode_delta(difference(nullptr,*retained));alias=7.f;const bool unchanged=before==encode_delta(difference(nullptr,*retained));host.stop();std::cout<<(unchanged?"PASS":"FAIL")<<" moved-input mutable alias cannot mutate published snapshot"<<std::endl;return unchanged?0:1;}
