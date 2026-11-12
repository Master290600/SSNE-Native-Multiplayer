#include "mod_menu.h"
#include "settings_menu.h"
#include <imgui.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
namespace port {
namespace {int g_mod_tab=-1;bool g_mod_open=false;const ImVec4 kDim(.62f,.62f,.66f,1),kAccent(1,.76f,.22f,1),kWarn(1,.55f,.35f,1);}
void mod_menu_select_tab(int tab){g_mod_tab=std::clamp(tab,0,4);}
bool mod_menu_open() { return g_mod_open; }
void mod_menu_set_open(bool open) { g_mod_open=open; if(open) menu_set_open(false); }
void draw_mod_menu(const ModMenuControls& controls) {
 if(!g_mod_open) return;
 auto s=ssne::mod_state(); auto options=s.options;
 if(controls.netplay && controls.host) options=controls.requested;
 auto apply=[&](bool local=false) { if(controls.netplay) { auto& fn=local?controls.apply_local:controls.apply; if(fn) fn(options); } else ssne::mod_options(options); };
 auto vp=ImGui::GetMainViewport();
 ImGui::SetNextWindowPos(ImVec2(vp->Size.x*.5f,vp->Size.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
 ImGui::SetNextWindowSize(ImVec2(std::min(620.f,vp->Size.x-40),std::min(480.f,vp->Size.y-40)),ImGuiCond_Always);
 ImGui::SetNextWindowBgAlpha(1);
 bool visible=ImGui::Begin("Mod menu - Insert / Esc",&g_mod_open,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings);
 if(visible) {
  ImGui::TextWrapped(controls.netplay ? "Netplay: Aimbot / ESP and Item ESP use your own player and camera. Each PC configures them independently. The host controls shared cheats and spawns." : "Single player. Options last for this session. The game continues while this menu is open.");
  if(controls.netplay && !controls.host) ImGui::TextDisabled("The host controls the shared mods.");
  if(!s.active) ImGui::TextColored(kWarn,"Load a level to enable the controls.");
  if(ImGui::BeginTabBar("##mods")) {
   if(ImGui::BeginTabItem("Player cheats",nullptr,g_mod_tab==0?ImGuiTabItemFlags_SetSelected:0)) {
    ImGui::BeginDisabled(!s.active || (controls.netplay && !controls.host));
    bool changed=ImGui::Checkbox("God mode",&options.god);
    changed|=ImGui::Checkbox("Infinite ammunition",&options.ammo);
    changed|=ImGui::Checkbox("All weapons",&options.weapons);
    changed|=ImGui::Checkbox("Prevent regular enemy spawns",&options.no_enemies);
    changed|=ImGui::Checkbox("Rapid fire",&options.rapid_fire);
    ImGui::BeginDisabled(!options.rapid_fire);
    changed|=ImGui::SliderFloat("Fire rate multiplier",&options.fire_multiplier,1,10,"%.1fx");
    ImGui::EndDisabled();
    ImGui::TextWrapped("Increases native attack rates. Ammo use and weapon animations still apply.");
    if(ImGui::Button("Disable all cheats")) { options={}; changed=true; }
    if(changed) apply();
    ImGui::EndDisabled();
    ImGui::EndTabItem();
   }
   static bool open_targeting=getenv("SSNE_OPEN_MOD_TARGETING")!=nullptr;
   if(ImGui::BeginTabItem("Aimbot / ESP",nullptr,(open_targeting || g_mod_tab==1)?ImGuiTabItemFlags_SetSelected:0)) {
    open_targeting=false;
    ImGui::BeginChild("Target options",ImVec2(0,-68),false);
    ImGui::BeginDisabled(!s.active || !s.targeting_available);
    bool changed=ImGui::Checkbox("Aimbot",&options.aimbot);
    changed|=ImGui::Checkbox("Only while holding right mouse button",&options.aim_hold);
    changed|=ImGui::SliderFloat("Aim cone (degrees)",&options.aim_fov,1,90,"%.0f");
    changed|=ImGui::SliderFloat("Aim speed",&options.aim_speed,1,60,"%.0f");
        changed|=ImGui::Combo("Aim point",&options.aim_point,"Model center\0Nearest bone to crosshair\0Head bone\0Torso bone\0Bone index\0");
    if(options.aim_point==4)changed|=ImGui::SliderInt("Bone index",&options.aim_bone,0,127);
    ImGui::BeginDisabled(options.aim_point!=0);
    changed|=ImGui::Checkbox("Automatic aim height from model size",&options.auto_aim_height);
    changed|=ImGui::SliderFloat(options.auto_aim_height?"Height adjustment":"Aim height (m)",&options.aim_height,0,10,"%.1f");
    if(options.auto_aim_height) ImGui::TextDisabled("Uses model center. Height adjustment 1.0 is neutral.");
    ImGui::EndDisabled();
    ImGui::Separator();
    changed|=ImGui::Checkbox("Enemy ESP (also through walls)",&options.esp);
    changed|=ImGui::Checkbox("Enemy names",&options.esp_names);
    changed|=ImGui::Checkbox("Distance",&options.esp_distance);
    changed|=ImGui::Checkbox("Health bars / HP",&options.esp_health);
    changed|=ImGui::Checkbox("Bones ESP (animated body parts)",&options.bone_esp);
    changed|=ImGui::Checkbox("Bone names / indices",&options.bone_names);
    changed|=ImGui::SliderFloat("Target / ESP range (m)",&options.range,5,500,"%.0f");
    if(changed) apply(true);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::Text("Living enemies tracked: %u",s.tracked_enemies);
    ImGui::TextWrapped("Targets use animated centers of named skin groups, not exact joints. Missing groups fall back to model center. Indices vary by monster. Yellow marks the aim target.");
    if(!s.targeting_available) ImGui::TextColored(kWarn,"Targeting hooks unavailable for this game build.");
    ImGui::EndTabItem();
   }
   static bool open_items=getenv("SSNE_OPEN_MOD_ITEMS")!=nullptr;
   if(ImGui::BeginTabItem("Item ESP",nullptr,(open_items || g_mod_tab==2)?ImGuiTabItemFlags_SetSelected:0)) {
    open_items=false;
    ImGui::BeginDisabled(!s.active || !s.targeting_available);
    bool changed=ImGui::Checkbox("Item ESP (also through walls)",&options.item_esp);
    changed|=ImGui::Checkbox("Weapons",&options.item_weapons);
    changed|=ImGui::Checkbox("Health",&options.item_health);
    changed|=ImGui::Checkbox("Armor",&options.item_armor);
    changed|=ImGui::Checkbox("Secrets (undiscovered triggers)",&options.item_secrets);
    changed|=ImGui::Checkbox("Distance",&options.esp_distance);
    changed|=ImGui::SliderFloat("ESP range (m)",&options.range,5,500,"%.0f");
    if(changed)apply(true);
    ImGui::EndDisabled();
    ImGui::TextWrapped("Weapons: orange. Health: green. Armor: blue. Secrets: purple. Secret markers show the discovery trigger, which may be separate from a hidden pickup.");
    ImGui::EndTabItem();
   }   static bool select_spawn=getenv("SSNE_OPEN_MOD_SPAWN")!=nullptr;
   if(ImGui::BeginTabItem("Spawn",nullptr,(select_spawn || g_mod_tab==3)?ImGuiTabItemFlags_SetSelected:0)) {
    select_spawn=false;
    ImGui::BeginDisabled(!s.active || (controls.netplay && !controls.host));
    static int filter=0,count=1; static unsigned selected=0; static float distance=6;
    static bool global=true;
    ImGui::Checkbox("Global catalog (weapons / items)",&global);
    ImGui::Combo("Type",&filter,"Enemies\0Weapons\0Items / ammunition\0");
    auto matches=[&](const ssne::ModTemplate& t) {
     if(filter==0) return t.enemy;
     if(t.enemy) return false;
     bool weapon=false;
     for(auto& entry:ssne::mod_catalog()) if(!entry.enemy && entry.type==t.type) weapon=entry.weapon;
     return filter==1 ? weapon : !weapon;
    };
    const char* preview=global?"Select an enemy or item":"Select a template";
    if(global) {
      for(auto &e:ssne::mod_catalog()) if(!e.availability_note && (filter==0?e.enemy:(!e.enemy&&e.weapon==(filter==1))) && selected==((e.enemy?0x80000000u:0x40000000u)|e.type)) preview=e.name.c_str();
    } else for(auto& t:s.templates) if(t.id==selected && matches(t)) preview=t.name.c_str();
    if(ImGui::BeginCombo(global?"Enemy / item":"Template",preview)) {
     if(global) {
       for(auto &e:ssne::mod_catalog()) if(!e.availability_note && (filter==0?e.enemy:(!e.enemy&&e.weapon==(filter==1)))) {
         unsigned id=(e.enemy?0x80000000u:0x40000000u)|e.type;
         if(ImGui::Selectable(e.name.c_str(),selected==id)) selected=id;
       }
     } else for(auto& t:s.templates) if(matches(t) && ImGui::Selectable(t.name.c_str(),t.id==selected)) selected=t.id;
     ImGui::EndCombo();
    }
    ImGui::SliderInt("Count",&count,1,10); ImGui::SliderFloat("Distance ahead",&distance,3,30,"%.1f");
    bool exists=false; for(auto& t:s.templates) exists|=t.id==selected && matches(t);
    if(global) for(auto&e:ssne::mod_catalog()) exists|=!e.availability_note && (filter==0?e.enemy:(!e.enemy&&e.weapon==(filter==1))) && selected==((e.enemy?0x80000000u:0x40000000u)|e.type);
    ImGui::BeginDisabled(!exists);
    if(ImGui::Button("Spawn")) {
      if(controls.netplay) { if(controls.spawn) controls.spawn(selected,count,distance); }
      else if(global) ssne::mod_spawn_type((selected&0x80000000u)!=0,selected&0x3FFFFFFFu,count,distance);
      else ssne::mod_spawn(selected,count,distance);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped(global?"Weapons and items do not require a level template. Enemies still require a model loaded in this level; absent models are refused. Vehicle weapon configurations are not independent pickups.":"Templates are collected from objects loaded in this level. Use an open area for spawning.");
    ImGui::EndDisabled();
    ImGui::EndTabItem();
   }
   static bool select_catalog=getenv("SSNE_OPEN_MOD_CATALOG")!=nullptr;
   if(ImGui::BeginTabItem("Names / catalog",nullptr,(select_catalog || g_mod_tab==4)?ImGuiTabItemFlags_SetSelected:0)) {
    select_catalog=false;
    static int category=0;
    ImGui::Combo("Show",&category,"Enemies\0Weapons\0Items / ammunition\0");
    ImGui::TextWrapped("Known type catalog. Use Global catalog on the Spawn tab to spawn native types across levels. Boss components can require their encounter; vehicle weapons have no separate pickup.");
    ImGui::BeginChild("##mod_catalog",ImVec2(0,-32),true);
    for(auto& entry:ssne::mod_catalog()) {
     if(category==0 ? !entry.enemy : (entry.enemy || entry.weapon!=(category==1))) continue;
     bool loaded=false;
     for(auto& t:s.templates) loaded|=!entry.availability_note && t.enemy==entry.enemy && t.type==entry.type;
     ImGui::TextColored(loaded?kAccent:kDim,"%s",entry.name.c_str());
     ImGui::SameLine();
     ImGui::TextColored(kDim,"(%s)",entry.availability_note?entry.availability_note:(loaded?"template loaded":"not loaded in this level"));
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
   }
   ImGui::EndTabBar();g_mod_tab=-1;
  }
  ImGui::Separator(); ImGui::TextWrapped("%s",s.status.c_str());
 }
 ImGui::End();
}

void draw_mod_overlay(float x,float y,float w,float h){
ImVec2 p0(x,y);auto dl=ImGui::GetBackgroundDrawList();
    auto options=ssne::mod_current_options();
    if((options.esp || options.item_esp || options.bone_esp)) {
        dl->PushClipRect(p0,ImVec2(p0.x+w,p0.y+h),true);
        for(auto &m:ssne::mod_item_markers()) {
            ImVec2 p(p0.x+m.x*w,p0.y+m.y*h);
            auto color=m.kind==ssne::ModItemKind::Health?IM_COL32(80,255,100,255):m.kind==ssne::ModItemKind::Armor?IM_COL32(90,170,255,255):m.kind==ssne::ModItemKind::Secret?IM_COL32(230,110,255,255):IM_COL32(255,190,65,255);
            dl->AddRect(ImVec2(p.x-5,p.y-5),ImVec2(p.x+5,p.y+5),color,0,0,2);
            std::string label=m.kind==ssne::ModItemKind::Secret?"Secret":ssne::mod_type_name(false,m.type);
            if(options.esp_distance) { char text[32];snprintf(text,sizeof(text)," %.0f m",m.distance);label+=text; }
            dl->AddText(ImVec2(p.x+9,p.y-7),IM_COL32(0,0,0,255),label.c_str());
            dl->AddText(ImVec2(p.x+8,p.y-8),color,label.c_str());
        }
        auto bones=ssne::mod_bone_markers();
        for(auto &b:bones) {
            ImVec2 p(p0.x+b.x*w,p0.y+b.y*h);
            auto color=b.target?IM_COL32(255,215,40,255):IM_COL32(80,240,245,255);
            if(b.parent>=0)for(auto &a:bones)if(a.enemy==b.enemy && int(a.index)==b.parent){dl->AddLine(p,ImVec2(p0.x+a.x*w,p0.y+a.y*h),IM_COL32(80,240,245,210),1.5f);break;}
            dl->AddCircleFilled(p,b.target?4.f:2.5f,color,8);
            if(options.bone_names){std::string name=std::to_string(b.index)+": "+b.name;dl->AddText(ImVec2(p.x+5,p.y-6),color,name.c_str());}
        }
        for(auto &m:ssne::mod_markers()) {
            ImVec2 p(p0.x+m.x*w,p0.y+m.y*h);
            auto color=m.target&&options.aimbot?IM_COL32(255,210,40,255):IM_COL32(255,90,90,255);
            dl->AddCircle(p,7,color,12,2);
            if(options.esp_health && m.max_health>0) {
                float fraction=std::clamp(m.health/m.max_health,0.f,1.f);
                ImVec2 a(p.x-23,p.y-16),b(p.x+23,p.y-10);
                dl->AddRectFilled(ImVec2(a.x-1,a.y-1),ImVec2(b.x+1,b.y+1),IM_COL32(0,0,0,220));
                dl->AddRectFilled(a,b,IM_COL32(55,55,55,230));
                dl->AddRectFilled(a,ImVec2(a.x+46*fraction,b.y),IM_COL32(int(255*(1-fraction)),int(220*fraction),45,255));
            }
            std::string label=options.esp_names?ssne::mod_type_name(true,m.type):"";
            if(options.esp_distance) { char distance[32]; snprintf(distance,sizeof(distance)," %.0f m",m.distance); label+=distance; }
            if(options.esp_health) { char hp[48]; snprintf(hp,sizeof(hp)," [%.0f / %.0f HP]",m.health,m.max_health); label+=hp; }
            if(!label.empty()) {
                dl->AddText(ImVec2(p.x+11,p.y+1),IM_COL32(0,0,0,255),label.c_str());
                dl->AddText(ImVec2(p.x+10,p.y),color,label.c_str());
            }
        }
        dl->PopClipRect();
    }
}
}
