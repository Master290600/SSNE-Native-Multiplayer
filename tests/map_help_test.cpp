#include "map_help.h"
#include <iostream>
#include <stdexcept>
using namespace mapedit;
int checks=0;
void check(bool v,const char*why){++checks;if(!v)throw std::runtime_error(why);}
int main(){try{
  HelpEvents events{};Tutorial t;
  check(!t.active()&&!t.finished(),"Unstarted tutorial is active");
  events.fill(8);t.start(events);check(t.active()&&t.step==0,"Start failed");
  check(!t.observe(events),"Old map load completed a new lesson");t.next(events);check(t.step==0,"Unfinished step advanced");
  ++events[unsigned(HelpEvent::Loaded)];check(t.observe(events),"New map load not observed");check(!t.observe(events),"Repeated completion changed state");
  t.next(events);check(t.step==1&&t.completed==1,"Completed step not retained");
  ++events[unsigned(HelpEvent::Move)];check(!t.observe(events),"Object move counted as camera navigation");
  ++events[unsigned(HelpEvent::Navigate)];check(t.observe(events),"Camera movement not observed");t.next(events);
  check(!t.observe(events),"Earlier actions completed selection step");t.next(events,true);check(t.step==3&&(t.skipped&4)&&!(t.completed&4),"Skipped step counted as completed");
  t.previous(events);check(t.step==2,"Previous failed");++events[unsigned(HelpEvent::Select)];check(t.observe(events)&&!(t.skipped&4),"Retry did not replace skipped state");t.next(events);
  while(t.active()){auto e=help_lessons()[size_t(t.step)].event;++events[unsigned(e)];check(t.observe(events),"Action did not complete lesson");t.next(events);}
  check(t.finished()&&!t.active()&&t.completed==4095&&t.skipped==0,"Final completion mask wrong");check(!t.observe(events),"Finished tutorial still records lessons");
  for(const auto& lesson:help_lessons())for(unsigned language=0;language<3;++language)check(lesson.title[language][0]&&lesson.body[language][0]&&lesson.hint[language][0],"Missing localized lesson");
  for(unsigned language=0;language<3;++language){check(!editor_tool_help("save",language).empty(),"Missing save tooltip");check(!editor_tool_help("help",language).empty(),"Missing help tooltip");check(editor_tool_help("unknown-control",language).empty(),"Unknown control invents help");}
  t.start(events);check(t.completed==0&&t.skipped==0&&t.step==0,"Restart kept stale progress");
  std::cout<<"PASS help checks="<<checks<<"\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<"\n";return 1;}}
