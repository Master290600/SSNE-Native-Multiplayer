#pragma once
#include <algorithm>
#include <string>
#include <string_view>
namespace port::coop {
// Capacity codes retain the original CLI/checkpoint meanings: 2 is the native
// maximum, while 3..5 select those factors. They do not grant infinite shots.
struct MultiplayerOptions {
    std::string name; // Native Latin-1, empty means the selected game profile.
    unsigned enemies=1,capacity=0,pickups=1;
    std::string host_name; // Native Latin-1, empty means the host's player name.
    std::string pin="123456";
    bool use_password=false; // Require the PIN instead of issuing LAN join tickets.
    unsigned skin=255; // 255 follows the selected profile; runtime checks the installed catalog.
};
inline bool valid_multiplayer_options(const MultiplayerOptions& o){
    const auto valid_label=[](const std::string& value,unsigned limit){
        return value.size()<=limit && (value.empty() || value.find_first_not_of(' ')!=std::string::npos) &&
            std::none_of(value.begin(),value.end(),[](unsigned char c){return c<32 || (c>=127 && c<160);});
    };
    return o.enemies>=1 && o.enemies<=5 && o.capacity<=5 && o.pickups>=1 && o.pickups<=5 &&
        valid_label(o.name,24) && valid_label(o.host_name,32) && o.skin<=255 && o.pin.size()==6 &&
        std::all_of(o.pin.begin(),o.pin.end(),[](char c){return c>='0' && c<='9';});
}
inline unsigned next_ammo_capacity(unsigned code){return code==0?1:code==1?3:code==5?2:code==2?0:code+1;}
inline std::string native_name_utf8(std::string_view name){
    std::string out;for(unsigned char c:name){if(c<128)out+=char(c);else{out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}}return out;
}
inline std::string native_name_input(std::string_view utf8){
    std::string out;for(size_t i=0;i<utf8.size();++i){unsigned char c=utf8[i];
        if(c>=32 && c<127)out+=char(c);
        else if((c==0xc2 || c==0xc3) && i+1<utf8.size()){
            const auto tail=static_cast<unsigned char>(utf8[i+1]);
            if((tail&0xc0)==0x80){const auto value=((c&31)<<6)|(tail&63);if(value>=160)out+=char(value);++i;}
        }
    }return out;
}
}
