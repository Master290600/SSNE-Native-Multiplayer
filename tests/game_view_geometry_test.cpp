#include "game_view_geometry.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void near(float value,float expected,float tolerance=.0002f){check(std::fabs(value-expected)<=tolerance,"Unexpected viewport coordinate");}
int main(){try{
 const auto four=port::fit_game_view(0,0,960,540,1);near(four.x,120);near(four.y,0);near(four.width,720);near(four.height,540);
 auto center=port::point_in_game_view(four,480,270);check(center.valid && center.inside,"Image center missing");near(center.u,.5f);near(center.v,.5f);
 check(!port::point_in_game_view(four,60,270).inside,"Side fill became interactive");check(port::point_in_game_view(four,120,0).inside,"Top/left edge excluded");
 check(!port::point_in_game_view(four,840,270).inside && !port::point_in_game_view(four,480,540).inside,"Right/bottom edges must be half open");
 const auto wide=port::fit_game_view(0,0,960,540,4.f/3.f);near(wide.x,0);near(wide.y,0);near(wide.width,960);near(wide.height,540);
 const auto ultra=port::fit_game_view(0,0,960,540,7.f/4.f);near(ultra.x,0);near(ultra.y,64.285713f);near(ultra.width,960);near(ultra.height,411.42856f);
 check(!port::point_in_game_view(ultra,480,20).inside,"Letterbox became interactive");
 const auto tall=port::fit_game_view(0,0,600,1000,4.f/3.f);near(tall.width,600);near(tall.height,337.5f);near(tall.y,331.25f);
 const auto moved=port::fit_game_view(30,70,960,540,1);const auto shifted=port::point_in_game_view(moved,510,340);near(shifted.u,.5f);near(shifted.v,.5f);
 for(float drawable_scale:{1.f,1.25f,1.5f,2.f,3.f}){const auto point=port::point_in_game_view(four,300,135);near(point.u,.25f);near(point.v,.25f);near((four.x+point.u*four.width)*drawable_scale,300.f*drawable_scale);}
 const auto resized=port::fit_game_view(0,0,1920,1080,1);const auto stable=port::point_in_game_view(resized,600,270);near(stable.u,.25f);near(stable.v,.25f);
 const auto outside=port::point_in_game_view(four,48,648);check(outside.valid && !outside.inside,"Outside drag lost coordinates");near(outside.u,-.1f);near(outside.v,1.2f);
 near(port::fit_game_view(0,0,960,540,.5f).width,four.width);
 for(float bad:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})check(!port::fit_game_view(0,0,bad,540,1).valid(),"Invalid viewport accepted");
 check(!port::point_in_game_view(four,std::numeric_limits<float>::quiet_NaN(),0).valid,"Invalid pointer accepted");
 std::cout<<"Game-view geometry: aspect, bars, logical DPI, translation, resize, edges and outside drags passed.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
