#pragma once
#include "world_state.h"
#include "movement_command_stream.h"
#include <optional>
#include <algorithm>
#include <string_view>
#include <cmath>
namespace port::coop::replica_detail {
// These four fields are preflighted/applied by the native door-audio owner.
// Keep this exact list shared with native preflight and its full-copy reference;
// unknown names and another class's similarly named fields must reach validation.
inline bool brush_audio_lifecycle_field(EntityKind kind,std::string_view key){
    return kind==EntityKind::Door && (key=="brush_audio_events" || key=="brush_audio_serial" ||
        key=="brush_audio_loop" || key=="brush_audio_position");
}
// This is a render-only value, validated with the complete player snapshot.
// It must not become a guest scalar write after an unrelated actor is created.
inline bool player_camera_lifecycle_field(EntityKind kind,std::string_view key){
    return kind==EntityKind::Player && key=="camera_head_compression";
}
inline bool valid_player_camera_head_compression(const Property& value){
    const auto* compression=std::get_if<float>(&value);
    return compression && std::isfinite(*compression) && *compression>=0.f && *compression<=.2f;
}
// Movement checkpoint/consumption fields are transport metadata. Validate the
// complete source before either scalar projection removes them; none is a
// guest memory offset, and a lookalike name or another class stays strict.
inline bool valid_player_movement_metadata(EntityKind kind,std::string_view key,const Property& value){
    if(!player_movement_metadata(kind,key))return false;
    if(key=="movement_checkpoint")return decode_movement_checkpoint(value).has_value();
    const auto* number=std::get_if<int64_t>(&value);
    return number && (key=="movement_ack"?*number>=0:*number>0);
}
inline bool valid_player_movement_fields(const EntityState& entity){
    for(const auto& [key,value]:entity.properties)
        if(player_movement_metadata(EntityKind::Player,key) &&
           !valid_player_movement_metadata(entity.kind,key,value))return false;
    return true;
}
// This is the exact scalar-only projection used after native lifecycle work.
// An empty optional means every field survives: use the immutable source
// directly rather than copying its complete skeleton/property storage.
inline bool lifecycle_scalar_field(EntityKind kind,const std::string& key){
    if(key=="particles_enabled" || key=="brush_segment" || key=="brush_progress" || brush_audio_lifecycle_field(kind,key) || player_camera_lifecycle_field(kind,key) || player_movement_metadata(kind,key) || key=="movement_mode" || key=="camera_frozen" || key=="selected_weapon" || key=="weapon_modes" || key=="marker_active" || key=="powerups" || key.starts_with("weapon_model") || key.starts_with("weapon_bomb"))return false;
    if(kind!=EntityKind::Player)return true;
    return key!="render_visible" && key!="render_row0" && key!="render_row1" && key!="render_row2" && !key.starts_with("body_flash") && !key.starts_with("body_render") && !key.starts_with("body0_bone_") && !key.starts_with("body1_bone_") && !key.starts_with("body2_bone_") && !key.starts_with("body3_bone_");
}
inline std::optional<EntityState> project_lifecycle_scalars(const EntityState& source){
    const bool filtered=std::any_of(source.properties.begin(),source.properties.end(),[&](const auto& field){return !lifecycle_scalar_field(source.kind,field.first);});
    if(!filtered)return std::nullopt;
    EntityState out{source.id,source.kind,source.definition,source.resource,source.owner,{}};
    for(const auto& field:source.properties)if(lifecycle_scalar_field(source.kind,field.first))out.properties.emplace_hint(out.properties.end(),field);
    return out;
}
}
