#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>

namespace port {
inline void draw_movie(ImDrawList& dl, ImTextureID tex, float x, float y, float w, float h, float rendered_factor, uint32_t packed) {
    const float aspect = float(packed >> 20) / 256.f;
    auto image = [&](float dx, float dy, float dw, float dh, float u0, float v0, float u1, float v1) {
        if (dw > 0 && dh > 0)
            dl.AddImage(tex, {x+dx, y+dy}, {x+dx+dw, y+dy+dh}, {u0,v0}, {u1,v1});
    };
    dl.PushClipRect({x,y}, {x+w,y+h}, true);
    if (aspect < 1.6f) {
        // Studio logos and the demo are authored for a 4:3 television.
        // Keep the middle three quarters at the native scale. Gently expand
        // the outer artwork rather than repeat the logo in a blurred sidebar.
        const float normal = std::min(w, h * (4.f/3.f));
        auto edge = [&](float u) {
            const float side = std::max(0.f, (std::abs(u - .5f) - .375f) * 8.f);
            return w * .5f + (u - .5f) * normal + std::copysign(side * side * (w - normal) * .5f, u-.5f);
        };
        constexpr float cuts[] = {0.f, .03125f, .0625f, .09375f, .125f, .875f, .90625f, .9375f, .96875f, 1.f};
        for (unsigned i = 1; i < sizeof(cuts)/sizeof(*cuts); ++i) {
            const float a = cuts[i-1], b = cuts[i];
            image(edge(a), 0, edge(b)-edge(a), h, a,0,b,1);
        }
    } else {
        const float top = float(packed & 1023) / 1023.f;
        const float bottom = float((packed >> 10) & 1023) / 1023.f;
        // The game draws its localized skip hint separately below the film.
        // Reserve room even on ultrawide displays, then copy that original band
        // at its original height rather than scaling the text with the video.
        const float hint = (1.f-bottom) > .07f ? .075f * h : 0.f;
        const float fh = std::min(w / aspect, h - hint);
        const float fw = fh * aspect;
        const float fy = std::min((h-fh)*.5f, h-fh-hint);
        const float menu_w = std::min(w, h * (4.f/3.f) * rendered_factor), menu_x = (w-menu_w)*.5f;
        dl.AddRectFilled({x,y}, {x+w,y+h}, IM_COL32_BLACK);
        image(menu_x, fy-top*h, menu_w, top*h, 0,0,1,top);
        image((w-fw)*.5f, fy, fw, fh, 0,top,1,bottom);
        image(menu_x, fy+fh, menu_w, (1.f-bottom)*h, 0,bottom,1,1);
    }
    dl.PopClipRect();
}
}
