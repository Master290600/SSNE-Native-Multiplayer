#pragma once
#include <filesystem>
#include <memory>
#include <string>
namespace skins {
inline constexpr const char* names[]={"Serious Sam","Baby Sam","Cheerleader","Clown","Cowboy","Elvis","Hippie","Roboter","Skelett","Spaceboy","Vampir"};
class Preview {
 struct Impl;std::unique_ptr<Impl> impl;
public:
 Preview();~Preview();
 bool load(const std::filesystem::path&);
 const std::string& error()const;
 unsigned thumbnail(unsigned skin);
 unsigned render(unsigned skin,unsigned width,unsigned height,float yaw,float pitch,float zoom);
};
}
