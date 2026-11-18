#pragma once
#include "team_hud.h"
#include "game_view_geometry.h"
#include "tactical_ping_math.h"
#include <imgui.h>
#include <cfloat>
#include <cstdio>
#include <span>

namespace port {
namespace team_hud_detail {
inline const char* status_text(TeamHudStatus status,unsigned language){
    switch(status){
    case TeamHudStatus::Loading:return language==1?"L\xC3\xA4" "dt":language==2?"Chargement":"Loading";
    case TeamHudStatus::Dead:return language==1?"Tot":language==2?"Mort":"Dead";
    case TeamHudStatus::Afk:return "AFK";
    case TeamHudStatus::Waiting:return language==1?"Wartet":language==2?"En attente":"Waiting";
    default:return "";
    }
}
inline std::string shown_number(float value,bool known){
    if(!known || !std::isfinite(value) || value<0 || value>1000000.f)return "--";
    char text[24];std::snprintf(text,sizeof(text),"%.0f",std::ceil(value));return text;
}
inline std::string shown_distance(float value,bool known){
    if(!known || !std::isfinite(value) || value<0)return "-- m";
    char text[24];std::snprintf(text,sizeof(text),"%.0f%s m",std::round(std::min(value,9999.f)),value>9999.f?"+":"");return text;
}
}
// Presentation thread only. No windows, input, game RAM or world captures.
inline void draw_team_hud(std::span<const TeamHudRow> rows,const GameViewRect& image,unsigned language){
    if(rows.empty() || !image.valid() || image.width<96.f || image.height<80.f)return;
    auto* font=ImGui::GetFont();auto* draw=ImGui::GetForegroundDrawList();
    const float size=std::clamp(image.height/52.f,11.f,14.f);
    const float margin=std::clamp(image.height*.025f,8.f,16.f),width=std::min(310.f,image.width-margin*2.f);
    const float line=size+2.f,row_height=line*2.f+5.f;
    const unsigned count=std::min<unsigned>({unsigned(rows.size()),coop::max_players-1,unsigned(std::max(0.f,std::floor((image.height-margin*2.f)/row_height)))});
    if(!count || width<=0)return;
    const auto measure=[&](std::string_view text){return text.empty()?0.f:font->CalcTextSizeA(size,FLT_MAX,0.f,text.data(),text.data()+text.size()).x;};
    const float x=image.x+image.width-margin-width,top=image.y+margin;
    draw->PushClipRect(ImVec2(x,top),ImVec2(x+width,image.y+image.height-margin),true);
    const auto text=[&](float y,ImU32 color,const std::string& value){
        draw->AddText(font,size,ImVec2(x+1,y+1),IM_COL32(0,0,0,220),value.c_str());
        draw->AddText(font,size,ImVec2(x,y),color,value.c_str());
    };
    for(unsigned i=0;i<count;++i){
        const auto& row=rows[i];const float y=top+float(i)*row_height;
        std::string name=row.name;
        for(auto& byte:name)if(static_cast<unsigned char>(byte)<32 || byte==127)byte=' ';
        if(name.empty())name=language==1?"Spieler":language==2?"Joueur":"Player";
        std::string suffix=row.bot?" [Bot]":"";
        if(const auto* state=team_hud_detail::status_text(row.status,language);*state){suffix+=" | ";suffix+=state;}
        const auto title=ping_detail::elide_utf8(name,std::max(0.f,width-measure(suffix)),measure)+suffix;
        const auto color=row.status==TeamHudStatus::Dead?IM_COL32(255,137,128,245):
            row.status==TeamHudStatus::Afk?IM_COL32(255,218,119,245):IM_COL32(214,239,255,245);
        text(y,color,ping_detail::elide_utf8(title,width,measure));
        const char* hp=language==1?"L":language==2?"PV":"HP";
        const char* armor=language==1?"R":"AR";
        const auto detail=std::string(hp)+" "+team_hud_detail::shown_number(row.health,row.health_known)+" | "+armor+" "+
            team_hud_detail::shown_number(row.armor,row.armor_known)+" | "+team_hud_detail::shown_distance(row.distance,row.distance_known);
        text(y+line,IM_COL32(238,238,238,235),ping_detail::elide_utf8(detail,width,measure));
    }
    draw->PopClipRect();
}
}
