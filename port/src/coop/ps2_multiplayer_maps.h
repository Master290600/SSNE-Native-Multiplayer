#pragma once
#include "gc/custom_levels.h"
#include <array>
#include <string_view>

namespace port::coop {
struct BundledPs2MapIdentity {std::string_view id,sha256;};
// Exact converted disc packages. Names alone never grant bundled-map behavior.
inline constexpr std::array<BundledPs2MapIdentity,15> bundled_ps2_multiplayer_maps{{
    {"ps2_canyon","95a03a60259732ca3e90dc78a649d77c09768180e34ac5ae75e35393c748befd"},
    {"ps2_egypt","77afc25b8a2885278b5f10c3d0672f41ab88603145e560d14dd3895c8c884261"},
    {"ps2_flight","54f31bd4ea247ab9c4f9f611825fc9c3cdb562099a25fb658a95b25b980d64ea"},
    {"ps2_fort","85c85ac1bf6781ed3c816eb13507a9d3b9d6665eab4cefee85515f4393fd8464"},
    {"ps2_maya","e725c2c9f9ddc96677dada4987cb324622245354e6e6f6cd9b73ab384ef22802"},
    {"ps2_medieval","82ce04d90a90c2d50357dc0b1522044d2e4fda00f019401e0b29334dc0b73fa2"},
    {"ps2_military","0931a7dd33290915762e11044198b23e9d226364893fc7ca7e5a14471b26b9c4"},
    {"ps2_neo_tokyo","c13a9be731030bf16b65f9f2d714df282555385dc4d7698929bce565f1928631"},
    {"ps2_offworld","d8742df8497b3eb194ed293b3203c91d9501b045e10b76e7910b1b9ed9c173a7"},
    {"ps2_russia","4fc91720030878319d772019c9566c0ca54cf0e8c6174d4a2c5040d957885d3c"},
    {"ps2_teeth","22fcf610cc2cc7e20d9c29d24869693cacddc4459342c707d8017cca0f534396"},
    {"ps2_venice","d1fa1556dce78aa66f365191ad3a3eddeec78e023606c3b9b5155bd6758f822f"},
    {"ps2_vertigo","391375437ccdf12b610b080e7a5148dc1bef3f102427403b489855a5296ceeca"},
    {"ps2_victorian","482334bf99b5b019e0ece85859dce45416229fbb30dc706c6b33b987ab15dc11"},
    {"ps2_western","444fe421bb26ca64e46ef002657fa1a9b4da9fe4e25d950615f8cdf77e20d4a9"},
}};
inline bool bundled_ps2_map_identity(const gc::custom::Level& level,unsigned verified_skins){
    if(verified_skins!=17 || level.base_level!="RlevelDM_2" || level.modes!=28)return false;
    for(const auto& expected:bundled_ps2_multiplayer_maps)
        if(level.id==expected.id && level.map_sha256==expected.sha256)return true;
    return false;
}
// Original PAL PS2 region strings, keyed by the verified converted map ID.
// TDB EN region/title records279..308; retain original Latin1 glyph bytes.
struct BundledPs2MapCategory {std::string_view id;std::array<const char*,3> region;};
inline constexpr std::array<BundledPs2MapCategory,15> bundled_ps2_map_categories{{
    {"ps2_canyon",{{"Canyon World","Die Welt der Schluchten","S\351rieux canyon"}}},
    {"ps2_egypt",{{"Ancient Egypt","Das alte \304gypten","Egypte antique"}}},
    {"ps2_flight",{{"Low Orbit","Niedrige Erdumlaufbahn","Orbite rase-mottes"}}},
    {"ps2_fort",{{"Penal Colony 303","Strafkolonie 303","Colonie p\351nitentiaire 303"}}},
    {"ps2_maya",{{"Mayan Era","Das Maya-Zeitalter","Ere maya"}}},
    {"ps2_medieval",{{"Medieval","Das Mittelalter","Moyen Age"}}},
    {"ps2_military",{{"Military World","Welt des Milit\344rs","Univers martial"}}},
    {"ps2_neo_tokyo",{{"Dark Future","Dark Future","No Future"}}},
    {"ps2_offworld",{{"Offworld World","Jenseits-Welt","Monde hors-monde"}}},
    {"ps2_russia",{{"Red Europe","Rotes Europa","Europe rouge"}}},
    {"ps2_teeth",{{"Teeth World","Die Welt der Z\344hne","Dentition"}}},
    {"ps2_venice",{{"Venice World","Venedig-Welt","Univers v\351nitien"}}},
    {"ps2_vertigo",{{"Vertigo World","Vertigo-Welt","Vertige"}}},
    {"ps2_victorian",{{"Victorian Era","Viktorianisches Zeitalter","Empire victorien"}}},
    {"ps2_western",{{"Western World","Western-Welt","Far West"}}},
}};
inline const char* bundled_ps2_map_category(std::string_view id,unsigned language){
    for(const auto& map:bundled_ps2_map_categories)
        if(map.id==id)return map.region[language<map.region.size()?language:0];
    return "";
}
}
