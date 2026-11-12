#pragma once
#include <array>
#include <vector>
#include <map>
#include <optional>
#include <queue>
#include <cmath>
#include <algorithm>
#include <functional>
namespace port::coop {
using BotPoint=std::array<float,3>;
inline float bot_distance(const BotPoint& a,const BotPoint& b){return std::hypot(a[0]-b[0],a[2]-b[2])+std::abs(a[1]-b[1])*2;}
// Incremental A*: every edge is supplied by the native swept-volume and floor
// queries. A bounded probe budget keeps navigation off the rendering hot path.
class BotNavigation {
    struct Node {BotPoint p;float g;unsigned parent,next=0;bool closed=false;};
    struct Entry {float f;unsigned i;bool operator<(const Entry& b)const{return f>b.f;}};
    std::vector<Node> nodes_;
    std::map<std::array<int,3>,unsigned> cells_;
    std::priority_queue<Entry> open_;
    BotPoint target_{},last_{};unsigned best_=0,cursor_=0;bool solved_=false;float cell_=2.5f;std::vector<BotPoint> path_;
    auto key(const BotPoint& p)const{return std::array<int,3>{int(std::lround(p[0]/cell_)),int(std::lround(p[1]*2)),int(std::lround(p[2]/cell_))};}
    void begin(const BotPoint& start,const BotPoint& goal){nodes_={{start,0,0}};cells_.clear();cells_[key(start)]=0;open_={};target_=goal;last_=start;best_=0;cursor_=0;solved_=false;path_.clear();open_.push({bot_distance(start,goal),0});}
public:
    size_t explored()const{return nodes_.size();}
    BotPoint step(const BotPoint& start,const BotPoint& goal,
        const std::function<std::optional<BotPoint>(const BotPoint&,const BotPoint&)>& probe,unsigned budget=8){
        if(nodes_.empty() || bot_distance(goal,target_)>3.f || (!solved_ && bot_distance(start,nodes_[0].p)>12.f))begin(start,goal);
        const auto previous=last_;last_=start;
        if(!budget)return start;
        --budget; // reserve one edge query for the current-to-waypoint shortcut
        unsigned reached=best_;
        while(!solved_ && budget && !open_.empty() && nodes_.size()<2048){
            const auto i=open_.top().i;
            if(nodes_[i].closed){open_.pop();continue;}
            if(bot_distance(nodes_[i].p,target_)<2.f){best_=i;solved_=true;break;}
            if(nodes_[i].next==8){nodes_[i].closed=true;open_.pop();continue;}
            const auto n=nodes_[i].next++;--budget;auto desired=nodes_[i].p;
            static constexpr int directions[8][2]={{1,0},{0,1},{-1,0},{0,-1},{1,1},{-1,1},{-1,-1},{1,-1}};
            desired[0]+=directions[n][0]*cell_;desired[2]+=directions[n][1]*cell_;
            const auto p=probe(nodes_[i].p,desired);if(!p)continue;
            const auto k=key(*p);const auto cost=nodes_[i].g+bot_distance(nodes_[i].p,*p);
            auto found=cells_.find(k);
            if(found!=cells_.end()){
                auto& old=nodes_[found->second];if(cost>=old.g-.01f)continue;
                old.g=cost;old.parent=i;old.closed=false;old.next=0;open_.push({cost+bot_distance(old.p,target_),found->second});continue;
            }
            const auto index=unsigned(nodes_.size());nodes_.push_back({*p,cost,i});cells_[k]=index;
            if(bot_distance(*p,target_)<bot_distance(nodes_[best_].p,target_))best_=index;
            open_.push({cost+bot_distance(*p,target_),index});
        }
        if(!solved_ && !open_.empty() && nodes_.size()<2048)return start;
        if(path_.empty()){
            reached=best_;for(unsigned n=0;reached && n<nodes_.size();++n){path_.push_back(nodes_[reached].p);reached=nodes_[reached].parent;}
            std::reverse(path_.begin(),path_.end());cursor_=0;
        }
        auto reached_point=[&](const BotPoint& point){
            if(bot_distance(start,point)<cell_*.45f)return true;
            const float dx=start[0]-previous[0],dz=start[2]-previous[2],length2=dx*dx+dz*dz;
            if(length2<.001f || length2>25.f)return false;
            const auto t=std::clamp(((point[0]-previous[0])*dx+(point[2]-previous[2])*dz)/length2,0.f,1.f);
            return std::hypot(point[0]-previous[0]-t*dx,point[2]-previous[2]-t*dz)<cell_*.3f && std::abs(point[1]-(previous[1]+t*(start[1]-previous[1])))<.5f;
        };
        while(cursor_<path_.size() && reached_point(path_[cursor_]))++cursor_;
        if(cursor_<path_.size()){
            if(auto safe=probe(start,path_[cursor_]))return *safe;
            begin(start,goal);return start;
        }
        if(bot_distance(start,goal)<3.f)if(auto safe=probe(start,goal))return *safe;
        if(!solved_)cell_=1.25f; // retry narrow passages with a finer grid
        nodes_.clear();return start;
    }
};
inline unsigned bot_weapon_pickup(unsigned type){return type>=35 && type<=45?type-35:type==55?11:255;}
inline float bot_pickup_value(unsigned type,float health,float armor,bool owns,bool low_ammo){
    if(type<=4)return health<100?(100-health)*1.5f:0;
    if(type<=9)return armor<100?(100-armor)*.6f:0;
    if(bot_weapon_pickup(type)<12)return owns?(low_ammo?35.f:0.f):85.f;
    if(type>=17 && type<=34)return low_ammo?65.f:8.f;
    if(type>=10 && type<=16)return 35.f;
    return 0;
}
}

