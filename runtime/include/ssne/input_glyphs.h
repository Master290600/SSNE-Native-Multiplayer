#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ssne {
enum GlyphButton { GLYPH_A, GLYPH_B, GLYPH_X, GLYPH_Y, GLYPH_Z, GLYPH_START, GLYPH_L, GLYPH_R, GLYPH_COUNT };
enum class GlyphShape { Key, MouseLeft, MouseRight, MouseMiddle, WheelUp, WheelDown, Pad, Cross, Circle, Square, Triangle };
struct InputGlyph {
    std::string label;
    GlyphShape shape = GlyphShape::Key;
    bool operator==(const InputGlyph&) const = default;
};
struct InputGlyphConfig {
    bool enabled = false;
    std::array<InputGlyph, GLYPH_COUNT> buttons;
    std::array<InputGlyph, GLYPH_COUNT> menu_buttons;
    bool separate_menu = false;
    uint64_t revision = 0;
};
struct InputGlyphPatch { int button, x, y, w, h; };
// Host publishes immutable configurations; texture work stays on the guest thread.
void set_input_glyphs(InputGlyphConfig config);
std::shared_ptr<const InputGlyphConfig> input_glyph_config();
std::vector<InputGlyphPatch> find_input_glyphs(int w, int h, const std::vector<uint32_t>& pixels);
void render_input_glyphs(int w, int h, std::vector<uint32_t>& pixels,
                         const std::vector<InputGlyphPatch>& patches, const InputGlyphConfig& config);
}
