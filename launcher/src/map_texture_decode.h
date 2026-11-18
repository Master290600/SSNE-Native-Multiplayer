#pragma once
#include <cstdint>
#include <vector>

// PC GX decoder shared by guest-memory and native host-span consumers.
// Algorithm extracted unchanged from the existing renderer; not a new original entry.
namespace ssne::graphics::detail {
inline uint32_t rgba(uint32_t r, uint32_t g_, uint32_t b, uint32_t a) { return r | (g_ << 8) | (b << 16) | (a << 24); }
inline uint32_t exp5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t exp6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t exp4(uint32_t v) { return v * 17; }
inline uint32_t exp3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

inline uint32_t from565(uint16_t v) { return rgba(exp5(v >> 11), exp6((v >> 5) & 63), exp5(v & 31), 255); }
inline uint32_t from5a3(uint16_t v) {
    if (v & 0x8000) return rgba(exp5((v >> 10) & 31), exp5((v >> 5) & 31), exp5(v & 31), 255);
    return rgba(exp4((v >> 8) & 15), exp4((v >> 4) & 15), exp4(v & 15), exp3((v >> 12) & 7));
}
inline uint32_t fromIA8(uint16_t v) { uint32_t i = v & 0xFF; return rgba(i, i, i, v >> 8); }

// Block dimensions per format (width, height in texels) and bytes per block.
inline void block_dims(uint32_t fmt, int& bw, int& bh) {
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;       // I4, C4, CMPR
    case 1: case 2: case 9: bw = 8; bh = 4; break;        // I8, IA4, C8
    default: bw = 4; bh = 4; break;                       // IA8, RGB565, RGB5A3, RGBA8, C14X2
    }
}

template<class Source, class Palette>
void decode(const Source& src, uint32_t fmt, int w, int h, uint32_t tlut, std::vector<uint32_t>& out, const Palette& tlut_color) {
    out.assign(size_t(w) * h, 0);
    int bw, bh;
    block_dims(fmt, bw, bh);
    int bx_n = (w + bw - 1) / bw, by_n = (h + bh - 1) / bh;
    uint32_t off = 0;
    auto put = [&](int x, int y, uint32_t c) { if (x < w && y < h) out[size_t(y) * w + x] = c; };
    for (int by = 0; by < by_n; by++)
        for (int bx = 0; bx < bx_n; bx++) {
            int x0 = bx * bw, y0 = by * bh;
            switch (fmt) {
            case 0:  // I4
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x += 2) {
                        uint8_t v = src.b(off++);
                        uint32_t i0 = exp4(v >> 4), i1 = exp4(v & 15);
                        put(x0 + x, y0 + y, rgba(i0, i0, i0, i0));
                        put(x0 + x + 1, y0 + y, rgba(i1, i1, i1, i1));
                    }
                break;
            case 1:  // I8
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) { uint32_t i = src.b(off++); put(x0 + x, y0 + y, rgba(i, i, i, i)); }
                break;
            case 2:  // IA4
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) {
                        uint8_t v = src.b(off++);
                        uint32_t i = exp4(v & 15);
                        put(x0 + x, y0 + y, rgba(i, i, i, exp4(v >> 4)));
                    }
                break;
            case 3:  // IA8
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, fromIA8(src.h(off))); off += 2; }
                break;
            case 4:  // RGB565
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, from565(src.h(off))); off += 2; }
                break;
            case 5:  // RGB5A3
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, from5a3(src.h(off))); off += 2; }
                break;
            case 6:  // RGBA8: 32 bytes of AR pairs, then 32 bytes of GB pairs
                for (int k = 0; k < 16; k++) {
                    uint32_t a = src.b(off + 2 * k), r = src.b(off + 2 * k + 1);
                    uint32_t gg = src.b(off + 32 + 2 * k), b = src.b(off + 32 + 2 * k + 1);
                    put(x0 + (k & 3), y0 + (k >> 2), rgba(r, gg, b, a));
                }
                off += 64;
                break;
            case 8:  // C4
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x += 2) {
                        uint8_t v = src.b(off++);
                        put(x0 + x, y0 + y, tlut_color(tlut, v >> 4));
                        put(x0 + x + 1, y0 + y, tlut_color(tlut, v & 15));
                    }
                break;
            case 9:  // C8
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) put(x0 + x, y0 + y, tlut_color(tlut, src.b(off++)));
                break;
            case 10:  // C14X2
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) { put(x0 + x, y0 + y, tlut_color(tlut, src.h(off) & 0x3FFF)); off += 2; }
                break;
            case 14:  // CMPR: 2x2 DXT1 sub-blocks
                for (int sb = 0; sb < 4; sb++) {
                    int sx = x0 + (sb & 1) * 4, sy = y0 + (sb >> 1) * 4;
                    uint16_t c0 = src.h(off), c1 = src.h(off + 2);
                    uint32_t pal[4];
                    pal[0] = from565(c0);
                    pal[1] = from565(c1);
                    auto ch = [](uint32_t c, int s) { return (c >> s) & 0xFF; };
                    if (c0 > c1) {
                        pal[2] = rgba((2 * ch(pal[0], 0) + ch(pal[1], 0)) / 3, (2 * ch(pal[0], 8) + ch(pal[1], 8)) / 3,
                                      (2 * ch(pal[0], 16) + ch(pal[1], 16)) / 3, 255);
                        pal[3] = rgba((ch(pal[0], 0) + 2 * ch(pal[1], 0)) / 3, (ch(pal[0], 8) + 2 * ch(pal[1], 8)) / 3,
                                      (ch(pal[0], 16) + 2 * ch(pal[1], 16)) / 3, 255);
                    } else {
                        pal[2] = rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                                      (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 255);
                        pal[3] = 0;  // transparent black
                    }
                    for (int y = 0; y < 4; y++) {
                        uint8_t bits = src.b(off + 4 + y);
                        for (int x = 0; x < 4; x++) put(sx + x, sy + y, pal[(bits >> (6 - 2 * x)) & 3]);
                    }
                    off += 8;
                }
                break;
            default:
                off += 32;
                break;
            }
        }
}

inline uint32_t data_size(uint32_t fmt, int w, int h) {
    int bw, bh;
    block_dims(fmt, bw, bh);
    uint32_t blocks = uint32_t((w + bw - 1) / bw) * uint32_t((h + bh - 1) / bh);
    return blocks * (fmt == 6 ? 64 : 32);
}

} // namespace ssne::graphics::detail
