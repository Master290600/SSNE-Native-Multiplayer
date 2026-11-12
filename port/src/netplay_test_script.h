// Optional developer fixture. All spawns use the same host queue as the mods UI.
// Times are completed world ticks, so local loading/render speed does not affect actions.
#pragma once
#include "netplay.h"
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
namespace port::netplay {
class TestScript {
    struct Spawn { unsigned at, id, count, distance; };
    struct Pad { unsigned first, last; std::string name; };
    std::vector<Spawn> spawns;
    std::vector<Pad> pads;
    size_t next=0;
    template<class F> static void entries(const char* env, F fn) {
        const char* value=std::getenv(env);if(!value)return;
        std::string spec(value);
        for(size_t pos=0;pos<spec.size();) {
            auto end=spec.find(',',pos);fn(spec.substr(pos,end==std::string::npos?end:end-pos));
            if(end==std::string::npos)break;pos=end+1;
        }
    }
public:
    TestScript() {
        entries("SSNE_NETPLAY_TEST_SPAWNS",[&](const std::string& text) {
            Spawn s{};char extra;
            if(std::sscanf(text.c_str(),"%u:%u:%u:%u%c",&s.at,&s.id,&s.count,&s.distance,&extra)==4)spawns.push_back(s);
        });
        std::stable_sort(spawns.begin(),spawns.end(),[](auto&a,auto&b){return a.at<b.at;});
        entries("SSNE_NETPLAY_TEST_PAD",[&](const std::string& text) {
            unsigned first,last;char name[24]{};
            if(std::sscanf(text.c_str(),"%u:%u:%23s",&first,&last,name)==3 && last>first)pads.push_back({first,last,name});
        });
    }
    void update(Session& session,bool host,unsigned ready_ticks) {
        if(host)while(next<spawns.size() && spawns[next].at<=ready_ticks) {
            const auto& s=spawns[next++];session.submit_spawn(s.id,s.count,s.distance);
        }
        if(pads.empty())return;
        ssne::PadState pad;double yaw=0;bool active=false;
        for(const auto& p:pads)if(ready_ticks>=p.first && ready_ticks<p.last) {
            active=true;
            if(p.name=="YAW_RIGHT")yaw+=.02;
            if(p.name=="YAW_LEFT")yaw-=.02;
            if(p.name=="CLEFT")pad.cstick_x=0;
            if(p.name=="CRIGHT")pad.cstick_x=255;
            if(p.name=="SUP")pad.stick_y=255;
            if(p.name=="SDOWN")pad.stick_y=0;
            if(p.name=="SLEFT")pad.stick_x=0;
            if(p.name=="SRIGHT")pad.stick_x=255;
            if(p.name=="FIRE") {pad.trigger_r=255;pad.buttons|=ssne::PAD_R;}
            if(p.name=="A")pad.buttons|=ssne::PAD_A;
        }
        if(!active)return;
        session.submit_pad(pad);if(yaw!=0)session.submit_mouse(yaw,0);
    }
};
}
