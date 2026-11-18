#include "chat_overlay.h"
#include "coop/chat.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>

namespace port {
namespace {
constexpr size_t text_limit=256,history_limit=64;
constexpr double visible_seconds=5,fade_seconds=1;
double seconds(){return double(SDL_GetTicks64())/1000.;}
void text_shadow(ImDrawList* draw,ImVec2 position,ImU32 color,const char* text,float wrap_width=0){
    const auto alpha=(color>>IM_COL32_A_SHIFT)&255u;
    draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),ImVec2(position.x+1,position.y+1),
        IM_COL32(0,0,0,unsigned(alpha*.85f)),text,nullptr,wrap_width);
}
size_t previous(const std::string& s,size_t position){
    if(!position)return 0;--position;
    while(position && (static_cast<unsigned char>(s[position])&0xc0)==0x80)--position;
    return position;
}
size_t next(const std::string& s,size_t position){
    if(position>=s.size())return s.size();++position;
    while(position<s.size() && (static_cast<unsigned char>(s[position])&0xc0)==0x80)++position;
    return position;
}
// SDL commits UTF-8. Clipboard text is checked too, so truncation never leaves
// a partial scalar, control sequence or invalid byte in the bounded draft.
std::string clean_text(std::string_view text,size_t limit){
    std::string result;
    for(size_t offset=0;offset<text.size();){
        const auto first=static_cast<unsigned char>(text[offset]);size_t count=1;uint32_t value=first,minimum=0;
        if(first>=0xc2 && first<=0xdf){count=2;value=first&0x1f;minimum=0x80;}
        else if(first>=0xe0 && first<=0xef){count=3;value=first&0x0f;minimum=0x800;}
        else if(first>=0xf0 && first<=0xf4){count=4;value=first&7;minimum=0x10000;}
        else if(first>=0x80){++offset;continue;}
        if(offset+count>text.size())break;
        bool valid=true;
        for(size_t i=1;i<count;++i){const auto byte=static_cast<unsigned char>(text[offset+i]);if((byte&0xc0)!=0x80){valid=false;break;}value=(value<<6)|(byte&0x3f);}
        if(!valid || value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)){++offset;continue;}
        if(value=='\n' || value=='\r' || value=='\t'){
            if(result.size()<limit && (result.empty() || result.back()!=' '))result+=' ';
        }else if(value>=0x20 && value!=0x7f && !(value>=0x80 && value<=0x9f)){
            if(result.size()+count>limit)break;result.append(text.substr(offset,count));
        }
        offset+=count;
    }
    return result;
}
bool blank(const std::string& text){return std::all_of(text.begin(),text.end(),[](unsigned char c){return c==' ' || c=='\t' || c=='\r' || c=='\n';});}
std::string opening_text(SDL_Keycode symbol){
    const auto c=uint32_t(symbol);if(c<0x20 || c>0x10ffff || (c>=0xd800 && c<=0xdfff))return {};
    std::string out;
    if(c<0x80)out+=char(c);
    else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
    else if(c<0x10000){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    return out;
}
const char* localized(unsigned language,const char* en,const char* de,const char* fr){return language==1?de:language==2?fr:en;}
std::string json_string(const std::string& text){
    std::string out="\"";constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:text){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}return out+'"';
}
}
void ChatOverlay::erase_selection(){
    const auto first=std::min(cursor_,anchor_),last=std::max(cursor_,anchor_);
    draft_.erase(first,last-first);cursor_=anchor_=first;
}
void ChatOverlay::insert_text(const std::string& text){
    erase_selection();const auto inserted=clean_text(text,text_limit-draft_.size());
    draft_.insert(cursor_,inserted);cursor_+=inserted.size();anchor_=cursor_;composition_.clear();
}
void ChatOverlay::close(){
    active_=false;suppress_open_text_=false;draft_.clear();composition_.clear();cursor_=anchor_=0;input_scroll_=0;error_until_=0;
    opening_key_=SDL_SCANCODE_UNKNOWN;opening_text_.clear();opening_upper_text_.clear();
}
void ChatOverlay::reset_session(){
    close();submissions_.clear();last_submitted_.clear();history_.clear();last_sequence_=0;scroll_to_bottom_=false;last_trace_=0;
}
void ChatOverlay::send_failed(){
    active_=true;draft_=last_submitted_;cursor_=anchor_=draft_.size();composition_.clear();input_scroll_=0;error_until_=seconds()+4;scroll_to_bottom_=true;
}
std::optional<std::string> ChatOverlay::take_submission(){
    if(submissions_.empty())return {};auto text=std::move(submissions_.front());submissions_.pop_front();last_submitted_=text;return text;
}
bool ChatOverlay::handle_event(const SDL_Event& event,bool multiplayer_available,bool can_open){
    return handle_event(event,multiplayer_available,can_open,event.type==SDL_KEYDOWN && event.key.keysym.scancode==SDL_SCANCODE_T);
}
bool ChatOverlay::handle_event(const SDL_Event& event,bool multiplayer_available,bool can_open,bool open_requested){
    if(!multiplayer_available){const bool was_active=active_;close();submissions_.clear();return was_active && (event.type==SDL_KEYDOWN || event.type==SDL_KEYUP || event.type==SDL_TEXTINPUT || event.type==SDL_TEXTEDITING);}
    if(!active_){
        if(can_open && open_requested && ((event.type==SDL_KEYDOWN && !event.key.repeat && !(event.key.keysym.mod&(KMOD_CTRL|KMOD_ALT|KMOD_GUI))) || event.type==SDL_MOUSEBUTTONDOWN)){
            active_=true;draft_.clear();cursor_=anchor_=0;input_scroll_=0;error_until_=0;composition_.clear();open_timestamp_=event.common.timestamp;
            opening_key_=event.type==SDL_KEYDOWN?event.key.keysym.scancode:SDL_SCANCODE_UNKNOWN;
            auto symbol=event.type==SDL_KEYDOWN?(event.key.keysym.sym?event.key.keysym.sym:SDL_GetKeyFromScancode(opening_key_)):SDLK_UNKNOWN;
            // The legacy editor API can receive scancode-only synthetic events
            // without an initialized video keyboard map.
            if(!symbol && opening_key_>=SDL_SCANCODE_A && opening_key_<=SDL_SCANCODE_Z)symbol=SDLK_a+(opening_key_-SDL_SCANCODE_A);
            opening_text_=opening_text(symbol);
            const auto upper=(symbol>='a' && symbol<='z') || (symbol>=0xe0 && symbol<=0xfe && symbol!=0xf7)?symbol-32:symbol;
            opening_upper_text_=opening_text(upper);suppress_open_text_=!opening_text_.empty();scroll_to_bottom_=true;return true;
        }
        return false;
    }
    switch(event.type){
    case SDL_TEXTINPUT:
        if(suppress_open_text_ && uint32_t(event.text.timestamp-open_timestamp_)<=100 && (std::string_view(event.text.text)==opening_text_ || std::string_view(event.text.text)==opening_upper_text_)){suppress_open_text_=false;return true;}
        suppress_open_text_=false;insert_text(event.text.text);return true;
    case SDL_TEXTEDITING:composition_=clean_text(event.edit.text,96);return true;
    case SDL_KEYUP:
        if(event.key.keysym.scancode==opening_key_)suppress_open_text_=false;
        return true;
    case SDL_KEYDOWN:{
        const auto key=event.key.keysym.scancode;const bool control=(event.key.keysym.mod&(KMOD_CTRL|KMOD_GUI))!=0,shift=(event.key.keysym.mod&KMOD_SHIFT)!=0;
        if(key==SDL_SCANCODE_ESCAPE){close();return true;}
        if(key==SDL_SCANCODE_RETURN || key==SDL_SCANCODE_KP_ENTER){
            if(event.key.repeat)return true;
            if(!blank(draft_)) {
                if(submissions_.size()>=4){error_until_=seconds()+4;return true;}
                submissions_.push_back(draft_);
            }
            close();return true;
        }
        if(control && key==SDL_SCANCODE_A){anchor_=0;cursor_=draft_.size();return true;}
        if(control && (key==SDL_SCANCODE_C || key==SDL_SCANCODE_X)){
            if(cursor_!=anchor_){const auto selected=draft_.substr(std::min(cursor_,anchor_),std::max(cursor_,anchor_)-std::min(cursor_,anchor_));SDL_SetClipboardText(selected.c_str());if(key==SDL_SCANCODE_X)erase_selection();}
            return true;
        }
        if(control && key==SDL_SCANCODE_V){if(char* clipboard=SDL_GetClipboardText()){insert_text(clipboard);SDL_free(clipboard);}return true;}
        if(key==SDL_SCANCODE_BACKSPACE){if(cursor_!=anchor_)erase_selection();else if(cursor_){anchor_=previous(draft_,cursor_);erase_selection();}return true;}
        if(key==SDL_SCANCODE_DELETE){if(cursor_!=anchor_)erase_selection();else if(cursor_<draft_.size()){anchor_=next(draft_,cursor_);erase_selection();}return true;}
        if(key==SDL_SCANCODE_LEFT || key==SDL_SCANCODE_RIGHT || key==SDL_SCANCODE_HOME || key==SDL_SCANCODE_END){
            if(!shift && cursor_!=anchor_ && (key==SDL_SCANCODE_LEFT || key==SDL_SCANCODE_RIGHT))cursor_=key==SDL_SCANCODE_LEFT?std::min(cursor_,anchor_):std::max(cursor_,anchor_);
            else if(key==SDL_SCANCODE_HOME)cursor_=0;
            else if(key==SDL_SCANCODE_END)cursor_=draft_.size();
            else if(key==SDL_SCANCODE_LEFT){cursor_=previous(draft_,cursor_);if(control)while(cursor_ && draft_[previous(draft_,cursor_)]!=' ')cursor_=previous(draft_,cursor_);}
            else {cursor_=next(draft_,cursor_);if(control)while(cursor_<draft_.size() && draft_[cursor_]!=' ')cursor_=next(draft_,cursor_);}
            if(!shift)anchor_=cursor_;composition_.clear();return true;
        }
        return true; // Never turn typed letters, gameplay hotkeys or pad binds into game actions.
    }
    case SDL_MOUSEBUTTONDOWN:case SDL_MOUSEBUTTONUP:case SDL_MOUSEMOTION:case SDL_MOUSEWHEEL:
    case SDL_CONTROLLERBUTTONDOWN:case SDL_CONTROLLERBUTTONUP:case SDL_CONTROLLERAXISMOTION:return true;
    case SDL_WINDOWEVENT:if(event.window.event==SDL_WINDOWEVENT_FOCUS_LOST)close();return false;
    default:return false;
    }
}
void ChatOverlay::input_field(unsigned language,ImVec2& minimum,ImVec2& maximum){
    const float height=ImGui::GetTextLineHeight()+12,width=ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##chat_input",ImVec2(width,height));minimum=ImGui::GetItemRectMin();maximum=ImGui::GetItemRectMax();
    const float cursor_width=ImGui::CalcTextSize(draft_.data(),draft_.data()+cursor_).x,visible_width=std::max(1.f,width-12);
    if(cursor_width-input_scroll_>visible_width-2)input_scroll_=cursor_width-visible_width+2;
    if(cursor_width-input_scroll_<0)input_scroll_=cursor_width;
    input_scroll_=std::max(0.f,input_scroll_);
    const float text_x=minimum.x+6-input_scroll_,text_y=minimum.y+6;
    if(ImGui::IsItemHovered())ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if(ImGui::IsItemClicked() || (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))){
        const auto mouse=ImGui::GetIO().MousePos;const float wanted=mouse.x-text_x;size_t chosen=0;
        for(size_t offset=0;offset<draft_.size();){const auto after=next(draft_,offset);const float left=ImGui::CalcTextSize(draft_.data(),draft_.data()+offset).x,right=ImGui::CalcTextSize(draft_.data(),draft_.data()+after).x;if(wanted<(left+right)*.5f){chosen=offset;break;}chosen=after;offset=after;}
        cursor_=chosen;if(ImGui::IsItemClicked() && !ImGui::GetIO().KeyShift)anchor_=cursor_;
    }
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(minimum.x,maximum.y-1),ImVec2(maximum.x,maximum.y-1),IM_COL32(211,160,64,110));
    draw->PushClipRect(ImVec2(minimum.x+4,minimum.y),ImVec2(maximum.x-4,maximum.y),true);
    if(cursor_!=anchor_){const float a=ImGui::CalcTextSize(draft_.data(),draft_.data()+std::min(cursor_,anchor_)).x,b=ImGui::CalcTextSize(draft_.data(),draft_.data()+std::max(cursor_,anchor_)).x;draw->AddRectFilled(ImVec2(text_x+a,text_y),ImVec2(text_x+b,text_y+ImGui::GetTextLineHeight()),IM_COL32(61,104,165,180));}
    if(draft_.empty() && composition_.empty()){
        const auto placeholder=localized(language,"Message...","Nachricht...","Message...");
        text_shadow(draw,ImVec2(minimum.x+6,text_y),IM_COL32(145,148,156,255),placeholder);
        draw->AddText(ImVec2(minimum.x+6,text_y),IM_COL32(145,148,156,255),placeholder);
    }else{
        text_shadow(draw,ImVec2(text_x,text_y),IM_COL32(245,245,245,255),draft_.c_str());
        draw->AddText(ImVec2(text_x,text_y),IM_COL32(245,245,245,255),draft_.c_str());
    }
    const float caret=ImGui::CalcTextSize(draft_.data(),draft_.data()+cursor_).x;
    if(!composition_.empty()){
        text_shadow(draw,ImVec2(text_x+caret,text_y),IM_COL32(225,198,126,255),composition_.c_str());
        draw->AddText(ImVec2(text_x+caret,text_y),IM_COL32(225,198,126,255),composition_.c_str());
        draw->AddLine(ImVec2(text_x+caret,text_y+ImGui::GetTextLineHeight()),ImVec2(text_x+caret+ImGui::CalcTextSize(composition_.c_str()).x,text_y+ImGui::GetTextLineHeight()),IM_COL32(225,198,126,255));
    }else if(std::fmod(seconds(),1.)<.6)draw->AddLine(ImVec2(text_x+caret,text_y),ImVec2(text_x+caret,text_y+ImGui::GetTextLineHeight()),IM_COL32(255,229,162,255));
    draw->PopClipRect();SDL_Rect ime{int(minimum.x),int(minimum.y),int(maximum.x-minimum.x),int(maximum.y-minimum.y)};SDL_SetTextInputRect(&ime);
}
void ChatOverlay::draw(const std::vector<coop::ChatMessage>& messages,unsigned language,ImVec2 origin,ImVec2 image_size){
    const double now=seconds();bool added=false;
    for(const auto& message:messages)if(message.sequence>last_sequence_){
        history_.push_back({message.sequence,message.sender.slot,clean_text(message.name,64),clean_text(message.text,text_limit),now});last_sequence_=message.sequence;added=true;
        while(history_.size()>history_limit)history_.pop_front();
    }
    std::vector<const Line*> visible;
    if(active_)for(const auto& line:history_)visible.push_back(&line);
    else {for(const auto& line:history_)if(now-line.received<visible_seconds)visible.push_back(&line);if(visible.size()>7)visible.erase(visible.begin(),visible.end()-7);}
    ImVec2 input_min{},input_max{};
    if(!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(image_size.x) || !std::isfinite(image_size.y) || image_size.x<80 || image_size.y<80){write_trace(visible,input_min,input_max);return;}
    if(visible.empty() && !active_){write_trace(visible,input_min,input_max);return;}
    const char* controls=localized(language,"Enter: send   Esc: cancel","Enter: senden   Esc: abbrechen","Entrée : envoyer   Échap : annuler");
    const char* failure=localized(language,"Could not send. Wait briefly and try again.","Senden nicht möglich. Kurz warten und erneut versuchen.","Envoi impossible. Attendez un instant puis réessayez.");
    const float margin=std::min(12.f,image_size.x*.03f),available_width=image_size.x-2*margin;
    const float width=std::min(430.f,available_width);
    const float line_height=ImGui::GetTextLineHeightWithSpacing(),wrap_width=std::max(12.f,width-20);
    const float spacing=ImGui::GetStyle().ItemSpacing.y;
    float text_height=0;for(const auto* line:visible){const auto text=line->name+": "+line->text;text_height+=ImGui::CalcTextSize(text.c_str(),nullptr,false,wrap_width).y+spacing;}
    const bool error=active_ && now<error_until_;
    const float footer=active_?ImGui::GetTextLineHeight()+12+ImGui::CalcTextSize(controls,nullptr,false,wrap_width).y+spacing*2+(error?ImGui::CalcTextSize(failure,nullptr,false,wrap_width).y+spacing:0):0.f;
    const float maximum_height=std::min(image_size.y-2*margin,std::max(footer+line_height+16,std::min(310.f,image_size.y*.48f)));
    const float height=std::min(maximum_height,std::max(active_?line_height*6:line_height+12,text_height+footer+16));
    ImGui::SetNextWindowPos(ImVec2(origin.x+margin,origin.y+margin),ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width,height),ImGuiCond_Always);ImGui::SetNextWindowBgAlpha(0.f);
    const auto flags=ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoFocusOnAppearing|(active_?0:ImGuiWindowFlags_NoInputs);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10,8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);ImGui::Begin("##multiplayer_chat",nullptr,flags);
    const float history_height=std::max(line_height,ImGui::GetContentRegionAvail().y-footer);
    const auto child_flags=ImGuiWindowFlags_NoBackground|(active_?ImGuiWindowFlags_None:ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginChild("##chat_history",ImVec2(0,history_height),false,child_flags);
    const bool at_bottom=ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-4;
    ImGui::PushTextWrapPos(0);
    float newest_alpha=0;
    for(const auto* line:visible){
        const float alpha=active_?1.f:float(std::clamp((visible_seconds-(now-line->received))/fade_seconds,0.,1.));
        newest_alpha=std::max(newest_alpha,alpha);
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(line->slot==0?1.f:.87f,line->slot==0?.85f:.94f,line->slot==0?.50f:1.f,alpha));
        const auto text=line->name+": "+line->text;
        text_shadow(ImGui::GetWindowDrawList(),ImGui::GetCursorScreenPos(),ImGui::GetColorU32(ImGuiCol_Text),text.c_str(),ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(text.c_str());ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();if(scroll_to_bottom_ || (!active_ && added) || (added && at_bottom))ImGui::SetScrollHereY(1);scroll_to_bottom_=false;ImGui::EndChild();
    if(active_){
        input_field(language,input_min,input_max);
        ImGui::PushTextWrapPos(0);
        text_shadow(ImGui::GetWindowDrawList(),ImGui::GetCursorScreenPos(),ImGui::GetColorU32(ImGuiCol_TextDisabled),controls,ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s",controls);
        if(error){
            text_shadow(ImGui::GetWindowDrawList(),ImGui::GetCursorScreenPos(),ImGui::GetColorU32(ImGuiCol_Text),failure,ImGui::GetContentRegionAvail().x);
            ImGui::TextWrapped("%s",failure);
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();ImGui::PopStyleVar(2);write_trace(visible,input_min,input_max,true,newest_alpha);
}
void ChatOverlay::write_trace(const std::vector<const Line*>& visible,ImVec2 minimum,ImVec2 maximum,bool drawn,float newest_alpha){
    const auto hidden=std::getenv("SSNE_TEST_HIDDEN");if(!hidden || std::string_view(hidden)!="1" || !std::getenv("SSNE_TEST_CHAT"))return;
    const auto tick=SDL_GetTicks();if(last_trace_ && uint32_t(tick-last_trace_)<40)return;last_trace_=tick;
    std::ofstream out("chat-ui-state.json.pending",std::ios::binary|std::ios::trunc);
    out<<"{\"window_drawn\":"<<(drawn?"true":"false")<<",\"newest_alpha\":"<<newest_alpha<<",\"active\":"<<(active_?"true":"false")<<",\"draft\":"<<json_string(draft_)<<",\"cursor\":"<<cursor_<<",\"input_rect\":["<<minimum.x<<','<<minimum.y<<','<<maximum.x<<','<<maximum.y<<"],\"messages\":[";
    bool comma=false;for(const auto& line:history_){if(comma)out<<',';comma=true;out<<"{\"sequence\":"<<line.sequence<<",\"name\":"<<json_string(line.name)<<",\"text\":"<<json_string(line.text)<<'}';}
    out<<"],\"visible_sequences\":[";comma=false;for(const auto* line:visible){if(comma)out<<',';comma=true;out<<line->sequence;}out<<"],\"last_submission\":"<<json_string(last_submitted_)<<",\"send_failed\":"<<(seconds()<error_until_?"true":"false")<<'}';out.close();
    if(out){std::remove("chat-ui-state.json");std::rename("chat-ui-state.json.pending","chat-ui-state.json");}
}
}
