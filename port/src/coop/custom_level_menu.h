#pragma once
#include "ssne/ppc_context.h"
#include <memory>
#include <string>
namespace gc::custom {struct Level;}
namespace ssne {struct CustomCrosshairImage;}

namespace port::coop {
void install_custom_level_menu();
// Guest thread only. Reuses the native menu-image draw observer and sprite
// destruction cleanup; a null image restores the sprite's authored texture.
void bind_native_menu_image(uint32_t sprite,std::shared_ptr<const ssne::CustomCrosshairImage>);
// Guest thread, after a verified downloaded package enters the registry.
void custom_level_menu_catalog_changed(PPCContext&);
void custom_level_menu_frame(PPCContext&);
bool custom_level_menu_available(PPCContext&);
bool custom_level_menu_has_user_levels(PPCContext&);
bool custom_level_menu_active();
uint32_t custom_level_menu_gui();
void custom_level_menu_toggle(PPCContext&,uint32_t gui);
void custom_level_menu_forget_gui(PPCContext&,uint32_t gui);
std::string custom_level_menu_diagnostics();
std::string custom_level_menu_category(const gc::custom::Level&,unsigned language);
}
