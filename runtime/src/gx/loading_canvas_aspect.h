#pragma once
#include <algorithm>
#include <array>
#include <cstddef>

namespace ssne { bool netricsa_canvas(); }

namespace ssne::gx {
struct CanvasBounds {
    float left=0.f, right=0.f, bottom=0.f, top=0.f;
    float coverage() const { return std::min(-left,right); }
    bool artwork() const {
        return coverage() > 1.33f && coverage() < 1.34f && bottom > -1.01f && bottom <= -.99f &&
               top >= .99f && top < 1.01f;
    }
};
template<class Read>
bool measure_canvas(std::size_t count, Read read, CanvasBounds& bounds) {
    for (std::size_t i=0; i<count; ++i) {
        const auto p=read(i);
        if (!(p[3]>0.f)) return false;
        const float x=p[0]/p[3], y=p[1]/p[3];
        bounds.left=std::min(bounds.left,x); bounds.right=std::max(bounds.right,x);
        bounds.bottom=std::min(bounds.bottom,y); bounds.top=std::max(bounds.top,y);
    }
    return true;
}
// During a NETRICSA fade the world and its HUD render first. Only the
// authored 854-by-480 menu backdrop and the widgets drawn after it belong
// to the information canvas.
template<class Read>
float draw_canvas_factor(float wide, bool ortho, bool saw_art, std::size_t count, Read read) {
    if (wide <= 1.f || saw_art) return wide;
    if (!ortho) return 1.f;
    if (!netricsa_canvas()) return wide;
    CanvasBounds bounds;
    return measure_canvas(count,read,bounds) && bounds.artwork() ? wide : 1.f;
}
// LoadingN fills a centered 854-by-480 canvas. The original 640-by-480
// projection clips the sides before display. Open that projection for every
// loading UI element; scale just the backdrop uniformly when even its full
// authored width cannot cover the selected display.
template<class Read, class Write>
bool fit_loading_canvas(float wide, std::size_t count, Read read, Write write, bool ortho=true) {
    if (wide <= 1.f || !count) return false;
    if (!ortho) {
        for (std::size_t i=0; i<count; ++i) {
            const auto p=read(i);
            write(i,p[0]/wide,p[1]);
        }
        return false;
    }
    CanvasBounds bounds;
    if (!measure_canvas(count,read,bounds)) return false;
    const float coverage=bounds.coverage();
    const bool backdrop=coverage>=.99f && bounds.bottom<=-.99f && bounds.top>=.99f;
    const float zoom=backdrop?std::max(1.f,wide/coverage):1.f;
    for (std::size_t i=0;i<count;++i) {
        auto p=read(i);
        write(i,p[0]*(zoom/wide),p[1]*zoom);
    }
    // The actual 854-by-480 GUI sheet is followed by perspective widgets
    // (the rotating NETRICSA initial). Earlier 640-wide world post-process
    // quads must not classify subsequent world meshes as menu widgets.
    return bounds.artwork();
}
} // namespace ssne::gx

