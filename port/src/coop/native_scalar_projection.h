#pragma once
#include "world_state.h"
#include <optional>
#include <algorithm>
namespace port::coop::replica_detail {
// This is the exact scalar-only projection used after native lifecycle work.
// An empty optional means every field survives: use the immutable source
// directly rather than copying its complete skeleton/property storage.
inline bool lifecycle_scalar_field(EntityKind kind,const std::string& key){
    if(key=="particles_enabled" || key=="brush_segment" || key=="brush_progress" || key=="movement_mode" || key=="camera_frozen" || key=="selected_weapon" || key=="weapon_modes" || key=="marker_active" || key=="powerups" || key.starts_with("weapon_model") || key.starts_with("weapon_bomb"))return false;
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
