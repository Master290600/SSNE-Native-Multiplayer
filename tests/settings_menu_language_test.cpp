#include "settings_menu_i18n.h"
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using port::settings_text::Row;
void require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}

bool utf8(std::string_view text){
    for(size_t i=0;i<text.size();){
        const auto lead=static_cast<unsigned char>(text[i++]);
        if(lead<0x80)continue;
        unsigned left=0,minimum=0,point=0;
        if((lead&0xe0)==0xc0){left=1;minimum=0x80;point=lead&31;}
        else if((lead&0xf0)==0xe0){left=2;minimum=0x800;point=lead&15;}
        else if((lead&0xf8)==0xf0){left=3;minimum=0x10000;point=lead&7;}
        else return false;
        if(left>text.size()-i)return false;
        while(left--){const auto next=static_cast<unsigned char>(text[i++]);if((next&0xc0)!=0x80)return false;point=(point<<6)|(next&63);}
        if(point<minimum || point>0x10ffff || (point>=0xd800 && point<=0xdfff))return false;
    }
    return true;
}
const Row& row(std::string_view key){
    for(const auto& entry:port::settings_text::rows)if(entry.en==key)return entry;
    throw std::runtime_error("Required menu text is absent: "+std::string(key));
}
std::string_view language_text(const Row& value,unsigned language){return language==1?value.de:language==2?value.fr:value.en;}

// Check ABI argument types/order, not prose containing literal percentages.
// Width/precision '*' consume int arguments; %% consumes no argument. A
// translator must not introduce a positional specifier or a new conversion.
struct Format {
    std::vector<std::string> arguments;
    unsigned escaped_percents=0;
    bool operator==(const Format&)const=default;
};
Format format(std::string_view text){
    Format result;
    const auto digit=[](char c){return c>='0' && c<='9';};
    for(size_t i=0;i<text.size();++i){
        if(text[i]!='%')continue;
        require(++i<text.size(),"Unterminated printf conversion");
        if(text[i]=='%'){++result.escaped_percents;continue;}
        while(i<text.size() && std::string_view("-+ #0'").find(text[i])!=std::string_view::npos)++i;
        if(i<text.size() && text[i]=='*'){result.arguments.push_back("width:int");++i;}
        else while(i<text.size() && digit(text[i]))++i;
        require(i==text.size() || text[i]!='$',"Unexpected positional printf argument");
        if(i<text.size() && text[i]=='.'){
            ++i;if(i<text.size() && text[i]=='*'){result.arguments.push_back("precision:int");++i;}
            else while(i<text.size() && digit(text[i]))++i;
        }
        std::string length;
        if(i+1<text.size() && (text.substr(i,2)=="hh" || text.substr(i,2)=="ll")){length=std::string(text.substr(i,2));i+=2;}
        else if(i<text.size() && std::string_view("hljztL").find(text[i])!=std::string_view::npos)length+=text[i++];
        require(i<text.size(),"Missing printf conversion type");
        const auto conversion=text[i];std::string type;
        if(conversion=='d' || conversion=='i')type="signed:"+length;
        else if(std::string_view("uoxX").find(conversion)!=std::string_view::npos)type="unsigned:"+length;
        else if(std::string_view("fFeEgGaA").find(conversion)!=std::string_view::npos)type=length=="L"?"long double":"double";
        else if(conversion=='c')type=length=="l"?"wide char":"int char";
        else if(conversion=='s')type=length=="l"?"wide string":"char string";
        else if(conversion=='p')type="pointer";
        else if(conversion=='n')type="count pointer:"+length;
        else throw std::runtime_error("Unknown printf conversion in: "+std::string(text));
        result.arguments.push_back(std::move(type));
    }
    return result;
}

std::vector<std::string_view> entries(std::string_view items){
    std::vector<std::string_view> result;size_t at=0;
    while(at<items.size() && items[at]){
        const auto end=items.find('\0',at);require(end!=std::string_view::npos,"Combo entry lacks a separator");
        result.push_back(items.substr(at,end-at));at=end+1;
    }
    require(at<items.size() && items[at]=='\0' && at+1==items.size(),"Combo list has no final terminator or contains trailing data");
    return result;
}

