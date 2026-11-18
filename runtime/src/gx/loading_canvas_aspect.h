#pragma once
#include <algorithm>
#include <array>
#include <cstddef>

namespace ssne::gx {
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
    float left=0.f,right=0.f,bottom=0.f,top=0.f;
    for (std::size_t i=0;i<count;++i) {
        auto p=read(i);
        if (!(p[3]>0.f)) return false;
        const float x=p[0]/p[3],y=p[1]/p[3];
        left=std::min(left,x);right=std::max(right,x);
        bottom=std::min(bottom,y);top=std::max(top,y);
    }
    const float coverage=std::min(-left,right);
    const bool backdrop=coverage>=.99f && bottom<=-.99f && top>=.99f;
    const float zoom=backdrop?std::max(1.f,wide/coverage):1.f;
    for (std::size_t i=0;i<count;++i) {
        auto p=read(i);
        write(i,p[0]*(zoom/wide),p[1]*zoom);
    }
    // The actual 854-by-480 GUI sheet is followed by perspective widgets
    // (the rotating NETRICSA initial). Earlier 640-wide world post-process
    // quads must not classify subsequent world meshes as menu widgets.
    return coverage > 1.33f && coverage < 1.34f && bottom > -1.01f && bottom <= -.99f &&
           top >= .99f && top < 1.01f;
}
} // namespace ssne::gx

