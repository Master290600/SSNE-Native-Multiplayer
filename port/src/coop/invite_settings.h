#pragma once
#include "invite_transport.h"
#include <string>

namespace port::coop::invite {
// Missing files retain defaults. Invalid files fail only Internet hosting/join,
// leaving ordinary LAN and direct-address sessions available.
bool load_settings(const std::string& path,Config& result,std::string& error);
bool parse_settings(const std::string& text,Config& result,std::string& error);
}
