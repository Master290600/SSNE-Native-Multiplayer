#include "tactical_ping.h"
#include "tactical_ping_math.h"
#include "ssne/native_party.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace port {
namespace {
using ping_detail::Point;
const Point* position(const coop::EntityState& entity){return coop::tactical_ping_position(entity);}
bool pickup_available(const coop::EntityState& entity,unsigned slot){
    if(auto it=entity.properties.find("pickup_view_mask");it!=entity.properties.end())
        if(const auto* mask=std::get_if<int64_t>(&it->second);mask && !(*mask&(int64_t(1)<<slot)))return false;
    if(auto it=entity.properties.find("pickup_ownership");it!=entity.properties.end())
        if(const auto* owners=std::get_if<std::vector<int64_t>>(&it->second);owners && slot<owners->size() && (*owners)[slot])return false;
    return true;
}
Point target_center(const Point& point,coop::PingKind kind){auto result=point;result[1]+=kind==coop::PingKind::Enemy?1.1f:kind==coop::PingKind::Pickup?.4f:0.f;return result;}
const char* type_text(coop::PingKind kind,unsigned language){
    if(kind==coop::PingKind::Enemy)return language==1?"Gegner":language==2?"Ennemi":"Enemy";
    if(kind==coop::PingKind::Pickup)return language==1?"Gegenstand":language==2?"Objet":"Item";
    return language==1?"Ort":language==2?"Point":"Location";
}
ImU32 color(coop::PingKind kind,float alpha){
    return kind==coop::PingKind::Enemy?IM_COL32(255,96,78,unsigned(255*alpha)):
        kind==coop::PingKind::Pickup?IM_COL32(104,238,153,unsigned(255*alpha)):IM_COL32(255,213,80,unsigned(255*alpha));
}
void caption(ImDrawList* draw,ImVec2 p,ImU32 ink,const std::string& text){
    draw->AddText(ImVec2(p.x+1,p.y+1),IM_COL32(0,0,0,((ink>>IM_COL32_A_SHIFT)&255)*4/5),text.c_str());
    draw->AddText(p,ink,text.c_str());
}
}
std::optional<coop::TacticalPingRequest> resolve_tactical_ping(PPCContext& context,const coop::WorldState& world,coop::PlayerId own,const ssne::ModViewCamera& camera){
    if(own.slot>=coop::max_players || camera.local_player!=int(own.slot) || !coop::tactical_ping_gameplay(world))return {};
    const auto& person=world.players[own.slot];
    if(person.id!=own || !person.connected || !person.ready || person.afk || person.spectator)return {};
    const auto actor=world.entities.find(person.character);
    if(actor==world.entities.end() || actor->second.owner!=own || !coop::tactical_ping_alive(actor->second))return {};
    const auto ray=ping_detail::camera_ray(camera);if(!ray)return {};
    Point distant=ray->eye;for(unsigned axis=0;axis<3;++axis)distant[axis]+=ray->direction[axis]*100.f;
    std::optional<ssne::NativeSegmentHit> hit;std::string error;
    if(!ssne::native_world_segment(context,ray->eye,distant,hit,error))return {};
    Point location=hit?hit->position:distant;
    if(!ping_detail::finite_point(location))return {};
    struct Candidate {coop::EntityId id;coop::PingKind kind;Point point;float angle,distance;};
    std::vector<Candidate> candidates;
    for(const auto& [id,entity]:world.entities){
        if(entity.kind!=coop::EntityKind::Enemy && entity.kind!=coop::EntityKind::Pickup)continue;
        if(!coop::tactical_ping_alive(entity) || (entity.kind==coop::EntityKind::Pickup && !pickup_available(entity,own.slot)))continue;
        const auto* base=position(entity);if(!base || !ping_detail::finite_point(*base))continue;
        const auto kind=entity.kind==coop::EntityKind::Enemy?coop::PingKind::Enemy:coop::PingKind::Pickup;
        const auto point=target_center(*base,kind);
        const auto angle=ping_detail::crosshair_angle(camera,point);
        const auto view=ping_detail::camera_point(camera,point);const auto distance=std::hypot(view[0],view[1],view[2]);
        if(angle<=.075f && distance>=.3f && distance<=coop::tactical_ping_range-5.f)candidates.push_back({id,kind,point,angle,distance});
    }
    std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.angle!=b.angle?a.angle<b.angle:a.distance<b.distance;});
    // Only an explicit key press scans entities. At most three native LOS
    // queries let a wall occlude a near-crosshair target without per-frame work.
    for(unsigned i=0;i<std::min<size_t>(candidates.size(),3);++i){
        const auto& candidate=candidates[i];std::optional<ssne::NativeSegmentHit> obstruction;
        if(!ssne::native_world_segment(context,ray->eye,candidate.point,obstruction,error))continue;
        if(obstruction && std::hypot(obstruction->position[0]-ray->eye[0],obstruction->position[1]-ray->eye[1],obstruction->position[2]-ray->eye[2])+.45f<candidate.distance)continue;
        return coop::TacticalPingRequest{world.epoch,candidate.point,candidate.id,candidate.kind};
    }
    return coop::TacticalPingRequest{world.epoch,location,0,coop::PingKind::Location};
}
void draw_tactical_pings(std::span<const coop::TacticalPing> pings,const coop::WorldState* world,const ssne::ModViewCamera& camera,const GameViewRect& image,unsigned language){
    if(pings.empty() || !world || !image.valid() || !coop::tactical_ping_gameplay(*world))return;
    auto* draw=ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(image.x,image.y),ImVec2(image.x+image.width,image.y+image.height),true);
    struct DrawMarker {
        ImVec2 center{};ping_detail::Projection projected{};
        float radius=0,alpha=0;ImU32 ink=0;
        std::string title,name;float text_width=0;
    };
    std::array<DrawMarker,coop::max_players> markers{};unsigned count=0;
    const auto measure=[](std::string_view text){return text.empty()?0.f:ImGui::CalcTextSize(text.data(),text.data()+text.size()).x;};
    const auto max_text_width=std::max(0.f,std::min(220.f,image.width*.44f));
    const auto line_height=ImGui::GetTextLineHeight();
    for(const auto& ping:pings){
        if(count>=coop::max_players)break;
        if(!ping.remaining_ms || ping.epoch!=world->epoch || ping.sender.slot>=coop::max_players)continue;
        const auto& sender=world->players[ping.sender.slot];if(!sender.connected || sender.id!=ping.sender)continue;
        Point point=ping.position;
        if(ping.target){
            const auto found=world->entities.find(ping.target);
            if(found==world->entities.end() || !coop::tactical_ping_alive(found->second))continue;
            const auto* current=position(found->second);if(!current)continue;
            if(ping.kind==coop::PingKind::Pickup && camera.local_player>=0 && !pickup_available(found->second,unsigned(camera.local_player)))continue;
            point=target_center(*current,ping.kind);
        }
        const auto projected=ping_detail::project(camera,point);if(!projected)continue;
        const float alpha=std::clamp(float(ping.remaining_ms)/1500.f,0.f,1.f);
        auto& marker=markers[count++];marker.projected=*projected;marker.alpha=alpha;marker.ink=color(ping.kind,alpha);
        marker.center=ImVec2(image.x+projected->u*image.width,image.y+projected->v*image.height);
        marker.radius=std::clamp(image.height/90.f,5.f,9.f);
        char distance[24];std::snprintf(distance,sizeof(distance)," %u m",unsigned(std::round(std::min(projected->distance,9999.f))));
        marker.title=ping_detail::elide_utf8(std::string(type_text(ping.kind,language))+distance,max_text_width,measure);
        marker.name=ping_detail::elide_utf8(ping.name,max_text_width,measure);
        marker.text_width=std::max(measure(marker.title),measure(marker.name));
    }
    std::array<ping_detail::CaptionRect,coop::max_players*2> occupied{};unsigned occupied_count=0;
    // Reserve all diamonds before placing any text. Close targets must not put
    // their names across each other's anchors or already placed caption rows.
    for(unsigned i=0;i<count;++i){const auto& marker=markers[i];const auto r=marker.radius+2.f;
        occupied[occupied_count++]={marker.center.x-r,marker.center.y-r,r*2,r*2};
    }
    for(unsigned i=0;i<count;++i){
        const auto& marker=markers[i];const auto center=marker.center;const auto radius=marker.radius,alpha=marker.alpha;const auto ink=marker.ink;
        const ping_detail::CaptionRect bounds{image.x+4,image.y+4,std::max(0.f,image.width-8),std::max(0.f,image.height-8)};
        const auto block=ping_detail::place_caption(center.x,center.y,radius+6,marker.text_width,line_height*2+1,bounds,
            std::span<const ping_detail::CaptionRect>(occupied.data(),occupied_count));
        if(block){
            occupied[occupied_count++]=*block;
            const ImVec2 edge(std::clamp(center.x,block->x,block->x+block->width),std::clamp(center.y,block->y,block->y+block->height));
            const float dx=edge.x-center.x,dy=edge.y-center.y,length=std::hypot(dx,dy);
            if(length>radius+4.f){
                const ImVec2 start(center.x+dx/length*(radius+1.f),center.y+dy/length*(radius+1.f));
                draw->AddLine(start,edge,IM_COL32(0,0,0,unsigned(120*alpha)),2.f);
                draw->AddLine(start,edge,(ink&~IM_COL32_A_MASK)|IM_COL32(0,0,0,unsigned(175*alpha)),1.f);
            }
            caption(draw,ImVec2(block->x+(block->width-measure(marker.title))*.5f,block->y),ink,marker.title);
            caption(draw,ImVec2(block->x+(block->width-measure(marker.name))*.5f,block->y+line_height+1),IM_COL32(242,242,242,unsigned(235*alpha)),marker.name);
        }
        if(marker.projected.onscreen){
            draw->AddQuad(ImVec2(center.x,center.y-radius),ImVec2(center.x+radius,center.y),ImVec2(center.x,center.y+radius),ImVec2(center.x-radius,center.y),IM_COL32(0,0,0,unsigned(180*alpha)),4.f);
            draw->AddQuad(ImVec2(center.x,center.y-radius),ImVec2(center.x+radius,center.y),ImVec2(center.x,center.y+radius),ImVec2(center.x-radius,center.y),ink,2.f);
            draw->AddCircleFilled(center,1.7f,ink,8);
        }else{
            const ImVec2 direction(marker.projected.direction_x,marker.projected.direction_y),side(-direction.y,direction.x);
            draw->AddTriangleFilled(ImVec2(center.x+direction.x*radius,center.y+direction.y*radius),ImVec2(center.x-direction.x*radius+side.x*radius*.65f,center.y-direction.y*radius+side.y*radius*.65f),ImVec2(center.x-direction.x*radius-side.x*radius*.65f,center.y-direction.y*radius-side.y*radius*.65f),ink);
        }
    }
    draw->PopClipRect();
}
}
