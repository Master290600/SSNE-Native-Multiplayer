#include "mod_menu.h"
#include "settings_menu.h"
#include "settings_menu_i18n.h"

#include <algorithm>
#include <cstdio>
#include <imgui.h>

namespace port {
namespace {
bool can_edit_shared(const ModMenuControls& controls) {
    return !controls.netplay || controls.host;
}

void apply_shared(const ModMenuControls& controls, const ssne::ModOptions& options) {
    if (!controls.netplay) ssne::mod_options(options);
    else if (controls.host && controls.apply) controls.apply(options);
}

bool slider_float(const char* name, float& value, float low, float high, const char* format) {
    return ImGui::SliderFloat(menu_ui::label(name).c_str(), &value, low, high, format);
}

struct CatalogChoice {
    unsigned id;
    const std::string& name;
    const char* note;
    bool available;
};

std::vector<CatalogChoice> spawn_catalog(const ssne::ModState& state, int category, bool global) {
    const auto& catalog = ssne::mod_catalog();
    std::vector<CatalogChoice> choices;
    if (global) {
        for (const auto& entry : catalog) {
            if (entry.enemy ? category != 0 : (category == 0 || entry.weapon != (category == 1))) continue;
            const bool loaded = std::any_of(state.templates.begin(), state.templates.end(), [&](const auto& t) {
                return t.enemy == entry.enemy && t.type == entry.type;
            });
            const bool available = !entry.availability_note && (!entry.enemy || loaded);
            const char* note = entry.availability_note;
            if (!note && entry.enemy && !loaded) note = "Not loaded in this level";
            choices.push_back({(entry.enemy ? 0x80000000u : 0x40000000u) | entry.type, entry.name, note, available});
        }
    } else {
        for (const auto& t : state.templates) {
            if (t.enemy != (category == 0)) continue;
            if (!t.enemy) {
                const bool weapon = std::any_of(catalog.begin(), catalog.end(), [&](const auto& entry) {
                    return !entry.enemy && entry.weapon && entry.type == t.type;
                });
                if (weapon != (category == 1)) continue;
            }
            choices.push_back({t.id, t.name, nullptr, true});
        }
    }
    return choices;
}

void show_spawn_status(const std::string& status) {
    if (status.empty() || status == "Ready. Select cheats or a spawn template." ||
        status == "Enter a level to use cheats and collect spawn templates.") return;
    unsigned placed = 0, requested = 0;
    if (std::sscanf(status.c_str(), "Spawned %u / %u.", &placed, &requested) == 2)
        menu_ui::Text("Spawned %u / %u.", placed, requested);
    else
        ImGui::TextWrapped("%s", menu_ui::text(status.c_str()));
}
} // namespace

void draw_rapid_fire(const ModMenuControls& controls, const ssne::ModState& state) {
    auto options = controls.netplay && controls.host ? controls.requested : state.options;
    const bool enabled = state.active && can_edit_shared(controls);
    ImGui::BeginDisabled(!enabled);
    bool changed = menu_ui::Checkbox("Rapid fire", &options.rapid_fire);
    menu_trace_cheat_widget("cheat_rapid", enabled);
    if (options.rapid_fire) {
        changed |= slider_float("Fire rate multiplier", options.fire_multiplier, 1, 10, "%.1fx");
        menu_trace_cheat_widget("cheat_fire_rate", enabled);
    }
    if (changed) apply_shared(controls, options);
    ImGui::EndDisabled();
}

void draw_spawn_controls(const ModMenuControls& controls, const ssne::ModState& state) {
    const bool enabled = state.active && can_edit_shared(controls);
    auto options = controls.netplay && controls.host ? controls.requested : state.options;
    ImGui::BeginDisabled(!enabled);
    if (menu_ui::Checkbox("Prevent regular enemy spawns", &options.no_enemies)) apply_shared(controls, options);
    menu_trace_cheat_widget("cheat_no_spawns", enabled);
    ImGui::EndDisabled();

    static bool global = true;
    static int category = 0, count = 1;
    static float distance = 6;
    static unsigned selected = 0;
    static ImGuiTextFilter search;
    const bool columns = ImGui::GetContentRegionAvail().x >= 570;
    const bool table = columns && ImGui::BeginTable("##spawn-columns", 2, ImGuiTableFlags_SizingStretchProp);
    if (table) {
        ImGui::TableSetupColumn("##list", ImGuiTableColumnFlags_WidthStretch, 1.45f);
        ImGui::TableSetupColumn("##placement", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
    }
    menu_ui::SeparatorText("Catalog");
    if (menu_ui::Checkbox("Show global catalog", &global)) selected = 0;
    if (menu_ui::Combo("Category", &category, "Enemies\0Weapons\0Items / ammunition\0")) selected = 0;
    const auto choices = spawn_catalog(state, category, global);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##catalog-search", menu_ui::text("Search by name"), search.InputBuf, sizeof(search.InputBuf))) {
        search.Build();
        selected = 0;
    }
    menu_trace_cheat_widget("cheat_catalog_search", true);
    if (ImGui::BeginListBox("##spawn-choices", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 7))) {
        bool matches = false;
        for (const auto& choice : choices) {
            if (!search.PassFilter(choice.name.c_str())) continue;
            matches = true;
            ImGui::PushID(choice.name.c_str());
            ImGui::PushID(static_cast<int>(choice.id));
            ImGui::BeginDisabled(!choice.available);
            if (ImGui::Selectable(choice.name.c_str(), selected && selected == choice.id)) selected = choice.id;
            if (choice.available) menu_trace_cheat_widget(("cheat_catalog_" + std::to_string(choice.id)).c_str(), true);
            if (choice.note && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                menu_ui::SetTooltip("%s", menu_ui::text(choice.note));
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::PopID();
        }
        if (!matches) menu_ui::TextDisabled("No matches");
        ImGui::EndListBox();
    }
    if (table) ImGui::TableSetColumnIndex(1);
    menu_ui::SeparatorText("Spawn");
    const auto choice = std::find_if(choices.begin(), choices.end(), [&](const auto& value) {
        return selected && value.available && value.id == selected;
    });
    if (choice != choices.end()) ImGui::TextWrapped("%s", choice->name.c_str());
    else menu_ui::TextWrapped("Select an entry in the catalog.");
    ImGui::BeginDisabled(!enabled);
    menu_ui::TextUnformatted("Count");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##spawn-count", &count, 1, 10);
    menu_ui::TextUnformatted("Distance ahead");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##spawn-distance", &distance, 3, 30, "%.1f m");
    ImGui::BeginDisabled(choice == choices.end());
    if (menu_ui::Button("Spawn")) {
        if (!controls.netplay) ssne::mod_spawn(selected, count, distance);
        else if (controls.host && controls.spawn) controls.spawn(selected, count, distance);
    }
    menu_trace_cheat_widget("cheat_spawn", enabled && choice != choices.end());
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (enabled) show_spawn_status(state.status);
    if (table) ImGui::EndTable();
}

void draw_aim_controls(const ModMenuControls& controls, const ssne::ModState& state) {
    auto options = state.options;
    const bool enabled = state.active && state.targeting_available;
    ImGui::BeginDisabled(!enabled);
    bool changed = menu_ui::Checkbox("Aimbot", &options.aimbot);
    menu_trace_cheat_widget("cheat_aimbot", enabled);
    if (options.aimbot) {
        changed |= menu_ui::Checkbox("Only while holding right mouse button", &options.aim_hold);
        changed |= slider_float("Aim cone", options.aim_fov, 1, 90, "%.0f°");
        changed |= slider_float("Aim speed", options.aim_speed, 1, 60, "%.0f");
        changed |= menu_ui::Combo("Aim point", &options.aim_point, "Model center\0Nearest bone to crosshair\0Head bone\0Torso bone\0Bone index\0");
        if (options.aim_point == 4) changed |= menu_ui::SliderInt("Bone index", &options.aim_bone, 0, 127);
        if (options.aim_point == 0) {
            changed |= menu_ui::Checkbox("Automatic aim height from model size", &options.auto_aim_height);
            changed |= slider_float(options.auto_aim_height ? "Height adjustment" : "Aim height (m)", options.aim_height, 0, 10, "%.1f");
        }
    }
    menu_ui::SeparatorText("Enemy markers");
    changed |= menu_ui::Checkbox("Enemy ESP (also through walls)", &options.esp);
    if (options.esp) {
        changed |= menu_ui::Checkbox("Enemy names", &options.esp_names);
        changed |= menu_ui::Checkbox("Health bars / HP", &options.esp_health);
    }
    changed |= menu_ui::Checkbox("Show enemy bones", &options.bone_esp);
    if (options.bone_esp) changed |= menu_ui::Checkbox("Bone names / indices", &options.bone_names);
    menu_ui::SeparatorText("Item markers");
    changed |= menu_ui::Checkbox("Item ESP (also through walls)", &options.item_esp);
    menu_trace_cheat_widget("cheat_item_esp", enabled);
    if (options.item_esp) {
        changed |= menu_ui::Checkbox("Weapons", &options.item_weapons);
        changed |= menu_ui::Checkbox("Health", &options.item_health);
        changed |= menu_ui::Checkbox("Armor", &options.item_armor);
        changed |= menu_ui::Checkbox("Secrets (undiscovered triggers)", &options.item_secrets);
    }
    menu_ui::SeparatorText("Range");
    changed |= slider_float("Target and marker range", options.range, 5, 500, "%.0f m");
    changed |= menu_ui::Checkbox("Show distance on markers", &options.esp_distance);
    ImGui::EndDisabled();
    if (changed) {
        if (!controls.netplay) ssne::mod_options(options);
        else if (controls.apply_local) controls.apply_local(options);
    }
    if (state.active && state.targeting_available) menu_ui::TextDisabled("Living enemies tracked: %u", state.tracked_enemies);
    if (state.active && !state.targeting_available) menu_ui::TextDisabled("Targeting is unavailable for this game build.");
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
