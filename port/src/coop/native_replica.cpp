#include "native_replica.h"
#include "native_vehicle.h"
#include "ssne/native_entity.h"
#include "ssne/performance.h"
#include "native_lifecycle.h"
#include "native_configuration.h"
#include "native_presentation.h"
#include "client_presentation.h"
#include "ssne/runtime.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/native_party.h"
#include "ssne/game_addresses.h"
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <limits>
#include <cstring>
namespace port::coop {
namespace {
bool readable(uint32_t p,unsigned n){return p>=0x80000000u && n<=ssne::guest_ram_size() && p<=ssne::guest_ram_end()-n;}
bool coordinates(const std::array<float,3>& p){for(auto f:p)if(!std::isfinite(f) || std::abs(f)>1000000.f)return false;return true;}
bool same_spawn(const EntityState& a,const EntityState& b) {
    if((a.kind==EntityKind::Physics && a.definition==5) || (b.kind==EntityKind::Physics && b.definition==5))return false; // Independent authoritative part proxies are never guessed from identical attachment spawn coordinates.
    if(a.kind==EntityKind::Projectile || b.kind==EntityKind::Projectile)return false;
    if(a.kind==EntityKind::Destructible || b.kind==EntityKind::Destructible)for(const auto key:{"destruct_index","destruct_resource"}){auto ia=a.properties.find(key),ib=b.properties.find(key);if(ia==a.properties.end() || ib==b.properties.end() || ia->second!=ib->second)return false;}
    if((a.kind==EntityKind::Physics && a.definition<=2) || (b.kind==EntityKind::Physics && b.definition<=2)){
        auto ia=a.properties.find("marker_index"),ib=b.properties.find("marker_index");
        if(ia==a.properties.end() || ib==b.properties.end() || !std::holds_alternative<int64_t>(ia->second) || std::get<int64_t>(ia->second)<=0 || ia->second!=ib->second)return false;
    }
    if(a.kind==EntityKind::Mission || a.kind==EntityKind::Switch || b.kind==EntityKind::Mission || b.kind==EntityKind::Switch) {
        auto ia=a.properties.find("script_index"),ib=b.properties.find("script_index");
        if(ia==a.properties.end() || ib==b.properties.end() || !std::holds_alternative<int64_t>(ia->second) || !std::holds_alternative<int64_t>(ib->second) || std::get<int64_t>(ia->second)<0 || ia->second!=ib->second)return false;
    }
    if(a.kind==EntityKind::Door || b.kind==EntityKind::Door) {
        auto ra=a.properties.find("brush_resource"),rb=b.properties.find("brush_resource");
        if(ra==a.properties.end() || rb==b.properties.end() || !std::holds_alternative<int64_t>(ra->second) || !std::holds_alternative<int64_t>(rb->second) || std::get<int64_t>(ra->second)<0 || ra->second!=rb->second)return false;
        auto ia=a.properties.find("brush_index"),ib=b.properties.find("brush_index");
        if(ia==a.properties.end() || ib==b.properties.end() || ia->second!=ib->second)return false;
    }
    auto ia=a.properties.find("spawn_position"),ib=b.properties.find("spawn_position");
    if(ia==a.properties.end() || ib==b.properties.end())return false;
    auto pa=std::get_if<std::array<float,3>>(&ia->second),pb=std::get_if<std::array<float,3>>(&ib->second);
    if(!pa || !pb || !coordinates(*pa) || !coordinates(*pb))return false;
    for(unsigned axis=0;axis<3;++axis)if(std::abs((*pa)[axis]-(*pb)[axis])>.001f)return false;return true;
}
bool same_player(const EntityState& a,const EntityState& b) {
    auto ia=a.properties.find("player_slot"),ib=b.properties.find("player_slot");
    if(ia==a.properties.end() || ib==b.properties.end())return false;
    auto pa=std::get_if<int64_t>(&ia->second),pb=std::get_if<int64_t>(&ib->second);
    return pa && pb && *pa>=0 && *pa<int64_t(max_players) && *pa==*pb;
}
std::string missing_binding(const EntityState& e,const NativeWorldSample& local) {
    std::string error="Native creation unsupported for kind "+std::to_string(unsigned(e.kind))+", id "+std::to_string(e.id)+", definition "+std::to_string(e.definition);
    auto source=e.properties.find("spawn_position");if(source==e.properties.end())return error+"; no authoritative spawn transform";
    auto expected=std::get_if<std::array<float,3>>(&source->second);if(!expected)return error+"; invalid authoritative spawn transform";
    error+="; spawn="+std::to_string((*expected)[0])+","+std::to_string((*expected)[1])+","+std::to_string((*expected)[2]);
    double closest=1e100;const EntityState* candidate=nullptr;
    for(const auto& [id,item]:local.entities) {
        if(item.kind!=e.kind || item.definition!=e.definition)continue;
        auto found=item.properties.find("spawn_position");if(found==item.properties.end())continue;
        auto p=std::get_if<std::array<float,3>>(&found->second);if(!p)continue;
        double distance=0;for(unsigned axis=0;axis<3;++axis)distance+=std::abs(double((*p)[axis])-(*expected)[axis]);
        if(distance<closest){closest=distance;candidate=&item;}
    }
    if(!candidate)return error+"; no local object of this definition";
    const auto& p=std::get<std::array<float,3>>(candidate->properties.at("spawn_position"));
    return error+"; nearest="+std::to_string(candidate->id)+" at "+std::to_string(p[0])+","+std::to_string(p[1])+","+std::to_string(p[2])+" distance="+std::to_string(closest);
}
struct Write {uint32_t address,value;unsigned size;uint32_t pickup=0;bool presentation=false;};
struct RenderPropertyNames {std::string visible;std::array<std::string,3> rows;};
const auto& render_property_names(){
    // Schema names never change. In particular, body-render names exceed
    // small-string storage: rebuilding them for every absent part allocates
    // repeatedly even for static level objects. Values and bindings remain
    // freshly checked for every received entity, including unsupported parts.
    static const std::array<RenderPropertyNames,4> names{{
        {"render_visible",{"render_row0","render_row1","render_row2"}},
        {"body_render1_visible",{"body_render1_row0","body_render1_row1","body_render1_row2"}},
        {"body_render2_visible",{"body_render2_row0","body_render2_row1","body_render2_row2"}},
        {"body_render3_visible",{"body_render3_row0","body_render3_row1","body_render3_row2"}},
    }};
    return names;
}
bool lifecycle_property(const std::string& key){return key=="brush_segment" || key=="brush_progress" || key=="movement_mode" || key=="camera_frozen" || key=="selected_weapon" || key=="weapon_modes";}
template<class Keep> EntityState filtered_fields(const EntityState& e,Keep keep){
    EntityState out{e.id,e.kind,e.definition,e.resource,e.owner,{}};
    for(const auto& item:e.properties)if(keep(item.first))out.properties.emplace_hint(out.properties.end(),item);
    return out;
}
// Classify names by their first character before checking the schema group.
// No received value or guest binding is cached: every batch is fully validated.
enum class PlanField : unsigned char {
    Unknown,
    Bomb, HeldFlash, MuzzleFlash, Powerups, Barrel, WeaponBones,
    WeaponModel, MarkerAnimation, Marker, Render, BodyRender, Corpse,
    PlayerNotice, Hint, Combo, Reentry, Sidebar, WeaponBar,
    HeldResource, Destructible, BodyBones, Enemy, Script, Brush,
    WeaponsHidden, WeaponModes, Selection, CameraFrozen, Movement, Availability,
    ProjectileModel, ProjectileSource, ProjectileTrail, ProjectileSpin, ProjectileVisual, EnergyPhase,
    ExtraVisual, EnergyVisual, ProjectileScale, ProjectileRotation, Velocity, Identity,
    CameraRows, CameraMode, CameraEffects, ViewAngles, Position, Health,
    Score, Armor, PlayerScalar, Respawn, WeaponCounts, Ammunition,
    PickupIdentity, PickupMask, GlowPose, Glow, PickupOwnership, Rotor, NpcPiece, BossParts,
};
PlanField plan_field(std::string_view key) {
    if(key=="boss_weakpoints")return PlanField::BossParts;
    if(key.starts_with("part_"))return PlanField::NpcPiece;
    switch(key.empty()?0:key.front()) {
    case 'a':
        if(key=="spawn_position" || key=="player_slot")return PlanField::Identity;
        if(key=="armor")return PlanField::Armor;
        if(key=="ammo" || key=="ammo_secondary" || key=="ammo_maximum")return PlanField::Ammunition;
        break;
    case 'b':
        if(key=="body_flash0" || key=="body_flash1" || key=="body_flash0_mask" || key=="body_flash1_mask")return PlanField::HeldFlash;
        if(key.starts_with("body_render1_") || key.starts_with("body_render2_") || key.starts_with("body_render3_"))return PlanField::BodyRender;
        if(key=="body_weapon_resource")return PlanField::HeldResource;
        if(key.starts_with("body0_bone_") || key.starts_with("body1_bone_") || key.starts_with("body2_bone_") || key.starts_with("body3_bone_"))return PlanField::BodyBones;
        if(key.starts_with("brush_"))return PlanField::Brush;
        break;
    case 'c':
        if(key.starts_with("corpse_"))return PlanField::Corpse;
        if(key=="combo_hud" || key=="combo_banner" || key=="combo_caption" || key=="combo_caption_style")return PlanField::Combo;
        if(key=="camera_frozen")return PlanField::CameraFrozen;
        if(key=="camera_row0" || key=="camera_row1" || key=="camera_row2")return PlanField::CameraRows;
        if(key=="camera_mode")return PlanField::CameraMode;
        if(key=="camera_bob" || key=="camera_shake")return PlanField::CameraEffects;
        if(key=="yaw" || key=="pitch" || key=="combo_timer")return PlanField::PlayerScalar;
        if(key=="combo_phase")return PlanField::Respawn;
        break;
    case 'd':
        if(key.starts_with("destruct_"))return PlanField::Destructible;
        break;
    case 'e':
        if(key.starts_with("enemy_"))return PlanField::Enemy;
        break;
    case 'h':
        if(key.starts_with("hint_"))return PlanField::Hint;
        if(key=="health")return PlanField::Health;
        break;
    case 'l':
        if(key.starts_with("pickup_notice_") || key.starts_with("level_intro_"))return PlanField::PlayerNotice;
        break;
    case 'm':
        if(key=="marker_animation")return PlanField::MarkerAnimation;
        if(key=="marker_index" || key=="marker_active")return PlanField::Marker;
        if(key=="movement_mode")return PlanField::Movement;
        if(key=="score" || key=="mode_score")return PlanField::Score;
        break;
    case 'n':
        if(key=="netricsa_pages")return PlanField::PlayerNotice;
        break;
    case 'p':
        if(key=="powerups")return PlanField::Powerups;
        if(key.starts_with("pickup_notice_") || key.starts_with("level_intro_"))return PlanField::PlayerNotice;
        if(key=="projectile_model")return PlanField::ProjectileModel;
        if(key=="projectile_source")return PlanField::ProjectileSource;
        if(key=="projectile_trail_active")return PlanField::ProjectileTrail;
        if(key=="projectile_spin")return PlanField::ProjectileSpin;
        if(key=="projectile_visual")return PlanField::ProjectileVisual;
        if(key=="projectile_energy_phase")return PlanField::EnergyPhase;
        if(key=="projectile_extra_visual")return PlanField::ExtraVisual;
        if(key=="projectile_energy_visual")return PlanField::EnergyVisual;
        if(key=="projectile_scale")return PlanField::ProjectileScale;
        if(key=="projectile_rotation")return PlanField::ProjectileRotation;
        if(key=="spawn_position" || key=="player_slot")return PlanField::Identity;
        if(key=="position")return PlanField::Position;
        if(key=="yaw" || key=="pitch" || key=="combo_timer")return PlanField::PlayerScalar;
        if(key=="pickup_model" || key=="pickup_value")return PlanField::PickupIdentity;
        if(key=="pickup_view_mask")return PlanField::PickupMask;
        if(key=="pickup_glow_pose")return PlanField::GlowPose;
        if(key=="pickup_glow")return PlanField::Glow;
        if(key=="pickup_ownership")return PlanField::PickupOwnership;
        break;
    case 'r':
        if(key=="render_visible" || key=="render_row0" || key=="render_row1" || key=="render_row2")return PlanField::Render;
        if(key=="reentry_hud")return PlanField::Reentry;
        if(key=="rotor_visible" || key=="rotor_mask" || key=="rotor_row0" || key=="rotor_row1" || key=="rotor_row2")return PlanField::Rotor;
        break;
    case 's':
        if(key.starts_with("script_") || key.starts_with("trigger_") || key.starts_with("switch_"))return PlanField::Script;
        if(key=="selected_weapon")return PlanField::Selection;
        if(key=="spawn_position" || key=="player_slot")return PlanField::Identity;
        if(key=="score" || key=="mode_score")return PlanField::Score;
        break;
    case 't':
        if(key.starts_with("script_") || key.starts_with("trigger_") || key.starts_with("switch_"))return PlanField::Script;
        break;
    case 'v':
        if(key=="velocity")return PlanField::Velocity;
        if(key=="view_angles")return PlanField::ViewAngles;
        break;
    case 'w':
        if(key=="weapon_bomb0" || key=="weapon_bomb1" || key=="weapon_bomb0_masks" || key=="weapon_bomb1_masks")return PlanField::Bomb;
        if(key=="weapon_model0_flash" || key=="weapon_model1_flash" || key=="weapon_model0_flash_mask" || key=="weapon_model1_flash_mask" || key=="weapon_model0_flash_aux" || key=="weapon_model1_flash_aux" || key=="weapon_model0_flash_aux_mask" || key=="weapon_model1_flash_aux_mask" || key=="weapon_model0_flash_aux2" || key=="weapon_model1_flash_aux2" || key=="weapon_model0_flash_aux2_mask" || key=="weapon_model1_flash_aux2_mask")return PlanField::MuzzleFlash;
        if(key=="weapon_model0_barrel" || key=="weapon_model1_barrel")return PlanField::Barrel;
        if(key.starts_with("weapon_model0_bone_") || key.starts_with("weapon_model1_bone_"))return PlanField::WeaponBones;
        if(key=="weapon_model_slot" || key=="weapon_model_visibility" || key=="weapon_model_masks" || key.starts_with("weapon_model0_row") || key.starts_with("weapon_model1_row"))return PlanField::WeaponModel;
        if(key=="weapon_sidebar")return PlanField::Sidebar;
        if(key=="weapon_bar" || key=="weapon_bar_pose")return PlanField::WeaponBar;
        if(key=="weapons_hidden")return PlanField::WeaponsHidden;
        if(key=="weapon_modes")return PlanField::WeaponModes;
        if(key=="weapon_available")return PlanField::Availability;
        if(key=="weapon_owned_counts" || key=="weapon_active_counts")return PlanField::WeaponCounts;
        break;
    case 'y':
        if(key=="yaw" || key=="pitch" || key=="combo_timer")return PlanField::PlayerScalar;
        break;
    }
    return PlanField::Unknown;
}
bool build_writes(const EntityState& e,uint32_t object,std::vector<Write>& writes,std::string& error,bool defer_presentation=false,bool defer_lifecycle=false) {
    ssne::perf::detail::Scope perf_detail(ssne::perf::detail::PropertyPlan);
    ssne::perf::detail::count(ssne::perf::detail::Properties,e.properties.size());
    auto fail=[&](const std::string& why){error=why;return false;};
    const bool corpse=e.kind==EntityKind::Physics && e.definition==3 && mem_read32(object+0x48)==0x8020C6B8u;
    const auto player=e.kind==EntityKind::Player,pickup=e.kind==EntityKind::Pickup;
    if(e.kind==EntityKind::Physics && e.definition==5){
        if(mem_read32(object+0x48)!=0x8020B990u || !validate_native_npc_piece(e,error))return false;
        const auto binding=native_render_binding(object,e.kind);
        if(!binding || !binding->model || ssne::native_static_model_name(mem_read32(binding->model+0x1C))!=std::get<std::string>(e.properties.at("part_model")) || mem_read32(object+0xD8)!=std::get<int64_t>(e.properties.at("part_side")))return fail("Native NPC part asset/side differs object="+std::to_string(object)+" expected="+std::get<std::string>(e.properties.at("part_model"))+" actual="+(binding && binding->model?ssne::native_static_model_name(mem_read32(binding->model+0x1C)):"no proxy")+" side="+std::to_string(mem_read32(object+0xD8)));
        writes.push_back({object+0x1C,uint32_t(std::get<int64_t>(e.properties.at("part_flags"))),4});
        writes.push_back({binding->model+0x30,uint32_t(std::get<int64_t>(e.properties.at("part_mask"))),4});
    }
    if(!match_native_pickup_metadata(object,e,error))return false;
    if(!validate_pickup_glow_metadata(e,error))return false;
    if(e.properties.contains("rotor_visible")){
        const auto child=readable(object,0x250)?mem_read32(object+0x24C):0;const auto rotor=native_render_binding(child,EntityKind::Pickup);
        const auto* visible=std::get_if<int64_t>(&e.properties.at("rotor_visible"));const auto mask=e.properties.find("rotor_mask");
        if(e.kind!=EntityKind::Enemy || e.definition!=20 || !readable(child,0x118) || mem_read32(child+0x48)!=0x8020B228u || !rotor || !rotor->model || !visible || *visible<0 || *visible>1 || mask==e.properties.end() || !std::holds_alternative<int64_t>(mask->second) || std::get<int64_t>(mask->second)<0 || uint64_t(std::get<int64_t>(mask->second))>UINT32_MAX)return fail("Invalid native propeller binding/visibility");
        writes.push_back({rotor->model+0x2D,uint32_t(*visible),1});writes.push_back({rotor->model+0x30,uint32_t(std::get<int64_t>(mask->second)),4});writes.push_back({rotor->model+0x34,*visible?0xFFFFFFF6u:0xFFFFFFFFu,4});
        for(unsigned row=0;row<3;++row){const auto it=e.properties.find("rotor_row"+std::to_string(row));const auto* values=it==e.properties.end()?nullptr:std::get_if<std::array<float,4>>(&it->second);if(!values)return fail("Incomplete native propeller matrix");
            for(unsigned col=0;col<4;++col){const float value=(*values)[col];if(!std::isfinite(value) || std::abs(value)>1000000.f)return fail("Invalid native propeller matrix");writes.push_back({rotor->matrix+row*16+col*4,f2u(value),4});}
            writes.push_back({child+0x20+row*4,f2u((*values)[3]),4});
        }
    }else if(e.properties.contains("rotor_mask") || e.properties.contains("rotor_row0") || e.properties.contains("rotor_row1") || e.properties.contains("rotor_row2"))return fail("Propeller matrix has no visibility state");
    const bool brush=e.kind==EntityKind::Door && e.definition==1;
    const bool destructible=e.kind==EntityKind::Destructible && (e.definition==1 || e.definition==2);
    const bool trigger=e.kind==EntityKind::Mission && e.definition==1;
    const bool button=e.kind==EntityKind::Switch && e.definition==1;
    const bool script=trigger || button;
    const bool marker=e.kind==EntityKind::Physics && (e.definition==1 || e.definition==2);
    const bool detached_rotor=e.kind==EntityKind::Physics && e.definition==4;
    if(detached_rotor && (mem_read32(object+0x48)!=0x8020B228u || !validate_native_detached_rotor(e,error)))return false;
    const auto* projectile_type=e.kind==EntityKind::Projectile?native_projectile_class(e.definition):nullptr;
    const bool rocket=projectile_type!=nullptr;
    if(!player && !pickup && e.kind!=EntityKind::Enemy && !brush && !script && !rocket && !marker && !corpse && !detached_rotor && !(e.kind==EntityKind::Physics && e.definition==5) && !destructible)return fail("Native class schema is not supported");
    if(destructible && !validate_native_destructible(object,e,error))return false;
    if(!readable(object,player?0x790:pickup?0x184:rocket?projectile_type->size:brush?0x13C:trigger?0xA8:button?0x68:0xD4))return fail("Native object storage is unavailable");
    if(rocket && (mem_read32(object+0x48)!=projectile_type->vtable || !validate_native_projectile_creation(e,error)))return false;
    auto add=[&](uint32_t address,uint32_t value,unsigned size=4){writes.push_back({address,value,size});};
    auto visible_write=[&](uint32_t proxy,uint32_t visible,bool presentation=false){
        if(mem_read8(proxy+0x2D)==visible)return;
        // Same enable byte and culling-cache reset as native 8013E7E8.
        writes.push_back({proxy+0x2D,visible,1,0,presentation});
        writes.push_back({proxy+0x34,visible?0xFFFFFFF6u:0xFFFFFFFFu,4,0,presentation});
    };
    if(marker){
        auto i=e.properties.find("marker_index"),a=e.properties.find("marker_active");
        if(i==e.properties.end() || !std::holds_alternative<int64_t>(i->second))return fail("Missing native marker identity");
        int64_t active=bool(mem_read32(object+0x8C));
        if(a!=e.properties.end()){const auto* v=std::get_if<int64_t>(&a->second);if(!v)return fail("Invalid marker state type");active=*v;}
        if(!validate_native_marker(object,e.definition,std::get<int64_t>(i->second),active,error))return false;
        if(!defer_presentation && active!=int64_t(bool(mem_read32(object+0x8C))))return fail("Marker visibility requires native lifecycle context");
    }
    // Non-player body fields are rejected by the schema loop below. Do not
    // repeatedly search for twelve absent player-only fields on every prop.
    for(unsigned model_index=0;model_index<(player?4u:1u);++model_index){
    const auto& names=render_property_names()[model_index];
    if(auto visibility=e.properties.find(names.visible);visibility!=e.properties.end()){
        if(model_index && !player)return fail("Body render pose targets another class");
        const auto* visible=std::get_if<int64_t>(&visibility->second);const auto binding=native_render_binding(object,e.kind,model_index);
        if(!visible || *visible<0 || *visible>1 || (!binding && !(player && defer_presentation)))return fail("Invalid native render visibility/binding");
        unsigned rows=0;for(unsigned row=0;row<3;++row)rows+=e.properties.contains(names.rows[row]);
        if((rows!=0 && rows!=3) || (*visible && rows!=3))return fail("Incomplete native render pose");
        if(!(player && defer_presentation) && !binding->model && *visible){
            error="Visible native model is missing locally: entity="+std::to_string(e.id)+" kind="+std::to_string(unsigned(e.kind))+" definition="+std::to_string(e.definition)+" object="+std::to_string(object)+" part="+std::to_string(model_index);return false;
        }
        if(!(player && defer_presentation) && binding->model)visible_write(binding->model,uint32_t(*visible),player);
        for(unsigned row=0;row<rows;++row){auto* values=std::get_if<std::array<float,4>>(&e.properties.at(names.rows[row]));if(!values)return fail("Invalid render matrix type");
            for(unsigned col=0;col<4;++col){if(!std::isfinite((*values)[col]) || std::abs((*values)[col])>1000000.f)return fail("Invalid render matrix coefficient");if(!(player && defer_presentation) && binding->model)writes.push_back({binding->matrix+row*16+col*4,f2u((*values)[col]),4,0,player});}
        }
    }else for(unsigned row=0;row<3;++row)if(e.properties.contains(names.rows[row]))return fail("Render pose has no visibility state");
    }
    const auto camera=player?mem_read32(object+0x368):0;
    // The CameraRows case rejects non-player input; its storage is never
    // inspected on that class. Player rows still require the complete trio.
    const unsigned matrix_rows=player?(e.properties.contains("camera_row0")+e.properties.contains("camera_row1")+e.properties.contains("camera_row2")):0;
    if(matrix_rows && (matrix_rows!=3 || !player || mem_read32(0x80000000u)!=0x47334250u || !readable(camera,0x100) || mem_read32(camera)!=0x80210880u || mem_read32(camera+4)!=object)){
        error="Incomplete or invalid native camera binding: entity="+std::to_string(e.id)+" object="+std::to_string(object)+" slot="+std::to_string(player?mem_read32(object+0x17C):max_players)+" camera="+std::to_string(camera)+" vtable="+std::to_string(readable(camera,8)?mem_read32(camera):0)+" owner="+std::to_string(readable(camera,8)?mem_read32(camera+4):0)+" rows="+std::to_string(matrix_rows);return false;
    }
    if(auto slot_property=e.properties.find("weapon_model_slot");slot_property!=e.properties.end()) {
        const auto* slot=std::get_if<int64_t>(&slot_property->second);
        const auto visibility_property=e.properties.find("weapon_model_visibility");
        const auto* visibility=visibility_property==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&visibility_property->second);
        const auto masks_property=e.properties.find("weapon_model_masks");
        const auto* masks=masks_property==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&masks_property->second);
        if(!player || !slot || *slot<0 || *slot>=12 || !visibility || visibility->size()!=2)return fail("Invalid authoritative weapon model binding");
        if(!masks || masks->size()!=2)return fail("Missing authoritative weapon view masks");
        for(auto mask:*masks)if(mask<0 || uint64_t(mask)>UINT32_MAX)return fail("Invalid authoritative weapon view mask");
        if(auto selected=e.properties.find("selected_weapon");selected!=e.properties.end())if(!std::holds_alternative<int64_t>(selected->second) || std::get<int64_t>(selected->second)!=*slot)return fail("Weapon pose does not match the selected weapon");
        for(unsigned index=0;index<2;++index) {
            const auto state=(*visibility)[index];
            if(state< -1 || state>1)return fail("Invalid authoritative weapon model visibility");
            const auto prefix="weapon_model"+std::to_string(index)+"_row";
            if(state<0){
                for(unsigned row=0;row<3;++row)if(e.properties.contains(prefix+std::to_string(row)))return fail("Absent weapon model has a pose");
                const auto instance=mem_read32(object+0x4BC+unsigned(*slot)*24+index*4);
                const auto model=readable(instance,0x228)?mem_read32(instance):0;
                if(model){const auto vt=readable(model,0x40)?mem_read32(model):0;if(!readable(vt,0x30) || mem_read32(vt+0x2C)!=0x8013E954u)return fail("Invalid stale weapon model binding");writes.push_back({model+0x30,0,4,0,true});}
                continue;
            }
            const auto instance=mem_read32(object+0x4BC+unsigned(*slot)*24+index*4);
            const auto model=readable(instance,0x228)?mem_read32(instance):0;
            const auto vt=readable(model,0x40)?mem_read32(model):0;
            const auto matrix=readable(model,8)?mem_read32(model+4):0;
            if(!readable(instance,0x228))return fail("Native weapon instance unavailable");
            const bool missing=!model;
            if(!missing && (mem_read32(0x80000000u)!=0x47334250u || !readable(vt,0x30) || mem_read32(vt+0x2C)!=0x8013E954u || !readable(matrix,48)))return fail("Invalid native weapon model/matrix binding");
            if(missing && state>0 && !defer_presentation)return fail("Visible weapon model requires native resource preparation");
            if(missing && !readable(mem_read32(instance+4),4))return fail("Native weapon model has no recreation resource");
            for(unsigned row=0;row<3;++row) {
                const auto found=e.properties.find(prefix+std::to_string(row));
                const auto* values=found==e.properties.end()?nullptr:std::get_if<std::array<float,4>>(&found->second);
                if(!values)return fail("Incomplete authoritative weapon pose");
                for(unsigned column=0;column<4;++column){if(!std::isfinite((*values)[column]) || std::abs((*values)[column])>(column==3?1000000.f:10000.f))return fail("Invalid authoritative weapon pose coefficient");if(!missing)writes.push_back({matrix+row*16+column*4,f2u((*values)[column]),4,0,true});}
            }
            if(!missing)visible_write(model,uint32_t(state),true);
            if(!missing)writes.push_back({model+0x30,uint32_t((*masks)[index]),4,0,true});
            const auto bone="weapon_model"+std::to_string(index)+"_bone_";
            const auto count=e.properties.find(bone+"count"),layout=e.properties.find(bone+"layout"),pose=e.properties.find(bone+"matrices");
            if(count!=e.properties.end() || layout!=e.properties.end() || pose!=e.properties.end()){
                const auto* n=count==e.properties.end()?nullptr:std::get_if<int64_t>(&count->second);
                const auto* signature=layout==e.properties.end()?nullptr:std::get_if<int64_t>(&layout->second);
                const auto* values=pose==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&pose->second);
                const auto binding=native_weapon_pose_binding(object,unsigned(*slot),index);
                if(!n || !signature || !values || *n<1 || *n>256 || values->size()!=size_t(*n)*12 || !binding || binding->count!=*n || binding->layout!=uint64_t(*signature))return fail("Invalid authoritative weapon skeleton binding");
                if(!binding->model && !defer_presentation)return fail("Weapon skeleton requires native resource preparation");
                for(unsigned i=0;i<values->size();++i){
                    if(!std::isfinite((*values)[i]) || std::abs((*values)[i])>10000.f)return fail("Invalid authoritative weapon bone transform");
                    if(binding->model)writes.push_back({binding->matrices+i*4,f2u((*values)[i]),4,0,true});
                }
            }
        }
    }
    // Nothing mutates guest memory while this plan is built. Reuse each
    // checked skeleton binding across its count/layout/matrices properties.
    std::array<std::optional<NativeEnemyPoseBinding>,4> body_bindings;
    std::array<bool,4> body_checked{};
    std::optional<NativeEnemyPoseBinding> enemy_binding;bool enemy_checked=false;
    // These validators inspect the complete entity, not just the current
    // property. A plan cannot mutate RAM, so validate each group once rather
    // than repeating its resource/schema walk for every member of the group.
    bool notice_checked=false,corpse_checked=false,hint_checked=false,combo_checked=false,weapon_bar_checked=false;
    for(const auto& [key,value]:e.properties) {
        if(defer_lifecycle && lifecycle_property(key))continue;
        switch(plan_field(key)) {
        case PlanField::Bomb: {
            const auto prefix=key.substr(0,12);const auto data=e.properties.find(prefix),masks=e.properties.find(prefix+"_masks"),slot=e.properties.find("weapon_model_slot");
            const auto* v=data==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&data->second);
            const auto* m=masks==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&masks->second);
            if(!player || slot==e.properties.end() || !std::holds_alternative<int64_t>(slot->second) || std::get<int64_t>(slot->second)!=11 || !v || !m || m->size()!=2 || (*m)[0]<0 || uint64_t((*m)[0])>UINT32_MAX || (*m)[1]<0 || uint64_t((*m)[1])>UINT32_MAX )return fail("Invalid authoritative bomb presentation binding");
            if(!ssne::validate_native_bomb_visual(*v,{uint32_t((*m)[0]),uint32_t((*m)[1])},error))return false;
            if(!defer_presentation)return fail("Bomb presentation requires native lifecycle context");continue;
        }
        case PlanField::HeldFlash: {
            const auto prefix=key.substr(0,11);const auto data=e.properties.find(prefix),mask=e.properties.find(prefix+"_mask");
            const auto* v=data==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&data->second);
            const auto* m=mask==e.properties.end()?nullptr:std::get_if<int64_t>(&mask->second);
            if(!player || !v || (v->size()!=14 && v->size()!=17) || !m || *m<0 || uint64_t(*m)>0xFFFFFFFFull || ((*v)[0]!=0 && (*v)[0]!=1) || (*v)[1]<0 || (*v)[1]>10)return fail("Invalid authoritative held-weapon flash state");
            for(auto f:*v)if(!std::isfinite(f) || std::abs(f)>1000000.f)return fail("Invalid held-weapon flash transform");
            if(!defer_presentation)return fail("Held-weapon flash requires native lifecycle context");continue;
        }
        case PlanField::MuzzleFlash: {
            const auto slot=e.properties.find("weapon_model_slot");
            const unsigned effect=key.find("_aux2")!=std::string::npos?2:key.find("_aux")!=std::string::npos?1:0;
            const auto prefix=native_weapon_flash_key(key[12]-'0',effect);const auto data=e.properties.find(prefix),mask=e.properties.find(prefix+"_mask");
            const auto* v=data==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&data->second);
            const auto* m=mask==e.properties.end()?nullptr:std::get_if<int64_t>(&mask->second);
            if(!player || slot==e.properties.end() || !std::holds_alternative<int64_t>(slot->second) || !native_weapon_flash_offset(unsigned(std::get<int64_t>(slot->second)),effect) || !v || (v->size()!=14 && v->size()!=17) || !m || *m<0 || uint64_t(*m)>0xFFFFFFFFull || ((*v)[0]!=0 && (*v)[0]!=1) || (*v)[1]<0 || (*v)[1]>10)return fail("Invalid authoritative muzzle flash state");
            for(auto f:*v)if(!std::isfinite(f) || std::abs(f)>1000000.f)return fail("Invalid muzzle flash transform");
            if(!defer_presentation)return fail("Muzzle flash requires native lifecycle context");continue;
        }
        case PlanField::Powerups: {
            const auto* values=std::get_if<std::vector<float>>(&value);
            if(!player || !values || !validate_native_powerups(object,*values,error))return fail("Invalid native power-up state");
            if(!defer_presentation)return fail("Power-up HUD requires native lifecycle context");
            continue;
        }
        case PlanField::Barrel: {
            const auto slot=e.properties.find("weapon_model_slot");const auto* data=std::get_if<std::vector<float>>(&value);
            if(!player || slot==e.properties.end() || !std::holds_alternative<int64_t>(slot->second) || std::get<int64_t>(slot->second)!=4 || !data || (data->size()!=1 && data->size()!=13))return fail("Invalid minigun barrel state");
            if((*data)[0]!=0 && (*data)[0]!=1)return fail("Invalid minigun barrel visibility");
            if(data->size()==1 && (*data)[0]!=0)return fail("Missing minigun barrel transform");
            for(unsigned i=1;i<data->size();++i)if(!std::isfinite((*data)[i]) || std::abs((*data)[i])>1000000.f)return fail("Invalid minigun barrel transform");
            const auto barrel=native_minigun_barrel(object,unsigned(key[12]-'0'));
            if(!barrel)return fail("Invalid local minigun barrel binding");
            if(barrel->model){visible_write(barrel->model,uint32_t((*data)[0]),true);for(unsigned i=1;i<data->size();++i)writes.push_back({barrel->matrix+(i-1)*4,f2u((*data)[i]),4,0,true});}
            else if((*data)[0] && !defer_presentation)return fail("Minigun barrel requires native resource preparation");
            continue;
        }
        case PlanField::WeaponBones: {
            const auto suffix=key.substr(19);
            if(!player || !e.properties.contains("weapon_model_slot") || (suffix!="count" && suffix!="layout" && suffix!="matrices"))return fail("Unsupported weapon skeleton property");
            const auto index=unsigned(key[12]-'0');
            const auto* visibility=std::get_if<std::vector<int64_t>>(&e.properties.at("weapon_model_visibility"));
            if(!visibility || visibility->size()!=2 || (*visibility)[index]<0)return fail("Absent weapon model has a skeleton pose");
            continue;
        }
        case PlanField::WeaponModel: {
            if(!e.properties.contains("weapon_model_slot") || ((key.starts_with("weapon_model0_row") || key.starts_with("weapon_model1_row")) && (key.size()!=18 || key.back()<'0' || key.back()>'2')))return fail("Unsupported weapon model presentation property");
            continue;
        }
        case PlanField::MarkerAnimation: {
            const auto* animation=std::get_if<std::vector<float>>(&value);
            if(!marker || e.definition!=2 || !animation || !validate_native_arrow_animation(object,*animation,error))return fail("Invalid authoritative arrow animation");
            continue;
        }
        case PlanField::Marker: {if(!marker)return fail("Marker state targets another class");continue;
        }
        case PlanField::Render: {continue;
        }
        case PlanField::BodyRender: {
            const auto suffix=key.substr(13);if(!player || (suffix!="visible" && suffix!="row0" && suffix!="row1" && suffix!="row2"))return fail("Unsupported body render field");continue;
        }
        case PlanField::Corpse: {
            if(!corpse)return false;
            if(!corpse_checked){if(!validate_native_corpse_creation(e,error))return false;corpse_checked=true;}
            if(key=="corpse_definition"){if(mem_read32(object+0x124)!=std::get<int64_t>(value))return fail("Corpse geometry definition differs");}
            else if(key=="corpse_mode"){if(mem_read32(object+0x174)!=std::get<int64_t>(value))return fail("Corpse geometry mode differs");}
            else if(key=="corpse_flags")add(object+0x1C,uint32_t(std::get<int64_t>(value)));
            else if(key=="corpse_matrix"){const auto& m=std::get<std::vector<float>>(value);for(unsigned i=0;i<12;++i)add(object+0x98+i*4,f2u(m[i]));}
            else if(key=="corpse_lifecycle" || key=="corpse_dissolve_matrices" || key.starts_with("corpse_particles") || key=="corpse_particle_count" || key=="corpse_ghost_matrix"){} // Applied after the received geometry.
            continue;
        }
        case PlanField::PlayerNotice: {
            if(key!="pickup_notice_sequence" && key!="pickup_notice_flags" && key!="pickup_notice_text" && key!="level_intro_text_ids" && key!="level_intro_image" && key!="netricsa_pages")return fail("Unknown player notice field");
            if(!notice_checked){if(!validate_native_player_notices(object,e,error))return false;notice_checked=true;}continue;
        }
        case PlanField::Hint: {
            if(key!="hint_active" && key!="hint_text" && key!="hint_flags" && key!="hint_timer")return fail("Unknown interaction hint field");
            if(!hint_checked){if(!validate_native_interaction_hint(object,e,error))return false;hint_checked=true;}
            continue; // The original UI setter owns text and fade resources.
            break;
        }
        case PlanField::Combo: {if(!combo_checked){if(!validate_native_combo_hud(object,e,error))return false;combo_checked=true;}continue;
        }
        case PlanField::Reentry: {if(!validate_native_reentry_hud(object,e,error))return false;continue;
        }
        case PlanField::Sidebar: {if(!validate_native_weapon_sidebar(object,e,error))return false;continue;
        }
        case PlanField::WeaponBar: {if(!weapon_bar_checked){if(!validate_native_weapon_bar(object,e,error))return false;weapon_bar_checked=true;}continue;
        }
        case PlanField::HeldResource: {const auto* n=std::get_if<int64_t>(&value);if(!player || !n || *n< -1 || *n>12)return fail("Invalid authoritative held weapon resource");continue;
        }
        case PlanField::Destructible: {
            if(!destructible)return fail("Destructible state targets another class");
            if(key=="destruct_health" || key=="destruct_damage")add(object+(key=="destruct_health"?0x58:0x5C),f2u(std::get<float>(value)));
            else if(key=="destruct_destroyed"){if(e.definition==1)add(object+0x60,uint32_t(std::get<int64_t>(value)));}
            else if(e.definition==2 && (key=="destruct_active" || key=="destruct_stage"))add(object+(key=="destruct_active"?0x84:0x88),uint32_t(std::get<int64_t>(value)));
            else if(key!="destruct_index" && key!="destruct_resource" && !(e.definition==2 && key=="destruct_state"))return fail("Unsupported destructible field");continue;
        }
        case PlanField::BodyBones: {
            const auto prefix=key.substr(0,11);const unsigned part=key[4]-'0';
            if(!player)return fail("Body pose targets another class");
            const auto mode=e.properties.find("movement_mode");
            const auto requested=mode==e.properties.end()?nullptr:std::get_if<int64_t>(&mode->second);
            const bool vehicle_exit=defer_presentation && !mem_read32(object+0x148) && mem_read32(object+0x340)==3 && mem_read32(object+0x6C4) && requested && *requested==1;
            auto count=e.properties.find(prefix+"count"),layout=e.properties.find(prefix+"layout"),pose=e.properties.find(prefix+"matrices");
            if(count==e.properties.end() || layout==e.properties.end() || pose==e.properties.end())return fail("Incomplete native body pose");
            if((!defer_presentation || (part<2 && !vehicle_exit)) && !body_checked[part]){body_bindings[part]=native_body_pose_binding(object,part);body_checked[part]=true;}
            const auto* n=std::get_if<int64_t>(&count->second);const auto* hash=std::get_if<int64_t>(&layout->second);const auto* matrices=std::get_if<std::vector<float>>(&pose->second);const auto& binding=body_bindings[part];
            if(!n || !hash || !matrices || *n<1 || *n>256 || matrices->size()!=size_t(*n)*12)return fail("Invalid native body skeleton metadata");
            // Equip can replace held proxies. Validate the new local layout
            // only after native selection, never against the previous gun.
            // The body/head models stay in place across equip. Reject their
            // incompatible skeletons before any native lifecycle mutation.
            if((!defer_presentation || (part<2 && !vehicle_exit)) && (!binding || !binding->model || *n!=binding->count || uint64_t(*hash)!=binding->layout))return fail("Native body skeleton differs: actor="+std::to_string(object)+" slot="+std::to_string(mem_read32(object+0x17C))+" part="+std::to_string(part)+" expected_count="+std::to_string(*n)+" local_count="+std::to_string(binding?binding->count:0)+" expected_layout="+std::to_string(uint64_t(*hash))+" local_layout="+std::to_string(binding?binding->layout:0)+" health="+std::to_string(u2f(mem_read32(object+0xCC)))+" respawn="+std::to_string(mem_read32(object+0x3AC)));
            if(key==prefix+"matrices"){for(unsigned i=0;i<matrices->size();++i){if(!std::isfinite((*matrices)[i]) || std::abs((*matrices)[i])>10000.f)return fail("Invalid body bone coefficient");if(!defer_presentation)writes.push_back({binding->matrices+i*4,f2u((*matrices)[i]),4,0,true});}}
            else if(key!=prefix+"count" && key!=prefix+"layout")return fail("Unsupported body pose field");continue;
        }
        case PlanField::Enemy: {
            if(e.kind!=EntityKind::Enemy && !corpse)return fail("Enemy pose targets another class");
            if(key=="enemy_rotation" || key=="enemy_scale") {
                const auto* v=std::get_if<std::array<float,3>>(&value);if(!v)return fail("Invalid enemy transform type");
                for(unsigned axis=0;axis<3;++axis){if(!std::isfinite((*v)[axis]) || (key=="enemy_rotation"?std::abs((*v)[axis])>10000.f:((*v)[axis]<=0 || (*v)[axis]>100)))return fail("Invalid enemy transform coefficient");add(object+(key=="enemy_rotation"?0x2C:0x68)+axis*4,f2u((*v)[axis]));}
            } else if(key=="enemy_bone_count" || key=="enemy_bone_layout" || key=="enemy_bone_matrices") {
                if(!enemy_checked){
                    const auto count=e.properties.find("enemy_bone_count"),layout=e.properties.find("enemy_bone_layout"),pose=e.properties.find("enemy_bone_matrices");
                    const auto* n=count==e.properties.end()?nullptr:std::get_if<int64_t>(&count->second);const auto* signature=layout==e.properties.end()?nullptr:std::get_if<int64_t>(&layout->second);const auto* matrices=pose==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&pose->second);
                    enemy_binding=corpse?native_corpse_pose_binding(object):native_enemy_pose_binding(object);
                    if(!n || !signature || !matrices || *n<=0 || *n>256 || matrices->size()!=size_t(*n)*12 || !enemy_binding || enemy_binding->count!=*n || enemy_binding->layout!=uint64_t(*signature))return fail("Enemy skeleton layout does not match its native resource");
                    enemy_checked=true;
                }
                // Count/layout/matrices share one complete metadata check in
                // this immutable plan. Every matrix coefficient remains fresh.
                if(key=="enemy_bone_matrices"){
                    const auto& matrices=std::get<std::vector<float>>(value);
                    for(unsigned i=0;i<matrices.size();++i){if(!std::isfinite(matrices[i]) || std::abs(matrices[i])>10000.f)return fail("Invalid enemy bone transform");add(enemy_binding->matrices+i*4,f2u(matrices[i]));}
                }
            } else return fail("Unsupported enemy presentation field");
            continue;
        }
        case PlanField::Script: {
            if(!script || mem_read32(object+0x48)!=(trigger?0x80209758u:0x80208B60u))return fail("Script state targets another native class");
            if(key=="script_index") {
                const auto* index=std::get_if<int64_t>(&value);auto resolved=index?native_script_reference_address(*index):std::nullopt;
                // Unindexed authored owners bind uniquely by class and spawn
                // before this preflight. Zero never becomes a target lookup.
                const bool unindexed=index && *index==0 && mem_read32(object+0x50)==0;
                if(!unindexed && (!resolved || *resolved!=object))return fail("Native script level index mismatch");
            } else if(key=="script_references") {
                const auto* references=std::get_if<std::vector<int64_t>>(&value);if(!references || references->size()!=(trigger?11u:1u))return fail("Invalid native script reference slots");
                for(unsigned i=0;i<references->size();++i){auto target=native_script_reference_address((*references)[i]);if(!target)return fail("Native script reference cannot be resolved in this level");add(object+0x58+i*4,*target);}
            } else if(key=="trigger_timer") {
                const auto* timer=std::get_if<float>(&value);if(!trigger || !timer || !std::isfinite(*timer) || *timer<0 || *timer>1000000.f)return fail("Invalid native trigger timer");add(object+0x98,f2u(*timer));
            } else {
                uint32_t offset=0;bool counter=false;
                if(key=="script_enabled")offset=0x4C;
                else if(trigger) {
                    if(key=="trigger_previous_enabled")offset=0x88;
                    else if(key=="trigger_enabled")offset=0x8C;
                    else if(key=="trigger_auto_start")offset=0x9C;
                    else if(key=="trigger_completed")offset=0xA0;
                    else if(key=="trigger_dispatched")offset=0xA4;
                    else if(key=="trigger_pulses"){offset=0x90;counter=true;}
                    else if(key=="trigger_firings"){offset=0x94;counter=true;}
                } else if(key=="switch_enabled")offset=0x60;
                else if(key=="switch_available")offset=0x64;
                const auto* n=std::get_if<int64_t>(&value);
                if(!offset || !n || *n<0 || uint64_t(*n)>(counter?0xFFFFFFFFull:1ull))return fail("Invalid native script scalar");
                add(object+offset,uint32_t(*n));
            }
            continue;
        }
        case PlanField::Brush: {
            if(!brush || mem_read32(object+0x48)!=0x80206868u)return fail("Moving-brush field targets another native class");
            if(key=="brush_index") {
                const auto* index=std::get_if<int64_t>(&value);if(!index || *index<0 || *index>32767 || *index!=mem_read32(object+0x50))return fail("Invalid moving-brush level identity");
            } else if(key=="brush_resource") {
                const auto* resource=std::get_if<int64_t>(&value);const auto cfg=mem_read32(object+0x5C);
                if(!resource || *resource<0 || !readable(cfg,0x38) || mem_read32(cfg+0xC)!=*resource)return fail("Invalid moving-brush resource identity");
            } else if(key=="brush_segment" || key=="brush_progress") {
                return fail("Moving-brush geometry requires native lifecycle context");
            } else if(key=="brush_active" || key=="brush_completed") {
                const auto* flag=std::get_if<int64_t>(&value);if(!flag || (*flag!=0 && *flag!=1))return fail("Invalid moving-brush state flag");
                add(object+(key=="brush_active"?0xB0:0x58),uint32_t(*flag));
            } else return fail("Unsupported moving-brush field");
            continue;
        }
        case PlanField::WeaponsHidden: {
            const auto* hidden=std::get_if<int64_t>(&value);if(!player || !hidden || (*hidden!=0 && *hidden!=1))return fail("Invalid authoritative weapon visibility rule");continue;
        }
        case PlanField::WeaponModes: {
            const auto selected=e.properties.find("selected_weapon");
            const auto* slot=selected==e.properties.end()?nullptr:std::get_if<int64_t>(&selected->second);
            const auto* modes=std::get_if<std::vector<int64_t>>(&value);
            std::vector<int64_t> current;
            if(!player || !slot || *slot<0 || *slot>=12 || !modes ||
               !validate_native_weapon_modes(object,unsigned(*slot),*modes,error) ||
               !capture_native_weapon_modes(object,unsigned(*slot),current,error) || current!=*modes)
                return fail("Weapon mode change requires native lifecycle context");
            continue;
        }
        case PlanField::Selection: {
            const auto* selected=std::get_if<int64_t>(&value);
            if(!player || !selected || *selected<0 || *selected>=12 || !validate_native_weapon_selection(object,unsigned(*selected),error) || mem_read32(object+0x5DC)!=uint32_t(*selected))return fail("Weapon change requires native lifecycle context");
            continue;
        }
        case PlanField::CameraFrozen: {
            auto frozen=std::get_if<int64_t>(&value);if(!player || !frozen || (*frozen!=0 && *frozen!=1) || mem_read32(object+0x370)!=uint32_t(*frozen))return fail("Camera change requires native lifecycle context");continue;
        }
        case PlanField::Movement: {
            const auto* mode=std::get_if<int64_t>(&value);if(!player || !mode || *mode<0 || *mode>3 || !validate_native_movement_mode(object,unsigned(*mode),error) || mem_read32(object+0x340)!=uint32_t(*mode))return fail("Movement change requires native lifecycle context");
            continue; // applied through the actual native controller lifecycle
            break;
        }
        case PlanField::Availability: {
            const auto* flags=std::get_if<std::vector<int64_t>>(&value);
            if(!player || !flags || flags->size()!=12)return fail("Invalid native weapon availability");
            for(auto flag:*flags)if(flag!=0 && flag!=1)return fail("Invalid native weapon availability flag");
            continue; // authority metadata, not an inventory grant
            break;
        }
        case PlanField::ProjectileModel: {
            const auto* model=std::get_if<int64_t>(&value);
            if(!rocket || (e.definition!=10 && e.definition!=15) || !model || *model<0 || *model>=165 || mem_read16(object+0xD4)!=uint16_t(*model))return fail("Generic projectile model requires creation lifecycle");
            continue; // Immutable resource identity, configured before native registration.
            break;
        }
        case PlanField::ProjectileSource: {if(!rocket || !std::holds_alternative<EntityReference>(value))return fail("Invalid projectile source metadata");continue;
        }
        case PlanField::ProjectileTrail: {
            const auto* active=std::get_if<int64_t>(&value);if(!rocket || e.definition!=4 || !active)return fail("Invalid adhesive projectile trail presentation");
            const auto trail=mem_read32(object+0x13C);if(*active && !trail)return fail("Retired adhesive projectile trail cannot return to flight");
            if(trail){if(!readable(trail,16))return fail("Invalid adhesive projectile trail storage");if(!*active){add(trail+8,1);add(trail+0xC,mem_read32(0x80200088));add(object+0x13C,0);}}
            continue;
        }
        case PlanField::ProjectileSpin: {const auto* elapsed=std::get_if<float>(&value);if(!rocket || e.definition!=17 || !elapsed)return fail("Invalid spinning projectile presentation");add(object+0x16C,f2u(*elapsed));continue;
        }
        case PlanField::ProjectileVisual: {const auto* v=std::get_if<std::array<float,3>>(&value);if(!rocket || e.definition!=12 || !v)return fail("Invalid power projectile presentation");for(unsigned i=0;i<3;++i)add(object+0x12C+i*4,f2u((*v)[i]));continue;
        }
        case PlanField::EnergyPhase: {const auto* phase=std::get_if<int64_t>(&value);if(!rocket || e.definition!=6 || !phase || (*phase<2 && mem_read32(object+0x148)==2))return fail("Invalid energy projectile phase transition");add(object+0x148,uint32_t(*phase));continue;
        }
        case PlanField::ExtraVisual: {
            const auto* v=std::get_if<std::vector<float>>(&value);const auto offsets=native_projectile_extra_offsets(e.definition);
            if(!rocket || !v || offsets.empty() || v->size()!=offsets.size())return fail("Invalid monster projectile visual layout");
            for(unsigned i=0;i<v->size();++i){if(!std::isfinite((*v)[i]) || std::abs((*v)[i])>1000000.f)return fail("Invalid monster projectile visual coefficient");add(object+offsets[i],f2u((*v)[i]));}continue;
        }
        case PlanField::EnergyVisual: {const auto* v=std::get_if<std::vector<float>>(&value);if(!rocket || e.definition!=6 || !v || v->size()!=native_energy_visual_offsets.size())return fail("Invalid energy projectile presentation layout");for(unsigned i=0;i<v->size();++i)add(object+native_energy_visual_offsets[i],f2u((*v)[i]));continue;
        }
        case PlanField::ProjectileScale: {const auto* v=std::get_if<std::array<float,3>>(&value);if(!rocket || e.definition==0 || !v)return fail("Invalid native grenade scale");for(unsigned i=0;i<3;++i){if(!std::isfinite((*v)[i]) || (*v)[i]<0 || (*v)[i]>100)return fail("Invalid native grenade scale coefficient");add(object+0x68+i*4,f2u((*v)[i]));}continue;
        }
        case PlanField::ProjectileRotation: {const auto* v=std::get_if<std::array<float,3>>(&value);if(!rocket || e.definition==0 || !v)return fail("Invalid native grenade rotation");for(unsigned axis=0;axis<3;++axis){if(!std::isfinite((*v)[axis]) || std::abs((*v)[axis])>10000.f)return fail("Invalid native grenade rotation coefficient");add(object+0x2C+axis*4,f2u((*v)[axis]));}continue;
        }
        case PlanField::Velocity: {
            const auto* v=std::get_if<std::array<float,3>>(&value);if(!rocket || !v)return fail("Invalid projectile velocity");
            for(unsigned axis=0;axis<3;++axis){if(!std::isfinite((*v)[axis]) || std::abs((*v)[axis])>10000.f)return fail("Invalid projectile velocity coefficient");add(object+0x104+axis*4,f2u((*v)[axis]));}
            continue;
        }
        case PlanField::Identity: {continue;
        }
        case PlanField::CameraRows: {
            const auto* matrix=std::get_if<std::array<float,4>>(&value);
            if(!matrix || !matrix_rows)return fail("Invalid replica camera matrix");
            const unsigned row=unsigned(key.back()-'0');
            for(unsigned column=0;column<4;++column) {
                if(!std::isfinite((*matrix)[column]) || std::abs((*matrix)[column])>(column==3?1000000.f:10000.f))return fail("Invalid replica camera matrix coefficient");
                // Both native cached copies are local presentation data.
                add(camera+0x24+row*16+column*4,f2u((*matrix)[column]));
                add(camera+0x54+row*16+column*4,f2u((*matrix)[column]));
            }
            break;
        }
        case PlanField::CameraMode: {
            const auto* mode=std::get_if<int64_t>(&value);
            if(!player || !matrix_rows || !mode || *mode<0 || *mode>4)return fail("Invalid original camera mode");
            add(camera+0xAC,uint32_t(*mode));
            break;
        }
        case PlanField::CameraEffects: {
            const auto* scalar=std::get_if<float>(&value);
            if(!player || !matrix_rows || !scalar || !std::isfinite(*scalar) || std::abs(*scalar)>10000.f)return fail("Invalid replica camera effect");
            add(camera+(key=="camera_bob"?0x84:0x88),f2u(*scalar));
            break;
        }
        case PlanField::ViewAngles: {
            const auto* angles=std::get_if<std::array<float,3>>(&value);
            if(!player || !angles)return fail("Invalid replica view angles");
            for(unsigned axis=0;axis<3;++axis){if(!std::isfinite((*angles)[axis]) || std::abs((*angles)[axis])>10000.f)return fail("Invalid replica view angle");add(object+0x2EC+axis*4,f2u((*angles)[axis]));}
            break;
        }
        case PlanField::Position: {
            const auto* p=std::get_if<std::array<float,3>>(&value);if(!p || !coordinates(*p))return fail("Invalid replica position");
            // Native setter 8009C300 uses +20; 80089358 copies that position
            // to +74 for the following actor update. Other classes have different fields here.
            for(unsigned axis=0;axis<3;++axis){add(object+0x20+axis*4,f2u((*p)[axis]));if(player || e.kind==EntityKind::Enemy)add(object+0x74+axis*4,f2u((*p)[axis]));}
            break;
        }
        case PlanField::Health: {
            const auto* f=std::get_if<float>(&value);if((!player && e.kind!=EntityKind::Enemy) || !f || !std::isfinite(*f) || std::abs(*f)>1000000.f)return fail("Invalid replica health");add(object+0xCC,f2u(*f));
            break;
        }
        case PlanField::Score: {
            const auto* f=std::get_if<float>(&value);if(!player || !f || !std::isfinite(*f) || std::abs(*f)>1000000000.f)return fail("Invalid replica score");add(object+(key=="score"?0x68C:0x6A0),f2u(*f));
            break;
        }
        case PlanField::Armor: {
            const auto* f=std::get_if<float>(&value);if(!player || !f || !std::isfinite(*f) || *f<0 || *f>1000000.f)return fail("Invalid replica armor");add(object+0x390,f2u(*f));
            break;
        }
        case PlanField::PlayerScalar: {
            const auto* f=std::get_if<float>(&value);if(!player || !f || !std::isfinite(*f) || std::abs(*f)>10000.f)return fail("Invalid player scalar");
            add(object+(key=="yaw"?0x290:key=="pitch"?0x294:0x3B4),f2u(*f));
            break;
        }
        case PlanField::Respawn: {
            const auto* n=std::get_if<int64_t>(&value);if(!player || !n || *n<0 || *n>7)return fail("Invalid respawn phase");add(object+0x3AC,uint32_t(*n));
            break;
        }
        case PlanField::WeaponCounts: {
            const auto* v=std::get_if<std::vector<int64_t>>(&value);if(!player || !v || v->size()!=12)return fail("Invalid weapon ownership/model slots");
            const auto offset=key=="weapon_owned_counts"?16u:12u;
            for(unsigned i=0;i<12;++i){
                if((*v)[i]<0 || (*v)[i]>2)return fail("Invalid weapon ownership/model count");
                if(key=="weapon_active_counts")for(unsigned index=0;index<unsigned((*v)[i]);++index) {
                    const auto instance=mem_read32(object+0x4BC+24*i+index*4);
                    if(!readable(instance,0x228) || !readable(mem_read32(instance+0xC),0x160))return fail("Active weapon model is not present locally");
                }
                add(object+0x4BC+24*i+offset,uint32_t((*v)[i]));
            }
            break;
        }
        case PlanField::Ammunition: {
            const auto* v=std::get_if<std::vector<int64_t>>(&value);if(!player || !v || v->size()!=18)return fail("Invalid ammunition slots");
            const auto inventory=mem_read32(object+0x5E8);if(!readable(inventory,0xDC))return fail("Native inventory is unavailable");
            auto offset=key=="ammo"?0u:key=="ammo_secondary"?0x24u:0xB4u;
            for(unsigned i=0;i<18;++i){if((*v)[i]<0 || (*v)[i]>(key=="ammo_maximum"?32767:65535))return fail("Invalid ammunition count");add(inventory+offset+2*i,uint32_t((*v)[i]),2);}
            break;
        }
        case PlanField::PickupIdentity: {
            // Immutable creation identity was validated against the original
            // native configuration before staging any memory writes.
            break;
        }
        case PlanField::PickupMask: {
            const auto* mask=std::get_if<int64_t>(&value);const auto binding=native_render_binding(object,e.kind);
            if(!pickup || !mask || *mask<0 || *mask>UINT32_MAX || !binding)return fail("Invalid pickup view mask");
            if(binding->model)add(binding->model+0x30,uint32_t(*mask));
            break;
        }
        case PlanField::GlowPose: {
            const auto* pose=std::get_if<std::vector<float>>(&value);const auto glow=pickup?native_pickup_glow_binding(object):std::nullopt;
            if(!glow || !pose || (!pose->empty() && !glow->model))return fail("Invalid pickup glow pose/binding id="+std::to_string(e.id)+" definition="+std::to_string(e.definition));
            for(unsigned i=0;i<pose->size();++i){const auto v=(*pose)[i];if(!std::isfinite(v) || std::abs(v)>1000000.f)return fail("Invalid pickup glow coefficient");if(glow->model)writes.push_back({glow->matrix+i*4,f2u(v),4,0,true});}
            break;
        }
        case PlanField::Glow: {
            const auto* state=std::get_if<std::vector<int64_t>>(&value);const auto proxy=pickup?native_pickup_glow_proxy(object):std::nullopt;
            if(!proxy || !state || state->size()!=2 || (*state)[0]<0 || (*state)[0]>1 || (*state)[1]<0 || (*state)[1]>UINT32_MAX)return fail("Invalid pickup glow state");
            if(*proxy){visible_write(*proxy,uint32_t((*state)[0]));add(*proxy+0x30,uint32_t((*state)[1]));}
            break;
        }
        case PlanField::PickupOwnership: {
            const auto* v=std::get_if<std::vector<int64_t>>(&value);if(!pickup || !v || v->size()!=ssne::native_party_capacity())return fail("Invalid pickup ownership slots");
            for(unsigned i=0;i<v->size();++i){if((*v)[i]<0 || (*v)[i]>1)return fail("Invalid pickup ownership value");writes.push_back({i*4,uint32_t((*v)[i]),4,object});}
            break;
        }
        case PlanField::BossParts: {if(!validate_native_boss_parts(e,error))return false;continue;}
        case PlanField::NpcPiece: {if(e.kind!=EntityKind::Physics || e.definition!=5)return fail("Part field targets another class");continue;}
        case PlanField::Rotor: {continue;
        }
        default: return fail("Native property schema is not supported: update adapter before applying it");
        }
    }
    if(!e.properties.contains("position"))return fail("Replica transform is missing");return true;
}
}
namespace {
bool commit_scalar_writes(std::vector<Write>& writes,std::string& error) {
    // No guest mutation until every write and the complete pickup allocation
    // requirement have passed. Read and commit remain in the same batch.
    std::vector<uint32_t> pickups;for(const auto& write:writes)if(write.pickup)pickups.push_back(write.pickup);
    if(!ssne::native_party_can_prepare_pickups(pickups)){error="Native pickup ownership storage is full";return false;}
    for(const auto& write:writes)if(!write.pickup && !readable(write.address,write.size)){error="Replica write leaves active RAM";return false;}
    for(auto& write:writes)if(write.pickup)write.address+=ssne::native_party_pickup_flags(write.pickup);
    for(const auto& write:writes)if(write.size==1)mem_write8(write.address,uint8_t(write.value));else if(write.size==2)mem_write16(write.address,uint16_t(write.value));else mem_write32(write.address,write.value);
    return true;
}
}
void NativeReplica::reset(){end_presentation();bindings_.clear();combo_presented_.clear();combo_timers_.clear();epoch_=0;projectile_visuals_.clear();seen_projectiles_.clear();projectile_visual_sequence_=0;ssne::replica_clear_enemy_poses();}
bool NativeReplica::advance_combo_huds(PPCContext& context,float seconds,std::string& error){
    if(!std::isfinite(seconds) || seconds<0 || seconds>60 || !ssne_find_func(0x800BC008) || mem_read32(0x800BC008)!=0x9421FFF0u || !readable(context.r[1]-0x800,0x800)){error="Invalid combo presentation clock";return false;}
    const auto base=context.r[1]-0x800;std::array<uint8_t,0x800> saved;std::memcpy(saved.data(),ssne::g_ram+(base-0x80000000u),saved.size());
    for(auto& [id,timer]:combo_timers_)if(timer.remaining>=0){
        timer.remaining-=seconds;if(timer.remaining>0)continue;
        const auto hud=mem_read32(timer.actor+0x374);auto call=context;call.msr&=~0x8000u;call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);call.r[3]=hud;
        const auto title=mem_read32(hud+0x58);
        float fade=timer.phase==7?u2f(mem_read32(0x802120F4u)):.5f;
        const auto config=mem_read32(timer.actor+0x458);
        if(timer.phase==2 && readable(config,0x28))fade=u2f(mem_read32(config+0x24));
        call.ps0[1]=call.ps1[1]=std::clamp(std::isfinite(fade)?fade:.5f,.001f,60.f);
        ssne_call(call,mem_read32(title+0xB8)?0x800BBECC:0x800BC008);
        // This is UI expiration only: never enter combo awards/800B5DEC.
        mem_write32(hud+0x60,0);timer.remaining=-1;
    }
    std::memcpy(ssne::g_ram+(base-0x80000000u),saved.data(),saved.size());return true;
}
bool NativeReplica::begin_presentation(const std::map<EntityId,EntityState>& entities,std::string& error){
    return begin_presentation_native(entities,nullptr,error);
}
bool NativeReplica::begin_presentation_with_vehicles(const std::map<EntityId,EntityState>& entities,const Properties& vehicles,std::string& error){
    return begin_presentation_native(entities,&vehicles,error);
}
bool NativeReplica::begin_presentation_native(const std::map<EntityId,EntityState>& entities,const Properties* vehicles,std::string& error){
    if(!presentation_saved_.empty() || !presentation_poses_.empty()){error="Nested client presentation lease";return false;}
    // Bone matrices contain thousands of adjacent words. Avoid one tree node
    // allocation per word in both the write plan and its restoration copy.
    struct VisualWrite {uint32_t address,value;bool replace;};
    std::vector<VisualWrite> plan;plan.reserve(entities.size()*128);
    std::vector<BorrowedPose> poses;
    auto stage=[&](uint32_t address,float value){if(!readable(address,4) || !std::isfinite(value) || std::abs(value)>1000000.f)return false;plan.push_back({address,f2u(value),true});return true;};
    auto matrix=[&](uint32_t address,const Properties& p,const std::string& prefix){if(!p.contains(prefix+"0") && !p.contains(prefix+"1") && !p.contains(prefix+"2"))return true;PresentationMatrix m;if(!presentation_matrix(p,prefix,m) || !readable(address,48))return false;for(unsigned i=0;i<12;++i)if(!stage(address+i*4,m[i]))return false;return true;};
    auto bone=[&](uint32_t object,const auto& binding,const Properties& p,const std::string& prefix,int weapon=-1,unsigned hand=0){
        const auto field=p.find(prefix+"matrices");if(field==p.end())return true;
        const auto* values=std::get_if<std::vector<float>>(&field->second);
        const auto count=p.find(prefix+"count"),layout=p.find(prefix+"layout");
        if(!binding || !binding->model)return true;
        if(!values || count==p.end() || layout==p.end() || std::get<int64_t>(count->second)!=binding->count || uint64_t(std::get<int64_t>(layout->second))!=binding->layout || values->size()!=binding->count*12 || !readable(binding->matrices,unsigned(values->size())*4))return false;
        BorrowedPose pose{object,binding->model,weapon,hand,{},*values};
        pose.original.reserve(values->size());
        for(unsigned i=0;i<values->size();++i){if(!stage(binding->matrices+i*4,(*values)[i]))return false;pose.original.push_back(u2f(mem_read32(binding->matrices+i*4)));}
        // Pose registration invalidates the mesh stamp; it belongs to this
        // lease too, and must not leak into a subsequent authority capture.
        plan.push_back({binding->model+0x84,mem_read32(binding->model+0x84),false});poses.push_back(std::move(pose));return true;
    };
    if(vehicles){
        NativeVehiclePresentation vehicle;
        if(!prepare_native_vehicle_presentation(*vehicles,vehicle,error))return false;
        for(const auto& [address,value]:vehicle.words)if(!stage(address,value)){error="Invalid vehicle presentation word";return false;}
        for(auto& p:vehicle.poses){
            const auto matrices=mem_read32(p.model+0x78);BorrowedPose pose{p.object,p.model,-1,0,{},std::move(p.bones)};
            for(unsigned i=0;i<pose.visual.size();++i)pose.original.push_back(u2f(mem_read32(matrices+i*4)));
            plan.push_back({p.model+0x84,mem_read32(p.model+0x84),false});poses.push_back(std::move(pose));
        }
    }
    {
    NativePoseReadScope pose_reads;
    for(const auto& [id,e]:entities){
        const auto object=local_address(id);if(!object)continue;
        if(e.kind==EntityKind::Pickup && e.properties.contains("pickup_glow_pose")){
            const auto glow=native_pickup_glow_binding(object);const auto* pose=std::get_if<std::vector<float>>(&e.properties.at("pickup_glow_pose"));
            if(!glow || !pose || (pose->size()!=0 && pose->size()!=12) || (!pose->empty() && !glow->model) || (pose->empty() && glow->model && (mem_read8(glow->model+0x2D) || mem_read32(glow->model+0x30)))){error="Client pickup glow presentation changed";return false;}
            if(!pose->empty())for(unsigned i=0;i<12;++i)if(!stage(glow->matrix+i*4,(*pose)[i])){error="Invalid interpolated pickup glow pose";return false;}
        }
        if(e.kind==EntityKind::Enemy && e.definition==20 && e.properties.contains("rotor_row0")){
            const auto child=mem_read32(object+0x24C);const auto rotor=native_render_binding(child,EntityKind::Pickup);
            if(!rotor || !rotor->model || !matrix(rotor->matrix,e.properties,"rotor_row")){error="Client propeller presentation changed";return false;}
        }
        if(e.kind==EntityKind::Player){
            const auto camera=mem_read32(object+0x368);
            if(!readable(camera,0x100) || mem_read32(camera)!=0x80210880u || mem_read32(camera+4)!=object){error="Client presentation camera binding changed";return false;}
            if(!matrix(camera+0x24,e.properties,"camera_row") || !matrix(camera+0x54,e.properties,"camera_row")){error="Invalid client presentation camera matrix";return false;}
        }
        for(unsigned part=0;part<(e.kind==EntityKind::Player?4u:1u);++part){
            const auto binding=native_render_binding(object,e.kind,part);
            if(binding && binding->model && !matrix(binding->matrix,e.properties,part?"body_render"+std::to_string(part)+"_row":"render_row")){error="Invalid client presentation model matrix";return false;}
            if(e.kind==EntityKind::Player){if(!bone(object,native_body_pose_binding(object,part),e.properties,"body"+std::to_string(part)+"_bone_")){error="Client body skeleton changed during presentation";return false;}}
            else if(e.kind==EntityKind::Enemy || (e.kind==EntityKind::Physics && e.definition==3)){
                if(!bone(object,e.kind==EntityKind::Enemy?native_enemy_pose_binding(object):native_corpse_pose_binding(object),e.properties,"enemy_bone_")){error="Client enemy skeleton changed during presentation";return false;}
            }
        }
        if(e.kind==EntityKind::Player && mem_read32(object+0x17C)==unsigned(ssne::netplay_local_player()) && u2f(mem_read32(object+0xCC))>0){
            if(mem_read32(object+0x5DC)==4){
                PresentationMatrix from,to;
                const auto camera=mem_read32(object+0x368);
                for(unsigned i=0;i<12;++i)from[i]=u2f(mem_read32(camera+0x24+i*4));
                if(presentation_matrix(e.properties,"camera_row",to))for(unsigned hand=0;hand<2;++hand){
                    const auto barrel=native_minigun_barrel(object,hand);if(!barrel || !barrel->model)continue;
                    PresentationMatrix pose;for(unsigned i=0;i<12;++i)pose[i]=u2f(mem_read32(barrel->matrix+i*4));
                    pose=matrix_product(matrix_product(to,matrix_rigid_inverse(from)),pose);
                    for(unsigned i=0;i<12;++i)if(!stage(barrel->matrix+i*4,pose[i])){error="Invalid interpolated minigun barrel";return false;}
                }
            }
            const auto weapon=mem_read32(object+0x5DC);if(weapon>=12)continue;
            const auto selected=e.properties.find("weapon_model_slot");if(selected==e.properties.end() || uint32_t(std::get<int64_t>(selected->second))!=weapon)continue;
            PresentationMatrix from,to;
            const auto camera=mem_read32(object+0x368);
            for(unsigned i=0;i<12;++i)from[i]=u2f(mem_read32(camera+0x24+i*4));
            const bool camera_changed=presentation_matrix(e.properties,"camera_row",to);
            // The original uses a separate owner-view streak, generated from
            // the first-person muzzle. Rebase only that one-frame visual from
            // its exact firing camera. World streaks and ballistic objects keep
            // their authoritative trajectories and never enter this path.
            if(camera_changed)for(unsigned i=0;i<std::min(4u,mem_read32(0x802DCE10u));++i){
                const auto proxy=mem_read32(0x802D90CCu+i*4);if(!readable(proxy,0x38))continue;
                const auto reference=ssne::replica_bullet_trail_view(proxy);if(!reference)continue;
                const auto root=mem_read32(proxy+4);PresentationMatrix pose;for(unsigned j=0;j<12;++j)pose[j]=u2f(mem_read32(root+j*4));
                pose=matrix_product(matrix_product(to,matrix_rigid_inverse(*reference)),pose);
                for(unsigned j=0;j<12;++j)if(!stage(root+j*4,pose[j])){error="Invalid first-person bullet streak camera adjustment";return false;}
            }
            for(unsigned hand=0;hand<2;++hand){
                const auto instance=mem_read32(object+0x4BC+weapon*24+hand*4);
                const auto proxy=readable(instance,0x228)?mem_read32(instance):0,vt=readable(proxy,0x38)?mem_read32(proxy):0;
                // Both native flag values0/1 are valid model bindings. Most
                // ordinary visible weapon proxies use0; testing for1 silently
                // left their roots authoritative while the camera was predicted.
                // Borrowing a selected hidden model does not change its flag.
                if(readable(vt,0x30) && mem_read32(vt+0x2C)==0x8013E954u && mem_read8(proxy+0x2D)<=1){
                    if(!matrix(mem_read32(proxy+4),e.properties,"weapon_model"+std::to_string(hand)+"_row") || !bone(object,native_weapon_pose_binding(object,weapon,hand),e.properties,"weapon_model"+std::to_string(hand)+"_bone_",int(weapon),hand)){error="Client first-person weapon presentation changed";return false;}
                }
                // Only first-person flash owners follow this view. Body flashes
                // and ballistic projectiles retain their world-space positions.
                for(unsigned effect=0;camera_changed && effect<3;++effect){
                    const auto off=native_weapon_flash_offset(weapon,effect);if(!off || !readable(instance+off,16) || !mem_read32(instance+off+12))continue;
                    const auto flash_proxy=mem_read32(instance+off+4),flash_root=readable(flash_proxy,8)?mem_read32(flash_proxy+4):0;
                    if(!readable(flash_root,48))continue;
                    PresentationMatrix pose;for(unsigned i=0;i<12;++i)pose[i]=u2f(mem_read32(flash_root+i*4));
                    pose=matrix_product(matrix_product(to,matrix_rigid_inverse(from)),pose);
                    for(unsigned i=0;i<12;++i)if(!stage(flash_root+i*4,pose[i])){error="Invalid first-person flash camera adjustment";return false;}
                }
            }
        }
        if(e.kind==EntityKind::Projectile){
            const auto* type=native_projectile_class(e.definition);const auto position=e.properties.find("position");
            if(!type || position==e.properties.end() || !readable(object,type->size) || mem_read32(object+0x48)!=type->vtable)continue;
            const auto& p=std::get<std::array<float,3>>(position->second);
            if(type->trail){
                const auto trail=mem_read32(object+type->trail),proxy=readable(trail,4)?mem_read32(trail):0;
                const auto vt=readable(proxy,0x38)?mem_read32(proxy):0,root=readable(proxy,8)?mem_read32(proxy+4):0;
                if(readable(vt,0x30) && mem_read32(vt+0x2C)==0x8013E954u && readable(root,48))
                    for(unsigned i=0;i<3;++i)if(!stage(root+12+i*16,u2f(mem_read32(root+12+i*16))+p[i]-u2f(mem_read32(object+0x20+i*4)))){error="Invalid interpolated projectile trail";return false;}
            }
            // Original classes own different proxy offsets. Only verified
            // PAL render proxies are eligible; no factory/setter/trail tick.
            for(unsigned off=0x50;off+4<=type->size;off+=4){
                const auto proxy=mem_read32(object+off),vt=readable(proxy,0x38)?mem_read32(proxy):0;
                if(!readable(vt,0x30) || mem_read32(vt+0x2C)!=0x8013E954u)continue;
                const auto address=mem_read32(proxy+4);if(!readable(address,48))continue;
                for(unsigned i=0;i<3;++i)if(!stage(address+12+i*16,u2f(mem_read32(address+12+i*16))+p[i]-u2f(mem_read32(object+0x20+i*4)))){error="Invalid interpolated projectile matrix";return false;}
            }
        }
    }
    } // All resource-layout reuse ends before guest writes.
    // Keep the old map's last-write semantics, including try_emplace for mesh
    // stamps and aliased projectile proxies. Validate the entire frame before
    // touching guest RAM, then save each unique address exactly once.
    std::stable_sort(plan.begin(),plan.end(),[](const auto& a,const auto& b){return a.address<b.address;});
    size_t unique=0;
    for(size_t first=0;first<plan.size();){
        size_t next=first+1;auto write=plan[first];
        while(next<plan.size() && plan[next].address==write.address){if(plan[next].replace)write=plan[next];++next;}
        plan[unique++]=write;first=next;
    }
    plan.resize(unique);presentation_saved_.reserve(unique);
    for(const auto& write:plan)presentation_saved_.emplace_back(write.address,mem_read32(write.address));
    presentation_poses_=std::move(poses);
    for(const auto& write:plan)mem_write32(write.address,write.value);
    for(const auto& pose:presentation_poses_){const bool ok=pose.weapon<0?ssne::replica_enemy_pose(pose.object,pose.model,pose.visual):ssne::replica_weapon_pose(pose.object,unsigned(pose.weapon),pose.hand,pose.visual);if(!ok){end_presentation();error="Client visual pose registration failed";return false;}}
    return true;
}
void NativeReplica::end_presentation(){
    for(const auto& pose:presentation_poses_){const bool ok=pose.weapon<0?ssne::replica_enemy_pose(pose.object,pose.model,pose.original):ssne::replica_weapon_pose(pose.object,unsigned(pose.weapon),pose.hand,pose.original);if(!ok)ssne::fatal("Client presentation skeleton lifetime changed inside native render");}
    for(const auto& [address,value]:presentation_saved_)mem_write32(address,value);
    presentation_saved_.clear();presentation_poses_.clear();
}
uint32_t NativeReplica::local_address(EntityId id)const{auto it=bindings_.find(id);return it==bindings_.end()?0:local_.local_address(it->second);}
bool NativeReplica::play_projectile_visual(PPCContext& context,uint64_t epoch,const NativeProjectileVisual& event,uint64_t frame,std::string& error){
    if(!validate_native_projectile_visual(event,error))return false;
    if(!epoch || epoch!=epoch_ || !ssne::replica_simulation()){error="Projectile visual requires current replica epoch";return false;}
    if(event.sequence<=projectile_visual_sequence_)return true;
    const auto id=uint64_t(event.tags[0]);
    if(seen_projectiles_.contains(id) || local_address(id)){
        seen_projectiles_.erase(id);projectile_visual_sequence_=event.sequence;
        ssne::log_message("[coop-projectile-visual-known] sequence="+std::to_string(event.sequence)+" id="+std::to_string(id));return true;
    }
    if(projectile_visuals_.size()>=4096 || projectile_visuals_.contains(id)){error="Native projectile visual lifetime capacity/identity exceeded";return false;}
    auto entity=native_projectile_visual_state(event,event.tags[1]==6);
    // A source can die before the reliable lifetime event arrives. Presentation
    // may still use the original resources, with no gameplay owner or damage.
    const auto source=local_address(uint64_t(event.tags[2]));
    const auto object=create_native_projectile(context,entity,source,error);if(!object)return false;
    if(!present_native_projectile(context,object,error)){std::string cleanup;remove_native_entity(context,object,cleanup);return false;}
    const auto local_id=local_.local_entity_id(object);if(!local_id){error="Native projectile visual registration missing";std::string cleanup;remove_native_entity(context,object,cleanup);return false;}
    projectile_visuals_.emplace(id,ProjectileVisual{event,local_id,mem_read32(context.r[13]-0x6D80),event.tags[1]==6});projectile_visual_sequence_=event.sequence;
    ssne::log_message("[coop-projectile-visual-replay] sequence="+std::to_string(event.sequence)+" id="+std::to_string(id)+" definition="+std::to_string(event.tags[1])+" native="+std::to_string(object));return true;
}
bool NativeReplica::advance_projectile_visuals(PPCContext& context,uint64_t frame,std::string& error){
    if(context.r[13]!=0x802E22A0u){error="Invalid projectile visual draw context";return false;}
    const auto draw_frame=mem_read32(context.r[13]-0x6D80);
    for(auto it=projectile_visuals_.begin();it!=projectile_visuals_.end();){
        auto& visual=it->second;const auto object=local_.local_address(visual.local);
        if(!object){it=projectile_visuals_.erase(it);continue;}
        if(draw_frame==visual.frame){++it;continue;}
        if(!visual.final_pose){
            const auto entity=native_projectile_visual_state(visual.event,true);std::vector<Write> writes;
            if(!build_writes(entity,object,writes,error))return false;
            for(const auto& write:writes)mem_write32(write.address,write.value);
            if(!present_native_projectile(context,object,error))return false;
            visual.final_pose=true;visual.frame=draw_frame;++it;continue;
        }
        // Two actual presentation frames establish the original trail between
        // the host's recorded endpoints; retirement uses its native fade path.
        if(!remove_native_entity(context,object,error))return false;
        ssne::log_message("[coop-projectile-visual-retired] id="+std::to_string(it->first)+" native="+std::to_string(object));it=projectile_visuals_.erase(it);
    }
    return true;
}
NativeReplicaResult NativeReplica::apply(PPCContext& context,uint64_t epoch,const std::map<EntityId,EntityState>& source) {
    return apply_native(context,epoch,source,nullptr);
}
NativeReplicaResult NativeReplica::apply_with_vehicles(PPCContext& context,uint64_t epoch,const std::map<EntityId,EntityState>& source,const Properties& mission) {
    return apply_native(context,epoch,source,&mission);
}
NativeReplicaResult NativeReplica::apply_native(PPCContext& context,uint64_t epoch,const std::map<EntityId,EntityState>& source,const Properties* vehicles) {
    ssne::perf::detail::Frame perf_frame(ssne::perf::detail::ReplicaApply);
    ssne::perf::detail::count(ssne::perf::detail::Entities,source.size());
    ssne::perf::detail::Scope perf_phase(ssne::perf::detail::CaptureBindings);
    ssne::perf::Laps timings;
    NativeReplicaResult result;
    // Reject an invalid guest call context before committing even scalar fields.
    if(mem_read32(0x80000000u)!=0x47334250u || context.r[13]!=0x802E22A0u || context.r[1]<0x80000800u || !readable(context.r[1]-0x800u,0x800u)){result.error="Native replica lifecycle context is unavailable";return result;}
    if(!epoch){result.error="Invalid replica epoch";return result;}
    auto local=local_.capture_bindings();if(!local.error.empty()){result.error=local.error;return result;}auto staged=epoch==epoch_?bindings_:std::map<EntityId,EntityId>{};
    timings.mark("capture_bindings");perf_phase.next(ssne::perf::detail::Preflight);
    const bool fresh_epoch=epoch!=epoch_;
    size_t new_projectiles=0;for(const auto& [id,e]:source)if(e.kind==EntityKind::Projectile && (fresh_epoch || !seen_projectiles_.contains(id)))++new_projectiles;
    if((fresh_epoch?0:seen_projectiles_.size())+new_projectiles>32768){result.error="Native projectile seen-lifetime capacity exceeded";return result;}
    std::set<EntityId> visual_only;if(!fresh_epoch)for(const auto& [id,visual]:projectile_visuals_)visual_only.insert(visual.local);
    std::vector<uint32_t> native_attached_parts;
    for(const auto&[id,e]:local.entities)if(e.kind==EntityKind::Physics && e.definition==5){const auto object=local_.local_address(id);if(readable(object,0x130) && mem_read32(object+0xC8)){
        const auto binding=native_render_binding(object,e.kind);if(!binding || !binding->model){result.error="Original NPC part proxy unavailable";return result;}native_attached_parts.push_back(object);visual_only.insert(id);
    }}
    std::vector<std::pair<uint32_t,EntityKind>> removals;std::vector<EntityId> creates;std::set<EntityId> used;
    auto validate_removal=[&](uint32_t object,EntityKind kind){
        if(!validate_native_removal(context,object,result.error))return false;
        if((kind==EntityKind::Enemy || kind==EntityKind::Pickup) && !native_render_binding(object,kind)){result.error="Removal has no verified render binding";return false;}
        if(kind==EntityKind::Pickup && !native_pickup_glow_proxy(object)){result.error="Removal has no verified pickup glow";return false;}
        return true;
    };
    // Verify the complete schema and every existing lifetime before calling any
    // native allocator/destructor. Invalid network batches cannot spawn actors.
    for(auto it=staged.begin();it!=staged.end();) {
        if(source.contains(it->first)){++it;continue;}
        auto object=local_.local_address(it->second);
        if(!object || !local.entities.contains(it->second) || local.entities.at(it->second).kind==EntityKind::Player){result.error="Player/stale removal needs native session lifecycle handling";return result;}
        const auto kind=local.entities.at(it->second).kind;if(!validate_removal(object,kind))return result;
        used.insert(it->second);
        removals.emplace_back(object,kind);it=staged.erase(it);
    }
    std::vector<Write> prepared_writes;prepared_writes.reserve(source.size()*32);
    {
    NativePoseReadScope pose_reads;
    for(const auto& [id,e]:source) {
        if(!id || id!=e.id){result.error="Invalid server entity identity";return result;}
        EntityId match=0;
        if(auto found=staged.find(id);found!=staged.end())match=found->second;
        else for(const auto& [candidate_id,candidate]:local.entities) {
            ssne::perf::detail::count(ssne::perf::detail::BindingCandidates);
            if(used.contains(candidate_id) || visual_only.contains(candidate_id) || candidate.kind!=e.kind || candidate.definition!=e.definition)continue;
            if(!(e.kind==EntityKind::Player?same_player(e,candidate):same_spawn(e,candidate)))continue;
            if(match){
                // Coincident unindexed authored pickups have no script target
                // identity. Pair their otherwise identical spawn keys in
                // original creation order, then retain the lifetime binding.
                if(e.kind==EntityKind::Pickup && !mem_read32(local_.local_address(match)+0x50) && !mem_read32(local_.local_address(candidate_id)+0x50))continue;
                result.error="Ambiguous native replica binding: kind="+std::to_string(unsigned(e.kind))+" definition="+std::to_string(e.definition);return result;
            }match=candidate_id;
        }
        if(!match) {
            if(e.kind==EntityKind::Projectile){if(!validate_native_projectile_creation(e,result.error))return result;}
            else if(e.kind==EntityKind::Pickup){if(!validate_native_weapon_pickup_creation(e,result.error)){result.error+=" id="+std::to_string(e.id)+" definition="+std::to_string(e.definition);return result;}}
            else if(e.kind==EntityKind::Physics && e.definition==3){if(!validate_native_corpse_creation(e,result.error))return result;}
            else if(e.kind==EntityKind::Physics && e.definition==4){if(!validate_native_detached_rotor(e,result.error))return result;}
            else if(e.kind==EntityKind::Physics && e.definition==5){if(!validate_native_npc_piece(e,result.error))return result;}
            else if(e.kind==EntityKind::Enemy){if(!validate_native_enemy_creation(e,result.error))return result;}
            else {result.error=missing_binding(e,local);return result;}
            creates.push_back(id);continue;
        }
        if(!local.entities.contains(match)){result.error="Replica binding expired";return result;}
        const auto& candidate=local.entities.at(match);
        if(!local_.local_address(match) || !used.insert(match).second || candidate.kind!=e.kind || candidate.definition!=e.definition || (e.kind==EntityKind::Player && !same_player(e,candidate))){result.error="Invalid/duplicate native lifetime binding";return result;}
        if(e.properties.contains("weapon_modes") && !e.properties.contains("selected_weapon")){result.error="Weapon modes have no selected weapon";return result;}
        const auto first_write=prepared_writes.size();
        if(!build_writes(e,local_.local_address(match),prepared_writes,result.error,true,true))return result;
        // Player resources/equip are prepared later. Keep only the scalar
        // writes from this complete preflight; no borrowed model write may
        // be committed before its native resource lifecycle.
        if(e.kind==EntityKind::Player)prepared_writes.erase(std::remove_if(prepared_writes.begin()+first_write,prepared_writes.end(),[](const Write& write){return write.presentation;}),prepared_writes.end());
        staged[id]=match;
    }
    } // Native lifecycle work below cannot see cached resource layouts.
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Projectile) {
        if(!validate_native_projectile_creation(e,result.error))return result;
        const auto reference=std::get<EntityReference>(e.properties.at("projectile_source")).id;
        if(reference && (!source.contains(reference) || (source.at(reference).kind!=EntityKind::Player && source.at(reference).kind!=EntityKind::Enemy))){result.error="Rocket source is absent or unsupported";return result;}
    }
    // Sources must exist before their projectiles are initialized.
    // A late join loads the original level, including objects already removed
    // by the host before the first snapshot. They have no previous binding, so
    // the normal delta-removal loop above cannot discover them.
    for(const auto& [id,e]:local.entities)if(!used.contains(id) && !visual_only.contains(id) && (e.kind==EntityKind::Pickup || e.kind==EntityKind::Enemy || e.kind==EntityKind::Projectile || (e.kind==EntityKind::Physics && (e.definition==3 || e.definition==4 || e.definition==5)))){
        const auto object=local_.local_address(id);if(!validate_removal(object,e.kind))return result;
        removals.emplace_back(object,e.kind);
    }
    std::stable_sort(creates.begin(),creates.end(),[&](auto a,auto b){return source.at(a).kind!=EntityKind::Projectile && source.at(b).kind==EntityKind::Projectile;});
    struct MarkerState{uint32_t object;bool active;std::vector<float> animation;};
    std::vector<MarkerState> markers;
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Physics && (e.definition==1 || e.definition==2)){
        auto a=e.properties.find("marker_active"),i=e.properties.find("marker_index");
        if(!staged.contains(id) || a==e.properties.end() || i==e.properties.end() || !std::holds_alternative<int64_t>(a->second) || !std::holds_alternative<int64_t>(i->second)){result.error="Incomplete authoritative marker";return result;}
        const auto object=local_.local_address(staged.at(id));
        if(!validate_native_marker(object,e.definition,std::get<int64_t>(i->second),std::get<int64_t>(a->second),result.error))return result;
        std::vector<float> animation;
        if(e.definition==2){
            const auto it=e.properties.find("marker_animation");
            const auto* values=it==e.properties.end()?nullptr:std::get_if<std::vector<float>>(&it->second);
            if(!values || !validate_native_arrow_animation(object,*values,result.error)){if(result.error.empty())result.error="Arrow animation is missing";return result;}
            animation=*values;
        }
        markers.push_back({object,bool(std::get<int64_t>(a->second)),std::move(animation)});
    }
    struct BrushState {uint32_t object;int64_t resource,segment;float progress;};
    std::vector<BrushState> brushes;
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Door) {
        auto r=e.properties.find("brush_resource"),s=e.properties.find("brush_segment"),p=e.properties.find("brush_progress");
        if(!staged.contains(id) || r==e.properties.end() || s==e.properties.end() || p==e.properties.end() || !std::holds_alternative<int64_t>(r->second) || !std::holds_alternative<int64_t>(s->second) || !std::holds_alternative<float>(p->second)){result.error="Incomplete native moving-brush command";return result;}
        auto object=local_.local_address(staged.at(id));auto resource=std::get<int64_t>(r->second),segment=std::get<int64_t>(s->second);auto progress=std::get<float>(p->second);
        if(!validate_native_brush_geometry(object,resource,segment,progress,result.error))return result;
        brushes.push_back({object,resource,segment,progress});
    }
    std::vector<std::pair<uint32_t,unsigned>> movement;
    for(const auto& [id,e]:source)if(auto value=e.properties.find("movement_mode");value!=e.properties.end()) {
        if(e.kind!=EntityKind::Player || !staged.contains(id)){result.error="Movement command has no native player";return result;}
        const auto* mode=std::get_if<int64_t>(&value->second);auto actor=local_.local_address(staged.at(id));
        if(!mode || *mode<0 || *mode>3 || !validate_native_movement_mode(actor,unsigned(*mode),result.error))return result;
        movement.emplace_back(actor,unsigned(*mode));
    }
    std::vector<std::pair<uint32_t,bool>> camera;
    for(const auto& [id,e]:source)if(auto value=e.properties.find("camera_frozen");value!=e.properties.end()) {
        auto frozen=std::get_if<int64_t>(&value->second);
        if(e.kind!=EntityKind::Player || !staged.contains(id) || !frozen || (*frozen!=0 && *frozen!=1)){result.error="Invalid native camera command";return result;}
        camera.emplace_back(local_.local_address(staged.at(id)),*frozen!=0);
    }
    struct WeaponSelection{uint32_t actor;unsigned weapon;bool available;std::vector<int64_t> modes;};
    std::vector<WeaponSelection> weapon_selection;
    for(const auto& [id,e]:source)if(auto value=e.properties.find("selected_weapon");value!=e.properties.end()) {
        const auto* selected=std::get_if<int64_t>(&value->second);
        if(auto pose=e.properties.find("weapon_model_slot");pose!=e.properties.end())if(!selected || !std::holds_alternative<int64_t>(pose->second) || std::get<int64_t>(pose->second)!=*selected){result.error="Authoritative weapon pose/selection mismatch";return result;}
        const auto ownership=e.properties.find("weapon_owned_counts");
        const auto* owned=ownership==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&ownership->second);
        if(e.kind!=EntityKind::Player || !staged.contains(id) || !selected || *selected<0 || *selected>=12 || !owned || owned->size()!=12){result.error="Authoritative selected weapon is invalid or not owned";return result;}
        auto actor=local_.local_address(staged.at(id));
        const auto availability=e.properties.find("weapon_available");
        const auto* available=availability==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&availability->second);
        if(!available || available->size()!=12){result.error="Authoritative weapon availability is missing";return result;}
        for(unsigned slot=0;slot<12;++slot)if(((*available)[slot]!=0 && (*available)[slot]!=1) || ((*owned)[slot]>0 && !(*available)[slot])){result.error="Authoritative weapon availability is inconsistent";return result;}
        if(!(*available)[*selected] && mem_read32(actor+0x5DC)!=uint32_t(*selected)){result.error="Authoritative weapon change selects an unavailable weapon";return result;}
        if(!validate_native_weapon_selection(actor,unsigned(*selected),result.error))return result;
        const auto mode_property=e.properties.find("weapon_modes");
        const auto* modes=mode_property==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&mode_property->second);
        if(!modes){result.error="Authoritative weapon modes are missing or malformed";return result;}
        if(!validate_native_weapon_modes(actor,unsigned(*selected),*modes,result.error))return result;
        weapon_selection.push_back({actor,unsigned(*selected),bool((*available)[*selected]),*modes});
    }
    Properties presentation_rules;
    for(const auto& [id,e]:source)if(auto found=e.properties.find("weapons_hidden");found!=e.properties.end()) {
        auto [entry,fresh]=presentation_rules.emplace("PuzzleConfig.HideWeapons",found->second);
        if(!fresh && entry->second!=found->second){result.error="Players disagree on authoritative weapon visibility";return result;}
    }
    if(!presentation_rules.empty() && !validate_native_configuration(context,native_global_configuration(),presentation_rules,"PuzzleConfig.HideWeapons",result.error))return result;
    if(vehicles && !validate_native_vehicles(*vehicles,source,result.error))return result;
    timings.mark("preflight");perf_phase.next(ssne::perf::detail::Creation);
    auto previous=bindings_;auto previous_epoch=epoch_;std::vector<uint32_t> created;
    auto rollback=[&](){for(auto p:created){std::string why;if(!remove_native_entity(context,p,why))ssne::fatal(why.c_str());}bindings_=previous;epoch_=previous_epoch;};
    for(auto id:creates) {
        const auto& entity=source.at(id);uint32_t object=0;
        if(entity.kind==EntityKind::Projectile){const auto reference=std::get<EntityReference>(entity.properties.at("projectile_source")).id;const auto source_object=reference && staged.contains(reference)?local_.local_address(staged.at(reference)):0;if(reference && !source_object){result.error="Rocket source has no local binding";rollback();return result;}object=create_native_projectile(context,entity,source_object,result.error);}
        else if(entity.kind==EntityKind::Pickup)object=create_native_weapon_pickup(context,entity,result.error);
        else if(entity.kind==EntityKind::Physics && entity.definition==3)object=create_native_corpse(context,entity,result.error);
        else if(entity.kind==EntityKind::Physics && entity.definition==4)object=create_native_detached_rotor(context,entity,result.error);
        else if(entity.kind==EntityKind::Physics && entity.definition==5)object=create_native_npc_piece(context,entity,result.error);
        else object=create_native_enemy(context,entity,result.error);
        if(!object){rollback();return result;}created.push_back(object);
        auto refreshed=local_.capture_bindings();EntityId native_id=0;
        for(const auto&[part_id,state]:refreshed.entities)if(state.kind==EntityKind::Physics && state.definition==5){const auto part=local_.local_address(part_id);if(mem_read32(part+0xC8)){const auto binding=native_render_binding(part,state.kind);if(binding && binding->model && std::find(native_attached_parts.begin(),native_attached_parts.end(),part)==native_attached_parts.end())native_attached_parts.push_back(part);}}
        for(const auto& [candidate,e]:refreshed.entities)if(local_.local_address(candidate)==object){native_id=candidate;break;}
        if(!native_id){result.error="Created native actor was not registered";rollback();return result;}
        staged[id]=native_id;
        ssne::log_message("[coop-replica-create] id="+std::to_string(id)+" definition="+std::to_string(source.at(id).definition)+" native="+std::to_string(object));
    }
    // Factory-created original attachments stay alive for the parent's native
    // render callback, but only their independent received proxies may draw.
    for(auto part:native_attached_parts)if(!ssne::replica_hide_npc_part(part))ssne::fatal("Original NPC part render suppression binding changed");
    timings.mark("creation");perf_phase.next(ssne::perf::detail::Scalars);
    bindings_=std::move(staged);epoch_=epoch;
    // Build the scalar batch directly; copying the complete world a second
    // time also duplicated every enemy skeleton before removing player poses.
    const bool reuse_preflight=creates.empty() && removals.empty();
    if(reuse_preflight) {
        // The complete preflight already checked every identity, lifetime,
        // class and value. No native allocator/destructor has intervened.
        // Commit its contiguous plan without rebuilding identity-only maps,
        // rediscovering bindings, or copying each entity's writes again.
        result.applied=commit_scalar_writes(prepared_writes,result.error);
        result.objects=unsigned(source.size());result.writes=unsigned(prepared_writes.size());
    } else {
        // Native creation can replace resource storage. Rebuild against the
        // current bindings exactly as before, including all validation.
        std::map<EntityId,EntityState> scalar_fields;
        for(const auto& [id,e]:source)scalar_fields.emplace_hint(scalar_fields.end(),id,filtered_fields(e,[&](const std::string& key){
            return !lifecycle_property(key) && key!="marker_active" && key!="powerups" && !key.starts_with("weapon_model") && !key.starts_with("weapon_bomb") &&
                !(e.kind==EntityKind::Player && (render_field(key) || key.starts_with("body_flash") || key.starts_with("body_render") || key.starts_with("body0_bone_") || key.starts_with("body1_bone_") || key.starts_with("body2_bone_") || key.starts_with("body3_bone_")));
        }));
        result=apply_scalars(epoch,scalar_fields);
    }
    if(!result.applied){rollback();return result;}
    timings.mark("scalars");perf_phase.next(ssne::perf::detail::Destructibles);
    if(fresh_epoch){projectile_visuals_.clear();seen_projectiles_.clear();projectile_visual_sequence_=0;}
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Destructible)if(!apply_native_destructible(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    for(const auto& [id,e]:source)if(e.properties.contains("hint_active"))if(!apply_native_interaction_hint(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    for(const auto& [id,e]:source)if(e.properties.contains("pickup_notice_sequence"))if(!apply_native_player_notices(context,epoch,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    timings.mark("destructibles_hints");perf_phase.next(ssne::perf::detail::Hud);
    if(fresh_epoch){combo_presented_.clear();combo_timers_.clear();}
    for(auto it=combo_presented_.begin();it!=combo_presented_.end();)if(!source.contains(it->first))it=combo_presented_.erase(it);else ++it;
    for(auto it=combo_timers_.begin();it!=combo_timers_.end();)if(!source.contains(it->first))it=combo_timers_.erase(it);else ++it;
    for(const auto& [id,e]:source)if(e.properties.contains("combo_hud")){
        if(e.properties.contains("combo_phase") && e.properties.contains("combo_timer")){
            const auto phase=std::get<int64_t>(e.properties.at("combo_phase"));
            const auto& hud=std::get<std::vector<float>>(e.properties.at("combo_hud"));
            // The award number outlives the gameplay combo (800B5EF4 resets
            // phase to zero). Remote HUDs need not be ticked by the host.
            // Use the widget's own lifetime in that phase, never an idle zero
            // timer or a repeatedly received elapsed value as a fresh start.
            // Phase6 has no countdown: it tallies kills/points until the host
            // enters phase7. Expiring a zero phase6 timer discarded both the
            // kill-count caption and its following final score.
            const bool expires=phase==2 || phase==7 || (phase==0 && hud[0]);
            const auto time=expires?(phase?std::get<float>(e.properties.at("combo_timer")):std::max(0.f,hud[2]-hud[1])):-1.f;
            bool new_number=false;
            if(auto old=combo_presented_.find(id);old!=combo_presented_.end()){
                const auto& prior=std::get<std::vector<float>>(old->second.at("combo_hud"));
                new_number=hud[0] && (!prior[0] || hud[3]!=prior[3] || hud[1]+.001f<prior[1]);
            }
            auto& timer=combo_timers_[id];
            if(timer.actor!=local_address(id) || timer.phase!=phase || new_number || time>timer.received+.001f)timer={local_address(id),phase,time,expires?std::max(0.f,time):-1.f};
            else {timer.received=time;if(timer.remaining>=0)timer.remaining=std::min(timer.remaining,std::max(0.f,time));}
        }
        Properties received;for(const auto key:{"combo_hud","combo_banner","combo_caption","combo_caption_style"})if(e.properties.contains(key))received[key]=e.properties.at(key);
        // A repeated authority sample must not restart an expired local HUD
        // animation, particularly when its remote HUD is no longer ticking.
        if((!combo_timers_.contains(id) || combo_timers_.at(id).received<0 || combo_timers_.at(id).remaining>=0) && (!combo_presented_.contains(id) || combo_presented_.at(id)!=received))
            if(!apply_native_combo_hud(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
        combo_presented_[id]=std::move(received);
    }
    for(const auto& [id,e]:source)if(e.properties.contains("reentry_hud"))if(!apply_native_reentry_hud(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    for(const auto& [id,e]:source)if(e.properties.contains("powerups"))if(!apply_native_powerups(context,local_address(id),std::get<std::vector<float>>(e.properties.at("powerups")),result.error))ssne::fatal(result.error.c_str());
    timings.mark("hud");perf_phase.next(ssne::perf::detail::Markers);
    for(const auto& marker:markers){
        if(!apply_native_marker(context,marker.object,marker.active,result.error))ssne::fatal(result.error.c_str());
        if(!marker.animation.empty() && !apply_native_arrow_animation(context,marker.object,marker.animation,result.error))ssne::fatal(result.error.c_str());
    }
    for(auto brush:brushes)if(!apply_native_brush_geometry(context,brush.object,brush.resource,brush.segment,brush.progress,result.error))ssne::fatal(result.error.c_str());
    timings.mark("markers_brushes");perf_phase.next(ssne::perf::detail::PlayerResources);
    // Vehicle entry must bind +6C4 before mode3 initializes its controller.
    // Commit only after the entire entity AND vehicle preflight succeeded.
    if(vehicles && (!prepare_native_vehicle_models(context,*vehicles,result.error) || !apply_native_vehicle_poses(*vehicles,result.error) || !apply_native_vehicle_drivers(context,*vehicles,source,result.error)))ssne::fatal(result.error.c_str());
    for(auto [actor,frozen]:camera)if(!apply_native_camera_control(context,actor,frozen,result.error))ssne::fatal(result.error.c_str());
    for(auto [actor,mode]:movement)if(!apply_native_movement_mode(context,actor,mode,result.error))ssne::fatal(result.error.c_str());
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player)if(!prepare_native_player_body(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    if(!presentation_rules.empty() && !write_native_configuration(context,native_global_configuration(),presentation_rules,"PuzzleConfig.HideWeapons",result.error))ssne::fatal(result.error.c_str());
    for(const auto& selection:weapon_selection){
        if(!apply_native_weapon_selection(context,selection.actor,selection.weapon,selection.available,result.error) ||
           !apply_native_weapon_modes(context,selection.actor,selection.weapon,selection.modes,result.error))ssne::fatal(result.error.c_str());
    }
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player)if(!apply_native_body_weapon_resource(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player && e.properties.contains("weapon_bar"))if(!apply_native_weapon_bar(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player && e.properties.contains("weapon_sidebar"))if(!apply_native_weapon_sidebar(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    timings.mark("player_resources");perf_phase.next(ssne::perf::detail::WeaponEffects);
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player && e.properties.contains("weapon_model_slot")) {
        const auto weapon=unsigned(std::get<int64_t>(e.properties.at("weapon_model_slot")));
        const auto& visibility=std::get<std::vector<int64_t>>(e.properties.at("weapon_model_visibility"));
        // A hidden model can still carry a received pose (holstering, death,
        // respawn). Resource lifetime is separate from draw visibility.
        for(unsigned index=0;index<2;++index)if(visibility[index]>=0 && !prepare_native_weapon_model(context,local_address(id),weapon,index,result.error))ssne::fatal(result.error.c_str());
        // Only presentation writes are consumed here, after resource/equip
        // preparation. Gameplay/HUD fields already passed full preflight and
        // scalar commit; copying/revalidating them cannot add a pose write.
        auto pose_fields=filtered_fields(e,[](const std::string& key){return key=="position" || render_field(key) || key.starts_with("body_render") ||
            key.starts_with("body0_bone_") || key.starts_with("body1_bone_") || key.starts_with("body2_bone_") || key.starts_with("body3_bone_") ||
            (key.starts_with("weapon_model") && !key.starts_with("weapon_model0_flash") && !key.starts_with("weapon_model1_flash"));});
        std::vector<Write> plan;if(!build_writes(pose_fields,local_address(id),plan,result.error))ssne::fatal(result.error.c_str());
        for(const auto& write:plan)if(write.presentation){if(write.size==1)mem_write8(write.address,uint8_t(write.value));else mem_write32(write.address,write.value);}
        for(unsigned hand=0;hand<2;++hand)for(unsigned effect=0;effect<3;++effect){const auto prefix=native_weapon_flash_key(hand,effect);if(e.properties.contains(prefix))if(!apply_native_weapon_flash(context,local_address(id),weapon,hand,std::get<std::vector<float>>(e.properties.at(prefix)),uint32_t(std::get<int64_t>(e.properties.at(prefix+"_mask"))),result.error,effect))ssne::fatal(result.error.c_str());}
        for(unsigned hand=0;hand<2;++hand){const auto prefix="body_flash"+std::to_string(hand);if(e.properties.contains(prefix))if(!apply_native_body_flash(context,local_address(id),hand,std::get<std::vector<float>>(e.properties.at(prefix)),uint32_t(std::get<int64_t>(e.properties.at(prefix+"_mask"))),result.error))ssne::fatal(result.error.c_str());}
        for(unsigned hand=0;hand<2;++hand){const auto key="weapon_bomb"+std::to_string(hand);
            if(weapon==11 && e.properties.contains(key)){const auto& masks=std::get<std::vector<int64_t>>(e.properties.at(key+"_masks"));if(!ssne::apply_native_bomb_visual(context,local_address(id),hand,std::get<std::vector<float>>(e.properties.at(key)),{uint32_t(masks[0]),uint32_t(masks[1])},result.error))ssne::fatal(result.error.c_str());}
            else if(weapon!=11 && !ssne::apply_native_bomb_visual(context,local_address(id),hand,std::vector<float>(32,0.f),{},result.error))ssne::fatal(result.error.c_str());
        }
        // A switch removes the old gun's effect even when its final inactive
        // snapshot was superseded by the newly selected weapon.
        for(unsigned old:{1u,2u,3u,4u,8u,9u})if(old!=weapon)for(unsigned effect=0;effect<3;++effect)if(native_weapon_flash_offset(old,effect))for(unsigned hand=0;hand<2;++hand)
            if(!apply_native_weapon_flash(context,local_address(id),old,hand,std::vector<float>(14,0.f),0,result.error,effect))ssne::fatal(result.error.c_str());
        // Native equip may temporarily select both models even when the host
        // currently presents one pistol. Restore the validated active counts
        // after that lifecycle call, alongside the authoritative model pose.
        const auto& active=std::get<std::vector<int64_t>>(e.properties.at("weapon_active_counts"));
        for(unsigned slot=0;slot<active.size();++slot)mem_write32(local_address(id)+0x4C8+24*slot,uint32_t(active[slot]));
    }
    timings.mark("weapon_models_effects");perf_phase.next(ssne::perf::detail::Skeletons);
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Physics && e.definition==3)if(!apply_native_corpse_lifecycle(context,local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    for(const auto& [id,e]:source)if(e.properties.contains("boss_weakpoints"))if(!apply_native_boss_parts(local_address(id),e,result.error))ssne::fatal(result.error.c_str());
    ssne::replica_clear_enemy_poses();
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player)for(unsigned part=0;part<4;++part)if(e.properties.contains("body"+std::to_string(part)+"_bone_matrices")){
        const auto object=local_address(id);const auto binding=native_body_pose_binding(object,part);
        if(!binding || !ssne::replica_enemy_pose(object,binding->model,std::get<std::vector<float>>(e.properties.at("body"+std::to_string(part)+"_bone_matrices"))))ssne::fatal("Native player body presentation hooks unavailable");
    }
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player && e.properties.contains("weapon_model_slot")){
        const auto weapon=unsigned(std::get<int64_t>(e.properties.at("weapon_model_slot")));
        for(unsigned index=0;index<2;++index){const auto key="weapon_model"+std::to_string(index)+"_bone_matrices";
            if(e.properties.contains(key) && !ssne::replica_weapon_pose(local_address(id),weapon,index,std::get<std::vector<float>>(e.properties.at(key))))ssne::fatal("Native weapon presentation hooks unavailable");
        }
    }
    for(const auto& [id,e]:source)if((e.kind==EntityKind::Enemy || (e.kind==EntityKind::Physics && e.definition==3)) && e.properties.contains("enemy_bone_matrices")) {
        const auto object=local_address(id);const auto binding=e.kind==EntityKind::Physics?native_corpse_pose_binding(object):native_enemy_pose_binding(object);
        if(!binding || !ssne::replica_enemy_pose(object,binding->model,std::get<std::vector<float>>(e.properties.at("enemy_bone_matrices"))))ssne::fatal("Native enemy presentation hooks unavailable");
    }
    timings.mark("skeletons");perf_phase.next(ssne::perf::detail::Retirement);
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Projectile) {
        const auto object=local_address(id);
        if(!present_native_projectile(context,object,result.error))ssne::fatal(result.error.c_str());
        seen_projectiles_.insert(id);
        if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")) {
            const auto& position=std::get<std::array<float,3>>(e.properties.at("position"));const auto& velocity=std::get<std::array<float,3>>(e.properties.at("velocity"));
            unsigned mismatches=0;for(unsigned i=0;i<3;++i){mismatches+=mem_read32(object+0x20+i*4)!=f2u(position[i]);mismatches+=mem_read32(object+0x104+i*4)!=f2u(velocity[i]);}
            const auto reference=std::get<EntityReference>(e.properties.at("projectile_source")).id;
            mismatches+=mem_read32(object+0xC8)!=(reference?local_address(reference):0);
            if(e.definition!=0){const auto& rotation=std::get<std::array<float,3>>(e.properties.at("projectile_rotation"));for(unsigned i=0;i<3;++i)mismatches+=mem_read32(object+0x2C+i*4)!=f2u(rotation[i]);const auto& scale=std::get<std::array<float,3>>(e.properties.at("projectile_scale"));for(unsigned i=0;i<3;++i)mismatches+=mem_read32(object+0x68+i*4)!=f2u(scale[i]);}
            ssne::log_message(std::string((e.definition==1 || e.definition==2)?"[coop-replica-special-rocket-state] id=":(e.definition==3 || e.definition==5)?"[coop-replica-grenade-state] id=":"[coop-replica-rocket-state] id=")+std::to_string(id)+" source="+std::to_string(reference)+" mismatches="+std::to_string(mismatches)+" z="+std::to_string(position[2])+" x="+std::to_string(position[0])+" y="+std::to_string(position[1]));
        }
    }
    if(getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES")){static std::map<EntityId,uint32_t> health_seen;for(const auto& [id,entity]:source)if(entity.kind==EntityKind::Enemy && entity.properties.contains("health")){const auto object=local_address(id),expected=f2u(std::get<float>(entity.properties.at("health"))),actual=mem_read32(object+0xCC);if(!health_seen.contains(id) || health_seen[id]!=actual){health_seen[id]=actual;ssne::log_message("[coop-client-health-observed] id="+std::to_string(id)+" definition="+std::to_string(entity.definition)+" expected="+std::to_string(u2f(expected))+" actual="+std::to_string(u2f(actual))+" mismatches="+std::to_string(expected!=actual));}}}
    for(const auto& [id,binding]:previous)if(!source.contains(id) && local.entities.contains(binding) && local.entities.at(binding).kind==EntityKind::Enemy && getenv("SSNE_TEST_HIDDEN") && getenv("SSNE_COOP_TRACE_LIFETIMES"))ssne::log_message("[coop-client-enemy-retired] id="+std::to_string(id)+" native="+std::to_string(local_.local_address(binding)));
    // The original holster tick is suppressed on replicas. Its first-person
    // render proxies have independent lifetimes and otherwise keep drawing
    // the old pistol/arms after the selected weapon has changed.
    for(const auto& [id,e]:source)if(e.kind==EntityKind::Player && e.properties.contains("selected_weapon")){
        const auto actor=local_address(id),selected=unsigned(std::get<int64_t>(e.properties.at("selected_weapon")));
        const bool remote=mem_read32(actor+0x17C)!=unsigned(ssne::netplay_local_player());
        const bool dead=!(u2f(mem_read32(actor+0xCC))>0);
        // The remote character has separate third-person held-weapon models.
        // Its selected first-person arms must never draw in our local view.
        for(unsigned weapon=0;weapon<12;++weapon)if(remote || dead || weapon!=selected)for(unsigned hand=0;hand<2;++hand){
            const auto instance=mem_read32(actor+0x4BC+weapon*24+hand*4);if(!readable(instance,0x228))continue;
            const auto proxy=mem_read32(instance),table=readable(proxy,0x40)?mem_read32(proxy):0;
            // 8013D9C8 intersects +30 with the current view mask. +2D is
            // spatial-culling policy; setting it to0 does NOT hide a model.
            if(readable(table,0x30) && mem_read32(table+0x2C)==0x8013E954u)mem_write32(proxy+0x30,0);
        }
    }
    timings.mark("projectiles_retirement");
    for(auto [object,kind]:removals){if(!remove_native_entity(context,object,result.error,kind))ssne::fatal(result.error.c_str());ssne::log_message("[coop-replica-remove] native="+std::to_string(object));}
    if(vehicles && (!apply_native_vehicle_poses(*vehicles,result.error) || !update_native_vehicle_huds(context,*vehicles,result.error)))ssne::fatal(result.error.c_str());
    return result;
}
NativeReplicaResult NativeReplica::apply(uint64_t epoch,const std::map<EntityId,EntityState>& source) {
    return apply_scalars(epoch,source);
}
NativeReplicaResult NativeReplica::apply_scalars(uint64_t epoch,const std::map<EntityId,EntityState>& source) {
    NativeReplicaResult result;if(!epoch){result.error="Invalid replica epoch";return result;}
    for(const auto& [id,e]:source)if(auto flag=e.properties.find("camera_frozen");flag!=e.properties.end()) {
        auto frozen=std::get_if<int64_t>(&flag->second);auto actor=local_address(id);
        if(!frozen || (*frozen!=0 && *frozen!=1) || (actor && mem_read32(actor+0x370)!=uint32_t(*frozen))){result.error="Camera change requires native lifecycle context";return result;}
    }
    for(const auto& [id,e]:source)if(auto mode=e.properties.find("movement_mode");mode!=e.properties.end()) {
        auto value=std::get_if<int64_t>(&mode->second);auto actor=local_address(id);
        if(!value || *value<0 || *value>3 || (actor && mem_read32(actor+0x340)!=uint32_t(*value))){result.error="Movement change requires native lifecycle context";return result;}
    }
    const auto local=local_.capture_bindings();
    if(!local.error.empty()){result.error=local.error;return result;}auto next=epoch==epoch_?bindings_:std::map<EntityId,EntityId>{};
    for(const auto& [id,binding]:next)if(!source.contains(id)){result.error="Native removal requires lifecycle handling";return result;}
    std::set<EntityId> used;std::vector<Write> writes;
    {
    NativePoseReadScope pose_reads;
    for(const auto& [id,entity]:source) {
        if(!id || entity.id!=id){result.error="Invalid server entity identity";return result;}
        EntityId match=0;
        if(auto it=next.find(id);it!=next.end()) {
            if(!local.entities.contains(it->second) || !local_.local_address(it->second)){result.error="Replica binding expired";return result;}
            match=it->second;
        } else {
            for(const auto& [candidate_id,candidate]:local.entities) {
                if(used.contains(candidate_id) || candidate.kind!=entity.kind || candidate.definition!=entity.definition)continue;
                bool equal=entity.kind==EntityKind::Player?same_player(entity,candidate):same_spawn(entity,candidate);
                if(!equal)continue;if(match){if(entity.kind==EntityKind::Pickup && !mem_read32(local_.local_address(match)+0x50) && !mem_read32(local_.local_address(candidate_id)+0x50))continue;result.error="Ambiguous native replica binding";return result;}match=candidate_id;
            }
        }
        if(!match){result.error="Native creation requires lifecycle handling";return result;}
        if(!used.insert(match).second){result.error="Multiple server entities alias a local object";return result;}
        const auto& target=local.entities.at(match);
        if(target.kind!=entity.kind || target.definition!=entity.definition ||
            (entity.kind==EntityKind::Player && !same_player(entity,target))){result.error="Replica class or player slot changed";return result;}
        if(!build_writes(entity,local_.local_address(match),writes,result.error))return result;
        next[id]=match;
    }
    } // Commit always follows the read-only phase.
    // Validate the complete batch before any native data is modified.
    if(!commit_scalar_writes(writes,result.error))return result;
    bindings_=std::move(next);epoch_=epoch;result.applied=true;result.objects=unsigned(source.size());result.writes=unsigned(writes.size());return result;
}
}
