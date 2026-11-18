#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace ssne::movie_presentation {

// One packed decoder binding. A Bink texture is only recognized while that
// decoder owns the buffer. Other game materials can reuse its address later.
inline std::atomic<uint64_t> decoded_texture{0};
// Changes on each completed decoder write. Physical addresses alone repeat, and a
// sparse texture fingerprint can miss small motions and changes inside a fade.
inline std::atomic<uint64_t> completed_copy{0};  // generation | GX buffer address
inline constexpr uint64_t streaming_texture_bit = uint64_t{1} << 63;
inline void bind(uint32_t guest_address, unsigned width, unsigned height, unsigned y) {
    if (!width || !height || width > 1023 || height > 1023 || y > 1023) return;
    const uint64_t info = width | (height << 10) | (y << 20);
    const uint32_t generation = uint32_t(completed_copy.load(std::memory_order_relaxed) >> 32) + 1;
    completed_copy.store((uint64_t(generation) << 32) | (guest_address & 0x3fffffffU), std::memory_order_release);
    decoded_texture.store((info << 32) | (guest_address & 0x3fffffffu), std::memory_order_release);
}

// Draw bounds in the original embedded framebuffer. XFB copies need not start
// at y=0 or have the full EFB height.
struct Draw {
    float top = 0, bottom = 0, aspect = 0;
    uint32_t pack(int first_row, int rows) const {
        if (aspect <= 0 || rows <= 0 || bottom <= top) return 0;
        auto edge = [&](float y) { return uint32_t(std::lround(1023.f * std::clamp((y - first_row) / rows, 0.f, 1.f))); };
        auto ratio = uint32_t(std::lround(256.f * std::clamp(aspect, 0.f, 15.f)));
        return (ratio << 20) | (edge(bottom) << 10) | edge(top);
    }
};
inline uint32_t displayed_gpu = 0, displayed_software = 0;  // render thread
}
