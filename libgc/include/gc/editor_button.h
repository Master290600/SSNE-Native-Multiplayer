#pragma once
#include <cstdint>
namespace gc::editor {
// Extension after the untouched native Switch config and toggle references.
// A regular original Switch/ModelHolder2 pair never carries this contract.
inline constexpr uint32_t button_magic = 0x53534254u; // SSBT
inline constexpr uint32_t button_version = 1;
inline constexpr unsigned button_config_bytes = 40;
inline constexpr unsigned button_magic_offset = 32;
inline constexpr unsigned button_version_offset = 36;
}
