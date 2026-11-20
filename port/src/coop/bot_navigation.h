#pragma once
#include <array>
#include <cstdint>
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
    struct Node {BotPoint p;float g;unsigned parent,next=0,blocked=0;bool closed=false;};
    struct Entry {float f;unsigned i;bool operator<(const Entry& b)const{return f>b.f;}};
    std::vector<Node> nodes_;
    std::map<std::array<int64_t,3>,unsigned> cells_;
    std::priority_queue<Entry> open_;
    BotPoint target_{},last_{},path_goal_{},advance_target_{};
    unsigned best_=0,cursor_=0,tip_=0,search_age_=0,stalled_=0,restarts_=0;
    bool solved_=false,advancing_=false;
    float cell_=1.25f;
    std::vector<BotPoint> path_;
    static bool valid(const BotPoint& p){return std::all_of(p.begin(),p.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1e9f;});}
    auto key(const BotPoint& p)const{return std::array<int64_t,3>{std::llround(p[0]/(cell_*.5f)),std::llround(p[1]*2),std::llround(p[2]/(cell_*.5f))};}
    void begin(const BotPoint& start,const BotPoint& goal){
        ++restarts_;nodes_={{start,0,0}};cells_.clear();cells_[key(start)]=0;open_={};target_=goal;last_=start;
        best_=cursor_=tip_=search_age_=stalled_=0;solved_=advancing_=false;path_.clear();open_.push({bot_distance(start,goal),0});
    }
    void retarget(const BotPoint& goal){
        target_=goal;best_=0;
        // Keep explored geometry when the human moves. Reweighting the bounded
        // frontier does not spend another native collision/floor query.
        open_={};
        for(unsigned i=0;i<nodes_.size();++i){
            if(bot_distance(nodes_[i].p,goal)<bot_distance(nodes_[best_].p,goal))best_=i;
            if(!nodes_[i].closed)open_.push({nodes_[i].g+bot_distance(nodes_[i].p,goal),i});
        }
    }
