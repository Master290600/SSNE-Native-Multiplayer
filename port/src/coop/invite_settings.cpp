#include "invite_settings.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace port::coop::invite { namespace {
bool hostname(const std::string& text){
    return !text.empty() && text.size()<=253 && text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:")==std::string::npos;
}
bool signal_url(const std::string& text){
    if(text.size()>1024 || text.find_first_of("\r\n\t @?#\\")!=std::string::npos)return false;
    if(text.starts_with("https://"))return text.size()>8;
    // Deliberate local-service support, also used by the private native tests.
    if(!text.starts_with("http://"))return false;
    const auto end=text.find_first_of(":/",7);const auto host=text.substr(7,end==std::string::npos?end:end-7);
    return host=="127.0.0.1" || host=="localhost";
}
bool fail(std::string& error,const char* reason){error=reason;return false;}
}
bool parse_settings(const std::string& text,Config& result,std::string& error){
    if(text.size()>16384 || text.find('\0')!=std::string::npos)return fail(error,"Invalid Internet settings file.");
    std::istringstream in(text);std::string line;Config next;std::set<std::string> seen;
    if(!std::getline(in,line) || (line!="SSNE-INTERNET-1" && line!="SSNE-INTERNET-1\r"))return fail(error,"Unsupported Internet settings format.");
    while(std::getline(in,line)){
        std::istringstream row(line);std::string key;row>>key;if(key.empty() || key.starts_with('#'))continue;
        if(key!="relay" && !seen.insert(key).second)return fail(error,"Duplicate Internet setting.");
        if(key=="signal_url"){if(!(row>>std::quoted(next.signal_url)) || !signal_url(next.signal_url))return fail(error,"Invalid invitation service address.");}
        else if(key=="stun_host"){if(!(row>>std::quoted(next.stun_host)) || !hostname(next.stun_host))return fail(error,"Invalid STUN server.");}
        else if(key=="stun_port"){unsigned value=0;if(!(row>>value) || !value || value>65535)return fail(error,"Invalid STUN port.");next.stun_port=uint16_t(value);}
        else if(key=="automatic_mapping"){unsigned value=2;if(!(row>>value) || value>1)return fail(error,"Invalid automatic mapping setting.");next.automatic_mapping=value!=0;}
        else if(key=="relay"){
            Relay relay;unsigned port=0;
            if(next.relays.size()>=2 || !(row>>std::quoted(relay.host)>>port>>std::quoted(relay.username)>>std::quoted(relay.password)) ||
               !hostname(relay.host) || !port || port>65535 || relay.username.empty() || relay.username.size()>128 || relay.password.empty() || relay.password.size()>128 ||
               relay.username.find_first_of("\r\n")!=std::string::npos || relay.password.find_first_of("\r\n")!=std::string::npos)return fail(error,"Invalid relay settings.");
            relay.port=uint16_t(port);next.relays.push_back(std::move(relay));
        }else return fail(error,"Unknown Internet setting.");
        std::string extra;if(row>>extra)return fail(error,"Unexpected Internet settings data.");
    }
    result=std::move(next);error.clear();return true;
}
bool load_settings(const std::string& path,Config& result,std::string& error){
    const std::filesystem::path file{std::u8string(path.begin(),path.end())};
    std::error_code ec;const auto exists=std::filesystem::exists(file,ec);
    if(ec)return fail(error,"Cannot read Internet settings.");
    if(!exists){result=Config{};error.clear();return true;}
    const auto size=std::filesystem::file_size(file,ec);
    if(ec || size>16384)return fail(error,"Invalid Internet settings size.");
    std::ifstream stream(file,std::ios::binary);
    if(!stream)return fail(error,"Cannot open Internet settings.");
    std::string text(size,'\0');if(!stream.read(text.data(),std::streamsize(size)) || stream.peek()!=EOF)return fail(error,"Internet settings changed while reading.");
    return parse_settings(text,result,error);
}
}
