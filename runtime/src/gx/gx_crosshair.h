#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "gx_internal.h"
namespace ssne::gx {
inline constexpr uint8_t kCrosshairOpcode=0xfe,kCrosshairMagic=0x43;
inline constexpr size_t kCrosshairTokenBytes=10;
void custom_crosshair_marker(uint64_t ticket);
bool custom_crosshair_stream_active();
bool custom_crosshair_prepare(uint8_t primitive,std::vector<Vtx>& vertices);
void custom_crosshair_finish_draw();
void custom_crosshair_apply_cfg(DrawCfg&);
void custom_crosshair_software_flush();
void custom_crosshair_reset_stream();
}