public:
    struct State {
        size_t nodes,open,path,cursor;
        unsigned best,tip,age,restarts;
        bool solved,advancing;
        BotPoint root,best_position,next,frontier,goal;
    };
    State state()const{
        return {nodes_.size(),open_.size(),path_.size(),cursor_,best_,tip_,search_age_,restarts_,solved_,advancing_,
            nodes_.empty()?last_:nodes_.front().p,nodes_.empty()?last_:nodes_[best_].p,
            cursor_<path_.size()?path_[cursor_]:last_,open_.empty()?last_:nodes_[open_.top().i].p,target_};
    }
    size_t explored()const{return nodes_.size();}
    BotPoint step(const BotPoint& start,const BotPoint& requested_goal,
        const std::function<std::optional<BotPoint>(const BotPoint&,const BotPoint&)>& probe,unsigned budget=8,bool goal_supported=true){
        if(!valid(start) || !valid(requested_goal) || !probe || !budget)return start;
        // The native caller owns support/floor evidence. During a jump, retain
        // the last supported goal and the checked detour instead of chasing Y,
        // reweighting every frontier, or clearing a solved route. A new follower
        // with no grounded target waits; it cannot invent a landing position.
        if(!goal_supported && nodes_.empty())return start;
        const auto goal=goal_supported?requested_goal:target_;
        if(nodes_.empty() || bot_distance(start,last_)>12.f || (!path_.empty() && bot_distance(goal,path_goal_)>8.f))begin(start,goal);
        const auto previous=last_;last_=start;
        // A bot can slide sideways along a wall without approaching its
        // waypoint. Measure progress along the route, not speed alone.
        const float remaining=bot_distance(start,advance_target_);
        if(advancing_ && remaining>.18f && bot_distance(previous,advance_target_)-remaining<.055f)
            ++stalled_;
        else
            stalled_=0;
        if(stalled_>=12){cell_=.625f;begin(start,goal);}
        if(bot_distance(goal,target_)>.5f){
            // Finish a validated route prefix instead of resetting it whenever
            // the leader takes another few steps. Its tail is replanned below.
            if(solved_)target_=goal;else retarget(goal);
        }
        advancing_=false;
        --budget; // reserve one query to validate the actual movement edge
        ++search_age_;
        while(!solved_ && budget && !open_.empty() && nodes_.size()<2048){
            const auto i=open_.top().i;
            if(nodes_[i].closed){open_.pop();continue;}
            if(bot_distance(nodes_[i].p,target_)<cell_*.8f){best_=i;solved_=true;break;}
            if(nodes_[i].next==16){nodes_[i].closed=true;open_.pop();continue;}
            const auto n=nodes_[i].next++;
            // Use shorter edges near obstacles to align with narrow doorways.
            // Open ground retains the cheaper coarse expansion.
            if(n>=8 && !nodes_[i].blocked)continue;
            --budget;auto desired=nodes_[i].p;
            static constexpr int directions[8][2]={{1,0},{0,1},{-1,0},{0,-1},{1,1},{-1,1},{-1,-1},{1,-1}};
            const float stride=n<8?cell_:cell_*.5f;
            desired[0]+=directions[n%8][0]*stride;desired[2]+=directions[n%8][1]*stride;
            const auto p=probe(nodes_[i].p,desired);
            if(!p || !valid(*p) || bot_distance(nodes_[i].p,*p)<.05f){if(n<8)nodes_[i].blocked|=1u<<n;continue;}
            const auto k=key(*p);const auto cost=nodes_[i].g+bot_distance(nodes_[i].p,*p);
            auto found=cells_.find(k);
            if(found!=cells_.end()){
                auto& old=nodes_[found->second];if(cost>=old.g-.01f)continue;
                old.g=cost;old.parent=i;old.closed=false;old.next=old.blocked=0;open_.push({cost+bot_distance(old.p,target_),found->second});continue;
            }
            const auto index=unsigned(nodes_.size());nodes_.push_back({*p,cost,i});cells_[k]=index;
            if(bot_distance(*p,target_)<bot_distance(nodes_[best_].p,target_))best_=index;
            open_.push({cost+bot_distance(*p,target_),index});
        }
        const bool exhausted=open_.empty() || nodes_.size()>=2048;
        // Start following useful, checked progress while a longer detour is
        // still being searched. A moving leader cannot keep the bot motionless.
        if(path_.empty() && best_ && (solved_ || exhausted || search_age_>=4) && best_!=tip_){
            tip_=best_;path_goal_=target_;
            for(unsigned i=best_,n=0;i && n<nodes_.size();i=nodes_[i].parent,++n)path_.push_back(nodes_[i].p);
            std::reverse(path_.begin(),path_.end());cursor_=0;
            // The search root can be behind us after following a partial path.
            // Rejoin at the last nearby point, without cutting an unchecked edge.
            for(unsigned i=0;i<path_.size();++i)if(bot_distance(start,path_[i])<cell_*.8f)cursor_=i+1;
        }
        auto reached_point=[&](const BotPoint& point){
            if(bot_distance(start,point)<cell_*.35f)return true;
            const float dx=start[0]-previous[0],dz=start[2]-previous[2],length2=dx*dx+dz*dz;
            if(length2<.001f || length2>25.f)return false;
            const auto t=std::clamp(((point[0]-previous[0])*dx+(point[2]-previous[2])*dz)/length2,0.f,1.f);
            return std::hypot(point[0]-previous[0]-t*dx,point[2]-previous[2]-t*dz)<cell_*.3f && std::abs(point[1]-(previous[1]+t*(start[1]-previous[1])))<.5f;
        };
        while(cursor_<path_.size() && reached_point(path_[cursor_]))++cursor_;
        if(cursor_<path_.size()){
            if(auto safe=probe(start,path_[cursor_]);safe && valid(*safe)){advance_target_=*safe;advancing_=true;return *safe;}
            begin(start,goal);return start; // a door/actor may have blocked the old edge
        }
        path_.clear();cursor_=0;
        if(bot_distance(start,goal)<cell_*2.5f){
            if(auto safe=probe(start,goal);safe && valid(*safe)){advance_target_=*safe;advancing_=bot_distance(start,*safe)>.1f;return *safe;}
        }
        if(solved_ || exhausted){
            if(!solved_ && cell_> .625f)cell_=.625f;
            begin(start,goal);
        }
        return start;
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

