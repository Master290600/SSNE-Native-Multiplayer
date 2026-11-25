#pragma once
#include "world_state.h"
#include <string_view>
namespace port::coop::replica_detail {
// Audio commands are consumed into a replica cursor and generation-tagged
// mixer voices. NativeWorld's readback instead observes the original producer,
// whose journal and original brush handle intentionally stay untouched there.
// Spatial audio position and all geometry remain ordinary strict readback.
inline bool campaign_brush_audio_command(EntityKind kind,std::string_view key){
    return kind==EntityKind::Door && (key=="brush_audio_events" ||
        key=="brush_audio_serial" || key=="brush_audio_loop");
}
}
