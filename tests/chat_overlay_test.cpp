#include "chat_overlay.h"
#include <SDL.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
SDL_Event key(SDL_Scancode code,SDL_Keymod modifiers=KMOD_NONE,bool repeat=false,uint32_t timestamp=100){
    SDL_Event e{};e.type=SDL_KEYDOWN;e.key.timestamp=timestamp;e.key.keysym.scancode=code;e.key.keysym.mod=modifiers;e.key.repeat=repeat;return e;
}
void text(port::ChatOverlay& chat,const std::string& input,uint32_t timestamp=110){
    require(input.size()<SDL_TEXTINPUTEVENT_TEXT_SIZE,"Test text must fit one real SDL text event");
    SDL_Event e{};e.type=SDL_TEXTINPUT;e.text.timestamp=timestamp;std::memcpy(e.text.text,input.data(),input.size());
    require(chat.handle_event(e,true,true),"Chat text escaped to gameplay");
}
void press(port::ChatOverlay& chat,SDL_Scancode code,SDL_Keymod modifiers=KMOD_NONE){require(chat.handle_event(key(code,modifiers),true,true),"Active chat key escaped to gameplay");}
void open(port::ChatOverlay& chat){
    require(chat.handle_event(key(SDL_SCANCODE_T),true,true) && chat.active(),"T did not open the connected chat");
    text(chat,"t",100); // SDL emits this for the same physical opening key.
    SDL_Event release{};release.type=SDL_KEYUP;release.key.timestamp=101;release.key.keysym.scancode=SDL_SCANCODE_T;
    require(chat.handle_event(release,true,true),"Opening T release escaped to gameplay");
}
std::string submit(port::ChatOverlay& chat){
    press(chat,SDL_SCANCODE_RETURN);require(!chat.active(),"Enter did not close the submitted chat");
    auto result=chat.take_submission();require(result.has_value(),"Submitted chat disappeared");return *result;
}
void event_routing(){
    port::ChatOverlay chat;
    require(!chat.handle_event(key(SDL_SCANCODE_T),false,true) && !chat.active(),"Offline T opened chat");
    require(!chat.handle_event(key(SDL_SCANCODE_T),true,false) && !chat.active(),"A gameplay/menu gate was ignored");
    require(!chat.handle_event(key(SDL_SCANCODE_T,KMOD_NONE,true),true,true),"Repeated T opened chat");
    require(!chat.handle_event(key(SDL_SCANCODE_T,KMOD_CTRL),true,true),"A modified gameplay shortcut opened chat");
    open(chat);text(chat,"Hällo 100% ###");
    press(chat,SDL_SCANCODE_W);press(chat,SDL_SCANCODE_SPACE);press(chat,SDL_SCANCODE_F12);
    SDL_Event mouse{};mouse.type=SDL_MOUSEBUTTONDOWN;require(chat.handle_event(mouse,true,true),"Active chat leaked a fire click");
    SDL_Event pad{};pad.type=SDL_CONTROLLERBUTTONDOWN;require(chat.handle_event(pad,true,true),"Active chat leaked a controller button");
    require(submit(chat)=="Hällo 100% ###","Opening T leaked, or literal user text changed");
    open(chat);text(chat,"cancel me");press(chat,SDL_SCANCODE_ESCAPE);
    require(!chat.active() && !chat.take_submission(),"Escape sent the cancelled draft");
    open(chat);text(chat,"disconnect me");chat.handle_event(key(SDL_SCANCODE_A),false,true);
    require(!chat.active() && !chat.take_submission(),"A disconnected draft remained live");
}
void utf8_editor(){
    port::ChatOverlay chat;open(chat);text(chat,"Aä🙂");
    press(chat,SDL_SCANCODE_BACKSPACE);press(chat,SDL_SCANCODE_LEFT);press(chat,SDL_SCANCODE_DELETE);text(chat,"ö");
    press(chat,SDL_SCANCODE_HOME);press(chat,SDL_SCANCODE_RIGHT);press(chat,SDL_SCANCODE_END,KMOD_SHIFT);text(chat,"é");
    require(submit(chat)=="Aé","UTF-8 cursor/delete/selection split a character");
    open(chat);text(chat,"will be replaced");press(chat,SDL_SCANCODE_A,KMOD_CTRL);text(chat,"neu");
    require(submit(chat)=="neu","Ctrl+A did not replace the whole draft");
    open(chat);text(chat,"abc");press(chat,SDL_SCANCODE_A,KMOD_CTRL);press(chat,SDL_SCANCODE_BACKSPACE);press(chat,SDL_SCANCODE_RETURN);
    require(!chat.active() && !chat.take_submission(),"An empty draft produced a message");
    open(chat);text(chat,std::string("a\xff")+"b");
    require(submit(chat)=="ab","Invalid clipboard/text bytes reached a submission");
    open(chat);text(chat,"a\nb\tc");require(submit(chat)=="a b c","Multiline text was not flattened safely");
}
void byte_limit(){
    port::ChatOverlay chat;open(chat);
    for(unsigned i=0;i<8;++i)text(chat,std::string(30,'x'));
    std::string accents;for(unsigned i=0;i<8;++i)accents+="ä";text(chat,accents);text(chat,"🙂");
    press(chat,SDL_SCANCODE_BACKSPACE);text(chat,"🙂");text(chat,"zz");
    std::string expected(240,'x');for(unsigned i=0;i<7;++i)expected+="ä";expected+="zz";
    const auto actual=submit(chat);require(actual==expected && actual.size()==256,"A full UTF-8 draft split or exceeded the byte limit");
}
void submission_lifetime(){
    port::ChatOverlay chat;
    for(unsigned i=0;i<4;++i){open(chat);text(chat,"message "+std::to_string(i));press(chat,SDL_SCANCODE_RETURN);}
    open(chat);text(chat,"still editing");press(chat,SDL_SCANCODE_RETURN);
    require(chat.active(),"A full bounded submission queue silently discarded the draft");
    for(unsigned i=0;i<4;++i){auto message=chat.take_submission();require(message && *message=="message "+std::to_string(i),"Queued chat submission ordering changed");}
    require(!chat.take_submission(),"Submission queue repeated a message");
    require(submit(chat)=="still editing","Queue recovery lost the user's draft");
    chat.send_failed();require(chat.active(),"A failed send did not reopen the draft");
    require(submit(chat)=="still editing","Retry changed the rejected text");
    open(chat);text(chat,"next session");press(chat,SDL_SCANCODE_RETURN);chat.reset_session();
    require(!chat.active() && !chat.take_submission(),"A queued message leaked across sessions");
}
void configurable_opening(){
    port::ChatOverlay chat;
    require(!chat.handle_event(key(SDL_SCANCODE_T),true,true,false),"Unbound old T still opened chat");
    require(chat.handle_event(key(SDL_SCANCODE_C),true,true,true) && chat.active(),"Custom chat binding did not open");
    text(chat,"C",100);text(chat,"hello",130);require(submit(chat)=="hello","Custom opening letter leaked into draft");
    SDL_Event mouse{};mouse.type=SDL_MOUSEBUTTONDOWN;mouse.button.button=SDL_BUTTON_X1;
    require(chat.handle_event(mouse,true,true,true),"Mouse binding failed to open chat");
    text(chat,"t",100);require(submit(chat)=="t","Mouse opening incorrectly swallowed first typed letter");
    auto accent=key(SDL_SCANCODE_SEMICOLON);accent.key.keysym.sym=0xe9;
    require(chat.handle_event(accent,true,true,true),"Accented physical key failed to open");
    text(chat,"É",100);text(chat,"bonjour",130);require(submit(chat)=="bonjour","Accented opening letter leaked into draft");
}
}
int main()try{
    require(SDL_Init(0)==0,"SDL initialization failed");
    event_routing();utf8_editor();byte_limit();submission_lifetime();configurable_opening();SDL_Quit();
    std::cout<<"Chat editor: opening-key suppression, gameplay event isolation, UTF-8 editing/limits, cancellation, bounded submission order and session reset passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';SDL_Quit();return 1;}
