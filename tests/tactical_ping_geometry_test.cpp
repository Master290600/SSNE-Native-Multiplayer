#include "tactical_ping_math.h"
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void near(float a,float b){check(std::abs(a-b)<.0002f,"Unexpected tactical camera coordinate");}
ssne::ModViewCamera camera(){ssne::ModViewCamera c;c.view={1,0,0,0,0,1,0,0,0,0,1,0};c.projection_x=1;c.projection_y=1;return c;}
void basics(){
    auto c=camera();auto ray=port::ping_detail::camera_ray(c);check(bool(ray),"Identity camera lost ray");near(ray->eye[0],0);near(ray->direction[2],-1);
    auto p=port::ping_detail::project(c,{0,0,-10});check(p && p->onscreen,"Center marker missing");near(p->u,.5);near(p->v,.5);near(p->distance,10);
    p=port::ping_detail::project(c,{5,2,-10});near(p->u,.75);near(p->v,.4);
    c.projection_x=.5;p=port::ping_detail::project(c,{5,2,-10});near(p->u,.625);near(p->v,.4);
    c.projection_y=2;p=port::ping_detail::project(c,{5,2,-10});near(p->v,.3);
    check(port::ping_detail::crosshair_angle(c,{0,0,-10})==0,"Crosshair center angle changed");
    check(port::ping_detail::crosshair_angle(c,{0,0,10})>.075f,"Behind-camera entity entered targeting cone");
}
void moved_and_rotated(){
    auto c=camera();c.view={0,0,-1,30,0,1,0,-2,1,0,0,-10};
    const auto ray=port::ping_detail::camera_ray(c);check(bool(ray),"Rotated camera inverse missing");
    near(ray->eye[0],10);near(ray->eye[1],2);near(ray->eye[2],30);near(ray->direction[0],-1);near(ray->direction[2],0);
    auto p=port::ping_detail::project(c,{0,2,30});check(p && p->onscreen,"World ray target did not project to center");near(p->u,.5);near(p->v,.5);
    // General inverse supports a finite native basis scale, not just transpose.
    c.view={2,0,0,-20,0,2,0,-4,0,0,2,-60};const auto scaled=port::ping_detail::camera_ray(c);check(bool(scaled),"Scaled camera inverse missing");near(scaled->eye[0],10);near(scaled->eye[1],2);near(scaled->eye[2],30);near(scaled->direction[2],-1);
}
void edges_and_bad_values(){
    auto c=camera();for(auto target:{port::ping_detail::Point{100,0,-10},{0,100,-10},{0,0,10},{100,0,10}}){
        const auto p=port::ping_detail::project(c,target);check(p && !p->onscreen,"Offscreen marker became centered");check(p->u>=.0449f && p->u<=.9551f && p->v>=.0449f && p->v<=.9551f,"Offscreen marker escaped image margin");}
    auto p=port::ping_detail::project(c,{0,0,10});check(p->direction_y>0,"Directly behind direction is ambiguous");
    check(!port::ping_detail::project(c,{0,0,0}),"Camera-origin marker divided by zero");
    for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){auto broken=c;broken.view[0]=bad;check(!port::ping_detail::camera_ray(broken),"Invalid basis accepted");check(!port::ping_detail::project(c,{bad,0,-10}),"Invalid marker accepted");}
    c.view.fill(0);check(!port::ping_detail::camera_ray(c),"Singular camera accepted");c=camera();c.projection_x=0;check(!port::ping_detail::camera_ray(c) && !port::ping_detail::project(c,{0,0,-10}),"Invalid projection accepted");
}
void caption_declutter(){
    using port::ping_detail::CaptionRect;
    const CaptionRect image{4,119,1092,611};
    for(float anchor_y:{420.f,720.f,124.f}){
        std::array<CaptionRect,16> occupied{};
        for(unsigned i=0;i<8;++i)occupied[i]={510.f+float(i%2)*40-10,anchor_y-10,20,20};
        unsigned count=8;
        for(unsigned i=0;i<8;++i){
            const auto rect=port::ping_detail::place_caption(510.f+float(i%2)*40,anchor_y,14,96,27,image,
                std::span<const CaptionRect>(occupied.data(),count));
            check(bool(rect),"An eight-marker cluster lost a caption despite available space");
            check(rect->x>=image.x && rect->y>=image.y && rect->x+rect->width<=image.x+image.width && rect->y+rect->height<=image.y+image.height,"Caption escaped actual image bounds");
            for(unsigned previous=0;previous<count;++previous)check(!port::ping_detail::caption_overlaps(*rect,occupied[previous]),"Caption overlaps a marker anchor or another caption");
            occupied[count++]=*rect;
        }
    }
    check(!port::ping_detail::place_caption(0,0,14,120,27,{0,0,100,20},{}),"Too-small image produced a clipped caption");
    // A caption fits between crowded rows in a side column without moving its
    // world anchor; this is the fallback used near image edges.
    const std::array<CaptionRect,1> full_center{{{100,0,100,200}}};
    const auto side=port::ping_detail::place_caption(150,100,14,50,27,{0,0,300,200},full_center);
    check(side && !port::ping_detail::caption_overlaps(*side,full_center[0]),"Side-column fallback overlaps occupied text");
}
void utf8_caption_elision(){
    const auto measure=[](std::string_view text){unsigned count=0;for(unsigned char value:text)if((value&0xC0u)!=0x80u)++count;return float(count*10);};
    const std::string accent="\xC3\xA4";
    const std::string emoji="\xF0\x9F\x98\x80";
    check(port::ping_detail::elide_utf8(accent+"bcdef",40,measure)==accent+"...","UTF8 accent was split while truncating a name");
    check(port::ping_detail::elide_utf8(emoji+"bcdef",40,measure)==emoji+"...","Four-byte name character was split while truncating");
    check(port::ping_detail::elide_utf8("Host-214",100,measure)=="Host-214","Fitting name was unnecessarily truncated");
    check(port::ping_detail::elide_utf8("abcdef",20,measure).empty(),"Ellipsis overflowed a tiny image");
}
}
int main(){try{basics();moved_and_rotated();edges_and_bad_values();caption_declutter();utf8_caption_elision();std::cout<<"Tactical ping: camera inverse, crosshair, FOV, image projection, offscreen direction, caption declutter, UTF8 names and invalid data passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
