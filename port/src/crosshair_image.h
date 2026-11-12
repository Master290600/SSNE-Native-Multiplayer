#pragma once
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace port {
inline constexpr unsigned kCrosshairMaxDimension=1024;
inline constexpr size_t kCrosshairMaxFileBytes=4u*1024*1024;
inline constexpr size_t kCrosshairMaxChoices=256;
struct CrosshairVisibleBounds {
    unsigned left=0,top=0,right=0,bottom=0; // Exclusive right/bottom, nonzero output alpha.
    unsigned width() const {return right-left;}
    unsigned height() const {return bottom-top;}
};
struct CrosshairImage {
    unsigned width=0,height=0;
    std::vector<uint8_t> rgba; // Straight alpha, top-left row first; original canvas retained.
    CrosshairVisibleBounds visible;
};
// Supports PNG gray/RGB/palette/gray-alpha/RGBA, PNG bit depths and Adam7.
// Every failure preserves `image`; fully transparent assets are unusable.
bool decode_crosshair_png(std::span<const uint8_t> bytes,CrosshairImage& image,std::string& error);
bool load_crosshair_png(const std::filesystem::path& path,CrosshairImage& image,std::string& error);
// Safe UTF-8 leaf names only. A missing folder is an empty catalog, not an error.
bool safe_crosshair_filename(std::string_view filename);
std::vector<std::string> list_crosshair_pngs(const std::filesystem::path& directory,std::string& error);
}
