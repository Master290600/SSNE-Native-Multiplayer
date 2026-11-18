#pragma once
#include <cstdint>
namespace port::coop::wire {
inline constexpr uint32_t magic=0x53534350;
inline constexpr uint16_t version=70; // Native editor logic effects and matching checkpoint ownership.
inline constexpr unsigned header_size=12;
enum class Message:uint16_t { Hello=1,Assigned,Rejected,World,Input,Ready,Afk,Skip,SnapshotRequest,Ping,Pong,WorldAck,NativeEventsAck,LobbyReady,Chat,ChatHistory,Spectator,TacticalPing,SessionTelemetry,CustomMapRequest,CustomMapOffer,CustomMapChunk,CustomMapComplete,CustomMapError,CustomMapCancel };
}
