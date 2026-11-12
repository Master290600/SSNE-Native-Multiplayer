#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace multiplayer_launcher {
struct Request {std::string mode="Client",address,pin,project,player_name="Sam";int player_skin=0,port=27341,enemy_multiplier=1,ammo_capacity=0,ammo_pickup_multiplier=1;bool describe=false,validate_only=false,fullscreen=false;};
struct Result {bool ok=false,ready=false;int build=0,protocol=0,pid=0;std::string message,project,exe;};
std::wstring quote_argument(const std::wstring&);
Result parse_result(const std::string&);
Result run_backend(const std::filesystem::path& directory,const Request&);
std::filesystem::path exe_directory();
std::string utf8(const std::wstring&);
std::wstring wide(const std::string&);
std::string choose_project();
std::string local_address();
}
