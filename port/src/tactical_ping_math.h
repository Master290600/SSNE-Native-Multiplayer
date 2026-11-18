#pragma once
#include "ssne/mods.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace port::ping_detail {
using Point=std::array<float,3>;
inline bool finite_point(const Point& point){
    return std::all_of(point.begin(),point.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1000000.f;});
}
inline Point camera_point(const ssne::ModViewCamera& camera,const Point& point){
    Point result{};
    for(unsigned row=0;row<3;++row){result[row]=camera.view[row*4+3];for(unsigned i=0;i<3;++i)result[row]+=camera.view[row*4+i]*point[i];}
    return result;
}
struct Ray {Point eye{},direction{};};
inline std::optional<Ray> camera_ray(const ssne::ModViewCamera& camera){
    const auto& m=camera.view;
    if(!std::all_of(m.begin(),m.end(),[](float v){return std::isfinite(v);}) ||
       !std::isfinite(camera.projection_x) || !std::isfinite(camera.projection_y) || camera.projection_x<=0 || camera.projection_y<=0)return {};
    const double determinant=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])-
        double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])+double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
    if(!std::isfinite(determinant) || std::abs(determinant)<1.e-8)return {};
    const std::array<double,9> inverse{
        (double(m[5])*m[10]-double(m[6])*m[9])/determinant,(double(m[2])*m[9]-double(m[1])*m[10])/determinant,(double(m[1])*m[6]-double(m[2])*m[5])/determinant,
        (double(m[6])*m[8]-double(m[4])*m[10])/determinant,(double(m[0])*m[10]-double(m[2])*m[8])/determinant,(double(m[2])*m[4]-double(m[0])*m[6])/determinant,
        (double(m[4])*m[9]-double(m[5])*m[8])/determinant,(double(m[1])*m[8]-double(m[0])*m[9])/determinant,(double(m[0])*m[5]-double(m[1])*m[4])/determinant};
    Ray ray;
    for(unsigned row=0;row<3;++row){ray.eye[row]=float(-(inverse[row*3]*m[3]+inverse[row*3+1]*m[7]+inverse[row*3+2]*m[11]));ray.direction[row]=float(-inverse[row*3+2]);}
    const auto length=std::hypot(ray.direction[0],ray.direction[1],ray.direction[2]);
    if(!finite_point(ray.eye) || !std::isfinite(length) || length<.0001f)return {};
    for(auto& value:ray.direction)value/=length;
    return ray;
}
struct Projection {float u=.5f,v=.5f,distance=0,direction_x=0,direction_y=-1;bool onscreen=false;};
inline std::optional<Projection> project(const ssne::ModViewCamera& camera,const Point& point,float margin=.045f){
    if(!finite_point(point) || !std::isfinite(camera.projection_x) || !std::isfinite(camera.projection_y) || camera.projection_x<=0 || camera.projection_y<=0)return {};
    const auto p=camera_point(camera,point);const float distance=std::hypot(p[0],p[1],p[2]);
    if(!std::isfinite(distance) || distance<.1f)return {};
    Projection out;out.distance=distance;
    const float depth=std::max(.001f,std::abs(p[2]));
    float dx=camera.projection_x*p[0]/depth,dy=-camera.projection_y*p[1]/depth;
    if(!std::isfinite(dx) || !std::isfinite(dy))return {};
    if(p[2]<-.05f){
        out.u=.5f+.5f*dx;out.v=.5f+.5f*dy;
        out.onscreen=out.u>=margin && out.u<=1-margin && out.v>=margin && out.v<=1-margin;
    }
    if(!out.onscreen){
        if(std::abs(dx)+std::abs(dy)<.0001f)dy=1; // Directly behind: point down.
        const auto scale=(.5f-margin)/std::max(std::abs(dx),std::abs(dy));
        out.u=.5f+dx*scale;out.v=.5f+dy*scale;
    }
    const auto length=std::hypot(dx,dy);if(length>.0001f){out.direction_x=dx/length;out.direction_y=dy/length;}
    return out;
}
inline float crosshair_angle(const ssne::ModViewCamera& camera,const Point& point){
    const auto p=camera_point(camera,point);
    if(!finite_point(p) || p[2]>=-.1f)return 4.f;
    return std::atan2(std::hypot(p[0],p[1]),-p[2]);
}
struct CaptionRect {float x=0,y=0,width=0,height=0;};
inline bool caption_overlaps(const CaptionRect& a,const CaptionRect& b,float gap=2.f){
    return a.x<b.x+b.width+gap && a.x+a.width+gap>b.x && a.y<b.y+b.height+gap && a.y+a.height+gap>b.y;
}
// At most eight active marker captions. Try a bounded set of vertical rows,
// then nearby/edge side columns; never move the actual world marker anchor.
inline std::optional<CaptionRect> place_caption(float anchor_x,float anchor_y,float anchor_gap,float width,float height,
                                               const CaptionRect& image,std::span<const CaptionRect> occupied){
    if(!std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0 ||
       width>image.width || height>image.height)return {};
    const auto left=image.x,right=image.x+image.width-width,top=image.y,bottom=image.y+image.height-height;
    const std::array<float,5> columns{std::clamp(anchor_x-width*.5f,left,right),
        std::clamp(anchor_x-width-anchor_gap,left,right),std::clamp(anchor_x+anchor_gap,left,right),left,right};
    const float step=height+4.f;
    for(const auto x:columns)for(unsigned row=0;row<16;++row)for(unsigned above=0;above<2;++above){
        const float y=above?anchor_y-anchor_gap-height-float(row)*step:anchor_y+anchor_gap+float(row)*step;
        if(y<top || y>bottom)continue;
        const CaptionRect candidate{x,y,width,height};
        if(std::none_of(occupied.begin(),occupied.end(),[&](const auto& used){return caption_overlaps(candidate,used);}))return candidate;
    }
    return {};
}
template<class Measure>
std::string elide_utf8(std::string_view text,float width,Measure measure){
    if(width<=0 || !std::isfinite(width))return {};
    if(measure(text)<=width)return std::string(text);
    constexpr std::string_view tail="...";
    if(measure(tail)>width)return {};
    size_t end=text.size();
    while(end){
        --end;while(end && (uint8_t(text[end])&0xC0u)==0x80u)--end;
        auto shortened=std::string(text.substr(0,end))+std::string(tail);
        if(measure(shortened)<=width)return shortened;
    }
    return std::string(tail);
}
}
