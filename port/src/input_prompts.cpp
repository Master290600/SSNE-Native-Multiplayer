#include "input_prompts.h"
#include <algorithm>
#include <cctype>
namespace port {
namespace {
ssne::InputGlyph glyph(const Bind& bind, SDL_GameControllerType type) {
    using S=ssne::GlyphShape;
    if(bind.type==Bind::MOUSE) {
        if(bind.code==SDL_BUTTON_LEFT)return {"",S::MouseLeft};
        if(bind.code==SDL_BUTTON_RIGHT)return {"",S::MouseRight};
        if(bind.code==SDL_BUTTON_MIDDLE)return {"",S::MouseMiddle};
        return {"M"+std::to_string(bind.code),S::Key};
    }
    if(bind.type==Bind::WHEEL)return {"",bind.code>0?S::WheelUp:S::WheelDown};
    if(bind.type==Bind::KEY) {
        auto code=SDL_Scancode(bind.code);
        switch(code) {
        case SDL_SCANCODE_SPACE:return {"SPACE",S::Key};
        case SDL_SCANCODE_RETURN:case SDL_SCANCODE_KP_ENTER:return {"ENT",S::Key};
        case SDL_SCANCODE_BACKSPACE:return {"BACK",S::Key};
        case SDL_SCANCODE_ESCAPE:return {"ESC",S::Key};
        case SDL_SCANCODE_LSHIFT:case SDL_SCANCODE_RSHIFT:return {"SHIFT",S::Key};
        case SDL_SCANCODE_LCTRL:case SDL_SCANCODE_RCTRL:return {"CTRL",S::Key};
        case SDL_SCANCODE_LALT:case SDL_SCANCODE_RALT:return {"ALT",S::Key};
        case SDL_SCANCODE_UP:return {"UP",S::Key};
        case SDL_SCANCODE_DOWN:return {"DOWN",S::Key};
        case SDL_SCANCODE_LEFT:return {"<",S::Key};
        case SDL_SCANCODE_RIGHT:return {">",S::Key};
        default:break;
        }
        std::string name=SDL_GetScancodeName(code);
        if(name.starts_with("Keypad "))name="N"+name.substr(7);
        name.erase(std::remove(name.begin(),name.end(),' '),name.end());
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::toupper(c));});
        return {name.empty()?"?":name,S::Key};
    }
    if(bind.type==Bind::PAD_AXIS) {
        if(bind.code==SDL_CONTROLLER_AXIS_TRIGGERLEFT)return {"LT",S::Pad};
        if(bind.code==SDL_CONTROLLER_AXIS_TRIGGERRIGHT)return {"RT",S::Pad};
        return {"STK",S::Pad};
    }
    if(bind.type==Bind::PAD_BUTTON) {
        bool ps=type==SDL_CONTROLLER_TYPE_PS3||type==SDL_CONTROLLER_TYPE_PS4||type==SDL_CONTROLLER_TYPE_PS5;
        bool nintendo=type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO||type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT||
                      type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT||type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
        switch(bind.code) {
        case SDL_CONTROLLER_BUTTON_A:return ps?ssne::InputGlyph{"",S::Cross}:ssne::InputGlyph{nintendo?"B":"A",S::Pad};
        case SDL_CONTROLLER_BUTTON_B:return ps?ssne::InputGlyph{"",S::Circle}:ssne::InputGlyph{nintendo?"A":"B",S::Pad};
        case SDL_CONTROLLER_BUTTON_X:return ps?ssne::InputGlyph{"",S::Square}:ssne::InputGlyph{nintendo?"Y":"X",S::Pad};
        case SDL_CONTROLLER_BUTTON_Y:return ps?ssne::InputGlyph{"",S::Triangle}:ssne::InputGlyph{nintendo?"X":"Y",S::Pad};
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:return {ps?"L1":nintendo?"L":"LB",S::Pad};
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:return {ps?"R1":nintendo?"R":"RB",S::Pad};
        case SDL_CONTROLLER_BUTTON_START:return {ps?"OPT":"MENU",S::Pad};
        case SDL_CONTROLLER_BUTTON_BACK:return {"BACK",S::Pad};
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:return {"L3",S::Pad};
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:return {"R3",S::Pad};
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:return {"<",S::Pad};
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:return {">",S::Pad};
        case SDL_CONTROLLER_BUTTON_DPAD_UP:return {"UP",S::Pad};
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:return {"DOWN",S::Pad};
        default:return {"?",S::Pad};
        }
    }
    return {"-",S::Key};
}
}
ssne::InputGlyphConfig input_prompt_config(const Settings& settings,bool controller,SDL_GameControllerType type) {
    ssne::InputGlyphConfig out;
    out.enabled=settings.prompts=="keyboard" || (settings.prompts=="auto" && !controller);
    constexpr Action actions[]={ACT_A,ACT_B,ACT_X,ACT_Y,ACT_Z,ACT_START,ACT_L,ACT_R};
    for(int i=0;i<ssne::GLYPH_COUNT;++i){const auto& keys=settings.kb[actions[i]];out.buttons[i]=glyph(keys[0].type!=Bind::NONE?keys[0]:keys[1],type);}
    out.menu_buttons=out.buttons;out.separate_menu=true;
    // Only keyboard jump bindings are also A in the actual menu input path.
    for(const auto& bind:settings.kb[ACT_L])if(bind.type==Bind::KEY){out.menu_buttons[ssne::GLYPH_A]=glyph(bind,type);break;}
    return out;
}
}
