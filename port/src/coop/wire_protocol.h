#pragma once
#include <cstdint>
namespace port::coop::wire {
inline constexpr uint32_t magic=0x53534350;
inline constexpr uint16_t version=63; // Extended host rules (1..5) require peers with matching validation.
inline constexpr unsigned header_size=12;
enum class Message:uint16_t { Hello=1,Assigned,Rejected,World,Input,Ready,Afk,Skip,SnapshotRequest,Ping,Pong,WorldAck,NativeEventsAck,LobbyReady };
}
