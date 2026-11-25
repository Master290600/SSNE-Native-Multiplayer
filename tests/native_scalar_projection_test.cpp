#include "native_scalar_projection.h"
#include <bit>
#include <cstdlib>
#include <iostream>
#include <set>
#include <type_traits>
#include <limits>
using namespace port::coop;
namespace {
void require(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
bool bits(const Property& a,const Property& b){
    if(a.index()!=b.index())return false;
    return std::visit([&](const auto& x){using T=std::decay_t<decltype(x)>;const auto& y=std::get<T>(b);
        if constexpr(std::is_same_v<T,float>)return std::bit_cast<uint32_t>(x)==std::bit_cast<uint32_t>(y);
        else if constexpr(std::is_same_v<T,std::vector<float>> || std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<float,4>>){
            if(x.size()!=y.size())return false;for(size_t i=0;i<x.size();++i)if(std::bit_cast<uint32_t>(x[i])!=std::bit_cast<uint32_t>(y[i]))return false;return true;
        }else return x==y;
    },a);
}
void check(const EntityState& source,const std::set<std::string>& expected,bool materialized){
    const auto before=source;const auto projection=replica_detail::project_lifecycle_scalars(source);
    require(bool(projection)==materialized,"Unexpected scalar projection materialization");const auto& result=projection?*projection:source;
    require(result.id==source.id && result.kind==source.kind && result.definition==source.definition && result.resource==source.resource && result.owner==source.owner,"Projection changed lifetime/asset identity");
    require(result.properties.size()==expected.size(),"Projection retained or discarded unexpected fields");
    for(const auto& key:expected){require(result.properties.contains(key),"Projection discarded a required or unknown field");require(bits(result.properties.at(key),source.properties.at(key)),"Projection changed scalar or floating-point bits");}
    require(source.properties.size()==before.properties.size(),"Projection mutated input fields");for(const auto& [key,value]:before.properties)require(bits(value,source.properties.at(key)),"Projection mutated immutable source values");
    if(!projection && source.properties.contains("enemy_bone_matrices"))require(std::get<std::vector<float>>(result.properties.at("enemy_bone_matrices")).data()==std::get<std::vector<float>>(source.properties.at("enemy_bone_matrices")).data(),"Unfiltered NPC skeleton was copied");
}
void movement_metadata(){
    ssne::NativeMovementState native;
    native.position={1,2,3};native.velocity={-4,0,0};native.grounded=true;native.contact_kind=1;
    native.curve[5]=.1f;
    const auto checkpoint=encode_movement_checkpoint(native);
    EntityState created;created.id=100;created.kind=EntityKind::Player;created.definition=2;
    created.resource="new/player";created.owner={3,7};
    created.properties={
        {"position",native.position}, {"movement_checkpoint",checkpoint},
        {"movement_spawn",int64_t(1)}, {"movement_stream",int64_t(19)}, {"movement_ack",int64_t(0)},
    };
    require(replica_detail::valid_player_movement_fields(created),"New player movement metadata failed complete preflight");
    check(created,{"position"},true);
    for(const auto& [key,value]:created.properties)if(key!="position"){
        require(player_movement_metadata(created.kind,key),"Exact movement metadata was not classified");
        require(replica_detail::valid_player_movement_metadata(created.kind,key,value),"Valid movement metadata value rejected");
        auto only=created;only.properties.clear();only.properties[key]=value;
        check(only,{},true);
        for(unsigned kind=0;kind<=unsigned(EntityKind::Mission);++kind)if(EntityKind(kind)!=EntityKind::Player){
            only.kind=EntityKind(kind);
            require(!player_movement_metadata(only.kind,key),"Movement metadata concealed on another class");
            require(!replica_detail::valid_player_movement_fields(only),"Another class accepted exact player movement metadata");
            check(only,{key},false);
        }
    }
    for(const auto* key:{"movement_ack_extra","movement_checkpoint_future","movement_spawned","movement_stream_id","movement_unknown"}){
        auto unknown=created;unknown.properties[key]=int64_t(5);
        require(!player_movement_metadata(created.kind,key),"Lookalike movement field hidden by a prefix filter");
        check(unknown,{"position",key},true); // Unknown field must reach the strict native schema.
    }
    for(const auto* key:{"movement_spawn","movement_stream","movement_ack"}){
        for(const Property bad:{Property{int64_t(-1)},Property{1.f},Property{std::string("1")},Property{std::vector<float>{1.f}}}){
            auto invalid=created;invalid.properties[key]=bad;
            require(!replica_detail::valid_player_movement_fields(invalid),"Invalid movement integer survived complete preflight");
        }
        auto maximum=created;maximum.properties[key]=std::numeric_limits<int64_t>::max();
        require(replica_detail::valid_player_movement_fields(maximum),"Valid signed movement integer limit rejected");
        auto zero=created;zero.properties[key]=int64_t(0);
        require(replica_detail::valid_player_movement_fields(zero)==(std::string_view(key)=="movement_ack"),"Movement identity accepted zero or ACK rejected zero");
    }
    std::vector<Property> invalid_checkpoints{int64_t(0),std::vector<float>{},std::vector<float>(22),std::vector<float>(24)};
    for(const auto& [index,value]:std::vector<std::pair<size_t,float>>{
        {0,std::numeric_limits<float>::quiet_NaN()}, {3,160.f}, {6,2.f},
        {7,.5f}, {8,-1.f}, {15,.21f}, {22,2.f},
    }){
        auto bad=checkpoint;bad[index]=value;invalid_checkpoints.emplace_back(std::move(bad));
    }
    for(const auto& value:invalid_checkpoints){
        auto invalid=created;invalid.properties["movement_checkpoint"]=value;
        require(!replica_detail::valid_player_movement_fields(invalid),"Invalid checkpoint was ignored before complete preflight");
    }
}
}
int main(){
    movement_metadata();
    EntityState npc;npc.id=41;npc.kind=EntityKind::Enemy;npc.definition=12;npc.resource="original/npc";npc.owner={2,9};
    std::vector<float> pose(3072,1.f);pose[0]=std::bit_cast<float>(0x80000000u);pose[1]=std::bit_cast<float>(0x7FC12345u);pose[2]=std::bit_cast<float>(1u);
    npc.properties={{"position",std::array<float,3>{1,2,3}},{"spawn_position",std::array<float,3>{1,2,3}},{"enemy_bone_count",int64_t(256)},{"enemy_bone_layout",int64_t(-7)},{"enemy_bone_matrices",pose},{"health",-0.f},{"render_visible",int64_t(1)},{"render_row0",std::array<float,4>{1,0,0,-0.f}},{"unrecognized_property",std::string("must reach validation")}};
    const std::set<std::string> npc_keys{"position","spawn_position","enemy_bone_count","enemy_bone_layout","enemy_bone_matrices","health","render_visible","render_row0","unrecognized_property"};check(npc,npc_keys,false);
    EntityState projectile;projectile.id=42;projectile.kind=EntityKind::Projectile;projectile.definition=17;projectile.properties={{"position",std::array<float,3>{0,0,0}},{"velocity",std::array<float,3>{1,0,0}},{"projectile_spin",-0.f},{"projectile_source",EntityReference{41}},{"render_row0",std::array<float,4>{1,0,0,0}}};check(projectile,{"position","velocity","projectile_spin","projectile_source","render_row0"},false);
    EntityState player=npc;player.id=43;player.kind=EntityKind::Player;player.definition=2;
    player.properties={{"position",std::array<float,3>{0,0,0}},{"health",100.f},{"camera_row0",std::array<float,4>{1,0,0,-0.f}},{"body_weapon_resource",int64_t(3)},{"bodyish_unknown",pose},{"camera_unknown",int64_t(4)},{"movement_mode",int64_t(3)},{"camera_frozen",int64_t(1)},{"selected_weapon",int64_t(4)},{"weapon_modes",std::vector<int64_t>{1,2}},{"powerups",std::vector<float>{-0.f}},{"render_visible",int64_t(1)},{"render_row0",std::array<float,4>{1,0,0,0}},{"body_render1_row0",std::array<float,4>{1,0,0,0}},{"body0_bone_matrices",pose},{"body1_bone_matrices",pose},{"body2_bone_matrices",pose},{"body3_bone_matrices",pose},{"body_flash0",std::vector<float>{1}},{"weapon_model0_bone_matrices",pose},{"weapon_bomb0",pose}};
    check(player,{"position","health","camera_row0","body_weapon_resource","bodyish_unknown","camera_unknown"},true);
    // This presentation-only value must survive complete player validation,
    // then disappear from a scalar fallback caused by an unrelated factory.
    // The same spelling on any other class, or an unknown suffix, stays visible
    // to the strict native schema instead of being hidden by a broad prefix.
    player.properties["camera_head_compression"]=.1f;
    require(replica_detail::player_camera_lifecycle_field(player.kind,"camera_head_compression"),"Player camera compression not owned by presentation lifecycle");
    check(player,{"position","health","camera_row0","body_weapon_resource","bodyish_unknown","camera_unknown"},true);
    for(unsigned kind=0;kind<=unsigned(EntityKind::Mission);++kind)if(EntityKind(kind)!=EntityKind::Player){
        auto wrong=npc;wrong.kind=EntityKind(kind);wrong.properties.clear();wrong.properties["camera_head_compression"]=.1f;
        require(!replica_detail::player_camera_lifecycle_field(wrong.kind,"camera_head_compression"),"Player camera metadata concealed on wrong class");check(wrong,{"camera_head_compression"},false);
    }
    for(const auto key:{"camera_head_compression_extra","camera_head_unknown"}){
        auto unknown=player;unknown.properties[key]=.1f;
        require(!replica_detail::player_camera_lifecycle_field(player.kind,key),"Unknown camera field concealed before validation");
        check(unknown,{"position","health","camera_row0","body_weapon_resource","bodyish_unknown","camera_unknown",key},true);
    }
    for(float value:{0.f,.1f,.2f})require(replica_detail::valid_player_camera_head_compression(Property{value}),"Valid original camera compression rejected");
    for(float value:{-.001f,.201f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
        require(!replica_detail::valid_player_camera_head_compression(Property{value}),"Invalid camera compression accepted by preflight validator");
    for(const Property value:{Property{int64_t(0)},Property{std::string(".1")},Property{std::vector<float>{.1f}}})
        require(!replica_detail::valid_player_camera_head_compression(value),"Wrong camera compression type accepted by preflight validator");
    EntityState door;door.id=44;door.kind=EntityKind::Door;door.properties={{"position",std::array<float,3>{0,0,0}},{"brush_index",int64_t(1)},{"brush_resource",int64_t(4)},{"brush_segment",int64_t(2)},{"brush_progress",.5f},{"brush_active",int64_t(1)}};check(door,{"position","brush_index","brush_resource","brush_active"},true);
    // An actor factory/removal forces the post-lifecycle scalar fallback for
    // every existing door. Its four audio fields belong to the later audio
    // commit and must not reach the scalar-only MovingBrush schema again.
    const Properties audio{{"brush_audio_serial",int64_t(1)},{"brush_audio_events",std::vector<int64_t>{1,0}},{"brush_audio_loop",int64_t(1)},{"brush_audio_position",std::array<float,3>{1,2,3}}};
    for(const auto& [key,value]:audio){
        require(replica_detail::brush_audio_lifecycle_field(EntityKind::Door,key),"Known door audio field not classified for native lifecycle");
        EntityState only_audio;only_audio.id=46;only_audio.kind=EntityKind::Door;only_audio.properties.emplace(key,value);check(only_audio,{},true);
        for(unsigned kind=0;kind<=unsigned(EntityKind::Mission);++kind)if(EntityKind(kind)!=EntityKind::Door){
            only_audio.kind=EntityKind(kind);require(!replica_detail::brush_audio_lifecycle_field(only_audio.kind,key),"Door audio lifecycle field concealed a wrong native class");check(only_audio,{key},false);
        }
        door.properties.emplace(key,value);
    }
    check(door,{"position","brush_index","brush_resource","brush_active"},true);
    for(const auto key:{"brush_audio_unknown","brush_audio_events_extra","brush_audio"}){
        require(!replica_detail::brush_audio_lifecycle_field(EntityKind::Door,key),"Unknown brush audio field concealed before strict schema validation");
        auto unknown=door;unknown.properties[key]=int64_t(9);check(unknown,{"position","brush_index","brush_resource","brush_active",key},true);
    }
    EntityState marker;marker.id=45;marker.kind=EntityKind::Physics;marker.properties={{"position",std::array<float,3>{0,0,0}},{"marker_index",int64_t(8)},{"marker_active",int64_t(0)},{"marker_animation",pose}};check(marker,{"position","marker_index","marker_animation"},true);
    // The old projection's player-specific groups must never remove another
    // class's similarly named fields before the schema can reject them.
    npc.properties["body0_bone_unexpected"]=pose;auto with_unknown=npc_keys;with_unknown.insert("body0_bone_unexpected");check(npc,with_unknown,false);
    for(unsigned repeat=0;repeat<100;++repeat){check(npc,with_unknown,false);check(player,{"position","health","camera_row0","body_weapon_resource","bodyish_unknown","camera_unknown"},true);}
    std::cout<<"Lifecycle scalar projection: borrowed NPC/projectile skeletons, exact identity/float bits, player/brush/marker exclusions and unknown-field validation passed.\n";
}
