#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace mapedit {
enum class HelpEvent : unsigned {
  Loaded, Navigate, Select, Focus, Move, Rotate, Scale, CreateLogic,
  ConfigureLogic, Connect, Save, Test, Count
};
using HelpEvents=std::array<uint64_t, unsigned(HelpEvent::Count)>;
struct HelpLesson {
  const char *id;
  HelpEvent event;
  std::array<const char*,3> title, body, hint;
};
const std::array<HelpLesson,12>& help_lessons();
// A lesson completes only on a new event after entering that lesson. Earlier
// actions and automatic helper links cannot silently complete later steps.
struct Tutorial {
  int step=-1;
  uint32_t completed=0, skipped=0;
  uint64_t baseline=0;
  bool active()const {return step>=0 && step<int(help_lessons().size());}
  bool finished()const {return step==int(help_lessons().size());}
  void start(const HelpEvents&);
  bool observe(const HelpEvents&);
  void next(const HelpEvents&,bool skip=false);
  void previous(const HelpEvents&);
};
std::string_view editor_tool_help(std::string_view id,unsigned language);
}
