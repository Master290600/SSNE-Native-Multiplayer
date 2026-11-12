#pragma once
#include <algorithm>
#include <cmath>
namespace port {
// ImGui/SDL logical window points. The factor belongs to the displayed frame.
struct GameViewRect {
    float x=0,y=0,width=0,height=0,wide=1;
    bool valid()const{return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) && std::isfinite(height) && width>0 && height>0;}
};
inline GameViewRect fit_game_view(float x,float y,float viewport_width,float viewport_height,float frame_wide){
    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(viewport_width) || !std::isfinite(viewport_height) || !std::isfinite(frame_wide) || viewport_width<=0 || viewport_height<=0)return {};
    const float wide=std::max(1.f,frame_wide),aspect=4.f/3.f*wide;
    float width=viewport_width,height=viewport_width/aspect;
    if(height>viewport_height){height=viewport_height;width=viewport_height*aspect;}
    return {x+(viewport_width-width)*.5f,y+(viewport_height-height)*.5f,width,height,wide};
}
struct GameViewPoint {float u=0,v=0;bool inside=false,valid=false;};
inline GameViewPoint point_in_game_view(const GameViewRect& image,float window_x,float window_y){
    if(!image.valid() || !std::isfinite(window_x) || !std::isfinite(window_y))return {};
    const float u=(window_x-image.x)/image.width,v=(window_y-image.y)/image.height;
    // Outside UV remains valid for a captured drag; normal hits require inside.
    return {u,v,u>=0 && u<1 && v>=0 && v<1,true};
}
}
