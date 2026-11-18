#pragma once
#include <array>
#include <cstddef>
#include <string_view>

// Exact original GameCube map filenames, from the validated 49-map inventory
// in baseline226-original.json. Additional owned maps have arbitrary titles;
// neither a PS2_ prefix nor another naming convention identifies originals.
inline constexpr std::array<std::string_view, 49> original_gc_map_filenames{
    "Alevel10_2.ssw", "Alevel10_3.ssw", "Alevel10_4.ssw",
    "Alevel11_1.ssw", "Alevel11_2.ssw", "Alevel11_3.ssw", "Alevel11_4.ssw",
    "Alevel12_1.ssw", "Alevel12_2.ssw", "Alevel12_3.ssw", "Alevel12_4.ssw",
    "Alevel9_1.ssw", "Alevel9_4.ssw", "AlevelDM_1.ssw", "AlevelDM_2.ssw",
    "Clevel5_1.ssw", "Clevel5_3.ssw", "Clevel5_4.ssw", "Clevel6_1.ssw",
    "Clevel6_2.ssw", "Clevel7_1.ssw", "Clevel7_2.ssw", "Clevel8_1.ssw",
    "Clevel8_2.ssw", "Clevel8_3.ssw", "Clevel8_4.ssw", "ClevelDM_1.ssw",
    "ClevelDM_2.ssw", "Gallery.ssw", "Rlevel0_1.ssw", "Rlevel1_1.ssw",
    "Rlevel1_2.ssw", "Rlevel1_3.ssw", "Rlevel1_4a.ssw", "Rlevel1_4b.ssw",
    "Rlevel2_1.ssw", "Rlevel2_2.ssw", "Rlevel2_3.ssw", "Rlevel2_4b.ssw",
    "Rlevel3_1.ssw", "Rlevel3_2.ssw", "Rlevel3_3a.ssw", "Rlevel3_3b.ssw",
    "Rlevel3_4.ssw", "Rlevel4_1.ssw", "Rlevel4_3.ssw", "Rlevel4_4b.ssw",
    "RlevelDM_2.ssw", "RlevelDM_3.ssw",
};

constexpr bool original_gc_map(std::string_view filename) {
  for (const auto original : original_gc_map_filenames)
    if (filename == original)
      return true;
  return false;
}

static_assert([] {
  for (std::size_t i = 0; i < original_gc_map_filenames.size(); ++i) {
    const auto name = original_gc_map_filenames[i];
    if (!name.ends_with(".ssw") || name.find_first_of("/\\") != name.npos)
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (name == original_gc_map_filenames[j])
        return false;
  }
  return true;
}(), "Original map inventory must contain 49 unique filenames");
static_assert(original_gc_map("Rlevel1_1.ssw") && original_gc_map("Gallery.ssw"));
static_assert(!original_gc_map("Canyon.ssw") && !original_gc_map("PS2_canyon.ssw") &&
              !original_gc_map("Rlevel1_1_edit.ssw") && !original_gc_map("Rlevel1_1"));
