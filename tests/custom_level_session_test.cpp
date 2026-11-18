#include "custom_levels_runtime.h"
#include "ps2_content_identity.h"
#include "ps2_multiplayer_maps.h"
#include "frontend_text.h"
#include "wire_protocol.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace port::coop;
unsigned checks = 0;
void check(bool value, const char *why) {
  ++checks;
  if (!value)
    throw std::runtime_error(why);
}
gc::custom::Level level(std::string id, char hash = 'a') {
  gc::custom::Level entry;
  entry.id = std::move(id);
  entry.title = "My custom name";
  entry.base_level = "Rlevel1_1";
  entry.map_sha256 = std::string(64, hash);
  entry.alias = gc::custom::map_alias(entry.id, entry.map_sha256);
  return entry;
}
int main() {
  try {
    static_assert(wire::version >= 68);
    auto first = level("First");
    auto second = level("Second");
    configure_custom_levels({{first, second}, {}});
    check(custom_levels_catalog().levels.size() == 2 &&
              find_custom_level(first.alias)->id == first.id,
          "Independent aliases of the same base map are registered");
    Properties setup{{"GameSetup.RulesCoop.Level", first.alias},
                     {"GameSetup.RulesCoop.LevelSet", int64_t(3)},
                     {"GameSetup.RulesCoop.Difficulty", int64_t(2)}};
    std::string error;
    check(bind_custom_level_identity(first.alias, 1, setup, error),
          "Host binds custom identity");
    check(setup.at("Session.Custom.Id") == Property(first.id) &&
              setup.at("Session.Custom.Base") == Property(first.base_level) &&
              setup.at("Session.Custom.SHA256") == Property(first.map_sha256) &&
              setup.at("Session.Custom.Alias") == Property(first.alias),
          "Host carries complete exact package identity");
    check(setup.at("GameSetup.RulesCoop.LevelSet") == Property(int64_t(0)) &&
              setup.at("GameSetup.RulesCoop.Difficulty") ==
                  Property(int64_t(2)),
          "Menu-only category stays outside original integer config range");
    check(verify_custom_level_identity(first.alias, 1, setup, error),
          "Matching client may load");
    check(verify_custom_level_identity(first.alias, 0, setup, error),
          "Singleplayer mode shares the catalog");
    auto cosmetic = first;
    cosmetic.title = "Different local display name";
    cosmetic.preview_path = "another-preview.png";
    configure_custom_levels({{cosmetic, second}, {}});
    check(verify_custom_level_identity(first.alias, 1, setup, error),
          "Cosmetic name/preview changes do not break compatibility");
    auto missing = setup;
    missing.erase("Session.Custom.SHA256");
    check(!verify_custom_level_identity(first.alias, 1, missing, error),
          "Missing checksum is rejected");
    for (const auto *field :
         {"Session.Custom.Id", "Session.Custom.Base", "Session.Custom.SHA256",
          "Session.Custom.Alias"}) {
      auto spoof = setup;
      spoof[field] = std::string("wrong");
      check(!verify_custom_level_identity(first.alias, 1, spoof, error),
            "Mismatched host identity is rejected");
      spoof[field] = int64_t(1);
      check(!verify_custom_level_identity(first.alias, 1, spoof, error),
            "Wrong metadata type is rejected");
    }
    configure_custom_levels({{second}, {}});
    check(!verify_custom_level_identity(first.alias, 1, setup, error) &&
              error.find("not installed") != std::string::npos,
          "Missing client package is explicit");
    auto changed = level("First", 'b');
    configure_custom_levels({{changed, second}, {}});
    check(!verify_custom_level_identity(first.alias, 1, setup, error) &&
              error.find("differs") != std::string::npos,
          "A different version of the same package is explicit");
    check(!bind_custom_level_identity(first.alias, 1, missing, error),
          "Host cannot select an unprepared alias");
    configure_custom_levels({{first, second}, {}});
    check(!verify_custom_level_identity(first.alias, 2, setup, error),
          "A map cannot load in an unadvertised native mode");
    auto survival = setup;
    survival["Session.Survival.Active"] = int64_t(1);
    check(verify_custom_level_identity(first.alias, 1, survival, error),
          "Survival uses native Coop plus explicit Survival metadata");
    auto coop_only = first;
    coop_only.modes = 2;
    configure_custom_levels({{coop_only}, {}});
    check(!verify_custom_level_identity(first.alias, 1, survival, error),
          "A Coop-only package is not treated as Survival-compatible");
    auto versus = first;
    versus.modes = 1u << 2;
    configure_custom_levels({{versus}, {}});
    Properties menu;
    check(bind_custom_level_identity(first.alias, 0, menu, error, false) &&
              verify_custom_level_identity(first.alias, 0, menu, error, false),
          "Returning to a menu retains identity without treating it as a "
          "singleplayer load");
    check(!verify_custom_level_identity(first.alias, 0, menu, error) &&
              verify_custom_level_identity(first.alias, 2, menu, error),
          "The actual next level load still enforces its compatible mode");
    // Snapshot expectations come from the owned PS2 TDB, not invented world labels.
    struct OriginalCategory {const char* id;std::array<const char*,3> text;};
    const std::array<OriginalCategory,15> original_categories{{
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
    for(const auto& map:original_categories)
      for(unsigned language=0;language<3;++language)
        check(std::string(bundled_ps2_map_category(map.id,language))==map.text[language],
              "Every converted PS2 map retains its original localized region");
    check(std::string(bundled_ps2_map_category("ps2_canyon",99))=="Canyon World" &&
              std::string(bundled_ps2_map_category("user_map",1)).empty(),
          "Unknown language falls back to original English; user maps gain no PS2 region");
    check(std::string(frontend_text(FrontText::NoCustomLevels, 0)) ==
                  "No custom maps installed" &&
              std::string(frontend_text(FrontText::NoCustomLevels, 1)) ==
                  "Keine Custom-Karten installiert" &&
              std::string(frontend_text(FrontText::NoCustomLevels, 2)) ==
                  "Aucune carte custom install\351e",
          "Empty Custom hint remains localized without changing the map title");
    check(std::string(frontend_text(FrontText::ChooseCharacter, 0)) ==
                  "Choose character" &&
              std::string(frontend_text(FrontText::ChooseCharacter, 1)) ==
                  "Figur w\344hlen" &&
              std::string(frontend_text(FrontText::ChooseCharacter, 2)) ==
                  "Choisir le personnage",
          "Direct character chooser text preserves native PAL language encoding");
    const auto prior_skin_count = installed_skin_count.load();
    installed_skin_count.store(17);
    for (const auto &identity : bundled_ps2_multiplayer_maps) {
      auto ps2 = level(std::string(identity.id));
      ps2.base_level = "RlevelDM_2";
      ps2.map_sha256 = identity.sha256;
      ps2.alias = gc::custom::map_alias(ps2.id, ps2.map_sha256);
      ps2.modes = 28;
      const auto original_alias = ps2.alias;
      configure_custom_levels({{ps2}, {}});
      check(custom_level_is_bundled_ps2(ps2) &&
                custom_level_supports_mode(ps2, 2) &&
                custom_level_supports_mode(ps2, 3) &&
                custom_level_supports_mode(ps2, 4) &&
                custom_level_supports_mode(ps2, 5),
            "Every exact bundled PS2 map supports original versus plus Survival");
      check(!custom_level_in_user_category(ps2, 2) &&
                !custom_level_in_user_category(ps2, 5),
            "Official PS2 maps appear only in the standard carousel, never Custom");
      check(!custom_level_supports_mode(ps2, 0) &&
                !custom_level_supports_mode(ps2, 1) &&
                !custom_level_supports_mode(ps2, 6),
            "PS2 Survival support does not admit singleplayer/Coop/unknown modes");
      Properties ps2_setup{{"GameSetup.RulesCoop.Level", ps2.alias},
                           {"GameSetup.RulesCoop.LevelSet", int64_t(0)},
                           {"Session.Survival.Active", int64_t(1)}};
      check(bind_custom_level_identity(ps2.alias, 1, ps2_setup, error) &&
                verify_custom_level_identity(ps2.alias, 1, ps2_setup, error),
            "Actual host bind and client verification both admit PS2 Survival");
      check(find_custom_level(ps2.alias)->modes == 28 &&
                find_custom_level(ps2.alias)->alias == original_alias &&
                find_custom_level(ps2.alias)->map_sha256 == identity.sha256,
            "Survival does not rewrite package modes/hash/alias identity");
      installed_skin_count.store(11);
      check(!custom_level_is_bundled_ps2(ps2) &&
                !verify_custom_level_identity(ps2.alias, 1, ps2_setup, error),
            "Unverified PS2 extension does not grant bundled Survival behavior");
      installed_skin_count.store(17);
      for (unsigned changed = 0; changed < 4; ++changed) {
        auto other = ps2;
        if (changed == 0) other.id += "_custom";
        if (changed == 1) other.map_sha256[0] = other.map_sha256[0] == '0' ? '1' : '0';
        if (changed == 2) other.base_level = "Rlevel1_1";
        if (changed == 3) other.modes = 4;
        other.alias = gc::custom::map_alias(other.id, other.map_sha256);
        check(!custom_level_is_bundled_ps2(other) &&
                  !custom_level_supports_mode(other, 5),
              "Names/checksums/base/mode edits never acquire PS2 Survival support");
        check(custom_level_in_user_category(other, 2),
              "A changed user package remains in Custom rather than standard PS2");
      }
    }
    installed_skin_count.store(prior_skin_count);
    check(custom_level_in_user_category(first, 0) &&
              custom_level_in_user_category(first, 1) &&
              custom_level_in_user_category(first, 5) &&
              !custom_level_in_user_category(first, 2),
          "Genuine user maps retain Custom mode filtering and Survival metadata");
    configure_custom_levels({{first, second}, {}});
    check(!verify_custom_level_identity("Rlevel1_1", 1, setup, error),
          "Custom identity cannot be applied silently to an original map");
    check(bind_custom_level_identity("Rlevel1_2", 1, setup, error) &&
              !setup.contains("Session.Custom.Id") &&
              !setup.contains("Session.Custom.SHA256") &&
              setup.contains("GameSetup.RulesCoop.Difficulty"),
          "Returning to an original campaign clears only custom metadata");
    check(verify_custom_level_identity("Rlevel1_2", 1, setup, error),
          "Ordinary peers still follow original campaign levels");
    auto invalid = first;
    invalid.alias = "Custom_forged";
    bool rejected = false;
    try {
      configure_custom_levels({{invalid}, {}});
    } catch (...) {
      rejected = true;
    }
    check(rejected && find_custom_level(first.alias),
          "Invalid replacement catalogs reject atomically");
    rejected = false;
    try {
      configure_custom_levels({{first, first}, {}});
    } catch (...) {
      rejected = true;
    }
    check(rejected && custom_levels_catalog().levels.size() == 2,
          "Duplicate aliases/IDs reject atomically");
    const auto* boot_pointer = find_custom_level(first.alias);
    const auto before_generation = custom_levels_generation();
    auto received = first;
    received.title = "The host title";
    received.map_path = "DownloadedCustomLevels/version/map.ssw";
    received.preview_path = "DownloadedCustomLevels/version/preview.png";
    register_downloaded_custom_level(received);
    const auto* received_pointer = find_custom_level(first.alias);
    check(received_pointer && received_pointer != boot_pointer &&
              received_pointer->title == received.title &&
              boot_pointer->title == first.title &&
              custom_levels_catalog().levels.size() == 2 &&
              custom_levels_generation() > before_generation,
          "Received metadata overrides alias while boot GUI pointers remain stable");
    auto choices = custom_levels_available();
    check(choices.size() == 2 &&
              std::count(choices.begin(), choices.end(), received_pointer) == 1 &&
              std::count(choices.begin(), choices.end(), boot_pointer) == 0,
          "Choices omit a boot entry overridden by received metadata");
    received.title = "Updated host title";
    register_downloaded_custom_level(received);
    check(find_custom_level(first.alias) == received_pointer &&
              received_pointer->title == received.title,
          "Received entry replacement retains stable GUI address");
    auto changed_received = changed;
    changed_received.map_path = "DownloadedCustomLevels/changed/map.ssw";
    register_downloaded_custom_level(changed_received);
    check(custom_levels_available().size() == 3 &&
              find_custom_level(changed.alias) && find_custom_level(first.alias),
          "Remote and local versions of the same ID coexist by exact alias");
    configure_custom_levels({});
    check(verify_custom_level_identity("Rlevel1_1", 1, {}, error),
          "No-package startup keeps ordinary multiplayer available");
    std::cout << "PASS custom_session_checks=" << checks
              << " protocol=" << wire::version << "\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << " checks=" << checks << "\n";
    return 1;
  }
}
