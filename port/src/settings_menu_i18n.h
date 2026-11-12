#pragma once
#include "settings_menu_text.h"
#include <imgui.h>
#include <cstdarg>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Display-thread helpers. Original English labels remain the stable widget
// identity; an explicit ### suffix (e.g. Crosshair) is preserved verbatim.
namespace port::menu_ui {
inline thread_local unsigned current_language=0;
inline void set_language(unsigned value){current_language=value<3?value:0;}
inline const char* text(const char* value){return settings_text::translate(value?value:"",current_language).data();}
inline std::string message(std::string_view value){
    const auto translated=settings_text::translate(value,current_language);
    if(translated!=value)return std::string(translated);
    for(std::string_view prefix:{"Cannot create the crosshairs folder: ","Cannot read the crosshairs folder: ","Cannot open PNG file: ","Cannot read PNG file: "})
        if(value.starts_with(prefix))return std::string(settings_text::translate(prefix,current_language))+std::string(value.substr(prefix.size()));
    return std::string(value);
}
inline std::string label(const char* value){
    const std::string_view original=value?value:"";const auto hidden=original.find("##");
    if(hidden==0)return std::string(original);
    const auto visible=original.substr(0,hidden);const auto translated=settings_text::translate(visible,current_language);
    std::string result(translated);
    if(visible.starts_with("Key ") && visible.size()>4 && visible[4]>='0' && visible[4]<='9')
        result=std::string(settings_text::translate("Key ",current_language))+std::string(visible.substr(4));
    if(hidden!=std::string_view::npos && original.substr(hidden,3)=="###")result+=original.substr(hidden);
    else{result+="###";result+=original;}
    return result;
}
inline bool Begin(const char* name,bool* open,ImGuiWindowFlags flags){const auto name_id=label(name);return ImGui::Begin(name_id.c_str(),open,flags);}
inline void SeparatorText(const char* value){ImGui::SeparatorText(text(value));}
inline void TextUnformatted(const char* value){ImGui::TextUnformatted(text(value));}
inline void Text(const char* fmt,...){va_list args;va_start(args,fmt);ImGui::TextV(text(fmt),args);va_end(args);}
inline void TextWrapped(const char* fmt,...){va_list args;va_start(args,fmt);ImGui::TextWrappedV(text(fmt),args);va_end(args);}
inline void TextDisabled(const char* fmt,...){va_list args;va_start(args,fmt);ImGui::TextDisabledV(text(fmt),args);va_end(args);}
inline void TextColored(const ImVec4& color,const char* fmt,...){va_list args;va_start(args,fmt);ImGui::TextColoredV(color,text(fmt),args);va_end(args);}
inline void SetTooltip(const char* fmt,...){va_list args;va_start(args,fmt);ImGui::SetTooltipV(text(fmt),args);va_end(args);}
inline bool SmallButton(const char* value){const auto id=label(value);return ImGui::SmallButton(id.c_str());}
inline bool Button(const char* value,const ImVec2& size=ImVec2(0,0)){const auto id=label(value);return ImGui::Button(id.c_str(),size);}
inline bool Checkbox(const char* value,bool* selected){const auto id=label(value);return ImGui::Checkbox(id.c_str(),selected);}
inline bool RadioButton(const char* value,bool selected){const auto id=label(value);return ImGui::RadioButton(id.c_str(),selected);}
inline bool SliderInt(const char* value,int* number,int low,int high,const char* format="%d",ImGuiSliderFlags flags=0){const auto id=label(value);return ImGui::SliderInt(id.c_str(),number,low,high,text(format),flags);}
inline bool BeginCombo(const char* value,const char* preview,ImGuiComboFlags flags=0){const auto id=label(value);return ImGui::BeginCombo(id.c_str(),text(preview),flags);}
inline bool Selectable(const char* value,bool selected=false,ImGuiSelectableFlags flags=0,const ImVec2& size=ImVec2(0,0)){const auto id=label(value);return ImGui::Selectable(id.c_str(),selected,flags,size);}
inline bool BeginTabItem(const char* value,bool* open=nullptr,ImGuiTabItemFlags flags=0){const auto id=label(value);return ImGui::BeginTabItem(id.c_str(),open,flags);}
inline void TableSetupColumn(const char* value,ImGuiTableColumnFlags flags=0,float width=0,ImGuiID user_id=0){ImGui::TableSetupColumn(text(value),flags,width,user_id);}
inline bool Combo(const char* value,int* selected,const char* const items[],int count,int height=-1){
    const auto id=label(value);std::vector<const char*> translated;translated.reserve(count);
    for(int i=0;i<count;++i)translated.push_back(text(items[i]));
    return ImGui::Combo(id.c_str(),selected,translated.data(),count,height);
}
inline std::string combo_items(const char* items){
    std::string translated;for(const char* p=items;p&&*p;p+=std::strlen(p)+1){translated+=text(p);translated+='\0';}
    translated+='\0';return translated;
}
inline bool Combo(const char* value,int* selected,const char* items,int height=-1){
    const auto id=label(value);const auto translated=combo_items(items);return ImGui::Combo(id.c_str(),selected,translated.c_str(),height);
}
}