void catalog_checks(){
    std::set<std::string_view> keys;size_t count=0;
    for(const auto& entry:port::settings_text::rows){
        ++count;require(keys.insert(entry.en).second,"Duplicate English lookup key: "+std::string(entry.en));
        for(unsigned language=0;language<3;++language){
            const auto value=language_text(entry,language);
            require(value.find_first_not_of(" \t\r\n")!=std::string_view::npos,"Empty menu translation: "+std::string(entry.en));
            require(value.find('\0')==std::string_view::npos,"Embedded NUL in a menu catalog row");
            require(value.find("##")==std::string_view::npos,"Widget ID marker leaked into translated visible text");
            require(utf8(value),"Invalid UTF-8 in menu catalog: "+std::string(entry.en));
            require(port::settings_text::translate(entry.en,language)==value,"Menu language resolves to the wrong catalog column");
        }
    }
    require(count>0,"The F12 catalog is empty");
}
void id_checks(){
    // English labels may gain a stable suffix; their identity must then remain
    // the same in every locale, for every catalog-backed visible label.
    for(ImGuiID seed:{ImGuiID(0),ImGuiID(0x12345678),ImGuiID(0xfedcba98)}){
        for(const auto& entry:port::settings_text::rows){
            const std::string original(entry.en);port::menu_ui::set_language(0);
            const auto english=port::menu_ui::label(original.c_str());const auto expected=ImHashStr(english.c_str(),0,seed);
            for(unsigned language=1;language<3;++language){
                port::menu_ui::set_language(language);const auto localized=port::menu_ui::label(original.c_str());
                require(ImHashStr(localized.c_str(),0,seed)==expected,"Translation changes widget identity: "+original);
            }
        }
        // These IDs predate localization and must keep the original ImGui hash.
        for(const char* original:{"Use custom PNG###crosshair_enabled","Texture###crosshair_file","Refresh###crosshair_reload",
                                 "Size###crosshair_size","Opacity###crosshair_opacity","Use original color###crosshair_native_color",
                                 "Use original crosshair###crosshair_original","Game and menu language###menu_language"}){
            const auto marker=std::string_view(original).find("###");const auto expected=ImHashStr(original,0,seed);
            for(unsigned language=0;language<3;++language){
                port::menu_ui::set_language(language);const auto localized=port::menu_ui::label(original);
                require(localized.ends_with(std::string_view(original).substr(marker)),"Explicit widget ID suffix changed");
                require(ImHashStr(localized.c_str(),0,seed)==expected,"Existing explicit ImGui identity changed");
            }
        }
        for(const char* original:{"##crosshair_entry","##keys","##kb","##pad","##sel","###opaque_identity"})
            for(unsigned language=0;language<3;++language){port::menu_ui::set_language(language);const auto value=port::menu_ui::label(original);require(value==original && ImHashStr(value.c_str(),0,seed)==ImHashStr(original,0,seed),"An invisible widget ID was translated");}
    }
}
void format_checks(){
    // This list is deliberately limited to actual format strings in F12,
    // including the two-stage FOV slider format. Help prose such as "100% is
    // the game's own speed" is displayed through %s and is not parsed here.
    for(const char* source:{"%s","%s %s","%d","%d%%","%%d deg (original)  - %.0f horizontal","%%d deg  - %.0f horizontal",
                           "Your GPU allows MSAA %dx at most here.","Your GPU allows %dx at most.","Now rendering at %dx (%dx%d)",
                           "Now rendering at %dx%d"," -  GPU %.1f ms per frame","GPU %.1f ms per frame%s","Running: %s",
                           "Controller %d","Controller: %s","Slot %d","Folder: %s"}){
        const auto signature=format(source);
        for(unsigned language=0;language<3;++language){port::menu_ui::set_language(language);require(format(port::menu_ui::text(source))==signature,"Translation changes printf argument types/count: "+std::string(source));}
    }
}
void help_and_combo_checks(){
    // Match the complete adjacent-literal key used by help(), including its
    // newlines. Partial lookup of one source line must not silently replace it.
    const char* help="The game times gameplay by real elapsed time, so it plays at the same speed at any frame rate.\n"
                     "Menus, the pause screen, cutscenes and vehicles always run at 60 (their animations and the jeep's\n"
                     "physics depend on the original frame rate).\n"
                     "Unlimited is still capped by your monitor when vertical sync is on.";
    const auto& help_row=row(help);
    const char* combo="Hidden\0Frame rate only\0Full (frame rate, key hints)\0";
    const std::array<std::string_view,3> keys{"Hidden","Frame rate only","Full (frame rate, key hints)"};
    const char* resampling="Original (linear, as on GameCube)\0High quality (band-limited)\0";
    const std::array<std::string_view,2> resampling_keys{"Original (linear, as on GameCube)","High quality (band-limited)"};
    for(unsigned language=0;language<3;++language){
        port::menu_ui::set_language(language);
        require(std::string_view(port::menu_ui::text(help))==language_text(help_row,language),"Adjacent-literal help failed complete-key localization");
        for(unsigned list=0;list<2;++list){
            const auto encoded=port::menu_ui::combo_items(list?resampling:combo);
            require(encoded.size()>=2 && encoded[encoded.size()-1]=='\0' && encoded[encoded.size()-2]=='\0',"Nonempty combo list lost double-NUL termination");
            const auto parsed=entries(encoded);require(parsed.size()==(list?resampling_keys.size():keys.size()),"Translated combo changed its selection indices");
            for(size_t i=0;i<parsed.size();++i){const auto key=list?resampling_keys[i]:keys[i];require(parsed[i]==language_text(row(key),language),"Translated combo merged, split or reordered an entry");}
        }
    }
}
void fallback_checks(){
    for(unsigned language=0;language<3;++language){
        port::menu_ui::set_language(language);
        for(const std::string original:{"my reticle ###file #=100% \xc3\x9c.png","Unlisted Bluetooth Controller X200","Monitor #2: Unlisted Display","unknown-format-%s-100%%"}){
            require(std::string_view(port::menu_ui::text(original.c_str()))==original,"User filename/device text was translated or treated as a format");
            require(port::settings_text::translate(original,language)==original,"Unknown catalog key did not fall back verbatim");
        }
    }
    port::menu_ui::set_language(99);require(std::string_view(port::menu_ui::text("Crosshair"))=="Crosshair","Invalid display language must fall back to English");
    require(port::settings_text::translate("Crosshair",99)=="Crosshair","Invalid catalog language must fall back to English");
}
}
int main()try{
    catalog_checks();id_checks();format_checks();help_and_combo_checks();fallback_checks();
    std::cout<<"F12 catalog integrity, UTF-8, locale-independent ImGui identities, printf signatures, complete help/combo text and literal filename/device fallback passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
