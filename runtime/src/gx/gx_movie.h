#pragma once
#include "ssne/movie_presentation.h"
#include "gx_internal.h"

namespace ssne::gx {
// Only a quad sampling the decoder's actual output qualifies. A 640-wide
// material, an EFB copy, a scene number or a recent disc read alone do not.
template<class Point>
void observe_movie(const DrawCfg& cfg, unsigned n, Point point, movie_presentation::Draw& draw) {
    if (n != 4 || !cfg.stages || !cfg.st[0].tex_enabled) return;
    const uint64_t binding = movie_presentation::decoded_texture.load(std::memory_order_acquire);
    if (!binding) return;
    const int map = cfg.st[0].texmap & 7;
    const unsigned base = (map < 4 ? 0x80 : 0xA0) + (map & 3);
    if (((g.bp[base + 0x14] & 0xffffff) << 5) != uint32_t(binding)) return;
    const auto *texture = cfg.tex[map];
    if (!texture || texture->efb_copy >= 0) return;
    const unsigned info = unsigned(binding >> 32);
    const unsigned width = info & 1023, height = (info >> 10) & 1023, pad = info >> 20;
    float left = 1e6f, right = -1e6f, top = 1e6f, bottom = -1e6f;
    for (unsigned i = 0; i < n; ++i) {
        float x, y;
        point(i, x, y);
        left = std::min(left, x); right = std::max(right, x);
        top = std::min(top, y); bottom = std::max(bottom, y);
    }
    if (right - left < 500.f || bottom - top < 140.f) return;
    // BinkCopyToBuffer sometimes writes into the middle of a taller canvas.
    // Only remove those decoder-owned rows, never black pixels inside a movie.
    const float extent = bottom - top;
    if (pad && pad + height <= unsigned(texture->h)) {
        bottom = top + extent * (pad + height) / texture->h;
        top += extent * pad / texture->h;
    }
    const float aspect = float(width) / height;
    draw = {top, bottom, aspect > 1.60f ? aspect : 4.f / 3.f};
}
}
