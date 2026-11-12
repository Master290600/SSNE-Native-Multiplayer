// Multiplayer front end. Reuses the original recompiler's procedural menu theme.
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include "backend.h"
#include "theme.h"
#include "skins.h"
namespace ml=multiplayer_launcher;
namespace fs=std::filesystem;
namespace {
void text(ImDrawList* dl,ImVec2 p,float size,const std::string& value,ImU32 color=IM_COL32(233,237,255,255),ImFont* font=nullptr,float wrap=0){dl->AddText(font?font:theme::fonts.body,size,p,color,value.c_str(),nullptr,wrap);}
void heading(ImDrawList* dl,ImVec2 p,float size,const char* label,float t){theme::chunky_text(dl,p,size,label,theme::Look::Normal,t);}
bool digits(const std::string& s){return !s.empty() && std::all_of(s.begin(),s.end(),[](unsigned char c){return c>='0' && c<='9';});}
bool ipv4(const std::string& input){std::istringstream stream(input);std::string item;unsigned count=0;while(std::getline(stream,item,'.')){if(!digits(item)||item.size()>3 || std::stoi(item)>255)return false;++count;}return count==4 && !input.empty() && input.back()!='.';}
struct App {
 fs::path directory;SDL_Window* window;std::future<ml::Result> worker;ml::Result info;bool checking=false,launching=false,quit=false,preview=false,fullscreen=false,options=false,remember=true,host=false,error=false,checked=false;
 std::string message,project,own_ip;std::array<char,64> address{};std::array<char,7> pin{};std::array<char,65> player_name={'S','a','m',0};int player_skin=0,port=27341,enemy_multiplier=1,ammo_capacity=0,ammo_pickup_multiplier=1;HANDLE game=nullptr;float time=0;
 skins::Preview skin_preview;bool skins_page=false,skin_rotate=false;float skin_yaw=-15,skin_pitch=0,skin_zoom=1;
 App(SDL_Window* w,fs::path dir,bool p,bool host_page):directory(std::move(dir)),window(w),preview(p),host(host_page){
  own_ip=ml::local_address();std::ifstream file(directory/L"multiplayer-launcher.ini");std::string line;
  while(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();auto equal=line.find('=');if(equal==std::string::npos)continue;auto k=line.substr(0,equal),v=line.substr(equal+1);
   if(k=="address" && v.size()<address.size())std::copy(v.begin(),v.end(),address.begin());else if(k=="player_name" && !v.empty() && v.size()<player_name.size()){player_name.fill(0);std::copy(v.begin(),v.end(),player_name.begin());}else if(k=="player_skin"){try{size_t used=0;int n=std::stoi(v,&used);if(used==v.size() && n>=0 && n<=10)player_skin=n;}catch(...){}}else if(k=="ammo_capacity")ammo_capacity=v=="1"?1:v=="2"?2:0;else if(k=="ammo_pickup_multiplier")ammo_pickup_multiplier=v=="2"?2:1;else if(k=="enemy_multiplier")enemy_multiplier=v=="2"?2:1;else if(k=="project")project=v;else if(k=="fullscreen")fullscreen=v=="1";else if(k=="remember")remember=v!="0";else if(k=="port"){try{auto n=std::stoi(v);if(n>=1024 && n<=65535)port=n;}catch(...){}}
  }
  skin_preview.load(directory/L"skins.bin");describe();
 }
 ~App(){if(game)CloseHandle(game);}
 bool busy()const{return checking||launching;}
 void save(){if(preview)return;std::ofstream f(directory/L"multiplayer-launcher.ini",std::ios::trunc);if(f)f<<"# SSNE-Native Multiplayer\nplayer_name="<<player_name.data()<<"\nplayer_skin="<<player_skin<<"\naddress="<<(remember?address.data():"")<<"\nproject="<<project<<"\nport="<<port<<"\nfullscreen="<<fullscreen<<"\nammo_capacity="<<ammo_capacity<<"\nammo_pickup_multiplier="<<ammo_pickup_multiplier<<"\nenemy_multiplier="<<enemy_multiplier<<"\nremember="<<remember<<"\n";}
 void describe(){if(busy())return;ml::Request request;request.describe=true;request.project=project;checking=true;checked=false;message.clear();error=false;worker=std::async(std::launch::async,[dir=directory,request]{return ml::run_backend(dir,request);});}
 void choose(){if(busy())return;auto chosen=ml::choose_project();if(!chosen.empty()){project=chosen;save();describe();}}
 bool start(){
  if(busy()||game||options||skins_page)return false;
  std::string ip=address.data(),code=pin.data();
  const auto name_length=SDL_utf8strlen(player_name.data());
  if(!name_length || name_length>24){message="Bitte einen Spielernamen mit 1 bis 24 Zeichen eingeben.";error=true;return false;}
  if(!host && !ipv4(ip)){message="Bitte eine gültige Host-IP eingeben.";error=true;return false;}
  if((!host || !code.empty()) && (code.size()!=6 || !digits(code))){message=host?"Die PIN muss sechs Ziffern enthalten oder leer bleiben.":"Bitte die sechsstellige PIN des Hosts eingeben.";error=true;return false;}
  if(port<1024 || port>65535){message="Der Port muss zwischen 1024 und 65535 liegen.";error=true;return false;}
  if(!info.ready){message=info.message.empty()?"Bitte zuerst die Spieldaten auswählen.":info.message;error=true;return false;}
  save();ml::Request request;request.mode=host?"Host":"Client";request.address=ip;request.pin=code;request.project=project;request.port=port;request.fullscreen=fullscreen;request.player_name=player_name.data();request.player_skin=player_skin;request.enemy_multiplier=host?enemy_multiplier:1;request.ammo_capacity=host?ammo_capacity:0;request.ammo_pickup_multiplier=host?ammo_pickup_multiplier:1;
  launching=true;error=false;message="Spiel wird gestartet …";worker=std::async(std::launch::async,[dir=directory,request]{return ml::run_backend(dir,request);});return true;
 }
 void poll(){
  if(worker.valid() && worker.wait_for(std::chrono::seconds(0))==std::future_status::ready){
   const bool was_launching=launching;auto result=worker.get();checking=launching=false;error=!result.ok;message=result.message;
   if(!was_launching){info=result;checked=true;if(result.ok){project=result.project;message.clear();}}
   else if(result.ok && result.pid){game=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(result.pid));message="Spiel gestartet. Viel Spaß!";if(!preview)SDL_MinimizeWindow(window);}
  }
  if(game && WaitForSingleObject(game,0)==WAIT_OBJECT_0){DWORD code=0;GetExitCodeProcess(game,&code);CloseHandle(game);game=nullptr;error=code!=0;message=error?"Das Spiel wurde unerwartet beendet. Details stehen im Protokollordner.":"Spiel beendet. Bereit für die nächste Runde.";if(!preview){SDL_RestoreWindow(window);SDL_RaiseWindow(window);}}
 }
 void page(bool value){if(!busy() && !game){host=value;options=false;skins_page=false;message.clear();error=false;}}
 #include "skin_page.inc"
 void frame(){
  poll();time+=ImGui::GetIO().DeltaTime;auto* vp=ImGui::GetMainViewport();ImVec2 p=vp->WorkPos,extent=vp->WorkSize;float w=extent.x,h=extent.y,s=std::min(w/1200.f,h/760.f);float t=preview?1.4f:time;
  theme::draw_background(ImGui::GetBackgroundDrawList(),p,p+extent,t);
  ImGui::SetNextWindowPos(p);ImGui::SetNextWindowSize(extent);ImGui::Begin("##Multiplayer",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetWindowFontScale(s);auto* dl=ImGui::GetWindowDrawList();
  const float title_size=76*s;auto title=theme::chunky_text(nullptr,{},title_size,"SSNE-Native",theme::Look::Normal,t);
  heading(dl,p+ImVec2((w-title.x)*.5f,29*s),title_size,"SSNE-Native",t);
  const char* subtitle="MULTIPLAYER";float subsize=16*s,spacing=7*s;float subwidth=theme::spaced_text(nullptr,theme::fonts.bold,subsize,{},0,subtitle,spacing);
  theme::spaced_text(dl,theme::fonts.bold,subsize,p+ImVec2((w-subwidth)*.5f,129*s),IM_COL32(198,205,255,255),subtitle,spacing);
  dl->AddLine(p+ImVec2(48*s,176*s),p+ImVec2(w-48*s,176*s),IM_COL32(128,141,255,60),1);
  if(skins_page){skin_page(p,w,h,s,t);ImGui::End();return;}
  const float left=60*s,top=205*s,split=w*.46f,right=w-54*s,bottom=h-75*s;
  struct Menu {const char* label;int action;};Menu menus[]={{"Beitreten",0},{"Spiel hosten",1},{"Spieler-Skin",2},{"Optionen",3},{"Spieldaten",4},{"Beenden",5}};
  for(unsigned i=0;i<6;++i){auto pos=p+ImVec2(left+18*s,top+i*52*s);bool active=options?i==3:((i==0&&!host)||(i==1&&host));auto size=i<2?39*s:31*s;
   ImGui::SetCursorScreenPos(pos-ImVec2(15*s,0));bool click=ImGui::InvisibleButton(menus[i].label,ImVec2(split-left-32*s,47*s));bool hover=ImGui::IsItemHovered();
   if(active){text(dl,pos-ImVec2(30*s,-11*s),23*s,">",theme::kYellow,theme::fonts.bold);}
   theme::chunky_text(dl,pos,size,menus[i].label,(active||hover)?theme::Look::Selected:theme::Look::Normal,t);
   if(click && !busy() && !game){if(i==0)page(false);if(i==1)page(true);if(i==2){skins_page=true;message.clear();error=false;}if(i==3){options=true;message.clear();error=false;}if(i==4)choose();if(i==5)quit=true;}
  }
  float note=bottom-126*s;dl->AddLine(p+ImVec2(left,note-17*s),p+ImVec2(split-28*s,note-17*s),IM_COL32(136,151,255,55),1);
  text(dl,p+ImVec2(left,note),16*s,"KOOP  /  BIS ZU 8 SPIELER",IM_COL32(255,215,116,255),theme::fonts.bold);
  text(dl,p+ImVec2(left,note+32*s),17*s,"Koop und Versus spielen.\nHost und Mitspieler brauchen denselben Build.",IM_COL32(172,181,223,255),nullptr,split-left-30*s);
  const auto build_text=info.build?"Build "+std::to_string(info.build)+"  ·  Protokoll "+std::to_string(info.protocol):"Multiplayer-Entwicklungsbuild";
  text(dl,p+ImVec2(left,bottom-15*s),13*s,build_text,IM_COL32(127,142,202,255));
  const ImVec2 panel0=p+ImVec2(split,top-6*s),panel1=p+ImVec2(right,bottom);theme::panel(dl,panel0,panel1);
  float x=panel0.x+28*s,y=panel0.y+22*s,field_width=panel1.x-x-28*s;
  heading(dl,{x,y},33*s,options?"Optionen":host?"Deine Spielrunde":"Auf ins Gefecht!",t);y+=55*s;
  ImU32 state_color=checking?IM_COL32(255,194,74,255):info.ready?IM_COL32(139,225,111,255):IM_COL32(255,128,100,255);
  dl->AddCircleFilled({x+5*s,y+10*s},4*s,state_color,16);
  text(dl,{x+19*s,y},15*s,options?"Wird automatisch gespeichert":checking?"Spiel wird geprüft …":info.ready?"Multiplayer bereit":"Spieldaten prüfen",state_color);y+=33*s;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{12*s,9*s});ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,4*s);ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{10*s,7*s});
  ImGui::BeginDisabled(busy() || game);
  if(options){
   const float row_label=166*s,row_width=field_width-row_label;
   ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{8*s,5*s});
   auto label=[&](const char* name){text(dl,{x,y+6*s},14*s,name,IM_COL32(172,184,229,255));ImGui::SetCursorScreenPos({x+row_label,y});ImGui::SetNextItemWidth(row_width);};
   label("Gegnerzahl");int selected=enemy_multiplier-1;
   if(ImGui::Combo("##enemy-multiplier",&selected,"Normal (1x)\0Doppelt (2x)\0")){enemy_multiplier=selected+1;save();}y+=38*s;
   label("Munitionslimit");
   if(ImGui::Combo("##ammo-capacity",&ammo_capacity,"Normal\0Doppelt\0Unbegrenzt*\0"))save();y+=38*s;
   label("Munition je Pickup");selected=ammo_pickup_multiplier-1;
   if(ImGui::Combo("##ammo-pickup",&selected,"Normal (1x)\0Doppelt (2x)\0")){ammo_pickup_multiplier=selected+1;save();}y+=38*s;
   text(dl,{x,y},13*s,"Host-Regeln ab dem nächsten Start. Bosse bleiben einzeln.\n* Engine-Grenze: 32.767 je Munitionsart. Verbrauch bleibt normal.",IM_COL32(164,181,226,255),nullptr,field_width);y+=39*s;
   ImGui::SetCursorScreenPos({x,y});if(ImGui::Checkbox("Vollbild",&fullscreen))save();ImGui::SameLine(0,22*s);if(ImGui::Checkbox("Host-Adresse merken",&remember))save();y+=35*s;
   label("Verbindungsport");ImGui::SetNextItemWidth(105*s);if(ImGui::InputInt("##port",&port,0,0))save();ImGui::SameLine();ImGui::TextDisabled("TCP");ImGui::PopStyleVar();
  }else{
  const float name_width=field_width*.59f,pin_x=x+name_width+16*s,pin_width=field_width-name_width-16*s;
  text(dl,{x,y},13*s,"SPIELERNAME",IM_COL32(172,184,229,255),theme::fonts.bold);
  text(dl,{pin_x,y},13*s,host?"PIN (OPTIONAL)":"PIN DES HOSTS",IM_COL32(172,184,229,255),theme::fonts.bold);y+=22*s;
  ImGui::SetCursorScreenPos({x,y});ImGui::SetNextItemWidth(name_width);if(ImGui::InputTextWithHint("##player-name","Dein Name",player_name.data(),player_name.size()))save();
  ImGui::SameLine(0,16*s);ImGui::SetNextItemWidth(pin_width);ImGui::InputTextWithHint("##pin",host?"Zufällig":"6 Ziffern",pin.data(),pin.size(),ImGuiInputTextFlags_CharsDecimal);y+=51*s;
  if(!host){
   text(dl,{x,y},13*s,"HOST-IP",IM_COL32(172,184,229,255),theme::fonts.bold);y+=22*s;
   ImGui::SetCursorScreenPos({x,y});ImGui::SetNextItemWidth(field_width-112*s);ImGui::InputTextWithHint("##host-ip","z. B. 192.168.178.20",address.data(),address.size());
   ImGui::SameLine(0,8*s);if(ImGui::Button("Dieser PC",{104*s,37*s})){snprintf(address.data(),address.size(),"127.0.0.1");}
   y+=51*s;
  }else{
   text(dl,{x,y},13*s,"DEINE LAN-IP",IM_COL32(172,184,229,255),theme::fonts.bold);y+=22*s;
   text(dl,{x,y},26*s,own_ip,IM_COL32(232,238,255,255),theme::fonts.bold);
   ImGui::SetCursorScreenPos({panel1.x-117*s,y-1*s});if(ImGui::Button("Kopieren",{89*s,35*s}))ImGui::SetClipboardText(own_ip.c_str());y+=51*s;
  }
  text(dl,{x,y},14*s,host?(enemy_multiplier==2?"Gegner: Doppelt (2x)":"Gegner: Normal (1x)"):"Die Spielregeln legt der Host fest.",IM_COL32(164,181,226,255));
  if(host){y+=25*s;text(dl,{x,y},14*s,std::string("Munitionslimit: ")+(ammo_capacity==0?"Normal":ammo_capacity==1?"Doppelt":"Unbegrenzt*")+"  /  Pickup: "+std::to_string(ammo_pickup_multiplier)+"x",IM_COL32(164,181,226,255));}
  y+=26*s;text(dl,{x,y},14*s,std::string(fullscreen?"Vollbild":"Fenster")+"  /  Port "+std::to_string(port)+"  ·  Änderungen unter Optionen",IM_COL32(164,181,226,255),nullptr,field_width);
  }
  ImGui::EndDisabled();ImGui::PopStyleVar(3);
  const float button_y=panel1.y-137*s;ImGui::SetCursorScreenPos({x,button_y});ImGui::BeginDisabled(busy() || game || (!checked && !options));
  bool clicked=ImGui::InvisibleButton("##start-game",{field_width,57*s});bool over=ImGui::IsItemHovered();ImGui::EndDisabled();
  ImU32 upper=(busy()||game)?IM_COL32(90,87,113,255):over?IM_COL32(255,226,100,255):IM_COL32(255,190,52,255);ImU32 lower=(busy()||game)?IM_COL32(41,42,65,255):IM_COL32(211,91,5,255);
  dl->AddRectFilled({x+3*s,button_y+5*s},{x+field_width+3*s,button_y+62*s},IM_COL32(0,0,0,150),5*s);dl->AddRectFilledMultiColor({x,button_y},{x+field_width,button_y+57*s},upper,upper,lower,lower);dl->AddRect({x,button_y},{x+field_width,button_y+57*s},IM_COL32(255,227,131,230),4*s,0,1.5f*s);
  const char* button_label=options?"ZURUECK":launching?"STARTET …":checking?"PRÜFUNG …":game?"SPIEL LÄUFT":host?"RUNDE STARTEN":"BEITRETEN";
  // The display alphabet is ASCII; body font handles German umlauts.
  if(busy()||game){auto ts=theme::fonts.bold->CalcTextSizeA(24*s,FLT_MAX,0,button_label);text(dl,{x+(field_width-ts.x)*.5f,button_y+14*s},24*s,button_label,IM_COL32(221,226,255,255),theme::fonts.bold);}
  else{auto ts=theme::chunky_text(nullptr,{},28*s,button_label,theme::Look::Normal,t);heading(dl,{x+(field_width-ts.x)*.5f,button_y+8*s},28*s,button_label,t);}
  if(clicked){if(options)page(host);else start();}
  std::string status=message;if(status.empty()&&!info.ready&&!checking)status=info.message;
  if(status.empty())status=options?"Die Optionen gelten für den nächsten Spielstart.":host?"Deine Freunde verbinden sich mit deiner IP und PIN.":"Die sechsstellige PIN steht beim Host im Spiel.";
  text(dl,{x,button_y+74*s},15*s,status,error?IM_COL32(255,151,122,255):IM_COL32(164,181,226,255),nullptr,field_width);
  dl->AddRectFilled(p+ImVec2(0,h-49*s),p+extent,IM_COL32(4,6,23,227));dl->AddLine(p+ImVec2(0,h-49*s),p+ImVec2(w,h-49*s),IM_COL32(102,120,196,85));
  theme::key_prompt(dl,p+ImVec2(42*s,h-37*s),23*s,"TAB","Feld wechseln",IM_COL32(66,74,106,255));theme::key_prompt(dl,p+ImVec2(275*s,h-37*s),23*s,"ENTER",options?"Zurück":"Starten",IM_COL32(66,74,106,255));
  text(dl,p+ImVec2(w-300*s,h-32*s),13*s,"SERIOUS SAM  /  NEXT ENCOUNTER",IM_COL32(142,153,200,255),theme::fonts.bold);
  if(ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false)){if(options)page(host);else start();}
  ImGui::End();
 }
};
void screenshot(SDL_Window* window,const fs::path& path){int w,h;SDL_GL_GetDrawableSize(window,&w,&h);std::vector<unsigned char> pixels(size_t(w)*h*3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());std::ofstream out(path,std::ios::binary);out<<"P6\n"<<w<<" "<<h<<"\n255\n";for(int y=h-1;y>=0;--y)out.write(reinterpret_cast<char*>(pixels.data()+size_t(y)*w*3),w*3);}
}
int main(int argc,char** argv){
 bool host=false,options=false,skin_page=false;float skin_angle=-15;int width=1200,height=760;fs::path capture,root=ml::exe_directory();
 for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--host")host=true;else if(arg=="--options")options=true;else if(arg=="--skins")skin_page=true;else if(arg=="--skin-angle" && i+1<argc)skin_angle=float(atof(argv[++i]));else if(arg=="--screenshot"&&i+1<argc)capture=fs::path(ml::wide(argv[++i]));else if(arg=="--profile-dir"&&i+1<argc)root=fs::path(ml::wide(argv[++i]));else if(arg=="--size"&&i+2<argc){width=std::max(960,atoi(argv[++i]));height=std::max(640,atoi(argv[++i]));}}
 SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS,"permonitorv2");if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0)return 1;
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
 auto* window=SDL_CreateWindow("SSNE-Native - Multiplayer",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,width,height,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI|(capture.empty()?0:SDL_WINDOW_HIDDEN));if(!window){SDL_Quit();return 2;}
 SDL_SetWindowMinimumSize(window,960,640);auto context=SDL_GL_CreateContext(window);bool core=context!=nullptr;if(!context){SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,0);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);context=SDL_GL_CreateContext(window);}if(!context){SDL_DestroyWindow(window);SDL_Quit();return 3;}
 SDL_GL_SetSwapInterval(1);SDL_SysWMinfo wm;SDL_VERSION(&wm.version);if(SDL_GetWindowWMInfo(window,&wm)){auto icon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101));if(icon){SendMessageW(wm.info.win.window,WM_SETICON,ICON_BIG,(LPARAM)icon);SendMessageW(wm.info.win.window,WM_SETICON,ICON_SMALL,(LPARAM)icon);}}
 IMGUI_CHECKVERSION();ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;static const ImWchar glyphs[]={0x20,0xFF,0x2000,0x206F,0};theme::load_fonts(1.25f);theme::apply_style();ImGui_ImplSDL2_InitForOpenGL(window,context);ImGui_ImplOpenGL3_Init(core?"#version 330 core":"#version 130");
 {App app(window,root,!capture.empty(),host);app.options=options;app.skins_page=skin_page;app.skin_yaw=skin_angle;bool done=false;unsigned frames=0;while(!done&&!app.quit){auto began=SDL_GetTicks();SDL_Event event;while(SDL_PollEvent(&event)){ImGui_ImplSDL2_ProcessEvent(&event);if(event.type==SDL_QUIT)done=true;}
   if(capture.empty() && (SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)){app.poll();SDL_Delay(120);continue;}
   ImGui_ImplOpenGL3_NewFrame();ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();app.frame();ImGui::Render();int w,h;SDL_GL_GetDrawableSize(window,&w,&h);glViewport(0,0,w,h);glClearColor(.01f,.02f,.1f,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
   if(!capture.empty() && ++frames>=8 && (!app.busy()||frames>180)){screenshot(window,capture);done=true;}SDL_GL_SwapWindow(window);auto elapsed=SDL_GetTicks()-began;if(elapsed<16)SDL_Delay(16-elapsed);
  }}
 ImGui_ImplOpenGL3_Shutdown();ImGui_ImplSDL2_Shutdown();ImGui::DestroyContext();SDL_GL_DeleteContext(context);SDL_DestroyWindow(window);SDL_Quit();return 0;
}
