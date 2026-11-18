#include "invite_code.h"
#include "gc/sha256.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
namespace port::coop::invite { namespace {
constexpr char alphabet[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ";
bool valid_pin(const std::string& pin){return pin.size()==6 && std::all_of(pin.begin(),pin.end(),[](char c){return c>='0' && c<='9';});}
bool compact_payload(const Invitation& invitation,uint64_t& payload){
    if(invitation.format!=InvitationFormat::Compact || !valid_pin(invitation.pin) ||
       std::any_of(invitation.room.begin()+5,invitation.room.end(),[](uint8_t byte){return byte!=0;}))return false;
    unsigned pin=0;for(char c:invitation.pin)pin=pin*10+unsigned(c-'0');
    uint64_t room=0;for(unsigned i=0;i<5;++i)room=(room<<8)|invitation.room[i];
    payload=(uint64_t(pin)<<40)|room;return true;
}
uint16_t crc16(const uint8_t* bytes,size_t size){
    uint16_t crc=0xffff;
    for(size_t i=0;i<size;++i){crc^=uint16_t(bytes[i])<<8;for(unsigned bit=0;bit<8;++bit)crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));}
    return crc;
}
}
std::optional<Invitation> make_invitation(const std::string& pin,std::string& error){
    error.clear();if(!valid_pin(pin)){error="The PIN must contain exactly six digits.";return {};}
    Invitation result;result.pin=pin;
    if(BCryptGenRandom(nullptr,result.room.data(),ULONG(result.room.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){error="Cannot generate a secure invitation.";return {};}
    return result;
}
std::optional<Invitation> make_compact_invitation(const std::string& pin,std::string& error){
    error.clear();if(!valid_pin(pin)){error="The PIN must contain exactly six digits.";return {};}
    Invitation result;result.pin=pin;result.format=InvitationFormat::Compact;
    if(BCryptGenRandom(nullptr,result.room.data(),5,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){error="Cannot generate a secure invitation.";return {};}
    return result;
}
std::string encode_invitation(const Invitation& invitation){
    if(invitation.format==InvitationFormat::Compact){
        uint64_t payload=0;if(!compact_payload(invitation,payload))return {};
        std::string text(12,'0');for(unsigned i=12;i-->0;){text[i]=alphabet[payload&31];payload>>=5;}
        return text.substr(0,4)+"-"+text.substr(4,4)+"-"+text.substr(8,4);
    }
    if(invitation.format!=InvitationFormat::Legacy)return {};
    if(!valid_pin(invitation.pin))return {};
    unsigned pin=0;for(char c:invitation.pin)pin=pin*10+unsigned(c-'0');
    std::array<uint8_t,15> bytes{};bytes[0]=uint8_t(0x10|(pin>>16));bytes[1]=uint8_t(pin>>8);bytes[2]=uint8_t(pin);
    std::copy(invitation.room.begin(),invitation.room.end(),bytes.begin()+3);auto crc=crc16(bytes.data(),13);bytes[13]=uint8_t(crc>>8);bytes[14]=uint8_t(crc);
    std::string text="SSNE";uint32_t acc=0;unsigned bits=0,count=0;
    for(uint8_t byte:bytes){acc=(acc<<8)|byte;bits+=8;while(bits>=5){bits-=5;if(count++%4==0)text+='-';text+=alphabet[(acc>>bits)&31];}}
    return text;
}
std::optional<Invitation> decode_invitation(const std::string& input,std::string& error){
    error.clear();if(input.size()>128){error="Invitation is too long.";return {};}
    std::string text;for(unsigned char c:input){if(c=='-' || c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\v' || c=='\f')continue;if(c>='a' && c<='z')c-=32;text+=char(c);}
    if(text.size()==12){
        uint64_t payload=0;
        for(unsigned char c:text){const auto* digit=std::find(std::begin(alphabet),std::end(alphabet)-1,c);if(digit==std::end(alphabet)-1){error="Invitation contains an invalid character.";return {};}payload=(payload<<5)|uint64_t(digit-alphabet);}
        const unsigned pin=unsigned(payload>>40);if(pin>999999){error="Invitation PIN is out of range.";return {};}
        Invitation result;result.format=InvitationFormat::Compact;
        for(unsigned i=5;i-->0;){result.room[i]=uint8_t(payload);payload>>=8;}
        char digits[7];std::snprintf(digits,sizeof(digits),"%06u",pin);result.pin=digits;return result;
    }
    if(text.size()!=28 || text.compare(0,4,"SSNE")){error="Enter a complete 12-character or SSNE invitation code.";return {};}
    std::array<uint8_t,15> bytes{};uint32_t acc=0;unsigned bits=0,pos=0;
    for(size_t i=4;i<text.size();++i){const auto* digit=std::find(std::begin(alphabet),std::end(alphabet)-1,text[i]);if(digit==std::end(alphabet)-1){error="Invitation contains an invalid character.";return {};}
        acc=(acc<<5)|unsigned(digit-alphabet);bits+=5;if(bits>=8){bits-=8;bytes[pos++]=uint8_t(acc>>bits);}}
    if(pos!=15 || bits || (bytes[0]>>4)!=1){error="Unsupported invitation version.";return {};}
    if(crc16(bytes.data(),13)!=(uint16_t(bytes[13])<<8|bytes[14])){error="Invitation checksum does not match.";return {};}
    const unsigned pin=(unsigned(bytes[0]&15)<<16)|(unsigned(bytes[1])<<8)|bytes[2];if(pin>999999){error="Invitation PIN is out of range.";return {};}
    Invitation result;std::copy(bytes.begin()+3,bytes.begin()+13,result.room.begin());char digits[7];std::snprintf(digits,sizeof(digits),"%06u",pin);result.pin=digits;return result;
}
std::string invitation_topic(const Invitation& invitation){
    if(invitation.format==InvitationFormat::Compact){
        uint64_t payload=0;if(!compact_payload(invitation,payload))return {};
        std::array<uint8_t,8> bytes{};for(unsigned i=8;i-->0;){bytes[i]=uint8_t(payload);payload>>=8;}
        // Retain the signalling protocol's accepted topic envelope. Include
        // every code bit and a format domain; hashing adds no code entropy.
        constexpr char domain[]="SSNE-INVITE-V2";gc::Sha256 hash;
        hash.update(domain,sizeof(domain)-1);hash.update(bytes.data(),bytes.size());
        return "ssne-v1-"+gc::Sha256::hex(hash.finish()).substr(0,20);
    }
    if(invitation.format!=InvitationFormat::Legacy)return {};
    constexpr char hex[]="0123456789abcdef";std::string topic="ssne-v1-";for(uint8_t c:invitation.room){topic+=hex[c>>4];topic+=hex[c&15];}return topic;
}
}
