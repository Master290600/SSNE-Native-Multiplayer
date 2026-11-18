#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
namespace port::coop::invite {
enum class InvitationFormat : uint8_t {Legacy=1,Compact=2};
struct Invitation {
    std::array<uint8_t,10> room{};
    std::string pin;
    InvitationFormat format=InvitationFormat::Legacy;
};
// Preserve the original 80-bit-room/CRC invitation API for existing callers.
std::optional<Invitation> make_invitation(const std::string& pin,std::string& error);
// Twelve Crockford symbols: PIN20 + random room40; room[5..9] remain zero.
std::optional<Invitation> make_compact_invitation(const std::string& pin,std::string& error);
std::string encode_invitation(const Invitation&);
std::optional<Invitation> decode_invitation(const std::string&,std::string& error);
std::string invitation_topic(const Invitation&);
}
