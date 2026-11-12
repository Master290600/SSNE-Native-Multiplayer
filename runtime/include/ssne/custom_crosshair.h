#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace ssne {
struct CustomCrosshairImage {
    unsigned width=0,height=0,left=0,top=0,right=0,bottom=0;
    std::vector<uint8_t> rgba; // Straight RGBA8, full top-left-origin canvas; right/bottom exclusive.
};
struct CustomCrosshairConfig {
    bool enabled=false;
    int size=32; // Relative original-quad scale: 32 keeps authored weapon/view scaling; 14 means14/32.
    int opacity=100;
    std::shared_ptr<const CustomCrosshairImage> image;
    bool native_color=true; // Inherit the original reticle health/target RGB; false keeps PNG colors.
};
// UI thread: publish an immutable image/config; invalid input safely disables replacement.
void set_custom_crosshair(const CustomCrosshairConfig&);
bool custom_crosshair_active();
// Guest thread only, at GX command boundaries. Ordered with the original draw's FIFO bytes.
// false is always safe after disable and clears a previously queued scope.
void gx_custom_crosshair_scope(bool on);
struct CustomCrosshairStats {
    bool ready=false,stream_active=false,last_replaced=false;
    bool last_native_color=true;
    uint64_t queued=0,parsed=0,tags=0,replacements=0,fallbacks=0,rejected=0,dropped=0,resets=0;
    uint64_t texture_id=0;
    unsigned width=0,height=0,left=0,top=0,right=0,bottom=0;
    uint32_t last_primitive=0,last_stages=0,last_cenv=0,last_aenv=0;
    int last_texmap=-1,last_texcoord=-1;
    float original_left=0,original_top=0,original_right=0,original_bottom=0;
    float drawn_left=0,drawn_top=0,drawn_right=0,drawn_bottom=0;
};
CustomCrosshairStats custom_crosshair_stats();
}
